#include "CampaignVisibilityBarrier.h"
#include "MpActor.h"
#include "PartOne.h"
#include <algorithm>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>

namespace {
bool IsPacketEpoch(const std::string& value)
{
  return value.size() == 32 &&
    value.find_first_not_of("0123456789abcdef") == std::string::npos;
}

bool IsObserver(const MpActor& owner, const MpActor* actor)
{
  if (!actor || actor->GetWorldInstance() != owner.GetWorldInstance())
    return false;
  // Private mutations can change subscriptions, so the entire phase must wait
  // for its new projection. In shared space only the owner can be mutated;
  // its connected listeners need a new baseline, not every distant player.
  if (!owner.GetWorldInstance().empty() || actor == &owner)
    return true;
  // Only direct observers have this actor in their replication baseline.
  // Hosting an offline observer does not itself subscribe a distant player.
  const auto& listeners = owner.GetActorListeners();
  return std::find(listeners.begin(), listeners.end(), actor) !=
    listeners.end();
}
}

struct CampaignVisibilityBarrier::Context
{
  struct Participant
  {
    Networking::UserId user;
    std::shared_ptr<const uint8_t> connection;
    std::shared_ptr<MpForm> actor;
    uint64_t instanceEpoch;
    std::string oldEpoch;
    std::string preparedEpoch;
  };
  std::shared_ptr<MpForm> owner;
  uint64_t ownerEpoch;
  std::string instance;
  UserIds users;
  std::vector<Participant> participants;
  bool prepared = false;
};

CampaignVisibilityBarrier::CampaignVisibilityBarrier(PartOne& server,
                                                     Before before,
                                                     After after, Close close)
  : server(server)
  , before(std::move(before))
  , after(std::move(after))
  , close(std::move(close))
{
  if (!this->before || !this->after || !this->close)
    throw std::invalid_argument(
      "Campaign visibility requires paired callbacks");
}

CampaignVisibilityBarrier::~CampaignVisibilityBarrier() = default;

bool CampaignVisibilityBarrier::IsActive() const noexcept
{
  return bool(context);
}

bool CampaignVisibilityBarrier::HoldsGate(
  Networking::UserId user) const noexcept
{
  if (!context || !server.serverState.IsConnected(user))
    return false;
  for (const auto& participant : context->participants) {
    if (participant.user == user &&
        server.serverState.userInfo[user]->connectionIdentity ==
          participant.connection)
      return true;
  }
  return false;
}

void CampaignVisibilityBarrier::BeforeCommit(MpActor& owner)
{
  if (context) {
    // WorldState also rejects nested visibility commits. Keep the bridge safe
    // when used directly: the nested failure's paired cleanup owns no gates.
    ++rejectedNested;
    throw std::runtime_error("Nested campaign visibility barrier");
  }
  auto pending = std::make_unique<Context>();
  pending->owner = server.worldState.LookupFormByIdNoLoad(owner.GetFormId());
  pending->ownerEpoch = owner.GetWorldInstanceEpoch();
  pending->instance = owner.GetWorldInstance();
  const auto prefix = "campaign:" + std::to_string(owner.GetFormId()) + ":";
  if (pending->owner.get() != &owner || owner.IsDeleted() ||
      (!pending->instance.empty() &&
       (!pending->instance.starts_with(prefix) ||
        pending->instance.size() == prefix.size())))
    throw std::runtime_error(
      "Campaign visibility requires the campaign owner");

  auto& state = server.serverState;
  for (size_t id = 0; id < state.userInfo.size(); ++id) {
    if (!state.userInfo[id])
      continue;
    const auto userId = static_cast<Networking::UserId>(id);
    auto* actor = state.ActorByUser(userId);
    if (!IsObserver(owner, actor))
      continue;
    auto& user = *state.userInfo[id];
    const auto lifetime =
      server.worldState.LookupFormByIdNoLoad(actor->GetFormId());
    if (lifetime.get() != actor || actor->IsDisabled() || actor->IsDeleted() ||
        user.isDisconnecting || user.campaignVisibilityFailed ||
        !IsPacketEpoch(user.packetEpoch))
      throw std::runtime_error(
        "Campaign visibility observer is not projection-ready");
    pending->users.push_back(userId);
    pending->participants.push_back({ userId,
                                      user.connectionIdentity,
                                      lifetime,
                                      actor->GetWorldInstanceEpoch(),
                                      user.packetEpoch,
                                      {} });
  }
  // All allocations and legacy-client checks above precede the first gate.
  // Installing the context first makes every subsequent failure cleanable.
  context = std::move(pending);
  for (const auto& participant : context->participants)
    state.userInfo[participant.user]->gameplayBlocked = true;
  if (!before(owner.GetFormId(), context->users))
    throw std::runtime_error(
      "Campaign visibility preparation was not received");
  Validate(true);
  for (auto& participant : context->participants)
    participant.preparedEpoch = state.userInfo[participant.user]->packetEpoch;
  context->prepared = true;
}

void CampaignVisibilityBarrier::Validate(bool advanced)
{
  auto& state = server.serverState;
  auto* owner = context->owner->AsActor();
  if (!owner || owner->IsDeleted() ||
      server.worldState.LookupFormByIdNoLoad(owner->GetFormId()) !=
        context->owner ||
      owner->GetWorldInstanceEpoch() != context->ownerEpoch ||
      owner->GetWorldInstance() != context->instance)
    throw std::runtime_error("Campaign visibility owner changed");
  size_t next = 0;
  for (size_t id = 0; id < state.userInfo.size(); ++id) {
    if (!state.userInfo[id])
      continue;
    const auto userId = static_cast<Networking::UserId>(id);
    auto* actor = state.ActorByUser(userId);
    if (!IsObserver(*owner, actor))
      continue;
    if (next == context->users.size() || context->users[next++] != userId)
      throw std::runtime_error("Campaign visibility participants changed");
  }
  if (next != context->users.size())
    throw std::runtime_error("Campaign visibility participants changed");
  for (const auto& participant : context->participants) {
    if (!state.IsConnected(participant.user))
      throw std::runtime_error("Campaign visibility observer disconnected");
    const auto& user = *state.userInfo[participant.user];
    auto* actor = participant.actor->AsActor();
    if (user.connectionIdentity != participant.connection ||
        state.ActorByUser(participant.user) != actor || !actor ||
        server.worldState.LookupFormByIdNoLoad(actor->GetFormId()) !=
          participant.actor ||
        actor->IsDeleted() || actor->IsDisabled() ||
        actor->GetWorldInstanceEpoch() != participant.instanceEpoch ||
        actor->GetWorldInstance() != context->instance ||
        user.isDisconnecting || user.campaignVisibilityFailed ||
        !user.gameplayBlocked || !IsPacketEpoch(user.packetEpoch) ||
        (advanced ? user.packetEpoch == participant.oldEpoch
                  : user.packetEpoch != participant.preparedEpoch))
      throw std::runtime_error("Campaign visibility observer context changed");
  }
}

void CampaignVisibilityBarrier::Quarantine() noexcept
{
  auto& state = server.serverState;
  for (const auto& participant : context->participants) {
    // A disconnect/reconnect may have reused this user slot during JS. Never
    // quarantine or close the replacement connection or its new character.
    if (!state.IsConnected(participant.user) ||
        state.userInfo[participant.user]->connectionIdentity !=
          participant.connection ||
        state.ActorByUser(participant.user) != participant.actor->AsActor())
      continue;
    auto& user = *state.userInfo[participant.user];
    user.gameplayBlocked = true;
    user.campaignVisibilityFailed = true;
    try {
      close(participant.user);
    } catch (...) {
      // Keep the native gate terminal even if the transport cannot close it.
      try {
        spdlog::error("Campaign visibility could not close user {}",
                      participant.user);
      } catch (...) {
      }
    }
  }
}

void CampaignVisibilityBarrier::AfterCommit(MpActor& owner,
                                            bool synchronized) noexcept
{
  if (rejectedNested) {
    --rejectedNested;
    return;
  }
  if (!context)
    return; // Preparation failed before acquiring any gates.
  bool ready =
    synchronized && context->prepared && context->owner.get() == &owner;
  try {
    if (ready)
      Validate(false);
  } catch (...) {
    ready = false;
  }
  try {
    // Notify using the captured owner/list even if its live context changed.
    const bool received =
      after(context->owner->GetFormId(), context->users, ready);
    if (!received)
      ready = false;
    if (ready)
      // Publishing a replacement baseline may rotate once more; a connection
      // awaiting hello may instead retain its prepared epoch. Both remain
      // blocked, and neither may restore the pre-mutation packet epoch.
      Validate(true);
  } catch (...) {
    ready = false;
  }
  if (!ready)
    Quarantine();
  context.reset();
}
