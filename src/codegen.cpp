#include "codegen.h"

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

}  // namespace

KoopaGenerator::KoopaGenerator(std::ostream &out) : out_(out) {}

void KoopaGenerator::Generate(const Program &program) {
  out_ << "fun @main(): i32 {\n";
  out_ << "%entry:\n";
  GenerateBlock(*program.block);
  out_ << "}\n";
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
                     Symbol{Symbol::Kind::Const, EvalConstExpr(*def.init), "", 0});
      }
      break;
    case BlockItem::Kind::VarDecl:
      for (const VarDef &def : item.var_defs) {
        const std::string ir_name = NewAllocName(def.name);
        out_ << "  " << ir_name << " = alloc i32\n";
        if (def.init) {
          const std::string value = GenerateExpr(*def.init);
          out_ << "  store " << value << ", " << ir_name << "\n";
        }
        InsertSymbol(def.name, Symbol{Symbol::Kind::Var, 0, ir_name, 0});
      }
      break;
    case BlockItem::Kind::Assign: {
      Symbol &symbol = LookupSymbol(item.lval);
      if (symbol.kind != Symbol::Kind::Var) {
        throw std::runtime_error("cannot assign to constant: " + item.lval);
      }
      const std::string value = GenerateExpr(*item.expr);
      out_ << "  store " << value << ", " << symbol.ir_name << "\n";
      break;
    }
    case BlockItem::Kind::Return: {
      const std::string value = GenerateExpr(*item.expr);
      out_ << "  ret " << value << "\n";
      entry_terminated_ = true;
      break;
    }
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
  const std::string else_label =
      item.else_stmt ? NewBlockName("else") : end_label;

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

  const std::string lhs = GenerateExpr(*binary->lhs);
  const std::string rhs = GenerateExpr(*binary->rhs);
  switch (binary->op) {
    case BinaryOp::Add:
      return EmitBinary("add", lhs, rhs);
    case BinaryOp::Sub:
      return EmitBinary("sub", lhs, rhs);
    case BinaryOp::Mul:
      return EmitBinary("mul", lhs, rhs);
    case BinaryOp::Div:
      return EmitBinary("div", lhs, rhs);
    case BinaryOp::Mod:
      return EmitBinary("mod", lhs, rhs);
    case BinaryOp::Lt:
      return EmitBinary("lt", lhs, rhs);
    case BinaryOp::Gt:
      return EmitBinary("gt", lhs, rhs);
    case BinaryOp::Le:
      return EmitBinary("le", lhs, rhs);
    case BinaryOp::Ge:
      return EmitBinary("ge", lhs, rhs);
    case BinaryOp::Eq:
      return EmitBinary("eq", lhs, rhs);
    case BinaryOp::Ne:
      return EmitBinary("ne", lhs, rhs);
    case BinaryOp::And: {
      const std::string lhs_bool = EmitBinary("ne", lhs, "0");
      const std::string rhs_bool = EmitBinary("ne", rhs, "0");
      return EmitBinary("and", lhs_bool, rhs_bool);
    }
    case BinaryOp::Or: {
      const std::string lhs_bool = EmitBinary("ne", lhs, "0");
      const std::string rhs_bool = EmitBinary("ne", rhs, "0");
      return EmitBinary("or", lhs_bool, rhs_bool);
    }
  }
  throw std::runtime_error("unknown binary operator");
}

void KoopaGenerator::GenerateCond(const Expr &expr,
                                  const std::string &true_label,
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
  out_ << "  br " << value << ", " << true_label << ", " << false_label
       << "\n";
  entry_terminated_ = true;
}

int KoopaGenerator::EvalConstExpr(const Expr &expr) const {
  if (const auto *number = dynamic_cast<const NumberExpr *>(&expr)) {
    return number->value;
  }
  if (const auto *lval = dynamic_cast<const LValExpr *>(&expr)) {
    const Symbol &symbol = LookupSymbol(lval->name);
    if (symbol.kind != Symbol::Kind::Const) {
      throw std::runtime_error("variable is not allowed in constant expression: " +
                               lval->name);
    }
    return symbol.const_value;
  }
  if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
    const int value = EvalConstExpr(*unary->operand);
    switch (unary->op) {
      case UnaryOp::Plus:
        return value;
      case UnaryOp::Minus:
        return -value;
      case UnaryOp::Not:
        return value == 0;
    }
  }

  const auto *binary = dynamic_cast<const BinaryExpr *>(&expr);
  if (binary == nullptr) {
    throw std::runtime_error("unknown expression node");
  }
  const int lhs = EvalConstExpr(*binary->lhs);
  const int rhs = EvalConstExpr(*binary->rhs);
  switch (binary->op) {
    case BinaryOp::Add:
      return lhs + rhs;
    case BinaryOp::Sub:
      return lhs - rhs;
    case BinaryOp::Mul:
      return lhs * rhs;
    case BinaryOp::Div:
      return lhs / rhs;
    case BinaryOp::Mod:
      return lhs % rhs;
    case BinaryOp::Lt:
      return lhs < rhs;
    case BinaryOp::Gt:
      return lhs > rhs;
    case BinaryOp::Le:
      return lhs <= rhs;
    case BinaryOp::Ge:
      return lhs >= rhs;
    case BinaryOp::Eq:
      return lhs == rhs;
    case BinaryOp::Ne:
      return lhs != rhs;
    case BinaryOp::And:
      return (lhs != 0) && (rhs != 0);
    case BinaryOp::Or:
      return (lhs != 0) || (rhs != 0);
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

std::string KoopaGenerator::NewValueName() {
  return "%" + std::to_string(next_value_id_++);
}

std::string KoopaGenerator::NewAllocName(const std::string &hint) {
  return "@" + hint + "_" + std::to_string(next_alloc_id_++);
}

std::string KoopaGenerator::NewBlockName(const std::string &hint) {
  return "%" + hint + "_" + std::to_string(next_block_id_++);
}

RiscvGenerator::RiscvGenerator(std::ostream &out) : out_(out) {}

void RiscvGenerator::Generate(const Program &program) {
  ScanBlock(*program.block);
  frame_size_ = AlignTo16(next_var_offset_ + max_temp_depth_ * 4);

  scopes_.clear();
  next_var_offset_ = 0;

  out_ << "  .text\n";
  out_ << "  .globl main\n";
  out_ << "main:\n";
  EmitStackAdjust(-frame_size_);
  GenerateBlock(*program.block);
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
                     Symbol{Symbol::Kind::Const, EvalConstExpr(*def.init), "", 0});
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
    case BlockItem::Kind::Assign: {
      const Symbol &symbol = LookupSymbol(item.lval);
      if (symbol.kind != Symbol::Kind::Var) {
        throw std::runtime_error("cannot assign to constant: " + item.lval);
      }
      ScanExpr(*item.expr, 0);
      break;
    }
    case BlockItem::Kind::Return:
      ScanExpr(*item.expr, 0);
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
  if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
    ScanExpr(*unary->operand, depth);
    return;
  }
  const auto *binary = dynamic_cast<const BinaryExpr *>(&expr);
  if (binary == nullptr) {
    throw std::runtime_error("unknown expression node");
  }
  if (max_temp_depth_ < depth + 1) {
    max_temp_depth_ = depth + 1;
  }
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
                     Symbol{Symbol::Kind::Const, EvalConstExpr(*def.init), "", 0});
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
          EmitStoreStack(out_, "t0", symbol.stack_offset);
        }
        InsertSymbol(def.name, symbol);
      }
      break;
    case BlockItem::Kind::Assign: {
      const Symbol &symbol = LookupSymbol(item.lval);
      if (symbol.kind != Symbol::Kind::Var) {
        throw std::runtime_error("cannot assign to constant: " + item.lval);
      }
      GenerateExpr(*item.expr);
      EmitStoreStack(out_, "t0", symbol.stack_offset);
      break;
    }
    case BlockItem::Kind::Return:
      GenerateExpr(*item.expr);
      out_ << "  mv a0, t0\n";
      EmitStackAdjust(frame_size_);
      out_ << "  ret\n";
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
      EmitLoadStack(out_, "t0", symbol.stack_offset);
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

  GenerateExpr(*binary->lhs, depth);
  EmitStoreStack(out_, "t0", TempOffset(depth));
  GenerateExpr(*binary->rhs, depth + 1);
  EmitLoadStack(out_, "t1", TempOffset(depth));

  switch (binary->op) {
    case BinaryOp::Add:
      out_ << "  add t0, t1, t0\n";
      break;
    case BinaryOp::Sub:
      out_ << "  sub t0, t1, t0\n";
      break;
    case BinaryOp::Mul:
      out_ << "  mul t0, t1, t0\n";
      break;
    case BinaryOp::Div:
      out_ << "  div t0, t1, t0\n";
      break;
    case BinaryOp::Mod:
      out_ << "  rem t0, t1, t0\n";
      break;
    case BinaryOp::Lt:
      out_ << "  slt t0, t1, t0\n";
      break;
    case BinaryOp::Gt:
      out_ << "  sgt t0, t1, t0\n";
      break;
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

void RiscvGenerator::GenerateCond(const Expr &expr,
                                  const std::string &true_label,
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
      throw std::runtime_error("variable is not allowed in constant expression: " +
                               lval->name);
    }
    return symbol.const_value;
  }
  if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
    const int value = EvalConstExpr(*unary->operand);
    switch (unary->op) {
      case UnaryOp::Plus:
        return value;
      case UnaryOp::Minus:
        return -value;
      case UnaryOp::Not:
        return value == 0;
    }
  }

  const auto *binary = dynamic_cast<const BinaryExpr *>(&expr);
  if (binary == nullptr) {
    throw std::runtime_error("unknown expression node");
  }
  const int lhs = EvalConstExpr(*binary->lhs);
  const int rhs = EvalConstExpr(*binary->rhs);
  switch (binary->op) {
    case BinaryOp::Add:
      return lhs + rhs;
    case BinaryOp::Sub:
      return lhs - rhs;
    case BinaryOp::Mul:
      return lhs * rhs;
    case BinaryOp::Div:
      return lhs / rhs;
    case BinaryOp::Mod:
      return lhs % rhs;
    case BinaryOp::Lt:
      return lhs < rhs;
    case BinaryOp::Gt:
      return lhs > rhs;
    case BinaryOp::Le:
      return lhs <= rhs;
    case BinaryOp::Ge:
      return lhs >= rhs;
    case BinaryOp::Eq:
      return lhs == rhs;
    case BinaryOp::Ne:
      return lhs != rhs;
    case BinaryOp::And:
      return (lhs != 0) && (rhs != 0);
    case BinaryOp::Or:
      return (lhs != 0) || (rhs != 0);
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
  return next_var_offset_ + depth * 4;
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
