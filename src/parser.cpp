#include "parser.h"

#include <stdexcept>
#include <utility>

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

std::unique_ptr<Program> Parser::ParseCompUnit() {
  Expect(TokenKind::Int, "'int'");
  const Token ident = Expect(TokenKind::Ident, "function name");
  if (ident.text != "main") {
    throw std::runtime_error("Lv5 only supports function 'main'");
  }
  Expect(TokenKind::LParen, "'('");
  Expect(TokenKind::RParen, "')'");

  auto program = std::make_unique<Program>();
  program->block = ParseBlock();
  Expect(TokenKind::End, "end of file");
  return program;
}

std::unique_ptr<Block> Parser::ParseBlock() {
  Expect(TokenKind::LBrace, "'{'");
  auto block = std::make_unique<Block>();
  while (Peek().kind != TokenKind::RBrace) {
    block->items.push_back(ParseBlockItem());
  }
  Expect(TokenKind::RBrace, "'}'");
  return block;
}

BlockItem Parser::ParseBlockItem() {
  if (Peek().kind == TokenKind::Const) {
    return ParseConstDecl();
  }
  if (Peek().kind == TokenKind::Int) {
    return ParseVarDecl();
  }
  return ParseStmt();
}

BlockItem Parser::ParseConstDecl() {
  BlockItem item;
  item.kind = BlockItem::Kind::ConstDecl;
  Expect(TokenKind::Const, "'const'");
  Expect(TokenKind::Int, "'int'");
  while (true) {
    ConstDef def;
    def.name = Expect(TokenKind::Ident, "constant name").text;
    Expect(TokenKind::Assign, "'='");
    def.init = ParseExp();
    item.const_defs.push_back(std::move(def));
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  Expect(TokenKind::Semicolon, "';'");
  return item;
}

BlockItem Parser::ParseVarDecl() {
  BlockItem item;
  item.kind = BlockItem::Kind::VarDecl;
  Expect(TokenKind::Int, "'int'");
  while (true) {
    VarDef def;
    def.name = Expect(TokenKind::Ident, "variable name").text;
    if (Match(TokenKind::Assign)) {
      def.init = ParseExp();
    }
    item.var_defs.push_back(std::move(def));
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  Expect(TokenKind::Semicolon, "';'");
  return item;
}

BlockItem Parser::ParseStmt() {
  BlockItem item;
  if (Match(TokenKind::Return)) {
    item.kind = BlockItem::Kind::Return;
    item.expr = ParseExp();
    Expect(TokenKind::Semicolon, "';'");
    return item;
  }
  if (Peek().kind == TokenKind::LBrace) {
    item.kind = BlockItem::Kind::Block;
    item.block = ParseBlock();
    return item;
  }
  if (Match(TokenKind::Semicolon)) {
    item.kind = BlockItem::Kind::ExprStmt;
    return item;
  }
  if (Peek().kind == TokenKind::Ident && Peek(1).kind == TokenKind::Assign) {
    item.kind = BlockItem::Kind::Assign;
    item.lval = Expect(TokenKind::Ident, "assignment target").text;
    Expect(TokenKind::Assign, "'='");
    item.expr = ParseExp();
    Expect(TokenKind::Semicolon, "';'");
    return item;
  }

  item.kind = BlockItem::Kind::ExprStmt;
  item.expr = ParseExp();
  Expect(TokenKind::Semicolon, "';'");
  return item;
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
  if (Peek().kind == TokenKind::Ident) {
    return std::make_unique<LValExpr>(Expect(TokenKind::Ident, "identifier").text);
  }
  return std::make_unique<NumberExpr>(
      Expect(TokenKind::Number, "integer constant, identifier or '('").value);
}

const Token &Parser::Peek(size_t offset) const {
  if (pos_ + offset >= tokens_.size()) {
    throw std::runtime_error("unexpected end of token stream");
  }
  return tokens_[pos_ + offset];
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
