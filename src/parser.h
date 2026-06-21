#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast.h"
#include "token.h"

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens);

  std::unique_ptr<Expr> ParseCompUnit();

 private:
  std::unique_ptr<Expr> ParseExp();
  std::unique_ptr<Expr> ParseLOrExp();
  std::unique_ptr<Expr> ParseLAndExp();
  std::unique_ptr<Expr> ParseEqExp();
  std::unique_ptr<Expr> ParseRelExp();
  std::unique_ptr<Expr> ParseAddExp();
  std::unique_ptr<Expr> ParseMulExp();
  std::unique_ptr<Expr> ParseUnaryExp();
  std::unique_ptr<Expr> ParsePrimaryExp();

  const Token &Peek() const;
  Token Expect(TokenKind kind, const std::string &expected);
  bool Match(TokenKind kind);

  std::vector<Token> tokens_;
  size_t pos_ = 0;
};
