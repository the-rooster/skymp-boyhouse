#pragma once
#include <cstdint>
#include <memory>

class MpActor;
class MpForm;

// A client effect belongs to one actor lifetime and one visit to a campaign.
// Returning to the same phase name does not revive an earlier effect.
class ClientEffectContext
{
public:
  static ClientEffectContext Capture(MpActor& actor);
  bool IsCurrent(MpActor& actor) const;

private:
  uint32_t ownerId = 0;
  uint64_t actorEpoch = 0, ownerEpoch = 0;
  std::weak_ptr<MpForm> actorLifetime, ownerLifetime;
};
