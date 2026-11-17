#include "CampaignMovement.h"
#include "CampaignReferenceFactory.h"
#include "ChangeValuesMessage.h"
#include "PartOne.h"
#include "QuestJobRunner.h"

namespace {
using Json = nlohmann::json;
using Admission = QuestJobRunner::AdmissionStatus;

bool Enrolled(const MpActor& actor)
{
  return actor.GetDynamicFields().GetValueDump(
           CampaignMovementPlan::kProperty) != "null";
}

struct ResumeArea
{
  std::string id;
  uint32_t cell;
  NiPoint3 center;
  float radius;
};

ResumeArea ReadArea(MpActor& owner, const std::string& phase, const Json& area)
{
  const auto& center = area.at("center");
  const auto& cell = area.at("worldOrCell");
  if (!center.is_array() || center.size() != 3 || !cell.is_number_integer() ||
      cell.get<double>() < 0 || cell.get<double>() > UINT32_MAX ||
      owner.GetWorldInstance() != phase || owner.GetProfileId() < 0)
    throw std::runtime_error("Invalid campaign resume area or owner");
  NiPoint3 point{ center[0].get<float>(), center[1].get<float>(),
                  center[2].get<float>() };
  const auto radius = area.at("radius").get<float>();
  const auto distance = (owner.GetPos() - point).Length();
  const auto cellId = cell.get<uint32_t>();
  if (!std::isfinite(distance) || !std::isfinite(radius) ||
      distance > radius ||
      owner.GetCellOrWorld().ToFormId(owner.GetParent()->espmFiles) != cellId)
    throw std::runtime_error("Campaign resume is outside its configured area");
  return { area.at("id").get<std::string>(), cellId, point, radius };
}

void ValidateArea(MpActor& owner, const std::string& phase, const Json& area)
{
  const auto location = ReadArea(owner, phase, area);
  CampaignReferenceFactory::ValidateExistingArea(
    owner, location.id, location.cell, location.center, location.radius);
}
}

struct PartOne::ResumeAttempt
{
  Networking::UserId user = Networking::InvalidUserId;
  std::shared_ptr<const uint8_t> connection;
  std::shared_ptr<MpForm> lifetime;
  QuestJobRunner::ResumeLease lease;
  std::shared_ptr<CampaignMovementPlan> plan;
  std::string phase, savedPhase, token, status = "Pending", error, epoch,
                                        proof;
  struct Delivery
  {
    CampaignMovementPlan::SubscriptionChange edge;
    Networking::UserId user;
    std::shared_ptr<const uint8_t> connection;
    std::string epoch;
  };
  // A replacement intent inherits sealed observer changes until publication.
  using ObserverKey = std::pair<uint32_t, uint32_t>;
  std::map<ObserverKey, Delivery> deliveries;
  Json area, state, projection;
  bool detach = false, completing = false, processing = false;
};

bool PartOne::IsCurrentResume(const ResumeAttempt& a)
{
  const auto id = a.lifetime->GetFormId();
  const auto it = resumes.find(id);
  if (it == resumes.end() || it->second.get() != &a ||
      worldState.LookupFormByIdNoLoad(id) != a.lifetime)
    return false;
  if (a.detach)
    return serverState.UserByActor(a.lifetime->AsActor()) ==
      Networking::InvalidUserId;
  return IsConnected(a.user) &&
    serverState.userInfo[a.user]->connectionIdentity == a.connection &&
    serverState.ActorByUser(a.user) == a.lifetime->AsActor() &&
    serverState.userInfo[a.user]->gameplayBlocked &&
    !serverState.userInfo[a.user]->campaignVisibilityFailed;
}

std::shared_ptr<PartOne::ResumeAttempt> PartOne::CurrentResume(
  Networking::UserId user)
{
  auto* actor = serverState.ActorByUser(user);
  if (!actor || !IsConnected(user))
    return {};
  const auto it = resumes.find(actor->GetFormId());
  if (it == resumes.end() || it->second->detach || it->second->user != user ||
      it->second->connection != serverState.userInfo[user]->connectionIdentity)
    return {};
  return it->second;
}

Json PartOne::GetCampaignResume(Networking::UserId user)
{
  auto a = CurrentResume(user);
  if (!a)
    return nullptr;
  return { { "attempt", a->token },
           { "actor", a->lifetime->GetFormId() },
           { "phase", a->phase },
           { "status", a->status },
           { "error", a->error } };
}

Json PartOne::ResumeCampaignActor(Networking::UserId user, uint32_t id,
                                  const std::string& phase, const Json& area)
{
  serverState.EnsureUserExists(user);
  auto pin = worldState.LookupFormByIdNoLoad(id);
  auto* actor = pin ? pin->AsActor() : nullptr;
  if (!actor)
    throw std::runtime_error("Campaign resume actor is missing");
  if (!Enrolled(*actor))
    return nullptr;
  auto& connection = *serverState.userInfo[user];
  const auto incarnation = connection.connectionIdentity;
  connection.gameplayBlocked = true;
  if (!worldState.IsStorageRestoreReady() ||
      connection.campaignVisibilityFailed ||
      (serverState.ActorByUser(user) &&
       serverState.ActorByUser(user) != actor))
    throw std::runtime_error("Campaign resume connection is unavailable");
  const auto currentConnection = [&] {
    return worldState.IsStorageRestoreReady() && !worldState.IsRestoring() &&
      IsConnected(user) &&
      serverState.userInfo[user]->connectionIdentity == incarnation &&
      serverState.userInfo[user]->gameplayBlocked &&
      !serverState.userInfo[user]->campaignVisibilityFailed &&
      worldState.LookupFormByIdNoLoad(id) == pin;
  };
  if (auto previous = CurrentResume(user); previous &&
      (previous->phase == phase || previous->savedPhase == phase) &&
      previous->area == area && previous->lifetime == pin) {
    ValidateArea(*actor, previous->phase, area);
    if (!currentConnection() || !IsCurrentResume(*previous))
      throw std::runtime_error("Campaign resume changed during validation");
    return GetCampaignResume(user);
  }
  const auto location = ReadArea(*actor, phase, area);
  const bool visitor = QuestJournal::ParseCampaignOwnerId(phase, true) != id;
  auto a = std::make_shared<ResumeAttempt>();
  a->user = user;
  a->connection = incarnation;
  a->lifetime = pin;
  a->phase =
    visitor ? "campaign:" + std::to_string(id) + ":" + location.id : phase;
  a->savedPhase = phase;
  a->area = area;
  a->token = NewCampaignToken();

  if (visitor) {
    const auto& level = area.at("encounterLevel");
    if (!level.is_number_integer() || level.get<double>() < 1 ||
        level.get<double>() > UINT16_MAX)
      throw std::runtime_error("Invalid visitor recovery encounter level");
    const auto available = [&] {
      return currentConnection() && !serverState.ActorByUser(user) &&
        serverState.UserByActor(actor) == Networking::InvalidUserId &&
        actor->IsDisabled() && actor->GetWorldInstance() == phase;
    };
    if (!available())
      throw std::runtime_error(
        "Visitor recovery requires an unassigned restored actor");
    // Capability checks call into the configured storage backend. Complete
    // those calls before pinning a lease, a barrier or any prepared world
    // state.
    worldState.RequireDurableSaveStorage();
    if (!available())
      throw std::runtime_error(
        "Visitor or connection changed during storage preflight");
    // Seal the disabled personal row before binding any connection. Even a
    // gamemode feed that reads its actor immediately can only see personal
    // data.
    a->lease =
      worldState.GetQuestJobs().AcquireVisitorResumeLease(*actor, [&] {
        return CampaignReferenceFactory::RecoverVisitor(
          *actor, phase, location.id, location.cell, location.center,
          location.radius, level.get<uint32_t>(), available);
      });
  } else {
    CampaignReferenceFactory::ValidateExistingArea(
      *actor, location.id, location.cell, location.center, location.radius);
  }
  if (!currentConnection() || actor->GetWorldInstance() != a->phase ||
      (serverState.ActorByUser(user) &&
       serverState.ActorByUser(user) != actor) ||
      (visitor && serverState.UserByActor(actor) != Networking::InvalidUserId))
    throw std::runtime_error(
      "Campaign owner or connection changed during validation");
  auto mapping = serverState.actorsMap;
  mapping.Set(user, actor);
  auto next = resumes;
  if (auto it = next.find(id); it != next.end() && it->second->lifetime == pin)
    a->deliveries = it->second->deliveries;
  next[id] = a;
  if (!visitor)
    a->lease = worldState.GetQuestJobs().AcquireResumeLease(*actor);
  const auto old = serverState.UserByActor(actor);
  if (IsConnected(old))
    serverState.userInfo[old]->gameplayBlocked = true;
  resumes.swap(next);
  std::swap(mapping, serverState.actorsMap);
  return GetCampaignResume(user);
}

bool PartOne::DetachCampaignActor(Networking::UserId user)
{
  auto* actor = serverState.ActorByUser(user);
  if (!actor || !Enrolled(*actor))
    return false;
  serverState.userInfo.at(user)->gameplayBlocked = true;
  auto a = std::make_shared<ResumeAttempt>();
  a->lifetime = worldState.LookupFormByIdNoLoad(actor->GetFormId());
  if (a->lifetime.get() != actor)
    throw std::runtime_error("Campaign detach actor was replaced");
  a->detach = true;
  auto next = resumes;
  if (auto it = next.find(actor->GetFormId());
      it != next.end() && it->second->lifetime == a->lifetime)
    a->deliveries = it->second->deliveries;
  next[actor->GetFormId()] = a;
  a->lease = worldState.GetQuestJobs().AcquireResumeLease(*actor);
  resumes.swap(next);
  serverState.actorsMap.Erase(user);
  return true;
}

void PartOne::TickCampaignResumes()
{
  // Stream-in callbacks can disconnect, supersede attempts, or re-enter Tick.
  const auto pending = resumes;
  for (const auto& [id, a] : pending) {
    if (a->status != "Pending" || a->processing || !IsCurrentResume(*a))
      continue;
    a->processing = true;
    try {
      auto& owner = *a->lifetime->AsActor();
      auto& jobs = worldState.GetQuestJobs();
      const auto receipt = jobs.InspectResumeLease(owner, a->lease);
      if (receipt.status == Admission::Failed)
        throw std::runtime_error("Campaign recovery save was abandoned");
      if (receipt.status == Admission::Durable && !a->plan) {
        if (!a->detach)
          ValidateArea(owner, a->phase, a->area);
        auto plan = owner.PrepareCampaignResume(owner, a->detach);
        plan->SetAuthorityValidator(
          [this, weak = std::weak_ptr<ResumeAttempt>(a)] {
            const auto current = weak.lock();
            return current && IsCurrentResume(*current);
          });
        auto deliveries = a->deliveries;
        for (const auto& edge : plan->GetSubscriptionChanges()) {
          auto* listener = edge.listener->AsActor();
          if (!listener || listener == &owner)
            continue;
          const auto user = serverState.UserByActor(listener);
          if (!IsConnected(user))
            continue;
          auto key =
            std::make_pair(edge.emitter->GetFormId(), listener->GetFormId());
          auto previous = deliveries.find(key);
          const auto& connection =
            serverState.userInfo[user]->connectionIdentity;
          auto epoch = GetPacketEpoch(user);
          if (previous != deliveries.end() &&
              previous->second.connection == connection &&
              previous->second.epoch == epoch &&
              previous->second.edge.added != edge.added) {
            deliveries.erase(previous);
          } else {
            deliveries.insert_or_assign(
              key,
              ResumeAttempt::Delivery{ edge, user, connection,
                                       std::move(epoch) });
          }
        }
        jobs.AdmitResume(owner, a->lease, plan);
        a->deliveries.swap(deliveries);
        a->plan = std::move(plan);
      }
      if (a->plan &&
          jobs.InspectResumeLease(owner, a->lease).status ==
            Admission::Durable) {
        a->plan->ValidateCommitted(owner);
        while (!a->deliveries.empty()) {
          const auto it = a->deliveries.begin();
          const auto& delivery = it->second;
          const auto& edge = delivery.edge;
          auto* listener = edge.listener->AsActor();
          const auto user = delivery.user;
          if (IsConnected(user) &&
              serverState.userInfo[user]->connectionIdentity ==
                delivery.connection &&
              serverState.ActorByUser(user) == listener &&
              GetPacketEpoch(user) == delivery.epoch) {
            auto validate = [&] {
              a->plan->ValidateCommitted(owner);
              return IsCurrentResume(*a);
            };
            if (!validate())
              throw std::runtime_error("Campaign observer changed");
            PublishResumeChange(*edge.emitter->AsObjectReference(), *listener,
                                edge.added, validate);
          }
          a->deliveries.erase(it);
        }
        a->plan->ValidateCommitted(owner);
        if (a->detach)
          resumes.erase(id); // The offline owner remains held.
        else
          a->status = "ProjectionReady";
      }
    } catch (const std::exception& e) {
      a->status = "Failed";
      a->error = e.what();
    }
    a->processing = false;
  }
}

Json PartOne::ReadResumeProjectionState(ResumeAttempt& a)
{
  if (!IsCurrentResume(a) || !a.plan || a.status != "ProjectionReady")
    throw std::runtime_error("Campaign projection is no longer current");
  auto& owner = *a.lifetime->AsActor();
  a.plan->ValidateCommitted(owner);
  Json state = { { "manifest", CampaignReferenceFactory::Snapshot(owner) },
                 { "forms", Json::object() } };
  state["configuration"] = projectionConfigRevision;
  auto add = [&](MpObjectReference& ref) {
    const auto id = ref.GetFormId();
    const auto host = worldState.hosters.find(id);
    auto& entry = state["forms"][std::to_string(id)];
    entry = { { "saved", MpChangeForm::ToJson(ref.GetChangeForm()) },
              { "epoch", ref.GetWorldInstanceEpoch() },
              { "idx", ref.GetIdx() },
              { "host",
                host == worldState.hosters.end() ? 0u : host->second } };
    auto& saved = entry["saved"];
    // Private presentation scratch data is invisible to the client. Keep the
    // campaign ledger, journal, allocations and indexed reference identities.
    auto& fields = saved.at("dynamicFields");
    for (auto it = fields.begin(); it != fields.end();) {
      if (it.key().starts_with("private.") &&
          !it.key().starts_with("private.boyhouse.") &&
          !it.key().starts_with("private.indexed."))
        it = fields.erase(it);
      else
        ++it;
    }
    const auto lastAnimation = ref.GetChangeForm().lastAnimation;
    if (lastAnimation)
      entry["lastAnimation"] = *lastAnimation;
    if (&ref == &owner) {
      saved.erase("healthPercentage");
      saved.erase("magickaPercentage");
      saved.erase("staminaPercentage");
    }
    if (auto actor = ref.AsActor()) {
      entry["animation"] = actor->GetLastAnimEventAsJson();
      entry["user"] = serverState.UserByActor(actor);
    }
  };
  add(owner);
  for (auto* ref : owner.GetEmitters())
    if (ref != &owner)
      add(*ref);
  return state;
}

Json PartOne::CaptureCampaignResumeProjection(Networking::UserId user,
                                              const std::string& epoch)
{
  auto a = CurrentResume(user);
  if (!a || epoch.empty() || epoch != GetPacketEpoch(user))
    throw std::runtime_error("Invalid campaign projection epoch");
  a->proof.clear();
  auto state = ReadResumeProjectionState(*a);
  auto baseline = GetWorldBaseline(user, epoch);
  if (state != ReadResumeProjectionState(*a) || GetPacketEpoch(user) != epoch)
    throw std::runtime_error("Campaign projection changed during capture");
  auto proof = NewCampaignToken();
  a->projection = { { "attempt", a->token },
                    { "proof", proof },
                    { "manifest", state.at("manifest") },
                    { "baseline", std::move(baseline) } };
  a->state = std::move(state);
  a->epoch = epoch;
  a->proof = std::move(proof);
  return a->projection;
}

bool PartOne::CompleteCampaignResume(Networking::UserId user,
                                     const std::string& attempt,
                                     const std::string& epoch,
                                     const std::string& proof,
                                     const std::function<void()>& publishReady)
{
  auto a = CurrentResume(user);
  auto valid = [&] {
    return a && IsCurrentResume(*a) && a->token == attempt && !proof.empty() &&
      proof == a->proof && epoch == a->epoch &&
      epoch == GetPacketEpoch(user) &&
      ReadResumeProjectionState(*a) == a->state;
  };
  if (!a || a->completing || !valid())
    return false;
  a->completing = true;
  try {
    // These three mutable owner values have an explicit reliable catch-up
    // frame, queued in the same epoch as the baseline before ready.
    auto& owner = *a->lifetime->AsActor();
    const auto values = owner.GetActorValues();
    ChangeValuesMessage message;
    message.idx = owner.GetIdx();
    message.data.health = values.healthPercentage;
    message.data.magicka = values.magickaPercentage;
    message.data.stamina = values.staminaPercentage;
    GetSendTarget().Send(user, message, true);
    if (!valid()) {
      a->completing = false;
      return false;
    }
    publishReady();
    a->completing = false;
    if (!valid())
      return false;
    const auto latest = owner.GetActorValues();
    if (latest.healthPercentage != values.healthPercentage ||
        latest.magickaPercentage != values.magickaPercentage ||
        latest.staminaPercentage != values.staminaPercentage)
      return false;
    worldState.GetQuestJobs().CompleteResumeLease(*a->lifetime->AsActor(),
                                                  a->lease);
    resumes.erase(owner.GetFormId());
    serverState.userInfo[user]->gameplayBlocked = false;
    return true;
  } catch (...) {
    a->completing = false;
    throw;
  }
}
