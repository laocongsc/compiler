#pragma once

#include <memory>

enum class UnaryOp {
  Plus,
  Minus,
  Not,
};

enum class BinaryOp {
  Add,
  Sub,
  Mul,
  Div,
  Mod,
  Lt,
  Gt,
  Le,
  Ge,
  Eq,
  Ne,
  And,
  Or,
};

struct Expr {
  virtual ~Expr() = default;
};

struct NumberExpr final : Expr {
  explicit NumberExpr(int value) : value(value) {}
  int value;
};

struct UnaryExpr final : Expr {
  UnaryExpr(UnaryOp op, std::unique_ptr<Expr> operand)
      : op(op), operand(std::move(operand)) {}
  UnaryOp op;
  std::unique_ptr<Expr> operand;
};

struct BinaryExpr final : Expr {
  BinaryExpr(BinaryOp op, std::unique_ptr<Expr> lhs, std::unique_ptr<Expr> rhs)
      : op(op), lhs(std::move(lhs)), rhs(std::move(rhs)) {}
  BinaryOp op;
  std::unique_ptr<Expr> lhs;
  std::unique_ptr<Expr> rhs;
};
