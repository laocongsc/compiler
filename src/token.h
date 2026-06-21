#pragma once

#include <string>

enum class TokenKind {
  Const,
  If,
  Else,
  While,
  Break,
  Continue,
  Int,
  Void,
  Return,
  Ident,
  Number,
  LParen,
  RParen,
  LBrace,
  RBrace,
  Semicolon,
  Comma,
  Assign,
  Plus,
  Minus,
  Not,
  Star,
  Slash,
  Percent,
  Less,
  Greater,
  LessEqual,
  GreaterEqual,
  Equal,
  NotEqual,
  And,
  Or,
  End,
};

struct Token {
  TokenKind kind;
  std::string text;
  int value = 0;
};
