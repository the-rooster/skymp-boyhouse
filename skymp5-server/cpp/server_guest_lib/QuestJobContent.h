#pragma once
#include "libespm/QuestScriptData.h"
#include "papyrus-vm/PexScript.h"
#include <map>
#include <string>
#include <vector>

class WorldState;

class QuestJobContent
{
public:
  // Loads each lazy once for this calculation. Callers may cache the digest
  // only while retaining the same immutable loaded script objects/manifest.
  static std::string ScriptManifest(const std::vector<PexScript::Lazy>& scripts);
  // Pins the entire fixture override map, including entries for retired or
  // not-yet-visited child quests. An empty map preserves the source manifest.
  static std::string WithMetadataOverrides(
    WorldState& world, const std::string& scriptManifest,
    const std::map<uint32_t, espm::QuestScriptData>& overrides);
  // Shared-heap epoch, independent of any one quest. The supplied manifest
  // includes the entire fixture override map when fixture content is used.
  static std::string Environment(WorldState& world,
                                 const std::string& scriptManifest);
  static std::string Hash(WorldState& world, uint32_t questId,
                          const espm::QuestScriptData& metadata,
                          const std::string& scriptManifest);
  static std::string Hash(WorldState& world, uint32_t questId,
                          const espm::QuestScriptData& metadata,
                          const std::vector<PexScript::Lazy>& scripts);
};
