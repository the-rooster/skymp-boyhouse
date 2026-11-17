#include "MpObjectReference.h"
#include "CampaignMovement.h"
#include "CampaignReferenceBinding.h"
#include "ChangeFormGuard.h"
#include "EvaluateTemplate.h"
#include "FormCallbacks.h"
#include "GetWeightFromRecord.h"
#include "Inventory.h"
#include "InventoryDefaults.h"
#include "LeveledListUtils.h"
#include "MathUtils.h"
#include "MessageBase.h"
#include "MpActor.h"
#include "MpChangeForms.h"
#include "MsgType.h"
#include "Primitive.h"
#include "QuestJournal.h"
#include "QuestScriptRuntime.h"
#include "SaveReceipt.h"
#include "ScriptVariablesHolder.h"
#include "UpdatePropertyMessage.h"
#include "WorldState.h"
#include "gamemode_events/ActivateEvent.h"
#include "gamemode_events/PutItemEvent.h"
#include "gamemode_events/TakeItemEvent.h"
#include "libespm/CompressedFieldsCache.h"
#include "libespm/Convert.h"
#include "libespm/GroupUtils.h"
#include "libespm/Utils.h"
#include "papyrus-vm/Reader.h"
#include "papyrus-vm/Utils.h" // stricmp
#include "papyrus-vm/VirtualMachine.h"
#include "script_objects/EspmGameObject.h"
#include "script_storages/IScriptStorage.h"
#include <ScopedTask.h>
#include <TimeUtils.h>
#include <antigo/Context.h>
#include <antigo/ResolvedContext.h>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <optional>

#include "OpenContainerMessage.h"
#include "SetInventoryMessage.h"
#include "TeleportMessage.h"

#include "script_classes/PapyrusObjectReference.h" // kOriginalNameExpression

constexpr uint32_t kPlayerCharacterLevel = 1;

namespace {
void RequirePositionMutation(MpObjectReference& reference)
{
  reference.RequireCampaignSpatialMutation();
  auto* world = reference.GetParent();
  if (world && !world->IsRestoring() && !world->IsClearing() &&
      reference.UsesCampaignMovement())
    throw std::runtime_error(
      "Private campaign movement requires durable admission");
}
}

UpdatePropertyMessage MpObjectReference::CreatePropertyMessage_(
  MpObjectReference* self, const char* name, const std::string& valueDump)
{
  return PreparePropertyMessage_(self, name, valueDump);
}

UpdatePropertyMessage MpObjectReference::PreparePropertyMessage_(
  MpObjectReference* self, const char* name, const std::string& valueDump)
{
  UpdatePropertyMessage res;

  std::string baseRecordType;

  auto& loader = self->GetParent()->GetEspm();
  auto base = loader.GetBrowser().LookupById(GetBaseId());
  if (base.rec) {
    baseRecordType = base.rec->GetType().ToString();
  }

  res.idx = self->GetIdx();
  res.propName = name;
  res.refrId = self->GetFormId();
  res.dataDump = valueDump;

  // See 'perf: improve game framerate #1186'
  // Client needs to know if it is DOOR or not
  if (baseRecordType == "DOOR") {
    res.baseRecordType = baseRecordType;
  }

  return res;
}

// TODO: de-duplicate code of
// OccupantDisableEventSink/OccupantDisableEventSink, the only difference is
// base classes

class OccupantDestroyEventSink : public MpActor::DestroyEventSink
{
public:
  OccupantDestroyEventSink(WorldState& wst_,
                           MpObjectReference* untrustedRefPtr_)
    : wst(wst_)
    , untrustedRefPtr(untrustedRefPtr_)
    , refId(untrustedRefPtr_->GetFormId())
  {
  }

  void BeforeDestroy(MpActor& actor) override
  {
    if (!RefStillValid()) {
      return;
    }

    if (untrustedRefPtr->occupant == &actor) {
      untrustedRefPtr->SetOpen(false);
      untrustedRefPtr->occupant = nullptr;
    }
  }

private:
  bool RefStillValid() const
  {
    return untrustedRefPtr == wst.LookupFormById(refId).get();
  }

  WorldState& wst;
  MpObjectReference* const untrustedRefPtr;
  const uint32_t refId;
};

class OccupantDisableEventSink : public MpActor::DisableEventSink
{
public:
  OccupantDisableEventSink(WorldState& wst_,
                           MpObjectReference* untrustedRefPtr_)
    : wst(wst_)
    , untrustedRefPtr(untrustedRefPtr_)
    , refId(untrustedRefPtr_->GetFormId())
  {
  }

  void BeforeDisable(MpActor& actor) override
  {
    if (!RefStillValid()) {
      return;
    }

    if (untrustedRefPtr->occupant == &actor) {
      untrustedRefPtr->SetOpen(false);
      untrustedRefPtr->occupant = nullptr;
    }
  }

private:
  bool RefStillValid() const
  {
    return untrustedRefPtr == wst.LookupFormById(refId).get();
  }

  WorldState& wst;
  MpObjectReference* const untrustedRefPtr;
  const uint32_t refId;
};

namespace {
std::pair<int16_t, int16_t> GetGridPos(const NiPoint3& pos) noexcept
{
  return { int16_t(pos.x / 4096), int16_t(pos.y / 4096) };
}
}

struct AnimGraphHolder
{
  AnimGraphHolder() { boolVariables.fill(false); }

  std::array<bool, static_cast<size_t>(AnimationVariableBool::kNumVariables)>
    boolVariables;
};

struct ScriptState
{
  std::map<std::string, std::shared_ptr<ScriptVariablesHolder>> varHolders;
};

struct PrimitiveData
{
  NiPoint3 boundsDiv2;
  GeoProc::GeoPolygonProc polygonProc;
};

struct MpObjectReference::Impl
{
public:
  bool onInitEventSent = false;
  bool scriptsInited = false;
  std::unique_ptr<ScriptState> scriptState;
  AnimGraphHolder animGraphHolder;
  std::optional<PrimitiveData> primitive;
  bool teleportFlag = false;
  bool setPropertyCalled = false;
  // Spatial membership reconstructed from a save has not been published to
  // scripts or clients. Keep that distinction until a real subscription pass.
  std::map<MpObjectReference*, uint64_t> pendingListeners;
  // Published edges retain their occurrence too: a later player profile can
  // make their load events eligible without requiring another network create.
  std::map<MpObjectReference*, uint64_t> listenerOccurrences;
  std::set<MpObjectReference*> publishingListeners;
  uint64_t nextPublication = 0;
  bool subscriptionsPending = false;
  bool campaignMovementEnrolled = false;
  bool destroying = false;
  std::shared_ptr<const SaveReceipt> movementReceipt;
};

struct CampaignMovementPlan::Impl
{
  struct Read
  {
    std::shared_ptr<MpForm> lifetime;
    uint64_t epoch;
    nlohmann::json snapshot;
  };
  struct Topology
  {
    std::set<MpObjectReference*> listeners, emitters, inside, publishing;
    std::vector<MpActor*> actors;
    std::map<MpObjectReference*, bool> occupancy;
    std::map<MpObjectReference*, uint64_t> pending, occurrences;
    uint64_t nextPublication = 0;
    bool subscriptionsPending = false, initialized = false, ever = false;
    bool operator==(const Topology&) const = default;
  };
  static Topology Capture(const MpObjectReference& ref)
  {
    Topology result;
    result.listeners = ref.GetListeners();
    result.emitters = ref.GetEmitters();
    result.actors = ref.actorListenerArray;
    if (ref.emittersWithPrimitives)
      result.occupancy = *ref.emittersWithPrimitives;
    if (ref.primitivesWeAreInside)
      result.inside = *ref.primitivesWeAreInside;
    result.pending = ref.pImpl->pendingListeners;
    result.occurrences = ref.pImpl->listenerOccurrences;
    result.publishing = ref.pImpl->publishingListeners;
    result.nextPublication = ref.pImpl->nextPublication;
    result.subscriptionsPending = ref.pImpl->subscriptionsPending;
    result.initialized = ref.pImpl->onInitEventSent;
    result.ever = ref.everSubscribedOrListened;
    return result;
  }
  struct Graph
  {
    MpObjectReference* reference;
    Topology before, after;
    std::unique_ptr<std::set<MpObjectReference*>> listeners, emitters, inside;
    std::unique_ptr<std::map<MpObjectReference*, bool>> occupancy;
    std::vector<MpActor*> actors;
    std::map<MpObjectReference*, uint64_t> pending, occurrences;
  };
  Kind kind = Kind::Movement;
  std::vector<Graph> graph;
  std::vector<SubscriptionChange> subscriptions;
  std::set<MpObjectReference*> neighbors;
  std::shared_ptr<const SaveReceipt> receipt;
  WorldState* world = nullptr;
  MpActor* source = nullptr;
  MpActor* requester = nullptr;
  MpActor* owner = nullptr;
  NiPoint3 position, angle;
  std::vector<Read> reads;
  std::vector<Event> events;
  std::vector<std::shared_ptr<MpForm>> targets;
  std::set<MpObjectReference*> emitters, listeners;
  std::map<MpObjectReference*, bool> oldOccupancy;
  std::set<MpObjectReference*> oldInside;
  std::unique_ptr<std::map<MpObjectReference*, bool>> occupancy;
  std::unique_ptr<std::set<MpObjectReference*>> inside;
  std::string baseline;
  std::function<bool()> authority;
  bool published = false;
};

CampaignMovementPlan::CampaignMovementPlan(std::unique_ptr<Impl> impl)
  : pImpl(std::move(impl))
{
}

CampaignMovementPlan::~CampaignMovementPlan() = default;

CampaignMovementPlan::Kind CampaignMovementPlan::GetKind() const noexcept
{
  return pImpl->kind;
}

const std::vector<CampaignMovementPlan::SubscriptionChange>&
CampaignMovementPlan::GetSubscriptionChanges() const
{
  return pImpl->subscriptions;
}

const std::vector<CampaignMovementPlan::Event>&
CampaignMovementPlan::GetEvents() const
{
  return pImpl->events;
}

MpActor& CampaignMovementPlan::GetSource() const
{
  return *pImpl->source;
}
const NiPoint3& CampaignMovementPlan::GetPosition() const
{
  return pImpl->position;
}
const NiPoint3& CampaignMovementPlan::GetAngle() const
{
  return pImpl->angle;
}
const std::string& CampaignMovementPlan::GetBaselineDump() const
{
  return pImpl->baseline;
}

void CampaignMovementPlan::Validate(MpActor& requester) const
{
  if (pImpl->published || &requester != pImpl->requester ||
      requester.GetParent() != pImpl->world)
    throw std::runtime_error("Campaign movement plan context changed");
  if (pImpl->authority && !pImpl->authority())
    throw std::runtime_error("Campaign movement packet authority changed");
  for (const auto& read : pImpl->reads) {
    auto* reference = read.lifetime->AsObjectReference();
    if (!reference ||
        pImpl->world->LookupFormByIdNoLoad(reference->GetFormId()) !=
          read.lifetime ||
        reference->GetWorldInstanceEpoch() != read.epoch ||
        MpChangeForm::ToJson(reference->GetChangeForm()) != read.snapshot)
      throw std::runtime_error("Campaign movement participant changed");
  }
  // This read-only preparation also rechecks bindings, initialized targets,
  // exact primitive geometry, the persistent baseline and the complete roster.
  const auto current = pImpl->kind == Kind::Movement
    ? pImpl->source->PrepareCampaignMovement(requester, pImpl->position,
                                             pImpl->angle)
    : pImpl->source->PrepareCampaignResume(requester,
                                           pImpl->kind == Kind::Detach);
  if (pImpl->kind != Kind::Movement) {
    if (current->pImpl->neighbors != pImpl->neighbors ||
        current->pImpl->graph.size() != pImpl->graph.size())
      throw std::runtime_error("Recovery subscription roster changed");
    for (size_t i = 0; i < pImpl->graph.size(); ++i)
      if (current->pImpl->graph[i].reference != pImpl->graph[i].reference ||
          current->pImpl->graph[i].before != pImpl->graph[i].before ||
          current->pImpl->graph[i].after != pImpl->graph[i].after)
        throw std::runtime_error("Recovery subscription topology changed");
  }
  if (current->pImpl->owner != pImpl->owner ||
      current->pImpl->baseline != pImpl->baseline ||
      current->pImpl->emitters != pImpl->emitters ||
      current->pImpl->listeners != pImpl->listeners ||
      current->pImpl->oldOccupancy != pImpl->oldOccupancy ||
      current->pImpl->oldInside != pImpl->oldInside ||
      current->pImpl->targets != pImpl->targets)
    throw std::runtime_error("Campaign movement roster or geometry changed");
}

void CampaignMovementPlan::SetAuthorityValidator(
  std::function<bool()> validator)
{
  if (pImpl->published || pImpl->authority || !validator)
    throw std::runtime_error("Campaign movement authority is already fixed");
  pImpl->authority = std::move(validator);
}

void CampaignMovementPlan::Publish(
  std::shared_ptr<const SaveReceipt> receipt) noexcept
{
  auto& source = static_cast<MpObjectReference&>(*pImpl->source);
  source.CommitPreparedMovementTransform(pImpl->position, pImpl->angle);
  if (pImpl->kind == Kind::Movement) {
    source.emittersWithPrimitives.swap(pImpl->occupancy);
    source.primitivesWeAreInside.swap(pImpl->inside);
  } else {
    for (auto& entry : pImpl->graph) {
      auto& ref = *entry.reference;
      ref.listeners.swap(entry.listeners);
      ref.emitters.swap(entry.emitters);
      ref.primitivesWeAreInside.swap(entry.inside);
      ref.emittersWithPrimitives.swap(entry.occupancy);
      ref.actorListenerArray.swap(entry.actors);
      ref.pImpl->pendingListeners.swap(entry.pending);
      ref.pImpl->listenerOccurrences.swap(entry.occurrences);
      ref.pImpl->nextPublication = entry.after.nextPublication;
      ref.pImpl->subscriptionsPending = entry.after.subscriptionsPending;
      ref.pImpl->onInitEventSent = entry.after.initialized;
      ref.everSubscribedOrListened = entry.after.ever;
    }
  }
  pImpl->receipt = receipt;
  source.pImpl->campaignMovementEnrolled = true;
  source.pImpl->movementReceipt = receipt;
  for (const auto& target : pImpl->targets) {
    auto* reference = target->AsObjectReference();
    reference->pImpl->campaignMovementEnrolled = true;
    reference->pImpl->movementReceipt = receipt;
  }
  pImpl->published = true;
}

bool MpObjectReference::UsesCampaignMovement() const
{
  if (IsCampaignMovementEnrolled())
    return true;
  if (AsActor() && emittersWithPrimitives)
    for (const auto& [emitter, inside] : *emittersWithPrimitives)
      if (emitter->GetDynamicFields().GetValueDump(
            CampaignReferenceBinding::kProperty) != "null")
        return true;
  return false;
}

bool MpObjectReference::IsCampaignMovementEnrolled() const
{
  if (pImpl->campaignMovementEnrolled ||
      GetDynamicFields().GetValueDump(CampaignMovementPlan::kProperty) !=
        "null")
    return true;
  // During passive restoration the source may load before or after its target.
  // Persistent source ownership protects reconstructed target geometry too.
  if (pImpl->primitive && listeners)
    for (const auto* listener : *listeners)
      if (listener->GetDynamicFields().GetValueDump(
            CampaignMovementPlan::kProperty) != "null")
        return true;
  return false;
}

bool MpObjectReference::HasPendingCampaignMovement() const
{
  return pImpl->movementReceipt &&
    pImpl->movementReceipt->GetState() != SaveReceipt::State::Durable;
}

void MpObjectReference::RequireCampaignSpatialMutation() const
{
  auto* world = GetParent();
  if (pImpl->destroying ||
      (world && (world->IsRestoring() || world->IsClearing())))
    return;
  if (IsCampaignMovementEnrolled() || HasPendingCampaignMovement())
    throw std::runtime_error(
      "Campaign movement requires an atomic spatial transition");
}

std::shared_ptr<CampaignMovementPlan>
MpObjectReference::PrepareCampaignMovement(MpActor& requester,
                                           const NiPoint3& position,
                                           const NiPoint3& angle)
{
  using Json = nlohmann::json;
  constexpr size_t maxPrimitives = 64, maxBaselineBytes = 1024 * 1024;
  constexpr uint64_t maxSequence = 9007199254740991ULL;
  auto* world = GetParent();
  auto* source = AsActor();
  if (!world || !source || world->IsRestoring() || world->IsClearing() ||
      requester.GetParent() != world || IsDeleted() || IsDisabled() ||
      requester.IsDeleted() || requester.IsDisabled() ||
      !SharesWorldInstance(requester) || HasPendingCampaignMovement() ||
      HasPendingSubscriptions())
    throw std::runtime_error("Campaign movement source is unavailable");
  const auto finite = [](const NiPoint3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
      std::isfinite(value.z);
  };
  const auto gridSafe = [&](const NiPoint3& value) {
    return finite(value) && std::abs(value.x / 4096) < 32767 &&
      std::abs(value.y / 4096) < 32767;
  };
  if (!gridSafe(GetPos()) || !gridSafe(position) || !finite(GetAngle()) ||
      !finite(angle) || GetGridPos(GetPos()) != GetGridPos(position) ||
      !everSubscribedOrListened || !emitters || !listeners || HasPrimitive())
    throw std::runtime_error(
      "Campaign movement requires a stable grid roster");
  auto& owner = QuestJournal::Owner(requester);
  if (&owner != &QuestJournal::Owner(*source))
    throw std::runtime_error("Campaign movement belongs to another owner");
  if (source != &requester) {
    const auto host = world->hosters.find(GetFormId());
    if (GetBaseId() == 7 || GetChangeForm().profileId >= 0 ||
        host == world->hosters.end() || host->second != requester.GetFormId())
      throw std::runtime_error("Campaign movement source is not hosted");
  }
  auto prepared = std::make_unique<CampaignMovementPlan::Impl>();
  prepared->world = world;
  prepared->source = source;
  prepared->requester = &requester;
  prepared->owner = &owner;
  prepared->position = position;
  prepared->angle = angle;
  prepared->emitters = *emitters;
  prepared->listeners = *listeners;
  if (emittersWithPrimitives)
    prepared->oldOccupancy = *emittersWithPrimitives;
  if (primitivesWeAreInside)
    prepared->oldInside = *primitivesWeAreInside;
  prepared->occupancy = std::make_unique<std::map<MpObjectReference*, bool>>();
  prepared->inside = std::make_unique<std::set<MpObjectReference*>>();
  const auto read = [&](MpObjectReference& reference) {
    const auto lifetime = world->LookupFormByIdNoLoad(reference.GetFormId());
    if (lifetime.get() != &reference || reference.GetParent() != world)
      throw std::runtime_error("Unregistered campaign movement participant");
    prepared->reads.push_back(
      { lifetime, reference.GetWorldInstanceEpoch(),
        MpChangeForm::ToJson(reference.GetChangeForm()) });
  };
  read(*source);
  read(requester);
  read(owner);
  const auto point = [](const NiPoint3& value) {
    return Json::array({ value.x, value.y, value.z });
  };
  struct Target
  {
    std::string identity;
    std::shared_ptr<MpForm> lifetime;
    Json saved;
    bool before, after;
  };
  std::vector<Target> targets;
  if (emittersWithPrimitives && emittersWithPrimitives->size() > maxPrimitives)
    throw std::runtime_error("Campaign movement primitive limit exceeded");
  // Each reconstruction gets its own validated read snapshot. Do not carry
  // it across authority callbacks or reuse it in another validation pass.
  // Inside a journal transaction initialization reads already borrow the
  // current ledger, so an external snapshot is neither needed nor allowed.
  std::optional<QuestJournal::CaptureContext> captureContext;
  if (!QuestJournal::HasActiveTransaction() && emittersWithPrimitives &&
      !emittersWithPrimitives->empty())
    captureContext.emplace(QuestJournal::CaptureReadContext(requester));
  if (emittersWithPrimitives)
    for (const auto& [emitter, oldInside] : *emittersWithPrimitives) {
      if (!emitter || !emitter->pImpl->primitive || emitter->IsDisabled() ||
          emitter->IsDeleted() || !SharesWorldInstance(*emitter) ||
          emitter->GetCellOrWorld() != GetCellOrWorld() ||
          emitter->HasPendingCampaignMovement() ||
          emitter->HasPendingSubscriptions() || !emitters->count(emitter) ||
          !emitter->GetListeners().count(this))
        throw std::runtime_error("Campaign movement primitive roster changed");
      const auto& bounds = emitter->pImpl->primitive->boundsDiv2;
      if (!finite(emitter->GetPos()) || !finite(emitter->GetAngle()) ||
          !finite(bounds) || bounds.x <= 0 || bounds.y <= 0 || bounds.z <= 0 ||
          emitter->GetAngle().x != 0 || emitter->GetAngle().y != 0)
        throw std::runtime_error("Invalid campaign primitive geometry");
      const auto vertices =
        Primitive::GetVertices(emitter->GetPos(), emitter->GetAngle(), bounds);
      const auto cached =
        emitter->pImpl->primitive->polygonProc.GetPolygon().GetV();
      if (vertices.size() != cached.size())
        throw std::runtime_error("Campaign primitive geometry is stale");
      for (size_t index = 0; index < vertices.size(); ++index)
        if (!finite(vertices[index]) || vertices[index].x != cached[index].x ||
            vertices[index].y != cached[index].y ||
            vertices[index].z != cached[index].z)
          throw std::runtime_error("Campaign primitive geometry is stale");
      const auto captured = captureContext
        ? QuestScriptRuntime::CaptureInteraction(
            requester, *emitter, "OnTrigger", *source, *captureContext)
        : QuestScriptRuntime::CaptureInteraction(requester, *emitter,
                                                 "OnTrigger", *source);
      read(*emitter);
      const bool before = emitter->IsPointInsidePrimitive(GetPos());
      const bool after = emitter->IsPointInsidePrimitive(position);
      targets.push_back({ captured.target,
                          world->LookupFormByIdNoLoad(emitter->GetFormId()),
                          { { "target", captured.target },
                            { "reference", captured.targetReference },
                            { "templates", captured.targetTemplates },
                            { "position", point(emitter->GetPos()) },
                            { "angle", point(emitter->GetAngle()) },
                            { "bounds", point(bounds) },
                            { "inside", before } },
                          before,
                          after });
    }
  std::sort(targets.begin(), targets.end(),
            [](const auto& left, const auto& right) {
              return left.identity < right.identity;
            });
  Json roster = Json::array();
  std::string previous;
  for (const auto& target : targets) {
    if (target.identity == previous)
      throw std::runtime_error("Duplicate campaign movement primitive");
    previous = target.identity;
    roster.push_back(target.saved);
  }
  const auto& savedDump =
    GetDynamicFields().GetValueDump(CampaignMovementPlan::kProperty);
  uint64_t sequence = 0;
  Json saved;
  if (savedDump != "null") {
    if (savedDump.size() > maxBaselineBytes)
      throw std::runtime_error("Campaign movement baseline is too large");
    saved = Json::parse(savedDump);
    if (!saved.is_object() || !saved.at("sequence").is_number_integer() ||
        (!saved.at("sequence").is_number_unsigned() &&
         saved.at("sequence").get<int64_t>() < 1) ||
        !saved.at("schemaVersion").is_number_integer())
      throw std::runtime_error("Malformed campaign movement baseline");
    sequence = saved.at("sequence").get<uint64_t>();
    if (!sequence || sequence >= maxSequence)
      throw std::runtime_error("Campaign movement sequence exhausted");
  }
  Json baseline = { { "schemaVersion", 1 },
                    { "source", GetChangeForm().formDesc.ToString() },
                    { "owner", owner.GetChangeForm().formDesc.ToString() },
                    { "campaignIdentity",
                      CampaignReferenceBinding::GetIdentity(owner) },
                    { "instance", GetWorldInstance() },
                    { "cell", GetCellOrWorld().ToString() },
                    { "position", point(GetPos()) },
                    { "angle", point(GetAngle()) },
                    { "sequence", sequence },
                    { "primitives", roster } };
  if (savedDump != "null" && saved != baseline)
    throw std::runtime_error("Saved campaign movement baseline changed");
  for (size_t index = 0; index < targets.size(); ++index) {
    const auto& target = targets[index];
    auto* reference = target.lifetime->AsObjectReference();
    prepared->targets.push_back(target.lifetime);
    prepared->occupancy->emplace(reference, target.after);
    if (target.after)
      prepared->inside->insert(reference);
    if (target.before != target.after)
      prepared->events.push_back(
        { target.lifetime,
          target.after ? "OnTriggerEnter" : "OnTriggerLeave" });
    baseline["primitives"][index]["inside"] = target.after;
  }
  for (const auto& target : targets)
    if (target.after)
      prepared->events.push_back({ target.lifetime, "OnTrigger" });
  if (prepared->events.size() > QuestJobState::MaxBatchOccurrences)
    throw std::runtime_error("Campaign movement occurrence limit exceeded");
  baseline["position"] = point(position);
  baseline["angle"] = point(angle);
  baseline["sequence"] = sequence + 1;
  prepared->baseline = baseline.dump();
  if (prepared->baseline.size() > maxBaselineBytes)
    throw std::runtime_error("Campaign movement baseline is too large");
  if (captureContext)
    captureContext->Validate(requester);
  return std::shared_ptr<CampaignMovementPlan>(
    new CampaignMovementPlan(std::move(prepared)));
}

std::shared_ptr<CampaignMovementPlan> MpObjectReference::PrepareCampaignResume(
  MpActor& requester, bool detach)
{
  using Json = nlohmann::json;
  using Topology = CampaignMovementPlan::Impl::Topology;
  auto* world = GetParent();
  auto* source = AsActor();
  if (!world || !source || source != &requester || IsDeleted() ||
      world->IsClearing() || world->IsRestoring() ||
      !world->IsStorageRestoreReady() || HasPrimitive() ||
      HasPendingCampaignMovement() ||
      &QuestJournal::Owner(requester) != source || GetWorldInstance().empty())
    throw std::runtime_error("Campaign recovery source is unavailable");
  const auto finite = [](const NiPoint3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
      std::isfinite(value.z);
  };
  if (!finite(GetPos()) || !finite(GetAngle()) ||
      std::abs(GetPos().x / 4096) >= 32767 ||
      std::abs(GetPos().y / 4096) >= 32767)
    throw std::runtime_error("Invalid recovery position");
  const auto savedDump =
    GetDynamicFields().GetValueDump(CampaignMovementPlan::kProperty);
  if (savedDump == "null" || savedDump.size() > 1024 * 1024)
    throw std::runtime_error("Recovery requires a saved movement baseline");
  const auto saved = Json::parse(savedDump);
  if (!saved.is_object() || !saved.at("sequence").is_number_integer() ||
      (!saved.at("sequence").is_number_unsigned() &&
       saved.at("sequence").get<int64_t>() < 1) ||
      !saved.at("sequence").get<uint64_t>() ||
      saved.at("sequence").get<uint64_t>() > 9007199254740991ULL)
    throw std::runtime_error("Malformed recovery movement baseline");
  auto prepared = std::make_unique<CampaignMovementPlan::Impl>();
  prepared->kind = detach ? CampaignMovementPlan::Kind::Detach
                          : CampaignMovementPlan::Kind::Resume;
  prepared->world = world;
  prepared->source = source;
  prepared->requester = source;
  prepared->owner = source;
  prepared->position = GetPos();
  prepared->angle = GetAngle();
  prepared->baseline = savedDump;
  prepared->emitters = GetEmitters();
  prepared->listeners = GetListeners();
  if (emittersWithPrimitives)
    prepared->oldOccupancy = *emittersWithPrimitives;
  if (primitivesWeAreInside)
    prepared->oldInside = *primitivesWeAreInside;
  const auto cell = GetCellOrWorld().ToFormId(world->espmFiles);
  const auto grid = world->grids.find(cell);
  if (grid != world->grids.end()) {
    // Grid's historical const query grows its lookup cache. Query a copy so
    // recovery preparation itself cannot change the live spatial structure.
    auto gridCopy = *grid->second.grid;
    const auto [x, y] = GetGridPos(GetPos());
    for (auto* ref : gridCopy.GetNeighboursByPosition(x, y))
      if (ref != this && !ref->IsDisabled() && !ref->IsDeleted() &&
          SharesWorldInstance(*ref))
        prepared->neighbors.insert(ref);
  }
  prepared->neighbors.insert(this);
  std::map<MpObjectReference*, Topology> before, after;
  const auto add = [&](MpObjectReference* ref) {
    if (before.contains(ref))
      return;
    if (!ref || before.size() >= 4096 || ref->GetParent() != world ||
        world->LookupFormByIdNoLoad(ref->GetFormId()).get() != ref ||
        !SharesWorldInstance(*ref) ||
        ref->GetCellOrWorld() != GetCellOrWorld() ||
        !ref->pImpl->publishingListeners.empty() ||
        ref->HasPendingCampaignMovement())
      throw std::runtime_error("Invalid recovery subscription participant");
    before.emplace(ref, CampaignMovementPlan::Impl::Capture(*ref));
    prepared->reads.push_back({ world->LookupFormByIdNoLoad(ref->GetFormId()),
                                ref->GetWorldInstanceEpoch(),
                                MpChangeForm::ToJson(ref->GetChangeForm()) });
  };
  for (auto* ref : prepared->neighbors)
    add(ref);
  // Reconcile the restored connected component, including NPC-only passive
  // edges. Otherwise a primitive could retain a pending NPC edge forever and
  // reject the owner's next ordinary movement despite its own edge being
  // ready.
  std::set<MpObjectReference*> visited;
  size_t edges = 0;
  while (visited.size() != before.size()) {
    for (auto it = before.begin(); it != before.end(); ++it) {
      if (!visited.insert(it->first).second)
        continue;
      if (it->second.listeners.size() + it->second.emitters.size() >
          65536 - edges)
        throw std::runtime_error("Recovery subscription edge limit exceeded");
      edges += it->second.listeners.size() + it->second.emitters.size();
      for (auto* ref : it->second.listeners)
        add(ref);
      for (auto* ref : it->second.emitters)
        add(ref);
    }
  }
  after = before;
  std::optional<QuestJournal::CaptureContext> capture;
  if (!QuestJournal::HasActiveTransaction())
    capture.emplace(QuestJournal::CaptureReadContext(requester));
  const auto initialized = [&](uint32_t original) {
    return capture ? capture->ReferenceInitialized(requester, original)
                   : QuestJournal::ReferenceInitialized(requester, original);
  };
  const auto ensureInitialized = [&](MpObjectReference* ref) {
    if (ref == this ||
        (ref->AsActor() && ref->AsActor()->GetProfileId() >= 0)) {
      after.at(ref).initialized = true;
      return;
    }
    const auto [owner, original] = CampaignReferenceBinding::Context(*ref);
    if (owner != source || !initialized(original))
      throw std::runtime_error(
        "Recovery cannot replace reference initialization");
    after.at(ref).initialized = true;
  };
  const auto remove = [&](MpObjectReference* emitter,
                          MpObjectReference* listener) {
    auto& e = after.at(emitter);
    auto& l = after.at(listener);
    if (!e.listeners.erase(listener))
      return;
    l.emitters.erase(emitter);
    e.pending.erase(listener);
    e.occurrences.erase(listener);
    if (auto* actor = listener->AsActor())
      std::erase(e.actors, actor);
    l.occupancy.erase(emitter);
    l.inside.erase(emitter);
    if (!emitter->HasPrimitive() &&
        !before.at(emitter).pending.contains(listener))
      prepared->subscriptions.push_back(
        { world->LookupFormByIdNoLoad(emitter->GetFormId()),
          world->LookupFormByIdNoLoad(listener->GetFormId()), false });
  };
  // Tear down only this owner's relationships; other restored edges retain
  // their scene membership and are promoted below without gameplay callbacks.
  for (auto* ref : before.at(this).listeners)
    remove(this, ref);
  for (auto* ref : before.at(this).emitters)
    remove(ref, this);
  const auto connect = [&](MpObjectReference* emitter,
                           MpObjectReference* listener) {
    auto& e = after.at(emitter);
    auto& l = after.at(listener);
    if (!e.listeners.insert(listener).second)
      return;
    if (e.nextPublication == std::numeric_limits<uint64_t>::max())
      throw std::runtime_error("Recovery subscription identity exhausted");
    e.occurrences[listener] = ++e.nextPublication;
    e.pending[listener] = e.nextPublication;
    l.emitters.insert(emitter);
  };
  if (!detach) {
    for (auto* ref : prepared->neighbors) {
      connect(this, ref);
      if (ref != this)
        connect(ref, this);
    }
  }
  const auto point = [](const NiPoint3& value) {
    return Json::array({ value.x, value.y, value.z });
  };
  std::map<std::string, Json> primitives;
  for (auto* ref : prepared->neighbors) {
    if (ref == this || !ref->HasPrimitive())
      continue;
    if (primitives.size() >= 64)
      throw std::runtime_error("Recovery primitive limit exceeded");
    const auto& bounds = ref->pImpl->primitive->boundsDiv2;
    if (!finite(ref->GetPos()) || !finite(ref->GetAngle()) ||
        !finite(bounds) || bounds.x <= 0 || bounds.y <= 0 || bounds.z <= 0 ||
        ref->GetAngle().x != 0 || ref->GetAngle().y != 0)
      throw std::runtime_error("Invalid recovery primitive geometry");
    const auto vertices =
      Primitive::GetVertices(ref->GetPos(), ref->GetAngle(), bounds);
    const auto cached = ref->pImpl->primitive->polygonProc.GetPolygon().GetV();
    if (vertices.size() != cached.size())
      throw std::runtime_error("Stale recovery primitive");
    for (size_t i = 0; i < vertices.size(); ++i)
      if (!finite(vertices[i]) || vertices[i].x != cached[i].x ||
          vertices[i].y != cached[i].y || vertices[i].z != cached[i].z)
        throw std::runtime_error("Stale recovery primitive");
    const auto event = capture
      ? QuestScriptRuntime::CaptureInteraction(requester, *ref, "OnTrigger",
                                               requester, *capture)
      : QuestScriptRuntime::CaptureInteraction(requester, *ref, "OnTrigger",
                                               requester);
    if (!primitives
           .emplace(
             event.target,
             Json{ { "target", event.target },
                   { "reference", event.targetReference },
                   { "templates", event.targetTemplates },
                   { "position", point(ref->GetPos()) },
                   { "angle", point(ref->GetAngle()) },
                   { "bounds", point(bounds) },
                   { "inside", ref->IsPointInsidePrimitive(GetPos()) } })
           .second)
      throw std::runtime_error("Duplicate recovery primitive");
    prepared->targets.push_back(world->LookupFormByIdNoLoad(ref->GetFormId()));
  }
  Json roster = Json::array();
  for (const auto& [key, entry] : primitives)
    roster.push_back(entry);
  const Json baseline = {
    { "schemaVersion", 1 },
    { "source", GetChangeForm().formDesc.ToString() },
    { "owner", requester.GetChangeForm().formDesc.ToString() },
    { "campaignIdentity", CampaignReferenceBinding::GetIdentity(requester) },
    { "instance", GetWorldInstance() },
    { "cell", GetCellOrWorld().ToString() },
    { "position", point(GetPos()) },
    { "angle", point(GetAngle()) },
    { "sequence", saved.at("sequence") },
    { "primitives", roster }
  };
  if (saved != baseline)
    throw std::runtime_error("Recovery baseline identity or geometry changed");
  for (auto& [ref, state] : after) {
    state.ever = true;
    state.occupancy.clear();
    state.inside.clear();
    for (auto* emitter : state.emitters) {
      if (!after.at(emitter).listeners.contains(ref))
        throw std::runtime_error("Recovery subscription is not reciprocal");
      if (emitter->HasPrimitive()) {
        const bool inside = !(detach && ref == this) && !ref->IsDeleted() &&
          (ref == this || !ref->IsDisabled()) &&
          emitter->IsPointInsidePrimitive(ref->GetPos());
        state.occupancy.emplace(emitter, inside);
        if (inside)
          state.inside.insert(emitter);
      }
    }
    for (auto* listener : state.listeners) {
      if (!after.at(listener).emitters.contains(ref))
        throw std::runtime_error("Recovery subscription is not reciprocal");
      if (listener->GetChangeForm().profileId >= 0)
        ensureInitialized(ref);
      if (auto* actor = listener->AsActor(); actor &&
          std::find(state.actors.begin(), state.actors.end(), actor) ==
            state.actors.end())
        state.actors.push_back(actor);
      if (state.pending.erase(listener) && !ref->HasPrimitive())
        prepared->subscriptions.push_back(
          { world->LookupFormByIdNoLoad(ref->GetFormId()),
            world->LookupFormByIdNoLoad(listener->GetFormId()), true });
    }
  }
  for (auto& [ref, state] : after) {
    state.subscriptionsPending = !state.pending.empty();
    for (auto* listener : state.listeners)
      state.subscriptionsPending |=
        !state.initialized && listener->GetChangeForm().profileId >= 0;
    for (auto* emitter : state.emitters)
      state.subscriptionsPending |= after.at(emitter).pending.contains(ref) ||
        (!after.at(emitter).initialized &&
         ref->GetChangeForm().profileId >= 0);
    CampaignMovementPlan::Impl::Graph entry;
    entry.reference = ref;
    entry.before = before.at(ref);
    entry.after = state;
    entry.listeners =
      std::make_unique<std::set<MpObjectReference*>>(state.listeners);
    entry.emitters =
      std::make_unique<std::set<MpObjectReference*>>(state.emitters);
    entry.inside =
      std::make_unique<std::set<MpObjectReference*>>(state.inside);
    entry.occupancy =
      std::make_unique<std::map<MpObjectReference*, bool>>(state.occupancy);
    entry.actors = state.actors;
    entry.pending = state.pending;
    entry.occurrences = state.occurrences;
    prepared->graph.push_back(std::move(entry));
  }
  if (capture)
    capture->Validate(requester);
  return std::shared_ptr<CampaignMovementPlan>(
    new CampaignMovementPlan(std::move(prepared)));
}

void CampaignMovementPlan::ValidateCommitted(MpActor& requester) const
{
  if (pImpl->kind == Kind::Movement || !pImpl->published || !pImpl->receipt ||
      pImpl->receipt->GetState() != SaveReceipt::State::Durable ||
      &requester != pImpl->requester ||
      requester.GetParent() != pImpl->world || pImpl->world->IsClearing() ||
      !pImpl->world->IsStorageRestoreReady() ||
      (pImpl->authority && !pImpl->authority()))
    throw std::runtime_error(
      "Recovery publication is not current and durable");
  for (const auto& read : pImpl->reads) {
    auto* ref = read.lifetime->AsObjectReference();
    if (!ref ||
        pImpl->world->LookupFormByIdNoLoad(ref->GetFormId()) !=
          read.lifetime ||
        ref->GetWorldInstanceEpoch() != read.epoch)
      throw std::runtime_error(
        "Recovery publication participant was replaced");
    const auto current = MpChangeForm::ToJson(ref->GetChangeForm());
    for (const auto* key : { "recType", "formDesc", "baseDesc", "position",
                             "angle", "worldOrCellDesc", "worldInstance",
                             "profileId", "isDeleted", "templateChain" })
      if (current.value(key, nlohmann::json()) !=
          read.snapshot.value(key, nlohmann::json()))
        throw std::runtime_error(
          "Recovery publication physical identity changed");
    if (ref != &requester &&
        current.at("isDisabled") != read.snapshot.at("isDisabled"))
      throw std::runtime_error("Recovery publication visibility changed");
    if (current.at("dynamicFields")
          .value(CampaignReferenceBinding::kProperty, nlohmann::json()) !=
        read.snapshot.at("dynamicFields")
          .value(CampaignReferenceBinding::kProperty, nlohmann::json()))
      throw std::runtime_error("Recovery publication binding changed");
  }
  for (const auto& entry : pImpl->graph)
    if (Impl::Capture(*entry.reference) != entry.after)
      throw std::runtime_error("Recovery publication topology changed");
  if (requester.IsDisabled() != (pImpl->kind == Kind::Detach))
    throw std::runtime_error("Recovery publication enabled state changed");
  const auto current =
    requester.PrepareCampaignResume(requester, pImpl->kind == Kind::Detach);
  if (current->pImpl->baseline != pImpl->baseline ||
      current->pImpl->neighbors != pImpl->neighbors ||
      current->pImpl->targets != pImpl->targets)
    throw std::runtime_error(
      "Recovery publication identity or geometry changed");
}

namespace {
ChangeFormGuard::Mode MakeMode(bool isLocationSaveNeeded,
                               SetPosMode setPosMode)
{
  if (isLocationSaveNeeded) {
    return ChangeFormGuard::Mode::RequestSave;
  }

  switch (setPosMode) {
    case SetPosMode::CalledByUpdateMovement:
      return ChangeFormGuard::Mode::NoRequestSave;
    case SetPosMode::Other:
      return ChangeFormGuard::Mode::RequestSave;
    default:
      spdlog::critical("Invalid SetPosMode");
      std::terminate();
      return ChangeFormGuard::Mode::RequestSave;
  }
}

MpChangeForm MakeChangeForm(const LocationalData& locationalData)
{
  MpChangeForm changeForm;
  changeForm.position = locationalData.pos;
  changeForm.angle = locationalData.rot;
  changeForm.worldOrCellDesc = locationalData.cellOrWorldDesc;
  return changeForm;
}
}

MpObjectReference::MpObjectReference(
  const LocationalData& locationalData_, const FormCallbacks& callbacks_,
  uint32_t baseId_, std::string baseType_,
  std::optional<NiPoint3> primitiveBoundsDiv2)
  : callbacks(new FormCallbacks(callbacks_))
  , baseId(baseId_)
  , baseType(baseType_)
  , ChangeFormGuard(MakeChangeForm(locationalData_), this)
{
  pImpl.reset(new Impl);
  asObjectReference = this;

  if (primitiveBoundsDiv2)
    SetPrimitive(*primitiveBoundsDiv2);
}

const NiPoint3& MpObjectReference::GetPos() const
{
  return ChangeForm().position;
}

const NiPoint3& MpObjectReference::GetAngle() const
{
  return ChangeForm().angle;
}

const FormDesc& MpObjectReference::GetCellOrWorld() const
{
  return ChangeForm().worldOrCellDesc;
}

const uint32_t& MpObjectReference::GetBaseId() const
{
  return baseId;
}

const std::string& MpObjectReference::GetBaseType() const
{
  return baseType;
}

const Inventory& MpObjectReference::GetInventory() const
{
  return ChangeForm().inv;
}

const bool& MpObjectReference::IsHarvested() const
{
  return ChangeForm().isHarvested;
}

const bool& MpObjectReference::IsOpen() const
{
  return ChangeForm().isOpen;
}

const bool& MpObjectReference::IsDisabled() const
{
  return ChangeForm().isDisabled;
}

const bool& MpObjectReference::IsDeleted() const
{
  return ChangeForm().isDeleted;
}

const uint32_t& MpObjectReference::GetCount() const
{
  return ChangeForm().count;
}

std::chrono::system_clock::duration MpObjectReference::GetRelootTime() const
{
  if (relootTimeOverride) {
    return *relootTimeOverride;
  }

  if (auto time = GetParent()->GetRelootTime(baseType)) {
    return *time;
  }

  if (!std::strcmp(baseType.data(), "FLOR") ||
      !std::strcmp(baseType.data(), "TREE")) {
    return std::chrono::hours(1);
  } else if (!std::strcmp(baseType.data(), "DOOR")) {
    return std::chrono::seconds(3);
  } else if (espm::utils::IsItem(espm::Type{ baseType.data() })) {
    return std::chrono::hours(1);
  } else if (!std::strcmp(baseType.data(), "CONT")) {
    return std::chrono::hours(1);
  }

  return std::chrono::hours(0);
}

bool MpObjectReference::GetAnimationVariableBool(const char* name) const
{
  AnimationVariableBool variable = AnimationVariableBool::kInvalidVariable;

  if (!Utils::stricmp(name, "bInJumpState")) {
    variable = AnimationVariableBool::kVariable_bInJumpState;
  } else if (!Utils::stricmp(name, "_skymp_isWeapDrawn")) {
    variable = AnimationVariableBool::kVariable__skymp_isWeapDrawn;
  } else if (!Utils::stricmp(name, "IsBlocking")) {
    variable = AnimationVariableBool::kVariable_IsBlocking;
  } else if (!Utils::stricmp(name, "IsSneaking")) {
    variable = AnimationVariableBool::kVariable_IsSneaking;
  }

  if (variable == AnimationVariableBool::kInvalidVariable) {
    spdlog::warn("MpObjectReference::GetAnimationVariableBool - unknown "
                 "variable name: {}",
                 name);
    return false;
  }

  return pImpl->animGraphHolder.boolVariables[static_cast<size_t>(variable)];
}

bool MpObjectReference::IsPointInsidePrimitive(const NiPoint3& point) const
{
  if (pImpl->primitive) {
    return Primitive::IsInside(point, pImpl->primitive->polygonProc);
  }
  return false;
}

bool MpObjectReference::HasPrimitive() const
{
  return pImpl->primitive.has_value();
}

FormCallbacks MpObjectReference::GetCallbacks() const
{
  if (!callbacks)
    return FormCallbacks::DoNothing();
  return *callbacks;
}

bool MpObjectReference::HasScript(const char* name) const
{
  return ToGameObject()->HasScript(name);
}

bool MpObjectReference::IsActivationBlocked() const
{
  return activationBlocked;
}

bool MpObjectReference::GetTeleportFlag() const
{
  return pImpl->teleportFlag;
}

void MpObjectReference::VisitProperties(CreateActorMessage& message,
                                        VisitPropertiesMode mode)
{
  if (IsHarvested()) {
    message.props.isHarvested = true;
  }

  if (IsOpen()) {
    message.props.isOpen = true;
  }

  if (auto actor = AsActor(); actor && actor->IsDead()) {
    message.props.isDead = true;
  }

  if (mode == VisitPropertiesMode::All && !GetInventory().IsEmpty()) {
    message.props.inventory = GetInventory();
  }

  if (IsEspmForm() && IsDisabled()) {
    message.props.isDisabled = true;
  }

  if (ChangeForm().lastAnimation.has_value()) {
    message.props.lastAnimation = *ChangeForm().lastAnimation;
  }

  if (ChangeForm().setNodeScale.has_value()) {
    std::vector<SetNodeScaleEntry> setNodeScale;
    for (auto& [nodeName, scale] : *ChangeForm().setNodeScale) {
      SetNodeScaleEntry setNodeScaleEntry;
      setNodeScaleEntry.nodeName = nodeName;
      setNodeScaleEntry.scale = scale;
      setNodeScale.push_back(setNodeScaleEntry);
    }
    message.props.setNodeScale = std::move(setNodeScale);
  }

  if (ChangeForm().setNodeTextureSet.has_value()) {
    std::vector<SetNodeTextureSetEntry> setNodeTextureSet;
    for (auto& [nodeName, textureSetId] : *ChangeForm().setNodeTextureSet) {
      SetNodeTextureSetEntry setNodeTextureSetEntry;
      setNodeTextureSetEntry.nodeName = nodeName;
      setNodeTextureSetEntry.textureSetId =
        FormDesc::FromString(textureSetId).ToFormId(GetParent()->espmFiles);
      setNodeTextureSet.push_back(setNodeTextureSetEntry);
    }
    message.props.setNodeTextureSet = std::move(setNodeTextureSet);
  }

  if (ChangeForm().displayName.has_value()) {
    const std::string& raw = *ChangeForm().displayName;
    if (raw != PapyrusObjectReference::kOriginalNameExpression) {
      message.props.displayName = raw;
    }
  }

  // Property flags (isVisibleByOwner, isVisibleByNeighbor) are expected to be
  // checked by a caller (PartOne.cpp in this case)
  ChangeForm().dynamicFields.ForEachValueDump(
    [&](const std::string& propName, const std::string& valueDump) {
      CustomPropsEntry customPropsEntry;
      customPropsEntry.propName = propName;
      customPropsEntry.propValueJsonDump = valueDump;
      message.customPropsJsonDumps.push_back(customPropsEntry);
    });
}

void MpObjectReference::Activate(MpObjectReference& activationSource,
                                 bool defaultProcessingOnly,
                                 bool isSecondActivation)
{
  if (!SharesWorldInstance(activationSource)) {
    throw std::runtime_error("World instance does not match");
  }
  if (spdlog::should_log(spdlog::level::trace)) {
    for (auto& script : ListActivePexInstances()) {
      spdlog::trace("MpObjectReference::Activate {:x} - found script {}",
                    GetFormId(), script->GetSourcePexName());
    }
  }

  if (auto worldState = activationSource.GetParent(); worldState->HasEspm()) {
    CheckInteractionAbility(activationSource);

    // A teleport door must refuse unsupported movement before the activation
    // callback or default processing can change either participant.
    if (!isSecondActivation && (!activationBlocked || defaultProcessingOnly)) {
      const auto& browser = worldState->GetEspm().GetBrowser();
      const auto base = browser.LookupById(GetBaseId());
      const auto* record =
        espm::Convert<espm::REFR>(browser.LookupById(GetFormId()).rec);
      if (base.rec && base.rec->GetType() == espm::DOOR::kType && record &&
          record->GetData(worldState->GetEspmCache()).teleport)
        RequirePositionMutation(activationSource);
    }

    // Pillars puzzle Bleak Falls Barrow
    bool workaroundBypassParentsCheck = &activationSource == this;

    // Block if only activation parents can activate this
    auto refrId = GetFormId();
    if (!workaroundBypassParentsCheck && IsEspmForm() && !AsActor()) {
      auto lookupRes = worldState->GetEspm().GetBrowser().LookupById(refrId);
      auto data = espm::GetData<espm::REFR>(refrId, worldState);
      auto it = std::find_if(
        data.activationParents.begin(), data.activationParents.end(),
        [&](const espm::REFR::ActivationParentInfo& info) {
          return lookupRes.ToGlobalId(info.refrId) ==
            activationSource.GetFormId();
        });
      if (it == data.activationParents.end()) {
        if (data.isParentActivationOnly) {
          throw std::runtime_error(
            "Only activation parents can activate this object");
        }
      }
    }
  }

  if (isSecondActivation) {
    bool processed = ProcessActivateSecond(activationSource);
    if (processed) {
      auto arg = activationSource.ToVarValue();
      SendPapyrusEvent("SkympOnActivateClose", &arg, 1);
    }
  } else {
    ActivateEvent activateEvent(GetFormId(), activationSource.GetFormId());

    bool activationBlockedByMpApi = !activateEvent.Fire(GetParent());

    if (!activationBlockedByMpApi &&
        (!activationBlocked || defaultProcessingOnly)) {
      ProcessActivateNormal(activationSource);
      ActivateChilds();
    } else {
      spdlog::trace(
        "Activation of form {:#x} has been blocked. Reasons: "
        "blocked by MpApi={}, form is blocked={}, defaultProcessingOnly={}",
        GetFormId(), activationBlockedByMpApi, activationBlocked,
        defaultProcessingOnly);
    }

    if (!defaultProcessingOnly && !activationBlockedByMpApi) {
      auto arg = activationSource.ToVarValue();
      SendPapyrusEvent("OnActivate", &arg, 1);
    }
  }
}

void MpObjectReference::Disable()
{
  if (ChangeForm().isDisabled) {
    return;
  }

  RequireCampaignSpatialMutation();
  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.isDisabled = true; });

  if (!IsEspmForm() || AsActor()) {
    RemoveFromGridAndUnsubscribeAll();
  }
}

void MpObjectReference::Enable()
{
  if (!ChangeForm().isDisabled) {
    return;
  }

  RequireCampaignSpatialMutation();
  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.isDisabled = false; });

  if (!IsEspmForm() || AsActor()) {
    ForceSubscriptionsUpdate();
  }
}

void MpObjectReference::ValidateCampaignVisibilityChange() const
{
  RequireCampaignSpatialMutation();
  if (occupant)
    throw std::runtime_error(
      "Occupied references require transactional occupancy changes");
}

void MpObjectReference::ReconcileCampaignVisibility()
{
  if (IsDisabled()) {
    RemoveFromGridAndUnsubscribeAll();
    UnsubscribeFromAll();
  } else {
    ForceSubscriptionsUpdate();
  }
}

void MpObjectReference::SetPos(const NiPoint3& newPos, SetPosMode setPosMode)
{
  RequirePositionMutation(*this);
  auto* world = GetParent();
  const auto lifetime =
    world ? world->LookupFormByIdNoLoad(GetFormId()) : nullptr;
  const auto epoch = GetWorldInstanceEpoch();
  const auto cell = GetCellOrWorld();
  const auto oldPosition = GetPos();
  const auto requestedPosition = newPos;
  const auto current = [&](const NiPoint3& position) {
    return !world ||
      (world->LookupFormByIdNoLoad(GetFormId()) == lifetime &&
       lifetime.get() == this && GetWorldInstanceEpoch() == epoch &&
       GetCellOrWorld() == cell && GetPos() == position);
  };
  const bool restoring = world && world->IsRestoring();
  if (!restoring && pImpl->subscriptionsPending)
    ForceSubscriptionsUpdate();
  if (!current(oldPosition))
    return;

  auto oldGridPos = GetGridPos(ChangeForm().position);
  auto newGridPos = GetGridPos(requestedPosition);

  EditChangeForm(
    [&requestedPosition](MpChangeFormREFR& changeForm) {
      changeForm.position = requestedPosition;
    },
    MakeMode(IsLocationSavingNeeded(), setPosMode));

  if (oldGridPos != newGridPos || !everSubscribedOrListened)
    ForceSubscriptionsUpdate();
  if (!current(requestedPosition))
    return;

  if (restoring) {
    RestorePrimitiveOccupancy();
    return;
  }

  if (!IsDisabled()) {
    if (emittersWithPrimitives) {
      if (!primitivesWeAreInside) {
        primitivesWeAreInside.reset(new std::set<MpObjectReference*>);
      }

      for (auto& [emitterRefr, wasInside] : *emittersWithPrimitives) {
        bool inside = emitterRefr->IsPointInsidePrimitive(requestedPosition);
        if (wasInside != inside) {
          wasInside = inside;
          auto me = ToVarValue();

          auto wst = GetParent();
          auto id = emitterRefr->GetFormId();
          auto myId = GetFormId();
          const auto sourceLifetime = wst->LookupFormByIdNoLoad(myId);
          const auto emitterLifetime = wst->LookupFormByIdNoLoad(id);
          const auto sourceEpoch = GetWorldInstanceEpoch();
          const auto emitterEpoch = emitterRefr->GetWorldInstanceEpoch();
          wst->SetTimer(std::chrono::seconds(0))
            .Then([wst, id, inside, me, myId, this, sourceLifetime,
                   emitterLifetime, sourceEpoch, emitterEpoch](Viet::Void) {
              auto& emitter = wst->LookupFormByIdNoLoad(id);
              MpObjectReference* emitterRefr =
                emitter ? emitter->AsObjectReference() : nullptr;
              // Delayed movement events must not cross a phase transition or
              // target a replacement which reused either native form ID.
              if (wst->LookupFormByIdNoLoad(myId) != sourceLifetime ||
                  emitter != emitterLifetime || !emitterRefr ||
                  GetWorldInstanceEpoch() != sourceEpoch ||
                  emitterRefr->GetWorldInstanceEpoch() != emitterEpoch ||
                  !SharesWorldInstance(*emitterRefr) ||
                  UsesCampaignMovement()) {
                return;
              }
              emitterRefr->SendPapyrusEvent(
                inside ? "OnTriggerEnter" : "OnTriggerLeave", &me, 1);
            });

          if (inside) {
            primitivesWeAreInside->insert(emitterRefr);
          } else {
            primitivesWeAreInside->erase(emitterRefr);
          }
        }
      }
    }

    if (primitivesWeAreInside) {
      auto me = ToVarValue();

      std::vector<std::shared_ptr<MpForm>> set;
      for (auto* emitter : *primitivesWeAreInside)
        set.push_back(world->LookupFormByIdNoLoad(emitter->GetFormId()));
      for (const auto& emitter : set) {
        if (!current(requestedPosition))
          return;
        auto* ref = emitter ? emitter->AsObjectReference() : nullptr;
        if (ref && world->LookupFormByIdNoLoad(ref->GetFormId()) == emitter &&
            primitivesWeAreInside->count(ref) && SharesWorldInstance(*ref))
          ref->SendPapyrusEvent("OnTrigger", &me, 1);
      }
    }
  }
}

void MpObjectReference::SetAngle(const NiPoint3& newAngle,
                                 SetAngleMode setAngleMode)
{
  RequireCampaignSpatialMutation();
  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.angle = newAngle; },
    MakeMode(IsLocationSavingNeeded(), setAngleMode));
}

void MpObjectReference::SetHarvested(bool harvested)
{
  if (harvested != ChangeForm().isHarvested) {
    EditChangeForm([&](MpChangeFormREFR& changeForm) {
      changeForm.isHarvested = harvested;
    });
    SendMessageToActorListeners(
      CreatePropertyMessage_(this, "isHarvested",
                             harvested ? "true" : "false"),
      true);
  }
}

void MpObjectReference::SetOpen(bool open)
{
  if (open != ChangeForm().isOpen) {
    EditChangeForm(
      [&](MpChangeFormREFR& changeForm) { changeForm.isOpen = open; });
    SendMessageToActorListeners(
      CreatePropertyMessage_(this, "isOpen", open ? "true" : "false"), true);
  }
}

void MpObjectReference::PutItem(MpActor& ac, const Inventory::Entry& e)
{
  CheckInteractionAbility(ac);
  if (this->occupant != &ac) {
    std::stringstream err;
    err << std::hex << "Actor 0x" << ac.GetFormId() << " doesn't occupy ref 0x"
        << GetFormId();
    throw std::runtime_error(err.str());
  }

  PutItemEvent putItemEvent(&ac, this, e);
  putItemEvent.Fire(GetParent());
}

void MpObjectReference::TakeItem(MpActor& ac, const Inventory::Entry& e)
{
  CheckInteractionAbility(ac);
  if (this->occupant != &ac) {
    std::stringstream err;
    err << std::hex << "Actor 0x" << ac.GetFormId() << " doesn't occupy ref 0x"
        << GetFormId();
    throw std::runtime_error(err.str());
  }

  TakeItemEvent takeItemEvent(&ac, this, e);
  takeItemEvent.Fire(GetParent());
}

void MpObjectReference::SetRelootTime(
  std::chrono::system_clock::duration newRelootTime)
{
  relootTimeOverride = newRelootTime;
}

void MpObjectReference::SetChanceNoneOverride(uint8_t newChanceNone)
{
  chanceNoneOverride.reset(new uint8_t(newChanceNone));
}

const std::string& MpObjectReference::GetWorldInstance() const
{
  return ChangeForm().worldInstance;
}

bool MpObjectReference::SharesWorldInstance(
  const MpObjectReference& other) const
{
  return GetWorldInstance() == other.GetWorldInstance();
}

void MpObjectReference::SetWorldInstance(const std::string& instance)
{
  if (instance.size() > 128) {
    throw std::invalid_argument("worldInstance exceeds 128 bytes");
  }
  if (instance == GetWorldInstance()) {
    return;
  }
  RequireCampaignSpatialMutation();
  ++worldInstanceEpoch;
  EditChangeForm([&](MpChangeForm& form) { form.worldInstance = instance; });
  if (auto world = GetParent()) {
    // Revoke leases in both directions before accepting further packets.
    for (auto it = world->hosters.begin(); it != world->hosters.end();) {
      if (it->first == GetFormId() || it->second == GetFormId()) {
        it = world->hosters.erase(it);
      } else {
        ++it;
      }
    }
  }
  ForceSubscriptionsUpdate();
}

void MpObjectReference::SetCellOrWorld(const FormDesc& newWorldOrCell)
{
  SetCellOrWorldObsolete(newWorldOrCell);
  ForceSubscriptionsUpdate();
}

void MpObjectReference::SetActivationBlocked(bool blocked)
{
  // TODO: Save
  activationBlocked = blocked;
}

void MpObjectReference::ForceSubscriptionsUpdate()
{
  RequireCampaignSpatialMutation();
  auto worldState = GetParent();
  if (!worldState || IsDisabled() || IsDeleted()) {
    return;
  }
  const auto lifetime = worldState->LookupFormByIdNoLoad(GetFormId());
  if (lifetime.get() != this)
    return;
  const auto epoch = GetWorldInstanceEpoch();
  const auto position = GetPos();
  const auto cell = GetCellOrWorld();
  const auto current = [&] {
    return worldState->LookupFormByIdNoLoad(GetFormId()) == lifetime &&
      GetWorldInstanceEpoch() == epoch && GetPos() == position &&
      GetCellOrWorld() == cell && !IsDisabled() && !IsDeleted();
  };
  InitListenersAndEmitters();

  auto worldOrCell = GetCellOrWorld().ToFormId(worldState->espmFiles);

  auto& gridInfo = worldState->grids[worldOrCell];
  MoveOnGrid(*gridInfo.grid);

  auto& was = *this->listeners;
  auto pos = GetGridPos(GetPos());
  const auto& nearby =
    worldState->GetNeighborsByPosition(worldOrCell, pos.first, pos.second);
  if (!current())
    return;
  std::set<MpObjectReference*> now;
  for (auto candidate : nearby) {
    if (!candidate->IsDisabled() && !candidate->IsDeleted() &&
        SharesWorldInstance(*candidate)) {
      now.insert(candidate);
    }
  }

  std::vector<MpObjectReference*> toRemove;
  std::set_difference(was.begin(), was.end(), now.begin(), now.end(),
                      std::inserter(toRemove, toRemove.begin()));
  // Callbacks may destroy another neighbour before its turn. Pin the entire
  // pass and check registration again before using any candidate.
  struct Neighbor
  {
    std::shared_ptr<MpForm> lifetime;
    uint64_t epoch;
    NiPoint3 position;
    FormDesc cell;
  };
  std::map<MpObjectReference*, Neighbor> lifetimes;
  const auto pin = [&](MpObjectReference* ref) {
    lifetimes.emplace(
      ref,
      Neighbor{ worldState->LookupFormByIdNoLoad(ref->GetFormId()),
                ref->GetWorldInstanceEpoch(), ref->GetPos(),
                ref->GetCellOrWorld() });
  };
  for (auto* ref : now)
    pin(ref);
  for (auto* ref : toRemove)
    pin(ref);
  const auto registered = [&](MpObjectReference* ref) {
    const auto& saved = lifetimes.at(ref);
    return saved.lifetime.get() == ref &&
      worldState->LookupFormByIdNoLoad(ref->GetFormId()) == saved.lifetime &&
      ref->GetWorldInstanceEpoch() == saved.epoch &&
      ref->GetPos() == saved.position && ref->GetCellOrWorld() == saved.cell;
  };
  for (auto listener : toRemove) {
    if (!current())
      return;
    if (!registered(listener))
      continue;
    Unsubscribe(this, listener);
    // Unsubscribe from self is NEEDED. See comment below
    if (current() && registered(listener) && this != listener)
      Unsubscribe(listener, this);
  }

  for (auto listener : now) {
    if (!current())
      return;
    if (!registered(listener))
      continue;
    if (worldState->IsRestoring() || !was.count(listener) ||
        pImpl->pendingListeners.count(listener) ||
        (!pImpl->onInitEventSent && listener->ChangeForm().profileId != -1))
      Subscribe(this, listener);
    // Note: Self-subscription is OK this check is performed as we don't want
    // to self-subscribe twice! We have already been subscribed to self in
    // the last line of code
    if (current() && registered(listener) && this != listener &&
        (worldState->IsRestoring() || !listener->GetListeners().count(this) ||
         listener->pImpl->pendingListeners.count(this) ||
         (!listener->pImpl->onInitEventSent && ChangeForm().profileId != -1)))
      Subscribe(listener, this);
  }

  if (!current())
    return;
  everSubscribedOrListened = true;
  if (!worldState->IsRestoring())
    pImpl->subscriptionsPending = HasPendingSubscriptions();
}

bool MpObjectReference::HasPendingSubscriptions() const
{
  if (!pImpl->pendingListeners.empty())
    return true;
  if (!pImpl->onInitEventSent)
    for (auto* listener : GetListeners())
      if (listener->ChangeForm().profileId != -1)
        return true;
  for (auto* emitter : GetEmitters())
    if (emitter->pImpl->pendingListeners.count(
          const_cast<MpObjectReference*>(this)) ||
        (!emitter->pImpl->onInitEventSent && ChangeForm().profileId != -1))
      return true;
  return false;
}

void MpObjectReference::RestorePrimitiveOccupancy()
{
  if (!emittersWithPrimitives)
    return;
  if (!primitivesWeAreInside)
    primitivesWeAreInside.reset(new std::set<MpObjectReference*>);
  for (auto& [emitter, inside] : *emittersWithPrimitives) {
    if (GetDynamicFields().GetValueDump(CampaignMovementPlan::kProperty) !=
        "null") {
      pImpl->campaignMovementEnrolled = true;
      emitter->pImpl->campaignMovementEnrolled = true;
    }
    inside = !IsDisabled() && !IsDeleted() && !emitter->IsDisabled() &&
      !emitter->IsDeleted() && SharesWorldInstance(*emitter) &&
      emitter->IsPointInsidePrimitive(GetPos());
    if (inside)
      primitivesWeAreInside->insert(emitter);
    else
      primitivesWeAreInside->erase(emitter);
  }
}

void MpObjectReference::SetPrimitive(const NiPoint3& boundsDiv2)
{
  RequireCampaignSpatialMutation();
  auto vertices = Primitive::GetVertices(GetPos(), GetAngle(), boundsDiv2);
  pImpl->primitive =
    PrimitiveData{ boundsDiv2, Primitive::CreateGeoPolygonProc(vertices) };
}

void MpObjectReference::UpdateHoster(uint32_t newHosterId)
{
  auto hostedMsg = CreatePropertyMessage_(this, "isHostedByOther", "true");
  auto notHostedMsg = CreatePropertyMessage_(this, "isHostedByOther", "false");
  for (auto listener : this->GetActorListeners()) {
    if (newHosterId != 0 && newHosterId != listener->GetFormId()) {
      listener->GetActorToSendTo().SendToUser(hostedMsg, true);
    } else {
      listener->GetActorToSendTo().SendToUser(notHostedMsg, true);
    }
  }
}

void MpObjectReference::SetPropertyValueDump(const std::string& propertyName,
                                             const std::string& valueDump,
                                             bool isVisibleByOwner,
                                             bool isVisibleByNeighbor)
{
  if (propertyName == CampaignMovementPlan::kProperty)
    throw std::runtime_error(
      "Campaign movement baseline requires atomic publication");
  auto msg = CreatePropertyMessage_(this, propertyName.c_str(), valueDump);
  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.dynamicFields.SetValueDump(propertyName, valueDump);
  });
  if (isVisibleByNeighbor) {
    SendMessageToActorListeners(msg, true);
  } else if (isVisibleByOwner) {
    if (auto ac = AsActor()) {
      ac->GetActorToSendTo().SendToUser(msg, true);
    }
  }
  pImpl->setPropertyCalled = true;
}

void MpObjectReference::SetTeleportFlag(bool value)
{
  pImpl->teleportFlag = value;
}

void MpObjectReference::SetPosAndAngleSilent(const NiPoint3& pos,
                                             const NiPoint3& rot)
{
  RequirePositionMutation(*this);
  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) {
      changeForm.position = pos;
      changeForm.angle = rot;
    },
    Mode::NoRequestSave);
}

void MpObjectReference::Delete()
{
  RequireCampaignSpatialMutation();
  if (IsEspmForm()) {
    spdlog::warn("MpObjectReference::Delete {:x} - Attempt to delete non-FF "
                 "object, ignoring",
                 GetFormId());
    return;
  }

  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.isDeleted = true; });
  RemoveFromGridAndUnsubscribeAll();
}

void MpObjectReference::SetCount(uint32_t newCount)
{
  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.count = newCount; });
}

void MpObjectReference::SetAnimationVariableBool(
  AnimationVariableBool animationVariableBool, bool value)
{
  auto i = static_cast<size_t>(animationVariableBool);
  pImpl->animGraphHolder.boolVariables[i] = value;
}

void MpObjectReference::SetInventory(const Inventory& inv)
{
  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.baseContainerAdded = true;
    changeForm.inv = inv;
  });
  SendInventoryUpdate();
}

void MpObjectReference::AddItem(uint32_t baseId, uint32_t count)
{
  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.baseContainerAdded = true;
    changeForm.inv.AddItem(baseId, count);
  });
  SendInventoryUpdate();

  if (auto worldState = GetParent(); worldState->HasEspm()) {
    espm::LookupResult lookupRes =
      worldState->GetEspm().GetBrowser().LookupById(baseId);

    if (lookupRes.rec) {
      auto baseItem = VarValue(std::make_shared<EspmGameObject>(lookupRes));
      auto itemCount = VarValue(static_cast<int32_t>(count));
      auto itemReference =
        VarValue(static_cast<IGameObject*>(nullptr)); // TODO
      auto sourceContainer =
        VarValue(static_cast<IGameObject*>(nullptr)); // TODO
      VarValue args[4] = { baseItem, itemCount, itemReference,
                           sourceContainer };
      SendPapyrusEvent("OnItemAdded", args, 4);
    } else {
      spdlog::warn("MpObjectReference::AddItem - failed to lookup item {:x}",
                   baseId);
    }
  }
}

void MpObjectReference::AddItems(const std::vector<Inventory::Entry>& entries)
{
  if (entries.size() > 0) {
    EditChangeForm([&](MpChangeFormREFR& changeForm) {
      changeForm.baseContainerAdded = true;
      changeForm.inv.AddItems(entries);
    });
    SendInventoryUpdate();
  }

  // for (const auto& entri : entries) {
  //   auto baseItem = VarValue(static_cast<int32_t>(entri.baseId));
  //   auto itemCount = VarValue(static_cast<int32_t>(entri.count));
  //   auto itemReference = VarValue((IGameObject*)nullptr);
  //   auto sourceContainer = VarValue((IGameObject*)nullptr);
  //   VarValue args[4] = { baseItem, itemCount, itemReference,
  //   sourceContainer
  //   }; SendPapyrusEvent("OnItemAdded", args, 4);
  // }
}

void MpObjectReference::RemoveItem(uint32_t baseId, uint32_t count,
                                   MpObjectReference* target)
{
  RemoveItems({ { baseId, count } }, target);
}

void MpObjectReference::RemoveItems(
  const std::vector<Inventory::Entry>& entries, MpObjectReference* target)
{
  if (target && !SharesWorldInstance(*target)) {
    throw std::runtime_error(
      "Cannot transfer inventory across world instances");
  }
  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.inv.RemoveItems(entries);
  });

  if (target)
    target->AddItems(entries);

  SendInventoryUpdate();

  if (GetBaseType() == "CONT") {
    if (GetInventory().IsEmpty()) {
      spdlog::info("MpObjectReference::RemoveItems - {:x} requesting reloot",
                   this->GetFormId());
      RequestReloot();
    }
  }
}

void MpObjectReference::RemoveAllItems(MpObjectReference* target)
{
  auto prevInv = GetInventory();
  RemoveItems(prevInv.entries, target);
}

void MpObjectReference::RelootContainer()
{
  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) {
      changeForm.baseContainerAdded = false;
    },
    Mode::NoRequestSave);
  EnsureBaseContainerAdded(*GetParent()->espm);
}

void MpObjectReference::RegisterProfileId(int32_t profileId)
{
  auto worldState = GetParent();
  if (!worldState) {
    throw std::runtime_error("Not attached to WorldState");
  }

  if (profileId < 0) {
    throw std::runtime_error(
      "Negative profileId passed to RegisterProfileId, should be >= 0");
  }

  auto currentProfileId = ChangeForm().profileId;
  auto formId = GetFormId();

  if (currentProfileId > 0) {
    worldState->actorIdByProfileId[currentProfileId].erase(formId);
  }

  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.profileId = profileId; });

  if (currentProfileId == -1) {
    // Existing spatial edges may already be published as NPC relationships.
    // Registration makes their load events eligible on the next real refresh.
    pImpl->subscriptionsPending = true;
    for (auto* emitter : GetEmitters())
      emitter->pImpl->subscriptionsPending = true;
  }

  if (profileId > 0) {
    worldState->actorIdByProfileId[profileId].insert(formId);
  }
}

void MpObjectReference::RegisterPrivateIndexedProperty(
  const std::string& propertyName, const std::string& propertyValueStringified)
{
  auto worldState = GetParent();
  if (!worldState) {
    throw std::runtime_error("Not attached to WorldState");
  }

  bool (*isNull)(const std::string&) = [](const std::string& s) {
    return s == "null" || s == "undefined" || s == "" || s == "''" ||
      s == "\"\"";
  };

  auto currentValueStringified =
    ChangeForm().dynamicFields.GetValueDump(propertyName);
  auto formId = GetFormId();
  if (!isNull(currentValueStringified)) {
    auto key = worldState->MakePrivateIndexedPropertyMapKey(
      propertyName, currentValueStringified);
    worldState->actorIdByPrivateIndexedProperty[key].erase(formId);
    spdlog::trace("MpObjectReference::RegisterPrivateIndexedProperty {:x} - "
                  "unregister {}",
                  formId, key);
  }

  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.dynamicFields.SetValueDump(propertyName,
                                          propertyValueStringified);
  });

  if (!isNull(propertyValueStringified)) {
    auto key = worldState->MakePrivateIndexedPropertyMapKey(
      propertyName, propertyValueStringified);
    worldState->actorIdByPrivateIndexedProperty[key].insert(formId);
    spdlog::trace(
      "MpObjectReference::RegisterPrivateIndexedProperty {:x} - register {}",
      formId, key);
  }
}

void MpObjectReference::Subscribe(MpObjectReference* emitter,
                                  MpObjectReference* listener)
{
  if (!emitter->SharesWorldInstance(*listener) || emitter->IsDisabled() ||
      emitter->IsDeleted() || listener->IsDisabled() ||
      listener->IsDeleted()) {
    return;
  }
  auto actorEmitter = emitter->AsActor();
  auto actorListener = listener->AsActor();
  if (!actorEmitter && !actorListener) {
    return;
  }

  auto* world = emitter->GetParent();
  if (!world || listener->GetParent() != world)
    return;
  const auto emitterId = emitter->GetFormId();
  const auto listenerId = listener->GetFormId();
  const auto emitterLifetime = world->LookupFormByIdNoLoad(emitterId);
  const auto listenerLifetime = world->LookupFormByIdNoLoad(listenerId);
  if (emitterLifetime.get() != emitter || listenerLifetime.get() != listener)
    return;
  const auto emitterEpoch = emitter->GetWorldInstanceEpoch();
  const auto listenerEpoch = listener->GetWorldInstanceEpoch();
  if (!emitter->pImpl->destroying && !listener->pImpl->destroying) {
    emitter->RequireCampaignSpatialMutation();
    listener->RequireCampaignSpatialMutation();
  }
  const bool restoring = world->IsRestoring();
  const bool hasPrimitive = emitter->HasPrimitive();
  emitter->InitListenersAndEmitters();
  listener->InitListenersAndEmitters();

  if (!emitter->listeners->count(listener)) {
    if (emitter->pImpl->nextPublication ==
        std::numeric_limits<uint64_t>::max())
      throw std::overflow_error("Subscription publication sequence exhausted");
    const auto occurrence = emitter->pImpl->nextPublication + 1;
    try {
      emitter->pImpl->listenerOccurrences.emplace(listener, occurrence);
      emitter->pImpl->pendingListeners.emplace(listener, occurrence);
      emitter->listeners->insert(listener);
      listener->emitters->insert(emitter);
    } catch (...) {
      emitter->pImpl->listenerOccurrences.erase(listener);
      emitter->pImpl->pendingListeners.erase(listener);
      emitter->listeners->erase(listener);
      listener->emitters->erase(emitter);
      throw;
    }
    emitter->pImpl->nextPublication = occurrence;
    emitter->pImpl->subscriptionsPending = true;
    listener->pImpl->subscriptionsPending = true;
  }
  if (hasPrimitive) {
    if (!listener->emittersWithPrimitives)
      listener->emittersWithPrimitives.reset(
        new std::map<MpObjectReference*, bool>);
    listener->emittersWithPrimitives->insert({ emitter, false });
    if (restoring)
      listener->RestorePrimitiveOccupancy();
  }
  const bool pendingPublication =
    emitter->pImpl->pendingListeners.count(listener) != 0;
  const bool needsLoad =
    !emitter->pImpl->onInitEventSent && listener->ChangeForm().profileId != -1;
  if (restoring || (!pendingPublication && !needsLoad))
    return;
  const auto occurrence = emitter->pImpl->listenerOccurrences.at(listener);
  if (!emitter->pImpl->publishingListeners.insert(listener).second)
    return;
  const auto sameSubscription = [&] {
    const auto it = emitter->pImpl->listenerOccurrences.find(listener);
    return world->LookupFormByIdNoLoad(emitterId) == emitterLifetime &&
      world->LookupFormByIdNoLoad(listenerId) == listenerLifetime &&
      emitter->GetWorldInstanceEpoch() == emitterEpoch &&
      listener->GetWorldInstanceEpoch() == listenerEpoch &&
      !emitter->IsDisabled() && !emitter->IsDeleted() &&
      !listener->IsDisabled() && !listener->IsDeleted() &&
      it != emitter->pImpl->listenerOccurrences.end() &&
      it->second == occurrence;
  };

  struct Publication
  {
    Impl& state;
    MpObjectReference* listener;
  } publication{ *emitter->pImpl, listener };
  Viet::ScopedTask<Publication> guard(
    [](Publication& value) {
      value.state.publishingListeners.erase(value.listener);
    },
    publication);

  if (needsLoad) {
    if (emitter->GetDynamicFields().GetValueDump(
          CampaignReferenceBinding::kProperty) != "null") {
      const auto [owner, original] =
        CampaignReferenceBinding::Context(*emitter);
      QuestScriptRuntime::ExecuteReferenceLoad(*owner, original);
      emitter->pImpl->onInitEventSent = true;
    } else {
      emitter->pImpl->onInitEventSent = true;
      emitter->SendPapyrusEvent("OnInit");
      if (!sameSubscription())
        return;
      emitter->SendPapyrusEvent("OnCellLoad");
      if (!sameSubscription())
        return;
      emitter->SendPapyrusEvent("OnLoad");
    }
  }

  // A load handler may have disabled or moved either endpoint. Do not publish
  // a relationship it removed while executing.
  if (!sameSubscription() || !emitter->listeners->count(listener) ||
      !emitter->SharesWorldInstance(*listener))
    return;
  if (!pendingPublication)
    return;
  if (!hasPrimitive)
    emitter->callbacks->subscribe(emitter, listener);
  if (!sameSubscription())
    return;
  if (actorListener)
    emitter->actorListenerArray.push_back(actorListener);
  emitter->pImpl->pendingListeners.erase(listener);
}

void MpObjectReference::Unsubscribe(MpObjectReference* emitter,
                                    MpObjectReference* listener)
{
  if (!emitter->pImpl->destroying && !listener->pImpl->destroying) {
    emitter->RequireCampaignSpatialMutation();
    listener->RequireCampaignSpatialMutation();
  }
  auto actorEmitter = emitter->AsActor();
  auto actorListener = listener->AsActor();
  bool bothNonActors = !actorEmitter && !actorListener;
  if (bothNonActors) {
    return;
  }

  const bool hasPrimitive = emitter->HasPrimitive();

  auto* world = emitter->GetParent();
  const auto emitterLifetime =
    world->LookupFormByIdNoLoad(emitter->GetFormId());
  const auto listenerLifetime =
    world->LookupFormByIdNoLoad(listener->GetFormId());
  const bool pending = emitter->pImpl->pendingListeners.erase(listener) > 0;
  emitter->pImpl->listenerOccurrences.erase(listener);
  const bool publish = !hasPrimitive && !pending && !world->IsRestoring() &&
    emitter->GetListeners().count(listener);

  if (!emitter->listeners || !listener->emitters)
    return;
  size_t numElementsErased = emitter->listeners->erase(listener);

  if (actorListener && numElementsErased > 0) {
    emitter->actorListenerArray.erase(
      std::remove(emitter->actorListenerArray.begin(),
                  emitter->actorListenerArray.end(), actorListener),
      emitter->actorListenerArray.end());
  }

  listener->emitters->erase(emitter);

  if (listener->emittersWithPrimitives && hasPrimitive) {
    listener->emittersWithPrimitives->erase(emitter);
  }
  if (listener->primitivesWeAreInside)
    listener->primitivesWeAreInside->erase(emitter);
  if (publish)
    emitter->callbacks->unsubscribe(emitter, listener);
}

void MpObjectReference::SetLastAnimation(const std::string& lastAnimation)
{
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.lastAnimation = lastAnimation;
  });
}

void MpObjectReference::SetNodeTextureSet(const std::string& node,
                                          const espm::LookupResult& textureSet,
                                          bool firstPerson)
{
  // This only changes var in database, SpSnippet is being sent in
  // PapyrusNetImmerse.cpp
  EditChangeForm([&](MpChangeForm& changeForm) {
    if (changeForm.setNodeTextureSet == std::nullopt) {
      changeForm.setNodeTextureSet = std::map<std::string, std::string>();
    }

    uint32_t textureSetId = textureSet.ToGlobalId(textureSet.rec->GetId());

    FormDesc textureSetFormDesc =
      FormDesc::FromFormId(textureSetId, GetParent()->espmFiles);

    changeForm.setNodeTextureSet->insert_or_assign(
      node, textureSetFormDesc.ToString());
  });
}

void MpObjectReference::SetNodeScale(const std::string& node, float scale,
                                     bool firstPerson)
{
  // This only changes var in database, SpSnippet is being sent in
  // PapyrusNetImmerse.cpp
  EditChangeForm([&](MpChangeForm& changeForm) {
    if (changeForm.setNodeScale == std::nullopt) {
      changeForm.setNodeScale = std::map<std::string, float>();
    }
    changeForm.setNodeScale->insert_or_assign(node, scale);
  });
}

void MpObjectReference::SetDisplayName(
  const std::optional<std::string>& newName)
{
  EditChangeForm(
    [&](MpChangeForm& changeForm) { changeForm.displayName = newName; });
}

const std::set<MpObjectReference*>& MpObjectReference::GetListeners() const
{
  static const std::set<MpObjectReference*> kEmptyListeners;
  return listeners ? *listeners : kEmptyListeners;
}

const std::vector<MpActor*>& MpObjectReference::GetActorListeners()
  const noexcept
{
  return actorListenerArray;
}

const std::set<MpObjectReference*>& MpObjectReference::GetEmitters() const
{
  static const std::set<MpObjectReference*> kEmptyEmitters;
  return emitters ? *emitters : kEmptyEmitters;
}

void MpObjectReference::RequestReloot(
  std::optional<std::chrono::system_clock::duration> time)
{
  if (!IsEspmForm()) {
    return;
  }

  if (GetParent()->IsRelootForbidden(baseType)) {
    return;
  }

  if (!time)
    time = GetRelootTime();

  if (!ChangeForm().nextRelootDatetime) {
    EditChangeForm([&](MpChangeFormREFR& changeForm) {
      changeForm.nextRelootDatetime = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now() + GetRelootTime());
    });

    GetParent()->RequestReloot(*this, *time);
  }
}

void MpObjectReference::DoReloot()
{
  if (ChangeForm().nextRelootDatetime) {
    EditChangeForm([&](MpChangeFormREFR& changeForm) {
      changeForm.nextRelootDatetime = 0;
    });
    SetOpen(false);
    SetHarvested(false);
    RelootContainer();
  }
}

std::shared_ptr<std::chrono::time_point<std::chrono::system_clock>>
MpObjectReference::GetNextRelootMoment() const
{
  std::shared_ptr<std::chrono::time_point<std::chrono::system_clock>> res;
  if (ChangeForm().nextRelootDatetime) {
    res.reset(new std::chrono::time_point<std::chrono::system_clock>(
      std::chrono::system_clock::from_time_t(
        ChangeForm().nextRelootDatetime)));
  }
  return res;
}

MpChangeForm MpObjectReference::GetChangeForm() const
{
  MpChangeForm res = ChangeForm();

  if (GetParent() && !GetParent()->espmFiles.empty()) {
    res.formDesc = FormDesc::FromFormId(GetFormId(), GetParent()->espmFiles);
    res.baseDesc = FormDesc::FromFormId(GetBaseId(), GetParent()->espmFiles);
  } else {
    res.formDesc = res.baseDesc = FormDesc(GetFormId(), "");
  }

  return res;
}

void MpObjectReference::CommitPreparedCampaignChange(
  MpChangeForm& prepared) noexcept
{
  SwapPreparedCampaignFields(prepared);
  pImpl->setPropertyCalled = true;
}

void MpObjectReference::ApplyChangeForm(const MpChangeForm& changeForm)
{
  RequireCampaignSpatialMutation();
  ANTIGO_CONTEXT_INIT(ctx);
  auto* world = GetParent();
  std::optional<WorldState::RestorationScope> restoration;
  if (world)
    restoration.emplace(*world);

  if (pImpl->setPropertyCalled) {
    GetParent()->logger->critical(
      "ApplyChangeForm called after SetProperty\n{}",
      ctx.Resolve().ToString());
    // std::terminate();
  }

  blockSaving = true;
  Viet::ScopedTask<MpObjectReference> unblockTask(
    [](MpObjectReference& self) { self.blockSaving = false; }, *this);

  const auto currentBaseId = GetBaseId();
  const auto newBaseId = changeForm.baseDesc.ToFormId(
    world ? world->espmFiles : std::vector<std::string>{});
  if (currentBaseId != newBaseId) {
    spdlog::error("Anomaly, baseId should never change ({:x} => {:x})",
                  currentBaseId, newBaseId);
  }

  if (ChangeForm().formDesc != changeForm.formDesc) {
    throw std::runtime_error("Expected formDesc to be " +
                             ChangeForm().formDesc.ToString() +
                             ", but found " + changeForm.formDesc.ToString());
  }

  const bool spatialChange = GetPos() != changeForm.position ||
    GetAngle() != changeForm.angle ||
    GetCellOrWorld() != changeForm.worldOrCellDesc ||
    GetWorldInstance() != changeForm.worldInstance ||
    IsDisabled() != changeForm.isDisabled ||
    IsDeleted() != changeForm.isDeleted;
  if (world && spatialChange) {
    // Loading a save is a cold reconstruction operation. Silently removing a
    // published edge here would leave a ghost on an already connected client.
    for (auto* listener : GetListeners())
      if (!pImpl->pendingListeners.count(listener))
        throw std::runtime_error(
          "Cannot restore spatial state with published subscriptions");
    for (auto* emitter : GetEmitters())
      if (!emitter->pImpl->pendingListeners.count(this))
        throw std::runtime_error(
          "Cannot restore spatial state with published subscriptions");
    // Forget the old cell before replacing its descriptor.
    RemoveFromGridAndUnsubscribeAll();
    UnsubscribeFromAll();
  }

  if (changeForm.profileId >= 0) {
    RegisterProfileId(changeForm.profileId);
  }

  changeForm.dynamicFields.ForEachValueDump(
    [&](const std::string& propertyName, const std::string& valueDump) {
      static const std::string kPrefix = GetPropertyPrefixPrivateIndexed();
      bool startsWith = propertyName.compare(0, kPrefix.size(), kPrefix) == 0;
      if (startsWith) {
        RegisterPrivateIndexedProperty(propertyName, valueDump);
      }
    });

  // See https://github.com/skyrim-multiplayer/issue-tracker/issues/42
  EditChangeForm(
    [&](MpChangeFormREFR& f) {
      f = changeForm;

      // Fix: RequestReloot doesn't work with non-zero 'nextRelootDatetime'
      f.nextRelootDatetime = 0;
    },
    Mode::NoRequestSave);
  if (pImpl->primitive)
    SetPrimitive(pImpl->primitive->boundsDiv2);
  if (changeForm.nextRelootDatetime) {
    auto tp =
      std::chrono::system_clock::from_time_t(changeForm.nextRelootDatetime);

    // Fix: Handle situations when the server is stopped at the 'tp' moment
    if (tp < std::chrono::system_clock::now()) {
      tp = std::chrono::system_clock::now() + std::chrono::milliseconds(1);
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      tp - std::chrono::system_clock::now());
    RequestReloot(ms);
  }

  // Perform all required grid operations
  // Mirrors MpActor impl
  if (!AsActor()) {
    changeForm.isDisabled ? Disable() : Enable();
    SetCellOrWorldObsolete(changeForm.worldOrCellDesc);
    SetPos(changeForm.position);
  }
}

const DynamicFields& MpObjectReference::GetDynamicFields() const
{
  return ChangeForm().dynamicFields;
}

void MpObjectReference::SetCellOrWorldObsolete(const FormDesc& newWorldOrCell)
{
  RequireCampaignSpatialMutation();
  auto worldState = GetParent();
  if (!worldState) {
    return;
  }

  auto worldOrCell =
    ChangeForm().worldOrCellDesc.ToFormId(worldState->espmFiles);

  everSubscribedOrListened = false;
  auto gridIterator = worldState->grids.find(worldOrCell);
  if (gridIterator != worldState->grids.end()) {
    gridIterator->second.grid->Forget(this);
  }

  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.worldOrCellDesc = newWorldOrCell;
  });
}

void MpObjectReference::VisitNeighbours(const Visitor& visitor)
{
  if (IsDisabled()) {
    return;
  }

  auto worldState = GetParent();
  if (!worldState) {
    return;
  }

  auto worldOrCell =
    ChangeForm().worldOrCellDesc.ToFormId(worldState->espmFiles);

  auto gridIterator = worldState->grids.find(worldOrCell);
  if (gridIterator == worldState->grids.end()) {
    return;
  }

  auto& grid = gridIterator->second;
  auto pos = GetGridPos(GetPos());
  auto& neighbours =
    worldState->GetNeighborsByPosition(worldOrCell, pos.first, pos.second);
  for (auto neighbour : neighbours) {
    if (SharesWorldInstance(*neighbour)) {
      visitor(neighbour);
    }
  }
}

void MpObjectReference::SendPapyrusEvent(const char* eventName,
                                         const VarValue* arguments,
                                         size_t argumentsCount)
{
  if (GetDynamicFields().GetValueDump(CampaignReferenceBinding::kProperty) !=
      "null")
    return QuestScriptRuntime::ExecuteReferenceEvent(
      *this, eventName, arguments, argumentsCount);
  if (!pImpl->scriptsInited) {
    InitScripts();
    pImpl->scriptsInited = true;
  }
  return MpForm::SendPapyrusEvent(eventName, arguments, argumentsCount);
}

void MpObjectReference::Init(WorldState* parent, uint32_t formId,
                             bool hasChangeForm)
{
  MpForm::Init(parent, formId, hasChangeForm);

  // We should queue created form for saving as soon as it is initialized
  const auto mode = (!hasChangeForm && !IsEspmForm()) ? Mode::RequestSave
                                                      : Mode::NoRequestSave;

  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) {
      changeForm.formDesc =
        FormDesc::FromFormId(formId, GetParent()->espmFiles);
    },
    mode);

  auto refrId = GetFormId();
  if (parent->HasEspm() && IsEspmForm() && !AsActor()) {
    auto lookupRes = parent->GetEspm().GetBrowser().LookupById(refrId);
    auto data = espm::GetData<espm::REFR>(refrId, parent);
    for (auto& info : data.activationParents) {
      auto activationParent = lookupRes.ToGlobalId(info.refrId);

      // Using WorldState for that, because we don't want search (potentially
      // load) other references during OnInit
      parent->activationChildsByActivationParent[activationParent].insert(
        { refrId, info.delay });
    }
  }
}

bool MpObjectReference::IsLocationSavingNeeded() const
{
  auto last = GetLastSaveRequestMoment();
  return !last ||
    std::chrono::system_clock::now() - *last > std::chrono::seconds(30);
}

void MpObjectReference::GivePickupItemsToActivationSource(
  MpObjectReference& activationSource, const espm::LookupResult& base)
{
  auto& loader = GetParent()->GetEspm();
  auto& compressedFieldsCache = GetParent()->GetEspmCache();
  auto t = base.rec->GetType();

  auto mapping = loader.GetBrowser().GetCombMapping(base.fileIdx);
  uint32_t resultItem = 0;
  if (espm::utils::Is<espm::TREE>(t)) {
    auto data =
      espm::Convert<espm::TREE>(base.rec)->GetData(compressedFieldsCache);
    resultItem = espm::utils::GetMappedId(data.resultItem, *mapping);
  }

  if (espm::utils::Is<espm::FLOR>(t)) {
    auto data =
      espm::Convert<espm::FLOR>(base.rec)->GetData(compressedFieldsCache);
    resultItem = espm::utils::GetMappedId(data.resultItem, *mapping);
  }

  if (espm::utils::Is<espm::LIGH>(t)) {
    auto res =
      espm::Convert<espm::LIGH>(base.rec)->GetData(compressedFieldsCache);
    bool isTorch = res.data.flags & espm::LIGH::Flags::CanBeCarried;
    if (!isTorch) {
      return;
    }
    resultItem = espm::utils::GetMappedId(base.rec->GetId(), *mapping);
  }

  if (resultItem == 0) {
    resultItem = espm::utils::GetMappedId(base.rec->GetId(), *mapping);
  }

  auto resultItemLookupRes = loader.GetBrowser().LookupById(resultItem);
  auto leveledItem = espm::Convert<espm::LVLI>(resultItemLookupRes.rec);
  if (leveledItem) {
    const auto kCountMult = 1;
    auto map = LeveledListUtils::EvaluateListRecurse(
      loader.GetBrowser(), resultItemLookupRes, kCountMult,
      kPlayerCharacterLevel, chanceNoneOverride.get());
    for (auto& p : map) {
      activationSource.AddItem(p.first, p.second);
    }
  } else {
    auto refrRecord = espm::Convert<espm::REFR>(
      loader.GetBrowser().LookupById(GetFormId()).rec);

    uint32_t countRecord =
      refrRecord ? refrRecord->GetData(compressedFieldsCache).count : 1;

    uint32_t countChangeForm = ChangeForm().count;

    constexpr uint32_t kCountDefault = 1;

    uint32_t resultingCount =
      std::max(kCountDefault, std::max(countRecord, countChangeForm));

    activationSource.AddItem(resultItem, resultingCount);
  }
}

void MpObjectReference::ProcessActivateNormal(
  MpObjectReference& activationSource)
{
  auto actorActivator = activationSource.AsActor();

  auto worldState = GetParent();
  auto& loader = GetParent()->GetEspm();
  auto& compressedFieldsCache = GetParent()->GetEspmCache();

  auto base = loader.GetBrowser().LookupById(GetBaseId());
  if (!base.rec || !GetBaseId()) {
    return spdlog::error("MpObjectReference::ProcessActivate {:x} - doesn't "
                         "have base form, activationSource is {:x}",
                         GetFormId(), activationSource.GetFormId());
  }

  if (IsDeleted()) {
    return spdlog::warn("MpObjectReference::ProcessActivate {:x} - deleted "
                        "object, activationSource is {:x}",
                        GetFormId(), activationSource.GetFormId());
  }

  if (IsDisabled()) {
    return spdlog::warn("MpObjectReference::ProcessActivate {:x} - disabled "
                        "object, activationSource is {:x}",
                        GetFormId(), activationSource.GetFormId());
  }

  auto t = base.rec->GetType();

  bool pickable = espm::utils::Is<espm::TREE>(t) ||
    espm::utils::Is<espm::FLOR>(t) || espm::utils::IsItem(t);
  if (pickable && !IsHarvested()) {
    GivePickupItemsToActivationSource(activationSource, base);
    SetHarvested(true);
    RequestReloot();

    if ((espm::utils::Is<espm::TREE>(t) || espm::utils::Is<espm::FLOR>(t)) &&
        actorActivator) {
      // SendOpenContainer being used to activate the object
      // TODO: rename SendOpenContainer to SendActivate
      activationSource.SendOpenContainer(GetFormId());
    }

    if (espm::utils::IsItem(t) && !IsEspmForm()) {
      spdlog::info("MpObjectReference::ProcessActivate - Deleting 0xff item");
      Delete();
    }
  } else if (t == espm::DOOR::kType) {
    auto lookupRes = loader.GetBrowser().LookupById(GetFormId());
    auto refrRecord = espm::Convert<espm::REFR>(lookupRes.rec);
    auto teleport = refrRecord->GetData(compressedFieldsCache).teleport;
    if (teleport) {
      RequirePositionMutation(activationSource);
      if (!IsOpen()) {
        SetOpen(true);
        RequestReloot();
      }

      auto destinationId = lookupRes.ToGlobalId(teleport->destinationDoor);
      auto destination = loader.GetBrowser().LookupById(destinationId);
      auto destinationRecord = espm::Convert<espm::REFR>(destination.rec);
      if (!destinationRecord) {
        throw std::runtime_error(
          "No destination found for this teleport door");
      }

      auto teleportWorldOrCell = destination.ToGlobalId(
        GetWorldOrCell(loader.GetBrowser(), destinationRecord));

      static const auto kPi = std::acos(-1.f);
      const auto& pos = teleport->pos;
      const float rot[] = { teleport->rotRadians[0] / kPi * 180,
                            teleport->rotRadians[1] / kPi * 180,
                            teleport->rotRadians[2] / kPi * 180 };

      TeleportMessage msg;
      msg.idx = activationSource.GetIdx();
      std::copy(std::begin(pos), std::end(pos), msg.pos.begin());
      std::copy(std::begin(rot), std::end(rot), msg.rot.begin());
      msg.worldOrCell = teleportWorldOrCell;

      if (actorActivator) {
        actorActivator->GetActorToSendTo().SendToUser(msg, true);
      }

      activationSource.SetCellOrWorldObsolete(
        FormDesc::FromFormId(teleportWorldOrCell, worldState->espmFiles));
      activationSource.SetPos({ pos[0], pos[1], pos[2] });
      activationSource.SetAngle({ rot[0], rot[1], rot[2] });

    } else {
      SetOpen(!IsOpen());
    }
  } else if (t == espm::CONT::kType && actorActivator) {
    EnsureBaseContainerAdded(loader);

    constexpr float kOccupationReach = 512.f;

    if (CheckIfObjectCanStartOccupyThis(activationSource, kOccupationReach)) {
      if (this->occupant) {
        this->occupant->RemoveEventSink(this->occupantDestroySink);
        this->occupant->RemoveEventSink(this->occupantDisableSink);
      }
      SetOpen(true);
      actorActivator->GetActorToSendTo().SendToUser(
        CreatePropertyMessage_(this, "inventory",
                               GetInventory().ToJson().dump()),
        true);
      activationSource.SendOpenContainer(GetFormId());

      this->occupant = actorActivator;

      this->occupantDestroySink.reset(
        new OccupantDestroyEventSink(*GetParent(), this));
      this->occupant->AddEventSink(this->occupantDestroySink);

      this->occupantDisableSink.reset(
        new OccupantDisableEventSink(*GetParent(), this));
      this->occupant->AddEventSink(this->occupantDisableSink);
    }
  } else if (t == espm::ACTI::kType && actorActivator) {
    // SendOpenContainer being used to activate the object
    // TODO: rename SendOpenContainer to SendActivate
    activationSource.SendOpenContainer(GetFormId());
  } else if (t == "FURN" && actorActivator) {

    constexpr float kOccupationReach = 256.f;

    if (CheckIfObjectCanStartOccupyThis(activationSource, kOccupationReach)) {
      if (this->occupant) {
        this->occupant->RemoveEventSink(this->occupantDestroySink);
        this->occupant->RemoveEventSink(this->occupantDisableSink);
      }

      // SendOpenContainer being used to activate the object
      // TODO: rename SendOpenContainer to SendActivate
      activationSource.SendOpenContainer(GetFormId());

      this->occupant = actorActivator;

      this->occupantDestroySink.reset(
        new OccupantDestroyEventSink(*GetParent(), this));
      this->occupant->AddEventSink(this->occupantDestroySink);

      this->occupantDisableSink.reset(
        new OccupantDisableEventSink(*GetParent(), this));
      this->occupant->AddEventSink(this->occupantDisableSink);
    }
  }
}

bool MpObjectReference::ProcessActivateSecond(
  MpObjectReference& activationSource)
{
  auto actorActivator = activationSource.AsActor();

  auto worldState = GetParent();
  auto& loader = GetParent()->GetEspm();
  auto& compressedFieldsCache = GetParent()->GetEspmCache();

  auto base = loader.GetBrowser().LookupById(GetBaseId());
  if (!base.rec || !GetBaseId()) {
    spdlog::error("MpObjectReference::ProcessActivate {:x} - doesn't "
                  "have base form, activationSource is {:x}",
                  GetFormId(), activationSource.GetFormId());
    return false;
  }

  auto t = base.rec->GetType();

  if (t == espm::CONT::kType && actorActivator) {
    if (this->occupant == &activationSource) {
      SetOpen(false);
      this->occupant->RemoveEventSink(this->occupantDestroySink);
      this->occupant = nullptr;
      return true;
    }
  } else if (t == "FURN" && actorActivator) {
    if (this->occupant == &activationSource) {
      this->occupant->RemoveEventSink(this->occupantDestroySink);
      this->occupant = nullptr;
      return true;
    }
  }

  return false;
}

void MpObjectReference::ActivateChilds()
{
  auto worldState = GetParent();
  if (!worldState) {
    return;
  }

  auto myFormId = GetFormId();

  for (auto& pair : worldState->activationChildsByActivationParent[myFormId]) {
    auto childRefrId = pair.first;
    auto delay = pair.second;

    auto delayMs = Viet::TimeUtils::To<std::chrono::milliseconds>(delay);
    worldState->SetTimer(delayMs).Then([worldState, childRefrId,
                                        myFormId](Viet::Void) {
      auto& childForm = worldState->LookupFormById(childRefrId);
      MpObjectReference* childRefr =
        childForm ? childForm->AsObjectReference() : nullptr;
      if (!childRefr) {
        spdlog::warn("MpObjectReference::ActivateChilds {:x} - Bad/missing "
                     "activation child {:x}",
                     myFormId, childRefrId);
        return;
      }

      // Not sure about activationSource and defaultProcessingOnly in this
      // case I'll try to keep vanilla scripts working
      childRefr->Activate(worldState->GetFormAt<MpObjectReference>(myFormId));
    });
  }
}

bool MpObjectReference::CheckIfObjectCanStartOccupyThis(
  MpObjectReference& activationSource, float occupationReach)
{
  if (!this->occupant) {
    spdlog::info("MpObjectReference::ProcessActivate {:x} - no occupant "
                 "(activationSource = {:x})",
                 GetFormId(), activationSource.GetFormId());
    return true;
  }

  if (this->occupant->IsDisabled() || !SharesWorldInstance(*this->occupant)) {
    spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is "
                 "disabled (activationSource = {:x})",
                 GetFormId(), activationSource.GetFormId());
    return true;
  }

  auto& occupantPos = this->occupant->GetPos();
  auto distanceToOccupantSqr = (occupantPos - GetPos()).SqrLength();
  if (distanceToOccupantSqr > occupationReach * occupationReach) {
    spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is too "
                 "far away (activationSource = {:x})",
                 GetFormId(), activationSource.GetFormId());
    return true;
  }

  auto& occupantCellOrWorld = this->occupant->GetCellOrWorld();
  if (occupantCellOrWorld != GetCellOrWorld()) {
    spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is in "
                 "another cell/world (activationSource = {:x})",
                 GetFormId(), activationSource.GetFormId());
    return true;
  }

  if (this->occupant == &activationSource) {
    auto& loader = GetParent()->GetEspm();
    auto base = loader.GetBrowser().LookupById(GetBaseId());
    auto t = base.rec->GetType();
    auto actorActivator = activationSource.AsActor();
    if (t == "FURN" && actorActivator) {
      spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is "
                   "already this object (activationSource = {:x}). Blocking "
                   "because it's FURN",
                   GetFormId(), activationSource.GetFormId());
      return false;
    } else {
      spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is "
                   "already this object (activationSource = {:x})",
                   GetFormId(), activationSource.GetFormId());
      return true;
    }
  }

  spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is "
               "another object and is nearby (activationSource = {:x})",
               GetFormId(), activationSource.GetFormId());
  return false;
}

void MpObjectReference::RemoveFromGridAndUnsubscribeAll()
{
  RequireCampaignSpatialMutation();
  auto* world = GetParent();
  const auto lifetime = world->LookupFormByIdNoLoad(GetFormId());
  auto worldOrCell = GetCellOrWorld().ToFormId(world->espmFiles);
  auto gridIterator = world->grids.find(worldOrCell);
  if (gridIterator != world->grids.end()) {
    gridIterator->second.grid->Forget(this);
  }

  std::vector<std::shared_ptr<MpForm>> listenersCopy;
  for (auto* listener : GetListeners())
    listenersCopy.push_back(
      world->LookupFormByIdNoLoad(listener->GetFormId()));
  for (const auto& listener : listenersCopy) {
    if (listener &&
        world->LookupFormByIdNoLoad(listener->GetFormId()) == listener)
      Unsubscribe(this, listener->AsObjectReference());
  }

  everSubscribedOrListened = false;
}

void MpObjectReference::UnsubscribeFromAll()
{
  RequireCampaignSpatialMutation();
  auto* world = GetParent();
  const auto lifetime = world->LookupFormByIdNoLoad(GetFormId());
  std::vector<std::shared_ptr<MpForm>> emittersCopy;
  for (auto* emitter : GetEmitters())
    emittersCopy.push_back(world->LookupFormByIdNoLoad(emitter->GetFormId()));
  for (const auto& emitter : emittersCopy)
    if (emitter &&
        world->LookupFormByIdNoLoad(emitter->GetFormId()) == emitter)
      Unsubscribe(emitter->AsObjectReference(), this);
}

void MpObjectReference::InitScripts()
{
  if (GetDynamicFields().GetValueDump(CampaignReferenceBinding::kProperty) !=
      "null")
    throw std::runtime_error(
      "Private references require the campaign event dispatcher");
  auto baseId = GetBaseId();
  if (!baseId || !GetParent()->espm) {
    return;
  }

  auto scriptStorage = GetParent()->GetScriptStorage();
  if (!scriptStorage) {
    return;
  }

  auto& compressedFieldsCache = GetParent()->GetEspmCache();

  std::vector<std::string> scriptNames;

  auto& br = GetParent()->espm->GetBrowser();
  auto base = br.LookupById(baseId);
  auto refr = br.LookupById(GetFormId());
  for (auto record : { base.rec, refr.rec }) {
    if (!record) {
      continue;
    }

    std::optional<espm::ScriptData> scriptData;

    if (record == base.rec && record->GetType() == "NPC_") {
      auto baseId = base.ToGlobalId(base.rec->GetId());
      if (auto actor = AsActor()) {
        auto& templateChain = actor->GetTemplateChain();
        scriptData = EvaluateTemplate<espm::NPC_::UseScript>(
          GetParent(), baseId, templateChain,
          [&compressedFieldsCache](const auto& npcLookupRes,
                                   const auto& npcData) {
            espm::ScriptData scriptData;
            npcLookupRes.rec->GetScriptData(&scriptData,
                                            compressedFieldsCache);
            return scriptData;
          });
      }
    }

    if (!scriptData) {
      scriptData = espm::ScriptData();
      record->GetScriptData(&*scriptData, compressedFieldsCache);
    }

    auto& scriptsInStorage =
      GetParent()->GetScriptStorage()->ListScripts(false);
    for (auto& script : scriptData->scripts) {
      if (scriptsInStorage.count(
            { script.scriptName.begin(), script.scriptName.end() })) {

        if (std::count(scriptNames.begin(), scriptNames.end(),
                       script.scriptName) == 0) {
          scriptNames.push_back(script.scriptName);
        }
      } else if (auto wst = GetParent())
        wst->logger->warn("Script '{}' not found in the script storage",
                          script.scriptName);
    }
  }

  // A hardcoded hack to remove all scripts except SweetPie scripts from
  // exterior objects
  // TODO: make is a server setting with proper conditions or an API
  if (GetParent() && GetParent()->disableVanillaScriptsInExterior &&
      GetFormId() < 0x05000000) {
    auto cellOrWorld = GetCellOrWorld().ToFormId(GetParent()->espmFiles);
    auto lookupRes =
      GetParent()->GetEspm().GetBrowser().LookupById(cellOrWorld);
    if (lookupRes.rec && lookupRes.rec->GetType() == "WRLD") {
      spdlog::info("Skipping non-Sweet scripts for exterior form {:x}",
                   cellOrWorld);
      scriptNames.erase(std::remove_if(scriptNames.begin(), scriptNames.end(),
                                       [](const std::string& val) {
                                         auto kPrefix = "Sweet";
                                         bool startsWith = val.size() >= 5 &&
                                           !memcmp(kPrefix, val.data(), 5);
                                         return !startsWith;
                                       }),
                        scriptNames.end());
    }
  }

  // A hardcoded hack to remove unsupported scripts
  // TODO: make is a server setting with proper conditions or an API
  auto isScriptEraseNeeded = [](const std::string& val) {
    // 1. GetStage in OnTrigger
    // 2. Unable to determine Actor for 'Game.GetPlayer' in 'OnLoad'
    const bool isRemoveNeeded =
      !Utils::stricmp(val.data(), "DA06PreRitualSceneTriggerScript") ||
      !Utils::stricmp(val.data(), "CritterSpawn");

    spdlog::info("Skipping script {}", val);

    return isRemoveNeeded;
  };
  scriptNames.erase(std::remove_if(scriptNames.begin(), scriptNames.end(),
                                   isScriptEraseNeeded),
                    scriptNames.end());

  if (!scriptNames.empty()) {
    pImpl->scriptState = std::make_unique<ScriptState>();

    std::vector<VirtualMachine::ScriptInfo> scriptInfo;
    for (auto& scriptName : scriptNames) {
      auto scriptVariablesHolder = std::make_shared<ScriptVariablesHolder>(
        scriptName, base, refr, base.parent, &compressedFieldsCache,
        GetParent());
      scriptInfo.push_back({ scriptName, std::move(scriptVariablesHolder) });
    }

    GetParent()->GetPapyrusVm().AddObject(ToGameObject(), scriptInfo);
  }
}

void MpObjectReference::MoveOnGrid(GridImpl<MpObjectReference*>& grid)
{
  auto newGridPos = GetGridPos(GetPos());
  grid.Move(this, newGridPos.first, newGridPos.second);
}

void MpObjectReference::InitListenersAndEmitters()
{
  if (!listeners) {
    listeners.reset(new std::set<MpObjectReference*>);
    emitters.reset(new std::set<MpObjectReference*>);
    actorListenerArray.clear();
  }
}

void MpObjectReference::SendInventoryUpdate()
{
  constexpr int kChannelSetInventory = 0;
  auto actor = AsActor();
  if (actor) {
    SetInventoryMessage message;
    message.inventory = actor->GetInventory();
    actor->SendToUserDeferred(message, true, kChannelSetInventory, true);
  }
}

void MpObjectReference::SendOpenContainer(uint32_t targetId)
{
  auto actor = AsActor();
  if (actor) {
    OpenContainerMessage msg;
    msg.target = targetId;
    actor->GetActorToSendTo().SendToUser(msg, true);
  }
}

std::vector<espm::CONT::ContainerObject> GetOutfitObjects(
  WorldState* worldState, const std::vector<FormDesc>& templateChain,
  const espm::LookupResult& lookupRes)
{
  auto& compressedFieldsCache = worldState->GetEspmCache();

  std::vector<espm::CONT::ContainerObject> res;

  if (auto baseNpc = espm::Convert<espm::NPC_>(lookupRes.rec)) {
    auto baseId = lookupRes.ToGlobalId(lookupRes.rec->GetId());
    auto outfitId = EvaluateTemplate<espm::NPC_::UseInventory>(
      worldState, baseId, templateChain,
      [](const auto& npcLookupRes, const auto& npcData) {
        return npcLookupRes.ToGlobalId(npcData.defaultOutfitId);
      });

    auto outfitLookupRes =
      worldState->GetEspm().GetBrowser().LookupById(outfitId);
    auto outfit = espm::Convert<espm::OTFT>(outfitLookupRes.rec);
    auto outfitData =
      outfit ? outfit->GetData(compressedFieldsCache) : espm::OTFT::Data();

    for (uint32_t i = 0; i != outfitData.count; ++i) {
      auto outfitElementId = outfitLookupRes.ToGlobalId(outfitData.formIds[i]);
      res.push_back({ outfitElementId, 1 });
    }
  }
  return res;
}

std::vector<espm::CONT::ContainerObject> GetInventoryObjects(
  WorldState* worldState, const std::vector<FormDesc>& templateChain,
  const espm::LookupResult& lookupRes)
{
  auto& compressedFieldsCache = worldState->GetEspmCache();

  auto baseContainer = espm::Convert<espm::CONT>(lookupRes.rec);
  if (baseContainer) {
    auto objects = baseContainer->GetData(compressedFieldsCache).objects;
    for (auto& entry : objects)
      entry.formId = lookupRes.ToGlobalId(entry.formId);
    return objects;
  }

  auto baseNpc = espm::Convert<espm::NPC_>(lookupRes.rec);
  if (baseNpc) {
    auto baseId = lookupRes.ToGlobalId(lookupRes.rec->GetId());
    return EvaluateTemplate<espm::NPC_::UseInventory>(
      worldState, baseId, templateChain,
      [](const auto& source, const auto& npcData) {
        auto objects = npcData.objects;
        for (auto& entry : objects)
          entry.formId = source.ToGlobalId(entry.formId);
        return objects;
      });
  }

  return {};
}

void MpObjectReference::AddContainerObject(
  const espm::CONT::ContainerObject& entry,
  std::map<uint32_t, uint32_t>* itemsToAdd)
{
  if (!entry.count)
    return;
  const auto add = [&](uint32_t base, uint32_t count) {
    auto& previous = (*itemsToAdd)[base];
    if (count > std::numeric_limits<uint32_t>::max() - previous)
      throw std::overflow_error("Container inventory count overflow");
    previous += count;
  };
  auto& espm = GetParent()->GetEspm();
  auto formLookupRes = espm.GetBrowser().LookupById(entry.formId);
  auto leveledItem = espm::Convert<espm::LVLI>(formLookupRes.rec);
  if (leveledItem) {
    auto map = LeveledListUtils::EvaluateListRecurse(
      espm.GetBrowser(), formLookupRes, entry.count, kPlayerCharacterLevel,
      chanceNoneOverride.get());
    for (auto& p : map) {
      add(p.first, p.second);
    }
  } else {
    add(entry.formId, entry.count);
  }
}

void MpObjectReference::EnsureBaseContainerAdded(espm::Loader& espm)
{
  if (ChangeForm().baseContainerAdded) {
    return;
  }

  auto worldState = GetParent();
  if (!worldState) {
    return;
  }

  auto actor = AsActor();
  const std::vector<FormDesc> kEmptyTemplateChain;
  const std::vector<FormDesc>& templateChain =
    actor ? actor->GetTemplateChain() : kEmptyTemplateChain;

  auto lookupRes = espm.GetBrowser().LookupById(GetBaseId());

  std::map<uint32_t, uint32_t> itemsToAdd, itemsToEquip;

  auto inventoryObjects =
    GetInventoryObjects(GetParent(), templateChain, lookupRes);
  for (auto& entry : inventoryObjects) {
    AddContainerObject(entry, &itemsToAdd);
  }

  auto outfitObjects = GetOutfitObjects(GetParent(), templateChain, lookupRes);
  for (auto& entry : outfitObjects) {
    AddContainerObject(entry, &itemsToEquip);
  }
  for (const auto& [base, count] : itemsToEquip) {
    if (count > std::numeric_limits<uint32_t>::max() - itemsToAdd[base])
      throw std::overflow_error("Outfit inventory count overflow");
    itemsToAdd[base] += count;
  }
  if (actor) {
    Equipment eq;
    for (auto& p : itemsToEquip) {
      Inventory::Entry e;
      e.baseId = p.first;
      e.count = p.second;
      e.SetWorn(Inventory::Worn::Right);
      eq.inv.AddItems({ e });
    }
    actor->SetEquipment(eq);
  }

  std::vector<Inventory::Entry> entries;
  for (auto& p : itemsToAdd) {
    entries.push_back({ p.first, p.second });
  }
  AddItems(entries);

  if (!ChangeForm().baseContainerAdded) {
    EditChangeForm([&](MpChangeFormREFR& changeForm) {
      changeForm.baseContainerAdded = true;
    });
  }
}

void MpObjectReference::CheckInteractionAbility(MpObjectReference& refr)
{
  if (!SharesWorldInstance(refr)) {
    throw std::runtime_error("World instance does not match");
  }
  auto& loader = GetParent()->GetEspm();
  auto& compressedFieldsCache = GetParent()->GetEspmCache();

  auto casterWorldId = refr.GetCellOrWorld().ToFormId(GetParent()->espmFiles);
  auto targetWorldId = GetCellOrWorld().ToFormId(GetParent()->espmFiles);

  auto casterWorld = loader.GetBrowser().LookupById(casterWorldId).rec;
  auto targetWorld = loader.GetBrowser().LookupById(targetWorldId).rec;

  if (targetWorld != casterWorld) {
    const char* casterWorldName =
      casterWorld ? casterWorld->GetEditorId(compressedFieldsCache) : "<null>";

    const char* targetWorldName =
      targetWorld ? targetWorld->GetEditorId(compressedFieldsCache) : "<null>";

    throw std::runtime_error(fmt::format(
      "WorldSpace doesn't match: caster is in {} ({:#x}), target is in "
      "{} ({:#x})",
      casterWorldName, casterWorldId, targetWorldName, targetWorldId));
  }
}

void MpObjectReference::SendMessageToActorListeners(const IMessageBase& msg,
                                                    bool reliable) const
{
  for (auto listener : GetActorListeners()) {
    listener->GetActorToSendTo().SendToUser(msg, true);
  }
}

void MpObjectReference::BeforeDestroy()
{
  pImpl->destroying = true;
  if (this->occupant && this->occupantDestroySink) {
    this->occupant->RemoveEventSink(this->occupantDestroySink);
  }

  MpForm::BeforeDestroy();

  // Destruction is not movement and must never promote an unpublished save.
  // Remove both directions before releasing the native identity.
  RemoveFromGridAndUnsubscribeAll();
  UnsubscribeFromAll();
}

float MpObjectReference::GetTotalItemWeight() const
{
  const auto& entries = GetInventory().entries;
  const auto calculateWeight = [this](float sum,
                                      const Inventory::Entry& entry) {
    const auto& espm = GetParent()->GetEspm();
    const auto* record = espm.GetBrowser().LookupById(entry.baseId).rec;
    if (!record) {
      spdlog::warn(
        "MpObjectReference::GetTotalItemWeight of ({:x}): Record of form "
        "({}) is nullptr",
        GetFormId(), entry.baseId);
      return 0.f;
    }
    float weight = GetWeightFromRecord(record, GetParent()->GetEspmCache());
    if (!espm::utils::IsItem(record->GetType())) {
      spdlog::warn("Unsupported espm type {} has been detected, when "
                   "calculating overall weight.",
                   record->GetType().ToString());
    } else {
      spdlog::trace("Weight: {} for record of type {}", weight,
                    record->GetType().ToString());
    }
    return sum + entry.count * weight;
  };

  return std::accumulate(entries.begin(), entries.end(), 0.f, calculateWeight);
}
