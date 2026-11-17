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

QuestJobRunner::Result WaitForDurable(PartOne& server, MpActor& owner)
{
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    server.worldState.Tick();
    auto result = server.worldState.GetQuestJobs().Inspect(owner, kCommand);
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
