#pragma once

#include <string>
#include <vector>

#include "ast.h"

enum class Severity { High, Medium, Info };

enum class LeakKind {
  SecretBranch,
  SecretLoopBound,
  SecretBreak,
  SecretContinue,
  SecretArrayIndex,
  SecretShortCircuit,
  SecretObservableCall,
  SecretTimingCall,
};

struct LeakReport {
  LeakKind kind;
  Severity severity;
  std::string function;
  SourceLocation loc;
  std::string subject;
  std::string message;
  std::string reason;
};

std::string LeakKindName(LeakKind kind);
std::string SeverityName(Severity severity);
std::vector<LeakReport> DetectSideChannelLeaks(const Program &program);
