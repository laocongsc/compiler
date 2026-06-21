#pragma once

#include <string>

enum class TokenKind {
  Int,
  Return,
  Ident,
  Number,
  LParen,
  RParen,
  LBrace,
  RBrace,
  Semicolon,
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
