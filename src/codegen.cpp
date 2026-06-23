#include "codegen.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <numeric>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "koopa.h"

namespace {

bool Fits12Bit(int value) { return value >= -2048 && value <= 2047; }

std::string LocKey(SourceLocation loc) {
  return std::to_string(loc.line) + ":" + std::to_string(loc.column);
}

const BlockItem *SingleInnerItem(const BlockItem &item) {
  if (item.kind == BlockItem::Kind::Block) {
    if (item.block == nullptr || item.block->items.size() != 1) {
      return nullptr;
    }
    return &item.block->items.front();
  }
  return &item;
}

const BlockItem *SingleAssignItem(const BlockItem &item) {
  const BlockItem *inner = SingleInnerItem(item);
  if (inner == nullptr || inner->kind != BlockItem::Kind::Assign ||
      inner->lval == nullptr || inner->expr == nullptr) {
    return nullptr;
  }
  return inner;
}

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


std::string StripPrefix(const char *name) {
  if (name == nullptr) {
    return "";
  }
  std::string result(name);
  if (!result.empty() && (result[0] == '@' || result[0] == '%')) {
    result.erase(result.begin());
  }
  return result;
}

int Product(const std::vector<int> &dims, size_t start = 0) {
  int result = 1;
  for (size_t i = start; i < dims.size(); ++i) {
    result *= dims[i];
  }
  return result;
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

bool IsUnconditionalTerminator(const std::string &line) {
  const std::string trimmed = TrimLeft(line);
  return trimmed == "ret" || trimmed.rfind("j ", 0) == 0;
}

bool IsSectionOrGlobalDirective(const std::string &line) {
  const std::string trimmed = TrimLeft(line);
  return trimmed == ".text" || trimmed == ".data" || trimmed.rfind(".globl", 0) == 0;
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
  bool unreachable = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (unreachable) {
      if (IsLabelLine(lines[i]) || IsSectionOrGlobalDirective(lines[i])) {
        unreachable = false;
      } else {
        continue;
      }
    }

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
    if (IsUnconditionalTerminator(lines[i])) {
      unreachable = true;
    }
  }

  std::ostringstream out;
  for (const std::string &optimized_line : optimized) {
    out << optimized_line << '\n';
  }
  return out.str();
}

}  // namespace

KoopaGenerator::KoopaGenerator(std::ostream &out) : out_(out) {}

KoopaGenerator::KoopaGenerator(
    std::ostream &out, std::unordered_set<std::string> rewrite_if_locs,
    std::unordered_set<std::string> rewrite_array_lookup_locs,
    std::unordered_set<std::string> rewrite_logic_expr_locs)
    : out_(out),
      rewrite_if_locs_(std::move(rewrite_if_locs)),
      rewrite_array_lookup_locs_(std::move(rewrite_array_lookup_locs)),
      rewrite_logic_expr_locs_(std::move(rewrite_logic_expr_locs)) {}

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
  if (TryGenerateRewriteIf(item)) {
    return;
  }
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

bool KoopaGenerator::TryGenerateRewriteIf(const BlockItem &item) {
  if (rewrite_if_locs_.count(LocKey(item.loc)) == 0 || item.else_stmt == nullptr) {
    return false;
  }
  const BlockItem *then_assign = SingleAssignItem(*item.then_stmt);
  const BlockItem *else_assign = SingleAssignItem(*item.else_stmt);
  if (then_assign == nullptr || else_assign == nullptr) {
    return false;
  }

  const Symbol &symbol = LookupSymbol(then_assign->lval->name);
  if (symbol.kind == Symbol::Kind::Const || !then_assign->lval->indices.empty() ||
      !else_assign->lval->indices.empty() ||
      then_assign->lval->name != else_assign->lval->name) {
    return false;
  }

  const std::string cond_value = GenerateExpr(*item.expr);
  const std::string cond_bool = EmitBinary("ne", cond_value, "0");
  const std::string mask = EmitBinary("sub", "0", cond_bool);
  const std::string not_mask = EmitBinary("xor", mask, "-1");
  const std::string then_value = GenerateExpr(*then_assign->expr);
  const std::string else_value = GenerateExpr(*else_assign->expr);
  const std::string masked_then = EmitBinary("and", then_value, mask);
  const std::string masked_else = EmitBinary("and", else_value, not_mask);
  const std::string selected = EmitBinary("or", masked_then, masked_else);
  const KoopaAddrInfo addr = GenerateLValAddress(*then_assign->lval);
  out_ << "  store " << selected << ", " << addr.ptr << "\n";
  entry_terminated_ = false;
  return true;
}


std::string KoopaGenerator::TryGenerateRewriteArrayLookup(const LValExpr &lval) {
  if (rewrite_array_lookup_locs_.count(LocKey(lval.loc)) == 0) {
    return "";
  }
  const Symbol &symbol = LookupSymbol(lval.name);
  if (symbol.is_pointer || symbol.dimensions.size() != 1 || lval.indices.size() != 1) {
    return "";
  }
  const int length = symbol.dimensions.front();
  const std::string index = GenerateExpr(*lval.indices.front());
  std::string result = "0";
  for (int i = 0; i < length; ++i) {
    const std::string ptr = NewValueName();
    out_ << "  " << ptr << " = getelemptr " << symbol.ir_name << ", " << i << "\n";
    const std::string value = NewValueName();
    out_ << "  " << value << " = load " << ptr << "\n";
    const std::string match = EmitBinary("eq", index, std::to_string(i));
    const std::string mask = EmitBinary("sub", "0", match);
    const std::string not_mask = EmitBinary("xor", mask, "-1");
    const std::string taken = EmitBinary("and", value, mask);
    const std::string kept = EmitBinary("and", result, not_mask);
    result = EmitBinary("or", taken, kept);
  }
  return result;
}

std::string KoopaGenerator::TryGenerateRewriteLogicExpr(const BinaryExpr &expr) {
  if (rewrite_logic_expr_locs_.count(LocKey(expr.loc)) == 0) {
    return "";
  }
  if (expr.op != BinaryOp::And && expr.op != BinaryOp::Or) {
    return "";
  }
  const std::string lhs = GenerateExpr(*expr.lhs);
  const std::string rhs = GenerateExpr(*expr.rhs);
  const std::string lhs_bool = EmitBinary("ne", lhs, "0");
  const std::string rhs_bool = EmitBinary("ne", rhs, "0");
  return EmitBinary(expr.op == BinaryOp::And ? "and" : "or", lhs_bool, rhs_bool);
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
    if (std::string rewritten = TryGenerateRewriteArrayLookup(*lval); !rewritten.empty()) {
      return rewritten;
    }
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

  if (std::string rewritten = TryGenerateRewriteLogicExpr(*binary); !rewritten.empty()) {
    return rewritten;
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


class KoopaRawRiscvGenerator {
 public:
  explicit KoopaRawRiscvGenerator(std::ostream &out) : out_(out) {}

  void Generate(const koopa_raw_program_t &program) {
    if (program.values.len > 0) {
      out_ << "  .data\n";
      for (uint32_t i = 0; i < program.values.len; ++i) {
        GenerateGlobal(reinterpret_cast<koopa_raw_value_t>(program.values.buffer[i]));
      }
    }

    out_ << "  .text\n";
    for (uint32_t i = 0; i < program.funcs.len; ++i) {
      const auto func = reinterpret_cast<koopa_raw_function_t>(program.funcs.buffer[i]);
      if (func->bbs.len != 0) {
        GenerateFunction(func);
      }
    }
  }

 private:
  int TypeSize(koopa_raw_type_t type) const {
    switch (type->tag) {
      case KOOPA_RTT_UNIT:
        return 0;
      case KOOPA_RTT_INT32:
        return 4;
      case KOOPA_RTT_POINTER:
        return 4;
      case KOOPA_RTT_ARRAY:
        return static_cast<int>(type->data.array.len) * TypeSize(type->data.array.base);
      case KOOPA_RTT_FUNCTION:
        return 0;
    }
    throw std::runtime_error("unknown Koopa type");
  }

  int PointeeSize(koopa_raw_value_t ptr) const {
    if (ptr->ty->tag != KOOPA_RTT_POINTER) {
      throw std::runtime_error("Koopa value is not a pointer");
    }
    return TypeSize(ptr->ty->data.pointer.base);
  }

  int ElemPtrStride(koopa_raw_value_t src) const {
    if (src->ty->tag != KOOPA_RTT_POINTER) {
      throw std::runtime_error("getelemptr source is not a pointer");
    }
    koopa_raw_type_t base = src->ty->data.pointer.base;
    if (base->tag == KOOPA_RTT_ARRAY) {
      return TypeSize(base->data.array.base);
    }
    return TypeSize(base);
  }

  bool HasRuntimeValue(koopa_raw_value_t value) const {
    return value->ty->tag != KOOPA_RTT_UNIT &&
           value->kind.tag != KOOPA_RVT_INTEGER &&
           value->kind.tag != KOOPA_RVT_ZERO_INIT &&
           value->kind.tag != KOOPA_RVT_UNDEF &&
           value->kind.tag != KOOPA_RVT_AGGREGATE &&
           value->kind.tag != KOOPA_RVT_GLOBAL_ALLOC;
  }

  bool IsPureInst(koopa_raw_value_t value) const {
    switch (value->kind.tag) {
      case KOOPA_RVT_LOAD:
      case KOOPA_RVT_GET_PTR:
      case KOOPA_RVT_GET_ELEM_PTR:
      case KOOPA_RVT_BINARY:
        return true;
      default:
        return false;
    }
  }

  bool IsDeadPureInst(koopa_raw_value_t value) const {
    return IsPureInst(value) && live_values_.count(value) == 0;
  }

  void MarkValueLive(koopa_raw_value_t value) {
    if (value == nullptr || live_values_.count(value) != 0) {
      return;
    }
    live_values_.insert(value);
    switch (value->kind.tag) {
      case KOOPA_RVT_LOAD:
        MarkValueLive(value->kind.data.load.src);
        break;
      case KOOPA_RVT_STORE:
        MarkValueLive(value->kind.data.store.value);
        MarkValueLive(value->kind.data.store.dest);
        break;
      case KOOPA_RVT_GET_PTR:
        MarkValueLive(value->kind.data.get_ptr.src);
        MarkValueLive(value->kind.data.get_ptr.index);
        break;
      case KOOPA_RVT_GET_ELEM_PTR:
        MarkValueLive(value->kind.data.get_elem_ptr.src);
        MarkValueLive(value->kind.data.get_elem_ptr.index);
        break;
      case KOOPA_RVT_BINARY:
        MarkValueLive(value->kind.data.binary.lhs);
        MarkValueLive(value->kind.data.binary.rhs);
        break;
      case KOOPA_RVT_BRANCH:
        MarkValueLive(value->kind.data.branch.cond);
        break;
      case KOOPA_RVT_CALL:
        for (uint32_t i = 0; i < value->kind.data.call.args.len; ++i) {
          MarkValueLive(reinterpret_cast<koopa_raw_value_t>(value->kind.data.call.args.buffer[i]));
        }
        break;
      case KOOPA_RVT_RETURN:
        MarkValueLive(value->kind.data.ret.value);
        break;
      default:
        break;
    }
  }

  void BuildLiveValues(koopa_raw_function_t func) {
    live_values_.clear();
    value_blocks_.clear();
    for (uint32_t i = 0; i < func->bbs.len; ++i) {
      const auto bb = reinterpret_cast<koopa_raw_basic_block_t>(func->bbs.buffer[i]);
      for (uint32_t j = 0; j < bb->insts.len; ++j) {
        const auto inst = reinterpret_cast<koopa_raw_value_t>(bb->insts.buffer[j]);
        value_blocks_[inst] = bb;
      }
    }
    for (uint32_t i = 0; i < func->bbs.len; ++i) {
      const auto bb = reinterpret_cast<koopa_raw_basic_block_t>(func->bbs.buffer[i]);
      for (uint32_t j = 0; j < bb->insts.len; ++j) {
        const auto inst = reinterpret_cast<koopa_raw_value_t>(bb->insts.buffer[j]);
        switch (inst->kind.tag) {
          case KOOPA_RVT_STORE:
          case KOOPA_RVT_BRANCH:
          case KOOPA_RVT_JUMP:
          case KOOPA_RVT_CALL:
          case KOOPA_RVT_RETURN:
            MarkValueLive(inst);
            break;
          default:
            break;
        }
      }
    }
  }

  bool IsPromotableScalarAlloc(koopa_raw_value_t value) const {
    if (value->kind.tag != KOOPA_RVT_ALLOC || value->ty->tag != KOOPA_RTT_POINTER ||
        value->ty->data.pointer.base->tag != KOOPA_RTT_INT32) {
      return false;
    }
    for (uint32_t i = 0; i < value->used_by.len; ++i) {
      const auto user = reinterpret_cast<koopa_raw_value_t>(value->used_by.buffer[i]);
      if (user->kind.tag == KOOPA_RVT_LOAD && user->kind.data.load.src == value) {
        continue;
      }
      if (user->kind.tag == KOOPA_RVT_STORE && user->kind.data.store.dest == value) {
        continue;
      }
      return false;
    }
    return true;
  }

  bool IsCacheableRuntimeValue(koopa_raw_value_t value) const {
    return HasRuntimeValue(value) && value->kind.tag != KOOPA_RVT_ALLOC &&
           value->kind.tag != KOOPA_RVT_GLOBAL_ALLOC;
  }

  static const std::vector<std::string> &LinearScanRegs() {
    static const std::vector<std::string> regs = {"t3", "t4", "t5", "t6"};
    return regs;
  }

  int RemainingUses(koopa_raw_value_t value) const {
    const auto found = remaining_uses_.find(value);
    return found == remaining_uses_.end() ? 0 : found->second;
  }

  void CountValueUse(koopa_raw_value_t value) {
    if (IsCacheableRuntimeValue(value)) {
      ++remaining_uses_[value];
    }
  }

  void CountInstUses(koopa_raw_value_t inst) {
    switch (inst->kind.tag) {
      case KOOPA_RVT_LOAD:
        CountValueUse(inst->kind.data.load.src);
        break;
      case KOOPA_RVT_STORE:
        CountValueUse(inst->kind.data.store.value);
        CountValueUse(inst->kind.data.store.dest);
        break;
      case KOOPA_RVT_GET_PTR:
        CountValueUse(inst->kind.data.get_ptr.src);
        CountValueUse(inst->kind.data.get_ptr.index);
        break;
      case KOOPA_RVT_GET_ELEM_PTR:
        CountValueUse(inst->kind.data.get_elem_ptr.src);
        CountValueUse(inst->kind.data.get_elem_ptr.index);
        break;
      case KOOPA_RVT_BINARY:
        CountValueUse(inst->kind.data.binary.lhs);
        CountValueUse(inst->kind.data.binary.rhs);
        break;
      case KOOPA_RVT_BRANCH:
        CountValueUse(inst->kind.data.branch.cond);
        break;
      case KOOPA_RVT_CALL:
        for (uint32_t i = 0; i < inst->kind.data.call.args.len; ++i) {
          CountValueUse(reinterpret_cast<koopa_raw_value_t>(inst->kind.data.call.args.buffer[i]));
        }
        break;
      case KOOPA_RVT_RETURN:
        if (inst->kind.data.ret.value != nullptr) {
          CountValueUse(inst->kind.data.ret.value);
        }
        break;
      default:
        break;
    }
  }

  template <typename Fn>
  void ForEachOperand(koopa_raw_value_t inst, Fn fn) const {
    switch (inst->kind.tag) {
      case KOOPA_RVT_LOAD:
        fn(inst->kind.data.load.src);
        break;
      case KOOPA_RVT_STORE:
        fn(inst->kind.data.store.value);
        fn(inst->kind.data.store.dest);
        break;
      case KOOPA_RVT_GET_PTR:
        fn(inst->kind.data.get_ptr.src);
        fn(inst->kind.data.get_ptr.index);
        break;
      case KOOPA_RVT_GET_ELEM_PTR:
        fn(inst->kind.data.get_elem_ptr.src);
        fn(inst->kind.data.get_elem_ptr.index);
        break;
      case KOOPA_RVT_BINARY:
        fn(inst->kind.data.binary.lhs);
        fn(inst->kind.data.binary.rhs);
        break;
      case KOOPA_RVT_BRANCH:
        fn(inst->kind.data.branch.cond);
        break;
      case KOOPA_RVT_CALL:
        for (uint32_t i = 0; i < inst->kind.data.call.args.len; ++i) {
          fn(reinterpret_cast<koopa_raw_value_t>(inst->kind.data.call.args.buffer[i]));
        }
        break;
      case KOOPA_RVT_RETURN:
        if (inst->kind.data.ret.value != nullptr) {
          fn(inst->kind.data.ret.value);
        }
        break;
      default:
        break;
    }
  }

  bool AllAllocUsesInBlock(koopa_raw_value_t alloc, koopa_raw_basic_block_t bb) const {
    for (uint32_t i = 0; i < alloc->used_by.len; ++i) {
      const auto user = reinterpret_cast<koopa_raw_value_t>(alloc->used_by.buffer[i]);
      const auto found = value_blocks_.find(user);
      if (found == value_blocks_.end() || found->second != bb) {
        return false;
      }
    }
    return true;
  }

  bool AllUsesInBlock(koopa_raw_value_t value, koopa_raw_basic_block_t bb) const {
    for (uint32_t i = 0; i < value->used_by.len; ++i) {
      const auto user = reinterpret_cast<koopa_raw_value_t>(value->used_by.buffer[i]);
      const auto found = value_blocks_.find(user);
      if (found == value_blocks_.end() || found->second != bb) {
        return false;
      }
    }
    return true;
  }

  bool HasStoreValueUse(koopa_raw_value_t value) const {
    for (uint32_t i = 0; i < value->used_by.len; ++i) {
      const auto user = reinterpret_cast<koopa_raw_value_t>(value->used_by.buffer[i]);
      if (user->kind.tag == KOOPA_RVT_STORE && user->kind.data.store.value == value) {
        return true;
      }
    }
    return false;
  }

  bool IntervalCrossesCall(int start, int last, const std::vector<int> &call_positions) const {
    for (int call_pos : call_positions) {
      if (start < call_pos && call_pos < last) {
        return true;
      }
    }
    return false;
  }

  void BuildBlockRegisterPlan(koopa_raw_basic_block_t bb) {
    allocated_regs_.clear();
    reg_only_values_.clear();

    struct Interval {
      koopa_raw_value_t value = nullptr;
      int start = 0;
      int last = 0;
      std::string reg;
    };

    std::unordered_map<koopa_raw_value_t, int> starts;
    std::unordered_map<koopa_raw_value_t, int> lasts;
    std::unordered_map<koopa_raw_value_t, int> uses;
    std::vector<int> call_positions;

    for (uint32_t i = 0; i < bb->insts.len; ++i) {
      const auto inst = reinterpret_cast<koopa_raw_value_t>(bb->insts.buffer[i]);
      if (IsDeadPureInst(inst)) {
        continue;
      }
      const int pos = static_cast<int>(i);
      if (inst->kind.tag == KOOPA_RVT_CALL) {
        call_positions.push_back(pos);
      }
      if (IsCacheableRuntimeValue(inst)) {
        starts.emplace(inst, pos);
      }
      ForEachOperand(inst, [&](koopa_raw_value_t value) {
        if (!IsCacheableRuntimeValue(value)) {
          return;
        }
        const auto block = value_blocks_.find(value);
        if (block == value_blocks_.end() || block->second != bb) {
          return;
        }
        lasts[value] = pos;
        ++uses[value];
      });
    }

    std::vector<Interval> intervals;
    for (const auto &entry : starts) {
      koopa_raw_value_t value = entry.first;
      const auto use_count = uses.find(value);
      const auto last = lasts.find(value);
      if (use_count == uses.end() || use_count->second == 0 || last == lasts.end()) {
        continue;
      }
      const int start = entry.second;
      if (IntervalCrossesCall(start, last->second, call_positions)) {
        continue;
      }
      intervals.push_back(Interval{value, start, last->second, ""});
    }

    std::sort(intervals.begin(), intervals.end(), [](const Interval &lhs, const Interval &rhs) {
      if (lhs.start != rhs.start) {
        return lhs.start < rhs.start;
      }
      return lhs.last < rhs.last;
    });

    std::vector<Interval *> active;
    std::vector<std::string> free_regs = LinearScanRegs();
    for (Interval &interval : intervals) {
      for (auto it = active.begin(); it != active.end();) {
        if ((*it)->last < interval.start) {
          free_regs.push_back((*it)->reg);
          it = active.erase(it);
        } else {
          ++it;
        }
      }

      if (!free_regs.empty()) {
        interval.reg = free_regs.back();
        free_regs.pop_back();
      } else {
        auto spill = std::max_element(active.begin(), active.end(),
                                      [](const Interval *lhs, const Interval *rhs) {
                                        return lhs->last < rhs->last;
                                      });
        if (spill != active.end() && (*spill)->last > interval.last) {
          interval.reg = (*spill)->reg;
          allocated_regs_.erase((*spill)->value);
          reg_only_values_.erase((*spill)->value);
          active.erase(spill);
        }
      }

      if (interval.reg.empty()) {
        continue;
      }
      allocated_regs_[interval.value] = interval.reg;
      if (AllUsesInBlock(interval.value, bb) && !HasStoreValueUse(interval.value)) {
        reg_only_values_.insert(interval.value);
      }
      active.push_back(&interval);
      std::sort(active.begin(), active.end(), [](const Interval *lhs, const Interval *rhs) {
        return lhs->last < rhs->last;
      });
    }
  }

  void PrepareBlock(koopa_raw_basic_block_t bb) {
    ClearValueCache();
    BuildBlockRegisterPlan(bb);
    remaining_uses_.clear();
    alloc_current_values_.clear();
    local_promotable_allocs_.clear();
    for (koopa_raw_value_t alloc : promotable_allocs_) {
      if (AllAllocUsesInBlock(alloc, bb)) {
        local_promotable_allocs_.insert(alloc);
      }
    }
    for (uint32_t i = 0; i < bb->insts.len; ++i) {
      const auto inst = reinterpret_cast<koopa_raw_value_t>(bb->insts.buffer[i]);
      if (!IsDeadPureInst(inst)) {
        CountInstUses(inst);
      }
    }
  }

  const std::string *FindCachedValue(koopa_raw_value_t value) const {
    const auto found = value_regs_.find(value);
    if (found == value_regs_.end()) {
      return nullptr;
    }
    return &found->second;
  }

  void ClearValueCache() {
    value_regs_.clear();
  }

  void ForgetRegister(const std::string &reg) {
    for (auto it = value_regs_.begin(); it != value_regs_.end();) {
      if (it->second == reg) {
        it = value_regs_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void CacheValue(koopa_raw_value_t value, const std::string &src_reg) {
    const auto assigned = allocated_regs_.find(value);
    if (assigned == allocated_regs_.end()) {
      return;
    }
    const std::string &reg = assigned->second;
    ForgetRegister(reg);
    value_regs_[value] = reg;
    if (reg != src_reg) {
      out_ << "  mv " << reg << ", " << src_reg << "\n";
    }
  }

  int StackOffset(koopa_raw_value_t value) const {
    const auto found = value_offsets_.find(value);
    if (found == value_offsets_.end()) {
      throw std::runtime_error("missing stack slot for Koopa value");
    }
    return out_arg_bytes_ + found->second;
  }

  void AssignSlot(koopa_raw_value_t value, int bytes) {
    if (bytes <= 0) {
      return;
    }
    value_offsets_.emplace(value, local_bytes_);
    local_bytes_ += bytes;
  }

  std::string LabelName(koopa_raw_basic_block_t bb) const {
    return ".L" + current_function_ + "_" + StripPrefix(bb->name);
  }

  void EmitLi(const std::string &reg, int value) {
    out_ << "  li " << reg << ", " << value << "\n";
  }

  void EmitStackAddr(const std::string &reg, int offset) {
    if (Fits12Bit(offset)) {
      out_ << "  addi " << reg << ", sp, " << offset << "\n";
      return;
    }
    out_ << "  li " << reg << ", " << offset << "\n";
    out_ << "  add " << reg << ", sp, " << reg << "\n";
  }

  void EmitStackAdjust(int bytes) {
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

  void EmitLoadValue(koopa_raw_value_t value, const std::string &reg) {
    switch (value->kind.tag) {
      case KOOPA_RVT_INTEGER:
        EmitLi(reg, value->kind.data.integer.value);
        return;
      case KOOPA_RVT_GLOBAL_ALLOC:
        out_ << "  la " << reg << ", " << StripPrefix(value->name) << "\n";
        return;
      case KOOPA_RVT_ALLOC:
        EmitStackAddr(reg, StackOffset(value));
        return;
      default:
        if (const std::string *cached = FindCachedValue(value)) {
          if (*cached != reg) {
            out_ << "  mv " << reg << ", " << *cached << "\n";
          }
          return;
        }
        EmitLoadStack(out_, reg, StackOffset(value));
        return;
    }
  }

  void EmitStoreValue(koopa_raw_value_t value, const std::string &reg) {
    if (!HasRuntimeValue(value)) {
      return;
    }
    CacheValue(value, reg);
    if (reg_only_values_.count(value) == 0) {
      EmitStoreStack(out_, reg, StackOffset(value));
    }
  }

  void EmitReturn() {
    if (needs_ra_) {
      EmitLoadStack(out_, "ra", frame_size_ - 4);
    }
    EmitStackAdjust(frame_size_);
    out_ << "  ret\n";
  }

  void ScanFunction(koopa_raw_function_t func) {
    BuildLiveValues(func);
    value_offsets_.clear();
    promotable_allocs_.clear();
    ClearValueCache();
    local_bytes_ = 0;
    out_arg_bytes_ = 0;
    needs_ra_ = false;
    frame_size_ = 0;

    for (uint32_t i = 0; i < func->params.len; ++i) {
      AssignSlot(reinterpret_cast<koopa_raw_value_t>(func->params.buffer[i]), 4);
    }

    for (uint32_t i = 0; i < func->bbs.len; ++i) {
      const auto bb = reinterpret_cast<koopa_raw_basic_block_t>(func->bbs.buffer[i]);
      for (uint32_t j = 0; j < bb->insts.len; ++j) {
        const auto inst = reinterpret_cast<koopa_raw_value_t>(bb->insts.buffer[j]);
        if (IsDeadPureInst(inst)) {
          continue;
        }
        if (inst->kind.tag == KOOPA_RVT_ALLOC) {
          AssignSlot(inst, PointeeSize(inst));
          if (IsPromotableScalarAlloc(inst)) {
            promotable_allocs_.insert(inst);
          }
        } else if (HasRuntimeValue(inst)) {
          AssignSlot(inst, 4);
        }
        if (inst->kind.tag == KOOPA_RVT_CALL) {
          needs_ra_ = true;
          const int argc = static_cast<int>(inst->kind.data.call.args.len);
          if (argc > 8) {
            out_arg_bytes_ = std::max(out_arg_bytes_, (argc - 8) * 4);
          }
        }
      }
    }
    frame_size_ = AlignTo16(out_arg_bytes_ + local_bytes_ + (needs_ra_ ? 4 : 0));
  }

  void GenerateFunction(koopa_raw_function_t func) {
    current_function_ = StripPrefix(func->name);
    ScanFunction(func);

    out_ << "  .globl " << current_function_ << "\n";
    out_ << current_function_ << ":\n";
    EmitStackAdjust(-frame_size_);
    if (needs_ra_) {
      EmitStoreStack(out_, "ra", frame_size_ - 4);
    }

    for (uint32_t i = 0; i < func->params.len; ++i) {
      const auto param = reinterpret_cast<koopa_raw_value_t>(func->params.buffer[i]);
      if (i < 8) {
        EmitStoreStack(out_, "a" + std::to_string(i), StackOffset(param));
      } else {
        EmitLoadStack(out_, "t0", frame_size_ + static_cast<int>(i - 8) * 4);
        EmitStoreStack(out_, "t0", StackOffset(param));
      }
    }

    for (uint32_t i = 0; i < func->bbs.len; ++i) {
      const auto bb = reinterpret_cast<koopa_raw_basic_block_t>(func->bbs.buffer[i]);
      PrepareBlock(bb);
      out_ << LabelName(bb) << ":\n";
      for (uint32_t j = 0; j < bb->insts.len; ++j) {
        GenerateInst(reinterpret_cast<koopa_raw_value_t>(bb->insts.buffer[j]));
      }
    }
  }

  void GenerateInst(koopa_raw_value_t inst) {
    if (IsDeadPureInst(inst)) {
      return;
    }
    switch (inst->kind.tag) {
      case KOOPA_RVT_ALLOC:
        break;
      case KOOPA_RVT_LOAD:
        GenerateLoad(inst);
        break;
      case KOOPA_RVT_STORE:
        GenerateStore(inst);
        break;
      case KOOPA_RVT_GET_PTR:
        GenerateGetPtr(inst);
        break;
      case KOOPA_RVT_GET_ELEM_PTR:
        GenerateGetElemPtr(inst);
        break;
      case KOOPA_RVT_BINARY:
        GenerateBinary(inst);
        break;
      case KOOPA_RVT_BRANCH:
        GenerateBranch(inst);
        break;
      case KOOPA_RVT_JUMP:
        out_ << "  j " << LabelName(inst->kind.data.jump.target) << "\n";
        break;
      case KOOPA_RVT_CALL:
        GenerateCall(inst);
        break;
      case KOOPA_RVT_RETURN:
        if (inst->kind.data.ret.value != nullptr) {
          EmitLoadValue(inst->kind.data.ret.value, "a0");
        }
        EmitReturn();
        break;
      default:
        throw std::runtime_error("unsupported Koopa instruction in RISC-V backend");
    }
  }

  void GenerateLoad(koopa_raw_value_t inst) {
    const koopa_raw_value_t src = inst->kind.data.load.src;
    const auto forwarded = alloc_current_values_.find(src);
    if (forwarded != alloc_current_values_.end()) {
      EmitLoadValue(forwarded->second, "t0");
      EmitStoreValue(inst, "t0");
      return;
    }
    EmitLoadValue(src, "t0");
    out_ << "  lw t0, 0(t0)\n";
    EmitStoreValue(inst, "t0");
  }

  void GenerateStore(koopa_raw_value_t inst) {
    const koopa_raw_value_t value = inst->kind.data.store.value;
    const koopa_raw_value_t dest = inst->kind.data.store.dest;
    EmitLoadValue(value, "t0");
    if (local_promotable_allocs_.count(dest) != 0) {
      alloc_current_values_[dest] = value;
      return;
    }
    EmitLoadValue(dest, "t1");
    out_ << "  sw t0, 0(t1)\n";
  }

  void GenerateGetPtr(koopa_raw_value_t inst) {
    const auto &ptr = inst->kind.data.get_ptr;
    EmitLoadValue(ptr.src, "t0");
    EmitLoadValue(ptr.index, "t1");
    const int stride = PointeeSize(ptr.src);
    if (stride != 1) {
      EmitLi("t2", stride);
      out_ << "  mul t1, t1, t2\n";
    }
    out_ << "  add t0, t0, t1\n";
    EmitStoreValue(inst, "t0");
  }

  void GenerateGetElemPtr(koopa_raw_value_t inst) {
    const auto &ptr = inst->kind.data.get_elem_ptr;
    EmitLoadValue(ptr.src, "t0");
    EmitLoadValue(ptr.index, "t1");
    const int stride = ElemPtrStride(ptr.src);
    if (stride != 1) {
      EmitLi("t2", stride);
      out_ << "  mul t1, t1, t2\n";
    }
    out_ << "  add t0, t0, t1\n";
    EmitStoreValue(inst, "t0");
  }

  void GenerateBinary(koopa_raw_value_t inst) {
    const auto &binary = inst->kind.data.binary;
    EmitLoadValue(binary.lhs, "t0");
    EmitLoadValue(binary.rhs, "t1");
    switch (binary.op) {
      case KOOPA_RBO_ADD:
        out_ << "  add t0, t0, t1\n";
        break;
      case KOOPA_RBO_SUB:
        out_ << "  sub t0, t0, t1\n";
        break;
      case KOOPA_RBO_MUL:
        out_ << "  mul t0, t0, t1\n";
        break;
      case KOOPA_RBO_DIV:
        out_ << "  div t0, t0, t1\n";
        break;
      case KOOPA_RBO_MOD:
        out_ << "  rem t0, t0, t1\n";
        break;
      case KOOPA_RBO_AND:
        out_ << "  and t0, t0, t1\n";
        break;
      case KOOPA_RBO_OR:
        out_ << "  or t0, t0, t1\n";
        break;
      case KOOPA_RBO_XOR:
        out_ << "  xor t0, t0, t1\n";
        break;
      case KOOPA_RBO_SHL:
        out_ << "  sll t0, t0, t1\n";
        break;
      case KOOPA_RBO_SHR:
        out_ << "  srl t0, t0, t1\n";
        break;
      case KOOPA_RBO_SAR:
        out_ << "  sra t0, t0, t1\n";
        break;
      case KOOPA_RBO_EQ:
        out_ << "  xor t0, t0, t1\n";
        out_ << "  seqz t0, t0\n";
        break;
      case KOOPA_RBO_NOT_EQ:
        out_ << "  xor t0, t0, t1\n";
        out_ << "  snez t0, t0\n";
        break;
      case KOOPA_RBO_LT:
        out_ << "  slt t0, t0, t1\n";
        break;
      case KOOPA_RBO_GT:
        out_ << "  sgt t0, t0, t1\n";
        break;
      case KOOPA_RBO_LE:
        out_ << "  sgt t0, t0, t1\n";
        out_ << "  seqz t0, t0\n";
        break;
      case KOOPA_RBO_GE:
        out_ << "  slt t0, t0, t1\n";
        out_ << "  seqz t0, t0\n";
        break;
    }
    EmitStoreValue(inst, "t0");
  }

  void GenerateBranch(koopa_raw_value_t inst) {
    const auto &branch = inst->kind.data.branch;
    EmitLoadValue(branch.cond, "t0");
    out_ << "  bnez t0, " << LabelName(branch.true_bb) << "\n";
    out_ << "  j " << LabelName(branch.false_bb) << "\n";
  }

  void GenerateCall(koopa_raw_value_t inst) {
    const auto &call = inst->kind.data.call;
    for (uint32_t i = 0; i < call.args.len; ++i) {
      const auto arg = reinterpret_cast<koopa_raw_value_t>(call.args.buffer[i]);
      EmitLoadValue(arg, "t0");
      if (i < 8) {
        out_ << "  mv a" << i << ", t0\n";
      } else {
        EmitStoreStack(out_, "t0", static_cast<int>(i - 8) * 4);
      }
    }
    ClearValueCache();
    out_ << "  call " << StripPrefix(call.callee->name) << "\n";
    if (inst->ty->tag != KOOPA_RTT_UNIT) {
      EmitStoreValue(inst, "a0");
    }
  }

  void GenerateGlobal(koopa_raw_value_t value) {
    if (value->kind.tag != KOOPA_RVT_GLOBAL_ALLOC) {
      return;
    }
    out_ << "  .globl " << StripPrefix(value->name) << "\n";
    out_ << StripPrefix(value->name) << ":\n";
    GenerateGlobalInit(value->kind.data.global_alloc.init, PointeeSize(value));
  }

  void GenerateGlobalInit(koopa_raw_value_t init, int bytes) {
    switch (init->kind.tag) {
      case KOOPA_RVT_INTEGER:
        out_ << "  .word " << init->kind.data.integer.value << "\n";
        break;
      case KOOPA_RVT_ZERO_INIT:
      case KOOPA_RVT_UNDEF:
        out_ << "  .zero " << bytes << "\n";
        break;
      case KOOPA_RVT_AGGREGATE:
        for (uint32_t i = 0; i < init->kind.data.aggregate.elems.len; ++i) {
          const auto elem = reinterpret_cast<koopa_raw_value_t>(init->kind.data.aggregate.elems.buffer[i]);
          GenerateGlobalInit(elem, TypeSize(elem->ty));
        }
        break;
      default:
        throw std::runtime_error("unsupported global initializer");
    }
  }

  static int AlignTo16(int bytes) { return (bytes + 15) / 16 * 16; }

  std::ostream &out_;
  std::unordered_map<koopa_raw_value_t, int> value_offsets_;
  std::unordered_map<koopa_raw_value_t, int> remaining_uses_;
  std::unordered_map<koopa_raw_value_t, std::string> allocated_regs_;
  std::unordered_map<koopa_raw_value_t, std::string> value_regs_;
  std::unordered_map<koopa_raw_value_t, koopa_raw_basic_block_t> value_blocks_;
  std::unordered_set<koopa_raw_value_t> reg_only_values_;
  std::unordered_set<koopa_raw_value_t> live_values_;
  std::unordered_set<koopa_raw_value_t> promotable_allocs_;
  std::unordered_set<koopa_raw_value_t> local_promotable_allocs_;
  std::unordered_map<koopa_raw_value_t, koopa_raw_value_t> alloc_current_values_;
  std::string current_function_;
  int local_bytes_ = 0;
  int out_arg_bytes_ = 0;
  int frame_size_ = 0;
  bool needs_ra_ = false;
};

class KoopaProgramHandle {
 public:
  explicit KoopaProgramHandle(const std::string &text) {
    if (koopa_parse_from_string(text.c_str(), &program_) != KOOPA_EC_SUCCESS) {
      throw std::runtime_error("failed to parse generated Koopa IR");
    }
    builder_ = koopa_new_raw_program_builder();
    raw_ = koopa_build_raw_program(builder_, program_);
  }

  ~KoopaProgramHandle() {
    if (builder_ != nullptr) {
      koopa_delete_raw_program_builder(builder_);
    }
    if (program_ != nullptr) {
      koopa_delete_program(program_);
    }
  }

  const koopa_raw_program_t &raw() const { return raw_; }

 private:
  koopa_program_t program_ = nullptr;
  koopa_raw_program_builder_t builder_ = nullptr;
  koopa_raw_program_t raw_{};
};

void WriteRiscvFromKoopaText(const std::string &path, const std::string &koopa_text) {
  KoopaProgramHandle program(koopa_text);
  std::ostringstream buffer;
  KoopaRawRiscvGenerator generator(buffer);
  generator.Generate(program.raw());

  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open output file: " + path);
  }
  out << OptimizeAsm(buffer.str());
}


void WriteKoopa(const std::string &path, const Program &program) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open output file: " + path);
  }
  KoopaGenerator generator(out);
  generator.Generate(program);
}

void WriteKoopaRewrite(const std::string &path, const Program &program,
                       std::unordered_set<std::string> rewrite_if_locs,
                       std::unordered_set<std::string> rewrite_array_lookup_locs,
                       std::unordered_set<std::string> rewrite_logic_expr_locs) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open output file: " + path);
  }
  KoopaGenerator generator(out, std::move(rewrite_if_locs),
                           std::move(rewrite_array_lookup_locs),
                           std::move(rewrite_logic_expr_locs));
  generator.Generate(program);
}

void WriteRiscv(const std::string &path, const Program &program) {
  std::ostringstream koopa;
  KoopaGenerator generator(koopa);
  generator.Generate(program);
  WriteRiscvFromKoopaText(path, koopa.str());
}

void WriteRiscvRewrite(const std::string &path, const Program &program,
                       std::unordered_set<std::string> rewrite_if_locs,
                       std::unordered_set<std::string> rewrite_array_lookup_locs,
                       std::unordered_set<std::string> rewrite_logic_expr_locs) {
  std::ostringstream koopa;
  KoopaGenerator generator(koopa, std::move(rewrite_if_locs),
                           std::move(rewrite_array_lookup_locs),
                           std::move(rewrite_logic_expr_locs));
  generator.Generate(program);
  WriteRiscvFromKoopaText(path, koopa.str());
}
