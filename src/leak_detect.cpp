#include "leak_detect.h"

#include <stdexcept>
#include <string>
#include <unordered_map>
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
    reports_.clear();
    functions_.clear();
    RegisterLibraryFunctions();
    RegisterFunctions(program);

    scopes_.clear();
    PushScope();
    for (const GlobalItem &item : program.items) {
      if (item.kind == GlobalItem::Kind::ConstDecl) {
        AnalyzeConstDecl(item.const_defs);
      } else if (item.kind == GlobalItem::Kind::VarDecl) {
        AnalyzeVarDecl(item.var_defs);
      }
    }
    for (const GlobalItem &item : program.items) {
      if (item.kind == GlobalItem::Kind::FuncDef) {
        AnalyzeFunction(*item.function);
      }
    }
    PopScope();
    return reports_;
  }

 private:
  void RegisterLibraryFunctions() {
    functions_.emplace("getint", FuncInfo{TypeKind::Int, true});
    functions_.emplace("getch", FuncInfo{TypeKind::Int, true});
    functions_.emplace("getarray", FuncInfo{TypeKind::Int, true});
    functions_.emplace("putint", FuncInfo{TypeKind::Void, true});
    functions_.emplace("putch", FuncInfo{TypeKind::Void, true});
    functions_.emplace("putarray", FuncInfo{TypeKind::Void, true});
    functions_.emplace("starttime", FuncInfo{TypeKind::Void, true});
    functions_.emplace("stoptime", FuncInfo{TypeKind::Void, true});
  }

  void RegisterFunctions(const Program &program) {
    for (const GlobalItem &item : program.items) {
      if (item.kind != GlobalItem::Kind::FuncDef) {
        continue;
      }
      const FunctionDef &function = *item.function;
      functions_[function.name] = FuncInfo{function.return_type, false};
    }
  }

  void PushScope() { scopes_.emplace_back(); }

  void PopScope() { scopes_.pop_back(); }

  void InsertSymbol(const std::string &name, LeakSymbol symbol) {
    scopes_.back()[name] = symbol;
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
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = it->find(name);
      if (found != it->end()) {
        found->second.level = Join(found->second.level, level);
        return;
      }
    }
    InsertSymbol(name, LeakSymbol{Join(NameLevel(name), level), false});
  }

  void Report(const std::string &kind, const std::string &message) {
    reports_.push_back(LeakReport{kind, current_function_, message});
  }

  void AnalyzeFunction(const FunctionDef &function) {
    const std::string previous_function = current_function_;
    current_function_ = function.name;
    PushScope();
    for (const Param &param : function.params) {
      InsertSymbol(param.name,
                   LeakSymbol{IsSecretName(param.name) ? SecLevel::Secret
                                                       : SecLevel::Public,
                              param.is_array});
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
        }
        AnalyzeItem(*item.then_stmt);
        if (item.else_stmt) {
          AnalyzeItem(*item.else_stmt);
        }
        break;
      }
      case BlockItem::Kind::While: {
        const SecLevel condition = AnalyzeExpr(*item.expr);
        if (IsSecret(condition)) {
          Report("secret-dependent branch",
                 "while condition depends on secret data");
        }
        AnalyzeItem(*item.body_stmt);
        break;
      }
      case BlockItem::Kind::Break:
      case BlockItem::Kind::Continue:
        break;
    }
  }

  void AnalyzeConstDecl(const std::vector<ConstDef> &defs) {
    for (const ConstDef &def : defs) {
      for (const auto &dimension : def.dimensions) {
        AnalyzeExpr(*dimension);
      }
      const SecLevel init_level = AnalyzeInit(def.init);
      InsertSymbol(def.name, LeakSymbol{Join(NameLevel(def.name), init_level),
                                        !def.dimensions.empty()});
    }
  }

  void AnalyzeVarDecl(const std::vector<VarDef> &defs) {
    for (const VarDef &def : defs) {
      for (const auto &dimension : def.dimensions) {
        AnalyzeExpr(*dimension);
      }
      SecLevel level = NameLevel(def.name);
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
    if (lval.indices.empty()) {
      SetSymbolLevel(lval.name, rhs_level);
      return;
    }
    if (IsSecret(rhs_level)) {
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
    }
    return Join(lhs, rhs);
  }

  SecLevel AnalyzeCall(const CallExpr &call) {
    SecLevel args_level = SecLevel::Public;
    bool has_secret_arg = false;
    for (const auto &arg : call.args) {
      const SecLevel arg_level = AnalyzeExpr(*arg);
      args_level = Join(args_level, arg_level);
      has_secret_arg = has_secret_arg || IsSecret(arg_level);
    }

    if (IsOutputFunction(call.name) && has_secret_arg) {
      Report("secret-dependent observable call",
             call.name + " observes secret data");
    }

    if (IsInputFunction(call.name)) {
      return SecLevel::Public;
    }

    const auto found = functions_.find(call.name);
    if (found != functions_.end() && found->second.return_type == TypeKind::Void) {
      return SecLevel::Public;
    }
    return args_level;
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

  std::vector<std::unordered_map<std::string, LeakSymbol>> scopes_;
  std::unordered_map<std::string, FuncInfo> functions_;
  std::vector<LeakReport> reports_;
  std::string current_function_ = "<global>";
};

}  // namespace

std::vector<LeakReport> DetectSideChannelLeaks(const Program &program) {
  LeakDetector detector;
  return detector.Detect(program);
}
