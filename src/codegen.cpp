#include "codegen.h"

#include <fstream>
#include <ostream>
#include <stdexcept>

KoopaGenerator::KoopaGenerator(std::ostream &out) : out_(out) {}

std::string KoopaGenerator::Generate(const Expr &expr) {
  if (const auto *number = dynamic_cast<const NumberExpr *>(&expr)) {
    return std::to_string(number->value);
  }

  if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
    const std::string operand = Generate(*unary->operand);
    switch (unary->op) {
      case UnaryOp::Plus:
        return operand;
      case UnaryOp::Minus:
        return Emit("sub", "0", operand);
      case UnaryOp::Not:
        return Emit("eq", operand, "0");
    }
  }

  const auto *binary = dynamic_cast<const BinaryExpr *>(&expr);
  if (binary == nullptr) {
    throw std::runtime_error("unknown expression node");
  }

  const std::string lhs = Generate(*binary->lhs);
  const std::string rhs = Generate(*binary->rhs);
  switch (binary->op) {
    case BinaryOp::Add:
      return Emit("add", lhs, rhs);
    case BinaryOp::Sub:
      return Emit("sub", lhs, rhs);
    case BinaryOp::Mul:
      return Emit("mul", lhs, rhs);
    case BinaryOp::Div:
      return Emit("div", lhs, rhs);
    case BinaryOp::Mod:
      return Emit("mod", lhs, rhs);
    case BinaryOp::Lt:
      return Emit("lt", lhs, rhs);
    case BinaryOp::Gt:
      return Emit("gt", lhs, rhs);
    case BinaryOp::Le:
      return Emit("le", lhs, rhs);
    case BinaryOp::Ge:
      return Emit("ge", lhs, rhs);
    case BinaryOp::Eq:
      return Emit("eq", lhs, rhs);
    case BinaryOp::Ne:
      return Emit("ne", lhs, rhs);
    case BinaryOp::And: {
      const std::string lhs_bool = Emit("ne", lhs, "0");
      const std::string rhs_bool = Emit("ne", rhs, "0");
      return Emit("and", lhs_bool, rhs_bool);
    }
    case BinaryOp::Or: {
      const std::string lhs_bool = Emit("ne", lhs, "0");
      const std::string rhs_bool = Emit("ne", rhs, "0");
      return Emit("or", lhs_bool, rhs_bool);
    }
  }
  throw std::runtime_error("unknown binary operator");
}

std::string KoopaGenerator::Emit(const std::string &op, const std::string &lhs,
                                 const std::string &rhs) {
  const std::string name = "%" + std::to_string(next_id_++);
  out_ << "  " << name << " = " << op << " " << lhs << ", " << rhs << "\n";
  return name;
}

RiscvGenerator::RiscvGenerator(std::ostream &out) : out_(out) {}

void RiscvGenerator::Generate(const Expr &expr) {
  GenerateExpr(expr);
  out_ << "  mv a0, t0\n";
}

void RiscvGenerator::GenerateExpr(const Expr &expr) {
  if (const auto *number = dynamic_cast<const NumberExpr *>(&expr)) {
    out_ << "  li t0, " << number->value << "\n";
    return;
  }

  if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
    GenerateExpr(*unary->operand);
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

  GenerateExpr(*binary->lhs);
  PushT0();
  GenerateExpr(*binary->rhs);
  PopT1();

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

void RiscvGenerator::PushT0() {
  out_ << "  addi sp, sp, -4\n";
  out_ << "  sw t0, 0(sp)\n";
}

void RiscvGenerator::PopT1() {
  out_ << "  lw t1, 0(sp)\n";
  out_ << "  addi sp, sp, 4\n";
}

void WriteKoopa(const std::string &path, const Expr &return_value) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open output file: " + path);
  }
  out << "fun @main(): i32 {\n";
  out << "%entry:\n";
  KoopaGenerator generator(out);
  const std::string result = generator.Generate(return_value);
  out << "  ret " << result << "\n";
  out << "}\n";
}

void WriteRiscv(const std::string &path, const Expr &return_value) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open output file: " + path);
  }
  out << "  .text\n";
  out << "  .globl main\n";
  out << "main:\n";
  RiscvGenerator generator(out);
  generator.Generate(return_value);
  out << "  ret\n";
}
