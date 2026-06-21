#pragma once

#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.h"

struct Symbol {
  enum class Kind {
    Const,
    Var,
  } kind;
  int const_value = 0;
  std::string ir_name;
  int stack_offset = 0;
};

class KoopaGenerator {
 public:
  explicit KoopaGenerator(std::ostream &out);

  void Generate(const Program &program);

 private:
  void GenerateBlock(const Block &block);
  void GenerateItem(const BlockItem &item);
  std::string GenerateExpr(const Expr &expr);
  int EvalConstExpr(const Expr &expr) const;
  void PushScope();
  void PopScope();
  void InsertSymbol(const std::string &name, Symbol symbol);
  Symbol &LookupSymbol(const std::string &name);
  const Symbol &LookupSymbol(const std::string &name) const;
  std::string EmitBinary(const std::string &op, const std::string &lhs,
                         const std::string &rhs);
  std::string NewValueName();
  std::string NewAllocName(const std::string &hint);

  std::ostream &out_;
  std::vector<std::unordered_map<std::string, Symbol>> scopes_;
  int next_value_id_ = 0;
  int next_alloc_id_ = 0;
  bool entry_terminated_ = false;
};

class RiscvGenerator {
 public:
  explicit RiscvGenerator(std::ostream &out);

  void Generate(const Program &program);

 private:
  void ScanBlock(const Block &block);
  void ScanItem(const BlockItem &item);
  void ScanExpr(const Expr &expr, int depth);
  void GenerateBlock(const Block &block);
  void GenerateItem(const BlockItem &item);
  void GenerateExpr(const Expr &expr, int depth = 0);
  int EvalConstExpr(const Expr &expr) const;
  void PushScope();
  void PopScope();
  void InsertSymbol(const std::string &name, Symbol symbol);
  Symbol &LookupSymbol(const std::string &name);
  const Symbol &LookupSymbol(const std::string &name) const;
  int TempOffset(int depth) const;
  void EmitStackAdjust(int bytes);
  static int AlignTo16(int bytes);

  std::ostream &out_;
  std::vector<std::unordered_map<std::string, Symbol>> scopes_;
  int next_var_offset_ = 0;
  int max_temp_depth_ = 0;
  int frame_size_ = 0;
};

void WriteKoopa(const std::string &path, const Program &program);
void WriteRiscv(const std::string &path, const Program &program);
