#pragma once
#include "VarValue.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct PapyrusFunctionIdentity
{
  // method, static, propertyGet or propertySet; empty means that a legacy
  // host entry has not supplied a resumable function identity.
  std::string kind, state, function;
};

// One suspended Papyrus frame. The campaign codec replaces object pointers
// with stable identities before save; C++ native frames are never serialized.
struct PapyrusFrame
{
  VarValue receiver;
  std::string kind = "method";
  std::string rootScript, script, state, function;
  std::vector<std::pair<std::string, std::string>> contentHashes;
  std::string resultType;
  uint32_t instruction = 0;
  std::vector<std::pair<std::string, VarValue>> locals;
};

struct PapyrusContinuation : PapyrusFrame
{
  std::string scope, token;
  uint32_t executedInstructions = 0;
  // Nearest caller first, outermost entry last. The inherited frame is the
  // suspended leaf. One operation token and budget cover the complete stack.
  std::vector<PapyrusFrame> callers;
};

struct PapyrusExecutionResult
{
  VarValue value = VarValue::None();
  std::optional<PapyrusContinuation> continuation;
};
