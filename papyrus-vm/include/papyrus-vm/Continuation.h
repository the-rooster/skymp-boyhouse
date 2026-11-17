#pragma once
#include "VarValue.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Data captured at an explicitly admitted native await. The campaign codec
// replaces receiver/local object pointers with stable identities before save.
// This is one leaf frame, not a complete quest job or a serializable promise.
struct PapyrusContinuation
{
  std::string scope;
  VarValue receiver;
  std::string rootScript, script, state, function;
  std::vector<std::pair<std::string, std::string>> contentHashes;
  std::string token, resultType;
  uint32_t instruction = 0, executedInstructions = 0;
  std::vector<std::pair<std::string, VarValue>> locals;
};

struct PapyrusExecutionResult
{
  VarValue value = VarValue::None();
  std::optional<PapyrusContinuation> continuation;
};
