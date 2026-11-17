#include "QuestJournal.h"
#include "CampaignFactionApi.h"
#include "CampaignReferenceBinding.h"
#include "MpActor.h"
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
  std::optional<uint64_t> jobId;
  bool jobTransitionApplied = false;
};
thread_local Transaction* transaction = nullptr;

Json SavedContinuations(const Json& heap)
{
  QuestVmState state(
    heap,
    [](IGameObject*) -> Json {
      throw std::runtime_error("Unexpected continuation object encoding");
    },
    [](const Json&) -> VarValue {
      throw std::runtime_error("Unexpected continuation object resolution");
    });
  return state.Save().at("continuations");
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
                                const QuestJobState::Job& next)
{
  using Stage = QuestJobState::Stage;
  if (next.events.size() < prior.events.size() ||
      !std::equal(prior.events.begin(), prior.events.end(),
                  next.events.begin()))
    throw std::runtime_error(
      "Job transaction rewrote inventory event history");
  const bool host = prior.cursor == "done" &&
    (!prior.handler || prior.handler->cursor == "done");
  const bool primary =
    prior.kind == QuestJobState::Kind::Interaction && !prior.interactionDone;
  const bool finishPrimary = primary && host && !prior.handler &&
    prior.dispatch && !prior.dispatch->captured &&
    prior.dispatch->recipient == prior.dispatch->recipients.size();
  if (next.interactionDone != (finishPrimary ? true : prior.interactionDone))
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
    if (validateHeap)
      jobs.ValidateHeap(ledger.value("vm", Json::object()));
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

Json Read(MpActor& owner)
{
  if (transaction) {
    if (transaction->owner != &owner)
      throw std::runtime_error("Cross-campaign quest transaction");
    return transaction->ledger;
  }
  const auto& dump =
    owner.GetDynamicFields().GetValueDump(QuestJournal::kProperty);
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

MpActor& QuestJournal::Owner(MpActor& actor)
{
  const auto& instance = actor.GetWorldInstance();
  if (instance.empty())
    return actor;
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
      !ownerId) {
    throw std::runtime_error("Invalid campaign owner");
  }
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

std::shared_ptr<const SaveReceipt> QuestJournal::AtomicImpl(
  MpActor& actor, const std::function<void()>& execute, bool durable,
  std::optional<uint64_t> jobId)
{
  if (transaction)
    throw std::runtime_error("Nested quest transaction");
  if (!actor.GetParent())
    throw std::runtime_error("Unregistered quest transaction participant");
  auto& owner = Owner(actor);
  auto& world = *actor.GetParent();
  if (durable)
    world.RequireDurableSaveStorage();
  const auto actorId = actor.GetFormId(), ownerId = owner.GetFormId();
  // Keep both objects alive and require the same registered identities at
  // commit.
  const auto actorLifetime = world.LookupFormByIdNoLoad(actorId);
  const auto ownerLifetime = world.LookupFormByIdNoLoad(ownerId);
  if (actorLifetime.get() != &actor || ownerLifetime.get() != &owner)
    throw std::runtime_error("Unregistered quest transaction participant");
  const auto actorEpoch = actor.GetWorldInstanceEpoch();
  const auto ownerEpoch = owner.GetWorldInstanceEpoch();
  const auto initial = Read(owner);
  const auto jobs = initial.contains("jobs")
    ? QuestJobState::Read(initial.at("jobs"))
    : QuestJobState{};
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
  } else if (jobs.HasLive())
    throw std::runtime_error("Campaign has an active quest job");
  // Snapshot all stack envelopes before the callback. Arrays remain shared
  // heap nodes, so their contents may legitimately change in another root.
  const auto initialContinuations = jobId
    ? SavedContinuations(initial.value("vm", Json::object()))
    : Json::object();
  const auto initialJournal = owner.GetDynamicFields().GetValueDump(kProperty);
  Transaction local{ &owner, initial, {}, {} };
  local.jobId = jobId;
  transaction = &local;
  try {
    execute();
    const auto validateReads = [&] {
      if (world.LookupFormByIdNoLoad(actorId) != actorLifetime ||
          world.LookupFormByIdNoLoad(ownerId) != ownerLifetime ||
          &Owner(actor) != &owner ||
          actor.GetWorldInstanceEpoch() != actorEpoch ||
          owner.GetWorldInstanceEpoch() != ownerEpoch)
        throw std::runtime_error("Quest context changed during execution");
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
                                           factions, validateReads, durable);
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
  const auto ledger = Read(owner);
  return ledger.contains("jobs") ? QuestJobState::Read(ledger.at("jobs"))
                                 : QuestJobState{};
}

void QuestJournal::RequireJobAccess(MpActor& actor)
{
  RequirePersonalJobAccess(Owner(actor));
}

void QuestJournal::RequirePersonalJobAccess(MpActor& owner)
{
  const auto ledger = Read(owner);
  if (!ledger.contains("jobs"))
    return;
  const auto jobs = QuestJobState::Read(ledger.at("jobs"));
  if (transaction && transaction->owner == &owner && transaction->jobId) {
    // The selected root can be terminal by commit-time inventory validation.
    // Its final result still belongs to this transaction's original lease.
    if (!jobs.FindLive(*transaction->jobId) &&
        !jobs.FindCompleted(*transaction->jobId))
      throw std::runtime_error("Quest job has not installed its root");
    return;
  }
  if (jobs.HasLive())
    throw std::runtime_error("Campaign has an active quest job");
}

void QuestJournal::RequireJobExecution(MpActor& actor, uint64_t jobId)
{
  auto& owner = Owner(actor);
  if (!jobId || !transaction || transaction->owner != &owner ||
      transaction->jobId != jobId)
    throw std::runtime_error("Quest runtime does not own this job lease");
  const auto jobs = JobState(actor);
  const auto* selected = jobs.FindLive(jobId);
  if (!selected ||
      selected->actor != actor.GetChangeForm().formDesc.ToString() ||
      selected->owner != owner.GetChangeForm().formDesc.ToString() ||
      selected->instance != actor.GetWorldInstance())
    throw std::runtime_error("Quest runtime job context changed");
}

void QuestJournal::SetJobState(MpActor& actor, const QuestJobState& state)
{
  auto& owner = Owner(actor);
  if (!transaction || transaction->owner != &owner || !transaction->jobId)
    throw std::runtime_error("Job state requires an owning job transaction");
  const auto saved = state.Save();
  const auto previous = JobState(actor);
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
  if (state.retiredThrough != retained.retiredThrough ||
      state.retiredRanges != retained.retiredRanges)
    throw std::runtime_error(
      "Job transaction changed its retirement frontier");
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
    if (!ValidateDispatchTransition(prior, *candidate)) {
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
  if (!state.FindLive(id))
    for (auto it = nextHistory.begin(); it != nextHistory.end(); ++it)
      if (it->at("id").get<uint64_t>() == id) {
        nextHistory.erase(it);
        break;
      }
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
  transaction->ledger = std::move(ledger);
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
  const auto ledger = Read(Owner(actor));
  return ledger.contains("referenceInit") &&
    ledger.at("referenceInit").contains(key);
}

void QuestJournal::MarkReferenceInitialized(MpActor& actor, uint32_t original)
{
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
