#include "lexer.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <utility>

Lexer::Lexer(std::string input) : input_(std::move(input)) {}

std::vector<Token> Lexer::Tokenize() {
  std::vector<Token> tokens;
  while (true) {
    SkipWhitespaceAndComments();
    if (pos_ >= input_.size()) {
      tokens.push_back({TokenKind::End, "", 0, CurrentLocation()});
      break;
    }

    const char ch = input_[pos_];
    if (IsIdentStart(ch)) {
      tokens.push_back(ReadIdentOrKeyword());
    } else if (std::isdigit(static_cast<unsigned char>(ch))) {
      tokens.push_back(ReadNumber());
    } else {
      tokens.push_back(ReadPunct());
    }
  }
  return tokens;
}

bool Lexer::IsIdentStart(char ch) {
  return ch == '_' || std::isalpha(static_cast<unsigned char>(ch));
}

bool Lexer::IsIdentPart(char ch) {
  return IsIdentStart(ch) || std::isdigit(static_cast<unsigned char>(ch));
}

char Lexer::Advance() {
  const char ch = input_[pos_++];
  if (ch == '\n') {
    ++line_;
    column_ = 1;
  } else {
    ++column_;
  }
  return ch;
}

SourceLocation Lexer::CurrentLocation() const { return SourceLocation{line_, column_}; }

void Lexer::SkipWhitespaceAndComments() {
  while (pos_ < input_.size()) {
    if (std::isspace(static_cast<unsigned char>(input_[pos_]))) {
      Advance();
      continue;
    }

    if (input_[pos_] == '/' && pos_ + 1 < input_.size()) {
      if (input_[pos_ + 1] == '/') {
        Advance();
        Advance();
        while (pos_ < input_.size() && input_[pos_] != '\n') {
          Advance();
        }
        continue;
      }
      if (input_[pos_ + 1] == '*') {
        Advance();
        Advance();
        while (pos_ + 1 < input_.size() &&
               !(input_[pos_] == '*' && input_[pos_ + 1] == '/')) {
          Advance();
        }
        if (pos_ + 1 >= input_.size()) {
          throw std::runtime_error("unterminated block comment");
        }
        Advance();
        Advance();
        continue;
      }
    }

    break;
  }
}

Token Lexer::ReadIdentOrKeyword() {
  const SourceLocation loc = CurrentLocation();
  const size_t start = pos_;
  while (pos_ < input_.size() && IsIdentPart(input_[pos_])) {
    Advance();
  }

  const std::string text = input_.substr(start, pos_ - start);
  if (text == "const") {
    return {TokenKind::Const, text, 0, loc};
  }
  if (text == "if") {
    return {TokenKind::If, text, 0, loc};
  }
  if (text == "else") {
    return {TokenKind::Else, text, 0, loc};
  }
  if (text == "while") {
    return {TokenKind::While, text, 0, loc};
  }
  if (text == "break") {
    return {TokenKind::Break, text, 0, loc};
  }
  if (text == "continue") {
    return {TokenKind::Continue, text, 0, loc};
  }
  if (text == "secret") {
    return {TokenKind::Secret, text, 0, loc};
  }
  if (text == "int") {
    return {TokenKind::Int, text, 0, loc};
  }
  if (text == "void") {
    return {TokenKind::Void, text, 0, loc};
  }
  if (text == "return") {
    return {TokenKind::Return, text, 0, loc};
  }
  return {TokenKind::Ident, text, 0, loc};
}

Token Lexer::ReadNumber() {
  const SourceLocation loc = CurrentLocation();
  const size_t start = pos_;
  while (pos_ < input_.size() &&
         std::isalnum(static_cast<unsigned char>(input_[pos_]))) {
    Advance();
  }

  const std::string text = input_.substr(start, pos_ - start);
  errno = 0;
  char *end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 0);
  if (errno != 0 || end == text.c_str() || *end != '\0' || value < 0 ||
      value > 2147483647L) {
    throw std::runtime_error("invalid integer constant: " + text);
  }
  return {TokenKind::Number, text, static_cast<int>(value), loc};
}

Token Lexer::ReadPunct() {
  const SourceLocation loc = CurrentLocation();
  const char ch = Advance();
  if (pos_ < input_.size()) {
    const char next = input_[pos_];
    if (ch == '<' && next == '=') {
      Advance();
      return {TokenKind::LessEqual, "<=", 0, loc};
    }
    if (ch == '>' && next == '=') {
      Advance();
      return {TokenKind::GreaterEqual, ">=", 0, loc};
    }
    if (ch == '=' && next == '=') {
      Advance();
      return {TokenKind::Equal, "==", 0, loc};
    }
    if (ch == '!' && next == '=') {
      Advance();
      return {TokenKind::NotEqual, "!=", 0, loc};
    }
    if (ch == '&' && next == '&') {
      Advance();
      return {TokenKind::And, "&&", 0, loc};
    }
    if (ch == '|' && next == '|') {
      Advance();
      return {TokenKind::Or, "||", 0, loc};
    }
  }

  switch (ch) {
    case '(':
      return {TokenKind::LParen, "(", 0, loc};
    case ')':
      return {TokenKind::RParen, ")", 0, loc};
    case '{':
      return {TokenKind::LBrace, "{", 0, loc};
    case '}':
      return {TokenKind::RBrace, "}", 0, loc};
    case '[':
      return {TokenKind::LBracket, "[", 0, loc};
    case ']':
      return {TokenKind::RBracket, "]", 0, loc};
    case ';':
      return {TokenKind::Semicolon, ";", 0, loc};
    case ',':
      return {TokenKind::Comma, ",", 0, loc};
    case '=':
      return {TokenKind::Assign, "=", 0, loc};
    case '+':
      return {TokenKind::Plus, "+", 0, loc};
    case '-':
      return {TokenKind::Minus, "-", 0, loc};
    case '!':
      return {TokenKind::Not, "!", 0, loc};
    case '*':
      return {TokenKind::Star, "*", 0, loc};
    case '/':
      return {TokenKind::Slash, "/", 0, loc};
    case '%':
      return {TokenKind::Percent, "%", 0, loc};
    case '<':
      return {TokenKind::Less, "<", 0, loc};
    case '>':
      return {TokenKind::Greater, ">", 0, loc};
    default:
      throw std::runtime_error(std::string("unexpected character: ") + ch);
  }
}
