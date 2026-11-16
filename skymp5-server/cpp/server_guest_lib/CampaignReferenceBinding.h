#pragma once
#include <cstdint>
#include <utility>

class MpActor;
class MpObjectReference;
class WorldState;
class MpChangeFormREFR;

// Stable original-form identities resolve only to an explicitly bound private
// world copy. This never loads or falls back to the shared vanilla reference.
class CampaignReferenceBinding
{
public:
  static constexpr auto kProperty =
    "private.indexed.boyhouse.campaignReference";
  static void Bind(MpActor& owner, uint32_t original, MpObjectReference& copy);
  static MpObjectReference& Resolve(MpActor& actor, uint32_t original);
  static std::pair<MpActor*, uint32_t> Context(MpObjectReference& copy);
  // Validate before restoring a saved copy. Its owner may load later.
  static uint32_t SavedOriginal(WorldState& world,
                                const MpChangeFormREFR& saved);
};
