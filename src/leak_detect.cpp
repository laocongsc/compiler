#include "leak_detect.h"

#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <optional>
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

struct ArrayTaint {
  bool whole_array_secret = false;
  TaintInfo whole_array_reason;
  std::unordered_map<int, TaintInfo> element_taint;
};

struct LeakSymbol {
  TaintInfo taint;
  bool is_array = false;
  bool declared_secret = false;
  TaintInfo declared_secret_taint;
  std::string alias_to;
  ArrayTaint array_taint;
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

std::string LValSubjectWithBase(const LValExpr &lval, const std::string &base) {
  std::string subject = base;
  for (const auto &index : lval.indices) {
    if (const auto *number = dynamic_cast<const NumberExpr *>(index.get())) {
      subject += "[" + std::to_string(number->value) + "]";
    } else {
      subject += "[?]";
    }
  }
  return subject;
}

std::optional<int> ConstIndex(const LValExpr &lval) {
  if (lval.indices.size() != 1) {
    return std::nullopt;
  }
  if (const auto *number = dynamic_cast<const NumberExpr *>(lval.indices[0].get())) {
    return number->value;
  }
  return std::nullopt;
}

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

LeakSymbol MakeSymbol(TaintInfo taint, bool is_array, bool declared_secret = false,
                      TaintInfo declared_secret_taint = TaintInfo{}) {
  LeakSymbol symbol;
  symbol.taint = std::move(taint);
  symbol.is_array = is_array;
  symbol.declared_secret = declared_secret;
  symbol.declared_secret_taint = std::move(declared_secret_taint);
  if (declared_secret && is_array && symbol.declared_secret_taint.level == SecLevel::Secret) {
    symbol.array_taint.whole_array_secret = true;
    symbol.array_taint.whole_array_reason = symbol.declared_secret_taint;
  }
  return symbol;
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
      const bool declared_secret = param.is_secret || IsSecretName(param.name);
      TaintInfo declared_taint = InitialTaint(param.name, param.loc, param.is_secret);
      InsertSymbol(param.name, MakeSymbol(declared_taint, param.is_array,
                                          declared_secret, declared_taint));
      if (static_cast<int>(i) == secret_param) {
        AssignSymbolTaint(param.name, SecretTaint(param.name + " is secret because it is the selected summary parameter at " + LocString(param.loc), param.loc, param.name));
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

  TaintInfo InitialTaint(const std::string &name, SourceLocation loc,
                         bool explicit_secret = false) const {
    if (explicit_secret) {
      return SecretTaint(name + " is secret because it is declared secret at " +
                             LocString(loc),
                         loc, name);
    }
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

  LeakSymbol *FindSymbol(const std::string &name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = it->find(name);
      if (found != it->end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  const LeakSymbol *FindSymbol(const std::string &name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = it->find(name);
      if (found != it->end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  std::string ResolveAlias(const std::string &name) const {
    const LeakSymbol *symbol = FindSymbol(name);
    if (symbol != nullptr && !symbol->alias_to.empty()) {
      return symbol->alias_to;
    }
    return name;
  }

  std::string ResolvedLValSubject(const LValExpr &lval) const {
    return LValSubjectWithBase(lval, ResolveAlias(lval.name));
  }

  LeakSymbol LookupSymbol(const std::string &name) const {
    const std::string resolved = ResolveAlias(name);
    if (const LeakSymbol *symbol = FindSymbol(resolved)) {
      return *symbol;
    }
    return MakeSymbol(InitialTaint(resolved, SourceLocation{}), false);
  }

  void AssignSymbolTaint(const std::string &name, TaintInfo taint) {
    const std::string resolved = ResolveAlias(name);
    if (LeakSymbol *symbol = FindSymbol(resolved)) {
      if (symbol->declared_secret) {
        if (IsSecret(taint.level) && !taint.reason.empty()) {
          TaintInfo combined = symbol->declared_secret_taint;
          combined.reason += "; " + taint.reason;
          combined.subject = resolved;
          combined.loc = symbol->declared_secret_taint.loc;
          taint = std::move(combined);
        } else {
          taint = Join(symbol->declared_secret_taint, taint);
        }
      } else if (IsSecretName(resolved)) {
        taint = Join(InitialTaint(resolved, taint.loc), taint);
      }
      symbol->taint = std::move(taint);
      if (!symbol->declared_secret && !IsSecret(symbol->taint.level)) {
        symbol->array_taint.whole_array_secret = false;
        symbol->array_taint.element_taint.clear();
      }
      return;
    }
    const bool declared_secret = IsSecretName(resolved);
    TaintInfo declared_taint = InitialTaint(resolved, taint.loc);
    if (declared_secret) {
      taint = Join(declared_taint, taint);
    }
    InsertSymbol(resolved, MakeSymbol(std::move(taint), false, declared_secret,
                                      std::move(declared_taint)));
  }

  void SetArrayElementTaint(const std::string &name, int index, TaintInfo taint) {
    const std::string resolved = ResolveAlias(name);
    LeakSymbol *symbol = FindSymbol(resolved);
    if (symbol == nullptr) {
      const bool declared_secret = IsSecretName(resolved);
      TaintInfo declared_taint = InitialTaint(resolved, taint.loc);
      InsertSymbol(resolved, MakeSymbol(PublicTaint(taint.loc, resolved), true,
                                        declared_secret, std::move(declared_taint)));
      symbol = FindSymbol(resolved);
    }
    symbol->is_array = true;
    if (IsSecret(taint.level)) {
      symbol->array_taint.element_taint[index] = std::move(taint);
    } else if (!symbol->declared_secret) {
      symbol->array_taint.element_taint.erase(index);
    }
  }

  void SetArrayWholeTaint(const std::string &name, TaintInfo taint) {
    const std::string resolved = ResolveAlias(name);
    LeakSymbol *symbol = FindSymbol(resolved);
    if (symbol == nullptr) {
      const bool declared_secret = IsSecretName(resolved);
      TaintInfo declared_taint = InitialTaint(resolved, taint.loc);
      InsertSymbol(resolved, MakeSymbol(PublicTaint(taint.loc, resolved), true,
                                        declared_secret, std::move(declared_taint)));
      symbol = FindSymbol(resolved);
    }
    symbol->is_array = true;
    if (IsSecret(taint.level)) {
      symbol->array_taint.whole_array_secret = true;
      symbol->array_taint.whole_array_reason = std::move(taint);
    }
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
      const bool declared_secret = param.is_secret || IsSecretName(param.name);
      TaintInfo declared_taint = InitialTaint(param.name, param.loc, param.is_secret);
      TaintInfo taint = declared_taint;
      if (param_override != nullptr && i < param_override->size()) {
        taint = Join(taint, (*param_override)[i]);
      }
      InsertSymbol(param.name, MakeSymbol(taint, param.is_array, declared_secret,
                                          declared_taint));
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
      const bool declared_secret = def.is_secret || IsSecretName(def.name);
      TaintInfo declared_taint = InitialTaint(def.name, def.loc, def.is_secret);
      TaintInfo taint = Join(declared_taint, AnalyzeInit(def.init));
      taint = Join(taint, control_taint_);
      InsertSymbol(def.name, MakeSymbol(taint, !def.dimensions.empty(),
                                        declared_secret, declared_taint));
      if (!def.dimensions.empty() && IsSecret(taint.level)) {
        SetArrayWholeTaint(def.name, taint);
      }
    }
  }

  void AnalyzeVarDecl(const std::vector<VarDef> &defs) {
    for (const VarDef &def : defs) {
      for (const auto &dimension : def.dimensions) {
        AnalyzeExpr(*dimension);
      }
      const bool declared_secret = def.is_secret || IsSecretName(def.name);
      TaintInfo declared_taint = InitialTaint(def.name, def.loc, def.is_secret);
      TaintInfo taint = declared_taint;
      if (def.has_init) {
        TaintInfo init = AnalyzeInit(def.init);
        if (IsSecret(init.level) && !IsSecret(taint.level)) {
          std::string source_reason = init.reason;
          init.reason = def.name + " is secret because it was assigned from " + init.subject + " at " + LocString(def.loc);
          if (!source_reason.empty()) {
            init.reason += "; " + source_reason;
          }
          init.subject = def.name;
          init.loc = def.loc;
        }
        taint = Join(taint, init);
      }
      if (IsSecret(control_taint_.level) && !IsSecret(taint.level)) {
        taint = SecretTaint(def.name + " is secret because it was assigned under secret control at " + LocString(def.loc), def.loc, def.name);
      }
      InsertSymbol(def.name, MakeSymbol(taint, !def.dimensions.empty(),
                                        declared_secret, declared_taint));
      if (!def.dimensions.empty() && IsSecret(taint.level)) {
        SetArrayWholeTaint(def.name, taint);
      }
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
             "write to " + ResolvedLValSubject(lval) + " uses a secret index", index_taint.reason);
    }

    TaintInfo assigned = Join(ToTaint(rhs_taint), control_taint_);
    if (lval.indices.empty()) {
      if (IsSecret(control_taint_.level)) {
        assigned = SecretTaint(lval.name + " is secret because it was assigned under secret control at " + LocString(loc), loc, lval.name);
      } else if (IsSecret(rhs_taint.level)) {
        assigned.reason = lval.name + " is secret because it was assigned from " + rhs_taint.subject + " at " + LocString(loc);
        if (!rhs_taint.reason.empty()) {
          assigned.reason += "; " + rhs_taint.reason;
        }
        assigned.subject = lval.name;
        assigned.loc = loc;
      }
      AssignSymbolTaint(lval.name, assigned);
      return;
    }

    const std::string subject = ResolvedLValSubject(lval);
    const std::optional<int> index = ConstIndex(lval);
    if (IsSecret(assigned.level)) {
      TaintInfo element_taint = assigned;
      element_taint.reason = subject + " is secret because it was assigned from " +
                             (rhs_taint.subject.empty() ? assigned.subject : rhs_taint.subject) +
                             " at " + LocString(loc);
      if (!rhs_taint.reason.empty()) {
        element_taint.reason += "; " + rhs_taint.reason;
      }
      if (IsSecret(control_taint_.level) && !IsSecret(rhs_taint.level)) {
        element_taint.reason = subject + " is secret because it was assigned under secret control at " + LocString(loc);
      }
      element_taint.subject = subject;
      element_taint.loc = loc;
      if (index.has_value()) {
        SetArrayElementTaint(lval.name, *index, element_taint);
      } else {
        SetArrayWholeTaint(lval.name, element_taint);
      }
    } else if (index.has_value()) {
      SetArrayElementTaint(lval.name, *index, PublicTaint(loc, subject));
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
               "read from " + ResolvedLValSubject(*lval) + " uses a secret index", index_taint.reason);
      }
      return AnalyzeLValRead(*lval);
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
    if (IsSecret(lhs.level) && IsSecret(rhs.level)) {
      result.subject = lhs.subject + " and " + rhs.subject;
      result.reason = lhs.reason;
      if (!rhs.reason.empty()) {
        if (!result.reason.empty()) {
          result.reason += "; ";
        }
        result.reason += rhs.reason;
      }
    }
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
               call.name + " observes " + secret_arg.subject, secret_arg.reason);
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
      const std::string source = secret_arg.subject.empty() ?
                                     "declared secret" : secret_arg.subject;
      return ExprTaint{SecLevel::Secret,
                       subject + " is secret because it depends on " + source + " at " + LocString(call.loc),
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
      const bool declared_secret = param.is_secret || IsSecretName(param.name);
      TaintInfo declared_taint = InitialTaint(param.name, param.loc, param.is_secret);
      TaintInfo taint = declared_taint;
      LeakSymbol symbol = MakeSymbol(taint, param.is_array, declared_secret,
                                     declared_taint);
      if (i < arg_taints.size()) {
        taint = Join(taint, ToTaint(arg_taints[i]));
        symbol.taint = taint;
        if (param.is_array && !symbol.declared_secret) {
          if (const auto *actual = dynamic_cast<const LValExpr *>(call.args[i].get())) {
            if (actual->indices.empty()) {
              symbol.alias_to = ResolveAlias(actual->name);
            }
          }
        }
      }
      InsertSymbol(param.name, std::move(symbol));
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
      const std::string source_reason = result.reason;
      result.reason = subject + " is secret because it depends on " + result.subject + " at " + LocString(call.loc);
      if (!source_reason.empty()) {
        result.reason += "; " + source_reason;
      }
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

  ExprTaint AnalyzeLValRead(const LValExpr &lval) {
    const LeakSymbol symbol = LookupSymbol(lval.name);
    if (lval.indices.empty()) {
      TaintInfo taint = symbol.taint;
      taint.subject = lval.name;
      taint.loc = lval.loc;
      return ToExpr(taint);
    }

    const std::string subject = ResolvedLValSubject(lval);
    if (symbol.array_taint.whole_array_secret) {
      TaintInfo taint = symbol.array_taint.whole_array_reason;
      taint.subject = subject;
      taint.loc = lval.loc;
      return ToExpr(taint);
    }
    const std::optional<int> index = ConstIndex(lval);
    if (index.has_value()) {
      const auto found = symbol.array_taint.element_taint.find(*index);
      if (found != symbol.array_taint.element_taint.end()) {
        TaintInfo taint = found->second;
        taint.subject = subject;
        taint.loc = lval.loc;
        return ToExpr(taint);
      }
      return ExprTaint{SecLevel::Public, "", lval.loc, subject};
    }
    TaintInfo taint = symbol.taint;
    taint.subject = subject;
    taint.loc = lval.loc;
    return ToExpr(taint);
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
