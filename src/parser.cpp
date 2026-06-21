#include "parser.h"

#include <stdexcept>
#include <utility>

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

std::unique_ptr<Expr> Parser::ParseCompUnit() {
  Expect(TokenKind::Int, "'int'");
  const Token ident = Expect(TokenKind::Ident, "function name");
  if (ident.text != "main") {
    throw std::runtime_error("Lv3 only supports function 'main'");
  }
  Expect(TokenKind::LParen, "'('");
  Expect(TokenKind::RParen, "')'");
  Expect(TokenKind::LBrace, "'{'");
  Expect(TokenKind::Return, "'return'");
  std::unique_ptr<Expr> return_value = ParseExp();
  Expect(TokenKind::Semicolon, "';'");
  Expect(TokenKind::RBrace, "'}'");
  Expect(TokenKind::End, "end of file");
  return return_value;
}

std::unique_ptr<Expr> Parser::ParseExp() { return ParseLOrExp(); }

std::unique_ptr<Expr> Parser::ParseLOrExp() {
  std::unique_ptr<Expr> expr = ParseLAndExp();
  while (Match(TokenKind::Or)) {
    expr = std::make_unique<BinaryExpr>(BinaryOp::Or, std::move(expr),
                                        ParseLAndExp());
  }
  return expr;
}

std::unique_ptr<Expr> Parser::ParseLAndExp() {
  std::unique_ptr<Expr> expr = ParseEqExp();
  while (Match(TokenKind::And)) {
    expr = std::make_unique<BinaryExpr>(BinaryOp::And, std::move(expr),
                                        ParseEqExp());
  }
  return expr;
}

std::unique_ptr<Expr> Parser::ParseEqExp() {
  std::unique_ptr<Expr> expr = ParseRelExp();
  while (true) {
    if (Match(TokenKind::Equal)) {
      expr = std::make_unique<BinaryExpr>(BinaryOp::Eq, std::move(expr),
                                          ParseRelExp());
    } else if (Match(TokenKind::NotEqual)) {
      expr = std::make_unique<BinaryExpr>(BinaryOp::Ne, std::move(expr),
                                          ParseRelExp());
    } else {
      return expr;
    }
  }
}

std::unique_ptr<Expr> Parser::ParseRelExp() {
  std::unique_ptr<Expr> expr = ParseAddExp();
  while (true) {
    if (Match(TokenKind::Less)) {
      expr = std::make_unique<BinaryExpr>(BinaryOp::Lt, std::move(expr),
                                          ParseAddExp());
    } else if (Match(TokenKind::Greater)) {
      expr = std::make_unique<BinaryExpr>(BinaryOp::Gt, std::move(expr),
                                          ParseAddExp());
    } else if (Match(TokenKind::LessEqual)) {
      expr = std::make_unique<BinaryExpr>(BinaryOp::Le, std::move(expr),
                                          ParseAddExp());
    } else if (Match(TokenKind::GreaterEqual)) {
      expr = std::make_unique<BinaryExpr>(BinaryOp::Ge, std::move(expr),
                                          ParseAddExp());
    } else {
      return expr;
    }
  }
}

std::unique_ptr<Expr> Parser::ParseAddExp() {
  std::unique_ptr<Expr> expr = ParseMulExp();
  while (true) {
    if (Match(TokenKind::Plus)) {
      expr = std::make_unique<BinaryExpr>(BinaryOp::Add, std::move(expr),
                                          ParseMulExp());
    } else if (Match(TokenKind::Minus)) {
      expr = std::make_unique<BinaryExpr>(BinaryOp::Sub, std::move(expr),
                                          ParseMulExp());
    } else {
      return expr;
    }
  }
}

std::unique_ptr<Expr> Parser::ParseMulExp() {
  std::unique_ptr<Expr> expr = ParseUnaryExp();
  while (true) {
    if (Match(TokenKind::Star)) {
      expr = std::make_unique<BinaryExpr>(BinaryOp::Mul, std::move(expr),
                                          ParseUnaryExp());
    } else if (Match(TokenKind::Slash)) {
      expr = std::make_unique<BinaryExpr>(BinaryOp::Div, std::move(expr),
                                          ParseUnaryExp());
    } else if (Match(TokenKind::Percent)) {
      expr = std::make_unique<BinaryExpr>(BinaryOp::Mod, std::move(expr),
                                          ParseUnaryExp());
    } else {
      return expr;
    }
  }
}

std::unique_ptr<Expr> Parser::ParseUnaryExp() {
  if (Match(TokenKind::Plus)) {
    return std::make_unique<UnaryExpr>(UnaryOp::Plus, ParseUnaryExp());
  }
  if (Match(TokenKind::Minus)) {
    return std::make_unique<UnaryExpr>(UnaryOp::Minus, ParseUnaryExp());
  }
  if (Match(TokenKind::Not)) {
    return std::make_unique<UnaryExpr>(UnaryOp::Not, ParseUnaryExp());
  }
  return ParsePrimaryExp();
}

std::unique_ptr<Expr> Parser::ParsePrimaryExp() {
  if (Match(TokenKind::LParen)) {
    std::unique_ptr<Expr> expr = ParseExp();
    Expect(TokenKind::RParen, "')'");
    return expr;
  }
  return std::make_unique<NumberExpr>(
      Expect(TokenKind::Number, "integer constant or '('").value);
}

const Token &Parser::Peek() const {
  if (pos_ >= tokens_.size()) {
    throw std::runtime_error("unexpected end of token stream");
  }
  return tokens_[pos_];
}

Token Parser::Expect(TokenKind kind, const std::string &expected) {
  const Token token = Peek();
  if (token.kind != kind) {
    throw std::runtime_error("expected " + expected + ", got '" + token.text +
                             "'");
  }
  ++pos_;
  return token;
}

bool Parser::Match(TokenKind kind) {
  if (Peek().kind != kind) {
    return false;
  }
  ++pos_;
  return true;
}
