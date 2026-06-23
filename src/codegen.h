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
  std::vector<int> dimensions;
  bool is_pointer = false;
};

struct FuncInfo {
  TypeKind return_type = TypeKind::Int;
  int param_count = 0;
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
