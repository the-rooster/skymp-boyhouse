#pragma once
#include <cstdint>

class MpActor;
class MpObjectReference;

// Stable original-form identities resolve only to an explicitly bound private
// world copy. This never loads or falls back to the shared vanilla reference.
class CampaignReferenceBinding
{
public:
  static constexpr auto kProperty =
    "private.indexed.boyhouse.campaignReference";
  static void Bind(MpActor& owner, uint32_t original, MpObjectReference& copy);
  static MpObjectReference& Resolve(MpActor& actor, uint32_t original);
};
