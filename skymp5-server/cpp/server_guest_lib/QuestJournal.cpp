#include "QuestJournal.h"
#include "CampaignReferenceBinding.h"
#include "MpActor.h"
#include "QuestVmState.h"
#include "WorldState.h"
#include "libespm/QUST.h"
#include "libespm/RecordHeaderAccess.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <set>

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
  };
  MpActor* owner;
  Json ledger;
  std::map<uint32_t, InventoryEdit> inventories;
  std::map<uint32_t, ReferenceEdit> references;
};
thread_local Transaction* transaction = nullptr;

Transaction::ReferenceEdit& ReferenceTarget(MpActor& actor,
                                            MpObjectReference& target)
{
  if (!actor.GetParent())
    throw std::runtime_error("Unregistered quest transaction participant");
  auto& owner = QuestJournal::Owner(actor);
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

void Validate(const Json& ledger)
{
  if (!ledger.is_object() || ledger.at("schemaVersion") != 1 ||
      !ledger.at("revision").is_number_unsigned() ||
      ledger.at("revision").get<uint64_t>() > kMaxRevision ||
      !ledger.at("quests").is_object() || ledger.at("quests").size() > 10000) {
    throw std::runtime_error("Invalid or unsupported campaign journal");
  }
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
  auto result = Read(owner);
  result.erase("vm"); // Script variables are private server state.
  result.erase("referenceInit");
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
    quest["completed"] = true;
    quest["active"] = false;
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
  Validate(ledger);
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

void QuestJournal::Atomic(MpActor& actor, const std::function<void()>& execute)
{
  if (transaction)
    throw std::runtime_error("Nested quest transaction");
  if (!actor.GetParent())
    throw std::runtime_error("Unregistered quest transaction participant");
  auto& owner = Owner(actor);
  auto& world = *actor.GetParent();
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
  const auto initialJournal = owner.GetDynamicFields().GetValueDump(kProperty);
  Transaction local{ &owner, initial, {}, {} };
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
    transaction = nullptr;
    if (!changes.empty() || !visibility.empty())
      world.CommitCampaignInventory(owner, dump, changes, visibility,
                                    validateReads);
    else if (local.ledger != initial)
      owner.SetPropertyValueDump(kProperty, dump, false, false);
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

void QuestJournal::SetScriptState(MpActor& actor, const nlohmann::json& state)
{
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
