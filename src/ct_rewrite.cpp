#include "ct_rewrite.h"

#include <string>
#include <unordered_map>
#include <utility>

namespace {

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

std::pair<bool, std::string> IsSupportedConditionalAssign(const BlockItem &item) {
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

class IfIndexBuilder {
 public:
  std::unordered_map<std::string, const BlockItem *> Build(const Program &program) {
    index_.clear();
    for (const GlobalItem &item : program.items) {
      if (item.kind == GlobalItem::Kind::FuncDef) {
        VisitBlock(*item.function->block);
      }
    }
    return index_;
  }

 private:
  void VisitBlock(const Block &block) {
    for (const BlockItem &item : block.items) {
      VisitItem(item);
    }
  }

  void VisitItem(const BlockItem &item) {
    switch (item.kind) {
      case BlockItem::Kind::If:
        index_[RewriteLocKey(item.loc)] = &item;
        if (item.then_stmt) {
          VisitItem(*item.then_stmt);
        }
        if (item.else_stmt) {
          VisitItem(*item.else_stmt);
        }
        break;
      case BlockItem::Kind::While:
        if (item.body_stmt) {
          VisitItem(*item.body_stmt);
        }
        break;
      case BlockItem::Kind::Block:
        if (item.block) {
          VisitBlock(*item.block);
        }
        break;
      default:
        break;
    }
  }

  std::unordered_map<std::string, const BlockItem *> index_;
};

std::string UnsupportedReasonForKind(LeakKind kind) {
  switch (kind) {
    case LeakKind::SecretArrayIndex:
      return "Lv13.1 does not rewrite array lookup";
    case LeakKind::SecretShortCircuit:
      return "Lv13.1 does not rewrite short-circuit";
    case LeakKind::SecretObservableCall:
      return "Lv13.1 does not rewrite observable call";
    case LeakKind::SecretTimingCall:
      return "Lv13.1 does not rewrite timing call";
    case LeakKind::SecretLoopBound:
      return "Lv13.1 does not rewrite loop bounds";
    case LeakKind::SecretBreak:
      return "Lv13.1 does not rewrite break";
    case LeakKind::SecretContinue:
      return "Lv13.1 does not rewrite continue";
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

RewritePlan BuildRewritePlan(const Program &program,
                             const std::vector<LeakReport> &reports) {
  RewritePlan plan;
  IfIndexBuilder builder;
  const auto if_index = builder.Build(program);
  for (const LeakReport &report : reports) {
    RewriteDecision decision;
    decision.report = report;
    if (report.kind == LeakKind::SecretBranch) {
      const auto found = if_index.find(RewriteLocKey(report.loc));
      if (found == if_index.end()) {
        decision.supported = false;
        decision.reason = "secret-dependent branch no longer exists in AST";
      } else {
        const auto support = IsSupportedConditionalAssign(*found->second);
        decision.supported = support.first;
        decision.reason = support.second;
        if (decision.supported) {
          plan.conditional_assign_locs.insert(RewriteLocKey(report.loc));
        }
      }
    } else {
      decision.supported = false;
      decision.reason = UnsupportedReasonForKind(report.kind);
    }
    plan.decisions.push_back(std::move(decision));
  }
  return plan;
}
