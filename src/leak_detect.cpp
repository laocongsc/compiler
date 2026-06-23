#include "leak_detect.h"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

enum class SecLevel { Public, Secret };

struct LeakSymbol {
  SecLevel level = SecLevel::Public;
  bool is_array = false;
};

struct FuncInfo {
  TypeKind return_type = TypeKind::Int;
  bool is_library = false;
  int param_count = 0;
};

struct FunctionSummary {
  TypeKind return_type = TypeKind::Int;
  bool returns_secret_by_default = false;
  std::vector<bool> return_depends_on_param;
  bool has_secret_dependent_control = false;
  bool writes_secret_to_global = false;
  bool conservative = false;
};

struct AnalysisOptions {
  bool collect_reports = true;
  bool use_summaries = true;
  bool collect_return_level = false;
};

SecLevel Join(SecLevel lhs, SecLevel rhs) {
  return lhs == SecLevel::Secret || rhs == SecLevel::Secret
             ? SecLevel::Secret
             : SecLevel::Public;
}

bool IsSecret(SecLevel level) { return level == SecLevel::Secret; }

bool IsSecretName(const std::string &name) {
  return name.rfind("secret_", 0) == 0;
}

class LeakDetector {
 public:
  std::vector<LeakReport> Detect(const Program &program) {
    program_ = &program;
    reports_.clear();
    functions_.clear();
    summaries_.clear();
    function_defs_.clear();
    analysis_stack_.clear();
    RegisterLibraryFunctions();
    RegisterFunctions(program);
    BuildFunctionSummaries(program);

    options_ = AnalysisOptions{};
    scopes_.clear();
    globals_.clear();
    control_level_ = SecLevel::Public;
    current_function_ = "<global>";

    PushScope();
    AnalyzeGlobals(program);
    for (const GlobalItem &item : program.items) {
      if (item.kind == GlobalItem::Kind::FuncDef) {
        AnalyzeFunction(*item.function, nullptr);
      }
    }
    PopScope();
    return reports_;
  }

 private:
  void RegisterLibraryFunctions() {
    functions_.emplace("getint", FuncInfo{TypeKind::Int, true, 0});
    functions_.emplace("getch", FuncInfo{TypeKind::Int, true, 0});
    functions_.emplace("getarray", FuncInfo{TypeKind::Int, true, 1});
    functions_.emplace("putint", FuncInfo{TypeKind::Void, true, 1});
    functions_.emplace("putch", FuncInfo{TypeKind::Void, true, 1});
    functions_.emplace("putarray", FuncInfo{TypeKind::Void, true, 2});
    functions_.emplace("starttime", FuncInfo{TypeKind::Void, true, 0});
    functions_.emplace("stoptime", FuncInfo{TypeKind::Void, true, 0});
  }

  void RegisterFunctions(const Program &program) {
    for (const GlobalItem &item : program.items) {
      if (item.kind != GlobalItem::Kind::FuncDef) {
        continue;
      }
      const FunctionDef &function = *item.function;
      functions_[function.name] = FuncInfo{function.return_type, false,
                                           static_cast<int>(function.params.size())};
      function_defs_[function.name] = &function;
    }
  }

  void BuildFunctionSummaries(const Program &program) {
    const AnalysisOptions saved_options = options_;
    options_.collect_reports = false;
    options_.use_summaries = true;
    options_.collect_return_level = true;

    for (const GlobalItem &item : program.items) {
      if (item.kind == GlobalItem::Kind::FuncDef) {
        EnsureSummary(item.function->name);
      }
    }

    options_ = saved_options;
  }

  const FunctionSummary &EnsureSummary(const std::string &name) {
    const auto existing = summaries_.find(name);
    if (existing != summaries_.end()) {
      return existing->second;
    }

    const auto function_found = function_defs_.find(name);
    if (function_found == function_defs_.end()) {
      return ConservativeSummary(name);
    }

    if (analysis_stack_.count(name) != 0) {
      FunctionSummary recursive = MakeConservativeSummary(*function_found->second);
      summaries_[name] = recursive;
      return summaries_.at(name);
    }

    analysis_stack_.insert(name);
    const FunctionDef &function = *function_found->second;
    FunctionSummary summary;
    summary.return_type = function.return_type;
    summary.return_depends_on_param.assign(function.params.size(), false);

    summary.returns_secret_by_default = IsSecret(AnalyzeFunctionReturn(function, -1));
    for (size_t i = 0; i < function.params.size(); ++i) {
      summary.return_depends_on_param[i] =
          IsSecret(AnalyzeFunctionReturn(function, static_cast<int>(i)));
    }

    analysis_stack_.erase(name);
    summaries_[name] = summary;
    return summaries_.at(name);
  }

  const FunctionSummary &ConservativeSummary(const std::string &name) {
    const auto found = summaries_.find(name);
    if (found != summaries_.end()) {
      return found->second;
    }
    FunctionSummary summary;
    const auto info = functions_.find(name);
    if (info != functions_.end()) {
      summary.return_type = info->second.return_type;
      summary.return_depends_on_param.assign(info->second.param_count, true);
    }
    summary.conservative = true;
    summaries_[name] = summary;
    return summaries_.at(name);
  }

  static FunctionSummary MakeConservativeSummary(const FunctionDef &function) {
    FunctionSummary summary;
    summary.return_type = function.return_type;
    summary.return_depends_on_param.assign(function.params.size(), true);
    summary.conservative = true;
    return summary;
  }

  SecLevel AnalyzeFunctionReturn(const FunctionDef &function, int secret_param) {
    const auto saved_scopes = scopes_;
    const auto saved_globals = globals_;
    const std::string saved_function = current_function_;
    const SecLevel saved_control = control_level_;
    const SecLevel saved_return = return_level_;

    scopes_.clear();
    globals_.clear();
    control_level_ = SecLevel::Public;
    return_level_ = SecLevel::Public;
    current_function_ = function.name;
    PushScope();
    if (program_ != nullptr) {
      AnalyzeGlobals(*program_);
    }
    PushScope();
    for (size_t i = 0; i < function.params.size(); ++i) {
      const Param &param = function.params[i];
      SecLevel level = IsSecretName(param.name) ? SecLevel::Secret : SecLevel::Public;
      if (static_cast<int>(i) == secret_param) {
        level = SecLevel::Secret;
      }
      InsertSymbol(param.name, LeakSymbol{level, param.is_array});
      for (const auto &dimension : param.dimensions) {
        AnalyzeExpr(*dimension);
      }
    }
    AnalyzeBlock(*function.block);
    PopScope();
    PopScope();

    const SecLevel result = return_level_;
    scopes_ = saved_scopes;
    globals_ = saved_globals;
    current_function_ = saved_function;
    control_level_ = saved_control;
    return_level_ = saved_return;
    return result;
  }

  void AnalyzeGlobals(const Program &program) {
    for (const GlobalItem &item : program.items) {
      if (item.kind == GlobalItem::Kind::ConstDecl) {
        AnalyzeConstDecl(item.const_defs);
      } else if (item.kind == GlobalItem::Kind::VarDecl) {
        AnalyzeVarDecl(item.var_defs);
      }
    }
  }

  void PushScope() { scopes_.emplace_back(); }

  void PopScope() { scopes_.pop_back(); }

  void InsertSymbol(const std::string &name, LeakSymbol symbol) {
    scopes_.back()[name] = symbol;
    if (scopes_.size() == 1) {
      globals_.insert(name);
    }
  }

  LeakSymbol LookupSymbol(const std::string &name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = it->find(name);
      if (found != it->end()) {
        return found->second;
      }
    }
    return LeakSymbol{IsSecretName(name) ? SecLevel::Secret : SecLevel::Public,
                      false};
  }

  void SetSymbolLevel(const std::string &name, SecLevel level) {
    const SecLevel combined = Join(NameLevel(name), level);
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = it->find(name);
      if (found != it->end()) {
        const SecLevel previous = found->second.level;
        found->second.level = Join(found->second.level, combined);
        if (globals_.count(name) != 0 && IsSecret(found->second.level) &&
            !IsSecret(previous)) {
          active_summary_writes_secret_global_ = true;
        }
        return;
      }
    }
    InsertSymbol(name, LeakSymbol{combined, false});
  }

  void Report(const std::string &kind, const std::string &message) {
    if (!options_.collect_reports) {
      return;
    }
    reports_.push_back(LeakReport{kind, current_function_, message});
  }

  void AnalyzeFunction(const FunctionDef &function,
                       const std::vector<SecLevel> *param_override) {
    const std::string previous_function = current_function_;
    current_function_ = function.name;
    PushScope();
    for (size_t i = 0; i < function.params.size(); ++i) {
      const Param &param = function.params[i];
      SecLevel level = IsSecretName(param.name) ? SecLevel::Secret : SecLevel::Public;
      if (param_override != nullptr && i < param_override->size()) {
        level = (*param_override)[i];
      }
      InsertSymbol(param.name, LeakSymbol{level, param.is_array});
      for (const auto &dimension : param.dimensions) {
        AnalyzeExpr(*dimension);
      }
    }
    AnalyzeBlock(*function.block);
    PopScope();
    current_function_ = previous_function;
  }

  void AnalyzeBlock(const Block &block) {
    PushScope();
    for (const BlockItem &item : block.items) {
      AnalyzeItem(item);
    }
    PopScope();
  }

  void AnalyzeItem(const BlockItem &item) {
    switch (item.kind) {
      case BlockItem::Kind::ConstDecl:
        AnalyzeConstDecl(item.const_defs);
        break;
      case BlockItem::Kind::VarDecl:
        AnalyzeVarDecl(item.var_defs);
        break;
      case BlockItem::Kind::Assign:
        AnalyzeAssign(*item.lval, *item.expr);
        break;
      case BlockItem::Kind::Return:
        if (item.expr) {
          const SecLevel value = AnalyzeExpr(*item.expr);
          return_level_ = Join(return_level_, Join(value, control_level_));
        }
        break;
      case BlockItem::Kind::ExprStmt:
        if (item.expr) {
          AnalyzeExpr(*item.expr);
        }
        break;
      case BlockItem::Kind::Block:
        AnalyzeBlock(*item.block);
        break;
      case BlockItem::Kind::If: {
        const SecLevel condition = AnalyzeExpr(*item.expr);
        if (IsSecret(condition)) {
          Report("secret-dependent branch", "if condition depends on secret data");
          active_summary_has_secret_control_ = true;
        }
        WithControl(Join(control_level_, condition), [&]() {
          AnalyzeItem(*item.then_stmt);
        });
        if (item.else_stmt) {
          WithControl(Join(control_level_, condition), [&]() {
            AnalyzeItem(*item.else_stmt);
          });
        }
        break;
      }
      case BlockItem::Kind::While: {
        const SecLevel condition = AnalyzeExpr(*item.expr);
        if (IsSecret(condition)) {
          Report("secret-dependent loop bound",
                 "while condition depends on secret data");
          active_summary_has_secret_control_ = true;
        }
        WithControl(Join(control_level_, condition), [&]() {
          AnalyzeItem(*item.body_stmt);
        });
        break;
      }
      case BlockItem::Kind::Break:
        if (IsSecret(control_level_)) {
          Report("secret-dependent break",
                 "break executes under secret control");
          active_summary_has_secret_control_ = true;
        }
        break;
      case BlockItem::Kind::Continue:
        if (IsSecret(control_level_)) {
          Report("secret-dependent continue",
                 "continue executes under secret control");
          active_summary_has_secret_control_ = true;
        }
        break;
    }
  }

  template <typename Fn>
  void WithControl(SecLevel level, Fn fn) {
    const SecLevel saved = control_level_;
    control_level_ = level;
    fn();
    control_level_ = saved;
  }

  void AnalyzeConstDecl(const std::vector<ConstDef> &defs) {
    for (const ConstDef &def : defs) {
      for (const auto &dimension : def.dimensions) {
        AnalyzeExpr(*dimension);
      }
      const SecLevel init_level = AnalyzeInit(def.init);
      InsertSymbol(def.name,
                   LeakSymbol{Join(Join(NameLevel(def.name), init_level),
                                   control_level_),
                              !def.dimensions.empty()});
    }
  }

  void AnalyzeVarDecl(const std::vector<VarDef> &defs) {
    for (const VarDef &def : defs) {
      for (const auto &dimension : def.dimensions) {
        AnalyzeExpr(*dimension);
      }
      SecLevel level = Join(NameLevel(def.name), control_level_);
      if (def.has_init) {
        level = Join(level, AnalyzeInit(def.init));
      }
      InsertSymbol(def.name, LeakSymbol{level, !def.dimensions.empty()});
    }
  }

  SecLevel AnalyzeInit(const InitVal &init) {
    if (!init.is_list) {
      return init.expr ? AnalyzeExpr(*init.expr) : SecLevel::Public;
    }
    SecLevel level = SecLevel::Public;
    for (const InitVal &child : init.list) {
      level = Join(level, AnalyzeInit(child));
    }
    return level;
  }

  void AnalyzeAssign(const LValExpr &lval, const Expr &rhs) {
    const SecLevel rhs_level = AnalyzeExpr(rhs);
    const bool secret_index = AnalyzeLValIndices(lval);
    if (secret_index) {
      Report("secret-dependent array index",
             "write to " + lval.name + " uses a secret index");
    }
    const SecLevel assigned_level = Join(rhs_level, control_level_);
    if (lval.indices.empty()) {
      SetSymbolLevel(lval.name, assigned_level);
      return;
    }
    if (IsSecret(assigned_level)) {
      SetSymbolLevel(lval.name, SecLevel::Secret);
    }
  }

  SecLevel AnalyzeExpr(const Expr &expr) {
    if (dynamic_cast<const NumberExpr *>(&expr) != nullptr) {
      return SecLevel::Public;
    }

    if (const auto *lval = dynamic_cast<const LValExpr *>(&expr)) {
      const bool secret_index = AnalyzeLValIndices(*lval);
      if (secret_index) {
        Report("secret-dependent array index",
               "read from " + lval->name + " uses a secret index");
      }
      return LookupSymbol(lval->name).level;
    }

    if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
      return AnalyzeCall(*call);
    }

    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
      return AnalyzeExpr(*unary->operand);
    }

    const auto *binary = dynamic_cast<const BinaryExpr *>(&expr);
    if (binary == nullptr) {
      throw std::runtime_error("unknown expression node in leak detector");
    }

    const SecLevel lhs = AnalyzeExpr(*binary->lhs);
    const SecLevel rhs = AnalyzeExpr(*binary->rhs);
    if ((binary->op == BinaryOp::And || binary->op == BinaryOp::Or) &&
        (IsSecret(lhs) || IsSecret(rhs))) {
      Report("secret-dependent short-circuit",
             "logical operator depends on secret data");
      active_summary_has_secret_control_ = true;
    }
    return Join(lhs, rhs);
  }

  SecLevel AnalyzeCall(const CallExpr &call) {
    std::vector<SecLevel> arg_levels;
    arg_levels.reserve(call.args.size());
    bool has_secret_arg = false;
    for (const auto &arg : call.args) {
      const SecLevel arg_level = AnalyzeExpr(*arg);
      arg_levels.push_back(arg_level);
      has_secret_arg = has_secret_arg || IsSecret(arg_level);
    }

    if (IsOutputFunction(call.name)) {
      if (has_secret_arg) {
        Report("secret-dependent observable call",
               call.name + " observes secret data");
      }
      if (IsSecret(control_level_)) {
        Report("secret-dependent observable call",
               call.name + " executes under secret control");
      }
      return SecLevel::Public;
    }

    if (IsTimingFunction(call.name)) {
      if (IsSecret(control_level_)) {
        Report("secret-dependent timing call",
               call.name + " executes under secret control");
      }
      return SecLevel::Public;
    }

    if (IsInputFunction(call.name)) {
      return SecLevel::Public;
    }

    const auto user_function = function_defs_.find(call.name);
    if (user_function != function_defs_.end() && options_.collect_reports &&
        runtime_call_stack_.count(call.name) == 0) {
      return AnalyzeUserFunctionCall(*user_function->second, arg_levels);
    }

    const auto found = functions_.find(call.name);
    if (found != functions_.end() && found->second.return_type == TypeKind::Void) {
      (void)EnsureSummaryIfUserFunction(call.name);
      return SecLevel::Public;
    }

    const FunctionSummary *summary = EnsureSummaryIfUserFunction(call.name);
    if (summary == nullptr || summary->conservative) {
      SecLevel level = SecLevel::Public;
      for (SecLevel arg_level : arg_levels) {
        level = Join(level, arg_level);
      }
      return level;
    }

    SecLevel result = summary->returns_secret_by_default ? SecLevel::Secret
                                                         : SecLevel::Public;
    for (size_t i = 0; i < arg_levels.size() &&
                       i < summary->return_depends_on_param.size();
         ++i) {
      if (IsSecret(arg_levels[i]) && summary->return_depends_on_param[i]) {
        result = SecLevel::Secret;
      }
    }
    return result;
  }

  SecLevel AnalyzeUserFunctionCall(const FunctionDef &function,
                                   const std::vector<SecLevel> &arg_levels) {
    runtime_call_stack_.insert(function.name);
    const std::string saved_function = current_function_;
    const SecLevel saved_return = return_level_;

    current_function_ = function.name;
    return_level_ = SecLevel::Public;
    PushScope();
    for (size_t i = 0; i < function.params.size(); ++i) {
      const Param &param = function.params[i];
      SecLevel level = IsSecretName(param.name) ? SecLevel::Secret : SecLevel::Public;
      if (i < arg_levels.size()) {
        level = Join(level, arg_levels[i]);
      }
      InsertSymbol(param.name, LeakSymbol{level, param.is_array});
      for (const auto &dimension : param.dimensions) {
        AnalyzeExpr(*dimension);
      }
    }
    AnalyzeBlock(*function.block);
    PopScope();

    const SecLevel result = return_level_;
    return_level_ = saved_return;
    current_function_ = saved_function;
    runtime_call_stack_.erase(function.name);
    return result;
  }

  const FunctionSummary *EnsureSummaryIfUserFunction(const std::string &name) {
    const auto function_found = function_defs_.find(name);
    if (function_found == function_defs_.end()) {
      return nullptr;
    }
    return &EnsureSummary(name);
  }

  bool AnalyzeLValIndices(const LValExpr &lval) {
    bool secret_index = false;
    for (const auto &index : lval.indices) {
      secret_index = secret_index || IsSecret(AnalyzeExpr(*index));
    }
    return secret_index;
  }

  static SecLevel NameLevel(const std::string &name) {
    return IsSecretName(name) ? SecLevel::Secret : SecLevel::Public;
  }

  static bool IsInputFunction(const std::string &name) {
    return name == "getint" || name == "getch" || name == "getarray";
  }

  static bool IsOutputFunction(const std::string &name) {
    return name == "putint" || name == "putch" || name == "putarray";
  }

  static bool IsTimingFunction(const std::string &name) {
    return name == "starttime" || name == "stoptime";
  }

  const Program *program_ = nullptr;
  AnalysisOptions options_;
  std::vector<std::unordered_map<std::string, LeakSymbol>> scopes_;
  std::unordered_set<std::string> globals_;
  std::unordered_map<std::string, FuncInfo> functions_;
  std::unordered_map<std::string, const FunctionDef *> function_defs_;
  std::unordered_map<std::string, FunctionSummary> summaries_;
  std::unordered_set<std::string> analysis_stack_;
  std::unordered_set<std::string> runtime_call_stack_;
  std::vector<LeakReport> reports_;
  std::string current_function_ = "<global>";
  SecLevel control_level_ = SecLevel::Public;
  SecLevel return_level_ = SecLevel::Public;
  bool active_summary_has_secret_control_ = false;
  bool active_summary_writes_secret_global_ = false;
};

}  // namespace

std::vector<LeakReport> DetectSideChannelLeaks(const Program &program) {
  LeakDetector detector;
  return detector.Detect(program);
}
