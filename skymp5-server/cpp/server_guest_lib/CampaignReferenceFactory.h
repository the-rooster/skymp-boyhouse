#pragma once
#include <cstdint>
#include <vector>

class MpActor;
class NiPoint3;

// Trusted server allocation. Defaults are selected once, then saved with the
// owner's allocation ledger. No shared reference or global VM is instantiated.
class CampaignReferenceFactory
{
public:
  static constexpr auto kAllocations = "private.boyhouse.referenceAllocations";
  static std::vector<uint32_t> Ensure(MpActor& owner,
                                      const std::vector<uint32_t>& originals,
                                      uint32_t encounterLevel);
  static std::vector<uint32_t> EnsureArea(MpActor& owner, uint32_t worldOrCell,
                                          const NiPoint3& center, float radius,
                                          uint32_t encounterLevel);
};
