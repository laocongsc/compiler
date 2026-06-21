#include "codegen.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <numeric>
#include <ostream>
#include <sstream>
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

int Product(const std::vector<int> &dims, size_t start = 0) {
  int result = 1;
  for (size_t i = start; i < dims.size(); ++i) {
    result *= dims[i];
  }
  return result;
}

int TypeSize(const std::vector<int> &dims, size_t start = 0) {
  return Product(dims, start) * 4;
}

std::string ArrayTypeName(const std::vector<int> &dims, size_t start = 0) {
  std::string type = "i32";
  for (size_t i = dims.size(); i > start; --i) {
    type = "[" + type + ", " + std::to_string(dims[i - 1]) + "]";
  }
  return type;
}

std::string PointerParamType(const std::vector<int> &dims) {
  return "*" + ArrayTypeName(dims);
}

void FlattenInitExprs(const InitVal &init, const std::vector<int> &dims,
                      size_t dim, size_t &pos,
                      std::vector<const Expr *> &values) {
  if (!init.is_list) {
    if (pos < values.size()) {
      values[pos] = init.expr.get();
    }
    ++pos;
    return;
  }

  const size_t start = pos;
  for (const InitVal &child : init.list) {
    if (!child.is_list) {
      FlattenInitExprs(child, dims, dims.size(), pos, values);
      continue;
    }

    size_t child_dim = dim + 1;
    if (child_dim >= dims.size()) {
      FlattenInitExprs(child, dims, child_dim, pos, values);
      continue;
    }
    const size_t rel = pos - start;
    while (child_dim + 1 < dims.size() &&
           rel % static_cast<size_t>(Product(dims, child_dim)) != 0) {
      ++child_dim;
    }
    while (child_dim + 1 < dims.size() &&
           rel % static_cast<size_t>(Product(dims, child_dim)) == 0 &&
           rel % static_cast<size_t>(Product(dims, child_dim + 1)) == 0 &&
           rel != 0) {
      break;
    }
    const size_t child_start = pos;
    FlattenInitExprs(child, dims, child_dim, pos, values);
    const size_t child_size = static_cast<size_t>(Product(dims, child_dim));
    pos = std::max(pos, child_start + child_size);
  }
}

std::vector<const Expr *> FlattenInitExprs(const InitVal &init,
                                           const std::vector<int> &dims) {
  std::vector<const Expr *> values(Product(dims), nullptr);
  size_t pos = 0;
  FlattenInitExprs(init, dims, 0, pos, values);
  return values;
}

std::string FormatAggregate(const std::vector<int> &values,
                            const std::vector<int> &dims, size_t dim,
                            size_t &offset) {
  if (dim == dims.size()) {
    return std::to_string(values[offset++]);
  }
  std::string result = "{";
  for (int i = 0; i < dims[dim]; ++i) {
    if (i != 0) {
      result += ", ";
    }
    result += FormatAggregate(values, dims, dim + 1, offset);
  }
  result += "}";
  return result;
}

std::string FormatAggregate(const std::vector<int> &values,
                            const std::vector<int> &dims) {
  if (std::all_of(values.begin(), values.end(), [](int value) { return value == 0; })) {
    return "zeroinit";
  }
  size_t offset = 0;
  return FormatAggregate(values, dims, 0, offset);
}

Symbol ScalarConstSymbol(int value) {
  Symbol symbol;
  symbol.kind = Symbol::Kind::Const;
  symbol.const_value = value;
  return symbol;
}

Symbol GlobalScalarSymbol(const std::string &name) {
  Symbol symbol;
  symbol.kind = Symbol::Kind::GlobalVar;
  symbol.ir_name = "@" + name;
  symbol.asm_name = name;
  return symbol;
}


std::string TrimLeft(std::string line) {
  const size_t first = line.find_first_not_of(" \t");
  if (first == std::string::npos) {
    return "";
  }
  return line.substr(first);
}

bool IsLabelLine(const std::string &line, std::string *label = nullptr) {
  const std::string trimmed = TrimLeft(line);
  if (trimmed.empty() || (trimmed[0] == '.' && trimmed.rfind(".globl", 0) == 0)) {
    return false;
  }
  if (trimmed.back() != ':') {
    return false;
  }
  const std::string name = trimmed.substr(0, trimmed.size() - 1);
  if (name.empty() || name.find(' ') != std::string::npos ||
      name.find('\t') != std::string::npos) {
    return false;
  }
  if (label != nullptr) {
    *label = name;
  }
  return true;
}

bool ParseJump(const std::string &line, std::string *target) {
  const std::string trimmed = TrimLeft(line);
  constexpr const char *prefix = "j ";
  if (trimmed.rfind(prefix, 0) != 0) {
    return false;
  }
  const std::string rest = trimmed.substr(2);
  if (rest.empty() || rest.find_first_of(" \t,") != std::string::npos) {
    return false;
  }
  *target = rest;
  return true;
}

bool ParseLoadStore(const std::string &line, std::string *op,
                    std::string *reg, std::string *addr) {
  const std::string trimmed = TrimLeft(line);
  if (trimmed.rfind("lw ", 0) != 0 && trimmed.rfind("sw ", 0) != 0) {
    return false;
  }
  const size_t comma = trimmed.find(',');
  if (comma == std::string::npos) {
    return false;
  }
  *op = trimmed.substr(0, 2);
  *reg = trimmed.substr(3, comma - 3);
  size_t addr_start = comma + 1;
  while (addr_start < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[addr_start]))) {
    ++addr_start;
  }
  *addr = trimmed.substr(addr_start);
  return !reg->empty() && !addr->empty();
}

std::string OptimizeAsm(const std::string &asm_text) {
  std::vector<std::string> lines;
  std::istringstream in(asm_text);
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }

  std::vector<std::string> optimized;
  optimized.reserve(lines.size());
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string jump_target;
    std::string next_label;
    if (i + 1 < lines.size() && ParseJump(lines[i], &jump_target) &&
        IsLabelLine(lines[i + 1], &next_label) && jump_target == next_label) {
      continue;
    }

    std::string op;
    std::string reg;
    std::string addr;
    if (ParseLoadStore(lines[i], &op, &reg, &addr) && op == "lw" &&
        !optimized.empty()) {
      std::string prev_op;
      std::string prev_reg;
      std::string prev_addr;
      if (ParseLoadStore(optimized.back(), &prev_op, &prev_reg, &prev_addr) &&
          prev_op == "sw" && prev_addr == addr) {
        if (prev_reg == reg) {
          continue;
        }
        optimized.push_back("  mv " + reg + ", " + prev_reg);
        continue;
      }
    }

    const std::string trimmed = TrimLeft(lines[i]);
    if (trimmed.rfind("mv ", 0) == 0) {
      const size_t comma = trimmed.find(',');
      if (comma != std::string::npos) {
        const std::string dst = trimmed.substr(3, comma - 3);
        size_t src_start = comma + 1;
        while (src_start < trimmed.size() &&
               std::isspace(static_cast<unsigned char>(trimmed[src_start]))) {
          ++src_start;
        }
        if (dst == trimmed.substr(src_start)) {
          continue;
        }
      }
    }

    optimized.push_back(lines[i]);
  }

  std::ostringstream out;
  for (const std::string &optimized_line : optimized) {
    out << optimized_line << '\n';
  }
  return out.str();
}

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
        const std::vector<int> dims = EvalDimensions(def.dimensions);
        if (dims.empty()) {
          InsertSymbol(def.name,
                       ScalarConstSymbol(EvalConstExpr(*def.init.expr)));
        } else {
          const std::vector<const Expr *> exprs = FlattenInitExprs(def.init, dims);
          std::vector<int> values(exprs.size(), 0);
          for (size_t i = 0; i < exprs.size(); ++i) {
            if (exprs[i] != nullptr) {
              values[i] = EvalConstExpr(*exprs[i]);
            }
          }
          out_ << "global @" << def.name << " = alloc " << ArrayTypeName(dims)
               << ", " << FormatAggregate(values, dims) << "\n";
          Symbol symbol;
          symbol.kind = Symbol::Kind::Const;
          symbol.ir_name = "@" + def.name;
          symbol.asm_name = def.name;
          symbol.dimensions = dims;
          InsertSymbol(def.name, std::move(symbol));
        }
      }
      break;
    case GlobalItem::Kind::VarDecl:
      for (const VarDef &def : item.var_defs) {
        const std::vector<int> dims = EvalDimensions(def.dimensions);
        if (dims.empty()) {
          const int value = def.has_init ? EvalConstExpr(*def.init.expr) : 0;
          out_ << "global @" << def.name << " = alloc i32, ";
          if (def.has_init) {
            out_ << value << "\n";
          } else {
            out_ << "zeroinit\n";
          }
          InsertSymbol(def.name,
                       GlobalScalarSymbol(def.name));
        } else {
          std::vector<int> values(Product(dims), 0);
          if (def.has_init) {
            const std::vector<const Expr *> exprs = FlattenInitExprs(def.init, dims);
            for (size_t i = 0; i < exprs.size(); ++i) {
              if (exprs[i] != nullptr) {
                values[i] = EvalConstExpr(*exprs[i]);
              }
            }
          }
          out_ << "global @" << def.name << " = alloc " << ArrayTypeName(dims)
               << ", " << FormatAggregate(values, dims) << "\n";
          Symbol symbol;
          symbol.kind = Symbol::Kind::GlobalVar;
          symbol.ir_name = "@" + def.name;
          symbol.asm_name = def.name;
          symbol.dimensions = dims;
          InsertSymbol(def.name, std::move(symbol));
        }
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
    const Param &param = function.params[i];
    out_ << "%param_" << i << ": ";
    if (param.is_array) {
      out_ << PointerParamType(EvalDimensions(param.dimensions));
    } else {
      out_ << "i32";
    }
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
  for (size_t i = 0; i < function.params.size(); ++i) {
    const Param &param = function.params[i];
    const std::string &alloc = param_alloc_names_.at(&param);
    out_ << "  store %param_" << i << ", " << alloc << "\n";
    Symbol symbol;
    symbol.kind = Symbol::Kind::Var;
    symbol.ir_name = alloc;
    symbol.dimensions = EvalDimensions(param.dimensions);
    symbol.is_pointer = param.is_array;
    InsertSymbol(param.name, std::move(symbol));
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
    const std::vector<int> dims = EvalDimensions(param.dimensions);
    const std::string ir_name = NewAllocName(param.name);
    param_alloc_names_.emplace(&param, ir_name);
    const std::string type = param.is_array ? PointerParamType(dims) : "i32";
    alloc_lines_.push_back("  " + ir_name + " = alloc " + type + "\n");
  }
  CollectAllocs(*function.block);
}

void KoopaGenerator::CollectAllocs(const Block &block) {
  PushScope();
  for (const BlockItem &item : block.items) {
    CollectAllocs(item);
  }
  PopScope();
}

void KoopaGenerator::CollectAllocs(const BlockItem &item) {
  switch (item.kind) {
    case BlockItem::Kind::VarDecl:
      for (const VarDef &def : item.var_defs) {
        const std::vector<int> dims = EvalDimensions(def.dimensions);
        const std::string ir_name = NewAllocName(def.name);
        var_alloc_names_.emplace(&def, ir_name);
        alloc_lines_.push_back("  " + ir_name + " = alloc " + ArrayTypeName(dims) + "\n");
        if (def.has_init) {
          if (dims.empty()) {
            CollectLogicTemps(*def.init.expr);
          } else {
            for (const Expr *expr : FlattenInitExprs(def.init, dims)) {
              if (expr != nullptr) {
                CollectLogicTemps(*expr);
              }
            }
          }
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
      for (const ConstDef &def : item.const_defs) {
        const std::vector<int> dims = EvalDimensions(def.dimensions);
        if (dims.empty()) {
          InsertSymbol(def.name,
                       ScalarConstSymbol(EvalConstExpr(*def.init.expr)));
        } else {
          const std::string ir_name = NewAllocName(def.name);
          alloc_lines_.push_back("  " + ir_name + " = alloc " + ArrayTypeName(dims) + "\n");
          var_alloc_names_.emplace(reinterpret_cast<const VarDef *>(&def), ir_name);
          for (const Expr *expr : FlattenInitExprs(def.init, dims)) {
            if (expr != nullptr) {
              CollectLogicTemps(*expr);
            }
          }
        }
      }
      break;
    case BlockItem::Kind::Assign:
    case BlockItem::Kind::Return:
    case BlockItem::Kind::ExprStmt:
      if (item.expr) {
        CollectLogicTemps(*item.expr);
      }
      if (item.kind == BlockItem::Kind::Assign && item.lval) {
        for (const auto &index : item.lval->indices) {
          CollectLogicTemps(*index);
        }
      }
      break;
    case BlockItem::Kind::Break:
    case BlockItem::Kind::Continue:
      break;
  }
}

void KoopaGenerator::CollectLogicTemps(const Expr &expr) {
  if (const auto *lval = dynamic_cast<const LValExpr *>(&expr)) {
    for (const auto &index : lval->indices) {
      CollectLogicTemps(*index);
    }
    return;
  }
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
        const std::vector<int> dims = EvalDimensions(def.dimensions);
        if (dims.empty()) {
          InsertSymbol(def.name,
                       ScalarConstSymbol(EvalConstExpr(*def.init.expr)));
        } else {
          const std::string &ir_name = var_alloc_names_.at(reinterpret_cast<const VarDef *>(&def));
          Symbol symbol;
          symbol.kind = Symbol::Kind::Const;
          symbol.ir_name = ir_name;
          symbol.dimensions = dims;
          InsertSymbol(def.name, symbol);
          GenerateLocalArrayInit(symbol, def.init);
        }
      }
      break;
    case BlockItem::Kind::VarDecl:
      for (const VarDef &def : item.var_defs) {
        const std::vector<int> dims = EvalDimensions(def.dimensions);
        const std::string &ir_name = var_alloc_names_.at(&def);
        Symbol symbol;
        symbol.kind = Symbol::Kind::Var;
        symbol.ir_name = ir_name;
        symbol.dimensions = dims;
        if (def.has_init) {
          if (dims.empty()) {
            const std::string value = GenerateExpr(*def.init.expr);
            out_ << "  store " << value << ", " << ir_name << "\n";
          } else {
            GenerateLocalArrayInit(symbol, def.init);
          }
        }
        InsertSymbol(def.name, std::move(symbol));
      }
      break;
    case BlockItem::Kind::Assign: {
      const Symbol &symbol = LookupSymbol(item.lval->name);
      if (symbol.kind == Symbol::Kind::Const) {
        throw std::runtime_error("cannot assign to constant: " + item.lval->name);
      }
      const std::string value = GenerateExpr(*item.expr);
      const KoopaAddrInfo addr = GenerateLValAddress(*item.lval);
      out_ << "  store " << value << ", " << addr.ptr << "\n";
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
    if (symbol.kind == Symbol::Kind::Const && symbol.dimensions.empty()) {
      return std::to_string(symbol.const_value);
    }
    const KoopaAddrInfo addr = GenerateLValAddress(*lval);
    const bool scalar = symbol.is_pointer
                            ? lval->indices.size() > symbol.dimensions.size()
                            : addr.remaining_dimensions.empty();
    if (scalar) {
      const std::string name = NewValueName();
      out_ << "  " << name << " = load " << addr.ptr << "\n";
      return name;
    }
    if (!symbol.is_pointer || addr.indices_used > 0) {
      const std::string name = NewValueName();
      out_ << "  " << name << " = getelemptr " << addr.ptr << ", 0\n";
      return name;
    }
    return addr.ptr;
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

KoopaAddrInfo KoopaGenerator::GenerateLValAddress(const LValExpr &lval) {
  const Symbol &symbol = LookupSymbol(lval.name);
  if (symbol.dimensions.empty() && !symbol.is_pointer) {
    return KoopaAddrInfo{symbol.ir_name, {}, false, 0};
  }

  std::string ptr = symbol.ir_name;
  if (symbol.is_pointer) {
    const std::string loaded = NewValueName();
    out_ << "  " << loaded << " = load " << ptr << "\n";
    ptr = loaded;
  }

  for (size_t i = 0; i < lval.indices.size(); ++i) {
    const std::string index = GenerateExpr(*lval.indices[i]);
    const std::string next = NewValueName();
    if (symbol.is_pointer && i == 0) {
      out_ << "  " << next << " = getptr " << ptr << ", " << index << "\n";
    } else {
      out_ << "  " << next << " = getelemptr " << ptr << ", " << index << "\n";
    }
    ptr = next;
  }

  std::vector<int> remaining;
  if (lval.indices.size() < symbol.dimensions.size()) {
    remaining.assign(symbol.dimensions.begin() + static_cast<long>(lval.indices.size()),
                     symbol.dimensions.end());
  }
  return KoopaAddrInfo{ptr, remaining, symbol.is_pointer,
                       static_cast<int>(lval.indices.size())};
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

std::vector<int> KoopaGenerator::EvalDimensions(
    const std::vector<std::unique_ptr<Expr>> &dimensions) const {
  std::vector<int> result;
  for (const auto &dimension : dimensions) {
    result.push_back(EvalConstExpr(*dimension));
  }
  return result;
}

void KoopaGenerator::GenerateLocalArrayInit(const Symbol &symbol, const InitVal &init) {
  const std::vector<const Expr *> exprs = FlattenInitExprs(init, symbol.dimensions);
  for (size_t i = 0; i < exprs.size(); ++i) {
    std::string ptr = symbol.ir_name;
    size_t linear = i;
    for (size_t dim = 0; dim < symbol.dimensions.size(); ++dim) {
      const int stride = Product(symbol.dimensions, dim + 1);
      const int index = static_cast<int>(linear / stride);
      linear %= stride;
      const std::string next = NewValueName();
      out_ << "  " << next << " = getelemptr " << ptr << ", " << index << "\n";
      ptr = next;
    }
    const std::string value = exprs[i] ? GenerateExpr(*exprs[i]) : "0";
    out_ << "  store " << value << ", " << ptr << "\n";
  }
}

int KoopaGenerator::EvalConstExpr(const Expr &expr) const {
  if (const auto *number = dynamic_cast<const NumberExpr *>(&expr)) {
    return number->value;
  }
  if (const auto *lval = dynamic_cast<const LValExpr *>(&expr)) {
    const Symbol &symbol = LookupSymbol(lval->name);
    if (symbol.kind != Symbol::Kind::Const || !symbol.dimensions.empty() || !lval->indices.empty()) {
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

// RISC-V backend.

RiscvGenerator::RiscvGenerator(std::ostream &out) : out_(out) {}

void RiscvGenerator::Generate(const Program &program) {
  RegisterLibraryFunctions();
  RegisterFunctions(program);
  scopes_.clear();
  PushScope();

  bool has_data = false;
  for (const GlobalItem &item : program.items) {
    if (item.kind == GlobalItem::Kind::ConstDecl || item.kind == GlobalItem::Kind::VarDecl) {
      bool emits_data = false;
      if (item.kind == GlobalItem::Kind::VarDecl) {
        emits_data = true;
      } else {
        for (const ConstDef &def : item.const_defs) {
          if (!def.dimensions.empty()) {
            emits_data = true;
          }
        }
      }
      if (!has_data && emits_data) {
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
        const std::vector<int> dims = EvalDimensions(def.dimensions);
        if (dims.empty()) {
          InsertSymbol(def.name,
                       ScalarConstSymbol(EvalConstExpr(*def.init.expr)));
        } else {
          const std::vector<const Expr *> exprs = FlattenInitExprs(def.init, dims);
          out_ << "  .globl " << ToAsmName(def.name) << "\n";
          out_ << ToAsmName(def.name) << ":\n";
          for (const Expr *expr : exprs) {
            out_ << "  .word " << (expr ? EvalConstExpr(*expr) : 0) << "\n";
          }
          Symbol symbol;
          symbol.kind = Symbol::Kind::Const;
          symbol.asm_name = ToAsmName(def.name);
          symbol.dimensions = dims;
          InsertSymbol(def.name, std::move(symbol));
        }
      }
      break;
    case GlobalItem::Kind::VarDecl:
      for (const VarDef &def : item.var_defs) {
        const std::vector<int> dims = EvalDimensions(def.dimensions);
        out_ << "  .globl " << ToAsmName(def.name) << "\n";
        out_ << ToAsmName(def.name) << ":\n";
        if (dims.empty()) {
          if (def.has_init) {
            out_ << "  .word " << EvalConstExpr(*def.init.expr) << "\n";
          } else {
            out_ << "  .zero 4\n";
          }
          InsertSymbol(def.name,
                       GlobalScalarSymbol(ToAsmName(def.name)));
        } else {
          if (def.has_init) {
            const std::vector<const Expr *> exprs = FlattenInitExprs(def.init, dims);
            for (const Expr *expr : exprs) {
              out_ << "  .word " << (expr ? EvalConstExpr(*expr) : 0) << "\n";
            }
          } else {
            out_ << "  .zero " << TypeSize(dims) << "\n";
          }
          Symbol symbol;
          symbol.kind = Symbol::Kind::GlobalVar;
          symbol.asm_name = ToAsmName(def.name);
          symbol.dimensions = dims;
          InsertSymbol(def.name, std::move(symbol));
        }
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
    const Param &param = function.params[i];
    Symbol symbol;
    symbol.kind = Symbol::Kind::Var;
    symbol.stack_offset = next_var_offset_;
    next_var_offset_ += 4;
    symbol.dimensions = EvalDimensions(param.dimensions);
    symbol.is_pointer = param.is_array;
    InsertSymbol(param.name, symbol);
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
    symbol.dimensions = EvalDimensions(param.dimensions);
    symbol.is_pointer = param.is_array;
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
        const std::vector<int> dims = EvalDimensions(def.dimensions);
        if (dims.empty()) {
          InsertSymbol(def.name,
                       ScalarConstSymbol(EvalConstExpr(*def.init.expr)));
        } else {
          Symbol symbol;
          symbol.kind = Symbol::Kind::Const;
          symbol.stack_offset = next_var_offset_;
          symbol.dimensions = dims;
          next_var_offset_ += TypeSize(dims);
          InsertSymbol(def.name, symbol);
          for (const Expr *expr : FlattenInitExprs(def.init, dims)) {
            if (expr != nullptr) {
              ScanExpr(*expr, 0);
            }
          }
        }
      }
      break;
    case BlockItem::Kind::VarDecl:
      for (const VarDef &def : item.var_defs) {
        const std::vector<int> dims = EvalDimensions(def.dimensions);
        if (def.has_init) {
          if (dims.empty()) {
            ScanExpr(*def.init.expr, 0);
          } else {
            for (const Expr *expr : FlattenInitExprs(def.init, dims)) {
              if (expr != nullptr) {
                ScanExpr(*expr, 0);
              }
            }
          }
        }
        Symbol symbol;
        symbol.kind = Symbol::Kind::Var;
        symbol.stack_offset = next_var_offset_;
        symbol.dimensions = dims;
        next_var_offset_ += dims.empty() ? 4 : TypeSize(dims);
        InsertSymbol(def.name, symbol);
      }
      break;
    case BlockItem::Kind::Assign:
      LookupSymbol(item.lval->name);
      for (const auto &index : item.lval->indices) {
        ScanExpr(*index, 0);
      }
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
    for (const auto &index : lval->indices) {
      ScanExpr(*index, depth);
    }
    max_temp_depth_ = std::max(max_temp_depth_, depth + 2);
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
    max_temp_depth_ = std::max(max_temp_depth_, depth + arg_count + 2);
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
        const std::vector<int> dims = EvalDimensions(def.dimensions);
        if (dims.empty()) {
          InsertSymbol(def.name,
                       ScalarConstSymbol(EvalConstExpr(*def.init.expr)));
        } else {
          Symbol symbol;
          symbol.kind = Symbol::Kind::Const;
          symbol.stack_offset = next_var_offset_;
          symbol.dimensions = dims;
          next_var_offset_ += TypeSize(dims);
          InsertSymbol(def.name, symbol);
          GenerateLocalArrayInit(symbol, def.init, 0);
        }
      }
      break;
    case BlockItem::Kind::VarDecl:
      for (const VarDef &def : item.var_defs) {
        const std::vector<int> dims = EvalDimensions(def.dimensions);
        Symbol symbol;
        symbol.kind = Symbol::Kind::Var;
        symbol.stack_offset = next_var_offset_;
        symbol.dimensions = dims;
        next_var_offset_ += dims.empty() ? 4 : TypeSize(dims);
        if (def.has_init) {
          if (dims.empty()) {
            GenerateExpr(*def.init.expr);
            StoreToSymbol(symbol, "t0");
          } else {
            GenerateLocalArrayInit(symbol, def.init, 0);
          }
        }
        InsertSymbol(def.name, symbol);
      }
      break;
    case BlockItem::Kind::Assign: {
      const Symbol &symbol = LookupSymbol(item.lval->name);
      if (symbol.kind == Symbol::Kind::Const) {
        throw std::runtime_error("cannot assign to constant: " + item.lval->name);
      }
      GenerateLValAddress(*item.lval);
      EmitStoreStack(out_, "t0", TempOffset(0));
      GenerateExpr(*item.expr, 1);
      EmitLoadStack(out_, "t1", TempOffset(0));
      StoreAddressed("t1", "t0");
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
    if (symbol.kind == Symbol::Kind::Const && symbol.dimensions.empty()) {
      out_ << "  li t0, " << symbol.const_value << "\n";
    } else {
      GenerateLValAddress(*lval, depth);
      const size_t used = lval->indices.size();
      const bool scalar = symbol.is_pointer ? used > symbol.dimensions.size()
                                           : used >= symbol.dimensions.size();
      if (scalar) {
        LoadAddressed("t0", "t0");
      } else if (!symbol.is_pointer || used > 0) {
        out_ << "  li t1, 0\n";
        const int stride = TypeSize(symbol.dimensions, used + 1);
        if (stride != 1) {
          out_ << "  li t2, " << stride << "\n";
          out_ << "  mul t1, t1, t2\n";
        }
        out_ << "  add t0, t0, t1\n";
      }
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
    case BinaryOp::Or:
      break;
  }
}

void RiscvGenerator::GenerateLValAddress(const LValExpr &lval, int depth) {
  const Symbol &symbol = LookupSymbol(lval.name);
  if (symbol.is_pointer) {
    LoadFromSymbol(symbol, "t0");
  } else {
    LoadSymbolAddress(symbol, "t0");
  }
  for (size_t i = 0; i < lval.indices.size(); ++i) {
    EmitStoreStack(out_, "t0", TempOffset(depth));
    GenerateExpr(*lval.indices[i], depth + 1);
    out_ << "  mv t1, t0\n";
    const size_t dim_index = symbol.is_pointer ? i : i + 1;
    const int stride = TypeSize(symbol.dimensions, dim_index);
    out_ << "  li t2, " << stride << "\n";
    out_ << "  mul t1, t1, t2\n";
    EmitLoadStack(out_, "t0", TempOffset(depth));
    out_ << "  add t0, t0, t1\n";
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

std::vector<int> RiscvGenerator::EvalDimensions(
    const std::vector<std::unique_ptr<Expr>> &dimensions) const {
  std::vector<int> result;
  for (const auto &dimension : dimensions) {
    result.push_back(EvalConstExpr(*dimension));
  }
  return result;
}

void RiscvGenerator::GenerateLocalArrayInit(const Symbol &symbol, const InitVal &init, int depth) {
  const std::vector<const Expr *> exprs = FlattenInitExprs(init, symbol.dimensions);
  for (size_t i = 0; i < exprs.size(); ++i) {
    Symbol elem = symbol;
    elem.dimensions.clear();
    elem.stack_offset = symbol.stack_offset + static_cast<int>(i) * 4;
    if (exprs[i] != nullptr) {
      GenerateExpr(*exprs[i], depth);
    } else {
      out_ << "  li t0, 0\n";
    }
    StoreToSymbol(elem, "t0");
  }
}

int RiscvGenerator::EvalConstExpr(const Expr &expr) const {
  if (const auto *number = dynamic_cast<const NumberExpr *>(&expr)) {
    return number->value;
  }
  if (const auto *lval = dynamic_cast<const LValExpr *>(&expr)) {
    const Symbol &symbol = LookupSymbol(lval->name);
    if (symbol.kind != Symbol::Kind::Const || !symbol.dimensions.empty() || !lval->indices.empty()) {
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
  if (symbol.kind == Symbol::Kind::GlobalVar || (!symbol.asm_name.empty())) {
    out_ << "  la t1, " << symbol.asm_name << "\n";
    out_ << "  sw " << reg << ", 0(t1)\n";
  } else {
    EmitStoreStack(out_, reg, symbol.stack_offset);
  }
}

void RiscvGenerator::LoadFromSymbol(const Symbol &symbol, const std::string &reg) {
  if (symbol.kind == Symbol::Kind::GlobalVar || (!symbol.asm_name.empty())) {
    out_ << "  la " << reg << ", " << symbol.asm_name << "\n";
    out_ << "  lw " << reg << ", 0(" << reg << ")\n";
  } else {
    EmitLoadStack(out_, reg, symbol.stack_offset);
  }
}

void RiscvGenerator::StoreAddressed(const std::string &addr_reg, const std::string &value_reg) {
  out_ << "  sw " << value_reg << ", 0(" << addr_reg << ")\n";
}

void RiscvGenerator::LoadAddressed(const std::string &addr_reg, const std::string &value_reg) {
  out_ << "  lw " << value_reg << ", 0(" << addr_reg << ")\n";
}

void RiscvGenerator::LoadSymbolAddress(const Symbol &symbol, const std::string &reg) {
  if (symbol.kind == Symbol::Kind::GlobalVar || (symbol.kind == Symbol::Kind::Const && !symbol.asm_name.empty())) {
    out_ << "  la " << reg << ", " << symbol.asm_name << "\n";
  } else {
    if (Fits12Bit(symbol.stack_offset)) {
      out_ << "  addi " << reg << ", sp, " << symbol.stack_offset << "\n";
    } else {
      out_ << "  li " << reg << ", " << symbol.stack_offset << "\n";
      out_ << "  add " << reg << ", sp, " << reg << "\n";
    }
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
  std::ostringstream buffer;
  RiscvGenerator generator(buffer);
  generator.Generate(program);

  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open output file: " + path);
  }
  out << OptimizeAsm(buffer.str());
}
