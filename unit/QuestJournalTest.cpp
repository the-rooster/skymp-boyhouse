#include "QuestJournal.h"
#include "ConditionsEvaluator.h"
#include "TestUtils.hpp"
#include "condition_functions/ConditionFunctionFactory.h"
#include "script_classes/PapyrusQuest.h"
#include "script_compatibility_policies/HeuristicPolicy.h"
#include "script_objects/EspmGameObject.h"

extern espm::Loader& GetEspmLoader();

namespace {
using Json = nlohmann::json;
constexpr uint32_t kQuest =
  0x4e50d; // MQ102: record validation only, not execution
struct Fixture
{
  PartOne server;
  Fixture() { server.AttachEspm(&GetEspmLoader()); }
  MpActor& Add(uint32_t id)
  {
    server.CreateActor(id, { 1, 2, 3 }, 0, 0x3c);
    auto& actor = server.worldState.GetFormAt<MpActor>(id);
    actor.SetPropertyValueDump(
      QuestJournal::kEnabled,
      Json::array({ FormDesc::FromFormId(kQuest, server.worldState.espmFiles)
                      .ToString() })
        .dump(),
      false, false);
    return actor;
  }
  bool Apply(MpActor& actor, Json command)
  {
    command["quest"] = kQuest;
    return QuestJournal::Apply(actor, command);
  }
};
}

TEST_CASE(
  "Quest progress stays personal until an admitted guest helps the owner",
  "[Quest]")
{
  Fixture f;
  auto& owner = f.Add(0xff000001);
  auto& guest = f.Add(0xff000002);
  auto& stranger = f.Add(0xff000003);
  f.Apply(guest, { { "op", "setStage" }, { "index", 5 } });
  f.Apply(owner, { { "op", "setStage" }, { "index", 10 } });
  const auto guestPersonal = QuestJournal::Get(guest, kQuest);
  REQUIRE(QuestJournal::Get(stranger, kQuest)["stage"] == 0);
  const auto instance =
    "campaign:" + std::to_string(owner.GetFormId()) + ":fixture";
  owner.SetWorldInstance(instance);
  guest.SetWorldInstance(
    instance); // Trusted party authority already admitted guest
  REQUIRE(QuestJournal::Snapshot(guest)["campaignOwner"] == owner.GetFormId());
  REQUIRE(QuestJournal::Get(guest, kQuest)["stage"] == 10);
  f.Apply(guest, { { "op", "setStage" }, { "index", 20 } });
  REQUIRE(QuestJournal::Get(owner, kQuest)["stage"] == 20);
  REQUIRE(QuestJournal::Get(stranger, kQuest)["stage"] == 0);
  guest.SetWorldInstance("");
  REQUIRE(QuestJournal::Get(guest, kQuest) == guestPersonal);
  REQUIRE(QuestJournal::Get(owner, kQuest)["stage"] == 20);
}

TEST_CASE("A journal uses visited stages and explicit objective states",
          "[Quest]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  REQUIRE(f.Apply(actor, { { "op", "start" } }));
  REQUIRE_FALSE(f.Apply(actor, { { "op", "start" } }));
  REQUIRE(f.Apply(actor, { { "op", "setStage" }, { "index", 20 } }));
  REQUIRE(f.Apply(actor, { { "op", "setStage" }, { "index", 10 } }));
  REQUIRE(QuestJournal::Get(actor, kQuest)["stage"] == 20);
  const auto saved = QuestJournal::Snapshot(actor);
  REQUIRE_FALSE(f.Apply(actor, { { "op", "setStage" }, { "index", 20 } }));
  REQUIRE(QuestJournal::Snapshot(actor) == saved);
  f.Apply(
    actor,
    { { "op", "setObjectiveDisplayed" }, { "index", 10 }, { "value", true } });
  f.Apply(
    actor,
    { { "op", "setObjectiveFailed" }, { "index", 10 }, { "value", true } });
  f.Apply(
    actor,
    { { "op", "setObjectiveCompleted" }, { "index", 10 }, { "value", true } });
  REQUIRE(QuestJournal::Get(actor, kQuest)["objectives"]["10"]["failed"] ==
          false);
  REQUIRE(QuestJournal::Get(actor, kQuest)["objectives"]["10"]["completed"] ==
          true);
  f.Apply(actor, { { "op", "complete" } });
  REQUIRE(QuestJournal::Get(actor, kQuest)["completed"] == true);
  f.Apply(actor, { { "op", "stop" } });
  REQUIRE(QuestJournal::Get(actor, kQuest)["running"] == false);
  f.Apply(actor, { { "op", "reset" } });
  REQUIRE(QuestJournal::Get(actor, kQuest)["stages"].empty());
  REQUIRE(QuestJournal::Get(actor, kQuest)["completed"] == false);
}

TEST_CASE(
  "Invalid and stale quest writes never partially change saved progress",
  "[Quest]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  f.Apply(actor, { { "op", "setStage" }, { "index", 10 } });
  const auto saved = QuestJournal::Snapshot(actor);
  for (const Json& command :
       { Json{ { "op", "setStage" }, { "index", 65535 } },
         Json{ { "op", "setStage" }, { "index", -1 } },
         Json{ { "op", "setStage" }, { "index", 20.5 } },
         Json{ { "op", "setObjectiveCompleted" },
               { "index", 999 },
               { "value", true } },
         Json{ { "op", "setObjectiveDisplayed" },
               { "index", 10 },
               { "value", "true" } },
         Json{
           { "op", "setStage" }, { "index", 20 }, { "expectedRevision", 0 } },
         Json{ { "op", "unknown" } } }) {
    REQUIRE_THROWS(f.Apply(actor, command));
    REQUIRE(QuestJournal::Snapshot(actor) == saved);
  }
  REQUIRE_THROWS(
    QuestJournal::Apply(actor, { { "quest", 0xf }, { "op", "start" } }));
  actor.SetPropertyValueDump(QuestJournal::kEnabled, "[]", false, false);
  REQUIRE_THROWS_WITH(
    f.Apply(actor, { { "op", "setStage" }, { "index", 20 } }),
    "Quest execution adapter is not enabled for this campaign");
  REQUIRE(QuestJournal::Snapshot(actor) == saved);
}

TEST_CASE(
  "Quest journals survive change-form serialization without a client save",
  "[Quest]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  f.Apply(actor, { { "op", "setStage" }, { "index", 10 } });
  f.Apply(
    actor,
    { { "op", "setObjectiveDisplayed" }, { "index", 10 }, { "value", true } });
  const auto snapshot = QuestJournal::Snapshot(actor);
  const auto json = MpChangeForm::ToJson(actor.GetChangeForm()).dump();
  simdjson::dom::parser parser;
  auto parsed = parser.parse(json).value();
  const auto saved = MpChangeForm::JsonToChangeForm(parsed);
  REQUIRE(saved == actor.GetChangeForm());
  Fixture restored;
  auto& reloaded = restored.Add(actor.GetFormId());
  reloaded.ApplyChangeForm(saved);
  REQUIRE(QuestJournal::Snapshot(reloaded) == snapshot);
  auto& oldCharacter = restored.Add(0xff000002);
  REQUIRE(QuestJournal::Snapshot(oldCharacter)["quests"].empty());
  reloaded.SetPropertyValueDump(QuestJournal::kProperty,
                                "{\"schemaVersion\":999}", false, false);
  REQUIRE_THROWS(restored.Apply(reloaded, { { "op", "start" } }));
  REQUIRE(reloaded.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          "{\"schemaVersion\":999}");
}

TEST_CASE("Papyrus quest calls use their initiating campaign and reject "
          "expired stacks",
          "[Quest][Papyrus]")
{
  Fixture f;
  auto& owner = f.Add(0xff000001);
  auto& guest = f.Add(0xff000002);
  auto policy = std::make_shared<HeuristicPolicy>(&f.server.worldState);
  PapyrusQuest quest;
  quest.compatibilityPolicy = policy;
  EspmGameObject object(
    f.server.worldState.GetEspm().GetBrowser().LookupById(kQuest));
  auto self = VarValue::AttachTestStackId(VarValue(&object), 108);
  REQUIRE_THROWS(quest.GetStage(self, {}));
  const auto instance =
    "campaign:" + std::to_string(owner.GetFormId()) + ":fixture";
  owner.SetWorldInstance(instance);
  guest.SetWorldInstance(instance);
  policy->SetDefaultActor(108, &guest);
  REQUIRE(static_cast<bool>(quest.SetStage(self, { VarValue(10) })));
  REQUIRE(static_cast<int32_t>(quest.GetCurrentStageID(self, {})) == 10);
  REQUIRE(static_cast<bool>(quest.IsStageDone(self, { VarValue(10) })));
  REQUIRE_FALSE(static_cast<bool>(quest.IsStageDone(self, { VarValue(20) })));
  REQUIRE_THROWS(quest.SetStage(self, { VarValue("20") }));
  const auto saved = QuestJournal::Snapshot(owner);
  guest.SetWorldInstance("");
  REQUIRE_THROWS(quest.SetStage(self, { VarValue(20) }));
  guest.SetWorldInstance(
    instance); // ABA: same instance string still expires the old stack
  REQUIRE_THROWS(quest.SetStage(self, { VarValue(20) }));
  policy->SetDefaultActor(108, &guest);
  REQUIRE(static_cast<int32_t>(quest.GetStage(self, {})) == 10);
  owner.SetWorldInstance("");
  owner.SetWorldInstance(instance);
  REQUIRE_THROWS(quest.SetStage(self, { VarValue(20) }));
  REQUIRE(QuestJournal::Snapshot(owner)["quests"] == saved["quests"]);
  REQUIRE(QuestJournal::Snapshot(owner)["revision"] == saved["revision"]);
  policy->SetDefaultActor(108, &guest);
  f.server.worldState.DestroyForm(guest.GetFormId());
  f.Add(0xff000002).SetWorldInstance(instance);
  REQUIRE_THROWS(quest.SetStage(self, { VarValue(20) }));
}

TEST_CASE("Unowned instances cannot redirect a quest write", "[Quest]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  auto& owner = f.Add(0xff000002);
  actor.SetWorldInstance("arbitrary-instance");
  REQUIRE_THROWS(QuestJournal::Snapshot(actor));
  actor.SetWorldInstance("campaign:4278190082:fixture");
  REQUIRE_THROWS(f.Apply(actor, { { "op", "start" } }));
  REQUIRE(QuestJournal::Snapshot(owner)["quests"].empty());
}

TEST_CASE("Registered quest natives dispatch through the Papyrus VM",
          "[Quest][Papyrus]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  auto policy = std::make_shared<HeuristicPolicy>(&f.server.worldState);
  VirtualMachine vm(std::vector<std::shared_ptr<PexScript>>{});
  PapyrusQuest quest;
  quest.Register(vm, policy);
  auto stack = std::shared_ptr<StackData>(new StackData{ StackIdHolder(vm) });
  policy->SetDefaultActor(stack->stackIdHolder.GetStackId(), &actor);
  EspmGameObject self(
    f.server.worldState.GetEspm().GetBrowser().LookupById(kQuest));
  std::vector<VarValue> stageArgs{ VarValue(10) }, noArgs;
  REQUIRE(
    static_cast<bool>(vm.CallMethod(&self, "SetStage", stageArgs, stack)));
  REQUIRE(static_cast<int32_t>(
            vm.CallMethod(&self, "GetStage", noArgs, stack)) == 10);
  REQUIRE(
    static_cast<bool>(vm.CallMethod(&self, "IsStageDone", stageArgs, stack)));
  std::vector<VarValue> objectiveArgs{ VarValue(10), VarValue(true) };
  vm.CallMethod(&self, "SetObjectiveDisplayed", objectiveArgs, stack);
  REQUIRE(static_cast<bool>(
    vm.CallMethod(&self, "IsObjectiveDisplayed", stageArgs, stack)));
}

TEST_CASE("Quest conditions check the acting campaign instead of granting an "
          "unknown condition",
          "[Quest]")
{
  Fixture f;
  auto& owner = f.Add(0xff000001);
  auto& guest = f.Add(0xff000002);
  const auto map = ConditionFunctionFactory::CreateConditionFunctions();
  const auto stage = map.GetConditionFunction(uint16_t(58));
  const auto done = map.GetConditionFunction(uint16_t(59));
  REQUIRE(stage == map.GetConditionFunction("GetStage"));
  REQUIRE(done == map.GetConditionFunction("GetStageDone"));
  REQUIRE(map.GetConditionFunction(uint16_t(56)) ==
          map.GetConditionFunction("GetQuestRunning"));
  REQUIRE(map.GetConditionFunction(uint16_t(543)) ==
          map.GetConditionFunction("GetQuestCompleted"));
  REQUIRE(stage->Execute(guest, kQuest, 0, {}) == 0.f);
  f.Apply(owner, { { "op", "setStage" }, { "index", 20 } });
  const auto instance =
    "campaign:" + std::to_string(owner.GetFormId()) + ":fixture";
  owner.SetWorldInstance(instance);
  guest.SetWorldInstance(instance);
  REQUIRE(stage->Execute(guest, kQuest, 0, {}) == 20.f);
  REQUIRE(done->Execute(guest, kQuest, 10, {}) == 0.f);
  REQUIRE(done->Execute(guest, kQuest, 20, {}) == 1.f);
  REQUIRE(map.GetConditionFunction("GetQuestRunning")
            ->Execute(guest, kQuest, 0, {}) == 1.f);
  f.Apply(owner, { { "op", "complete" } });
  REQUIRE(map.GetConditionFunction("GetQuestCompleted")
            ->Execute(guest, kQuest, 0, {}) == 1.f);
  Condition gate;
  gate.function = "GetStageDone";
  gate.parameter1 = "0x4E50D";
  gate.parameter2 = "0x14";
  gate.runsOn = "Subject";
  gate.comparison = "==";
  gate.value = 1;
  gate.logicalOperator = "AND";
  bool permitted = false;
  const auto evaluate = [&] {
    ConditionsEvaluator::EvaluateConditions(
      map, {}, ConditionsEvaluatorCaller::kCraft, { gate }, guest, guest,
      [&](bool result, auto&) { permitted = result; });
  };
  evaluate();
  REQUIRE(permitted);
  guest.SetWorldInstance("");
  evaluate();
  REQUIRE_FALSE(permitted);
  REQUIRE(map.GetConditionFunction("GetQuestCompleted")
            ->Execute(guest, kQuest, 0, {}) == 0.f);
}
