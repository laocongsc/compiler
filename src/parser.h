#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast.h"
#include "token.h"

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens);

  std::unique_ptr<Program> ParseCompUnit();

 private:
  TypeKind ParseFuncType();
  GlobalItem ParseGlobalItem();
  GlobalItem ParseTopLevelDeclOrFunc();
  std::unique_ptr<FunctionDef> ParseFuncDef(TypeKind return_type,
                                            std::string name);
  std::vector<Param> ParseFuncFParams();
  Param ParseFuncFParam();
  std::vector<std::unique_ptr<Expr>> ParseFuncRParams();
  std::unique_ptr<Block> ParseBlock();
  BlockItem ParseBlockItem();
  BlockItem ParseConstDecl();
  BlockItem ParseVarDecl(bool type_consumed = false,
                         std::string first_name = "",
                         bool is_secret = false,
                         SourceLocation first_loc = SourceLocation{});
  BlockItem ParseStmt();
  std::vector<std::unique_ptr<Expr>> ParseDimensions();
  std::unique_ptr<LValExpr> ParseLVal();
  InitVal ParseInitVal();

  std::unique_ptr<Expr> ParseExp();
  std::unique_ptr<Expr> ParseLOrExp();
  std::unique_ptr<Expr> ParseLAndExp();
  std::unique_ptr<Expr> ParseEqExp();
  std::unique_ptr<Expr> ParseRelExp();
  std::unique_ptr<Expr> ParseAddExp();
  std::unique_ptr<Expr> ParseMulExp();
  std::unique_ptr<Expr> ParseUnaryExp();
  std::unique_ptr<Expr> ParsePrimaryExp();

  const Token &Peek(size_t offset = 0) const;
  Token Expect(TokenKind kind, const std::string &expected);
  bool Match(TokenKind kind);

  std::vector<Token> tokens_;
  size_t pos_ = 0;
};
