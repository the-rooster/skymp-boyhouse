#pragma once
#include <cstdint>
#include <string>
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
  // Prepare the owner's destination without changing any player's current
  // phase. In particular, recovery from a guest phase uses personal globals.
  static std::vector<uint32_t> Prepare(MpActor& owner,
                                       const std::string& areaId,
                                       const std::vector<uint32_t>& originals,
                                       uint32_t encounterLevel);
  static std::vector<uint32_t> PrepareArea(
    MpActor& owner, const std::string& areaId, uint32_t worldOrCell,
    const NiPoint3& center, float radius, uint32_t encounterLevel);

private:
  static std::vector<uint32_t> EnsureForInstance(
    MpActor& owner, const std::string& instance,
    const std::vector<uint32_t>& originals, uint32_t encounterLevel);
  static std::vector<uint32_t> EnsureAreaForInstance(
    MpActor& owner, const std::string& instance, uint32_t worldOrCell,
    const NiPoint3& center, float radius, uint32_t encounterLevel);
};
