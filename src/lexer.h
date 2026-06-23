#pragma once

#include <string>
#include <vector>

#include "token.h"

class Lexer {
 public:
  explicit Lexer(std::string input);

  std::vector<Token> Tokenize();

 private:
  static bool IsIdentStart(char ch);
  static bool IsIdentPart(char ch);

  char Advance();
  SourceLocation CurrentLocation() const;
  void SkipWhitespaceAndComments();
  Token ReadIdentOrKeyword();
  Token ReadNumber();
  Token ReadPunct();

  std::string input_;
  size_t pos_ = 0;
  int line_ = 1;
  int column_ = 1;
};
