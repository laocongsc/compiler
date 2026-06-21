#include "parser.h"

#include <stdexcept>
#include <utility>

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

std::unique_ptr<Program> Parser::ParseCompUnit() {
  auto program = std::make_unique<Program>();
  while (Peek().kind != TokenKind::End) {
    program->items.push_back(ParseGlobalItem());
  }
  Expect(TokenKind::End, "end of file");
  return program;
}

TypeKind Parser::ParseFuncType() {
  if (Match(TokenKind::Int)) {
    return TypeKind::Int;
  }
  if (Match(TokenKind::Void)) {
    return TypeKind::Void;
  }
  throw std::runtime_error("expected function type, got '" + Peek().text + "'");
}

GlobalItem Parser::ParseGlobalItem() {
  if (Peek().kind == TokenKind::Const) {
    BlockItem decl = ParseConstDecl();
    GlobalItem item;
    item.kind = GlobalItem::Kind::ConstDecl;
    item.const_defs = std::move(decl.const_defs);
    return item;
  }
  return ParseTopLevelDeclOrFunc();
}

GlobalItem Parser::ParseTopLevelDeclOrFunc() {
  const TypeKind type = ParseFuncType();
  const std::string name = Expect(TokenKind::Ident, "identifier").text;
  if (Match(TokenKind::LParen)) {
    GlobalItem item;
    item.kind = GlobalItem::Kind::FuncDef;
    item.function = ParseFuncDef(type, name);
    return item;
  }
  if (type != TypeKind::Int) {
    throw std::runtime_error("global variable must have int type");
  }
  BlockItem decl = ParseVarDecl(true, name);
  GlobalItem item;
  item.kind = GlobalItem::Kind::VarDecl;
  item.var_defs = std::move(decl.var_defs);
  return item;
}

std::unique_ptr<FunctionDef> Parser::ParseFuncDef(TypeKind return_type,
                                                  std::string name) {
  auto function = std::make_unique<FunctionDef>();
  function->return_type = return_type;
  function->name = std::move(name);
  if (Peek().kind != TokenKind::RParen) {
    function->params = ParseFuncFParams();
  }
  Expect(TokenKind::RParen, "')'");
  function->block = ParseBlock();
  return function;
}

std::vector<Param> Parser::ParseFuncFParams() {
  std::vector<Param> params;
  while (true) {
    params.push_back(ParseFuncFParam());
    if (!Match(TokenKind::Comma)) {
      return params;
    }
  }
}

Param Parser::ParseFuncFParam() {
  Expect(TokenKind::Int, "'int'");
  Param param;
  param.name = Expect(TokenKind::Ident, "parameter name").text;
  if (Match(TokenKind::LBracket)) {
    param.is_array = true;
    Expect(TokenKind::RBracket, "']'");
    while (Match(TokenKind::LBracket)) {
      param.dimensions.push_back(ParseExp());
      Expect(TokenKind::RBracket, "']'");
    }
  }
  return param;
}

std::vector<std::unique_ptr<Expr>> Parser::ParseFuncRParams() {
  std::vector<std::unique_ptr<Expr>> args;
  while (true) {
    args.push_back(ParseExp());
    if (!Match(TokenKind::Comma)) {
      return args;
    }
  }
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
    def.dimensions = ParseDimensions();
    Expect(TokenKind::Assign, "'='");
    def.init = ParseInitVal();
    item.const_defs.push_back(std::move(def));
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  Expect(TokenKind::Semicolon, "';'");
  return item;
}

BlockItem Parser::ParseVarDecl(bool type_consumed, std::string first_name) {
  BlockItem item;
  item.kind = BlockItem::Kind::VarDecl;
  if (!type_consumed) {
    Expect(TokenKind::Int, "'int'");
  }
  bool first = true;
  while (true) {
    VarDef def;
    if (first && !first_name.empty()) {
      def.name = std::move(first_name);
    } else {
      def.name = Expect(TokenKind::Ident, "variable name").text;
    }
    first = false;
    def.dimensions = ParseDimensions();
    if (Match(TokenKind::Assign)) {
      def.has_init = true;
      def.init = ParseInitVal();
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
    if (Peek().kind != TokenKind::Semicolon) {
      item.expr = ParseExp();
    }
    Expect(TokenKind::Semicolon, "';'");
    return item;
  }
  if (Peek().kind == TokenKind::LBrace) {
    item.kind = BlockItem::Kind::Block;
    item.block = ParseBlock();
    return item;
  }
  if (Match(TokenKind::If)) {
    item.kind = BlockItem::Kind::If;
    Expect(TokenKind::LParen, "'('");
    item.expr = ParseExp();
    Expect(TokenKind::RParen, "')'");
    item.then_stmt = std::make_unique<BlockItem>(ParseStmt());
    if (Match(TokenKind::Else)) {
      item.else_stmt = std::make_unique<BlockItem>(ParseStmt());
    }
    return item;
  }
  if (Match(TokenKind::While)) {
    item.kind = BlockItem::Kind::While;
    Expect(TokenKind::LParen, "'('");
    item.expr = ParseExp();
    Expect(TokenKind::RParen, "')'");
    item.body_stmt = std::make_unique<BlockItem>(ParseStmt());
    return item;
  }
  if (Match(TokenKind::Break)) {
    item.kind = BlockItem::Kind::Break;
    Expect(TokenKind::Semicolon, "';'");
    return item;
  }
  if (Match(TokenKind::Continue)) {
    item.kind = BlockItem::Kind::Continue;
    Expect(TokenKind::Semicolon, "';'");
    return item;
  }
  if (Match(TokenKind::Semicolon)) {
    item.kind = BlockItem::Kind::ExprStmt;
    return item;
  }
  if (Peek().kind == TokenKind::Ident) {
    const size_t saved_pos = pos_;
    std::unique_ptr<LValExpr> lval = ParseLVal();
    if (Match(TokenKind::Assign)) {
      item.kind = BlockItem::Kind::Assign;
      item.lval = std::move(lval);
      item.expr = ParseExp();
      Expect(TokenKind::Semicolon, "';'");
      return item;
    }
    pos_ = saved_pos;
  }

  item.kind = BlockItem::Kind::ExprStmt;
  item.expr = ParseExp();
  Expect(TokenKind::Semicolon, "';'");
  return item;
}

std::vector<std::unique_ptr<Expr>> Parser::ParseDimensions() {
  std::vector<std::unique_ptr<Expr>> dimensions;
  while (Match(TokenKind::LBracket)) {
    dimensions.push_back(ParseExp());
    Expect(TokenKind::RBracket, "']'");
  }
  return dimensions;
}

std::unique_ptr<LValExpr> Parser::ParseLVal() {
  std::string name = Expect(TokenKind::Ident, "identifier").text;
  std::vector<std::unique_ptr<Expr>> indices;
  while (Match(TokenKind::LBracket)) {
    indices.push_back(ParseExp());
    Expect(TokenKind::RBracket, "']'");
  }
  return std::make_unique<LValExpr>(std::move(name), std::move(indices));
}

InitVal Parser::ParseInitVal() {
  InitVal init;
  if (Match(TokenKind::LBrace)) {
    init.is_list = true;
    if (Peek().kind != TokenKind::RBrace) {
      while (true) {
        init.list.push_back(ParseInitVal());
        if (!Match(TokenKind::Comma)) {
          break;
        }
      }
    }
    Expect(TokenKind::RBrace, "'}'");
    return init;
  }
  init.expr = ParseExp();
  return init;
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
  if (Peek().kind == TokenKind::Ident && Peek(1).kind == TokenKind::LParen) {
    const std::string name = Expect(TokenKind::Ident, "function name").text;
    Expect(TokenKind::LParen, "'('");
    std::vector<std::unique_ptr<Expr>> args;
    if (Peek().kind != TokenKind::RParen) {
      args = ParseFuncRParams();
    }
    Expect(TokenKind::RParen, "')'");
    return std::make_unique<CallExpr>(name, std::move(args));
  }
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
    return ParseLVal();
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
