#include "ct_rewrite.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct SupportInfo {
  bool supported = false;
  std::string reason;
};

struct ArrayInfo {
  bool known = false;
  bool is_param = false;
  std::vector<int> dims;
};

enum class ExprContext { Other, DirectAssignRhs, DirectReturn, DirectVarInit, FunctionArg };

std::optional<int> ConstNumber(const Expr &expr) {
  if (const auto *number = dynamic_cast<const NumberExpr *>(&expr)) {
    return number->value;
  }
  return std::nullopt;
}

std::vector<int> ConstDims(const std::vector<std::unique_ptr<Expr>> &dims) {
  std::vector<int> result;
  for (const auto &dim : dims) {
    if (const auto value = ConstNumber(*dim)) {
      result.push_back(*value);
    } else {
      result.push_back(-1);
    }
  }
  return result;
}

const BlockItem *SingleInnerItem(const BlockItem &item) {
  if (item.kind == BlockItem::Kind::Block) {
    if (item.block == nullptr || item.block->items.size() != 1) {
      return nullptr;
    }
    return &item.block->items.front();
  }
  return &item;
}

const BlockItem *SingleAssignItem(const BlockItem &item) {
  const BlockItem *inner = SingleInnerItem(item);
  if (inner == nullptr || inner->kind != BlockItem::Kind::Assign ||
      inner->lval == nullptr || inner->expr == nullptr) {
    return nullptr;
  }
  return inner;
}

bool SameScalarLValue(const LValExpr &lhs, const LValExpr &rhs) {
  return lhs.name == rhs.name && lhs.indices.empty() && rhs.indices.empty();
}

bool ContainsCall(const Expr &expr) {
  if (dynamic_cast<const CallExpr *>(&expr) != nullptr) {
    return true;
  }
  if (const auto *lval = dynamic_cast<const LValExpr *>(&expr)) {
    for (const auto &index : lval->indices) {
      if (ContainsCall(*index)) {
        return true;
      }
    }
    return false;
  }
  if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
    return ContainsCall(*unary->operand);
  }
  if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expr)) {
    return ContainsCall(*binary->lhs) || ContainsCall(*binary->rhs);
  }
  return false;
}

bool ContainsArrayAccess(const Expr &expr) {
  if (const auto *lval = dynamic_cast<const LValExpr *>(&expr)) {
    if (!lval->indices.empty()) {
      return true;
    }
    return false;
  }
  if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
    for (const auto &arg : call->args) {
      if (ContainsArrayAccess(*arg)) {
        return true;
      }
    }
    return false;
  }
  if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
    return ContainsArrayAccess(*unary->operand);
  }
  if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expr)) {
    return ContainsArrayAccess(*binary->lhs) || ContainsArrayAccess(*binary->rhs);
  }
  return false;
}

bool ContainsShortCircuit(const Expr &expr) {
  if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expr)) {
    if (binary->op == BinaryOp::And || binary->op == BinaryOp::Or) {
      return true;
    }
    return ContainsShortCircuit(*binary->lhs) || ContainsShortCircuit(*binary->rhs);
  }
  if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
    return ContainsShortCircuit(*unary->operand);
  }
  if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
    for (const auto &arg : call->args) {
      if (ContainsShortCircuit(*arg)) {
        return true;
      }
    }
  }
  if (const auto *lval = dynamic_cast<const LValExpr *>(&expr)) {
    for (const auto &index : lval->indices) {
      if (ContainsShortCircuit(*index)) {
        return true;
      }
    }
  }
  return false;
}

bool ContainsDivOrMod(const Expr &expr) {
  if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expr)) {
    if (binary->op == BinaryOp::Div || binary->op == BinaryOp::Mod) {
      return true;
    }
    return ContainsDivOrMod(*binary->lhs) || ContainsDivOrMod(*binary->rhs);
  }
  if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
    return ContainsDivOrMod(*unary->operand);
  }
  if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
    for (const auto &arg : call->args) {
      if (ContainsDivOrMod(*arg)) {
        return true;
      }
    }
  }
  if (const auto *lval = dynamic_cast<const LValExpr *>(&expr)) {
    for (const auto &index : lval->indices) {
      if (ContainsDivOrMod(*index)) {
        return true;
      }
    }
  }
  return false;
}

SupportInfo IsSupportedConditionalAssign(const BlockItem &item) {
  if (item.kind != BlockItem::Kind::If || item.expr == nullptr) {
    return {false, "warning is not a conditional assignment"};
  }
  if (item.else_stmt == nullptr) {
    return {false, "missing else"};
  }
  if (ContainsCall(*item.expr)) {
    return {false, "condition contains function call"};
  }

  const BlockItem *then_assign = SingleAssignItem(*item.then_stmt);
  const BlockItem *else_assign = SingleAssignItem(*item.else_stmt);
  if (then_assign == nullptr || else_assign == nullptr) {
    return {false, "then/else must each contain a single assignment"};
  }
  if (then_assign->lval->name == else_assign->lval->name &&
      (!then_assign->lval->indices.empty() || !else_assign->lval->indices.empty())) {
    return {false, "array element assignment is unsupported"};
  }
  if (!SameScalarLValue(*then_assign->lval, *else_assign->lval)) {
    return {false, "then/else assignments must target the same scalar variable"};
  }
  if (ContainsCall(*then_assign->expr) || ContainsCall(*else_assign->expr)) {
    return {false, "candidate expression contains function call"};
  }
  if (ContainsArrayAccess(*then_assign->expr) || ContainsArrayAccess(*else_assign->expr)) {
    return {false, "candidate expression contains array access"};
  }
  if (ContainsShortCircuit(*then_assign->expr) || ContainsShortCircuit(*else_assign->expr)) {
    return {false, "candidate expression contains short-circuit"};
  }
  return {true, "conditional assignment can be converted to mask-based select"};
}

SupportInfo IsSupportedLogicExpr(const BinaryExpr &expr) {
  if (ContainsCall(expr)) {
    return {false, "function call in short-circuit expression"};
  }
  if (ContainsArrayAccess(expr)) {
    return {false, "array access in short-circuit expression"};
  }
  if (ContainsDivOrMod(expr)) {
    return {false, "division in short-circuit expression"};
  }
  return {true, "pure short-circuit expression can be evaluated without branching"};
}

class RewriteIndexBuilder {
 public:
  void Build(const Program &program) {
    scopes_.clear();
    if_index.clear();
    array_index.clear();
    logic_index.clear();
    PushScope();
    for (const GlobalItem &item : program.items) {
      if (item.kind == GlobalItem::Kind::ConstDecl) {
        RegisterConstDecl(item.const_defs, false);
      } else if (item.kind == GlobalItem::Kind::VarDecl) {
        RegisterVarDecl(item.var_defs, false);
      }
    }
    for (const GlobalItem &item : program.items) {
      if (item.kind == GlobalItem::Kind::FuncDef) {
        VisitFunction(*item.function);
      }
    }
    PopScope();
  }

  std::unordered_map<std::string, const BlockItem *> if_index;
  std::unordered_map<std::string, SupportInfo> array_index;
  std::unordered_map<std::string, SupportInfo> logic_index;

 private:
  void PushScope() { scopes_.emplace_back(); }
  void PopScope() { scopes_.pop_back(); }

  void RegisterArray(const std::string &name, std::vector<int> dims, bool is_param) {
    ArrayInfo info;
    info.known = true;
    info.is_param = is_param;
    info.dims = std::move(dims);
    scopes_.back()[name] = std::move(info);
  }

  ArrayInfo LookupArray(const std::string &name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = it->find(name);
      if (found != it->end()) {
        return found->second;
      }
    }
    return ArrayInfo{};
  }

  void RegisterConstDecl(const std::vector<ConstDef> &defs, bool visit_init) {
    for (const ConstDef &def : defs) {
      if (!def.dimensions.empty()) {
        RegisterArray(def.name, ConstDims(def.dimensions), false);
      }
      if (visit_init) {
        VisitInit(def.init, ExprContext::Other);
      }
    }
  }

  void RegisterVarDecl(const std::vector<VarDef> &defs, bool visit_init) {
    for (const VarDef &def : defs) {
      if (!def.dimensions.empty()) {
        RegisterArray(def.name, ConstDims(def.dimensions), false);
      }
      if (visit_init && def.has_init) {
        VisitInit(def.init, ExprContext::DirectVarInit);
      }
    }
  }

  void VisitFunction(const FunctionDef &function) {
    PushScope();
    for (const Param &param : function.params) {
      if (param.is_array) {
        RegisterArray(param.name, ConstDims(param.dimensions), true);
      }
    }
    VisitBlock(*function.block);
    PopScope();
  }

  void VisitBlock(const Block &block) {
    PushScope();
    for (const BlockItem &item : block.items) {
      VisitItem(item);
    }
    PopScope();
  }

  void VisitItem(const BlockItem &item) {
    switch (item.kind) {
      case BlockItem::Kind::ConstDecl:
        RegisterConstDecl(item.const_defs, true);
        break;
      case BlockItem::Kind::VarDecl:
        RegisterVarDecl(item.var_defs, true);
        break;
      case BlockItem::Kind::Assign:
        if (item.lval) {
          VisitLValIndices(*item.lval);
        }
        if (item.expr) {
          VisitExpr(*item.expr, ExprContext::DirectAssignRhs);
        }
        break;
      case BlockItem::Kind::Return:
        if (item.expr) {
          VisitExpr(*item.expr, ExprContext::DirectReturn);
        }
        break;
      case BlockItem::Kind::ExprStmt:
        if (item.expr) {
          VisitExpr(*item.expr, ExprContext::Other);
        }
        break;
      case BlockItem::Kind::Block:
        if (item.block) {
          VisitBlock(*item.block);
        }
        break;
      case BlockItem::Kind::If:
        if_index[RewriteLocKey(item.loc)] = &item;
        if (item.expr) {
          VisitExpr(*item.expr, ExprContext::Other);
        }
        if (item.then_stmt) {
          VisitItem(*item.then_stmt);
        }
        if (item.else_stmt) {
          VisitItem(*item.else_stmt);
        }
        break;
      case BlockItem::Kind::While:
        if (item.expr) {
          VisitExpr(*item.expr, ExprContext::Other);
        }
        if (item.body_stmt) {
          VisitItem(*item.body_stmt);
        }
        break;
      case BlockItem::Kind::Break:
      case BlockItem::Kind::Continue:
        break;
    }
  }

  void VisitInit(const InitVal &init, ExprContext context) {
    if (init.is_list) {
      for (const InitVal &child : init.list) {
        VisitInit(child, ExprContext::Other);
      }
    } else if (init.expr) {
      VisitExpr(*init.expr, context);
    }
  }

  void VisitLValIndices(const LValExpr &lval) {
    for (const auto &index : lval.indices) {
      VisitExpr(*index, ExprContext::Other);
    }
  }

  SupportInfo ClassifyArrayLookup(const LValExpr &lval, ExprContext context) const {
    if (context != ExprContext::DirectAssignRhs && context != ExprContext::DirectReturn &&
        context != ExprContext::DirectVarInit) {
      return {false, context == ExprContext::FunctionArg ? "function argument" : "nested lookup"};
    }
    const ArrayInfo info = LookupArray(lval.name);
    if (!info.known) {
      return {false, "array lookup is not a supported simple lookup"};
    }
    if (info.is_param) {
      return {false, "array parameter"};
    }
    if (info.dims.size() != 1) {
      return {false, "multidimensional array"};
    }
    if (info.dims[0] < 0) {
      return {false, "array length is not a compile-time constant"};
    }
    if (info.dims[0] > 16) {
      return {false, "array length " + std::to_string(info.dims[0]) + " exceeds threshold 16"};
    }
    return {true, "small array lookup can be converted to fixed scan"};
  }

  void VisitExpr(const Expr &expr, ExprContext context) {
    if (dynamic_cast<const NumberExpr *>(&expr) != nullptr) {
      return;
    }
    if (const auto *lval = dynamic_cast<const LValExpr *>(&expr)) {
      VisitLValIndices(*lval);
      if (!lval->indices.empty()) {
        array_index[RewriteLocKey(lval->loc)] = ClassifyArrayLookup(*lval, context);
      }
      return;
    }
    if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
      for (const auto &arg : call->args) {
        VisitExpr(*arg, ExprContext::FunctionArg);
      }
      return;
    }
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
      VisitExpr(*unary->operand, context);
      return;
    }
    if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expr)) {
      if (binary->op == BinaryOp::And || binary->op == BinaryOp::Or) {
        logic_index[RewriteLocKey(binary->loc)] = IsSupportedLogicExpr(*binary);
      }
      VisitExpr(*binary->lhs, ExprContext::Other);
      VisitExpr(*binary->rhs, ExprContext::Other);
    }
  }

  std::vector<std::unordered_map<std::string, ArrayInfo>> scopes_;
};

std::string UnsupportedReasonForKind(LeakKind kind) {
  switch (kind) {
    case LeakKind::SecretArrayIndex:
      return "Lv13.2 does not rewrite this array lookup";
    case LeakKind::SecretShortCircuit:
      return "Lv13.2 does not rewrite this short-circuit";
    case LeakKind::SecretObservableCall:
      return "Lv13.2 does not rewrite observable call";
    case LeakKind::SecretTimingCall:
      return "Lv13.2 does not rewrite timing call";
    case LeakKind::SecretLoopBound:
      return "Lv13.2 does not rewrite loop bounds";
    case LeakKind::SecretBreak:
      return "Lv13.2 does not rewrite break";
    case LeakKind::SecretContinue:
      return "Lv13.2 does not rewrite continue";
    case LeakKind::SecretBranch:
      return "secret-dependent branch is not a supported conditional assignment";
  }
  return "unsupported warning kind";
}

}  // namespace

std::string RewriteLocKey(SourceLocation loc) {
  return std::to_string(loc.line) + ":" + std::to_string(loc.column);
}

bool RewritePlan::AllSupported() const {
  for (const RewriteDecision &decision : decisions) {
    if (!decision.supported) {
      return false;
    }
  }
  return true;
}

bool RewritePlan::ShouldRewriteIf(SourceLocation loc) const {
  return conditional_assign_locs.count(RewriteLocKey(loc)) != 0;
}

bool RewritePlan::ShouldRewriteArrayLookup(SourceLocation loc) const {
  return array_lookup_locs.count(RewriteLocKey(loc)) != 0;
}

bool RewritePlan::ShouldRewriteLogicExpr(SourceLocation loc) const {
  return logic_expr_locs.count(RewriteLocKey(loc)) != 0;
}

RewritePlan BuildRewritePlan(const Program &program,
                             const std::vector<LeakReport> &reports) {
  RewritePlan plan;
  RewriteIndexBuilder builder;
  builder.Build(program);
  for (const LeakReport &report : reports) {
    RewriteDecision decision;
    decision.report = report;
    if (report.kind == LeakKind::SecretBranch) {
      const auto found = builder.if_index.find(RewriteLocKey(report.loc));
      if (found == builder.if_index.end()) {
        decision.supported = false;
        decision.reason = "secret-dependent branch no longer exists in AST";
      } else {
        const SupportInfo support = IsSupportedConditionalAssign(*found->second);
        decision.supported = support.supported;
        decision.reason = support.reason;
        if (decision.supported) {
          plan.conditional_assign_locs.insert(RewriteLocKey(report.loc));
        }
      }
    } else if (report.kind == LeakKind::SecretArrayIndex) {
      const auto found = builder.array_index.find(RewriteLocKey(report.loc));
      if (found != builder.array_index.end()) {
        decision.supported = found->second.supported;
        decision.reason = found->second.reason;
        if (decision.supported) {
          plan.array_lookup_locs.insert(RewriteLocKey(report.loc));
        }
      } else {
        decision.supported = false;
        decision.reason = UnsupportedReasonForKind(report.kind);
      }
    } else if (report.kind == LeakKind::SecretShortCircuit) {
      const auto found = builder.logic_index.find(RewriteLocKey(report.loc));
      if (found != builder.logic_index.end()) {
        decision.supported = found->second.supported;
        decision.reason = found->second.reason;
        if (decision.supported) {
          plan.logic_expr_locs.insert(RewriteLocKey(report.loc));
        }
      } else {
        decision.supported = false;
        decision.reason = UnsupportedReasonForKind(report.kind);
      }
    } else {
      decision.supported = false;
      decision.reason = UnsupportedReasonForKind(report.kind);
    }
    plan.decisions.push_back(std::move(decision));
  }
  return plan;
}
