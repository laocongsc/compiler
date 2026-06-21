#pragma once

#include <memory>
#include <string>
#include <vector>

enum class TypeKind { Int, Void };

enum class UnaryOp { Plus, Minus, Not };

enum class BinaryOp {
  Add, Sub, Mul, Div, Mod,
  Lt, Gt, Le, Ge, Eq, Ne,
  And, Or,
};

struct Expr { virtual ~Expr() = default; };

struct NumberExpr final : Expr { explicit NumberExpr(int value) : value(value) {} int value; };
struct LValExpr final : Expr {
  explicit LValExpr(std::string name) : name(std::move(name)) {}
  LValExpr(std::string name, std::vector<std::unique_ptr<Expr>> indices)
      : name(std::move(name)), indices(std::move(indices)) {}
  std::string name;
  std::vector<std::unique_ptr<Expr>> indices;
};
struct CallExpr final : Expr {
  CallExpr(std::string name, std::vector<std::unique_ptr<Expr>> args)
      : name(std::move(name)), args(std::move(args)) {}
  std::string name;
  std::vector<std::unique_ptr<Expr>> args;
};
struct UnaryExpr final : Expr {
  UnaryExpr(UnaryOp op, std::unique_ptr<Expr> operand) : op(op), operand(std::move(operand)) {}
  UnaryOp op; std::unique_ptr<Expr> operand;
};
struct BinaryExpr final : Expr {
  BinaryExpr(BinaryOp op, std::unique_ptr<Expr> lhs, std::unique_ptr<Expr> rhs)
      : op(op), lhs(std::move(lhs)), rhs(std::move(rhs)) {}
  BinaryOp op; std::unique_ptr<Expr> lhs; std::unique_ptr<Expr> rhs;
};

struct InitVal {
  bool is_list = false;
  std::unique_ptr<Expr> expr;
  std::vector<InitVal> list;
};

struct ConstDef {
  std::string name;
  std::vector<std::unique_ptr<Expr>> dimensions;
  InitVal init;
};
struct VarDef {
  std::string name;
  std::vector<std::unique_ptr<Expr>> dimensions;
  bool has_init = false;
  InitVal init;
};
struct Param {
  std::string name;
  bool is_array = false;
  std::vector<std::unique_ptr<Expr>> dimensions;
};

struct Block;
struct BlockItem {
  enum class Kind { ConstDecl, VarDecl, Assign, Return, ExprStmt, Block, If, While, Break, Continue } kind;
  std::vector<ConstDef> const_defs;
  std::vector<VarDef> var_defs;
  std::unique_ptr<LValExpr> lval;
  std::unique_ptr<Expr> expr;
  std::unique_ptr<Block> block;
  std::unique_ptr<BlockItem> then_stmt;
  std::unique_ptr<BlockItem> else_stmt;
  std::unique_ptr<BlockItem> body_stmt;
};
struct Block { std::vector<BlockItem> items; };

struct FunctionDef {
  TypeKind return_type = TypeKind::Int;
  std::string name;
  std::vector<Param> params;
  std::unique_ptr<Block> block;
};

struct GlobalItem {
  enum class Kind { ConstDecl, VarDecl, FuncDef } kind;
  std::vector<ConstDef> const_defs;
  std::vector<VarDef> var_defs;
  std::unique_ptr<FunctionDef> function;
};

struct Program { std::vector<GlobalItem> items; };
