#pragma once

#include <iosfwd>
#include <string>

#include "ast.h"

class KoopaGenerator {
 public:
  explicit KoopaGenerator(std::ostream &out);

  std::string Generate(const Expr &expr);

 private:
  std::string Emit(const std::string &op, const std::string &lhs,
                   const std::string &rhs);

  std::ostream &out_;
  int next_id_ = 0;
};

class RiscvGenerator {
 public:
  explicit RiscvGenerator(std::ostream &out);

  void Generate(const Expr &expr);

 private:
  void GenerateExpr(const Expr &expr);
  void PushT0();
  void PopT1();

  std::ostream &out_;
};

void WriteKoopa(const std::string &path, const Expr &return_value);
void WriteRiscv(const std::string &path, const Expr &return_value);
