#include "QuestJobRunner.h"
#include "QuestJournal.h"
#include "QuestScriptRuntime.h"
#include "QuestVmState.h"
#include "TestUtils.hpp"
#include "database_drivers/SqliteDatabase.h"
#include "save_storages/AsyncSaveStorage.h"
#include "save_storages/ISaveStorage.h"
#include <catch2/catch_all.hpp>
#include <chrono>
#include <exception>
#include <filesystem>
#include <random>
#include <thread>

extern espm::Loader& GetEspmLoader();

namespace {
using Json = nlohmann::json;
using Status = QuestJobRunner::Status;
using Forms = std::vector<std::optional<MpChangeForm>>;
using SaveStorage =
  Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>;
constexpr uint32_t kQuest = 0x3372b, kOwner = 0xff006100;
constexpr uint16_t kStage = 250;
constexpr auto kCommand = "mq101-stage250";
constexpr auto kState = "3372b:Skyrim.esm/jobfixture";

VarValue Name(const char* value)
{
  return VarValue(VarValue::kType_Identifier, value);
}

FunctionInfo Reward(int count)
{
  FunctionInfo function;
  function.valid = true;
  function.returnType = "None";
  function.locals = { { "target", "ObjectReference" },
                      { "gold", "Form" },
                      { "temp", "None" } };
  function.code.instructions = {
    { FunctionCode::kOp_CallStatic,
      { VarValue("Game"), VarValue("GetForm"), Name("target"), VarValue(1),
        VarValue(0x14) } },
    { FunctionCode::kOp_CallStatic,
      { VarValue("Game"), VarValue("GetForm"), Name("gold"), VarValue(1),
        VarValue(0xf) } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("AddItem"), Name("target"), Name("temp"), VarValue(3),
        Name("gold"), VarValue(count), VarValue(true) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  return function;
}

struct Program
{
  espm::QuestScriptData metadata;
  std::shared_ptr<PexScript> script = std::make_shared<PexScript>();
  std::vector<std::shared_ptr<PexScript>> bases;
  explicit Program(bool failFalseCompletion = false)
  {
    metadata.version = 5;
    metadata.objectFormat = 2;
    metadata.scripts.push_back({ "JobFixture", 0, {} });
    metadata.fragments = { { kStage, 0, 1, "JobFixture", "First" },
                           { kStage, 1, 1, "JobFixture", "Waiting" },
                           { kStage, 2, 1, "JobFixture", "Last" } };
    script->source = "JobFixture";
    script->userFlagTable.push_back({ "Conditional", 4 });
    Object object;
    object.NameIndex = script->source;
    object.parentClassName = "Quest";
    object.userFlags = 16;
    object.variables = { { "::FactionPath_var", "Int", 16, VarValue(0) },
                         { "shared", "Int[]", 0, VarValue::None() },
                         { "prefixes", "Int", 0, VarValue(0) },
                         { "laterRuns", "Int", 0, VarValue(0) } };
    auto first = Reward(5);
    first.code.instructions.insert(
      first.code.instructions.begin(),
      { FunctionCode::kOp_Assign,
        { Name("::FactionPath_var"), VarValue(1) } });
    auto waiting = Reward(2);
    waiting.locals.push_back({ "sharedLocal", "Int[]" });
    waiting.locals.push_back({ "completed", "Bool" });
    waiting.locals.push_back({ "count", "Int" });
    waiting.code.instructions.pop_back();
    const std::vector<FunctionCode::Instruction> waitInstructions = {
      // The owned condition for this log is now false. Resumption must start
      // from its saved stack, not reevaluate the already-selected log.
      { FunctionCode::kOp_Assign, { Name("::FactionPath_var"), VarValue(2) } },
      { FunctionCode::kOp_IAdd,
        { Name("prefixes"), Name("prefixes"), VarValue(1) } },
      { FunctionCode::kOp_ArrayCreate, { Name("shared"), VarValue(1) } },
      { FunctionCode::kOp_ArraySetElement,
        { Name("shared"), VarValue(0), VarValue(3) } },
      { FunctionCode::kOp_Assign, { Name("sharedLocal"), Name("shared") } },
      { FunctionCode::kOp_CallMethod,
        { VarValue("Middle"), Name("self"), Name("completed"), VarValue(1),
          Name("sharedLocal") } },
      { FunctionCode::kOp_ArrayGetElement,
        { Name("count"), Name("sharedLocal"), VarValue(0) } },
      { FunctionCode::kOp_CallMethod,
        { VarValue("AddItem"), Name("target"), Name("temp"), VarValue(3),
          Name("gold"), Name("count"), VarValue(true) } },
      { FunctionCode::kOp_Return, { VarValue::None() } }
    };
    waiting.code.instructions.insert(waiting.code.instructions.end(),
                                     waitInstructions.begin(),
                                     waitInstructions.end());
    FunctionInfo middle;
    middle.valid = true;
    middle.returnType = "Bool";
    middle.params = { { "link", "Int[]" } };
    middle.locals = { { "completed", "Bool" } };
    middle.code.instructions = {
      { FunctionCode::kOp_CallMethod,
        { VarValue("Inner"), Name("self"), Name("completed"), VarValue(1),
          Name("link") } },
      { FunctionCode::kOp_Return, { Name("completed") } }
    };
    auto inner = middle;
    inner.locals.push_back({ "temp", "None" });
    inner.code.instructions = {
      { FunctionCode::kOp_CallStatic,
        { VarValue("FixtureAwait"), VarValue("AwaitBool"), Name("completed"),
          VarValue(1), VarValue("first") } },
      { FunctionCode::kOp_ArraySetElement,
        { Name("link"), VarValue(0), VarValue(9) } },
      { FunctionCode::kOp_CallStatic,
        { VarValue("FixtureAwait"), VarValue("AwaitBool"), Name("completed"),
          VarValue(1), VarValue("second") } }
    };
    if (failFalseCompletion) {
      inner.locals.push_back({ "target", "ObjectReference" });
      inner.locals.push_back({ "gold", "Form" });
      inner.code.instructions.push_back(
        { FunctionCode::kOp_JumpT, { Name("completed"), VarValue(6) } });
      auto transientReward = Reward(4).code.instructions;
      transientReward.pop_back();
      inner.code.instructions.insert(inner.code.instructions.end(),
                                     transientReward.begin(),
                                     transientReward.end());
      inner.code.instructions.push_back(
        { FunctionCode::kOp_ArraySetElement,
          { Name("link"), VarValue(0), VarValue(77) } });
      inner.code.instructions.push_back(
        { FunctionCode::kOp_CallMethod,
          { VarValue("UnsupportedAfterAwait"), Name("self"), Name("temp"),
            VarValue(0) } });
    }
    inner.code.instructions.push_back(
      { FunctionCode::kOp_Return, { Name("completed") } });
    auto last = Reward(11);
    last.code.instructions.insert(
      last.code.instructions.begin(),
      { FunctionCode::kOp_IAdd,
        { Name("laterRuns"), Name("laterRuns"), VarValue(1) } });
    object.states.push_back({ "",
                              { { "First", first },
                                { "Waiting", waiting },
                                { "Middle", middle },
                                { "Inner", inner },
                                { "Last", last } } });
    script->objectTable.push_back(std::move(object));
    for (const auto& [name, parent] :
         std::vector<std::pair<std::string, std::string>>{
           { "Form", "" },
           { "Quest", "Form" },
           { "ObjectReference", "Form" },
           { "MiscObject", "Form" },
           { "Actor", "ObjectReference" },
           { "Alias", "" },
           { "ReferenceAlias", "Alias" } }) {
      auto base = std::make_shared<PexScript>();
      base->source = name;
      Object baseObject;
      baseObject.NameIndex = name;
      baseObject.parentClassName = parent;
      base->objectTable.push_back(std::move(baseObject));
      bases.push_back(std::move(base));
    }
  }
  std::vector<PexScript::Lazy> Scripts() const
  {
    std::vector<PexScript::Lazy> result{ { script->source, [script = script] {
                                            return script;
                                          } } };
    for (const auto& base : bases)
      result.push_back({ base->source, [base] { return base; } });
    return result;
  }
};

struct ControlledStorage : SaveStorage
{
  struct Submission
  {
    Forms forms;
    UpsertResultCallback complete;
  };
  bool automatic = true;
  uint32_t finished = 0;
  std::vector<Submission> submissions;
  bool SupportsAtomicBatches() const noexcept override { return true; }
  bool SupportsUpsertResults() const noexcept override { return true; }
  void IterateSync(const IterateSyncCallback&) override {}
  void Iterate(const IterateCallback& callback,
               const std::optional<std::vector<FormDesc>>&) override
  {
    callback({});
  }
  void Upsert(Forms&& forms, const UpsertCallback& callback) override
  {
    UpsertWithResult(std::move(forms), [callback](std::exception_ptr error) {
      if (error)
        std::rethrow_exception(error);
      callback();
    });
  }
  void UpsertWithResult(Forms&& forms,
                        const UpsertResultCallback& callback) override
  {
    submissions.push_back({ std::move(forms), callback });
    if (automatic)
      Complete(submissions.size() - 1);
  }
  void Complete(size_t index, std::exception_ptr error = {})
  {
    const auto callback = submissions.at(index).complete;
    callback(error);
    ++finished;
  }
  uint32_t GetNumFinishedUpserts() const override { return finished; }
  uint32_t GetNumFinishedIterates() const override { return 0; }
  void Tick() override {}
  bool GetRecycledChangeFormsBuffer(Forms&) override { return false; }
  const std::string& GetName() const override
  {
    static const std::string name = "quest-job-controlled";
    return name;
  }
};

void Initialize(PartOne& server)
{
  server.CreateActor(kOwner, { 1, 2, 3 }, 0, 0x3c);
  auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
  owner.SetPropertyValueDump(QuestJournal::kEnabled, "[\"3372b:Skyrim.esm\"]",
                             false, false);
  owner.SetWorldInstance("campaign:" + std::to_string(kOwner) + ":jobs");
  owner.SetInventory(Inventory().AddItem(0xf, 10));
}

struct Fixture
{
  Program program;
  PartOne server;
  std::shared_ptr<ControlledStorage> storage =
    std::make_shared<ControlledStorage>();
  explicit Fixture(bool failFalseCompletion = false)
    : program(failFalseCompletion)
  {
    server.AttachEspm(&GetEspmLoader());
    server.AttachSaveStorage(storage);
    Initialize(server);
    Tick();
    storage->submissions.clear();
    storage->automatic = false;
  }
  MpActor& Owner() { return server.worldState.GetFormAt<MpActor>(kOwner); }
  QuestJobRunner& Jobs() { return server.worldState.GetQuestJobs(); }
  void Tick() { server.worldState.Tick(); }
  QuestJobRunner::Result Submit(const std::string& command = kCommand,
                                uint16_t stage = kStage)
  {
    return Jobs().Submit(Owner(), command, kQuest, stage, program.metadata,
                         program.Scripts());
  }
  QuestJobRunner::Result Resume(const std::string& token, bool value = true)
  {
    return Jobs().Resume(Owner(), kCommand, token, value, program.metadata,
                         program.Scripts());
  }
  QuestJobRunner::Result Inspect()
  {
    return Jobs().Inspect(Owner(), kCommand);
  }
  QuestJobRunner::Result Ack(size_t index)
  {
    storage->Complete(index);
    Tick();
    return Inspect();
  }
  uint32_t Gold() { return Owner().GetInventory().GetItemCount(0xf); }
  int Field(const char* name)
  {
    std::string key = std::string("jobfixture/") + name;
    for (auto& character : key)
      if (character >= 'A' && character <= 'Z')
        character += 'a' - 'A';
    return QuestJournal::ScriptState(Owner())
      .at("scripts")
      .at(kState)
      .at("values")
      .at(key)
      .at("value")
      .get<int>();
  }
};

Json Serialized(const Forms& forms)
{
  auto result = Json::array();
  for (const auto& form : forms)
    result.push_back(form ? MpChangeForm::ToJson(*form) : Json(nullptr));
  return result;
}

void RequirePending(const QuestJobRunner::Result& result)
{
  REQUIRE(result.status == Status::Pending);
  REQUIRE(result.token.empty());
  REQUIRE(result.key.empty());
}
}

TEST_CASE("Quest jobs expose waits and results only after exact durable saves",
          "[Quest][QuestJobs][SaveReceipt]")
{
  Fixture f;
  const auto submitted = f.Submit();
  RequirePending(submitted);
  REQUIRE(submitted.job > 0);
  REQUIRE(f.Gold() == 17);
  REQUIRE(f.Field("prefixes") == 1);
  const auto firstJob = QuestJournal::JobState(f.Owner());
  REQUIRE(firstJob.active);
  REQUIRE(firstJob.active->cursor == "waiting");
  REQUIRE(firstJob.active->log == 1);
  REQUIRE(firstJob.active->events.size() == 2);
  const auto heap = QuestJournal::ScriptState(f.Owner());
  REQUIRE(heap.at("continuations").size() == 1);
  REQUIRE(
    heap.at("continuations").at(firstJob.active->token).at("callers").size() ==
    2);
  REQUIRE(heap.at("arrays").size() == 1);
  REQUIRE(f.Submit().job == submitted.job);
  REQUIRE(f.Gold() == 17);
  REQUIRE_THROWS(f.Resume(firstJob.active->token));
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  const auto firstBytes = Serialized(f.storage->submissions[0].forms);
  f.storage->Complete(0, std::make_exception_ptr(std::runtime_error("disk")));
  f.Tick();
  RequirePending(f.Inspect());
  REQUIRE(f.storage->submissions.size() == 2);
  REQUIRE(Serialized(f.storage->submissions[1].forms) == firstBytes);
  REQUIRE(f.Gold() == 17);
  f.storage->Complete(0);
  f.Tick();
  RequirePending(f.Inspect());
  f.storage->Complete(1);
  RequirePending(f.Inspect());
  f.Tick();
  const auto first = f.Inspect();
  REQUIRE(first.status == Status::Waiting);
  REQUIRE(first.key == "first");
  REQUIRE(first.token == firstJob.active->token);
  REQUIRE(first.accepted);

  RequirePending(f.Resume(first.token));
  REQUIRE(f.Gold() == 17);
  REQUIRE(f.Field("prefixes") == 1);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 3);
  f.storage->Complete(1);
  f.Tick();
  RequirePending(f.Inspect());
  const auto second = f.Ack(2);
  REQUIRE(second.status == Status::Waiting);
  REQUIRE(second.key == "second");
  REQUIRE(second.token != first.token);
  REQUIRE(f.Resume(first.token).token == second.token);
  REQUIRE(f.Gold() == 17);
  REQUIRE_THROWS(f.Resume(first.token, false));
  REQUIRE_THROWS(f.Resume("unrecognized-operation"));

  RequirePending(f.Resume(second.token));
  REQUIRE(f.Gold() == 37);
  REQUIRE(f.Field("prefixes") == 1);
  REQUIRE(f.Field("laterRuns") == 1);
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations").empty());
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 4);
  RequirePending(f.Submit());
  const auto finalBytes = Serialized(f.storage->submissions[3].forms);
  f.storage->Complete(
    3, std::make_exception_ptr(std::runtime_error("final commit failed")));
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 5);
  REQUIRE(Serialized(f.storage->submissions[4].forms) == finalBytes);
  RequirePending(f.Inspect());
  REQUIRE(f.Gold() == 37);
  const auto complete = f.Ack(4);
  REQUIRE(complete.status == Status::Completed);
  REQUIRE(complete.job == first.job);
  REQUIRE(complete.accepted);
  REQUIRE(complete.token.empty());
  REQUIRE(complete.key.empty());
  REQUIRE(f.Submit().status == Status::Completed);
  REQUIRE(f.Resume(second.token).status == Status::Completed);
  REQUIRE(f.Resume(first.token).status == Status::Completed);
  REQUIRE_THROWS(f.Resume(second.token, false));
  REQUIRE(f.Gold() == 37);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 5);
}

TEST_CASE(
  "An active quest job refuses competing mutations and changed identity",
  "[Quest][QuestJobs]")
{
  Fixture f;
  f.Submit();
  f.Tick();
  const auto waiting = f.Ack(0);
  REQUIRE(waiting.status == Status::Waiting);
  const auto before = QuestJournal::Snapshot(f.Owner());
  REQUIRE_THROWS(f.Submit("competing-command"));
  REQUIRE_THROWS(f.Submit(kCommand, 255));
  REQUIRE_THROWS(f.Jobs().Submit(f.Owner(), kCommand, 0x4e50d, kStage,
                                 f.program.metadata, f.program.Scripts()));
  REQUIRE_THROWS(QuestJournal::Apply(
    f.Owner(), { { "quest", kQuest }, { "op", "reset" } }));
  REQUIRE_THROWS(QuestScriptRuntime::ExecuteStage(
    f.Owner(), kQuest, 255, f.program.metadata, f.program.Scripts()));
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
  Program changed;
  changed.script->objectTable[0]
    .states[0]
    .functions[0]
    .function.code.instructions[0]
    .args[1] = VarValue(2);
  REQUIRE_THROWS(f.Jobs().Resume(f.Owner(), kCommand, waiting.token, true,
                                 changed.metadata, changed.Scripts()));
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
  REQUIRE(f.Gold() == 17);
  const auto instance = f.Owner().GetWorldInstance();
  f.Owner().SetWorldInstance("");
  REQUIRE_THROWS(f.Submit());
  REQUIRE_THROWS(f.Resume(waiting.token));
  f.Owner().SetWorldInstance(instance);
  REQUIRE(f.Inspect().status == Status::Waiting);
  REQUIRE(f.Gold() == 17);
}

TEST_CASE("A failed resumed segment preserves its durable stack and rewards",
          "[Quest][QuestJobs][Rollback]")
{
  Fixture f(true);
  f.Submit();
  f.Tick();
  const auto first = f.Ack(0);
  REQUIRE(first.status == Status::Waiting);
  f.Resume(first.token);
  f.Tick();
  const auto second = f.Ack(1);
  REQUIRE(second.status == Status::Waiting);
  const auto before = QuestJournal::Snapshot(f.Owner());
  const auto heapBefore = QuestJournal::ScriptState(f.Owner());
  const auto jobsBefore = QuestJournal::JobState(f.Owner()).Save();
  REQUIRE_THROWS(f.Resume(second.token, false));
  REQUIRE(f.Gold() == 17);
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == heapBefore);
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == jobsBefore);
  REQUIRE(f.Inspect().status == Status::Waiting);
  REQUIRE(f.Inspect().token == second.token);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 2);
  RequirePending(f.Resume(second.token, true));
  REQUIRE(f.Gold() == 37);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 3);
  REQUIRE(f.Ack(2).status == Status::Completed);
  REQUIRE(f.Gold() == 37);
}

TEST_CASE("A quest job without awaits durably completes once",
          "[Quest][QuestJobs][SaveReceipt]")
{
  Fixture f;
  constexpr uint32_t quest = 0x2d512; // DA04 stage 5 has an owned log item.
  constexpr auto command = "da04-stage5";
  f.Owner().SetPropertyValueDump(QuestJournal::kEnabled,
                                 "[\"2d512:Skyrim.esm\"]", false, false);
  f.program.metadata.fragments = { { 5, 0, 1, "JobFixture", "Immediate" } };
  f.program.script->objectTable[0].states[0].functions.push_back(
    { "Immediate", Reward(4) });
  const auto submit = [&] {
    return f.Jobs().Submit(f.Owner(), command, quest, 5, f.program.metadata,
                           f.program.Scripts());
  };
  const auto submitted = submit();
  RequirePending(submitted);
  REQUIRE(f.Gold() == 14);
  REQUIRE_FALSE(QuestJournal::JobState(f.Owner()).active);
  REQUIRE(QuestJournal::JobState(f.Owner()).completed.size() == 1);
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations").empty());
  REQUIRE(submit().job == submitted.job);
  RequirePending(f.Jobs().Inspect(f.Owner(), command));
  REQUIRE(f.Gold() == 14);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  f.storage->Complete(0);
  RequirePending(f.Jobs().Inspect(f.Owner(), command));
  f.Tick();
  const auto completed = f.Jobs().Inspect(f.Owner(), command);
  REQUIRE(completed.status == Status::Completed);
  REQUIRE(completed.accepted);
  REQUIRE(completed.job == submitted.job);
  REQUIRE(completed.token.empty());
  REQUIRE(completed.key.empty());
  const auto before = QuestJournal::Snapshot(f.Owner());
  REQUIRE(submit().status == Status::Completed);
  REQUIRE(f.Gold() == 14);
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);

  // A newer job's pending receipt must not hide a previously durable result.
  const auto later = f.Jobs().Submit(f.Owner(), "da04-stage5-again", quest, 5,
                                     f.program.metadata, f.program.Scripts());
  RequirePending(later);
  REQUIRE(later.job > completed.job);
  REQUIRE(f.Gold() == 14);
  REQUIRE(f.Jobs().Inspect(f.Owner(), command).status == Status::Completed);
  REQUIRE(submit().status == Status::Completed);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 2);
  REQUIRE(f.Jobs().Inspect(f.Owner(), command).status == Status::Completed);
  f.storage->Complete(1);
  f.Tick();
  const auto laterCompleted = f.Jobs().Inspect(f.Owner(), "da04-stage5-again");
  REQUIRE(laterCompleted.status == Status::Completed);
  REQUIRE_FALSE(laterCompleted.accepted);
  REQUIRE(f.Gold() == 14);
}

TEST_CASE("Inventory event stage calls do not reuse the outer job log cursor",
          "[Quest][QuestJobs][CampaignInventory]")
{
  Fixture f;
  constexpr uint32_t quest = 0x4e50d;
  constexpr auto command = "inventory-event-stage";
  f.Owner().SetPropertyValueDump(QuestJournal::kEnabled,
                                 "[\"4e50d:Skyrim.esm\"]", false, false);
  f.program.metadata.fragments = { { 10, 0, 1, "JobFixture", "Outer" },
                                   { 20, 0, 1, "JobFixture", "FromEvent" } };
  auto& object = f.program.script->objectTable[0];
  object.variables.push_back({ "nestedRuns", "Int", 0, VarValue(0) });
  auto nested = Reward(4);
  nested.code.instructions.insert(
    nested.code.instructions.begin(),
    { FunctionCode::kOp_IAdd,
      { Name("nestedRuns"), Name("nestedRuns"), VarValue(1) } });
  object.states[0].functions.push_back({ "Outer", Reward(5) });
  object.states[0].functions.push_back({ "FromEvent", nested });

  espm::QuestScriptData::Alias alias;
  alias.object.type = 1;
  alias.object.formId = quest;
  alias.object.alias = 3;
  alias.version = 5;
  alias.objectFormat = 2;
  alias.scripts.push_back({ "InventoryEventFixture", 0, {} });
  f.program.metadata.aliases.push_back(alias);
  auto script = std::make_shared<PexScript>();
  script->source = "InventoryEventFixture";
  Object aliasObject;
  aliasObject.NameIndex = script->source;
  aliasObject.parentClassName = "ReferenceAlias";
  aliasObject.variables.push_back({ "calls", "Int", 0, VarValue(0) });
  FunctionInfo handler;
  handler.valid = true;
  handler.returnType = "None";
  handler.params = { { "item", "Form" },
                     { "count", "Int" },
                     { "reference", "ObjectReference" },
                     { "source", "ObjectReference" } };
  handler.locals = { { "target", "Quest" },
                     { "matches", "Bool" },
                     { "accepted", "Bool" } };
  handler.code.instructions = {
    { FunctionCode::kOp_CompareEQ,
      { Name("matches"), Name("count"), VarValue(5) } },
    { FunctionCode::kOp_JumpF, { Name("matches"), VarValue(4) } },
    { FunctionCode::kOp_CallStatic,
      { VarValue("Game"), VarValue("GetForm"), Name("target"), VarValue(1),
        VarValue(static_cast<int32_t>(quest)) } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("SetStage"), Name("target"), Name("accepted"), VarValue(1),
        VarValue(20) } },
    { FunctionCode::kOp_IAdd, { Name("calls"), Name("calls"), VarValue(1) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  aliasObject.states.push_back({ "", { { "OnItemAdded", handler } } });
  script->objectTable.push_back(std::move(aliasObject));
  f.program.bases.push_back(script);
  QuestJournal::Atomic(f.Owner(), [&] {
    QuestVmState state(QuestJournal::ScriptState(f.Owner()), {}, {});
    state.SetAliasBinding("4e50d:Skyrim.esm/alias:3", "14:Skyrim.esm");
    QuestJournal::SetScriptState(f.Owner(), state.Save());
  });
  const auto submit = [&] {
    return f.Jobs().Submit(f.Owner(), command, quest, 10, f.program.metadata,
                           f.program.Scripts());
  };
  RequirePending(submit());
  REQUIRE(f.Gold() == 19);
  const auto heap = QuestJournal::ScriptState(f.Owner());
  REQUIRE(heap.at("scripts")
            .at("4e50d:Skyrim.esm/jobfixture")
            .at("values")
            .at("jobfixture/nestedruns")
            .at("value") == 1);
  REQUIRE(heap.at("scripts")
            .at("4e50d:Skyrim.esm/alias:3/inventoryeventfixture")
            .at("values")
            .at("inventoryeventfixture/calls")
            .at("value") == 1);
  REQUIRE(QuestJournal::Get(f.Owner(), quest).at("stage") == 20);
  REQUIRE(QuestJournal::Get(f.Owner(), quest).at("stages") ==
          Json::array({ 10, 20 }));
  const auto jobs = QuestJournal::JobState(f.Owner());
  REQUIRE_FALSE(jobs.active);
  REQUIRE(jobs.completed.size() == 1);
  REQUIRE(jobs.completed[0].stage == 10);
  REQUIRE(jobs.completed[0].log == 0);
  REQUIRE(jobs.completed[0].cursor == "done");
  REQUIRE(jobs.completed[0].events.empty());
  REQUIRE(jobs.completed[0].completions.empty());
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  f.storage->Complete(0);
  f.Tick();
  REQUIRE(f.Jobs().Inspect(f.Owner(), command).status == Status::Completed);
  const auto before = QuestJournal::Snapshot(f.Owner());
  REQUIRE(submit().status == Status::Completed);
  REQUIRE(f.Gold() == 19);
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
}

TEST_CASE("A zero-log quest job saves visitation with a false accepted result",
          "[Quest][QuestJobs][QuestLifecycle]")
{
  Fixture f;
  constexpr uint32_t quest = 0x1075b1;
  constexpr auto command = "zero-log-stage0";
  f.Owner().SetPropertyValueDump(QuestJournal::kEnabled,
                                 "[\"1075b1:Skyrim.esm\"]", false, false);
  f.program.metadata.fragments.clear();
  const auto beforeRevision = QuestJournal::Snapshot(f.Owner()).at("revision");
  const auto submit = [&] {
    return f.Jobs().Submit(f.Owner(), command, quest, 0, f.program.metadata,
                           f.program.Scripts());
  };
  RequirePending(submit());
  REQUIRE(f.Gold() == 10);
  REQUIRE(QuestJournal::Get(f.Owner(), quest).at("stages") ==
          Json::array({ 0 }));
  REQUIRE(QuestJournal::Snapshot(f.Owner()).at("revision") > beforeRevision);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  f.storage->Complete(0);
  f.Tick();
  const auto completed = f.Jobs().Inspect(f.Owner(), command);
  REQUIRE(completed.status == Status::Completed);
  REQUIRE_FALSE(completed.accepted);
  REQUIRE(completed.token.empty());
  REQUIRE(completed.key.empty());
  const auto before = QuestJournal::Snapshot(f.Owner());
  REQUIRE_FALSE(submit().accepted);
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
  REQUIRE(f.Gold() == 10);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
}

TEST_CASE(
  "Quest jobs require durable storage before executing their first reward",
  "[Quest][QuestJobs][SaveReceipt]")
{
  Fixture f;
  f.server.worldState.AttachSaveStorage(nullptr);
  const auto before = QuestJournal::Snapshot(f.Owner());
  const auto jobsBefore = QuestJournal::JobState(f.Owner()).Save();
  REQUIRE_THROWS(f.Submit());
  REQUIRE(f.Gold() == 10);
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
  REQUIRE(QuestJournal::ScriptState(f.Owner()).empty());
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == jobsBefore);
  f.Tick();
  REQUIRE(f.storage->submissions.empty());
  f.server.worldState.AttachSaveStorage(f.storage);
  RequirePending(f.Submit());
  REQUIRE(f.Gold() == 17);
}

namespace {
struct SqliteFiles
{
  std::filesystem::path directory;
  SqliteFiles()
  {
    std::random_device random;
    for (int attempt = 0; attempt < 20; ++attempt) {
      const auto candidate = std::filesystem::temp_directory_path() /
        ("skymp-job-unit-" + std::to_string(random()) + "-" +
         std::to_string(random()));
      if (std::filesystem::create_directory(candidate)) {
        directory = candidate;
        return;
      }
    }
    throw std::runtime_error("Cannot create isolated quest job directory");
  }
  ~SqliteFiles()
  {
    // This path belongs exclusively to this fixture; creation never accepts
    // an already-existing directory.
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
  std::string File() const { return (directory / "jobs.sqlite3").string(); }
};

void AttachDatabase(PartOne& server, const SqliteFiles& files)
{
  using AsyncStorage =
    Viet::AsyncSaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>;
  server.AttachEspm(&GetEspmLoader());
  server.AttachSaveStorage(std::make_shared<AsyncStorage>(
    std::make_shared<SqliteDatabase>(files.File()), spdlog::default_logger(),
    "quest-job-restart", 1));
}

QuestJobRunner::Result WaitForDurable(PartOne& server, MpActor& owner,
                                      const std::string& command = kCommand)
{
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    server.worldState.Tick();
    auto result = server.worldState.GetQuestJobs().Inspect(owner, command);
    if (result.status != Status::Pending)
      return result;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  throw std::runtime_error("Timed out waiting for durable quest job");
}
}

TEST_CASE(
  "Nested quest jobs survive SQLite restart at every wait and completion",
  "[Quest][QuestJobs][SQLite]")
{
  SqliteFiles files;
  Program program;
  std::string firstToken, secondToken;
  uint64_t jobId = 0;
  {
    PartOne server;
    AttachDatabase(server, files);
    Initialize(server);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = server.worldState.GetQuestJobs();
    RequirePending(jobs.Submit(owner, kCommand, kQuest, kStage,
                               program.metadata, program.Scripts()));
    const auto first = WaitForDurable(server, owner);
    REQUIRE(first.status == Status::Waiting);
    REQUIRE(first.key == "first");
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 17);
    firstToken = first.token;
    jobId = first.job;
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = server.worldState.GetQuestJobs();
    const auto first = jobs.Inspect(owner, kCommand);
    REQUIRE(first.status == Status::Waiting);
    REQUIRE(first.token == firstToken);
    REQUIRE(first.job == jobId);
    REQUIRE(jobs
              .Submit(owner, kCommand, kQuest, kStage, program.metadata,
                      program.Scripts())
              .token == firstToken);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 17);
    RequirePending(jobs.Resume(owner, kCommand, firstToken, true,
                               program.metadata, program.Scripts()));
    const auto second = WaitForDurable(server, owner);
    REQUIRE(second.status == Status::Waiting);
    REQUIRE(second.key == "second");
    REQUIRE(second.token != firstToken);
    REQUIRE(second.job == jobId);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 17);
    secondToken = second.token;
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = server.worldState.GetQuestJobs();
    const auto second = jobs.Inspect(owner, kCommand);
    REQUIRE(second.status == Status::Waiting);
    REQUIRE(second.token == secondToken);
    REQUIRE(jobs
              .Resume(owner, kCommand, firstToken, true, program.metadata,
                      program.Scripts())
              .token == secondToken);
    REQUIRE_THROWS(jobs.Resume(owner, kCommand, firstToken, false,
                               program.metadata, program.Scripts()));
    RequirePending(jobs.Resume(owner, kCommand, secondToken, true,
                               program.metadata, program.Scripts()));
    const auto complete = WaitForDurable(server, owner);
    REQUIRE(complete.status == Status::Completed);
    REQUIRE(complete.job == jobId);
    REQUIRE(complete.accepted);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 37);
    REQUIRE(QuestJournal::ScriptState(owner).at("continuations").empty());
    REQUIRE_FALSE(QuestJournal::JobState(owner).active);
    REQUIRE(QuestJournal::JobState(owner).completed.size() == 1);
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = server.worldState.GetQuestJobs();
    const auto before = QuestJournal::Snapshot(owner);
    REQUIRE(jobs.Inspect(owner, kCommand).status == Status::Completed);
    REQUIRE(jobs
              .Submit(owner, kCommand, kQuest, kStage, program.metadata,
                      program.Scripts())
              .status == Status::Completed);
    for (const auto& token : { firstToken, secondToken })
      REQUIRE(jobs
                .Resume(owner, kCommand, token, true, program.metadata,
                        program.Scripts())
                .status == Status::Completed);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 37);
    REQUIRE(QuestJournal::Snapshot(owner) == before);
  }
}

namespace {
constexpr uint32_t kParentQuest = 0x4e50d;
constexpr auto kChildCommand = "queued-child-stage";

struct ChildProgram
{
  Program program;
  QuestJobRunner::MetadataOverrides metadata;
  uint32_t childQuest;
  uint16_t childStage;
  ChildProgram(bool waits = true, bool twice = false, bool repeated = false,
               bool failChild = false, bool failParent = false)
    : childQuest(repeated ? 0x2aa57 : 0x2d512)
    , childStage(repeated ? 10 : 5)
  {
    auto& object = program.script->objectTable[0];
    object.variables = { { "shared", "Int[]", 0, VarValue::None() },
                         { "runs", "Int", 0, VarValue(0) },
                         { "firstResult", "Bool", 0, VarValue(false) },
                         { "secondResult", "Bool", 0, VarValue(false) } };
    Object::PropInfo property;
    property.name = "Shared";
    property.type = "Int[]";
    property.flags = 7;
    property.autoVarName = "shared";
    object.properties = { property };
    auto parent = Reward(2);
    parent.locals.push_back({ "childQuest", "Quest" });
    parent.locals.push_back({ "localShared", "Int[]" });
    parent.locals.push_back({ "suffix", "Int" });
    parent.code.instructions.pop_back();
    const std::vector<FunctionCode::Instruction> prefix = {
      { FunctionCode::kOp_IAdd, { Name("runs"), Name("runs"), VarValue(1) } },
      { FunctionCode::kOp_ArrayCreate, { Name("shared"), VarValue(1) } },
      { FunctionCode::kOp_ArraySetElement,
        { Name("shared"), VarValue(0), VarValue(3) } },
      { FunctionCode::kOp_Assign, { Name("localShared"), Name("shared") } },
      { FunctionCode::kOp_CallStatic,
        { VarValue("Game"), VarValue("GetForm"), Name("childQuest"),
          VarValue(1), VarValue(static_cast<int32_t>(childQuest)) } },
      { FunctionCode::kOp_PropSet,
        { VarValue("Shared"), Name("childQuest"), Name("shared") } },
      { FunctionCode::kOp_CallMethod,
        { VarValue("SetStage"), Name("childQuest"), Name("firstResult"),
          VarValue(1), VarValue(static_cast<int32_t>(childStage)) } }
    };
    parent.code.instructions.insert(parent.code.instructions.end(),
                                    prefix.begin(), prefix.end());
    if (twice)
      parent.code.instructions.push_back(
        { FunctionCode::kOp_CallMethod,
          { VarValue("SetCurrentStageID"), Name("childQuest"),
            Name("secondResult"), VarValue(1),
            VarValue(static_cast<int32_t>(childStage)) } });
    parent.code.instructions.push_back(
      { FunctionCode::kOp_ArrayGetElement,
        { Name("suffix"), Name("localShared"), VarValue(0) } });
    parent.code.instructions.push_back(
      { FunctionCode::kOp_CallMethod,
        { VarValue("AddItem"), Name("target"), Name("temp"), VarValue(3),
          Name("gold"), Name("suffix"), VarValue(true) } });
    if (failParent)
      parent.code.instructions.push_back(
        { FunctionCode::kOp_CallMethod,
          { VarValue("UnsupportedParentSuffix"), Name("self"), Name("temp"),
            VarValue(0) } });
    parent.code.instructions.push_back(
      { FunctionCode::kOp_Return, { VarValue::None() } });

    auto child = Reward(3);
    child.locals.push_back({ "completed", "Bool" });
    child.code.instructions.pop_back();
    child.code.instructions.push_back(
      { FunctionCode::kOp_IAdd, { Name("runs"), Name("runs"), VarValue(1) } });
    if (waits) {
      child.code.instructions.push_back(
        { FunctionCode::kOp_ArraySetElement,
          { Name("shared"), VarValue(0), VarValue(7) } });
      child.code.instructions.push_back(
        { FunctionCode::kOp_CallStatic,
          { VarValue("FixtureAwait"), VarValue("AwaitBool"), Name("completed"),
            VarValue(1), VarValue("child") } });
      child.code.instructions.push_back(
        { FunctionCode::kOp_CallMethod,
          { VarValue("AddItem"), Name("target"), Name("temp"), VarValue(3),
            Name("gold"), VarValue(4), VarValue(true) } });
    }
    child.code.instructions.push_back(
      { FunctionCode::kOp_ArraySetElement,
        { Name("shared"), VarValue(0), VarValue(9) } });
    if (failChild) {
      child.code.instructions.push_back(
        { FunctionCode::kOp_JumpT, { Name("completed"), VarValue(2) } });
      child.code.instructions.push_back(
        { FunctionCode::kOp_CallMethod,
          { VarValue("UnsupportedChildSuffix"), Name("self"), Name("temp"),
            VarValue(0) } });
    }
    child.code.instructions.push_back(
      { FunctionCode::kOp_Return, { VarValue::None() } });
    object.states = { { "", { { "Parent", parent }, { "Child", child } } } };
    program.metadata.fragments = { { 10, 0, 1, "JobFixture", "Parent" } };
    metadata[kParentQuest] = program.metadata;
    auto childMetadata = program.metadata;
    childMetadata.fragments = { { childStage, 0, 1, "JobFixture", "Child" } };
    metadata[childQuest] = childMetadata;
  }
  std::vector<PexScript::Lazy> Scripts() const { return program.Scripts(); }
  void Enable(MpActor& actor) const
  {
    const auto enabled = Json::array(
      { "4e50d:Skyrim.esm",
        childQuest == 0x2aa57 ? "2aa57:Skyrim.esm" : "2d512:Skyrim.esm" });
    actor.SetPropertyValueDump(QuestJournal::kEnabled, enabled.dump(), false,
                               false);
  }
};

Json ChildField(MpActor& actor, uint32_t quest, const std::string& field)
{
  const auto key =
    FormDesc::FromFormId(quest, actor.GetParent()->espmFiles).ToString() +
    "/jobfixture";
  return QuestJournal::ScriptState(actor)
    .at("scripts")
    .at(key)
    .at("values")
    .at("jobfixture/" + field);
}

struct ChildFixture : Fixture
{
  ChildProgram content;
  ChildFixture(bool waits = true, bool twice = false, bool repeated = false,
               bool failChild = false, bool failParent = false)
    : content(waits, twice, repeated, failChild, failParent)
  {
    content.Enable(Owner());
  }
  QuestJobRunner::Result SubmitChild()
  {
    return Jobs().Submit(Owner(), kChildCommand, kParentQuest, 10,
                         content.metadata, content.Scripts());
  }
  QuestJobRunner::Result Advance()
  {
    return Jobs().Advance(Owner(), kChildCommand, content.metadata,
                          content.Scripts());
  }
  QuestJobRunner::Result ResumeChild(const std::string& token,
                                     bool value = true)
  {
    return Jobs().Resume(Owner(), kChildCommand, token, value,
                         content.metadata, content.Scripts());
  }
  QuestJobRunner::Result InspectChild()
  {
    return Jobs().Inspect(Owner(), kChildCommand);
  }
  QuestJobRunner::Result AckChild(size_t index)
  {
    storage->Complete(index);
    Tick();
    return InspectChild();
  }
};

void RequireReady(const QuestJobRunner::Result& result)
{
  REQUIRE(result.status == Status::Ready);
  REQUIRE(result.token.empty());
  REQUIRE(result.key.empty());
}

void ObserveInventoryOrder(ChildFixture& fixture)
{
  espm::QuestScriptData::Alias alias;
  alias.object.type = 1;
  alias.object.formId = kParentQuest;
  alias.object.alias = 3;
  alias.version = 5;
  alias.objectFormat = 2;
  alias.scripts.push_back({ "InventoryOrder", 0, {} });
  fixture.content.metadata.at(kParentQuest).aliases.push_back(alias);
  auto script = std::make_shared<PexScript>();
  script->source = "InventoryOrder";
  Object object;
  object.NameIndex = script->source;
  object.parentClassName = "ReferenceAlias";
  object.variables = { { "order", "Int", 0, VarValue(0) } };
  FunctionInfo event;
  event.valid = true;
  event.returnType = "None";
  event.params = { { "item", "Form" },
                   { "count", "Int" },
                   { "reference", "ObjectReference" },
                   { "source", "ObjectReference" } };
  event.code.instructions = {
    { FunctionCode::kOp_IMultiply,
      { Name("order"), Name("order"), VarValue(10) } },
    { FunctionCode::kOp_IAdd,
      { Name("order"), Name("order"), Name("count") } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  object.states.push_back({ "", { { "OnItemAdded", event } } });
  script->objectTable.push_back(std::move(object));
  fixture.content.program.bases.push_back(script);
  QuestJournal::Atomic(fixture.Owner(), [&] {
    QuestVmState state(QuestJournal::ScriptState(fixture.Owner()), {}, {});
    state.SetAliasBinding("4e50d:Skyrim.esm/alias:3", "14:Skyrim.esm");
    QuestJournal::SetScriptState(fixture.Owner(), state.Save());
  });
}

int InventoryOrder(MpActor& actor)
{
  const auto scripts = QuestJournal::ScriptState(actor).at("scripts");
  const auto found = scripts.find("4e50d:Skyrim.esm/alias:3/inventoryorder");
  if (found == scripts.end())
    return 0;
  return found->at("values").at("inventoryorder/order").at("value").get<int>();
}
}

TEST_CASE("Child stages commit their intent and result before parent progress",
          "[Quest][QuestJobs][ChildStages]")
{
  ChildFixture f;
  ObserveInventoryOrder(f);
  RequirePending(f.SubmitChild());
  REQUIRE(f.Gold() == 12);
  REQUIRE(InventoryOrder(f.Owner()) == 0);
  REQUIRE(
    QuestJournal::Get(f.Owner(), f.content.childQuest).at("stages").empty());
  const auto parentToken = QuestJournal::JobState(f.Owner()).active->token;
  REQUIRE_FALSE(parentToken.empty());
  RequirePending(f.Advance());
  REQUIRE_THROWS(f.ResumeChild(parentToken));
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  const auto intent = Serialized(f.storage->submissions[0].forms);
  f.storage->Complete(0,
                      std::make_exception_ptr(std::runtime_error("intent")));
  f.Tick();
  RequirePending(f.InspectChild());
  REQUIRE(f.storage->submissions.size() == 2);
  REQUIRE(Serialized(f.storage->submissions[1].forms) == intent);
  f.storage->Complete(0);
  f.Tick();
  RequirePending(f.InspectChild());
  RequireReady(f.AckChild(1));
  REQUIRE_THROWS(f.ResumeChild(parentToken, true));
  REQUIRE_THROWS(f.ResumeChild(parentToken, false));

  RequirePending(f.Advance());
  REQUIRE(f.Gold() == 15);
  RequirePending(f.Advance());
  REQUIRE(InventoryOrder(f.Owner()) == 0);
  f.Tick();
  const auto waiting = f.AckChild(2);
  REQUIRE(waiting.status == Status::Waiting);
  REQUIRE(waiting.key == "child");
  REQUIRE(waiting.token != parentToken);
  REQUIRE(f.Advance().status == Status::Waiting);
  REQUIRE_THROWS(f.ResumeChild(parentToken));
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations").size() ==
          2);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "shared") ==
          ChildField(f.Owner(), f.content.childQuest, "shared"));
  RequirePending(f.ResumeChild(waiting.token));
  REQUIRE(f.Gold() == 19);
  RequirePending(f.Advance());
  REQUIRE(InventoryOrder(f.Owner()) == 0);
  f.Tick();
  const auto childDone = Serialized(f.storage->submissions[3].forms);
  f.storage->Complete(
    3, std::make_exception_ptr(std::runtime_error("child done")));
  f.Tick();
  RequirePending(f.InspectChild());
  REQUIRE(Serialized(f.storage->submissions[4].forms) == childDone);
  REQUIRE(f.Gold() == 19);
  RequireReady(f.AckChild(4));
  REQUIRE(InventoryOrder(f.Owner()) == 0);
  RequireReady(f.ResumeChild(waiting.token));
  REQUIRE_THROWS(f.ResumeChild(waiting.token, false));
  RequirePending(f.Advance());
  REQUIRE(f.Gold() == 28);
  REQUIRE(InventoryOrder(f.Owner()) == 2349);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "runs").at("value") == 1);
  REQUIRE(ChildField(f.Owner(), f.content.childQuest, "runs").at("value") ==
          1);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "firstresult").at("value") ==
          true);
  f.Tick();
  REQUIRE(f.AckChild(5).status == Status::Completed);
  REQUIRE(f.SubmitChild().status == Status::Completed);
  REQUIRE(f.ResumeChild(waiting.token).status == Status::Completed);
  REQUIRE(f.Advance().status == Status::Completed);
  REQUIRE_THROWS(f.ResumeChild(parentToken, false));
  REQUIRE(f.Gold() == 28);
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations").empty());
  REQUIRE(QuestJournal::JobState(f.Owner()).completed.back().children.empty());
}

TEST_CASE("Repeated and already-visited child stages return their actual Bool",
          "[Quest][QuestJobs][ChildStages]")
{
  const bool repeated = GENERATE(false, true);
  ChildFixture f(false, true, repeated);
  RequirePending(f.SubmitChild());
  f.Tick();
  RequireReady(f.AckChild(0));
  for (size_t segment = 1; segment <= 4; ++segment) {
    RequirePending(f.Advance());
    f.Tick();
    const auto result = f.AckChild(segment);
    if (segment == 4)
      REQUIRE(result.status == Status::Completed);
    else
      RequireReady(result);
  }
  REQUIRE(f.Gold() == (repeated ? 27 : 24));
  REQUIRE(ChildField(f.Owner(), kParentQuest, "firstresult").at("value") ==
          true);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "secondresult").at("value") ==
          repeated);
  REQUIRE(ChildField(f.Owner(), f.content.childQuest, "runs").at("value") ==
          (repeated ? 2 : 1));
  REQUIRE(QuestJournal::Get(f.Owner(), f.content.childQuest).at("stages") ==
          Json::array({ f.content.childStage }));
  REQUIRE(f.SubmitChild().status == Status::Completed);
  REQUIRE(f.Gold() == (repeated ? 27 : 24));
}

TEST_CASE(
  "Failed child and parent segments preserve the last durable boundary",
  "[Quest][QuestJobs][ChildStages][Rollback]")
{
  const bool parentFailure = GENERATE(false, true);
  ChildFixture f(true, false, false, !parentFailure, parentFailure);
  f.SubmitChild();
  f.Tick();
  RequireReady(f.AckChild(0));
  f.Advance();
  f.Tick();
  const auto waiting = f.AckChild(1);
  REQUIRE(waiting.status == Status::Waiting);
  if (!parentFailure) {
    const auto before = QuestJournal::Snapshot(f.Owner());
    const auto heap = QuestJournal::ScriptState(f.Owner());
    REQUIRE_THROWS(f.ResumeChild(waiting.token, false));
    REQUIRE(f.Gold() == 15);
    REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
    REQUIRE(QuestJournal::ScriptState(f.Owner()) == heap);
    REQUIRE(f.InspectChild().token == waiting.token);
    f.Tick();
    REQUIRE(f.storage->submissions.size() == 2);
  }
  f.ResumeChild(waiting.token, true);
  f.Tick();
  RequireReady(f.AckChild(2));
  REQUIRE(f.Gold() == 19);
  if (parentFailure) {
    const auto before = QuestJournal::Snapshot(f.Owner());
    const auto heap = QuestJournal::ScriptState(f.Owner());
    REQUIRE_THROWS(f.Advance());
    REQUIRE(f.Gold() == 19);
    REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
    REQUIRE(QuestJournal::ScriptState(f.Owner()) == heap);
    RequireReady(f.InspectChild());
    f.Tick();
    REQUIRE(f.storage->submissions.size() == 3);
  } else {
    f.Advance();
    f.Tick();
    REQUIRE(f.AckChild(3).status == Status::Completed);
    REQUIRE(f.Gold() == 28);
  }
}

TEST_CASE(
  "Queued child content and actor identity remain bound to one campaign",
  "[Quest][QuestJobs][ChildStages]")
{
  ChildFixture f;
  f.SubmitChild();
  f.Tick();
  RequireReady(f.AckChild(0));
  const auto before = QuestJournal::Snapshot(f.Owner());
  auto changed = f.content.metadata;
  changed.at(f.content.childQuest).fragments[0].function = "DifferentChild";
  REQUIRE_THROWS(
    f.Jobs().Advance(f.Owner(), kChildCommand, changed, f.content.Scripts()));
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
  REQUIRE(f.Gold() == 12);
  f.Advance();
  f.Tick();
  const auto waiting = f.AckChild(1);
  REQUIRE(waiting.status == Status::Waiting);
  REQUIRE(f.Gold() == 15);
  REQUIRE_THROWS(f.Jobs().Resume(f.Owner(), kChildCommand, waiting.token, true,
                                 changed, f.content.Scripts()));
  const auto beforeMissingAdapter = QuestJournal::Snapshot(f.Owner());
  f.Owner().SetPropertyValueDump(QuestJournal::kEnabled,
                                 "[\"2d512:Skyrim.esm\"]", false, false);
  REQUIRE_THROWS(f.ResumeChild(waiting.token));
  REQUIRE(f.Gold() == 15);
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == beforeMissingAdapter);
  f.content.Enable(f.Owner());
  constexpr uint32_t guestId = kOwner + 1, otherId = kOwner + 2;
  f.server.CreateActor(guestId, { 1, 2, 3 }, 0, 0x3c);
  auto& guest = f.server.worldState.GetFormAt<MpActor>(guestId);
  guest.SetWorldInstance(f.Owner().GetWorldInstance());
  f.content.Enable(guest);
  REQUIRE_THROWS(f.Jobs().Inspect(guest, kChildCommand));
  REQUIRE_THROWS(f.Jobs().Resume(guest, kChildCommand, waiting.token, true,
                                 f.content.metadata, f.content.Scripts()));
  f.server.CreateActor(otherId, { 1, 2, 3 }, 0, 0x3c);
  auto& other = f.server.worldState.GetFormAt<MpActor>(otherId);
  other.SetWorldInstance("campaign:" + std::to_string(otherId) + ":jobs");
  other.SetInventory(Inventory().AddItem(0xf, 100));
  f.content.Enable(other);
  RequirePending(f.Jobs().Submit(other, kChildCommand, kParentQuest, 10,
                                 f.content.metadata, f.content.Scripts()));
  REQUIRE(other.GetInventory().GetItemCount(0xf) == 102);
  REQUIRE(f.Gold() == 15);
  REQUIRE(f.InspectChild().status == Status::Waiting);
  REQUIRE(f.InspectChild().token == waiting.token);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 3);
  f.storage->Complete(2);
  f.Tick();
  RequireReady(f.Jobs().Inspect(other, kChildCommand));
  REQUIRE(f.InspectChild().status == Status::Waiting);
  REQUIRE(QuestJournal::Get(other, f.content.childQuest).at("stages").empty());
  REQUIRE(QuestJournal::Get(f.Owner(), f.content.childQuest).at("stages") ==
          Json::array({ f.content.childStage }));
}

TEST_CASE(
  "A child can queue another stage in its parent's quest and unwind in order",
  "[Quest][QuestJobs][ChildStages]")
{
  ChildFixture f(false);
  auto& object = f.content.program.script->objectTable[0];
  object.variables.push_back({ "grandRuns", "Int", 0, VarValue(0) });
  auto& child = object.states[0].functions[1].function;
  child.locals.push_back({ "parentQuest", "Quest" });
  child.code.instructions.pop_back();
  const std::vector<FunctionCode::Instruction> suffix = {
    { FunctionCode::kOp_CallStatic,
      { VarValue("Game"), VarValue("GetForm"), Name("parentQuest"),
        VarValue(1), VarValue(static_cast<int32_t>(kParentQuest)) } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("SetStage"), Name("parentQuest"), Name("completed"),
        VarValue(1), VarValue(20) } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("AddItem"), Name("target"), Name("temp"), VarValue(3),
        Name("gold"), VarValue(1), VarValue(true) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  child.code.instructions.insert(child.code.instructions.end(), suffix.begin(),
                                 suffix.end());
  auto grandchild = Reward(6);
  grandchild.code.instructions.pop_back();
  grandchild.code.instructions.push_back(
    { FunctionCode::kOp_ArraySetElement,
      { Name("shared"), VarValue(0), VarValue(8) } });
  grandchild.code.instructions.push_back(
    { FunctionCode::kOp_IAdd,
      { Name("grandRuns"), Name("grandRuns"), VarValue(1) } });
  grandchild.code.instructions.push_back(
    { FunctionCode::kOp_Return, { VarValue::None() } });
  object.states[0].functions.push_back({ "Grandchild", grandchild });
  f.content.metadata.at(kParentQuest)
    .fragments.push_back({ 20, 0, 1, "JobFixture", "Grandchild" });
  RequirePending(f.SubmitChild());
  f.Tick();
  RequireReady(f.AckChild(0));
  const std::vector<uint32_t> expectedGold{ 15, 21, 22, 30 };
  const std::vector<size_t> expectedStacks{ 2, 2, 1, 0 };
  for (size_t index = 0; index < expectedGold.size(); ++index) {
    RequirePending(f.Advance());
    REQUIRE(f.Gold() == expectedGold[index]);
    REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations").size() ==
            expectedStacks[index]);
    f.Tick();
    const auto result = f.AckChild(index + 1);
    if (index + 1 == expectedGold.size())
      REQUIRE(result.status == Status::Completed);
    else
      RequireReady(result);
  }
  REQUIRE(QuestJournal::Get(f.Owner(), kParentQuest).at("stages") ==
          Json::array({ 10, 20 }));
  REQUIRE(ChildField(f.Owner(), kParentQuest, "runs").at("value") == 1);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "grandruns").at("value") == 1);
  REQUIRE(ChildField(f.Owner(), f.content.childQuest, "runs").at("value") ==
          1);
  REQUIRE(f.SubmitChild().status == Status::Completed);
  REQUIRE(f.Gold() == 30);
}

TEST_CASE(
  "Queued stage children recover from SQLite before and after child work",
  "[Quest][QuestJobs][ChildStages][SQLite]")
{
  SqliteFiles files;
  ChildProgram content;
  uint64_t jobId = 0;
  std::string externalToken;
  {
    PartOne server;
    AttachDatabase(server, files);
    Initialize(server);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    content.Enable(owner);
    auto& jobs = server.worldState.GetQuestJobs();
    RequirePending(jobs.Submit(owner, kChildCommand, kParentQuest, 10,
                               content.metadata, content.Scripts()));
    const auto ready = WaitForDurable(server, owner, kChildCommand);
    RequireReady(ready);
    jobId = ready.job;
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 12);
    REQUIRE(QuestJournal::Get(owner, content.childQuest).at("stages").empty());
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = server.worldState.GetQuestJobs();
    RequireReady(jobs.Inspect(owner, kChildCommand));
    REQUIRE(jobs.Inspect(owner, kChildCommand).job == jobId);
    RequireReady(jobs.Submit(owner, kChildCommand, kParentQuest, 10,
                             content.metadata, content.Scripts()));
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 12);
    RequirePending(
      jobs.Advance(owner, kChildCommand, content.metadata, content.Scripts()));
    const auto waiting = WaitForDurable(server, owner, kChildCommand);
    REQUIRE(waiting.status == Status::Waiting);
    REQUIRE(waiting.key == "child");
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 15);
    externalToken = waiting.token;
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = server.worldState.GetQuestJobs();
    const auto waiting = jobs.Inspect(owner, kChildCommand);
    REQUIRE(waiting.status == Status::Waiting);
    REQUIRE(waiting.token == externalToken);
    REQUIRE(waiting.job == jobId);
    RequirePending(jobs.Resume(owner, kChildCommand, externalToken, true,
                               content.metadata, content.Scripts()));
    RequireReady(WaitForDurable(server, owner, kChildCommand));
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 19);
    REQUIRE(QuestJournal::JobState(owner).active->Current().cursor == "done");
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = server.worldState.GetQuestJobs();
    RequireReady(jobs.Inspect(owner, kChildCommand));
    RequireReady(jobs.Resume(owner, kChildCommand, externalToken, true,
                             content.metadata, content.Scripts()));
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 19);
    RequirePending(
      jobs.Advance(owner, kChildCommand, content.metadata, content.Scripts()));
    REQUIRE(WaitForDurable(server, owner, kChildCommand).status ==
            Status::Completed);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 28);
    REQUIRE(ChildField(owner, kParentQuest, "runs").at("value") == 1);
    REQUIRE(ChildField(owner, content.childQuest, "runs").at("value") == 1);
    REQUIRE(ChildField(owner, kParentQuest, "shared") ==
            ChildField(owner, content.childQuest, "shared"));
    REQUIRE(QuestJournal::ScriptState(owner).at("continuations").empty());
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = server.worldState.GetQuestJobs();
    const auto before = QuestJournal::Snapshot(owner);
    REQUIRE(jobs
              .Submit(owner, kChildCommand, kParentQuest, 10, content.metadata,
                      content.Scripts())
              .status == Status::Completed);
    REQUIRE(jobs
              .Resume(owner, kChildCommand, externalToken, true,
                      content.metadata, content.Scripts())
              .status == Status::Completed);
    REQUIRE(QuestJournal::Snapshot(owner) == before);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 28);
  }
}

TEST_CASE("A repeated child cannot reenter its still-active ancestor stage",
          "[Quest][QuestJobs][ChildStages][Rollback]")
{
  ChildFixture f(false, false, true);
  auto& child =
    f.content.program.script->objectTable[0].states[0].functions[1].function;
  child.code.instructions.insert(
    child.code.instructions.end() - 1,
    { FunctionCode::kOp_CallMethod,
      { VarValue("SetStage"), Name("self"), Name("completed"), VarValue(1),
        VarValue(10) } });
  RequirePending(f.SubmitChild());
  REQUIRE(f.Gold() == 12);
  f.Tick();
  RequireReady(f.AckChild(0));
  const auto before = QuestJournal::Snapshot(f.Owner());
  const auto heap = QuestJournal::ScriptState(f.Owner());
  const auto jobs = QuestJournal::JobState(f.Owner()).Save();
  REQUIRE_THROWS_WITH(f.Advance(),
                      Catch::Matchers::ContainsSubstring(
                        "Reentrant repeated quest stage requires scheduling"));
  REQUIRE(f.Gold() == 12);
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == heap);
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == jobs);
  REQUIRE(
    QuestJournal::Get(f.Owner(), f.content.childQuest).at("stages").empty());
  REQUIRE(ChildField(f.Owner(), kParentQuest, "runs").at("value") == 1);
  RequireReady(f.InspectChild());
  RequireReady(f.SubmitChild());
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
}

TEST_CASE("Child stage VMs share the parent's cumulative instruction budget",
          "[Quest][QuestJobs][ChildStages][Quota]")
{
  ChildFixture f(false);
  auto& functions =
    f.content.program.script->objectTable[0].states[0].functions;
  // Four authored instructions execute about sixty thousand VM steps. Each
  // function fits by itself, but the child's fresh VM must retain the
  // parent's expenditure instead of starting another 100,000-step budget.
  for (size_t index = 0; index < 2; ++index) {
    auto& function = functions[index].function;
    function.locals.push_back({ "budgetCounter", "Int" });
    function.locals.push_back({ "budgetAgain", "Bool" });
    const std::vector<FunctionCode::Instruction> loop = {
      { FunctionCode::kOp_Assign, { Name("budgetCounter"), VarValue(0) } },
      { FunctionCode::kOp_IAdd,
        { Name("budgetCounter"), Name("budgetCounter"), VarValue(1) } },
      { FunctionCode::kOp_CompareLT,
        { Name("budgetAgain"), Name("budgetCounter"), VarValue(20000) } },
      { FunctionCode::kOp_JumpT, { Name("budgetAgain"), VarValue(-2) } }
    };
    function.code.instructions.insert(function.code.instructions.begin(),
                                      loop.begin(), loop.end());
  }
  RequirePending(f.SubmitChild());
  REQUIRE(f.Gold() == 12);
  f.Tick();
  RequireReady(f.AckChild(0));
  const auto active = QuestJournal::JobState(f.Owner()).active;
  REQUIRE(active);
  REQUIRE(active->instructions >= 60000);
  REQUIRE(active->instructions < 65000);
  const auto before = QuestJournal::Snapshot(f.Owner());
  const auto heap = QuestJournal::ScriptState(f.Owner());
  const auto jobs = QuestJournal::JobState(f.Owner()).Save();
  REQUIRE_THROWS_WITH(
    f.Advance(),
    Catch::Matchers::ContainsSubstring("instruction quota exceeded"));
  REQUIRE(f.Gold() == 12);
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == heap);
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == jobs);
  REQUIRE(
    QuestJournal::Get(f.Owner(), f.content.childQuest).at("stages").empty());
  RequireReady(f.InspectChild());
  RequireReady(f.SubmitChild());
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
}
