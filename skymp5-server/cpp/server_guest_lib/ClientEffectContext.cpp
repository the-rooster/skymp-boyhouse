#include "ClientEffectContext.h"
#include "MpActor.h"
#include "QuestJournal.h"
#include "WorldState.h"

ClientEffectContext ClientEffectContext::Capture(MpActor& actor)
{
  auto world = actor.GetParent();
  if (!world || actor.IsDeleted() || actor.IsDisabled() ||
      world->LookupFormByIdNoLoad(actor.GetFormId()).get() != &actor)
    throw std::runtime_error(
      "Client effect requires a registered active actor");
  // Noncampaign instances retain their existing behavior, but still carry an
  // actor epoch. Campaign guests additionally depend on the host's visit.
  auto& owner = actor.GetWorldInstance().rfind("campaign:", 0) == 0
    ? QuestJournal::Owner(actor)
    : actor;
  ClientEffectContext result;
  result.actorLifetime = world->LookupFormByIdNoLoad(actor.GetFormId());
  result.ownerLifetime = world->LookupFormByIdNoLoad(owner.GetFormId());
  result.ownerId = owner.GetFormId();
  result.actorEpoch = actor.GetWorldInstanceEpoch();
  result.ownerEpoch = owner.GetWorldInstanceEpoch();
  if (!result.IsCurrent(actor))
    throw std::runtime_error("Client effect campaign is unavailable");
  return result;
}

bool ClientEffectContext::IsCurrent(MpActor& actor) const
{
  auto world = actor.GetParent();
  auto capturedActor = actorLifetime.lock(),
       capturedOwner = ownerLifetime.lock();
  if (!world || !capturedActor || !capturedOwner ||
      capturedActor.get() != &actor || actor.IsDeleted() ||
      actor.IsDisabled() ||
      world->LookupFormByIdNoLoad(actor.GetFormId()) != capturedActor ||
      world->LookupFormByIdNoLoad(ownerId) != capturedOwner ||
      actor.GetWorldInstanceEpoch() != actorEpoch)
    return false;
  auto owner = capturedOwner->AsActor();
  return owner && !owner->IsDeleted() && !owner->IsDisabled() &&
    owner->GetWorldInstanceEpoch() == ownerEpoch &&
    actor.SharesWorldInstance(*owner);
}
