#pragma once
#include "FormDesc.h"
#include "libespm/CONT.h"
#include "libespm/LookupResult.h"
#include <vector>

class WorldState;

// Returned item IDs are global, including inherited NPC inventories. Selection
// of leveled items is separate so a campaign can use its own level and
// globals.
std::vector<espm::CONT::ContainerObject> GetInventoryObjects(
  WorldState* world, const std::vector<FormDesc>& templateChain,
  const espm::LookupResult& source);
std::vector<espm::CONT::ContainerObject> GetOutfitObjects(
  WorldState* world, const std::vector<FormDesc>& templateChain,
  const espm::LookupResult& source);
