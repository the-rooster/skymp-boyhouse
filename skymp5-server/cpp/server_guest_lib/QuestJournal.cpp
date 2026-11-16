#include "QuestJournal.h"
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
  MpActor* owner;
  Json ledger;
};
thread_local Transaction* transaction = nullptr;

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
  // This gate prevents us from silently claiming that setting a number runs
  // vanilla aliases, conditions, fragments, scenes, rewards, or story events.
  const auto enabled =
    Json::parse(owner.GetDynamicFields().GetValueDump(kEnabled));
  if (!enabled.is_array() ||
      std::find(enabled.begin(), enabled.end(), def.key) == enabled.end()) {
    throw std::runtime_error(
      "Quest execution adapter is not enabled for this campaign");
  }
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
  Transaction local{ &owner, initial };
  transaction = &local;
  try {
    execute();
    if (world.LookupFormByIdNoLoad(actorId) != actorLifetime ||
        world.LookupFormByIdNoLoad(ownerId) != ownerLifetime ||
        &Owner(actor) != &owner ||
        actor.GetWorldInstanceEpoch() != actorEpoch ||
        owner.GetWorldInstanceEpoch() != ownerEpoch)
      throw std::runtime_error("Quest context changed during execution");
    Validate(local.ledger);
    const auto dump = local.ledger.dump();
    if (dump.size() > 8 * 1024 * 1024)
      throw std::runtime_error("Campaign journal too large");
    transaction = nullptr;
    if (local.ledger != initial)
      owner.SetPropertyValueDump(kProperty, dump, false, false);
  } catch (...) {
    transaction = nullptr;
    throw;
  }
}

nlohmann::json QuestJournal::ScriptState(MpActor& actor)
{
  auto value = Read(Owner(actor)).value("vm", Json::object());
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
