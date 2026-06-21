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
      tokens.push_back({TokenKind::End, ""});
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

void Lexer::SkipWhitespaceAndComments() {
  while (pos_ < input_.size()) {
    if (std::isspace(static_cast<unsigned char>(input_[pos_]))) {
      ++pos_;
      continue;
    }

    if (input_[pos_] == '/' && pos_ + 1 < input_.size()) {
      if (input_[pos_ + 1] == '/') {
        pos_ += 2;
        while (pos_ < input_.size() && input_[pos_] != '\n') {
          ++pos_;
        }
        continue;
      }
      if (input_[pos_ + 1] == '*') {
        pos_ += 2;
        while (pos_ + 1 < input_.size() &&
               !(input_[pos_] == '*' && input_[pos_ + 1] == '/')) {
          ++pos_;
        }
        if (pos_ + 1 >= input_.size()) {
          throw std::runtime_error("unterminated block comment");
        }
        pos_ += 2;
        continue;
      }
    }

    break;
  }
}

Token Lexer::ReadIdentOrKeyword() {
  const size_t start = pos_;
  while (pos_ < input_.size() && IsIdentPart(input_[pos_])) {
    ++pos_;
  }

  const std::string text = input_.substr(start, pos_ - start);
  if (text == "const") {
    return {TokenKind::Const, text};
  }
  if (text == "if") {
    return {TokenKind::If, text};
  }
  if (text == "else") {
    return {TokenKind::Else, text};
  }
  if (text == "while") {
    return {TokenKind::While, text};
  }
  if (text == "break") {
    return {TokenKind::Break, text};
  }
  if (text == "continue") {
    return {TokenKind::Continue, text};
  }
  if (text == "int") {
    return {TokenKind::Int, text};
  }
  if (text == "void") {
    return {TokenKind::Void, text};
  }
  if (text == "return") {
    return {TokenKind::Return, text};
  }
  return {TokenKind::Ident, text};
}

Token Lexer::ReadNumber() {
  const size_t start = pos_;
  while (pos_ < input_.size() &&
         std::isalnum(static_cast<unsigned char>(input_[pos_]))) {
    ++pos_;
  }

  const std::string text = input_.substr(start, pos_ - start);
  errno = 0;
  char *end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 0);
  if (errno != 0 || end == text.c_str() || *end != '\0' || value < 0 ||
      value > 2147483647L) {
    throw std::runtime_error("invalid integer constant: " + text);
  }
  return {TokenKind::Number, text, static_cast<int>(value)};
}

Token Lexer::ReadPunct() {
  const char ch = input_[pos_++];
  if (pos_ < input_.size()) {
    const char next = input_[pos_];
    if (ch == '<' && next == '=') {
      ++pos_;
      return {TokenKind::LessEqual, "<="};
    }
    if (ch == '>' && next == '=') {
      ++pos_;
      return {TokenKind::GreaterEqual, ">="};
    }
    if (ch == '=' && next == '=') {
      ++pos_;
      return {TokenKind::Equal, "=="};
    }
    if (ch == '!' && next == '=') {
      ++pos_;
      return {TokenKind::NotEqual, "!="};
    }
    if (ch == '&' && next == '&') {
      ++pos_;
      return {TokenKind::And, "&&"};
    }
    if (ch == '|' && next == '|') {
      ++pos_;
      return {TokenKind::Or, "||"};
    }
  }

  switch (ch) {
    case '(':
      return {TokenKind::LParen, "("};
    case ')':
      return {TokenKind::RParen, ")"};
    case '{':
      return {TokenKind::LBrace, "{"};
    case '}':
      return {TokenKind::RBrace, "}"};
    case '[':
      return {TokenKind::LBracket, "["};
    case ']':
      return {TokenKind::RBracket, "]"};
    case ';':
      return {TokenKind::Semicolon, ";"};
    case ',':
      return {TokenKind::Comma, ","};
    case '=':
      return {TokenKind::Assign, "="};
    case '+':
      return {TokenKind::Plus, "+"};
    case '-':
      return {TokenKind::Minus, "-"};
    case '!':
      return {TokenKind::Not, "!"};
    case '*':
      return {TokenKind::Star, "*"};
    case '/':
      return {TokenKind::Slash, "/"};
    case '%':
      return {TokenKind::Percent, "%"};
    case '<':
      return {TokenKind::Less, "<"};
    case '>':
      return {TokenKind::Greater, ">"};
    default:
      throw std::runtime_error(std::string("unexpected character: ") + ch);
  }
}
