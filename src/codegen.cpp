#include "codegen.h"

#include <fstream>
#include <ostream>
#include <stdexcept>

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
  for (const BlockItem &item : program.items) {
    if (entry_terminated_) {
      break;
    }
    GenerateItem(item);
  }
  out_ << "}\n";
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
        InsertSymbol(def.name, Symbol{Symbol::Kind::Var, 0, ir_name, 0});
        if (def.init) {
          const std::string value = GenerateExpr(*def.init);
          out_ << "  store " << value << ", " << ir_name << "\n";
        }
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
    case BlockItem::Kind::Return:
      const std::string value = GenerateExpr(*item.expr);
      out_ << "  ret " << value << "\n";
      entry_terminated_ = true;
      break;
  }
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

void KoopaGenerator::InsertSymbol(const std::string &name, Symbol symbol) {
  if (!symbols_.emplace(name, std::move(symbol)).second) {
    throw std::runtime_error("redefined symbol: " + name);
  }
}

Symbol &KoopaGenerator::LookupSymbol(const std::string &name) {
  auto it = symbols_.find(name);
  if (it == symbols_.end()) {
    throw std::runtime_error("undefined symbol: " + name);
  }
  return it->second;
}

const Symbol &KoopaGenerator::LookupSymbol(const std::string &name) const {
  auto it = symbols_.find(name);
  if (it == symbols_.end()) {
    throw std::runtime_error("undefined symbol: " + name);
  }
  return it->second;
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

RiscvGenerator::RiscvGenerator(std::ostream &out) : out_(out) {}

void RiscvGenerator::Generate(const Program &program) {
  ScanProgram(program);

  out_ << "  .text\n";
  out_ << "  .globl main\n";
  out_ << "main:\n";
  EmitStackAdjust(-frame_size_);
  for (const BlockItem &item : program.items) {
    GenerateItem(item);
  }
}

void RiscvGenerator::ScanProgram(const Program &program) {
  for (const BlockItem &item : program.items) {
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
      case BlockItem::Kind::Assign:
        LookupSymbol(item.lval);
        ScanExpr(*item.expr, 0);
        break;
      case BlockItem::Kind::Return:
        ScanExpr(*item.expr, 0);
        break;
    }
  }
  frame_size_ = AlignTo16(next_var_offset_ + max_temp_depth_ * 4);
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

void RiscvGenerator::GenerateItem(const BlockItem &item) {
  switch (item.kind) {
    case BlockItem::Kind::ConstDecl:
      break;
    case BlockItem::Kind::VarDecl:
      for (const VarDef &def : item.var_defs) {
        if (def.init) {
          GenerateExpr(*def.init);
          EmitStoreStack(out_, "t0", LookupSymbol(def.name).stack_offset);
        }
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
      break;
  }
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

void RiscvGenerator::InsertSymbol(const std::string &name, Symbol symbol) {
  if (!symbols_.emplace(name, std::move(symbol)).second) {
    throw std::runtime_error("redefined symbol: " + name);
  }
}

Symbol &RiscvGenerator::LookupSymbol(const std::string &name) {
  auto it = symbols_.find(name);
  if (it == symbols_.end()) {
    throw std::runtime_error("undefined symbol: " + name);
  }
  return it->second;
}

const Symbol &RiscvGenerator::LookupSymbol(const std::string &name) const {
  auto it = symbols_.find(name);
  if (it == symbols_.end()) {
    throw std::runtime_error("undefined symbol: " + name);
  }
  return it->second;
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

int RiscvGenerator::AlignTo16(int bytes) {
  return (bytes + 15) / 16 * 16;
}

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
