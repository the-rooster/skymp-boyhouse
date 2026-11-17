#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

class MpActor;
class SaveReceipt;
class NiPoint3;

// Trusted server allocation. Defaults are selected once, then saved with the
// owner's allocation ledger. No shared reference or global VM is instantiated.
class CampaignReferenceFactory
{
public:
  static constexpr auto kAllocations = "private.boyhouse.referenceAllocations";
  // Read-only projection manifest for the actor's current campaign. Includes
  // hidden/deleted originals, which never produce ordinary stream-in messages.
  static nlohmann::json Snapshot(MpActor& actor);
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
  // Adopt only a complete, existing allocation in the owner's exact phase.
  // This never allocates, repairs bindings, runs scripts, or changes live
  // jobs.
  static void ValidateExistingArea(MpActor& owner, const std::string& areaId,
                                   uint32_t worldOrCell,
                                   const NiPoint3& center, float radius);

private:
  friend class PartOne;
  // Return a cold-restored visitor to their own disabled personal row. The
  // caller holds network and scheduler authority until the exact receipt and
  // a subsequent owner projection complete. stillCurrent only reads that
  // authority, and must never publish, allocate, or dispatch callbacks.
  static std::shared_ptr<const SaveReceipt> RecoverVisitor(
    MpActor& actor, const std::string& expectedForeignPhase,
    const std::string& areaId, uint32_t worldOrCell, const NiPoint3& center,
    float radius, uint32_t encounterLevel,
    const std::function<bool()>& stillCurrent);
  static std::vector<uint32_t> EnsureForInstance(
    MpActor& owner, const std::string& instance,
    const std::vector<uint32_t>& originals, uint32_t encounterLevel);
  static std::vector<uint32_t> EnsureAreaForInstance(
    MpActor& owner, const std::string& instance, uint32_t worldOrCell,
    const NiPoint3& center, float radius, uint32_t encounterLevel);
};
