#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "leak_detect.h"

struct RewriteDecision {
  LeakReport report;
  bool supported = false;
  std::string reason;
};

struct RewritePlan {
  std::vector<RewriteDecision> decisions;
  std::unordered_set<std::string> conditional_assign_locs;
  std::unordered_set<std::string> array_lookup_locs;
  std::unordered_set<std::string> logic_expr_locs;

  bool AllSupported() const;
  bool ShouldRewriteIf(SourceLocation loc) const;
  bool ShouldRewriteArrayLookup(SourceLocation loc) const;
  bool ShouldRewriteLogicExpr(SourceLocation loc) const;
};

RewritePlan BuildRewritePlan(const Program &program,
                             const std::vector<LeakReport> &reports);
std::string RewriteLocKey(SourceLocation loc);
