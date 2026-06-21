#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

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
  End,
};

struct Token {
  TokenKind kind;
  std::string text;
  int value = 0;
};

class Lexer {
 public:
  explicit Lexer(std::string input) : input_(std::move(input)) {}

  std::vector<Token> Tokenize() {
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

 private:
  static bool IsIdentStart(char ch) {
    return ch == '_' || std::isalpha(static_cast<unsigned char>(ch));
  }

  static bool IsIdentPart(char ch) {
    return IsIdentStart(ch) || std::isdigit(static_cast<unsigned char>(ch));
  }

  void SkipWhitespaceAndComments() {
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

  Token ReadIdentOrKeyword() {
    const size_t start = pos_;
    while (pos_ < input_.size() && IsIdentPart(input_[pos_])) {
      ++pos_;
    }

    const std::string text = input_.substr(start, pos_ - start);
    if (text == "int") {
      return {TokenKind::Int, text};
    }
    if (text == "return") {
      return {TokenKind::Return, text};
    }
    return {TokenKind::Ident, text};
  }

  Token ReadNumber() {
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

  Token ReadPunct() {
    const char ch = input_[pos_++];
    switch (ch) {
      case '(':
        return {TokenKind::LParen, "("};
      case ')':
        return {TokenKind::RParen, ")"};
      case '{':
        return {TokenKind::LBrace, "{"};
      case '}':
        return {TokenKind::RBrace, "}"};
      case ';':
        return {TokenKind::Semicolon, ";"};
      default:
        throw std::runtime_error(std::string("unexpected character: ") + ch);
    }
  }

  std::string input_;
  size_t pos_ = 0;
};

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  int ParseCompUnit() {
    Expect(TokenKind::Int, "'int'");
    const Token ident = Expect(TokenKind::Ident, "function name");
    if (ident.text != "main") {
      throw std::runtime_error("Lv1 only supports function 'main'");
    }
    Expect(TokenKind::LParen, "'('");
    Expect(TokenKind::RParen, "')'");
    Expect(TokenKind::LBrace, "'{'");
    Expect(TokenKind::Return, "'return'");
    const int return_value = Expect(TokenKind::Number, "integer constant").value;
    Expect(TokenKind::Semicolon, "';'");
    Expect(TokenKind::RBrace, "'}'");
    Expect(TokenKind::End, "end of file");
    return return_value;
  }

 private:
  const Token &Peek() const {
    if (pos_ >= tokens_.size()) {
      throw std::runtime_error("unexpected end of token stream");
    }
    return tokens_[pos_];
  }

  Token Expect(TokenKind kind, const std::string &expected) {
    const Token token = Peek();
    if (token.kind != kind) {
      throw std::runtime_error("expected " + expected + ", got '" + token.text +
                               "'");
    }
    ++pos_;
    return token;
  }

  std::vector<Token> tokens_;
  size_t pos_ = 0;
};

struct Options {
  std::string mode;
  std::string input;
  std::string output;
};

Options ParseArgs(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-koopa" || arg == "-riscv" || arg == "-perf") {
      if (++i >= argc) {
        throw std::runtime_error("missing input after " + arg);
      }
      options.mode = arg;
      options.input = argv[i];
    } else if (arg == "-o") {
      if (++i >= argc) {
        throw std::runtime_error("missing output after -o");
      }
      options.output = argv[i];
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }

  if ((options.mode != "-koopa" && options.mode != "-riscv" &&
       options.mode != "-perf") ||
      options.input.empty() || options.output.empty()) {
    throw std::runtime_error(
        "usage: compiler <-koopa|-riscv|-perf> <input> -o <output>");
  }
  return options;
}

std::string ReadFile(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("failed to open input file: " + path);
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

void WriteKoopa(const std::string &path, int return_value) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open output file: " + path);
  }
  out << "fun @main(): i32 {\n";
  out << "%entry:\n";
  out << "  ret " << return_value << "\n";
  out << "}\n";
}

void WriteRiscv(const std::string &path, int return_value) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open output file: " + path);
  }
  out << "  .text\n";
  out << "  .globl main\n";
  out << "main:\n";
  out << "  li a0, " << return_value << "\n";
  out << "  ret\n";
}

int main(int argc, char **argv) {
  try {
    const Options options = ParseArgs(argc, argv);
    Lexer lexer(ReadFile(options.input));
    Parser parser(lexer.Tokenize());
    const int return_value = parser.ParseCompUnit();
    if (options.mode == "-koopa") {
      WriteKoopa(options.output, return_value);
    } else {
      WriteRiscv(options.output, return_value);
    }
    return 0;
  } catch (const std::exception &err) {
    std::cerr << "compiler: " << err.what() << '\n';
    return 1;
  }
}
