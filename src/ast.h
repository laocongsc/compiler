#pragma once

#include <memory>
#include <string>
#include <vector>

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

struct LValExpr final : Expr {
  explicit LValExpr(std::string name) : name(std::move(name)) {}
  std::string name;
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

struct ConstDef {
  std::string name;
  std::unique_ptr<Expr> init;
};

struct VarDef {
  std::string name;
  std::unique_ptr<Expr> init;
};

struct Block;

struct BlockItem {
  enum class Kind {
    ConstDecl,
    VarDecl,
    Assign,
    Return,
    ExprStmt,
    Block,
    If,
    While,
    Break,
    Continue,
  } kind;

  std::vector<ConstDef> const_defs;
  std::vector<VarDef> var_defs;
  std::string lval;
  std::unique_ptr<Expr> expr;
  std::unique_ptr<Block> block;
  std::unique_ptr<BlockItem> then_stmt;
  std::unique_ptr<BlockItem> else_stmt;
  std::unique_ptr<BlockItem> body_stmt;
};

struct Block {
  std::vector<BlockItem> items;
};

struct Program {
  std::unique_ptr<Block> block;
};
