#pragma once

#include <iosfwd>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"

struct Symbol {
  enum class Kind {
    Const,
    Var,
    GlobalVar,
  } kind;
  int const_value = 0;
  std::string ir_name;
  std::string asm_name;
  int stack_offset = 0;
  std::vector<int> dimensions;
  bool is_pointer = false;
  bool cacheable = false;
};

struct FuncInfo {
  TypeKind return_type = TypeKind::Int;
  int param_count = 0;
};

struct CachedReg {
  int stack_offset = 0;
  std::string reg;
  bool dirty = false;
};

struct KoopaAddrInfo {
  std::string ptr;
  std::vector<int> remaining_dimensions;
  bool from_pointer = false;
  int indices_used = 0;
};

class KoopaGenerator {
 public:
  explicit KoopaGenerator(std::ostream &out);
  KoopaGenerator(std::ostream &out, std::unordered_set<std::string> rewrite_if_locs,
                 std::unordered_set<std::string> rewrite_array_lookup_locs,
                 std::unordered_set<std::string> rewrite_logic_expr_locs);

  void Generate(const Program &program);

 private:
  void RegisterLibraryFunctions();
  void RegisterFunctions(const Program &program);
  void GenerateGlobalItem(const GlobalItem &item);
  void GenerateFunction(const FunctionDef &function);
  void ResetFunctionState();
  void CollectAllocs(const FunctionDef &function);
  void CollectAllocs(const Block &block);
  void CollectAllocs(const BlockItem &item);
  void CollectLogicTemps(const Expr &expr);
  void GenerateBlock(const Block &block);
  void GenerateItem(const BlockItem &item);
  void GenerateIf(const BlockItem &item);
  bool TryGenerateRewriteIf(const BlockItem &item);
  std::string TryGenerateRewriteArrayLookup(const LValExpr &lval);
  std::string TryGenerateRewriteLogicExpr(const BinaryExpr &expr);
  void GenerateWhile(const BlockItem &item);
  std::string GenerateExpr(const Expr &expr);
  KoopaAddrInfo GenerateLValAddress(const LValExpr &lval);
  void GenerateCond(const Expr &expr, const std::string &true_label,
                    const std::string &false_label);
  std::vector<int> EvalDimensions(const std::vector<std::unique_ptr<Expr>> &dimensions) const;
  void GenerateLocalArrayInit(const Symbol &symbol, const InitVal &init);
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
  std::string NewBlockName(const std::string &hint);
  static std::string JoinArgs(const std::vector<std::string> &args);

  std::ostream &out_;
  std::vector<std::unordered_map<std::string, Symbol>> scopes_;
  std::unordered_map<std::string, FuncInfo> functions_;
  std::unordered_map<const VarDef *, std::string> var_alloc_names_;
  std::unordered_map<const Param *, std::string> param_alloc_names_;
  std::unordered_map<const BinaryExpr *, std::string> logic_alloc_names_;
  std::vector<std::string> alloc_lines_;
  int next_value_id_ = 0;
  int next_alloc_id_ = 0;
  int next_block_id_ = 0;
  bool entry_terminated_ = false;
  TypeKind current_return_type_ = TypeKind::Int;
  std::vector<std::string> loop_entry_labels_;
  std::vector<std::string> loop_end_labels_;
  std::unordered_set<std::string> rewrite_if_locs_;
  std::unordered_set<std::string> rewrite_array_lookup_locs_;
  std::unordered_set<std::string> rewrite_logic_expr_locs_;
};

class RiscvGenerator {
 public:
  explicit RiscvGenerator(std::ostream &out);
  RiscvGenerator(std::ostream &out, std::unordered_set<std::string> rewrite_if_locs,
                 std::unordered_set<std::string> rewrite_array_lookup_locs,
                 std::unordered_set<std::string> rewrite_logic_expr_locs);

  void Generate(const Program &program);

 private:
  void RegisterLibraryFunctions();
  void RegisterFunctions(const Program &program);
  void GenerateGlobalItem(const GlobalItem &item);
  void GenerateFunction(const FunctionDef &function);
  void ResetFunctionState();
  void ScanFunction(const FunctionDef &function);
  void ScanBlock(const Block &block);
  void ScanItem(const BlockItem &item);
  void ScanExpr(const Expr &expr, int depth);
  void GenerateBlock(const Block &block);
  void GenerateItem(const BlockItem &item);
  void GenerateIf(const BlockItem &item);
  bool TryGenerateRewriteIf(const BlockItem &item);
  bool TryGenerateRewriteArrayLookup(const LValExpr &lval, int depth);
  bool TryGenerateRewriteLogicExpr(const BinaryExpr &expr, int depth);
  void GenerateWhile(const BlockItem &item);
  std::string CurrentLoopEntry() const;
  std::string CurrentLoopEnd() const;
  void GenerateExpr(const Expr &expr, int depth = 0);
  void GenerateLValAddress(const LValExpr &lval, int depth = 0);
  void GenerateCond(const Expr &expr, const std::string &true_label,
                    const std::string &false_label, int depth = 0);
  std::vector<int> EvalDimensions(const std::vector<std::unique_ptr<Expr>> &dimensions) const;
  void GenerateLocalArrayInit(const Symbol &symbol, const InitVal &init, int depth);
  int EvalConstExpr(const Expr &expr) const;
  void PushScope();
  void PopScope();
  void InsertSymbol(const std::string &name, Symbol symbol);
  Symbol &LookupSymbol(const std::string &name);
  const Symbol &LookupSymbol(const std::string &name) const;
  int TempOffset(int depth) const;
  void EmitStackAdjust(int bytes);
  void EmitReturn();
  void StoreToSymbol(const Symbol &symbol, const std::string &reg);
  void LoadFromSymbol(const Symbol &symbol, const std::string &reg);
  bool CanCacheSymbol(const Symbol &symbol) const;
  CachedReg &EnsureCachedReg(const Symbol &symbol);
  CachedReg *FindCachedReg(const Symbol &symbol);
  void FlushRegCache();
  void ClearRegCache();
  void FlushAndClearRegCache();
  void StoreAddressed(const std::string &addr_reg, const std::string &value_reg);
  void LoadAddressed(const std::string &addr_reg, const std::string &value_reg);
  void LoadSymbolAddress(const Symbol &symbol, const std::string &reg);
  std::string NewLabel(const std::string &hint);
  static int AlignTo16(int bytes);

  std::ostream &out_;
  std::vector<std::unordered_map<std::string, Symbol>> scopes_;
  std::unordered_map<std::string, FuncInfo> functions_;
  int next_var_offset_ = 0;
  int local_bytes_ = 0;
  int out_arg_bytes_ = 0;
  int max_temp_depth_ = 0;
  int frame_size_ = 0;
  bool needs_ra_ = false;
  int next_label_id_ = 0;
  bool current_terminated_ = false;
  TypeKind current_return_type_ = TypeKind::Int;
  std::vector<std::string> loop_entry_labels_;
  std::vector<std::string> loop_end_labels_;
  std::vector<CachedReg> reg_cache_;
  std::unordered_set<std::string> rewrite_if_locs_;
  std::unordered_set<std::string> rewrite_array_lookup_locs_;
  std::unordered_set<std::string> rewrite_logic_expr_locs_;
};

void WriteKoopa(const std::string &path, const Program &program);
void WriteKoopaRewrite(const std::string &path, const Program &program,
                       std::unordered_set<std::string> rewrite_if_locs,
                       std::unordered_set<std::string> rewrite_array_lookup_locs,
                       std::unordered_set<std::string> rewrite_logic_expr_locs);
void WriteRiscv(const std::string &path, const Program &program);
void WriteRiscvRewrite(const std::string &path, const Program &program,
                       std::unordered_set<std::string> rewrite_if_locs,
                       std::unordered_set<std::string> rewrite_array_lookup_locs,
                       std::unordered_set<std::string> rewrite_logic_expr_locs);
