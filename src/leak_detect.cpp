#include "leak_detect.h"

#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

enum class SecLevel { Public, Secret };

struct TaintInfo {
  SecLevel level = SecLevel::Public;
  std::string reason;
  SourceLocation loc;
  std::string subject;
};

struct ExprTaint {
  SecLevel level = SecLevel::Public;
  std::string reason;
  SourceLocation loc;
  std::string subject;
};

struct LeakSymbol {
  TaintInfo taint;
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
  bool conservative = false;
};

struct AnalysisOptions {
  bool collect_reports = true;
};

bool IsSecret(SecLevel level) { return level == SecLevel::Secret; }

bool IsSecretName(const std::string &name) {
  return name.rfind("secret_", 0) == 0;
}

std::string LocString(SourceLocation loc) {
  return std::to_string(loc.line) + ":" + std::to_string(loc.column);
}

TaintInfo PublicTaint(SourceLocation loc = SourceLocation{}, std::string subject = "") {
  TaintInfo taint;
  taint.loc = loc;
  taint.subject = std::move(subject);
  return taint;
}

TaintInfo SecretTaint(std::string reason, SourceLocation loc, std::string subject) {
  TaintInfo taint;
  taint.level = SecLevel::Secret;
  taint.reason = std::move(reason);
  taint.loc = loc;
  taint.subject = std::move(subject);
  return taint;
}

TaintInfo Join(TaintInfo lhs, const TaintInfo &rhs) {
  if (IsSecret(lhs.level)) {
    return lhs;
  }
  if (IsSecret(rhs.level)) {
    return rhs;
  }
  return lhs;
}

ExprTaint ToExpr(TaintInfo taint) {
  return ExprTaint{taint.level, taint.reason, taint.loc, taint.subject};
}

TaintInfo ToTaint(const ExprTaint &taint) {
  return TaintInfo{taint.level, taint.reason, taint.loc, taint.subject};
}

std::string CallSubject(const CallExpr &call, const std::vector<ExprTaint> &args) {
  std::string text = call.name + "(";
  for (size_t i = 0; i < args.size(); ++i) {
    if (i != 0) {
      text += ", ";
    }
    text += args[i].subject.empty() ? "?" : args[i].subject;
  }
  text += ")";
  return text;
}

class LeakDetector {
 public:
  std::vector<LeakReport> Detect(const Program &program) {
    program_ = &program;
    reports_.clear();
    warning_keys_.clear();
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
    control_taint_ = PublicTaint();
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
    const auto found = function_defs_.find(name);
    if (found == function_defs_.end()) {
      return ConservativeSummary(name);
    }
    if (analysis_stack_.count(name) != 0) {
      FunctionSummary summary = MakeConservativeSummary(*found->second);
      summaries_[name] = summary;
      return summaries_.at(name);
    }

    analysis_stack_.insert(name);
    const FunctionDef &function = *found->second;
    FunctionSummary summary;
    summary.return_type = function.return_type;
    summary.return_depends_on_param.assign(function.params.size(), false);
    summary.returns_secret_by_default = IsSecret(AnalyzeFunctionReturn(function, -1).level);
    for (size_t i = 0; i < function.params.size(); ++i) {
      summary.return_depends_on_param[i] =
          IsSecret(AnalyzeFunctionReturn(function, static_cast<int>(i)).level);
    }
    analysis_stack_.erase(name);
    summaries_[name] = summary;
    return summaries_.at(name);
  }

  const FunctionSummary &ConservativeSummary(const std::string &name) {
    const auto existing = summaries_.find(name);
    if (existing != summaries_.end()) {
      return existing->second;
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

  TaintInfo AnalyzeFunctionReturn(const FunctionDef &function, int secret_param) {
    const auto saved_scopes = scopes_;
    const auto saved_globals = globals_;
    const std::string saved_function = current_function_;
    const TaintInfo saved_control = control_taint_;
    const TaintInfo saved_return = return_taint_;

    scopes_.clear();
    globals_.clear();
    control_taint_ = PublicTaint();
    return_taint_ = PublicTaint(function.loc, function.name);
    current_function_ = function.name;
    PushScope();
    if (program_ != nullptr) {
      AnalyzeGlobals(*program_);
    }
    PushScope();
    for (size_t i = 0; i < function.params.size(); ++i) {
      const Param &param = function.params[i];
      InsertSymbol(param.name, LeakSymbol{InitialTaint(param.name, param.loc), param.is_array});
      if (static_cast<int>(i) == secret_param) {
        SetSymbolTaint(param.name, SecretTaint(param.name + " is secret because it is the selected summary parameter at " + LocString(param.loc), param.loc, param.name));
      }
      for (const auto &dimension : param.dimensions) {
        AnalyzeExpr(*dimension);
      }
    }
    AnalyzeBlock(*function.block);
    PopScope();
    PopScope();

    const TaintInfo result = return_taint_;
    scopes_ = saved_scopes;
    globals_ = saved_globals;
    current_function_ = saved_function;
    control_taint_ = saved_control;
    return_taint_ = saved_return;
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

  TaintInfo InitialTaint(const std::string &name, SourceLocation loc) const {
    if (IsSecretName(name)) {
      return SecretTaint(name + " is secret because its name starts with secret_ at " + LocString(loc), loc, name);
    }
    return PublicTaint(loc, name);
  }

  void PushScope() { scopes_.emplace_back(); }
  void PopScope() { scopes_.pop_back(); }

  void InsertSymbol(const std::string &name, LeakSymbol symbol) {
    scopes_.back()[name] = std::move(symbol);
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
    return LeakSymbol{InitialTaint(name, SourceLocation{}), false};
  }

  void SetSymbolTaint(const std::string &name, TaintInfo taint) {
    if (IsSecretName(name)) {
      taint = Join(InitialTaint(name, taint.loc), taint);
    }
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = it->find(name);
      if (found != it->end()) {
        found->second.taint = Join(found->second.taint, taint);
        return;
      }
    }
    InsertSymbol(name, LeakSymbol{std::move(taint), false});
  }

  void Report(LeakKind kind, Severity severity, SourceLocation loc,
              const std::string &subject, const std::string &message,
              const std::string &reason = "") {
    if (!options_.collect_reports) {
      return;
    }
    std::ostringstream key;
    key << static_cast<int>(kind) << '|' << current_function_ << '|' << loc.line
        << '|' << loc.column << '|' << subject;
    if (!warning_keys_.insert(key.str()).second) {
      return;
    }
    reports_.push_back(LeakReport{kind, severity, current_function_, loc,
                                  subject, message, reason});
  }

  void AnalyzeFunction(const FunctionDef &function,
                       const std::vector<TaintInfo> *param_override) {
    const std::string previous_function = current_function_;
    current_function_ = function.name;
    PushScope();
    for (size_t i = 0; i < function.params.size(); ++i) {
      const Param &param = function.params[i];
      TaintInfo taint = InitialTaint(param.name, param.loc);
      if (param_override != nullptr && i < param_override->size()) {
        taint = Join(taint, (*param_override)[i]);
      }
      InsertSymbol(param.name, LeakSymbol{taint, param.is_array});
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
        AnalyzeAssign(*item.lval, *item.expr, item.loc);
        break;
      case BlockItem::Kind::Return:
        if (item.expr) {
          const ExprTaint value = AnalyzeExpr(*item.expr);
          return_taint_ = Join(return_taint_, Join(ToTaint(value), control_taint_));
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
        const ExprTaint condition = AnalyzeExpr(*item.expr);
        if (IsSecret(condition.level)) {
          Report(LeakKind::SecretBranch, Severity::Medium, item.loc, "if",
                 "condition uses " + condition.subject, condition.reason);
        }
        WithControl(Join(control_taint_, ToTaint(condition)), [&]() {
          AnalyzeItem(*item.then_stmt);
        });
        if (item.else_stmt) {
          WithControl(Join(control_taint_, ToTaint(condition)), [&]() {
            AnalyzeItem(*item.else_stmt);
          });
        }
        break;
      }
      case BlockItem::Kind::While: {
        const ExprTaint condition = AnalyzeExpr(*item.expr);
        if (IsSecret(condition.level)) {
          Report(LeakKind::SecretLoopBound, Severity::Medium, item.loc, "while",
                 "while condition uses " + condition.subject, condition.reason);
        }
        WithControl(Join(control_taint_, ToTaint(condition)), [&]() {
          AnalyzeItem(*item.body_stmt);
        });
        break;
      }
      case BlockItem::Kind::Break:
        if (IsSecret(control_taint_.level)) {
          Report(LeakKind::SecretBreak, Severity::Medium, item.loc, "break",
                 "break executes under secret control", control_taint_.reason);
        }
        break;
      case BlockItem::Kind::Continue:
        if (IsSecret(control_taint_.level)) {
          Report(LeakKind::SecretContinue, Severity::Medium, item.loc, "continue",
                 "continue executes under secret control", control_taint_.reason);
        }
        break;
    }
  }

  template <typename Fn>
  void WithControl(TaintInfo taint, Fn fn) {
    const TaintInfo saved = control_taint_;
    control_taint_ = taint;
    fn();
    control_taint_ = saved;
  }

  void AnalyzeConstDecl(const std::vector<ConstDef> &defs) {
    for (const ConstDef &def : defs) {
      for (const auto &dimension : def.dimensions) {
        AnalyzeExpr(*dimension);
      }
      TaintInfo taint = InitialTaint(def.name, def.loc);
      taint = Join(taint, AnalyzeInit(def.init));
      taint = Join(taint, control_taint_);
      InsertSymbol(def.name, LeakSymbol{taint, !def.dimensions.empty()});
    }
  }

  void AnalyzeVarDecl(const std::vector<VarDef> &defs) {
    for (const VarDef &def : defs) {
      for (const auto &dimension : def.dimensions) {
        AnalyzeExpr(*dimension);
      }
      TaintInfo taint = InitialTaint(def.name, def.loc);
      if (def.has_init) {
        TaintInfo init = AnalyzeInit(def.init);
        if (IsSecret(init.level) && !IsSecret(taint.level)) {
          init.reason = def.name + " is secret because it was assigned from " + init.subject + " at " + LocString(def.loc);
          init.subject = def.name;
          init.loc = def.loc;
        }
        taint = Join(taint, init);
      }
      if (IsSecret(control_taint_.level) && !IsSecret(taint.level)) {
        taint = SecretTaint(def.name + " is secret because it was assigned under secret control at " + LocString(def.loc), def.loc, def.name);
      }
      InsertSymbol(def.name, LeakSymbol{taint, !def.dimensions.empty()});
    }
  }

  TaintInfo AnalyzeInit(const InitVal &init) {
    if (!init.is_list) {
      return init.expr ? ToTaint(AnalyzeExpr(*init.expr)) : PublicTaint();
    }
    TaintInfo taint = PublicTaint();
    for (const InitVal &child : init.list) {
      taint = Join(taint, AnalyzeInit(child));
    }
    return taint;
  }

  void AnalyzeAssign(const LValExpr &lval, const Expr &rhs, SourceLocation loc) {
    const ExprTaint rhs_taint = AnalyzeExpr(rhs);
    const ExprTaint index_taint = AnalyzeLValIndices(lval);
    if (IsSecret(index_taint.level)) {
      Report(LeakKind::SecretArrayIndex, Severity::High, lval.loc, lval.name,
             "write to " + lval.name + " uses a secret index", index_taint.reason);
    }
    TaintInfo assigned = Join(ToTaint(rhs_taint), control_taint_);
    if (lval.indices.empty()) {
      if (IsSecret(control_taint_.level)) {
        assigned = SecretTaint(lval.name + " is secret because it was assigned under secret control at " + LocString(loc), loc, lval.name);
      } else if (IsSecret(rhs_taint.level)) {
        assigned.reason = lval.name + " is secret because it was assigned from " + rhs_taint.subject + " at " + LocString(loc);
        assigned.subject = lval.name;
        assigned.loc = loc;
      }
      SetSymbolTaint(lval.name, assigned);
      return;
    }
    if (IsSecret(assigned.level)) {
      SetSymbolTaint(lval.name, SecretTaint(lval.name + " is secret because a secret value was written into one of its elements at " + LocString(loc), loc, lval.name));
    }
  }

  ExprTaint AnalyzeExpr(const Expr &expr) {
    if (const auto *number = dynamic_cast<const NumberExpr *>(&expr)) {
      (void)number;
      return ExprTaint{SecLevel::Public, "", expr.loc, "constant"};
    }

    if (const auto *lval = dynamic_cast<const LValExpr *>(&expr)) {
      const ExprTaint index_taint = AnalyzeLValIndices(*lval);
      if (IsSecret(index_taint.level)) {
        Report(LeakKind::SecretArrayIndex, Severity::High, lval->loc, lval->name,
               "read from " + lval->name + " uses a secret index", index_taint.reason);
      }
      return ToExpr(LookupSymbol(lval->name).taint);
    }

    if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
      return AnalyzeCall(*call);
    }

    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
      ExprTaint operand = AnalyzeExpr(*unary->operand);
      operand.loc = expr.loc;
      return operand;
    }

    const auto *binary = dynamic_cast<const BinaryExpr *>(&expr);
    if (binary == nullptr) {
      throw std::runtime_error("unknown expression node in leak detector");
    }

    ExprTaint lhs = AnalyzeExpr(*binary->lhs);
    ExprTaint rhs = AnalyzeExpr(*binary->rhs);
    ExprTaint result = IsSecret(lhs.level) ? lhs : rhs;
    result.loc = expr.loc;
    if (!IsSecret(result.level)) {
      result.subject = "expression";
    }
    if ((binary->op == BinaryOp::And || binary->op == BinaryOp::Or) &&
        (IsSecret(lhs.level) || IsSecret(rhs.level))) {
      const std::string op = binary->op == BinaryOp::And ? "&&" : "||";
      Report(LeakKind::SecretShortCircuit, Severity::Medium, expr.loc, op,
             "logical operator " + op + " depends on secret data", result.reason);
    }
    return result;
  }

  ExprTaint AnalyzeCall(const CallExpr &call) {
    std::vector<ExprTaint> arg_taints;
    arg_taints.reserve(call.args.size());
    bool has_secret_arg = false;
    TaintInfo secret_arg;
    for (const auto &arg : call.args) {
      ExprTaint arg_taint = AnalyzeExpr(*arg);
      arg_taints.push_back(arg_taint);
      if (IsSecret(arg_taint.level) && !has_secret_arg) {
        has_secret_arg = true;
        secret_arg = ToTaint(arg_taint);
      }
    }

    if (IsOutputFunction(call.name)) {
      if (has_secret_arg) {
        Report(LeakKind::SecretObservableCall, Severity::High, call.loc, call.name,
               call.name + " observes secret data", secret_arg.reason);
      }
      if (IsSecret(control_taint_.level)) {
        Report(LeakKind::SecretObservableCall, Severity::Medium, call.loc, call.name,
               call.name + " executes under secret control", control_taint_.reason);
      }
      return ExprTaint{SecLevel::Public, "", call.loc, call.name};
    }

    if (IsTimingFunction(call.name)) {
      if (IsSecret(control_taint_.level)) {
        Report(LeakKind::SecretTimingCall, Severity::Medium, call.loc, call.name,
               call.name + " executes under secret control", control_taint_.reason);
      }
      return ExprTaint{SecLevel::Public, "", call.loc, call.name};
    }

    if (IsInputFunction(call.name)) {
      return ExprTaint{SecLevel::Public, "", call.loc, call.name + "()"};
    }

    const auto user_function = function_defs_.find(call.name);
    if (user_function != function_defs_.end() && options_.collect_reports &&
        runtime_call_stack_.count(call.name) == 0) {
      return AnalyzeUserFunctionCall(*user_function->second, call, arg_taints);
    }

    const FunctionSummary *summary = EnsureSummaryIfUserFunction(call.name);
    if (summary == nullptr || summary->conservative) {
      if (has_secret_arg) {
        TaintInfo taint = secret_arg;
        taint.reason = CallSubject(call, arg_taints) + " is secret because it was called with " + secret_arg.subject + " at " + LocString(call.loc);
        taint.subject = CallSubject(call, arg_taints);
        taint.loc = call.loc;
        return ToExpr(taint);
      }
      return ExprTaint{SecLevel::Public, "", call.loc, CallSubject(call, arg_taints)};
    }

    bool secret_return = summary->returns_secret_by_default;
    for (size_t i = 0; i < arg_taints.size() && i < summary->return_depends_on_param.size(); ++i) {
      if (IsSecret(arg_taints[i].level) && summary->return_depends_on_param[i]) {
        secret_return = true;
        secret_arg = ToTaint(arg_taints[i]);
        break;
      }
    }
    if (secret_return) {
      const std::string subject = CallSubject(call, arg_taints);
      return ExprTaint{SecLevel::Secret,
                       subject + " is secret because it depends on " + secret_arg.subject + " at " + LocString(call.loc),
                       call.loc, subject};
    }
    return ExprTaint{SecLevel::Public, "", call.loc, CallSubject(call, arg_taints)};
  }

  ExprTaint AnalyzeUserFunctionCall(const FunctionDef &function, const CallExpr &call,
                                    const std::vector<ExprTaint> &arg_taints) {
    runtime_call_stack_.insert(function.name);
    const std::string saved_function = current_function_;
    const TaintInfo saved_return = return_taint_;

    current_function_ = function.name;
    return_taint_ = PublicTaint(function.loc, function.name);
    PushScope();
    for (size_t i = 0; i < function.params.size(); ++i) {
      const Param &param = function.params[i];
      TaintInfo taint = InitialTaint(param.name, param.loc);
      if (i < arg_taints.size()) {
        taint = Join(taint, ToTaint(arg_taints[i]));
      }
      InsertSymbol(param.name, LeakSymbol{taint, param.is_array});
      for (const auto &dimension : param.dimensions) {
        AnalyzeExpr(*dimension);
      }
    }
    AnalyzeBlock(*function.block);
    PopScope();

    TaintInfo result = return_taint_;
    return_taint_ = saved_return;
    current_function_ = saved_function;
    runtime_call_stack_.erase(function.name);
    if (IsSecret(result.level)) {
      const std::string subject = CallSubject(call, arg_taints);
      result.reason = subject + " is secret because it depends on " + result.subject + " at " + LocString(call.loc);
      result.subject = subject;
      result.loc = call.loc;
    } else {
      result.subject = CallSubject(call, arg_taints);
      result.loc = call.loc;
    }
    return ToExpr(result);
  }

  const FunctionSummary *EnsureSummaryIfUserFunction(const std::string &name) {
    const auto found = function_defs_.find(name);
    if (found == function_defs_.end()) {
      return nullptr;
    }
    return &EnsureSummary(name);
  }

  ExprTaint AnalyzeLValIndices(const LValExpr &lval) {
    ExprTaint result{SecLevel::Public, "", lval.loc, lval.name};
    for (const auto &index : lval.indices) {
      ExprTaint index_taint = AnalyzeExpr(*index);
      if (IsSecret(index_taint.level)) {
        return index_taint;
      }
    }
    return result;
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
  std::unordered_set<std::string> warning_keys_;
  std::vector<LeakReport> reports_;
  std::string current_function_ = "<global>";
  TaintInfo control_taint_;
  TaintInfo return_taint_;
};

}  // namespace

std::string LeakKindName(LeakKind kind) {
  switch (kind) {
    case LeakKind::SecretBranch: return "secret-dependent branch";
    case LeakKind::SecretLoopBound: return "secret-dependent loop bound";
    case LeakKind::SecretBreak: return "secret-dependent break";
    case LeakKind::SecretContinue: return "secret-dependent continue";
    case LeakKind::SecretArrayIndex: return "secret-dependent array index";
    case LeakKind::SecretShortCircuit: return "secret-dependent short-circuit";
    case LeakKind::SecretObservableCall: return "secret-dependent observable call";
    case LeakKind::SecretTimingCall: return "secret-dependent timing call";
  }
  return "unknown";
}

std::string SeverityName(Severity severity) {
  switch (severity) {
    case Severity::High: return "high";
    case Severity::Medium: return "medium";
    case Severity::Info: return "info";
  }
  return "unknown";
}

std::vector<LeakReport> DetectSideChannelLeaks(const Program &program) {
  LeakDetector detector;
  return detector.Detect(program);
}
