#include "QuestJournal.h"
#include "CampaignFactionApi.h"
#include "CampaignMovement.h"
#include "CampaignReferenceBinding.h"
#include "MpActor.h"
#include "QuestJobRunner.h"
#include "QuestVmState.h"
#include "WorldState.h"
#include "libespm/QUST.h"
#include "libespm/QuestLifecycleData.h"
#include "libespm/RecordHeaderAccess.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <set>
#include <tuple>

namespace {
using Json = nlohmann::json;
constexpr uint64_t kMaxRevision = 9007199254740991ULL;
struct AdmissionBuilder
{
  // Only pristine or individually validated snapshots are visible here.
  // Mutable runtime outputs never become a source for guards/getters.
  std::map<uint64_t, std::shared_ptr<const QuestJobState::Job>> roots;
  std::shared_ptr<const QuestJobState::Job> successfulCapture;
  std::string identity;
};
struct Transaction
{
  struct InventoryEdit
  {
    std::shared_ptr<MpForm> lifetime;
    MpObjectReference* reference;
    Inventory initial, current;
    uint64_t epoch;
    bool baseContainerAdded;
  };
  struct ReferenceEdit
  {
    std::shared_ptr<MpForm> lifetime;
    MpObjectReference* reference;
    MpChangeForm initial, current;
    Json serializedInitial;
    uint64_t epoch;
    std::optional<std::vector<Faction>> factionBaseline;
    bool factionsEdited = false;
  };
  MpActor* owner;
  Json ledger;
  std::map<uint32_t, InventoryEdit> inventories;
  std::map<uint32_t, ReferenceEdit> references;
  // Immutable, transaction-local decoded jobs. Ordinary SetJobState publishes
  // this with ledger["jobs"]. The admission builder overlays validated nodes
  // until its one final assembly; other writers preserve the JSON job field.
  // Borrowed references stay inside their getter/guard call; the snapshot
  // never survives this transaction. External changes still fail read guards.
  std::shared_ptr<const QuestJobState> jobs;
  MpActor* requester = nullptr;
  std::shared_ptr<CampaignMovementPlan> movement;
  bool durable = false, admissionPreparing = false, stagingMovement = false;
  std::optional<uint64_t> jobId;
  bool jobTransitionApplied = false;
  bool admissionOnly = false, admissionRuntimeEntered = false;
  bool admissionBuilder = false;
  std::unique_ptr<AdmissionBuilder> builder;
  const Json* initialLedger = nullptr;
};
thread_local Transaction* transaction = nullptr;

const QuestJobState::Job* TransactionLive(uint64_t id)
{
  if (transaction->builder) {
    const auto found = transaction->builder->roots.find(id);
    if (found != transaction->builder->roots.end())
      return found->second.get();
  }
  return transaction->jobs->FindLive(id);
}

QuestJobState TransactionJobs()
{
  auto result = *transaction->jobs;
  if (transaction->builder) {
    for (const auto& [id, job] : transaction->builder->roots)
      result.live.emplace(id, *job);
    result.nextJob += transaction->builder->roots.size();
    result.ingress.next += transaction->builder->roots.size();
    if (!transaction->builder->roots.empty())
      result.ingress.identity = transaction->builder->identity;
  }
  return result;
}

void RequireGameplayWrites()
{
  if (transaction && transaction->admissionBuilder)
    throw std::runtime_error("Admission builder cannot change gameplay state");
}

Json SavedHeap(const Json& heap)
{
  QuestVmState state(
    heap,
    [](IGameObject*) -> Json {
      throw std::runtime_error("Unexpected continuation object encoding");
    },
    [](const Json&) -> VarValue {
      throw std::runtime_error("Unexpected continuation object resolution");
    });
  return state.Save();
}

Json SavedContinuations(const Json& heap)
{
  return SavedHeap(heap).at("continuations");
}

void RequireAdmissionHeap(const Json& before, const Json& after)
{
  if (before.at("continuations") != after.at("continuations") ||
      before.at("globals") != after.at("globals"))
    throw std::runtime_error("Admission changed existing VM work");
  // Recipient discovery may initialize missing scripts/properties and alias
  // bindings, but it cannot write any existing value or shared array node.
  for (const auto* field : { "arrays", "aliases" })
    for (const auto& [key, value] : before.at(field).items())
      if (!after.at(field).contains(key) || after.at(field).at(key) != value)
        throw std::runtime_error("Admission changed existing VM data");
  for (const auto& [key, script] : before.at("scripts").items()) {
    if (!after.at("scripts").contains(key))
      throw std::runtime_error("Admission removed an existing script");
    const auto& next = after.at("scripts").at(key);
    for (const auto& [field, value] : script.items()) {
      if (field == "values") {
        for (const auto& [name, oldValue] : value.items())
          if (!next.at("values").contains(name) ||
              next.at("values").at(name) != oldValue)
            throw std::runtime_error("Admission wrote an existing variable");
      } else if (!next.contains(field) || next.at(field) != value)
        throw std::runtime_error("Admission changed a script identity");
    }
  }
}

bool IsAdmissionSnapshot(const QuestJobState::Job& job)
{
  return job.kind == QuestJobState::Kind::Interaction && job.occurrence &&
    job.sequenced && job.segment == 1 && job.steps == 1 && job.dispatch &&
    !job.dispatch->captured && job.dispatch->recipient == 0 &&
    job.dispatch->attachment == 0 && !job.handler && job.children.empty() &&
    job.events.empty() && job.completions.empty() && job.eventHead == 0 &&
    job.nextHandler == 1 && job.nextOperation == 1 && !job.interactionDone &&
    job.cursor == "done" && job.token.empty();
}

void RequireUnchangedContinuations(const QuestJobState& before,
                                   uint64_t selected, const Json& initial,
                                   const Json& current)
{
  for (const auto& [id, job] : before.live) {
    if (id == selected)
      continue;
    const auto check = [&](const QuestJobState::Stage& stage) {
      if (stage.cursor == "waiting" &&
          (!current.contains(stage.token) ||
           initial.at(stage.token) != current.at(stage.token)))
        throw std::runtime_error(
          "Job transaction rewrote another root's continuation");
    };
    check(job);
    if (job.handler)
      check(*job.handler);
    for (const auto& child : job.children)
      check(child);
  }
}

// Host dispatch advances one saved boundary without executing script work.
// The native child-stack rules below govern root/handler body execution.
bool ValidateDispatchTransition(const QuestJobState::Job& prior,
                                const QuestJobState::Job& next,
                                bool referenceWasInitialized)
{
  using Stage = QuestJobState::Stage;
  if (next.events.size() < prior.events.size() ||
      !std::equal(prior.events.begin(), prior.events.end(),
                  next.events.begin()))
    throw std::runtime_error(
      "Job transaction rewrote inventory event history");
  const bool host = prior.cursor == "done" &&
    (!prior.handler || prior.handler->cursor == "done");
  const bool primary = prior.HasPrimaryEvent();
  const bool finishPrimary = primary && host && !prior.handler &&
    prior.dispatch && !prior.dispatch->captured &&
    prior.dispatch->recipient == prior.dispatch->recipients.size();
  const bool load = prior.kind == QuestJobState::Kind::ReferenceLoad;
  const bool skipLoadInit = load && host && !prior.handler &&
    !prior.dispatch && prior.loadPhase == QuestJobState::LoadPhase::Init &&
    referenceWasInitialized;
  auto expectedPhase = prior.loadPhase;
  if (load && (finishPrimary || skipLoadInit)) {
    switch (prior.loadPhase) {
      case QuestJobState::LoadPhase::Init:
        expectedPhase = QuestJobState::LoadPhase::CellLoad;
        break;
      case QuestJobState::LoadPhase::CellLoad:
        expectedPhase = QuestJobState::LoadPhase::Load;
        break;
      case QuestJobState::LoadPhase::Load:
        expectedPhase = QuestJobState::LoadPhase::Done;
        break;
      case QuestJobState::LoadPhase::Done:
        throw std::runtime_error("Completed load has another primary event");
    }
  }
  if (next.loadPhase != expectedPhase)
    throw std::runtime_error(
      "Job changed reference load phase outside its boundary");
  if (next.interactionDone !=
      (finishPrimary && prior.kind == QuestJobState::Kind::Interaction
         ? true
         : prior.interactionDone))
    throw std::runtime_error(
      "Job transaction changed interaction completion outside its boundary");
  if (!host) {
    if (next.eventHead != prior.eventHead ||
        next.nextHandler != prior.nextHandler ||
        next.dispatch != prior.dispatch ||
        next.handler.has_value() != prior.handler.has_value() ||
        (prior.handler &&
         (static_cast<const Stage&>(prior) !=
            static_cast<const Stage&>(next) ||
          next.handler->handlerIdentity != prior.handler->handlerIdentity)))
      throw std::runtime_error("Job body rewrote event dispatch state");
    return false;
  }
  if (static_cast<const Stage&>(prior) != static_cast<const Stage&>(next) ||
      next.events != prior.events || !prior.children.empty() ||
      !next.children.empty() || next.nextOperation != prior.nextOperation ||
      next.completions != prior.completions ||
      next.instructions < prior.instructions ||
      next.steps != prior.steps + 1 || next.segment != prior.segment + 1)
    throw std::runtime_error(
      "Event dispatch crossed a script execution boundary");
  const auto instructions = next.instructions - prior.instructions;
  if (skipLoadInit) {
    if (instructions || next.dispatch || next.handler ||
        next.eventHead != prior.eventHead ||
        next.nextHandler != prior.nextHandler)
      throw std::runtime_error(
        "Initialized reference skipped more than OnInit");
    return true;
  }
  if (prior.handler) {
    auto expected = *prior.dispatch;
    ++expected.attachment;
    if (instructions || next.handler || next.dispatch != expected ||
        next.eventHead != prior.eventHead ||
        next.nextHandler != prior.nextHandler)
      throw std::runtime_error(
        "Job transaction did not retire exactly one completed handler");
    return true;
  }
  if (!prior.dispatch) {
    if (instructions != 1 || !next.dispatch || next.dispatch->captured ||
        next.dispatch->recipient || next.handler ||
        next.eventHead != prior.eventHead ||
        next.nextHandler != prior.nextHandler)
      throw std::runtime_error(
        "Job transaction skipped event recipient snapshot");
    return true;
  }
  const auto& dispatch = *prior.dispatch;
  if (!dispatch.captured && dispatch.recipient == dispatch.recipients.size()) {
    if (instructions || next.dispatch || next.handler ||
        next.eventHead != prior.eventHead + (primary ? 0 : 1) ||
        next.nextHandler != prior.nextHandler)
      throw std::runtime_error(
        "Job transaction did not retire exactly one event");
    return true;
  }
  if (!next.dispatch || next.eventHead != prior.eventHead ||
      next.dispatch->recipients != dispatch.recipients)
    throw std::runtime_error(
      "Job transaction rewrote event recipient membership");
  if (!dispatch.captured) {
    if (instructions || !next.dispatch->captured ||
        next.dispatch->recipient != dispatch.recipient ||
        next.dispatch->attachment || next.handler ||
        next.nextHandler != prior.nextHandler)
      throw std::runtime_error(
        "Job transaction skipped recipient attachment capture");
    return true;
  }
  auto expected = dispatch;
  if (dispatch.attachment == dispatch.roots.size()) {
    ++expected.recipient;
    expected.attachment = 0;
    expected.captured = false;
    expected.roots.clear();
    expected.templates.clear();
    expected.reference.clear();
    if (instructions || next.handler || next.dispatch != expected ||
        next.nextHandler != prior.nextHandler)
      throw std::runtime_error(
        "Job transaction did not retire exactly one recipient");
  } else if (next.handler) {
    if (!instructions || instructions > 128 || next.dispatch != dispatch ||
        next.nextHandler != prior.nextHandler + 1 ||
        next.handler->cursor != "begin" ||
        next.handler->handlerIdentity->sequence != prior.nextHandler)
      throw std::runtime_error(
        "Job transaction skipped handler selection boundary");
  } else {
    ++expected.attachment;
    if (!instructions || instructions > 128 || next.dispatch != expected ||
        next.nextHandler != prior.nextHandler)
      throw std::runtime_error(
        "Job transaction did not skip exactly one absent handler");
  }
  return true;
}

bool EqualFactions(const std::vector<Faction>& lhs,
                   const std::vector<Faction>& rhs)
{
  return lhs.size() == rhs.size() &&
    std::equal(lhs.begin(), lhs.end(), rhs.begin(),
               [](const auto& a, const auto& b) {
                 return a.formDesc == b.formDesc && a.rank == b.rank;
               });
}

Transaction::ReferenceEdit& ReferenceTarget(MpActor& actor,
                                            MpObjectReference& target)
{
  if (!actor.GetParent())
    throw std::runtime_error("Unregistered quest transaction participant");
  auto& owner = QuestJournal::Owner(actor);
  QuestJournal::RequireJobAccess(actor);
  if (!transaction || transaction->owner != &owner)
    throw std::runtime_error(
      "Reference reads require an owning quest transaction");
  auto& world = *owner.GetParent();
  const auto lifetime = world.LookupFormByIdNoLoad(target.GetFormId());
  if (target.GetParent() != &world || lifetime.get() != &target ||
      target.IsDeleted())
    throw std::runtime_error("Unregistered campaign reference target");
  if (&target != &owner &&
      CampaignReferenceBinding::Context(target).first != &owner)
    throw std::runtime_error("Reference belongs to another campaign");
  const auto id = target.GetFormId();
  if (auto found = transaction->references.find(id);
      found != transaction->references.end()) {
    if (found->second.lifetime != lifetime)
      throw std::runtime_error("Campaign reference identity was replaced");
    return found->second;
  }
  if (transaction->references.size() >= 4096)
    throw std::runtime_error("Campaign reference read limit exceeded");
  const auto saved = target.GetChangeForm();
  auto current = saved;
  if (const auto inventory = transaction->inventories.find(id);
      inventory != transaction->inventories.end())
    current.inv = inventory->second.current;
  return transaction->references
    .emplace(id,
             Transaction::ReferenceEdit{
               lifetime, &target, saved, std::move(current),
               MpChangeForm::ToJson(saved), target.GetWorldInstanceEpoch() })
    .first->second;
}

Transaction::InventoryEdit& InventoryTarget(MpActor& actor,
                                            MpObjectReference& target)
{
  if (!actor.GetParent())
    throw std::runtime_error("Unregistered quest transaction participant");
  auto& owner = QuestJournal::Owner(actor);
  QuestJournal::RequireJobAccess(actor);
  if (!transaction || transaction->owner != &owner)
    throw std::runtime_error("Inventory requires an owning quest transaction");
  auto& world = *owner.GetParent();
  const auto lifetime = world.LookupFormByIdNoLoad(target.GetFormId());
  if (target.GetParent() != &world || lifetime.get() != &target ||
      target.IsDeleted())
    throw std::runtime_error("Unregistered campaign inventory target");
  const auto& instance = owner.GetWorldInstance();
  if (&target != &owner) {
    const auto prefix = "campaign:" + std::to_string(owner.GetFormId()) + ":";
    if (!instance.starts_with(prefix) || !target.SharesWorldInstance(owner) ||
        target.GetFormId() < 0xff000000)
      throw std::runtime_error(
        "Inventory target is not in the owner's private instance");
    const auto form = target.GetChangeForm();
    if (target.AsActor() && (form.profileId >= 0 || target.GetBaseId() == 7))
      throw std::runtime_error("Quest inventory cannot target another player");
    if (!form.baseContainerAdded)
      throw std::runtime_error("Campaign target inventory is not initialized");
  }
  const auto id = target.GetFormId();
  if (auto found = transaction->inventories.find(id);
      found != transaction->inventories.end()) {
    if (found->second.lifetime != lifetime)
      throw std::runtime_error("Campaign inventory identity was replaced");
    return found->second;
  }
  return transaction->inventories
    .emplace(id,
             Transaction::InventoryEdit{
               lifetime, &target, target.GetInventory(), target.GetInventory(),
               target.GetWorldInstanceEpoch(),
               target.GetChangeForm().baseContainerAdded })
    .first->second;
}

uint32_t Integer(const Json& value, uint32_t limit, const char* name)
{
  if (!value.is_number_integer() || value.get<int64_t>() < 0 ||
      value.get<uint64_t>() > limit) {
    throw std::invalid_argument(std::string("Invalid ") + name);
  }
  return value.get<uint32_t>();
}

Json EmptyQuest()
{
  return { { "stage", 0 },
           { "stages", Json::array() },
           { "running", false },
           { "started", false },
           { "completed", false },
           { "active", false },
           { "objectives", Json::object() } };
}

void Validate(const Json& ledger, bool validateHeap = true)
{
  if (!ledger.is_object() || !ledger.at("schemaVersion").is_number_integer() ||
      (ledger.at("schemaVersion") != 1 && ledger.at("schemaVersion") != 2) ||
      !ledger.at("revision").is_number_unsigned() ||
      ledger.at("revision").get<uint64_t>() > kMaxRevision ||
      !ledger.at("quests").is_object() || ledger.at("quests").size() > 10000) {
    throw std::runtime_error("Invalid or unsupported campaign journal");
  }
  if (ledger.at("schemaVersion") == 2) {
    for (const auto& [field, value] : ledger.items())
      if (field != "schemaVersion" && field != "revision" &&
          field != "quests" && field != "jobs" && field != "vm" &&
          field != "referenceInit")
        throw std::runtime_error("Unknown private campaign journal field");
    const auto jobs = QuestJobState::Read(ledger.at("jobs"));
    if (validateHeap) {
      jobs.ValidateHeap(ledger.value("vm", Json::object()));
      const auto initialized = ledger.value("referenceInit", Json::object());
      const auto checkLoad = [&](const QuestJobState::Job& job) {
        if (job.occurrence && !initialized.contains(job.interaction->target))
          throw std::runtime_error(
            "Native ingress requires an initialized reference");
        if (!job.referenceLoad)
          return;
        const bool marked = initialized.contains(job.referenceLoad->target);
        if ((job.loadPhase != QuestJobState::LoadPhase::Init && !marked) ||
            (job.loadPhase == QuestJobState::LoadPhase::Init && job.dispatch &&
             marked))
          throw std::runtime_error(
            "Reference load phase disagrees with its initialization marker");
      };
      for (const auto& [id, job] : jobs.live)
        checkLoad(job);
      for (const auto& job : jobs.completed)
        checkLoad(job);
    }
  } else if (ledger.contains("jobs"))
    throw std::runtime_error("Quest jobs require private journal schema two");
  if (ledger.contains("referenceInit")) {
    const auto& initialized = ledger.at("referenceInit");
    if (!initialized.is_object() || initialized.size() > 100000)
      throw std::runtime_error("Invalid saved reference initialization");
    for (const auto& [key, value] : initialized.items()) {
      if (key.empty() || key.size() > 300 || value != true)
        throw std::runtime_error("Invalid saved reference initialization");
      const auto form = FormDesc::FromString(key);
      if (form.file.empty() || form.ToString() != key)
        throw std::runtime_error("Invalid initialized reference identity");
    }
  }
  for (const auto& [key, quest] : ledger.at("quests").items()) {
    if (key.empty() || key.size() > 300 || !quest.is_object() ||
        !quest.at("stages").is_array() ||
        !quest.at("objectives").is_object()) {
      throw std::runtime_error("Invalid saved quest");
    }
    for (const auto* field : { "running", "started", "completed", "active" }) {
      if (!quest.at(field).is_boolean())
        throw std::runtime_error("Invalid quest flag");
    }
    const auto stage = Integer(quest.at("stage"), 65535, "saved stage");
    uint32_t highest = 0;
    std::set<uint32_t> unique;
    for (const auto& visited : quest.at("stages")) {
      const auto index = Integer(visited, 65535, "visited stage");
      if (!unique.insert(index).second)
        throw std::runtime_error("Duplicate saved stage");
      highest = std::max(highest, index);
    }
    if (stage != highest)
      throw std::runtime_error("Inconsistent highest quest stage");
    for (const auto& [index, objective] : quest.at("objectives").items()) {
      uint32_t parsed = 0;
      const auto result =
        std::from_chars(index.data(), index.data() + index.size(), parsed);
      if (result.ec != std::errc() ||
          result.ptr != index.data() + index.size() || parsed > 65535 ||
          std::to_string(parsed) != index) {
        throw std::runtime_error("Invalid saved objective index");
      }
      for (const auto* field : { "displayed", "completed", "failed" }) {
        if (!objective.at(field).is_boolean())
          throw std::runtime_error("Invalid objective flag");
      }
      if (objective.at("completed") == true &&
          objective.at("failed") == true) {
        throw std::runtime_error(
          "Objective cannot be both completed and failed");
      }
    }
  }
}

Json ReadDump(const std::string& dump)
{
  if (dump == "null") {
    return { { "schemaVersion", 1 },
             { "revision", uint64_t(0) },
             { "quests", Json::object() } };
  }
  if (dump.size() > 8 * 1024 * 1024)
    throw std::runtime_error("Campaign journal too large");
  auto ledger = Json::parse(dump);
  Validate(ledger);
  return ledger;
}

Json Read(MpActor& owner)
{
  if (transaction) {
    if (transaction->owner != &owner)
      throw std::runtime_error("Cross-campaign quest transaction");
    return transaction->ledger;
  }
  return ReadDump(
    owner.GetDynamicFields().GetValueDump(QuestJournal::kProperty));
}

struct Definition
{
  std::string key;
  std::set<uint32_t> stages, objectives;
};

Definition Lookup(MpActor& owner, uint32_t id)
{
  auto& world = *owner.GetParent();
  const auto record = world.GetEspm().GetBrowser().LookupById(id);
  if (!record.rec || record.rec->GetType() != espm::QUST::kType) {
    throw std::invalid_argument("Expected a QUST record");
  }
  Definition def;
  def.key = FormDesc::FromFormId(id, world.espmFiles).ToString();
  // Avoid the incomplete upstream alias/journal-text parser. Only indices are
  // required here; alias resolution and VMAD fragments are separate machinery.
  espm::RecordHeaderAccess::IterateFields(
    record.rec,
    [&](const char* tag, uint32_t size, const char* data) {
      if ((!std::memcmp(tag, "INDX", 4) || !std::memcmp(tag, "QOBJ", 4)) &&
          size >= 2) {
        uint16_t index;
        std::memcpy(&index, data, sizeof(index));
        (!std::memcmp(tag, "INDX", 4) ? def.stages : def.objectives)
          .insert(index);
      }
    },
    world.GetEspmCache());
  return def;
}
}

struct QuestJournal::CaptureContext::Data
{
  WorldState* world = nullptr;
  std::shared_ptr<MpForm> actor, owner;
  uint64_t actorEpoch = 0, ownerEpoch = 0;
  std::string actorIdentity, ownerIdentity, campaignIdentity, instance;
  std::string journal;
  std::vector<std::string> files;
  QuestJobState jobs;
  nlohmann::json initialized;
};

QuestJournal::CaptureContext::CaptureContext(std::shared_ptr<const Data> value)
  : data(std::move(value))
{
}

void QuestJournal::CaptureContext::Validate(MpActor& actor) const
{
  if (!data || transaction || actor.GetParent() != data->world ||
      data->actor.get() != &actor || actor.IsDeleted() ||
      data->world->LookupFormByIdNoLoad(actor.GetFormId()) != data->actor ||
      actor.GetWorldInstanceEpoch() != data->actorEpoch ||
      actor.GetWorldInstance() != data->instance ||
      data->world->espmFiles != data->files)
    throw std::runtime_error("Quest capture actor context changed");
  auto* owner = data->owner->AsActor();
  if (!owner || owner->GetParent() != data->world || owner->IsDeleted() ||
      data->world->LookupFormByIdNoLoad(owner->GetFormId()) != data->owner ||
      owner->GetWorldInstanceEpoch() != data->ownerEpoch ||
      &QuestJournal::Owner(actor) != owner ||
      CampaignReferenceBinding::GetIdentity(*owner) !=
        data->campaignIdentity ||
      owner->GetDynamicFields().GetValueDump(QuestJournal::kProperty) !=
        data->journal)
    throw std::runtime_error("Quest capture campaign context changed");
}

const QuestJobState& QuestJournal::CaptureContext::Jobs() const
{
  return data->jobs;
}
const std::string& QuestJournal::CaptureContext::ActorIdentity() const
{
  return data->actorIdentity;
}
const std::string& QuestJournal::CaptureContext::OwnerIdentity() const
{
  return data->ownerIdentity;
}
const std::string& QuestJournal::CaptureContext::CampaignIdentity() const
{
  return data->campaignIdentity;
}
const std::string& QuestJournal::CaptureContext::Instance() const
{
  return data->instance;
}

QuestJournal::CaptureContext QuestJournal::CaptureReadContext(MpActor& actor)
{
  if (transaction || !actor.GetParent())
    throw std::runtime_error("Quest capture snapshot requires external reads");
  auto value = std::make_shared<CaptureContext::Data>();
  auto& world = *actor.GetParent();
  value->world = &world;
  value->actor = world.LookupFormByIdNoLoad(actor.GetFormId());
  if (value->actor.get() != &actor || actor.IsDeleted())
    throw std::runtime_error("Unregistered quest capture actor");
  auto& owner = Owner(actor);
  value->owner = world.LookupFormByIdNoLoad(owner.GetFormId());
  if (value->owner.get() != &owner || owner.IsDeleted())
    throw std::runtime_error("Unregistered quest capture owner");
  value->actorEpoch = actor.GetWorldInstanceEpoch();
  value->ownerEpoch = owner.GetWorldInstanceEpoch();
  value->instance = actor.GetWorldInstance();
  value->files = world.espmFiles;
  const auto identity = [&](const MpForm& form) {
    return world.espmFiles.empty()
      ? FormDesc(form.GetFormId(), "").ToString()
      : FormDesc::FromFormId(form.GetFormId(), world.espmFiles).ToString();
  };
  value->actorIdentity = identity(actor);
  value->ownerIdentity = identity(owner);
  value->campaignIdentity = CampaignReferenceBinding::GetIdentity(owner);
  const auto& sourceJournal = owner.GetDynamicFields().GetValueDump(kProperty);
  if (sourceJournal.size() > 8 * 1024 * 1024)
    throw std::runtime_error("Campaign journal too large");
  value->journal = sourceJournal;
  const auto ledger = ReadDump(value->journal);
  value->jobs = ledger.contains("jobs")
    ? QuestJobState::Read(ledger.at("jobs"))
    : QuestJobState{};
  value->initialized = ledger.value("referenceInit", Json::object());
  CaptureContext result(std::move(value));
  result.Validate(actor);
  return result;
}

uint32_t QuestJournal::ParseCampaignOwnerId(const std::string& instance,
                                            bool requireCanonical)
{
  const std::string prefix = "campaign:";
  const auto separator = instance.find(':', prefix.size());
  if (instance.compare(0, prefix.size(), prefix) ||
      separator == std::string::npos || separator + 1 == instance.size()) {
    throw std::runtime_error("No campaign owner for this instance");
  }
  uint32_t ownerId = 0;
  const auto parsed = std::from_chars(instance.data() + prefix.size(),
                                      instance.data() + separator, ownerId);
  if (parsed.ec != std::errc() || parsed.ptr != instance.data() + separator ||
      !ownerId ||
      (requireCanonical &&
       instance.substr(prefix.size(), separator - prefix.size()) !=
         std::to_string(ownerId))) {
    throw std::runtime_error("Invalid campaign owner");
  }
  return ownerId;
}

MpActor& QuestJournal::Owner(MpActor& actor)
{
  const auto& instance = actor.GetWorldInstance();
  if (instance.empty())
    return actor;
  const auto ownerId = ParseCampaignOwnerId(instance);
  auto& owner = actor.GetParent()->GetFormAt<MpActor>(ownerId);
  if (!actor.SharesWorldInstance(owner)) {
    throw std::runtime_error("Campaign owner is no longer in this instance");
  }
  return owner;
}

nlohmann::json QuestJournal::Snapshot(MpActor& actor)
{
  auto& owner = Owner(actor);
  const auto ledger = Read(owner);
  // Public schema remains stable even when the private journal is migrated.
  Json result = { { "schemaVersion", 1 },
                  { "revision", ledger.at("revision") },
                  { "quests", ledger.at("quests") } };
  result["campaignOwner"] = owner.GetFormId();
  result["instance"] = actor.GetWorldInstance();
  result["actor"] = actor.GetFormId();
  result["contextEpoch"] = std::to_string(actor.GetWorldInstanceEpoch()) +
    ":" + std::to_string(owner.GetWorldInstanceEpoch());
  return result;
}

nlohmann::json QuestJournal::Get(MpActor& actor, uint32_t questId)
{
  auto& owner = Owner(actor);
  const auto def = Lookup(owner, questId);
  const auto ledger = Read(owner);
  return ledger.at("quests").value(def.key, EmptyQuest());
}

bool QuestJournal::Apply(MpActor& actor, const nlohmann::json& command)
{
  RequireGameplayWrites();
  RequireJobAccess(actor);
  if (!command.is_object())
    throw std::invalid_argument("Expected quest command object");
  auto& owner = Owner(actor);
  const auto id = Integer(command.at("quest"), 0xffffffff, "quest ID");
  const auto def = Lookup(owner, id);
  const auto op = command.at("op").get<std::string>();
  auto ledger = Read(owner);
  if (command.contains("expectedRevision") &&
      command.at("expectedRevision") != ledger.at("revision")) {
    throw std::runtime_error("Stale quest revision");
  }
  RequireAdapter(actor, id);
  const auto before = ledger;
  auto& quest = ledger["quests"][def.key];
  if (quest.is_null())
    quest = EmptyQuest();
  if (op == "start") {
    if (quest["running"] == true || quest["completed"] == true)
      return false;
    quest["running"] = quest["started"] = true;
  } else if (op == "stop") {
    quest["running"] = false;
    quest["active"] = false;
  } else if (op == "reset") {
    quest = EmptyQuest();
    if (ledger.contains("vm")) {
      QuestVmState::ResetQuest(ledger["vm"], def.key + "/");
    }
  } else if (op == "complete") {
    if (quest["completed"] != true) {
      auto& world = *owner.GetParent();
      const auto record = world.GetEspm().GetBrowser().LookupById(id);
      const auto metadata =
        espm::QuestLifecycleData::Read(record.rec, world.GetEspmCache());
      quest["completed"] = true;
      // Native completion preserves a miscellaneous quest's active flag and
      // never stops the quest. Repeated completion is a no-op, including when
      // a script explicitly reactivated an already-completed quest.
      if (metadata.questType != 6)
        quest["active"] = false;
    }
  } else if (op == "setActive") {
    quest["active"] = command.at("value").get<bool>();
  } else if (op == "setStage") {
    const auto stage = Integer(command.at("index"), 65535, "stage");
    if (!def.stages.count(stage))
      throw std::invalid_argument("Stage does not exist in this quest");
    auto& stages = quest["stages"];
    if (std::find(stages.begin(), stages.end(), stage) != stages.end())
      return false;
    quest["started"] = quest["running"] = true;
    stages.push_back(stage);
    quest["stage"] = std::max(quest["stage"].get<uint32_t>(), stage);
  } else if (op == "setObjectiveDisplayed" || op == "setObjectiveCompleted" ||
             op == "setObjectiveFailed") {
    const auto index = Integer(command.at("index"), 65535, "objective");
    if (!def.objectives.count(index))
      throw std::invalid_argument("Objective does not exist in this quest");
    auto& objective = quest["objectives"][std::to_string(index)];
    if (objective.is_null())
      objective = { { "displayed", false },
                    { "completed", false },
                    { "failed", false } };
    const auto flag = command.at("value").get<bool>();
    if (op == "setObjectiveDisplayed")
      objective["displayed"] = flag;
    if (op == "setObjectiveCompleted") {
      objective["completed"] = flag;
      if (flag)
        objective["failed"] = false;
    }
    if (op == "setObjectiveFailed") {
      objective["failed"] = flag;
      if (flag)
        objective["completed"] = false;
    }
  } else if (op == "completeAllObjectives" || op == "failAllObjectives") {
    for (auto& objective : quest["objectives"]) {
      if (objective.at("displayed") != true)
        continue;
      objective["completed"] = op == "completeAllObjectives";
      objective["failed"] = op == "failAllObjectives";
    }
  } else {
    throw std::invalid_argument("Unknown quest operation");
  }
  if (ledger == before)
    return false;
  const auto revision = ledger.at("revision").get<uint64_t>();
  if (revision == kMaxRevision)
    throw std::overflow_error("Quest revision exhausted");
  ledger["revision"] = revision + 1;
  Validate(ledger, !transaction);
  const auto dump = ledger.dump();
  if (dump.size() > 8 * 1024 * 1024) {
    throw std::runtime_error("Campaign journal too large");
  }
  // Single change-form update: validation failures never partially change a
  // journal; the ordinary save path persists it with the owning character.
  if (transaction)
    transaction->ledger = std::move(ledger);
  else
    owner.SetPropertyValueDump(kProperty, dump, false, false);
  return true;
}

void QuestJournal::RecordRepeatedStage(MpActor& actor, uint32_t questId,
                                       uint16_t stage)
{
  RequireGameplayWrites();
  if (!actor.GetParent())
    throw std::runtime_error("Unregistered quest transaction participant");
  auto& owner = Owner(actor);
  RequireJobAccess(actor);
  if (!transaction || transaction->owner != &owner)
    throw std::runtime_error(
      "Repeated stage execution requires an owning quest transaction");
  RequireAdapter(actor, questId);
  const auto def = Lookup(owner, questId);
  auto& world = *owner.GetParent();
  const auto record = world.GetEspm().GetBrowser().LookupById(questId);
  if (!record.rec || record.rec->IsDeleted())
    throw std::invalid_argument("Expected a live repeated quest record");
  const auto metadata =
    espm::QuestLifecycleData::Read(record.rec, world.GetEspmCache());
  if (!(metadata.flags & espm::QuestLifecycleData::AllowRepeatedStages))
    throw std::runtime_error("Quest does not allow repeated stages");
  if (!def.stages.count(stage))
    throw std::invalid_argument("Stage does not exist in this quest");
  if (std::count_if(
        metadata.stages.begin(), metadata.stages.end(),
        [stage](const auto& entry) { return entry.index == stage; }) != 1)
    throw std::runtime_error("Repeated stage definition is ambiguous");
  auto ledger = Read(owner);
  const auto found = ledger.at("quests").find(def.key);
  if (found == ledger.at("quests").end() || found->at("running") != true)
    throw std::runtime_error("Repeated stage requires a running quest");
  const auto& stages = found->at("stages");
  if (std::find(stages.begin(), stages.end(), stage) == stages.end())
    throw std::runtime_error("Repeated stage has not been visited");
  const auto revision = ledger.at("revision").get<uint64_t>();
  if (revision == kMaxRevision)
    throw std::overflow_error("Quest revision exhausted");
  ledger["revision"] = revision + 1;
  Validate(ledger, !transaction);
  if (ledger.dump().size() > 8 * 1024 * 1024)
    throw std::runtime_error("Campaign journal too large");
  transaction->ledger = std::move(ledger);
}

void QuestJournal::Atomic(MpActor& actor, const std::function<void()>& execute)
{
  AtomicImpl(actor, execute, false);
}

std::shared_ptr<const SaveReceipt> QuestJournal::AtomicDurable(
  MpActor& actor, const std::function<void()>& execute)
{
  return AtomicImpl(actor, execute, true);
}

std::shared_ptr<const SaveReceipt> QuestJournal::AtomicJob(
  MpActor& actor, uint64_t jobId, const std::function<void()>& execute)
{
  return AtomicImpl(actor, execute, true, jobId);
}

std::shared_ptr<const SaveReceipt> QuestJournal::AtomicJobAdmissions(
  MpActor& actor, const std::vector<uint64_t>& jobIds,
  const std::function<void()>& prepare,
  const std::function<void(uint64_t)>& initializeOne)
{
  if (jobIds.size() > QuestJobState::MaxBatchOccurrences)
    throw std::runtime_error("Native admission batch exceeds capacity");
  // Host callbacks must not invalidate the transaction's selected-ID plan.
  const auto ids = jobIds;
  return AtomicImpl(
    actor,
    [&] {
      const auto beforePrepare = transaction->ledger;
      {
        struct PreparationGuard
        {
          bool& active;
          explicit PreparationGuard(bool& value)
            : active(value)
          {
            active = true;
          }
          ~PreparationGuard() { active = false; }
        } guard(transaction->admissionPreparing);
        prepare();
      }
      if (transaction->ledger != beforePrepare)
        throw std::runtime_error("Admission preparation changed the journal");
      for (const auto id : ids) {
        transaction->jobId = id;
        transaction->jobTransitionApplied = false;
        transaction->admissionRuntimeEntered = false;
        const auto beforeHeap =
          SavedHeap(transaction->ledger.value("vm", Json::object()));
        initializeOne(id);
        RequireAdmissionHeap(
          beforeHeap,
          SavedHeap(transaction->ledger.value("vm", Json::object())));
        const auto* job = transaction->jobs->FindLive(id);
        if (!job || !IsAdmissionSnapshot(*job) ||
            !transaction->jobTransitionApplied)
          throw std::runtime_error(
            "Admission did not save its initial recipient snapshot");
      }
    },
    true, std::nullopt, &ids);
}

QuestJournal::AdmissionBatchCommit QuestJournal::AtomicJobAdmissionRoots(
  MpActor& actor, const std::vector<QuestJobState::Job>& pristineRoots,
  const std::function<void()>& prepare,
  const std::function<void(QuestJobState::Job&)>& captureOne)
{
  if (pristineRoots.size() > QuestJobState::MaxBatchOccurrences)
    throw std::runtime_error("Native admission batch exceeds capacity");
  const auto specs = pristineRoots;
  auto working = specs;
  std::vector<uint64_t> ids;
  ids.reserve(specs.size());
  for (const auto& root : specs)
    ids.push_back(root.id);
  AdmissionBatchCommit result;
  result.receipt = AtomicImpl(
    actor,
    [&] {
      transaction->admissionBuilder = true;
      const auto initialJobs = transaction->jobs;
      auto retained = *initialJobs;
      if (!specs.empty())
        result.retiredIds = retained.RetireForAdmission();
      auto planned = retained;
      const auto ownerName =
        transaction->owner->GetChangeForm().formDesc.ToString();
      const auto actorName = actor.GetChangeForm().formDesc.ToString();
      const auto identity = specs.empty()
        ? std::string{}
        : CampaignReferenceBinding::GetIdentity(*transaction->owner);
      if (specs.size() > kMaxRevision - retained.ingress.next)
        throw std::runtime_error("Native occurrence identities exhausted");
      for (const auto& root : specs) {
        if (!root.IsPristine() ||
            root.kind != QuestJobState::Kind::Interaction ||
            !root.interaction || !root.sequenced || !root.occurrence ||
            root.id != planned.nextJob ||
            root.generation != planned.generation ||
            *root.occurrence != planned.ingress.next ||
            root.owner != ownerName || root.actor != actorName ||
            root.instance != actor.GetWorldInstance() ||
            (!planned.ingress.identity.empty() &&
             planned.ingress.identity != identity))
          throw std::runtime_error(
            "Invalid pristine native admission identity");
        if (!transaction->initialLedger->contains("referenceInit") ||
            !transaction->initialLedger->at("referenceInit")
               .contains(root.interaction->target))
          throw std::runtime_error(
            "Native ingress requires prior reference initialization");
        planned.live.emplace(root.id, root);
        ++planned.nextJob;
        planned.ingress.identity = identity;
        ++planned.ingress.next;
      }
      // Validate every spec and the complete future identity/retention
      // envelope before preparation or runtime entry. No caller supplies
      // retirement.
      planned.Save();
      auto base = std::make_shared<const QuestJobState>(std::move(retained));
      const auto beforePrepare = transaction->ledger;
      {
        struct Guard
        {
          bool& active;
          explicit Guard(bool& flag)
            : active(flag)
          {
            active = true;
          }
          ~Guard() { active = false; }
        } guard(transaction->admissionPreparing);
        prepare();
      }
      if (transaction->ledger != beforePrepare)
        throw std::runtime_error("Admission preparation changed the journal");
      if (transaction->movement &&
          transaction->movement->GetEvents().size() != specs.size())
        throw std::runtime_error("Movement admission omitted an occurrence");
      if (specs.empty())
        return;
      auto builder = std::make_unique<AdmissionBuilder>();
      builder->identity = identity;
      transaction->jobs = base;
      transaction->builder = std::move(builder);
      const auto advanceRevision = [&] {
        const auto revision =
          transaction->ledger.at("revision").get<uint64_t>();
        if (revision == kMaxRevision)
          throw std::overflow_error("Quest revision exhausted");
        transaction->ledger["revision"] = revision + 1;
      };
      for (size_t index = 0; index < specs.size(); ++index) {
        const auto& pristine = specs[index];
        auto& output = working[index];
        transaction->jobId = pristine.id;
        transaction->admissionRuntimeEntered = false;
        transaction->builder->successfulCapture.reset();
        transaction->builder->roots.emplace(
          pristine.id, std::make_shared<const QuestJobState::Job>(pristine));
        advanceRevision();
        const auto beforeHeap =
          SavedHeap(transaction->ledger.value("vm", Json::object()));
        captureOne(output);
        RequireAdmissionHeap(
          beforeHeap,
          SavedHeap(transaction->ledger.value("vm", Json::object())));
        if (!transaction->builder->successfulCapture ||
            output != *transaction->builder->successfulCapture)
          throw std::runtime_error(
            "Admission did not retain its successful runtime capture");
        transaction->builder->roots.at(pristine.id)
          .swap(transaction->builder->successfulCapture);
        advanceRevision();
      }
      // A callback can retain an earlier output reference only for this call's
      // lifetime. Even such a later mutation cannot rewrite accepted
      // snapshots.
      for (size_t index = 0; index < working.size(); ++index)
        if (working[index] != *transaction->builder->roots.at(specs[index].id))
          throw std::runtime_error(
            "Admission rewrote an earlier captured root");
      auto assembled = TransactionJobs();
      if (assembled.completed != base->completed ||
          assembled.retiredThrough != base->retiredThrough ||
          assembled.retiredRanges != base->retiredRanges ||
          assembled.ingress.retiredThrough != base->ingress.retiredThrough ||
          assembled.ingress.retiredRanges != base->ingress.retiredRanges)
        throw std::runtime_error("Admission changed prepared retirement");
      for (const auto& [id, original] : initialJobs->live)
        if (!assembled.FindLive(id) || *assembled.FindLive(id) != original)
          throw std::runtime_error("Admission changed an existing live root");
      auto saved = assembled.Save();
      auto ledger = transaction->ledger;
      ledger["schemaVersion"] = 2;
      ledger["jobs"] = std::move(saved);
      Validate(ledger, false);
      auto snapshot =
        std::make_shared<const QuestJobState>(std::move(assembled));
      transaction->ledger.swap(ledger);
      transaction->jobs.swap(snapshot);
      transaction->builder.reset();
    },
    true, std::nullopt, &ids);
  return result;
}

void QuestJournal::CompleteAdmissionCapture(MpActor& actor,
                                            const QuestJobState::Job& job)
{
  if (!transaction || !transaction->admissionBuilder)
    return;
  RequireJobExecution(actor, job.id);
  if (!transaction->builder || !transaction->admissionRuntimeEntered ||
      transaction->builder->successfulCapture)
    throw std::runtime_error("Admission runtime capture is unavailable");
  const auto& pristine = *TransactionLive(job.id);
  auto captured = job;
  ++captured.segment;
  auto expected = pristine;
  expected.segment = 1;
  expected.steps = 1;
  expected.instructions = 1;
  expected.dispatch = captured.dispatch;
  if (captured != expected || !IsAdmissionSnapshot(captured) ||
      !ValidateDispatchTransition(pristine, captured, false))
    throw std::runtime_error(
      "Admission runtime did not capture exactly one recipient snapshot");
  transaction->builder->successfulCapture =
    std::make_shared<const QuestJobState::Job>(
      QuestJobState::ValidateJob(captured));
}

void QuestJournal::StageMovement(
  MpActor& requester, const std::shared_ptr<CampaignMovementPlan>& plan)
{
  if (!transaction || !transaction->durable || !transaction->admissionOnly ||
      !transaction->admissionPreparing || transaction->jobId ||
      transaction->requester != &requester || !plan || transaction->movement ||
      transaction->stagingMovement)
    throw std::runtime_error(
      "Movement requires the requester's unused admission prepare lease");
  struct StagingGuard
  {
    bool& active;
    explicit StagingGuard(bool& value)
      : active(value)
    {
      active = true;
    }
    ~StagingGuard() { active = false; }
  } guard(transaction->stagingMovement);
  plan->Validate(requester);
  transaction->movement = plan;
}

std::shared_ptr<const SaveReceipt> QuestJournal::AtomicImpl(
  MpActor& actor, const std::function<void()>& execute, bool durable,
  std::optional<uint64_t> jobId, const std::vector<uint64_t>* admissions)
{
  if (transaction)
    throw std::runtime_error("Nested quest transaction");
  if (!actor.GetParent())
    throw std::runtime_error("Unregistered quest transaction participant");
  auto& owner = Owner(actor);
  auto& world = *actor.GetParent();
  const auto actorId = actor.GetFormId(), ownerId = owner.GetFormId();
  // A backend capability callback may remove or replace either participant.
  const auto actorLifetime = world.LookupFormByIdNoLoad(actorId);
  const auto ownerLifetime = world.LookupFormByIdNoLoad(ownerId);
  if (actorLifetime.get() != &actor || ownerLifetime.get() != &owner)
    throw std::runtime_error("Unregistered quest transaction participant");
  const auto actorEpoch = actor.GetWorldInstanceEpoch();
  const auto ownerEpoch = owner.GetWorldInstanceEpoch();
  const auto validateContext = [&] {
    if (world.LookupFormByIdNoLoad(actorId) != actorLifetime ||
        world.LookupFormByIdNoLoad(ownerId) != ownerLifetime ||
        &Owner(actor) != &owner ||
        actor.GetWorldInstanceEpoch() != actorEpoch ||
        owner.GetWorldInstanceEpoch() != ownerEpoch)
      throw std::runtime_error("Quest context changed during execution");
  };
  const auto resumeLease = world.GetQuestJobs().RequireJournalAccess(
    actor, admissions && admissions->empty());
  if (durable) {
    world.RequireDurableSaveStorage();
    validateContext();
    world.GetQuestJobs().ValidateJournalAccess(actor, resumeLease);
  }
  const auto initial = Read(owner);
  const auto initialJobs = std::make_shared<const QuestJobState>(
    initial.contains("jobs") ? QuestJobState::Read(initial.at("jobs"))
                             : QuestJobState{});
  const auto& jobs = *initialJobs;
  if (jobId) {
    const auto* selected = jobs.FindLive(*jobId);
    if (!*jobId || (!selected && jobs.nextJob != *jobId))
      throw std::runtime_error("Quest transaction does not own this job");
    // These admission failures are known before a candidate is installed.
    // Reject them before invoking host code, while allowing an existing
    // migrated root to finish alone under its original execution lease.
    if (!selected && jobs.live.size() >= QuestJobState::MaxLive)
      throw std::runtime_error("Live quest job limit exceeded");
    if (!selected &&
        std::any_of(jobs.live.begin(), jobs.live.end(), [](const auto& entry) {
          return entry.second.environmentHash.empty();
        }))
      throw std::runtime_error(
        "Concurrent quest admission requires a pinned live environment");
    if (selected &&
        (selected->actor != actor.GetChangeForm().formDesc.ToString() ||
         selected->owner != owner.GetChangeForm().formDesc.ToString() ||
         selected->instance != actor.GetWorldInstance()))
      throw std::runtime_error("Quest transaction changed its job context");
    // Admission publishes a pristine typed root before any script executes.
    // Its codec/transition checks enforce the separate legacy and numbered
    // windows; total completed count is no longer a lifetime limit.
  } else if (admissions) {
    if (admissions->size() > QuestJobState::MaxBatchOccurrences ||
        admissions->size() > QuestJobState::MaxLive - jobs.live.size() ||
        admissions->size() > kMaxRevision - jobs.nextJob)
      throw std::runtime_error("Native admission batch exceeds capacity");
    for (size_t index = 0; index < admissions->size(); ++index)
      if ((*admissions)[index] != jobs.nextJob + index)
        throw std::runtime_error("Admission IDs must be contiguous new jobs");
    if (!admissions->empty() &&
        std::any_of(jobs.live.begin(), jobs.live.end(), [](const auto& entry) {
          return entry.second.environmentHash.empty();
        }))
      throw std::runtime_error(
        "Concurrent admission requires a pinned live environment");
  } else if (jobs.HasLive())
    throw std::runtime_error("Campaign has an active quest job");
  // Snapshot all stack envelopes before the callback. Arrays remain shared
  // heap nodes, so their contents may legitimately change in another root.
  const auto initialContinuations = (jobId || admissions)
    ? SavedContinuations(initial.value("vm", Json::object()))
    : Json::object();
  const auto initialJournal = owner.GetDynamicFields().GetValueDump(kProperty);
  Transaction local{ &owner, initial, {}, {} };
  local.jobs = initialJobs;
  local.requester = &actor;
  local.durable = durable;
  local.jobId = jobId;
  local.admissionOnly = admissions != nullptr;
  local.initialLedger = &initial;
  transaction = &local;
  try {
    execute();
    const auto validateReads = [&] {
      validateContext();
      if (owner.GetDynamicFields().GetValueDump(kProperty) != initialJournal)
        throw std::runtime_error(
          "Campaign journal changed outside its transaction");
      for (const auto& [id, edit] : local.inventories) {
        if (world.LookupFormByIdNoLoad(id) != edit.lifetime ||
            edit.reference->IsDeleted() ||
            edit.reference->GetWorldInstanceEpoch() != edit.epoch ||
            edit.reference->GetInventory() != edit.initial ||
            edit.reference->GetChangeForm().baseContainerAdded !=
              edit.baseContainerAdded)
          throw std::runtime_error(
            "Campaign inventory changed during execution");
      }
      for (const auto& [id, edit] : local.references) {
        if (world.LookupFormByIdNoLoad(id) != edit.lifetime ||
            edit.reference->GetWorldInstanceEpoch() != edit.epoch ||
            MpChangeForm::ToJson(edit.reference->GetChangeForm()) !=
              edit.serializedInitial ||
            (edit.reference != &owner &&
             CampaignReferenceBinding::Context(*edit.reference).first !=
               &owner))
          throw std::runtime_error(
            "Campaign reference changed during execution");
      }
      if (local.movement)
        local.movement->Validate(actor);
      world.GetQuestJobs().ValidateJournalAccess(actor, resumeLease);
    };
    validateReads();
    if (jobId) {
      const auto candidate = QuestJobState::Read(local.ledger.at("jobs"));
      if (!candidate.FindLive(*jobId) && !candidate.FindCompleted(*jobId))
        throw std::runtime_error("Quest transaction did not retain its job");
      RequireUnchangedContinuations(
        jobs, *jobId, initialContinuations,
        SavedContinuations(local.ledger.value("vm", Json::object())));
    }
    if (admissions) {
      const auto finalJobs = local.ledger.contains("jobs")
        ? QuestJobState::Read(local.ledger.at("jobs"))
        : QuestJobState{};
      if (finalJobs.nextJob != jobs.nextJob + admissions->size())
        throw std::runtime_error("Admission batch lost its job identities");
      if (local.movement) {
        // Both admission APIs must represent every physical occurrence in
        // order. The plan pins endpoint lifetimes; successful builder runtime
        // capture separately pins the canonical bindings and templates.
        const auto& events = local.movement->GetEvents();
        if (events.size() != admissions->size())
          throw std::runtime_error("Movement admission omitted an occurrence");
        const auto sourceReference =
          local.movement->GetSource().GetChangeForm().formDesc.ToString();
        for (size_t index = 0; index < events.size(); ++index) {
          const auto& event = events[index];
          auto* target =
            event.target ? event.target->AsObjectReference() : nullptr;
          const auto* admitted = finalJobs.FindLive((*admissions)[index]);
          if (!target || !admitted ||
              admitted->kind != QuestJobState::Kind::Interaction ||
              !admitted->sequenced || !admitted->occurrence ||
              !admitted->interaction ||
              admitted->interaction->name != event.name ||
              admitted->interaction->targetReference !=
                target->GetChangeForm().formDesc.ToString() ||
              admitted->interaction->sourceReference != sourceReference)
            throw std::runtime_error(
              "Movement admission changed its occurrence payload or order");
        }
      }
      RequireUnchangedContinuations(
        jobs, 0, initialContinuations,
        SavedContinuations(local.ledger.value("vm", Json::object())));
      RequireAdmissionHeap(
        SavedHeap(initial.value("vm", Json::object())),
        SavedHeap(local.ledger.value("vm", Json::object())));
      auto before = initial, after = local.ledger;
      for (const auto* key : { "jobs", "vm", "revision", "schemaVersion" }) {
        before.erase(key);
        after.erase(key);
      }
      if (before != after || (admissions->empty() && initial != local.ledger))
        throw std::runtime_error("Admission batch changed gameplay state");
      for (const auto& [id, edit] : local.inventories)
        if (edit.initial != edit.current)
          throw std::runtime_error("Admission batch changed inventory");
      for (const auto& [id, edit] : local.references)
        if (MpChangeForm::ToJson(edit.initial) !=
            MpChangeForm::ToJson(edit.current))
          throw std::runtime_error("Admission batch changed a reference");
    }
    Validate(local.ledger);
    const auto dump = local.ledger.dump();
    if (dump.size() > 8 * 1024 * 1024)
      throw std::runtime_error("Campaign journal too large");
    std::vector<WorldState::CampaignInventoryChange> changes;
    for (const auto& [id, edit] : local.inventories) {
      InventoryTarget(actor, *edit.reference);
      if (edit.initial != edit.current)
        changes.push_back({ edit.reference, edit.current });
    }
    std::vector<WorldState::CampaignVisibilityChange> visibility;
    for (const auto& [id, edit] : local.references)
      if (edit.initial.isDisabled != edit.current.isDisabled)
        visibility.push_back({ edit.reference, edit.current.isDisabled });
    std::vector<WorldState::CampaignFactionChange> factions;
    for (const auto& [id, edit] : local.references)
      if (edit.factionsEdited &&
          !EqualFactions(*edit.factionBaseline, *edit.current.factions))
        factions.push_back(
          { edit.reference->AsActor(), *edit.current.factions });
    transaction = nullptr;
    if (durable || !changes.empty() || !visibility.empty() ||
        !factions.empty())
      return world.CommitCampaignInventory(owner, dump, changes, visibility,
                                           factions, validateReads, durable,
                                           local.movement);
    else if (local.ledger != initial)
      owner.SetPropertyValueDump(kProperty, dump, false, false);
    return {};
  } catch (...) {
    transaction = nullptr;
    throw;
  }
}

const Inventory& QuestJournal::ReadInventory(MpActor& actor,
                                             MpObjectReference& target)
{
  return InventoryTarget(actor, target).current;
}

bool QuestJournal::HasActiveTransaction() noexcept
{
  return transaction != nullptr;
}

void QuestJournal::EditInventory(MpActor& actor, MpObjectReference& target,
                                 const std::function<void(Inventory&)>& edit)
{
  RequireGameplayWrites();
  auto& staged = InventoryTarget(actor, target);
  auto prepared = staged.current;
  edit(prepared);
  staged.current = std::move(prepared);
  if (const auto found = transaction->references.find(target.GetFormId());
      found != transaction->references.end())
    found->second.current.inv = staged.current;
}

const MpChangeFormREFR& QuestJournal::ReadReference(MpActor& actor,
                                                    MpObjectReference& target)
{
  return ReferenceTarget(actor, target).current;
}

const std::vector<Faction>& QuestJournal::ReadFactions(
  MpActor& actor, MpObjectReference& target)
{
  if (!target.AsActor())
    throw std::invalid_argument("Campaign faction target must be an actor");
  auto& staged = ReferenceTarget(actor, target);
  if (!staged.factionBaseline) {
    auto effective = CampaignFactionApi::InitialFactions(
      *target.AsActor(), staged.initial, &target == &Owner(actor));
    staged.factionBaseline = effective;
    staged.current.factions = std::move(effective);
  }
  return *staged.current.factions;
}

void QuestJournal::EditFactions(
  MpActor& actor, MpObjectReference& target,
  const std::function<void(std::vector<Faction>&)>& edit)
{
  RequireGameplayWrites();
  auto prepared = ReadFactions(actor, target);
  edit(prepared);
  target.GetParent()->ValidateCampaignFactions(prepared);
  auto& staged = ReferenceTarget(actor, target);
  staged.current.factions = std::move(prepared);
  staged.factionsEdited = true;
}

void QuestJournal::SetReferenceDisabled(MpActor& actor,
                                        MpObjectReference& target,
                                        bool disabled)
{
  RequireGameplayWrites();
  if (&target == &Owner(actor))
    throw std::runtime_error("Campaign visibility cannot disable a player");
  auto& staged = ReferenceTarget(actor, target);
  const auto original = CampaignReferenceBinding::Context(target).second;
  if (actor.GetParent()->HasCampaignEnableLinks(original))
    throw std::runtime_error(
      "Campaign visibility requires enable-parent propagation");
  target.ValidateCampaignVisibilityChange();
  staged.current.isDisabled = disabled;
}

nlohmann::json QuestJournal::ScriptState(MpActor& actor)
{
  return PersonalScriptState(Owner(actor));
}

nlohmann::json QuestJournal::PersonalScriptState(MpActor& actor)
{
  auto value = Read(actor).value("vm", Json::object());
  if (!value.is_object())
    throw std::runtime_error("Invalid saved quest VM state");
  return value;
}

QuestJobState QuestJournal::JobState(MpActor& actor)
{
  return PersonalJobState(Owner(actor));
}

QuestJobState QuestJournal::PersonalJobState(MpActor& owner)
{
  if (transaction) {
    if (transaction->owner != &owner)
      throw std::runtime_error("Cross-campaign quest transaction");
    // The public result remains an owned copy. A caller cannot mutate the
    // selected lease or a sleeping root through a borrowed cache reference.
    return TransactionJobs();
  }
  const auto ledger = Read(owner);
  return ledger.contains("jobs") ? QuestJobState::Read(ledger.at("jobs"))
                                 : QuestJobState{};
}

void QuestJournal::RequireJobAccess(MpActor& actor)
{
  if (auto* world = actor.GetParent())
    world->GetQuestJobs().RequireVisitorReturned(actor);
  RequirePersonalJobAccess(Owner(actor));
}

void QuestJournal::RequirePersonalJobAccess(MpActor& owner)
{
  if (transaction) {
    if (transaction->owner != &owner)
      throw std::runtime_error("Cross-campaign quest transaction");
    if (!transaction->ledger.contains("jobs") && !transaction->builder)
      return;
    const auto& jobs = *transaction->jobs;
    if (transaction->jobId) {
      // The selected root can be terminal by commit-time inventory validation.
      // Its final result still belongs to this transaction's original lease.
      if (!TransactionLive(*transaction->jobId) &&
          !jobs.FindCompleted(*transaction->jobId))
        throw std::runtime_error("Quest job has not installed its root");
      return;
    }
    if (jobs.HasLive())
      throw std::runtime_error("Campaign has an active quest job");
    return;
  }
  if (owner.GetParent() &&
      owner.GetParent()->GetQuestJobs().HasResumeHold(owner))
    throw std::runtime_error("Campaign execution is held for recovery");
  if (PersonalJobState(owner).HasLive())
    throw std::runtime_error("Campaign has an active quest job");
}

void QuestJournal::RequireJobExecution(MpActor& actor, uint64_t jobId)
{
  auto& owner = Owner(actor);
  if (!jobId || !transaction || transaction->owner != &owner ||
      transaction->jobId != jobId)
    throw std::runtime_error("Quest runtime does not own this job lease");
  const auto* selected = TransactionLive(jobId);
  if (!selected ||
      selected->actor != actor.GetChangeForm().formDesc.ToString() ||
      selected->owner != owner.GetChangeForm().formDesc.ToString() ||
      selected->instance != actor.GetWorldInstance())
    throw std::runtime_error("Quest runtime job context changed");
}

void QuestJournal::RequireJobExecution(MpActor& actor,
                                       const QuestJobState::Job& job)
{
  RequireJobExecution(actor, job.id);
  if (transaction->admissionOnly) {
    const auto* selected = TransactionLive(job.id);
    if (transaction->admissionRuntimeEntered || !selected ||
        *selected != job || !job.IsPristine() ||
        job.kind != QuestJobState::Kind::Interaction || !job.occurrence)
      throw std::runtime_error(
        "Admission runtime may only capture one pristine recipient roster");
    transaction->admissionRuntimeEntered = true;
  }
}

void QuestJournal::SetJobState(MpActor& actor, const QuestJobState& state)
{
  auto& owner = Owner(actor);
  if (!transaction || transaction->owner != &owner || !transaction->jobId)
    throw std::runtime_error("Job state requires an owning job transaction");
  if (transaction->admissionBuilder)
    throw std::runtime_error("Admission builder owns its job snapshots");
  const auto saved = state.Save();
  const auto previousSnapshot = transaction->jobs;
  const auto& previous = *previousSnapshot;
  const auto old = previous.Save();
  if (transaction->ledger.contains("jobs") && old == saved)
    return;
  if (transaction->jobTransitionApplied)
    throw std::runtime_error(
      "Job transaction already advanced its durable cursor");
  const auto id = *transaction->jobId;
  const auto* priorJob = previous.FindLive(id);
  const auto* candidate = state.FindLive(id);
  if (!candidate)
    candidate = state.FindCompleted(id);
  if (!candidate || candidate->id != id ||
      state.generation != previous.generation ||
      state.nextJob != (priorJob ? previous.nextJob : previous.nextJob + 1) ||
      candidate->owner != owner.GetChangeForm().formDesc.ToString() ||
      candidate->actor != actor.GetChangeForm().formDesc.ToString() ||
      candidate->instance != actor.GetWorldInstance())
    throw std::runtime_error("Job transaction changed another job identity");
  if (!priorJob &&
      (id != previous.nextJob || !state.FindLive(id) ||
       !candidate->IsPristine()))
    throw std::runtime_error(
      "New quest job must first install its pristine root");
  if (!priorJob) {
    for (const auto& [otherId, other] : previous.live)
      if (other.environmentHash.empty() ||
          candidate->environmentHash.empty() ||
          other.environmentHash != candidate->environmentHash ||
          other.contentMode != candidate->contentMode)
        throw std::runtime_error(
          "Concurrent quest jobs require the same pinned environment");
  }
  const auto expectedLive =
    previous.live.size() + (priorJob ? 0 : 1) - (state.FindLive(id) ? 0 : 1);
  if (state.live.size() != expectedLive)
    throw std::runtime_error("Job transaction changed another live root");
  for (const auto& [otherId, other] : previous.live)
    if (otherId != id &&
        (!state.FindLive(otherId) || *state.FindLive(otherId) != other))
      throw std::runtime_error("Job transaction rewrote another live root");
  auto retained = previous;
  if (!priorJob && candidate->sequenced)
    retained.RetireForAdmission();
  if (priorJob && !state.FindLive(id)) {
    retained.live.at(id) = *candidate;
    retained.CompleteJob(id);
  }
  if (state.retiredThrough != retained.retiredThrough ||
      state.retiredRanges != retained.retiredRanges)
    throw std::runtime_error(
      "Job transaction changed its retirement frontier");
  auto expectedIngress = retained.ingress;
  if (!priorJob && candidate->occurrence) {
    if (*candidate->occurrence != expectedIngress.next)
      throw std::runtime_error("Native ingress skipped its next occurrence");
    const auto identity = CampaignReferenceBinding::GetIdentity(owner);
    if (!expectedIngress.identity.empty() &&
        expectedIngress.identity != identity)
      throw std::runtime_error("Native ingress campaign identity changed");
    expectedIngress.identity = identity;
    ++expectedIngress.next;
    // A marker created during this transaction is not an admission lease.
    // The runner also gates the exact earlier receipt before entering here.
    if (!transaction->initialLedger->contains("referenceInit") ||
        !transaction->initialLedger->at("referenceInit")
           .contains(candidate->interaction->target))
      throw std::runtime_error(
        "Native ingress requires prior reference initialization");
  }
  if (state.ingress != expectedIngress)
    throw std::runtime_error("Job transaction rewrote native ingress history");
  if (priorJob) {
    const auto& prior = *priorJob;
    if (candidate->id != prior.id ||
        candidate->generation != prior.generation ||
        candidate->command != prior.command ||
        candidate->sequenced != prior.sequenced ||
        candidate->automatic != prior.automatic ||
        candidate->contentMode != prior.contentMode ||
        candidate->environmentHash != prior.environmentHash ||
        candidate->kind != prior.kind ||
        candidate->interaction != prior.interaction ||
        candidate->referenceLoad != prior.referenceLoad ||
        candidate->occurrence != prior.occurrence ||
        candidate->actor != prior.actor || candidate->owner != prior.owner ||
        candidate->quest != prior.quest ||
        candidate->instance != prior.instance ||
        candidate->contentHash != prior.contentHash ||
        candidate->stage != prior.stage ||
        candidate->segment < prior.segment ||
        candidate->nextOperation < prior.nextOperation ||
        candidate->nextOperation > prior.nextOperation + 1 ||
        candidate->instructions < prior.instructions ||
        candidate->steps < prior.steps ||
        (prior.accepted && !candidate->accepted))
      throw std::runtime_error("Job transaction rewrote immutable work");
    for (const auto& [token, result] : prior.completions)
      if (!candidate->completions.contains(token) ||
          candidate->completions.at(token) != result)
        throw std::runtime_error("Job transaction rewrote a completion");
    const bool referenceWasInitialized = prior.referenceLoad &&
      transaction->initialLedger->contains("referenceInit") &&
      transaction->initialLedger->at("referenceInit")
        .contains(prior.referenceLoad->target);
    if (!ValidateDispatchTransition(prior, *candidate,
                                    referenceWasInitialized)) {
      using Stage = QuestJobState::Stage;
      const auto sameStage = [](const Stage& a, const Stage& b) {
        return a == b;
      };
      std::vector<const Stage*> before{ prior.handler ? &*prior.handler
                                                      : &prior },
        after{ candidate->handler ? &*candidate->handler : candidate };
      for (const auto& child : prior.children)
        before.push_back(&child);
      for (const auto& child : candidate->children)
        after.push_back(&child);
      size_t shared = 0;
      while (shared < before.size() && shared < after.size() &&
             before[shared]->node == after[shared]->node)
        ++shared;
      if (!shared || before.size() - shared > 1 || after.size() - shared > 1)
        throw std::runtime_error(
          "Job transaction skipped a durable child boundary");
      if (before.size() != shared) {
        const auto& child = *before.back();
        const auto& parent = *before[before.size() - 2];
        const auto completion = candidate->completions.find(parent.token);
        if (child.cursor != "done" ||
            completion == candidate->completions.end() ||
            completion->second != QuestJobState::Completion(child.accepted))
          throw std::runtime_error(
            "Job transaction dropped unfinished child work");
      }
      const Stage* consumed = nullptr;
      if (before.size() != shared)
        consumed = before[before.size() - 2];
      else if (prior.Current().cursor == "waiting" &&
               prior.Current().token != after[shared - 1]->token)
        consumed = &prior.Current();
      for (const auto& [token, result] : candidate->completions)
        if (!prior.completions.contains(token) &&
            (!consumed || token != consumed->token ||
             result.kind !=
               (consumed->awaitKind == "timer"
                  ? QuestJobState::Completion::Kind::None
                  : QuestJobState::Completion::Kind::Boolean)))
          throw std::runtime_error(
            "Job transaction completed an unrequested or mistyped operation");
      for (size_t index = 0; index < shared; ++index) {
        const auto& oldStage = *before[index];
        const auto& nextStage = *after[index];
        if (oldStage.quest != nextStage.quest ||
            oldStage.contentHash != nextStage.contentHash ||
            oldStage.stage != nextStage.stage ||
            (oldStage.accepted && !nextStage.accepted))
          throw std::runtime_error(
            "Job transaction rewrote a live stage identity");
        if ((index + 1 < shared || oldStage.cursor == "done") &&
            !sameStage(oldStage, nextStage))
          throw std::runtime_error(
            "Job transaction rewrote a suspended ancestor");
        if (oldStage.cursor == "waiting") {
          if (oldStage.token != nextStage.token) {
            if (nextStage.cursor == "waiting" &&
                (nextStage.log < oldStage.log ||
                 (nextStage.log == oldStage.log &&
                  (nextStage.script != oldStage.script ||
                   nextStage.function != oldStage.function))))
              throw std::runtime_error(
                "Job transaction rewound a stage fragment cursor");
            if (!candidate->completions.contains(oldStage.token))
              throw std::runtime_error(
                "Job transaction lost a pending operation");
          } else if (oldStage.child != nextStage.child ||
                     oldStage.log != nextStage.log ||
                     oldStage.script != nextStage.script ||
                     oldStage.function != nextStage.function ||
                     oldStage.awaitKey != nextStage.awaitKey ||
                     oldStage.awaitKind != nextStage.awaitKind ||
                     oldStage.timer != nextStage.timer ||
                     nextStage.cursor != "waiting")
            throw std::runtime_error(
              "Job transaction rewrote a pending operation");
        }
      }
      if (after.size() != shared &&
          (after.back()->node != prior.nextOperation ||
           after.back()->cursor != "begin"))
        throw std::runtime_error(
          "Job transaction reused or bypassed a new child");
    }
  }
  auto history = retained.Save().at("completed");
  auto nextHistory = saved.at("completed");
  if (history != nextHistory)
    throw std::runtime_error("Job transaction rewrote another job's history");
  const auto revision = transaction->ledger.at("revision").get<uint64_t>();
  if (revision == kMaxRevision)
    throw std::overflow_error("Quest revision exhausted");
  // Job and heap updates may temporarily disagree inside this transaction;
  // AtomicImpl validates their complete cross-links before publication.
  auto ledger = transaction->ledger;
  ledger["schemaVersion"] = 2;
  ledger["jobs"] = saved;
  ledger["revision"] = revision + 1;
  Validate(ledger, false);
  // Both allocating/validating operations finish before either view changes.
  // A caught validation or allocation failure leaves the old lease and JSON
  // intact. No callback can run between these nonthrowing publications.
  auto nextSnapshot = std::make_shared<const QuestJobState>(state);
  transaction->ledger.swap(ledger);
  transaction->jobs.swap(nextSnapshot);
  // A pristine initial install exposes the new job to script/native guards.
  // Every other transition must receive its own durable receipt before the
  // next cursor can advance; repeated identical calls above remain harmless.
  if (priorJob || !state.FindLive(id) || !candidate->IsPristine())
    transaction->jobTransitionApplied = true;
}

void QuestJournal::SetScriptState(MpActor& actor, const nlohmann::json& state)
{
  RequireJobAccess(actor);
  auto& owner = Owner(actor);
  if (!transaction || transaction->owner != &owner || !state.is_object())
    throw std::runtime_error(
      "Script state requires an owning quest transaction");
  if (transaction->ledger.value("vm", Json::object()) == state)
    return;
  const auto revision = transaction->ledger.at("revision").get<uint64_t>();
  if (revision == kMaxRevision)
    throw std::overflow_error("Quest revision exhausted");
  transaction->ledger["vm"] = state;
  transaction->ledger["revision"] = revision + 1;
}

namespace {
std::string ReferenceKey(MpActor& actor, uint32_t original)
{
  if (!actor.GetParent() || original == 0x14 || original >= 0xff000000)
    throw std::invalid_argument("Expected a static campaign reference");
  auto& world = *actor.GetParent();
  const auto record = world.GetEspm().GetBrowser().LookupById(original);
  if (!record.rec || record.rec->IsDeleted() ||
      (record.rec->GetType() != "REFR" && record.rec->GetType() != "ACHR"))
    throw std::invalid_argument("Expected a live campaign reference");
  return FormDesc::FromFormId(original, world.espmFiles).ToString();
}
}

bool QuestJournal::ReferenceInitialized(MpActor& actor, uint32_t original)
{
  const auto key = ReferenceKey(actor, original);
  auto& owner = Owner(actor);
  if (transaction) {
    if (transaction->owner != &owner)
      throw std::runtime_error("Cross-campaign quest transaction");
    const auto& ledger = transaction->ledger;
    return ledger.contains("referenceInit") &&
      ledger.at("referenceInit").contains(key);
  }
  const auto ledger = Read(owner);
  return ledger.contains("referenceInit") &&
    ledger.at("referenceInit").contains(key);
}

bool QuestJournal::CaptureContext::ReferenceInitialized(
  MpActor& actor, uint32_t original) const
{
  return data->initialized.contains(ReferenceKey(actor, original));
}

void QuestJournal::MarkReferenceInitialized(MpActor& actor, uint32_t original)
{
  RequireGameplayWrites();
  RequireJobAccess(actor);
  const auto key = ReferenceKey(actor, original);
  auto& owner = Owner(actor);
  if (!transaction || transaction->owner != &owner)
    throw std::runtime_error(
      "Reference initialization requires an owning quest transaction");
  auto& ledger = transaction->ledger;
  if (ledger.contains("referenceInit") &&
      ledger.at("referenceInit").contains(key))
    return;
  if (transaction->jobId) {
    const auto* job = TransactionLive(*transaction->jobId);
    if (!job || job->kind != QuestJobState::Kind::ReferenceLoad ||
        !job->referenceLoad || job->referenceLoad->target != key ||
        job->loadPhase != QuestJobState::LoadPhase::Init || job->handler ||
        !job->children.empty() || !job->dispatch || job->dispatch->captured ||
        job->dispatch->recipient != job->dispatch->recipients.size())
      throw std::runtime_error(
        "Reference initialization requires its completed OnInit boundary");
  }
  const auto revision = ledger.at("revision").get<uint64_t>();
  if (revision == kMaxRevision)
    throw std::overflow_error("Quest revision exhausted");
  ledger["referenceInit"][key] = true;
  ledger["revision"] = revision + 1;
}

void QuestJournal::RequireAdapter(MpActor& actor, uint32_t questId)
{
  auto& owner = Owner(actor);
  const auto def = Lookup(owner, questId);
  const auto enabled =
    Json::parse(owner.GetDynamicFields().GetValueDump(kEnabled));
  if (!enabled.is_array() ||
      std::find(enabled.begin(), enabled.end(), def.key) == enabled.end())
    throw std::runtime_error(
      "Quest execution adapter is not enabled for this campaign");
}
