#include "codegen.h"

#include <algorithm>
#include <fstream>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace {

bool Fits12Bit(int value) { return value >= -2048 && value <= 2047; }

void EmitLoadStack(std::ostream &out, const std::string &reg, int offset) {
  if (Fits12Bit(offset)) {
    out << "  lw " << reg << ", " << offset << "(sp)\n";
    return;
  }
  out << "  li t2, " << offset << "\n";
  out << "  add t2, sp, t2\n";
  out << "  lw " << reg << ", 0(t2)\n";
}

void EmitStoreStack(std::ostream &out, const std::string &reg, int offset) {
  if (Fits12Bit(offset)) {
    out << "  sw " << reg << ", " << offset << "(sp)\n";
    return;
  }
  out << "  li t2, " << offset << "\n";
  out << "  add t2, sp, t2\n";
  out << "  sw " << reg << ", 0(t2)\n";
}

std::string ToAsmName(const std::string &name) { return name; }

}  // namespace

KoopaGenerator::KoopaGenerator(std::ostream &out) : out_(out) {}

void KoopaGenerator::Generate(const Program &program) {
  RegisterLibraryFunctions();
  RegisterFunctions(program);
  scopes_.clear();
  PushScope();

  out_ << "decl @getint(): i32\n";
  out_ << "decl @getch(): i32\n";
  out_ << "decl @getarray(*i32): i32\n";
  out_ << "decl @putint(i32)\n";
  out_ << "decl @putch(i32)\n";
  out_ << "decl @putarray(i32, *i32)\n";
  out_ << "decl @starttime()\n";
  out_ << "decl @stoptime()\n\n";

  for (const GlobalItem &item : program.items) {
    GenerateGlobalItem(item);
  }

  PopScope();
}

void KoopaGenerator::RegisterLibraryFunctions() {
  functions_.clear();
  functions_.emplace("getint", FuncInfo{TypeKind::Int, 0});
  functions_.emplace("getch", FuncInfo{TypeKind::Int, 0});
  functions_.emplace("getarray", FuncInfo{TypeKind::Int, 1});
  functions_.emplace("putint", FuncInfo{TypeKind::Void, 1});
  functions_.emplace("putch", FuncInfo{TypeKind::Void, 1});
  functions_.emplace("putarray", FuncInfo{TypeKind::Void, 2});
  functions_.emplace("starttime", FuncInfo{TypeKind::Void, 0});
  functions_.emplace("stoptime", FuncInfo{TypeKind::Void, 0});
}

void KoopaGenerator::RegisterFunctions(const Program &program) {
  for (const GlobalItem &item : program.items) {
    if (item.kind != GlobalItem::Kind::FuncDef) {
      continue;
    }
    const FunctionDef &function = *item.function;
    if (!functions_.emplace(function.name,
                            FuncInfo{function.return_type,
                                     static_cast<int>(function.params.size())})
             .second) {
      throw std::runtime_error("redefined function: " + function.name);
    }
  }
}

void KoopaGenerator::GenerateGlobalItem(const GlobalItem &item) {
  switch (item.kind) {
    case GlobalItem::Kind::ConstDecl:
      for (const ConstDef &def : item.const_defs) {
        InsertSymbol(def.name,
                     Symbol{Symbol::Kind::Const, EvalConstExpr(*def.init), "", "", 0});
      }
      break;
    case GlobalItem::Kind::VarDecl:
      for (const VarDef &def : item.var_defs) {
        const int value = def.init ? EvalConstExpr(*def.init) : 0;
        out_ << "global @" << def.name << " = alloc i32, ";
        if (def.init) {
          out_ << value << "\n";
        } else {
          out_ << "zeroinit\n";
        }
        InsertSymbol(def.name,
                     Symbol{Symbol::Kind::GlobalVar, 0, "@" + def.name, def.name, 0});
      }
      break;
    case GlobalItem::Kind::FuncDef:
      GenerateFunction(*item.function);
      break;
  }
}

void KoopaGenerator::GenerateFunction(const FunctionDef &function) {
  ResetFunctionState();
  current_return_type_ = function.return_type;
  CollectAllocs(function);

  out_ << "\nfun @" << function.name << "(";
  for (size_t i = 0; i < function.params.size(); ++i) {
    if (i != 0) {
      out_ << ", ";
    }
    out_ << "%param_" << i << ": i32";
  }
  out_ << ")";
  if (function.return_type == TypeKind::Int) {
    out_ << ": i32";
  }
  out_ << " {\n%entry:\n";
  for (const std::string &line : alloc_lines_) {
    out_ << line;
  }

  PushScope();
  for (const Param &param : function.params) {
    const std::string &alloc = param_alloc_names_.at(&param);
    out_ << "  store %param_" << (&param - function.params.data()) << ", " << alloc << "\n";
    InsertSymbol(param.name, Symbol{Symbol::Kind::Var, 0, alloc, "", 0});
  }
  GenerateBlock(*function.block);
  PopScope();

  if (!entry_terminated_) {
    if (function.return_type == TypeKind::Void) {
      out_ << "  ret\n";
    } else {
      out_ << "  ret 0\n";
    }
  }
  out_ << "}\n";
}

void KoopaGenerator::ResetFunctionState() {
  var_alloc_names_.clear();
  param_alloc_names_.clear();
  logic_alloc_names_.clear();
  alloc_lines_.clear();
  next_value_id_ = 0;
  next_alloc_id_ = 0;
  next_block_id_ = 0;
  entry_terminated_ = false;
  loop_entry_labels_.clear();
  loop_end_labels_.clear();
}

void KoopaGenerator::CollectAllocs(const FunctionDef &function) {
  for (const Param &param : function.params) {
    const std::string ir_name = NewAllocName(param.name);
    param_alloc_names_.emplace(&param, ir_name);
    alloc_lines_.push_back("  " + ir_name + " = alloc i32\n");
  }
  CollectAllocs(*function.block);
}

void KoopaGenerator::CollectAllocs(const Block &block) {
  for (const BlockItem &item : block.items) {
    CollectAllocs(item);
  }
}

void KoopaGenerator::CollectAllocs(const BlockItem &item) {
  switch (item.kind) {
    case BlockItem::Kind::VarDecl:
      for (const VarDef &def : item.var_defs) {
        const std::string ir_name = NewAllocName(def.name);
        var_alloc_names_.emplace(&def, ir_name);
        alloc_lines_.push_back("  " + ir_name + " = alloc i32\n");
        if (def.init) {
          CollectLogicTemps(*def.init);
        }
      }
      break;
    case BlockItem::Kind::Block:
      CollectAllocs(*item.block);
      break;
    case BlockItem::Kind::If:
      CollectLogicTemps(*item.expr);
      CollectAllocs(*item.then_stmt);
      if (item.else_stmt) {
        CollectAllocs(*item.else_stmt);
      }
      break;
    case BlockItem::Kind::While:
      CollectLogicTemps(*item.expr);
      CollectAllocs(*item.body_stmt);
      break;
    case BlockItem::Kind::ConstDecl:
    case BlockItem::Kind::Assign:
    case BlockItem::Kind::Return:
    case BlockItem::Kind::ExprStmt:
      if (item.expr) {
        CollectLogicTemps(*item.expr);
      }
      break;
    case BlockItem::Kind::Break:
    case BlockItem::Kind::Continue:
      break;
  }
}

void KoopaGenerator::CollectLogicTemps(const Expr &expr) {
  if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
    for (const auto &arg : call->args) {
      CollectLogicTemps(*arg);
    }
    return;
  }
  if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
    CollectLogicTemps(*unary->operand);
    return;
  }
  const auto *binary = dynamic_cast<const BinaryExpr *>(&expr);
  if (binary == nullptr) {
    return;
  }
  if (binary->op == BinaryOp::And || binary->op == BinaryOp::Or) {
    const std::string ir_name = NewAllocName("logic");
    logic_alloc_names_.emplace(binary, ir_name);
    alloc_lines_.push_back("  " + ir_name + " = alloc i32\n");
  }
  CollectLogicTemps(*binary->lhs);
  CollectLogicTemps(*binary->rhs);
}

void KoopaGenerator::GenerateBlock(const Block &block) {
  PushScope();
  for (const BlockItem &item : block.items) {
    if (entry_terminated_) {
      break;
    }
    GenerateItem(item);
  }
  PopScope();
}

void KoopaGenerator::GenerateItem(const BlockItem &item) {
  switch (item.kind) {
    case BlockItem::Kind::ConstDecl:
      for (const ConstDef &def : item.const_defs) {
        InsertSymbol(def.name,
                     Symbol{Symbol::Kind::Const, EvalConstExpr(*def.init), "", "", 0});
      }
      break;
    case BlockItem::Kind::VarDecl:
      for (const VarDef &def : item.var_defs) {
        const std::string &ir_name = var_alloc_names_.at(&def);
        if (def.init) {
          const std::string value = GenerateExpr(*def.init);
          out_ << "  store " << value << ", " << ir_name << "\n";
        }
        InsertSymbol(def.name, Symbol{Symbol::Kind::Var, 0, ir_name, "", 0});
      }
      break;
    case BlockItem::Kind::Assign: {
      Symbol &symbol = LookupSymbol(item.lval);
      if (symbol.kind == Symbol::Kind::Const) {
        throw std::runtime_error("cannot assign to constant: " + item.lval);
      }
      const std::string value = GenerateExpr(*item.expr);
      out_ << "  store " << value << ", " << symbol.ir_name << "\n";
      break;
    }
    case BlockItem::Kind::Return:
      if (item.expr) {
        const std::string value = GenerateExpr(*item.expr);
        out_ << "  ret " << value << "\n";
      } else {
        out_ << "  ret\n";
      }
      entry_terminated_ = true;
      break;
    case BlockItem::Kind::ExprStmt:
      if (item.expr) {
        GenerateExpr(*item.expr);
      }
      break;
    case BlockItem::Kind::Block:
      GenerateBlock(*item.block);
      break;
    case BlockItem::Kind::If:
      GenerateIf(item);
      break;
    case BlockItem::Kind::While:
      GenerateWhile(item);
      break;
    case BlockItem::Kind::Break:
      if (loop_end_labels_.empty()) {
        throw std::runtime_error("break outside loop");
      }
      out_ << "  jump " << loop_end_labels_.back() << "\n";
      entry_terminated_ = true;
      break;
    case BlockItem::Kind::Continue:
      if (loop_entry_labels_.empty()) {
        throw std::runtime_error("continue outside loop");
      }
      out_ << "  jump " << loop_entry_labels_.back() << "\n";
      entry_terminated_ = true;
      break;
  }
}

void KoopaGenerator::GenerateIf(const BlockItem &item) {
  const std::string then_label = NewBlockName("then");
  const std::string end_label = NewBlockName("end");
  const std::string else_label = item.else_stmt ? NewBlockName("else") : end_label;

  GenerateCond(*item.expr, then_label, else_label);

  out_ << then_label << ":\n";
  entry_terminated_ = false;
  GenerateItem(*item.then_stmt);
  const bool then_terminated = entry_terminated_;
  if (!then_terminated) {
    out_ << "  jump " << end_label << "\n";
  }

  bool else_terminated = false;
  if (item.else_stmt) {
    out_ << else_label << ":\n";
    entry_terminated_ = false;
    GenerateItem(*item.else_stmt);
    else_terminated = entry_terminated_;
    if (!else_terminated) {
      out_ << "  jump " << end_label << "\n";
    }
  }

  if (item.else_stmt && then_terminated && else_terminated) {
    entry_terminated_ = true;
    return;
  }

  out_ << end_label << ":\n";
  entry_terminated_ = false;
}

void KoopaGenerator::GenerateWhile(const BlockItem &item) {
  const std::string entry_label = NewBlockName("while_entry");
  const std::string body_label = NewBlockName("while_body");
  const std::string end_label = NewBlockName("while_end");

  out_ << "  jump " << entry_label << "\n";
  out_ << entry_label << ":\n";
  entry_terminated_ = false;
  GenerateCond(*item.expr, body_label, end_label);

  out_ << body_label << ":\n";
  entry_terminated_ = false;
  loop_entry_labels_.push_back(entry_label);
  loop_end_labels_.push_back(end_label);
  GenerateItem(*item.body_stmt);
  loop_entry_labels_.pop_back();
  loop_end_labels_.pop_back();
  if (!entry_terminated_) {
    out_ << "  jump " << entry_label << "\n";
  }

  out_ << end_label << ":\n";
  entry_terminated_ = false;
}

std::string KoopaGenerator::GenerateExpr(const Expr &expr) {
  if (const auto *number = dynamic_cast<const NumberExpr *>(&expr)) {
    return std::to_string(number->value);
  }

  if (const auto *lval = dynamic_cast<const LValExpr *>(&expr)) {
    const Symbol &symbol = LookupSymbol(lval->name);
    if (symbol.kind == Symbol::Kind::Const) {
      return std::to_string(symbol.const_value);
    }
    const std::string name = NewValueName();
    out_ << "  " << name << " = load " << symbol.ir_name << "\n";
    return name;
  }

  if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
    const auto found = functions_.find(call->name);
    if (found == functions_.end()) {
      throw std::runtime_error("undefined function: " + call->name);
    }
    std::vector<std::string> args;
    for (const auto &arg : call->args) {
      args.push_back(GenerateExpr(*arg));
    }
    if (found->second.return_type == TypeKind::Void) {
      out_ << "  call @" << call->name << "(" << JoinArgs(args) << ")\n";
      return "0";
    }
    const std::string name = NewValueName();
    out_ << "  " << name << " = call @" << call->name << "(" << JoinArgs(args) << ")\n";
    return name;
  }

  if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
    const std::string operand = GenerateExpr(*unary->operand);
    switch (unary->op) {
      case UnaryOp::Plus:
        return operand;
      case UnaryOp::Minus:
        return EmitBinary("sub", "0", operand);
      case UnaryOp::Not:
        return EmitBinary("eq", operand, "0");
    }
  }

  const auto *binary = dynamic_cast<const BinaryExpr *>(&expr);
  if (binary == nullptr) {
    throw std::runtime_error("unknown expression node");
  }

  if (binary->op == BinaryOp::And) {
    const std::string &temp = logic_alloc_names_.at(binary);
    const std::string rhs_label = NewBlockName("land_val_rhs");
    const std::string end_label = NewBlockName("land_val_end");
    out_ << "  store 0, " << temp << "\n";
    GenerateCond(*binary->lhs, rhs_label, end_label);
    out_ << rhs_label << ":\n";
    entry_terminated_ = false;
    const std::string rhs_value = GenerateExpr(*binary->rhs);
    const std::string rhs_bool = EmitBinary("ne", rhs_value, "0");
    out_ << "  store " << rhs_bool << ", " << temp << "\n";
    out_ << "  jump " << end_label << "\n";
    out_ << end_label << ":\n";
    entry_terminated_ = false;
    const std::string result = NewValueName();
    out_ << "  " << result << " = load " << temp << "\n";
    return result;
  }

  if (binary->op == BinaryOp::Or) {
    const std::string &temp = logic_alloc_names_.at(binary);
    const std::string rhs_label = NewBlockName("lor_val_rhs");
    const std::string end_label = NewBlockName("lor_val_end");
    out_ << "  store 1, " << temp << "\n";
    GenerateCond(*binary->lhs, end_label, rhs_label);
    out_ << rhs_label << ":\n";
    entry_terminated_ = false;
    const std::string rhs_value = GenerateExpr(*binary->rhs);
    const std::string rhs_bool = EmitBinary("ne", rhs_value, "0");
    out_ << "  store " << rhs_bool << ", " << temp << "\n";
    out_ << "  jump " << end_label << "\n";
    out_ << end_label << ":\n";
    entry_terminated_ = false;
    const std::string result = NewValueName();
    out_ << "  " << result << " = load " << temp << "\n";
    return result;
  }

  const std::string lhs = GenerateExpr(*binary->lhs);
  const std::string rhs = GenerateExpr(*binary->rhs);
  switch (binary->op) {
    case BinaryOp::Add: return EmitBinary("add", lhs, rhs);
    case BinaryOp::Sub: return EmitBinary("sub", lhs, rhs);
    case BinaryOp::Mul: return EmitBinary("mul", lhs, rhs);
    case BinaryOp::Div: return EmitBinary("div", lhs, rhs);
    case BinaryOp::Mod: return EmitBinary("mod", lhs, rhs);
    case BinaryOp::Lt: return EmitBinary("lt", lhs, rhs);
    case BinaryOp::Gt: return EmitBinary("gt", lhs, rhs);
    case BinaryOp::Le: return EmitBinary("le", lhs, rhs);
    case BinaryOp::Ge: return EmitBinary("ge", lhs, rhs);
    case BinaryOp::Eq: return EmitBinary("eq", lhs, rhs);
    case BinaryOp::Ne: return EmitBinary("ne", lhs, rhs);
    case BinaryOp::And:
    case BinaryOp::Or:
      break;
  }
  throw std::runtime_error("unknown binary operator");
}

void KoopaGenerator::GenerateCond(const Expr &expr, const std::string &true_label,
                                  const std::string &false_label) {
  if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expr)) {
    if (binary->op == BinaryOp::And) {
      const std::string rhs_label = NewBlockName("land_rhs");
      GenerateCond(*binary->lhs, rhs_label, false_label);
      out_ << rhs_label << ":\n";
      entry_terminated_ = false;
      GenerateCond(*binary->rhs, true_label, false_label);
      return;
    }
    if (binary->op == BinaryOp::Or) {
      const std::string rhs_label = NewBlockName("lor_rhs");
      GenerateCond(*binary->lhs, true_label, rhs_label);
      out_ << rhs_label << ":\n";
      entry_terminated_ = false;
      GenerateCond(*binary->rhs, true_label, false_label);
      return;
    }
  }

  const std::string value = GenerateExpr(expr);
  out_ << "  br " << value << ", " << true_label << ", " << false_label << "\n";
  entry_terminated_ = true;
}

int KoopaGenerator::EvalConstExpr(const Expr &expr) const {
  if (const auto *number = dynamic_cast<const NumberExpr *>(&expr)) {
    return number->value;
  }
  if (const auto *lval = dynamic_cast<const LValExpr *>(&expr)) {
    const Symbol &symbol = LookupSymbol(lval->name);
    if (symbol.kind != Symbol::Kind::Const) {
      throw std::runtime_error("variable is not allowed in constant expression: " + lval->name);
    }
    return symbol.const_value;
  }
  if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
    const int value = EvalConstExpr(*unary->operand);
    switch (unary->op) {
      case UnaryOp::Plus: return value;
      case UnaryOp::Minus: return -value;
      case UnaryOp::Not: return value == 0;
    }
  }
  const auto *binary = dynamic_cast<const BinaryExpr *>(&expr);
  if (binary == nullptr) {
    throw std::runtime_error("unknown expression node");
  }
  if (binary->op == BinaryOp::And) {
    return (EvalConstExpr(*binary->lhs) != 0) && (EvalConstExpr(*binary->rhs) != 0);
  }
  if (binary->op == BinaryOp::Or) {
    return (EvalConstExpr(*binary->lhs) != 0) || (EvalConstExpr(*binary->rhs) != 0);
  }
  const int lhs = EvalConstExpr(*binary->lhs);
  const int rhs = EvalConstExpr(*binary->rhs);
  switch (binary->op) {
    case BinaryOp::Add: return lhs + rhs;
    case BinaryOp::Sub: return lhs - rhs;
    case BinaryOp::Mul: return lhs * rhs;
    case BinaryOp::Div: return lhs / rhs;
    case BinaryOp::Mod: return lhs % rhs;
    case BinaryOp::Lt: return lhs < rhs;
    case BinaryOp::Gt: return lhs > rhs;
    case BinaryOp::Le: return lhs <= rhs;
    case BinaryOp::Ge: return lhs >= rhs;
    case BinaryOp::Eq: return lhs == rhs;
    case BinaryOp::Ne: return lhs != rhs;
    case BinaryOp::And:
    case BinaryOp::Or:
      break;
  }
  throw std::runtime_error("unknown binary operator");
}

void KoopaGenerator::PushScope() { scopes_.emplace_back(); }
void KoopaGenerator::PopScope() { scopes_.pop_back(); }

void KoopaGenerator::InsertSymbol(const std::string &name, Symbol symbol) {
  if (scopes_.empty()) {
    throw std::runtime_error("internal error: no active scope");
  }
  if (!scopes_.back().emplace(name, std::move(symbol)).second) {
    throw std::runtime_error("redefined symbol: " + name);
  }
}

Symbol &KoopaGenerator::LookupSymbol(const std::string &name) {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    auto found = it->find(name);
    if (found != it->end()) {
      return found->second;
    }
  }
  throw std::runtime_error("undefined symbol: " + name);
}

const Symbol &KoopaGenerator::LookupSymbol(const std::string &name) const {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    auto found = it->find(name);
    if (found != it->end()) {
      return found->second;
    }
  }
  throw std::runtime_error("undefined symbol: " + name);
}

std::string KoopaGenerator::EmitBinary(const std::string &op,
                                       const std::string &lhs,
                                       const std::string &rhs) {
  const std::string name = NewValueName();
  out_ << "  " << name << " = " << op << " " << lhs << ", " << rhs << "\n";
  return name;
}

std::string KoopaGenerator::NewValueName() { return "%" + std::to_string(next_value_id_++); }
std::string KoopaGenerator::NewAllocName(const std::string &hint) { return "%" + hint + "_" + std::to_string(next_alloc_id_++); }
std::string KoopaGenerator::NewBlockName(const std::string &hint) { return "%" + hint + "_" + std::to_string(next_block_id_++); }

std::string KoopaGenerator::JoinArgs(const std::vector<std::string> &args) {
  std::string result;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i != 0) {
      result += ", ";
    }
    result += args[i];
  }
  return result;
}

RiscvGenerator::RiscvGenerator(std::ostream &out) : out_(out) {}

void RiscvGenerator::Generate(const Program &program) {
  RegisterLibraryFunctions();
  RegisterFunctions(program);
  scopes_.clear();
  PushScope();

  bool has_data = false;
  for (const GlobalItem &item : program.items) {
    if (item.kind == GlobalItem::Kind::ConstDecl || item.kind == GlobalItem::Kind::VarDecl) {
      if (!has_data && item.kind == GlobalItem::Kind::VarDecl) {
        out_ << "  .data\n";
        has_data = true;
      }
      GenerateGlobalItem(item);
    }
  }

  out_ << "  .text\n";
  for (const GlobalItem &item : program.items) {
    if (item.kind == GlobalItem::Kind::FuncDef) {
      GenerateFunction(*item.function);
    }
  }

  PopScope();
}

void RiscvGenerator::RegisterLibraryFunctions() {
  functions_.clear();
  functions_.emplace("getint", FuncInfo{TypeKind::Int, 0});
  functions_.emplace("getch", FuncInfo{TypeKind::Int, 0});
  functions_.emplace("getarray", FuncInfo{TypeKind::Int, 1});
  functions_.emplace("putint", FuncInfo{TypeKind::Void, 1});
  functions_.emplace("putch", FuncInfo{TypeKind::Void, 1});
  functions_.emplace("putarray", FuncInfo{TypeKind::Void, 2});
  functions_.emplace("starttime", FuncInfo{TypeKind::Void, 0});
  functions_.emplace("stoptime", FuncInfo{TypeKind::Void, 0});
}

void RiscvGenerator::RegisterFunctions(const Program &program) {
  for (const GlobalItem &item : program.items) {
    if (item.kind != GlobalItem::Kind::FuncDef) {
      continue;
    }
    const FunctionDef &function = *item.function;
    if (!functions_.emplace(function.name,
                            FuncInfo{function.return_type,
                                     static_cast<int>(function.params.size())})
             .second) {
      throw std::runtime_error("redefined function: " + function.name);
    }
  }
}

void RiscvGenerator::GenerateGlobalItem(const GlobalItem &item) {
  switch (item.kind) {
    case GlobalItem::Kind::ConstDecl:
      for (const ConstDef &def : item.const_defs) {
        InsertSymbol(def.name,
                     Symbol{Symbol::Kind::Const, EvalConstExpr(*def.init), "", "", 0});
      }
      break;
    case GlobalItem::Kind::VarDecl:
      for (const VarDef &def : item.var_defs) {
        const int value = def.init ? EvalConstExpr(*def.init) : 0;
        out_ << "  .globl " << ToAsmName(def.name) << "\n";
        out_ << ToAsmName(def.name) << ":\n";
        if (def.init) {
          out_ << "  .word " << value << "\n";
        } else {
          out_ << "  .zero 4\n";
        }
        InsertSymbol(def.name,
                     Symbol{Symbol::Kind::GlobalVar, 0, "@" + def.name, ToAsmName(def.name), 0});
      }
      break;
    case GlobalItem::Kind::FuncDef:
      break;
  }
}

void RiscvGenerator::GenerateFunction(const FunctionDef &function) {
  ResetFunctionState();
  current_return_type_ = function.return_type;
  ScanFunction(function);
  local_bytes_ = next_var_offset_;
  frame_size_ = AlignTo16(out_arg_bytes_ + local_bytes_ + max_temp_depth_ * 4 + (needs_ra_ ? 4 : 0));

  scopes_.resize(1);
  next_var_offset_ = out_arg_bytes_;
  current_terminated_ = false;

  out_ << "  .globl " << function.name << "\n";
  out_ << function.name << ":\n";
  EmitStackAdjust(-frame_size_);
  if (needs_ra_) {
    EmitStoreStack(out_, "ra", frame_size_ - 4);
  }

  PushScope();
  for (size_t i = 0; i < function.params.size(); ++i) {
    Symbol symbol;
    symbol.kind = Symbol::Kind::Var;
    symbol.stack_offset = next_var_offset_;
    next_var_offset_ += 4;
    InsertSymbol(function.params[i].name, symbol);
    if (i < 8) {
      EmitStoreStack(out_, "a" + std::to_string(i), symbol.stack_offset);
    } else {
      EmitLoadStack(out_, "t0", frame_size_ + static_cast<int>(i - 8) * 4);
      EmitStoreStack(out_, "t0", symbol.stack_offset);
    }
  }
  GenerateBlock(*function.block);
  PopScope();

  if (!current_terminated_) {
    if (function.return_type == TypeKind::Int) {
      out_ << "  li a0, 0\n";
    }
    EmitReturn();
  }
}

void RiscvGenerator::ResetFunctionState() {
  next_var_offset_ = 0;
  local_bytes_ = 0;
  out_arg_bytes_ = 0;
  max_temp_depth_ = 0;
  frame_size_ = 0;
  needs_ra_ = false;
  current_terminated_ = false;
  loop_entry_labels_.clear();
  loop_end_labels_.clear();
}

void RiscvGenerator::ScanFunction(const FunctionDef &function) {
  scopes_.resize(1);
  PushScope();
  for (const Param &param : function.params) {
    Symbol symbol;
    symbol.kind = Symbol::Kind::Var;
    symbol.stack_offset = next_var_offset_;
    next_var_offset_ += 4;
    InsertSymbol(param.name, symbol);
  }
  ScanBlock(*function.block);
  PopScope();
}

void RiscvGenerator::ScanBlock(const Block &block) {
  PushScope();
  for (const BlockItem &item : block.items) {
    ScanItem(item);
  }
  PopScope();
}

void RiscvGenerator::ScanItem(const BlockItem &item) {
  switch (item.kind) {
    case BlockItem::Kind::ConstDecl:
      for (const ConstDef &def : item.const_defs) {
        InsertSymbol(def.name,
                     Symbol{Symbol::Kind::Const, EvalConstExpr(*def.init), "", "", 0});
      }
      break;
    case BlockItem::Kind::VarDecl:
      for (const VarDef &def : item.var_defs) {
        if (def.init) {
          ScanExpr(*def.init, 0);
        }
        Symbol symbol;
        symbol.kind = Symbol::Kind::Var;
        symbol.stack_offset = next_var_offset_;
        next_var_offset_ += 4;
        InsertSymbol(def.name, symbol);
      }
      break;
    case BlockItem::Kind::Assign:
      LookupSymbol(item.lval);
      ScanExpr(*item.expr, 0);
      break;
    case BlockItem::Kind::Return:
      if (item.expr) {
        ScanExpr(*item.expr, 0);
      }
      break;
    case BlockItem::Kind::ExprStmt:
      if (item.expr) {
        ScanExpr(*item.expr, 0);
      }
      break;
    case BlockItem::Kind::Block:
      ScanBlock(*item.block);
      break;
    case BlockItem::Kind::If:
      ScanExpr(*item.expr, 0);
      ScanItem(*item.then_stmt);
      if (item.else_stmt) {
        ScanItem(*item.else_stmt);
      }
      break;
    case BlockItem::Kind::While:
      ScanExpr(*item.expr, 0);
      ScanItem(*item.body_stmt);
      break;
    case BlockItem::Kind::Break:
    case BlockItem::Kind::Continue:
      break;
  }
}

void RiscvGenerator::ScanExpr(const Expr &expr, int depth) {
  if (const auto *lval = dynamic_cast<const LValExpr *>(&expr)) {
    LookupSymbol(lval->name);
    return;
  }
  if (dynamic_cast<const NumberExpr *>(&expr) != nullptr) {
    return;
  }
  if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
    needs_ra_ = true;
    if (call->args.size() > 8) {
      out_arg_bytes_ = std::max(out_arg_bytes_, static_cast<int>(call->args.size() - 8) * 4);
    }
    const int arg_count = static_cast<int>(call->args.size());
    max_temp_depth_ = std::max(max_temp_depth_, depth + arg_count);
    for (const auto &arg : call->args) {
      ScanExpr(*arg, depth + arg_count);
    }
    return;
  }
  if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
    ScanExpr(*unary->operand, depth);
    return;
  }
  const auto *binary = dynamic_cast<const BinaryExpr *>(&expr);
  if (binary == nullptr) {
    throw std::runtime_error("unknown expression node");
  }
  max_temp_depth_ = std::max(max_temp_depth_, depth + 1);
  ScanExpr(*binary->lhs, depth);
  ScanExpr(*binary->rhs, depth + 1);
}

void RiscvGenerator::GenerateBlock(const Block &block) {
  PushScope();
  for (const BlockItem &item : block.items) {
    if (current_terminated_) {
      break;
    }
    GenerateItem(item);
  }
  PopScope();
}

void RiscvGenerator::GenerateItem(const BlockItem &item) {
  switch (item.kind) {
    case BlockItem::Kind::ConstDecl:
      for (const ConstDef &def : item.const_defs) {
        InsertSymbol(def.name,
                     Symbol{Symbol::Kind::Const, EvalConstExpr(*def.init), "", "", 0});
      }
      break;
    case BlockItem::Kind::VarDecl:
      for (const VarDef &def : item.var_defs) {
        Symbol symbol;
        symbol.kind = Symbol::Kind::Var;
        symbol.stack_offset = next_var_offset_;
        next_var_offset_ += 4;
        if (def.init) {
          GenerateExpr(*def.init);
          StoreToSymbol(symbol, "t0");
        }
        InsertSymbol(def.name, symbol);
      }
      break;
    case BlockItem::Kind::Assign: {
      const Symbol &symbol = LookupSymbol(item.lval);
      if (symbol.kind == Symbol::Kind::Const) {
        throw std::runtime_error("cannot assign to constant: " + item.lval);
      }
      GenerateExpr(*item.expr);
      StoreToSymbol(symbol, "t0");
      break;
    }
    case BlockItem::Kind::Return:
      if (item.expr) {
        GenerateExpr(*item.expr);
        out_ << "  mv a0, t0\n";
      }
      EmitReturn();
      current_terminated_ = true;
      break;
    case BlockItem::Kind::ExprStmt:
      if (item.expr) {
        GenerateExpr(*item.expr);
      }
      break;
    case BlockItem::Kind::Block:
      GenerateBlock(*item.block);
      break;
    case BlockItem::Kind::If:
      GenerateIf(item);
      break;
    case BlockItem::Kind::While:
      GenerateWhile(item);
      break;
    case BlockItem::Kind::Break:
      out_ << "  j " << CurrentLoopEnd() << "\n";
      current_terminated_ = true;
      break;
    case BlockItem::Kind::Continue:
      out_ << "  j " << CurrentLoopEntry() << "\n";
      current_terminated_ = true;
      break;
  }
}

void RiscvGenerator::GenerateIf(const BlockItem &item) {
  const std::string then_label = NewLabel("then");
  const std::string end_label = NewLabel("end");
  const std::string else_label = item.else_stmt ? NewLabel("else") : end_label;
  GenerateCond(*item.expr, then_label, else_label);
  out_ << then_label << ":\n";
  current_terminated_ = false;
  GenerateItem(*item.then_stmt);
  const bool then_terminated = current_terminated_;
  if (!then_terminated) {
    out_ << "  j " << end_label << "\n";
  }
  bool else_terminated = false;
  if (item.else_stmt) {
    out_ << else_label << ":\n";
    current_terminated_ = false;
    GenerateItem(*item.else_stmt);
    else_terminated = current_terminated_;
    if (!else_terminated) {
      out_ << "  j " << end_label << "\n";
    }
  }
  if (item.else_stmt && then_terminated && else_terminated) {
    current_terminated_ = true;
    return;
  }
  out_ << end_label << ":\n";
  current_terminated_ = false;
}

void RiscvGenerator::GenerateWhile(const BlockItem &item) {
  const std::string entry_label = NewLabel("while_entry");
  const std::string body_label = NewLabel("while_body");
  const std::string end_label = NewLabel("while_end");
  out_ << "  j " << entry_label << "\n";
  out_ << entry_label << ":\n";
  current_terminated_ = false;
  GenerateCond(*item.expr, body_label, end_label);
  out_ << body_label << ":\n";
  current_terminated_ = false;
  loop_entry_labels_.push_back(entry_label);
  loop_end_labels_.push_back(end_label);
  GenerateItem(*item.body_stmt);
  loop_entry_labels_.pop_back();
  loop_end_labels_.pop_back();
  if (!current_terminated_) {
    out_ << "  j " << entry_label << "\n";
  }
  out_ << end_label << ":\n";
  current_terminated_ = false;
}

std::string RiscvGenerator::CurrentLoopEntry() const {
  if (loop_entry_labels_.empty()) {
    throw std::runtime_error("continue outside loop");
  }
  return loop_entry_labels_.back();
}

std::string RiscvGenerator::CurrentLoopEnd() const {
  if (loop_end_labels_.empty()) {
    throw std::runtime_error("break outside loop");
  }
  return loop_end_labels_.back();
}

void RiscvGenerator::GenerateExpr(const Expr &expr, int depth) {
  if (const auto *number = dynamic_cast<const NumberExpr *>(&expr)) {
    out_ << "  li t0, " << number->value << "\n";
    return;
  }
  if (const auto *lval = dynamic_cast<const LValExpr *>(&expr)) {
    const Symbol &symbol = LookupSymbol(lval->name);
    if (symbol.kind == Symbol::Kind::Const) {
      out_ << "  li t0, " << symbol.const_value << "\n";
    } else {
      LoadFromSymbol(symbol, "t0");
    }
    return;
  }
  if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
    const auto found = functions_.find(call->name);
    if (found == functions_.end()) {
      throw std::runtime_error("undefined function: " + call->name);
    }
    const int arg_count = static_cast<int>(call->args.size());
    for (size_t i = 0; i < call->args.size(); ++i) {
      GenerateExpr(*call->args[i], depth + arg_count);
      EmitStoreStack(out_, "t0", TempOffset(depth + static_cast<int>(i)));
    }
    for (size_t i = 0; i < call->args.size(); ++i) {
      EmitLoadStack(out_, "t0", TempOffset(depth + static_cast<int>(i)));
      if (i < 8) {
        out_ << "  mv a" << i << ", t0\n";
      } else {
        EmitStoreStack(out_, "t0", static_cast<int>(i - 8) * 4);
      }
    }
    out_ << "  call " << call->name << "\n";
    if (found->second.return_type == TypeKind::Int) {
      out_ << "  mv t0, a0\n";
    } else {
      out_ << "  li t0, 0\n";
    }
    return;
  }
  if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
    GenerateExpr(*unary->operand, depth);
    switch (unary->op) {
      case UnaryOp::Plus:
        break;
      case UnaryOp::Minus:
        out_ << "  sub t0, x0, t0\n";
        break;
      case UnaryOp::Not:
        out_ << "  seqz t0, t0\n";
        break;
    }
    return;
  }
  const auto *binary = dynamic_cast<const BinaryExpr *>(&expr);
  if (binary == nullptr) {
    throw std::runtime_error("unknown expression node");
  }
  if (binary->op == BinaryOp::And || binary->op == BinaryOp::Or) {
    const std::string true_label = NewLabel("logic_true");
    const std::string false_label = NewLabel("logic_false");
    const std::string end_label = NewLabel("logic_end");
    GenerateCond(expr, true_label, false_label, depth);
    out_ << true_label << ":\n";
    out_ << "  li t0, 1\n";
    out_ << "  j " << end_label << "\n";
    out_ << false_label << ":\n";
    out_ << "  li t0, 0\n";
    out_ << end_label << ":\n";
    return;
  }
  GenerateExpr(*binary->lhs, depth);
  EmitStoreStack(out_, "t0", TempOffset(depth));
  GenerateExpr(*binary->rhs, depth + 1);
  EmitLoadStack(out_, "t1", TempOffset(depth));
  switch (binary->op) {
    case BinaryOp::Add: out_ << "  add t0, t1, t0\n"; break;
    case BinaryOp::Sub: out_ << "  sub t0, t1, t0\n"; break;
    case BinaryOp::Mul: out_ << "  mul t0, t1, t0\n"; break;
    case BinaryOp::Div: out_ << "  div t0, t1, t0\n"; break;
    case BinaryOp::Mod: out_ << "  rem t0, t1, t0\n"; break;
    case BinaryOp::Lt: out_ << "  slt t0, t1, t0\n"; break;
    case BinaryOp::Gt: out_ << "  sgt t0, t1, t0\n"; break;
    case BinaryOp::Le:
      out_ << "  sgt t0, t1, t0\n";
      out_ << "  seqz t0, t0\n";
      break;
    case BinaryOp::Ge:
      out_ << "  slt t0, t1, t0\n";
      out_ << "  seqz t0, t0\n";
      break;
    case BinaryOp::Eq:
      out_ << "  xor t0, t1, t0\n";
      out_ << "  seqz t0, t0\n";
      break;
    case BinaryOp::Ne:
      out_ << "  xor t0, t1, t0\n";
      out_ << "  snez t0, t0\n";
      break;
    case BinaryOp::And:
      out_ << "  snez t1, t1\n";
      out_ << "  snez t0, t0\n";
      out_ << "  and t0, t1, t0\n";
      break;
    case BinaryOp::Or:
      out_ << "  or t0, t1, t0\n";
      out_ << "  snez t0, t0\n";
      break;
  }
}

void RiscvGenerator::GenerateCond(const Expr &expr, const std::string &true_label,
                                  const std::string &false_label, int depth) {
  if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expr)) {
    if (binary->op == BinaryOp::And) {
      const std::string rhs_label = NewLabel("land_rhs");
      GenerateCond(*binary->lhs, rhs_label, false_label, depth);
      out_ << rhs_label << ":\n";
      GenerateCond(*binary->rhs, true_label, false_label, depth);
      return;
    }
    if (binary->op == BinaryOp::Or) {
      const std::string rhs_label = NewLabel("lor_rhs");
      GenerateCond(*binary->lhs, true_label, rhs_label, depth);
      out_ << rhs_label << ":\n";
      GenerateCond(*binary->rhs, true_label, false_label, depth);
      return;
    }
  }
  GenerateExpr(expr, depth);
  out_ << "  bnez t0, " << true_label << "\n";
  out_ << "  j " << false_label << "\n";
}

int RiscvGenerator::EvalConstExpr(const Expr &expr) const {
  if (const auto *number = dynamic_cast<const NumberExpr *>(&expr)) {
    return number->value;
  }
  if (const auto *lval = dynamic_cast<const LValExpr *>(&expr)) {
    const Symbol &symbol = LookupSymbol(lval->name);
    if (symbol.kind != Symbol::Kind::Const) {
      throw std::runtime_error("variable is not allowed in constant expression: " + lval->name);
    }
    return symbol.const_value;
  }
  if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
    const int value = EvalConstExpr(*unary->operand);
    switch (unary->op) {
      case UnaryOp::Plus: return value;
      case UnaryOp::Minus: return -value;
      case UnaryOp::Not: return value == 0;
    }
  }
  const auto *binary = dynamic_cast<const BinaryExpr *>(&expr);
  if (binary == nullptr) {
    throw std::runtime_error("unknown expression node");
  }
  if (binary->op == BinaryOp::And) {
    return (EvalConstExpr(*binary->lhs) != 0) && (EvalConstExpr(*binary->rhs) != 0);
  }
  if (binary->op == BinaryOp::Or) {
    return (EvalConstExpr(*binary->lhs) != 0) || (EvalConstExpr(*binary->rhs) != 0);
  }
  const int lhs = EvalConstExpr(*binary->lhs);
  const int rhs = EvalConstExpr(*binary->rhs);
  switch (binary->op) {
    case BinaryOp::Add: return lhs + rhs;
    case BinaryOp::Sub: return lhs - rhs;
    case BinaryOp::Mul: return lhs * rhs;
    case BinaryOp::Div: return lhs / rhs;
    case BinaryOp::Mod: return lhs % rhs;
    case BinaryOp::Lt: return lhs < rhs;
    case BinaryOp::Gt: return lhs > rhs;
    case BinaryOp::Le: return lhs <= rhs;
    case BinaryOp::Ge: return lhs >= rhs;
    case BinaryOp::Eq: return lhs == rhs;
    case BinaryOp::Ne: return lhs != rhs;
    case BinaryOp::And:
    case BinaryOp::Or:
      break;
  }
  throw std::runtime_error("unknown binary operator");
}

void RiscvGenerator::PushScope() { scopes_.emplace_back(); }
void RiscvGenerator::PopScope() { scopes_.pop_back(); }

void RiscvGenerator::InsertSymbol(const std::string &name, Symbol symbol) {
  if (scopes_.empty()) {
    throw std::runtime_error("internal error: no active scope");
  }
  if (!scopes_.back().emplace(name, std::move(symbol)).second) {
    throw std::runtime_error("redefined symbol: " + name);
  }
}

Symbol &RiscvGenerator::LookupSymbol(const std::string &name) {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    auto found = it->find(name);
    if (found != it->end()) {
      return found->second;
    }
  }
  throw std::runtime_error("undefined symbol: " + name);
}

const Symbol &RiscvGenerator::LookupSymbol(const std::string &name) const {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    auto found = it->find(name);
    if (found != it->end()) {
      return found->second;
    }
  }
  throw std::runtime_error("undefined symbol: " + name);
}

int RiscvGenerator::TempOffset(int depth) const {
  return out_arg_bytes_ + local_bytes_ + depth * 4;
}

void RiscvGenerator::EmitStackAdjust(int bytes) {
  if (bytes == 0) {
    return;
  }
  if (Fits12Bit(bytes)) {
    out_ << "  addi sp, sp, " << bytes << "\n";
    return;
  }
  out_ << "  li t0, " << bytes << "\n";
  out_ << "  add sp, sp, t0\n";
}

void RiscvGenerator::EmitReturn() {
  if (needs_ra_) {
    EmitLoadStack(out_, "ra", frame_size_ - 4);
  }
  EmitStackAdjust(frame_size_);
  out_ << "  ret\n";
}

void RiscvGenerator::StoreToSymbol(const Symbol &symbol, const std::string &reg) {
  if (symbol.kind == Symbol::Kind::GlobalVar) {
    out_ << "  la t1, " << symbol.asm_name << "\n";
    out_ << "  sw " << reg << ", 0(t1)\n";
  } else {
    EmitStoreStack(out_, reg, symbol.stack_offset);
  }
}

void RiscvGenerator::LoadFromSymbol(const Symbol &symbol, const std::string &reg) {
  if (symbol.kind == Symbol::Kind::GlobalVar) {
    out_ << "  la " << reg << ", " << symbol.asm_name << "\n";
    out_ << "  lw " << reg << ", 0(" << reg << ")\n";
  } else {
    EmitLoadStack(out_, reg, symbol.stack_offset);
  }
}

std::string RiscvGenerator::NewLabel(const std::string &hint) {
  return ".L" + hint + "_" + std::to_string(next_label_id_++);
}

int RiscvGenerator::AlignTo16(int bytes) { return (bytes + 15) / 16 * 16; }

void WriteKoopa(const std::string &path, const Program &program) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open output file: " + path);
  }
  KoopaGenerator generator(out);
  generator.Generate(program);
}

void WriteRiscv(const std::string &path, const Program &program) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open output file: " + path);
  }
  RiscvGenerator generator(out);
  generator.Generate(program);
}
