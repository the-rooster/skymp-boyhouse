#pragma once
#include "libespm/QuestScriptData.h"
#include "papyrus-vm/PexScript.h"
#include <nlohmann/json.hpp>

class MpActor;

class QuestScriptRuntime
{
public:
  // Trusted experimental stage execution. The quest adapter remains
  // responsible for startup, alias filling and external effects. Unsupported
  // natives throw and roll back journal/VM state. No client packet invokes
  // this API.
  static bool ExecuteStage(MpActor& actor, uint32_t questId, uint16_t stage);

  // Explicit dependency injection for deterministic VM fixtures. Production
  // uses owned QUST metadata and PEX storage through the overload above.
  static bool ExecuteStage(MpActor& actor, uint32_t questId, uint16_t stage,
                           const espm::QuestScriptData& metadata,
                           const std::vector<PexScript::Lazy>& scripts);
};
