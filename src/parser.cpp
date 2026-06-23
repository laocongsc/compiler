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
    item.loc = decl.loc;
    item.kind = GlobalItem::Kind::ConstDecl;
    item.const_defs = std::move(decl.const_defs);
    return item;
  }
  return ParseTopLevelDeclOrFunc();
}

GlobalItem Parser::ParseTopLevelDeclOrFunc() {
  const TypeKind type = ParseFuncType();
  const Token name_token = Expect(TokenKind::Ident, "identifier");
  const std::string name = name_token.text;
  if (Match(TokenKind::LParen)) {
    GlobalItem item;
    item.loc = name_token.loc;
    item.kind = GlobalItem::Kind::FuncDef;
    item.function = ParseFuncDef(type, name);
    item.function->loc = name_token.loc;
    return item;
  }
  if (type != TypeKind::Int) {
    throw std::runtime_error("global variable must have int type");
  }
  BlockItem decl = ParseVarDecl(true, name);
  GlobalItem item;
  item.loc = decl.loc;
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
  const Token name_token = Expect(TokenKind::Ident, "parameter name");
  param.name = name_token.text;
  param.loc = name_token.loc;
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
  const Token const_token = Expect(TokenKind::Const, "'const'");
  item.loc = const_token.loc;
  Expect(TokenKind::Int, "'int'");
  while (true) {
    ConstDef def;
    const Token name_token = Expect(TokenKind::Ident, "constant name");
    def.name = name_token.text;
    def.loc = name_token.loc;
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
    const Token int_token = Expect(TokenKind::Int, "'int'");
    item.loc = int_token.loc;
  }
  bool first = true;
  while (true) {
    VarDef def;
    if (first && !first_name.empty()) {
      def.name = std::move(first_name);
      def.loc = item.loc;
    } else {
      const Token name_token = Expect(TokenKind::Ident, "variable name");
      def.name = name_token.text;
      def.loc = name_token.loc;
      if (first) {
        item.loc = name_token.loc;
      }
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
  if (Peek().kind == TokenKind::Return) {
    const Token return_token = Expect(TokenKind::Return, "'return'");
    item.kind = BlockItem::Kind::Return;
    item.loc = return_token.loc;
    if (Peek().kind != TokenKind::Semicolon) {
      item.expr = ParseExp();
    }
    Expect(TokenKind::Semicolon, "';'");
    return item;
  }
  if (Peek().kind == TokenKind::LBrace) {
    item.kind = BlockItem::Kind::Block;
    item.loc = Peek().loc;
    item.block = ParseBlock();
    return item;
  }
  if (Peek().kind == TokenKind::If) {
    const Token if_token = Expect(TokenKind::If, "'if'");
    item.kind = BlockItem::Kind::If;
    item.loc = if_token.loc;
    Expect(TokenKind::LParen, "'('");
    item.expr = ParseExp();
    Expect(TokenKind::RParen, "')'");
    item.then_stmt = std::make_unique<BlockItem>(ParseStmt());
    if (Match(TokenKind::Else)) {
      item.else_stmt = std::make_unique<BlockItem>(ParseStmt());
    }
    return item;
  }
  if (Peek().kind == TokenKind::While) {
    const Token while_token = Expect(TokenKind::While, "'while'");
    item.kind = BlockItem::Kind::While;
    item.loc = while_token.loc;
    Expect(TokenKind::LParen, "'('");
    item.expr = ParseExp();
    Expect(TokenKind::RParen, "')'");
    item.body_stmt = std::make_unique<BlockItem>(ParseStmt());
    return item;
  }
  if (Peek().kind == TokenKind::Break) {
    const Token break_token = Expect(TokenKind::Break, "'break'");
    item.kind = BlockItem::Kind::Break;
    item.loc = break_token.loc;
    Expect(TokenKind::Semicolon, "';'");
    return item;
  }
  if (Peek().kind == TokenKind::Continue) {
    const Token continue_token = Expect(TokenKind::Continue, "'continue'");
    item.kind = BlockItem::Kind::Continue;
    item.loc = continue_token.loc;
    Expect(TokenKind::Semicolon, "';'");
    return item;
  }
  if (Peek().kind == TokenKind::Semicolon) {
    const Token semi_token = Expect(TokenKind::Semicolon, "';'");
    item.kind = BlockItem::Kind::ExprStmt;
    item.loc = semi_token.loc;
    return item;
  }
  if (Peek().kind == TokenKind::Ident) {
    const size_t saved_pos = pos_;
    std::unique_ptr<LValExpr> lval = ParseLVal();
    if (Match(TokenKind::Assign)) {
      item.kind = BlockItem::Kind::Assign;
      item.loc = lval->loc;
      item.lval = std::move(lval);
      item.expr = ParseExp();
      Expect(TokenKind::Semicolon, "';'");
      return item;
    }
    pos_ = saved_pos;
  }

  item.kind = BlockItem::Kind::ExprStmt;
  item.expr = ParseExp();
  item.loc = item.expr ? item.expr->loc : SourceLocation{};
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
  const Token name_token = Expect(TokenKind::Ident, "identifier");
  std::string name = name_token.text;
  std::vector<std::unique_ptr<Expr>> indices;
  while (Match(TokenKind::LBracket)) {
    indices.push_back(ParseExp());
    Expect(TokenKind::RBracket, "']'");
  }
  auto expr = std::make_unique<LValExpr>(std::move(name), std::move(indices));
  expr->loc = name_token.loc;
  return expr;
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
  while (Peek().kind == TokenKind::Or) {
    const Token op_token = Expect(TokenKind::Or, "'||'");
    expr = std::make_unique<BinaryExpr>(BinaryOp::Or, std::move(expr),
                                        ParseLAndExp());
    expr->loc = op_token.loc;
  }
  return expr;
}

std::unique_ptr<Expr> Parser::ParseLAndExp() {
  std::unique_ptr<Expr> expr = ParseEqExp();
  while (Peek().kind == TokenKind::And) {
    const Token op_token = Expect(TokenKind::And, "'&&'");
    expr = std::make_unique<BinaryExpr>(BinaryOp::And, std::move(expr),
                                        ParseEqExp());
    expr->loc = op_token.loc;
  }
  return expr;
}

std::unique_ptr<Expr> Parser::ParseEqExp() {
  std::unique_ptr<Expr> expr = ParseRelExp();
  while (true) {
    if (Peek().kind == TokenKind::Equal) {
      const Token op_token = Expect(TokenKind::Equal, "'=='");
      expr = std::make_unique<BinaryExpr>(BinaryOp::Eq, std::move(expr),
                                          ParseRelExp());
      expr->loc = op_token.loc;
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
    if (Peek().kind == TokenKind::Less) {
      const Token op_token = Expect(TokenKind::Less, "'<'");
      expr = std::make_unique<BinaryExpr>(BinaryOp::Lt, std::move(expr),
                                          ParseAddExp());
      expr->loc = op_token.loc;
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
    if (Peek().kind == TokenKind::Plus) {
      const Token op_token = Expect(TokenKind::Plus, "'+'");
      expr = std::make_unique<BinaryExpr>(BinaryOp::Add, std::move(expr),
                                          ParseMulExp());
      expr->loc = op_token.loc;
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
    if (Peek().kind == TokenKind::Star) {
      const Token op_token = Expect(TokenKind::Star, "'*'");
      expr = std::make_unique<BinaryExpr>(BinaryOp::Mul, std::move(expr),
                                          ParseUnaryExp());
      expr->loc = op_token.loc;
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
    const Token name_token = Expect(TokenKind::Ident, "function name");
    const std::string name = name_token.text;
    Expect(TokenKind::LParen, "'('");
    std::vector<std::unique_ptr<Expr>> args;
    if (Peek().kind != TokenKind::RParen) {
      args = ParseFuncRParams();
    }
    Expect(TokenKind::RParen, "')'");
    auto call = std::make_unique<CallExpr>(name, std::move(args));
    call->loc = name_token.loc;
    return call;
  }
  if (Peek().kind == TokenKind::Plus) {
    const Token op_token = Expect(TokenKind::Plus, "'+'");
    auto expr = std::make_unique<UnaryExpr>(UnaryOp::Plus, ParseUnaryExp());
    expr->loc = op_token.loc;
    return expr;
  }
  if (Peek().kind == TokenKind::Minus) {
    const Token op_token = Expect(TokenKind::Minus, "'-'");
    auto expr = std::make_unique<UnaryExpr>(UnaryOp::Minus, ParseUnaryExp());
    expr->loc = op_token.loc;
    return expr;
  }
  if (Peek().kind == TokenKind::Not) {
    const Token op_token = Expect(TokenKind::Not, "'!'");
    auto expr = std::make_unique<UnaryExpr>(UnaryOp::Not, ParseUnaryExp());
    expr->loc = op_token.loc;
    return expr;
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
  const Token number = Expect(TokenKind::Number, "integer constant, identifier or '('");
  auto expr = std::make_unique<NumberExpr>(number.value);
  expr->loc = number.loc;
  return expr;
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
