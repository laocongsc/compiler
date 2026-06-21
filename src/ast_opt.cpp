#include "ast_opt.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

class ConstFolder {
 public:
  void Optimize(Program &program) {
    scopes_.clear();
    PushScope();
    for (GlobalItem &item : program.items) {
      OptimizeGlobalItem(item);
    }
    PopScope();
  }

 private:
  void PushScope() { scopes_.emplace_back(); }
  void PopScope() { scopes_.pop_back(); }

  void SetConst(const std::string &name, int value) {
    scopes_.back()[name] = value;
  }

  void RemoveConst(const std::string &name) {
    scopes_.back().erase(name);
  }

  std::optional<int> LookupConst(const std::string &name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = it->find(name);
      if (found != it->end()) {
        return found->second;
      }
    }
    return std::nullopt;
  }

  void OptimizeGlobalItem(GlobalItem &item) {
    switch (item.kind) {
      case GlobalItem::Kind::ConstDecl:
        OptimizeConstDecl(item.const_defs);
        break;
      case GlobalItem::Kind::VarDecl:
        OptimizeVarDecl(item.var_defs);
        break;
      case GlobalItem::Kind::FuncDef:
        OptimizeFunction(*item.function);
        break;
    }
  }

  void OptimizeFunction(FunctionDef &function) {
    PushScope();
    for (Param &param : function.params) {
      RemoveConst(param.name);
      for (auto &dimension : param.dimensions) {
        FoldExpr(dimension);
      }
    }
    OptimizeBlock(*function.block);
    PopScope();
  }

  void OptimizeBlock(Block &block) {
    PushScope();
    for (BlockItem &item : block.items) {
      OptimizeItem(item);
    }
    PopScope();
  }

  void OptimizeItem(BlockItem &item) {
    switch (item.kind) {
      case BlockItem::Kind::ConstDecl:
        OptimizeConstDecl(item.const_defs);
        break;
      case BlockItem::Kind::VarDecl:
        OptimizeVarDecl(item.var_defs);
        break;
      case BlockItem::Kind::Assign:
        FoldLVal(*item.lval);
        FoldExpr(item.expr);
        break;
      case BlockItem::Kind::Return:
      case BlockItem::Kind::ExprStmt:
        if (item.expr) {
          FoldExpr(item.expr);
        }
        break;
      case BlockItem::Kind::Block:
        OptimizeBlock(*item.block);
        break;
      case BlockItem::Kind::If:
        FoldExpr(item.expr);
        OptimizeItem(*item.then_stmt);
        if (item.else_stmt) {
          OptimizeItem(*item.else_stmt);
        }
        break;
      case BlockItem::Kind::While:
        FoldExpr(item.expr);
        OptimizeItem(*item.body_stmt);
        break;
      case BlockItem::Kind::Break:
      case BlockItem::Kind::Continue:
        break;
    }
  }

  void OptimizeConstDecl(std::vector<ConstDef> &defs) {
    for (ConstDef &def : defs) {
      for (auto &dimension : def.dimensions) {
        FoldExpr(dimension);
      }
      FoldInit(def.init);
      if (def.dimensions.empty()) {
        if (const auto value = ConstValue(def.init.expr.get())) {
          SetConst(def.name, *value);
        } else {
          RemoveConst(def.name);
        }
      } else {
        RemoveConst(def.name);
      }
    }
  }

  void OptimizeVarDecl(std::vector<VarDef> &defs) {
    for (VarDef &def : defs) {
      for (auto &dimension : def.dimensions) {
        FoldExpr(dimension);
      }
      if (def.has_init) {
        FoldInit(def.init);
      }
      RemoveConst(def.name);
    }
  }

  void FoldInit(InitVal &init) {
    if (init.is_list) {
      for (InitVal &child : init.list) {
        FoldInit(child);
      }
    } else {
      FoldExpr(init.expr);
    }
  }

  void FoldLVal(LValExpr &lval) {
    for (auto &index : lval.indices) {
      FoldExpr(index);
    }
  }

  void FoldExpr(std::unique_ptr<Expr> &expr) {
    if (!expr) {
      return;
    }

    if (dynamic_cast<NumberExpr *>(expr.get()) != nullptr) {
      return;
    }

    if (auto *lval = dynamic_cast<LValExpr *>(expr.get())) {
      FoldLVal(*lval);
      if (lval->indices.empty()) {
        if (const auto value = LookupConst(lval->name)) {
          expr = std::make_unique<NumberExpr>(*value);
        }
      }
      return;
    }

    if (auto *call = dynamic_cast<CallExpr *>(expr.get())) {
      for (auto &arg : call->args) {
        FoldExpr(arg);
      }
      return;
    }

    if (auto *unary = dynamic_cast<UnaryExpr *>(expr.get())) {
      FoldExpr(unary->operand);
      if (const auto value = ConstValue(unary->operand.get())) {
        switch (unary->op) {
          case UnaryOp::Plus:
            expr = std::make_unique<NumberExpr>(*value);
            return;
          case UnaryOp::Minus:
            expr = std::make_unique<NumberExpr>(-*value);
            return;
          case UnaryOp::Not:
            expr = std::make_unique<NumberExpr>(*value == 0);
            return;
        }
      }
      if (unary->op == UnaryOp::Plus) {
        expr = std::move(unary->operand);
      }
      return;
    }

    auto *binary = dynamic_cast<BinaryExpr *>(expr.get());
    if (binary == nullptr) {
      throw std::runtime_error("unknown expression node");
    }

    FoldExpr(binary->lhs);
    FoldExpr(binary->rhs);

    const std::optional<int> lhs = ConstValue(binary->lhs.get());
    const std::optional<int> rhs = ConstValue(binary->rhs.get());
    if (lhs && rhs) {
      if (const auto folded = EvalBinary(binary->op, *lhs, *rhs)) {
        expr = std::make_unique<NumberExpr>(*folded);
        return;
      }
    }

    ApplyAlgebraicIdentity(expr);
  }

  static std::optional<int> ConstValue(const Expr *expr) {
    if (const auto *number = dynamic_cast<const NumberExpr *>(expr)) {
      return number->value;
    }
    return std::nullopt;
  }

  static std::optional<int> EvalBinary(BinaryOp op, int lhs, int rhs) {
    switch (op) {
      case BinaryOp::Add: return lhs + rhs;
      case BinaryOp::Sub: return lhs - rhs;
      case BinaryOp::Mul: return lhs * rhs;
      case BinaryOp::Div:
        if (rhs == 0) return std::nullopt;
        return lhs / rhs;
      case BinaryOp::Mod:
        if (rhs == 0) return std::nullopt;
        return lhs % rhs;
      case BinaryOp::Lt: return lhs < rhs;
      case BinaryOp::Gt: return lhs > rhs;
      case BinaryOp::Le: return lhs <= rhs;
      case BinaryOp::Ge: return lhs >= rhs;
      case BinaryOp::Eq: return lhs == rhs;
      case BinaryOp::Ne: return lhs != rhs;
      case BinaryOp::And: return (lhs != 0) && (rhs != 0);
      case BinaryOp::Or: return (lhs != 0) || (rhs != 0);
    }
    return std::nullopt;
  }

  static bool HasSideEffects(const Expr &expr) {
    if (dynamic_cast<const NumberExpr *>(&expr) != nullptr) {
      return false;
    }
    if (const auto *lval = dynamic_cast<const LValExpr *>(&expr)) {
      for (const auto &index : lval->indices) {
        if (HasSideEffects(*index)) {
          return true;
        }
      }
      return false;
    }
    if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
      (void)call;
      return true;
    }
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
      return HasSideEffects(*unary->operand);
    }
    const auto *binary = dynamic_cast<const BinaryExpr *>(&expr);
    if (binary == nullptr) {
      return true;
    }
    return HasSideEffects(*binary->lhs) || HasSideEffects(*binary->rhs);
  }

  static void ApplyAlgebraicIdentity(std::unique_ptr<Expr> &expr) {
    auto *binary = dynamic_cast<BinaryExpr *>(expr.get());
    if (binary == nullptr) {
      return;
    }

    const std::optional<int> lhs = ConstValue(binary->lhs.get());
    const std::optional<int> rhs = ConstValue(binary->rhs.get());
    switch (binary->op) {
      case BinaryOp::Add:
        if (rhs && *rhs == 0) {
          expr = std::move(binary->lhs);
        } else if (lhs && *lhs == 0) {
          expr = std::move(binary->rhs);
        }
        break;
      case BinaryOp::Sub:
        if (rhs && *rhs == 0) {
          expr = std::move(binary->lhs);
        }
        break;
      case BinaryOp::Mul:
        if (rhs && *rhs == 1) {
          expr = std::move(binary->lhs);
        } else if (lhs && *lhs == 1) {
          expr = std::move(binary->rhs);
        } else if (rhs && *rhs == 0 && !HasSideEffects(*binary->lhs)) {
          expr = std::make_unique<NumberExpr>(0);
        } else if (lhs && *lhs == 0 && !HasSideEffects(*binary->rhs)) {
          expr = std::make_unique<NumberExpr>(0);
        }
        break;
      case BinaryOp::Div:
        if (rhs && *rhs == 1) {
          expr = std::move(binary->lhs);
        }
        break;
      case BinaryOp::And:
        if (lhs && *lhs == 0) {
          expr = std::make_unique<NumberExpr>(0);
        } else if (rhs && *rhs == 0 && !HasSideEffects(*binary->lhs)) {
          expr = std::make_unique<NumberExpr>(0);
        }
        break;
      case BinaryOp::Or:
        if (lhs && *lhs != 0) {
          expr = std::make_unique<NumberExpr>(1);
        } else if (rhs && *rhs != 0 && !HasSideEffects(*binary->lhs)) {
          expr = std::make_unique<NumberExpr>(1);
        }
        break;
      case BinaryOp::Mod:
      case BinaryOp::Lt:
      case BinaryOp::Gt:
      case BinaryOp::Le:
      case BinaryOp::Ge:
      case BinaryOp::Eq:
      case BinaryOp::Ne:
        break;
    }
  }

  std::vector<std::unordered_map<std::string, int>> scopes_;
};

}  // namespace

void OptimizeAst(Program &program) { ConstFolder().Optimize(program); }
