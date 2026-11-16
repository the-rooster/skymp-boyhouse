#pragma once
#include "libespm/Combiner.h"
#include "libespm/QuestScriptData.h"

class WorldState;

// Resolves base/reference VMAD layers without attaching live world objects or
// firing events. Returned property FormIDs are in the combined load order.
class CampaignReferenceScripts
{
public:
  static std::vector<espm::QuestScript> Read(
    WorldState& world, const espm::LookupResult& reference);
};
