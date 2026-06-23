#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include "ast_opt.h"
#include "codegen.h"
#include "leak_detect.h"
#include "lexer.h"
#include "parser.h"

struct Options {
  std::string mode;
  std::string input;
  std::string output;
};

Options ParseArgs(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-koopa" || arg == "-riscv" || arg == "-perf" ||
        arg == "-detect") {
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
       options.mode != "-perf" && options.mode != "-detect") ||
      options.input.empty() || options.output.empty()) {
    throw std::runtime_error(
        "usage: compiler <-koopa|-riscv|-perf|-detect> <input> -o <output>");
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

int main(int argc, char **argv) {
  try {
    const Options options = ParseArgs(argc, argv);
    Lexer lexer(ReadFile(options.input));
    Parser parser(lexer.Tokenize());
    const std::unique_ptr<Program> program = parser.ParseCompUnit();
    if (options.mode == "-detect") {
      const std::vector<LeakReport> leaks = DetectSideChannelLeaks(*program);
      std::ofstream out(options.output);
      if (!out) {
        throw std::runtime_error("failed to open output file: " + options.output);
      }
      for (const LeakReport &leak : leaks) {
        const std::string line = "warning: " + leak.kind + " in function " +
                                 leak.function + ": " + leak.message;
        out << line << '\n';
        std::cerr << line << '\n';
      }
      return 0;
    }
    OptimizeAst(*program);
    if (options.mode == "-koopa") {
      WriteKoopa(options.output, *program);
    } else {
      WriteRiscv(options.output, *program);
    }
    return 0;
  } catch (const std::exception &err) {
    std::cerr << "compiler: " << err.what() << '\n';
    return 1;
  }
}
