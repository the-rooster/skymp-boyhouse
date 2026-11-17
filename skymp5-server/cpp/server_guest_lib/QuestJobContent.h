#pragma once
#include "libespm/QuestScriptData.h"
#include "papyrus-vm/PexScript.h"
#include <string>
#include <vector>

class WorldState;

class QuestJobContent
{
public:
  // Loads each lazy once for this calculation. Callers may cache the digest
  // only while retaining the same immutable loaded script objects/manifest.
  static std::string ScriptManifest(const std::vector<PexScript::Lazy>& scripts);
  static std::string Hash(WorldState& world, uint32_t questId,
                          const espm::QuestScriptData& metadata,
                          const std::string& scriptManifest);
  static std::string Hash(WorldState& world, uint32_t questId,
                          const espm::QuestScriptData& metadata,
                          const std::vector<PexScript::Lazy>& scripts);
};
