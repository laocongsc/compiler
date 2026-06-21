#include "ast_opt.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
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


class LocalDce {
 public:
  void Optimize(Program &program) {
    CollectGlobals(program);
    for (GlobalItem &item : program.items) {
      if (item.kind == GlobalItem::Kind::FuncDef) {
        OptimizeFunction(*item.function);
      }
    }
  }

 private:
  void CollectGlobals(const Program &program) {
    globals_.clear();
    for (const GlobalItem &item : program.items) {
      if (item.kind == GlobalItem::Kind::VarDecl) {
        for (const VarDef &def : item.var_defs) {
          globals_.insert(def.name);
        }
      }
      if (item.kind == GlobalItem::Kind::ConstDecl) {
        for (const ConstDef &def : item.const_defs) {
          if (!def.dimensions.empty()) {
            globals_.insert(def.name);
          }
        }
      }
    }
  }

  void OptimizeFunction(FunctionDef &function) {
    OptimizeBlock(*function.block);
  }

  void OptimizeBlock(Block &block) {
    std::unordered_set<std::string> live_vars;
    for (auto it = block.items.rbegin(); it != block.items.rend();) {
      BlockItem &item = *it;
      if (CanEraseAssign(item, live_vars)) {
        it = decltype(it)(block.items.erase(std::next(it).base()));
        continue;
      }
      VisitItem(item, live_vars);
      ++it;
    }
  }

  bool CanEraseAssign(const BlockItem &item,
                      const std::unordered_set<std::string> &live_vars) const {
    if (item.kind != BlockItem::Kind::Assign || item.lval == nullptr ||
        item.expr == nullptr) {
      return false;
    }
    if (!item.lval->indices.empty()) {
      return false;
    }
    if (globals_.count(item.lval->name) != 0) {
      return false;
    }
    if (live_vars.count(item.lval->name) != 0) {
      return false;
    }
    return !HasSideEffects(*item.expr);
  }

  void VisitItem(const BlockItem &item, std::unordered_set<std::string> &live_vars) const {
    switch (item.kind) {
      case BlockItem::Kind::ConstDecl:
        for (const ConstDef &def : item.const_defs) {
          live_vars.erase(def.name);
          for (const auto &dimension : def.dimensions) {
            CollectExprUses(*dimension, live_vars);
          }
          CollectInitUses(def.init, live_vars);
        }
        break;
      case BlockItem::Kind::VarDecl:
        for (const VarDef &def : item.var_defs) {
          const bool var_live = live_vars.count(def.name) != 0;
          live_vars.erase(def.name);
          for (const auto &dimension : def.dimensions) {
            CollectExprUses(*dimension, live_vars);
          }
          if (def.has_init && (var_live || InitHasSideEffects(def.init))) {
            CollectInitUses(def.init, live_vars);
          }
        }
        break;
      case BlockItem::Kind::Assign:
        if (item.lval != nullptr) {
          for (const auto &index : item.lval->indices) {
            CollectExprUses(*index, live_vars);
          }
          if (item.lval->indices.empty()) {
            live_vars.erase(item.lval->name);
          } else {
            live_vars.insert(item.lval->name);
          }
        }
        if (item.expr) {
          CollectExprUses(*item.expr, live_vars);
        }
        break;
      case BlockItem::Kind::Return:
      case BlockItem::Kind::ExprStmt:
        if (item.expr) {
          CollectExprUses(*item.expr, live_vars);
        }
        break;
      case BlockItem::Kind::Block:
        if (item.block) {
          CollectBlockUses(*item.block, live_vars);
        }
        break;
      case BlockItem::Kind::If:
        if (item.expr) {
          CollectExprUses(*item.expr, live_vars);
        }
        if (item.then_stmt) {
          CollectItemUses(*item.then_stmt, live_vars);
        }
        if (item.else_stmt) {
          CollectItemUses(*item.else_stmt, live_vars);
        }
        break;
      case BlockItem::Kind::While:
        if (item.expr) {
          CollectExprUses(*item.expr, live_vars);
        }
        if (item.body_stmt) {
          CollectItemUses(*item.body_stmt, live_vars);
        }
        break;
      case BlockItem::Kind::Break:
      case BlockItem::Kind::Continue:
        break;
    }
  }

  static bool InitHasSideEffects(const InitVal &init) {
    if (init.is_list) {
      for (const InitVal &child : init.list) {
        if (InitHasSideEffects(child)) {
          return true;
        }
      }
      return false;
    }
    return init.expr && HasSideEffects(*init.expr);
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
    if (dynamic_cast<const CallExpr *>(&expr) != nullptr) {
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

  static void CollectInitUses(const InitVal &init,
                              std::unordered_set<std::string> &uses) {
    if (init.is_list) {
      for (const InitVal &child : init.list) {
        CollectInitUses(child, uses);
      }
    } else if (init.expr) {
      CollectExprUses(*init.expr, uses);
    }
  }

  static void CollectExprUses(const Expr &expr,
                              std::unordered_set<std::string> &uses) {
    if (dynamic_cast<const NumberExpr *>(&expr) != nullptr) {
      return;
    }
    if (const auto *lval = dynamic_cast<const LValExpr *>(&expr)) {
      uses.insert(lval->name);
      for (const auto &index : lval->indices) {
        CollectExprUses(*index, uses);
      }
      return;
    }
    if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
      for (const auto &arg : call->args) {
        CollectExprUses(*arg, uses);
      }
      return;
    }
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
      CollectExprUses(*unary->operand, uses);
      return;
    }
    const auto *binary = dynamic_cast<const BinaryExpr *>(&expr);
    if (binary == nullptr) {
      return;
    }
    CollectExprUses(*binary->lhs, uses);
    CollectExprUses(*binary->rhs, uses);
  }

  static void CollectBlockUses(const Block &block,
                               std::unordered_set<std::string> &uses) {
    for (const BlockItem &item : block.items) {
      CollectItemUses(item, uses);
    }
  }

  static void CollectItemUses(const BlockItem &item,
                              std::unordered_set<std::string> &uses) {
    switch (item.kind) {
      case BlockItem::Kind::ConstDecl:
        for (const ConstDef &def : item.const_defs) {
          for (const auto &dimension : def.dimensions) {
            CollectExprUses(*dimension, uses);
          }
          CollectInitUses(def.init, uses);
        }
        break;
      case BlockItem::Kind::VarDecl:
        for (const VarDef &def : item.var_defs) {
          for (const auto &dimension : def.dimensions) {
            CollectExprUses(*dimension, uses);
          }
          if (def.has_init) {
            CollectInitUses(def.init, uses);
          }
        }
        break;
      case BlockItem::Kind::Assign:
        if (item.lval) {
          uses.insert(item.lval->name);
          for (const auto &index : item.lval->indices) {
            CollectExprUses(*index, uses);
          }
        }
        if (item.expr) {
          CollectExprUses(*item.expr, uses);
        }
        break;
      case BlockItem::Kind::Return:
      case BlockItem::Kind::ExprStmt:
        if (item.expr) {
          CollectExprUses(*item.expr, uses);
        }
        break;
      case BlockItem::Kind::Block:
        if (item.block) {
          CollectBlockUses(*item.block, uses);
        }
        break;
      case BlockItem::Kind::If:
        if (item.expr) {
          CollectExprUses(*item.expr, uses);
        }
        if (item.then_stmt) {
          CollectItemUses(*item.then_stmt, uses);
        }
        if (item.else_stmt) {
          CollectItemUses(*item.else_stmt, uses);
        }
        break;
      case BlockItem::Kind::While:
        if (item.expr) {
          CollectExprUses(*item.expr, uses);
        }
        if (item.body_stmt) {
          CollectItemUses(*item.body_stmt, uses);
        }
        break;
      case BlockItem::Kind::Break:
      case BlockItem::Kind::Continue:
        break;
    }
  }

  std::unordered_set<std::string> globals_;
};

}  // namespace

void OptimizeAst(Program &program) {
  ConstFolder().Optimize(program);
  LocalDce().Optimize(program);
}
