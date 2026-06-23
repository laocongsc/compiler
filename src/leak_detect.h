#pragma once

#include <string>
#include <vector>

#include "ast.h"

struct LeakReport {
  std::string kind;
  std::string function;
  std::string message;
};

std::vector<LeakReport> DetectSideChannelLeaks(const Program &program);
