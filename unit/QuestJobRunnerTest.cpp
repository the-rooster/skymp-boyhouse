#include "QuestJobRunner.h"
#include "CampaignReferenceBinding.h"
#include "QuestJobClock.h"
#include "QuestJournal.h"
#include "QuestScriptRuntime.h"
#include "QuestVmState.h"
#include "TestUtils.hpp"
#include "database_drivers/SqliteDatabase.h"
#include "libespm/REFR.h"
#include "save_storages/AsyncSaveStorage.h"
#include "save_storages/ISaveStorage.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <exception>
#include <filesystem>
#include <limits>
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

std::optional<QuestJobState::Job> OnlyLive(const QuestJobState& jobs)
{
  REQUIRE(jobs.live.size() <= 1);
  if (jobs.live.empty())
    return std::nullopt;
  return jobs.live.begin()->second;
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

// Every host transition has its own receipt. This helper deliberately stops
// at an external wait or timer; it never invents a completion for a script.
QuestJobRunner::Result Drain(Fixture& f,
                             const QuestJobRunner::Request& command,
                             const QuestJobRunner::MetadataOverrides& metadata,
                             const std::vector<PexScript::Lazy>& scripts)
{
  for (size_t boundary = 0; boundary < 1024; ++boundary) {
    const auto result = f.Jobs().Inspect(f.Owner(), command);
    if (result.status == Status::Pending) {
      f.Tick();
      REQUIRE_FALSE(f.storage->submissions.empty());
      f.storage->Complete(f.storage->submissions.size() - 1);
      f.Tick();
    } else if (result.status == Status::Ready) {
      RequirePending(f.Jobs().Advance(f.Owner(), command, metadata, scripts));
    } else {
      return result;
    }
  }
  throw std::runtime_error("Quest dispatch failed to reach a stable boundary");
}

QuestJobRunner::Result Finish(Fixture& f,
                              const std::string& command = kCommand,
                              uint32_t quest = kQuest)
{
  const auto result =
    Drain(f, command, { { quest, f.program.metadata } }, f.program.Scripts());
  REQUIRE(result.status == Status::Completed);
  return result;
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
  REQUIRE(OnlyLive(firstJob));
  REQUIRE(OnlyLive(firstJob)->cursor == "waiting");
  REQUIRE(OnlyLive(firstJob)->log == 1);
  REQUIRE(OnlyLive(firstJob)->events.size() == 2);
  const auto heap = QuestJournal::ScriptState(f.Owner());
  REQUIRE(heap.at("continuations").size() == 1);
  REQUIRE(heap.at("continuations")
            .at(OnlyLive(firstJob)->token)
            .at("callers")
            .size() == 2);
  REQUIRE(heap.at("arrays").size() == 1);
  REQUIRE(f.Submit().job == submitted.job);
  REQUIRE(f.Gold() == 17);
  REQUIRE_THROWS(f.Resume(OnlyLive(firstJob)->token));
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
  REQUIRE(first.token == OnlyLive(firstJob)->token);
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
  REQUIRE(f.Ack(4).status == Status::Ready);
  const auto complete = Finish(f);
  const auto saved = f.storage->submissions.size();
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
  REQUIRE(f.storage->submissions.size() == saved);
}

TEST_CASE("Live quest jobs preserve mutation and identity checks",
          "[Quest][QuestJobs]")
{
  Fixture f;
  f.Submit();
  f.Tick();
  const auto waiting = f.Ack(0);
  REQUIRE(waiting.status == Status::Waiting);
  // A second non-repeating call sees the already visited stage and completes
  // without consuming the first job's saved continuation.
  RequirePending(f.Submit("independent-no-op"));
  f.Tick();
  f.storage->Complete(1);
  f.Tick();
  REQUIRE(f.Jobs().Inspect(f.Owner(), "independent-no-op").status ==
          Status::Completed);
  REQUIRE(f.Inspect().token == waiting.token);
  REQUIRE(f.Gold() == 17);
  const auto before = QuestJournal::Snapshot(f.Owner());
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
  REQUIRE(f.Ack(2).status == Status::Ready);
  Finish(f);
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
  REQUIRE(OnlyLive(QuestJournal::JobState(f.Owner()))->cursor == "done");
  REQUIRE(QuestJournal::JobState(f.Owner()).completed.empty());
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations").empty());
  REQUIRE(submit().job == submitted.job);
  RequirePending(f.Jobs().Inspect(f.Owner(), command));
  REQUIRE(f.Gold() == 14);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  f.storage->Complete(0);
  RequirePending(f.Jobs().Inspect(f.Owner(), command));
  f.Tick();
  REQUIRE(f.Jobs().Inspect(f.Owner(), command).status == Status::Ready);
  const auto completed = Finish(f, command, quest);
  const auto saved = f.storage->submissions.size();
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
  REQUIRE(f.storage->submissions.size() == saved);

  // A newer job's pending receipt must not hide a previously durable result.
  const auto later = f.Jobs().Submit(f.Owner(), "da04-stage5-again", quest, 5,
                                     f.program.metadata, f.program.Scripts());
  RequirePending(later);
  REQUIRE(later.job > completed.job);
  REQUIRE(f.Gold() == 14);
  REQUIRE(f.Jobs().Inspect(f.Owner(), command).status == Status::Completed);
  REQUIRE(submit().status == Status::Completed);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == saved + 1);
  REQUIRE(f.Jobs().Inspect(f.Owner(), command).status == Status::Completed);
  f.storage->Complete(saved);
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
  REQUIRE(f.Gold() == 15);
  REQUIRE(OnlyLive(QuestJournal::JobState(f.Owner()))->cursor == "done");
  Finish(f, command, quest);
  REQUIRE(f.Gold() == 19);
  const auto saved = f.storage->submissions.size();
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
  REQUIRE_FALSE(OnlyLive(jobs));
  REQUIRE(jobs.completed.size() == 1);
  REQUIRE(jobs.completed[0].stage == 10);
  REQUIRE(jobs.completed[0].log == 0);
  REQUIRE(jobs.completed[0].cursor == "done");
  REQUIRE(jobs.completed[0].eventHead == jobs.completed[0].events.size());
  REQUIRE(jobs.completed[0].events.size() == 2);
  REQUIRE(jobs.completed[0].completions.size() == 1);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == saved);
  REQUIRE(f.Jobs().Inspect(f.Owner(), command).status == Status::Completed);
  const auto before = QuestJournal::Snapshot(f.Owner());
  REQUIRE(submit().status == Status::Completed);
  REQUIRE(f.Gold() == 19);
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == saved);
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

QuestJobRunner::Result WaitForDurable(
  PartOne& server, MpActor& owner,
  const QuestJobRunner::Request& command = kCommand)
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

QuestJobRunner::Result DrainStored(
  PartOne& server, MpActor& owner, const QuestJobRunner::Request& command,
  const QuestJobRunner::MetadataOverrides& metadata,
  const std::vector<PexScript::Lazy>& scripts)
{
  for (size_t boundary = 0; boundary < 1024; ++boundary) {
    const auto result = WaitForDurable(server, owner, command);
    if (result.status != Status::Ready)
      return result;
    RequirePending(server.worldState.GetQuestJobs().Advance(
      owner, command, metadata, scripts));
  }
  throw std::runtime_error("SQLite quest dispatch did not settle");
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
    const auto complete =
      DrainStored(server, owner, kCommand, { { kQuest, program.metadata } },
                  program.Scripts());
    REQUIRE(complete.status == Status::Completed);
    REQUIRE(complete.job == jobId);
    REQUIRE(complete.accepted);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 37);
    REQUIRE(QuestJournal::ScriptState(owner).at("continuations").empty());
    REQUIRE_FALSE(OnlyLive(QuestJournal::JobState(owner)));
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

QuestJobRunner::Result FinishChild(ChildFixture& f)
{
  const auto result =
    Drain(f, kChildCommand, f.content.metadata, f.content.Scripts());
  REQUIRE(result.status == Status::Completed);
  return result;
}

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
  const auto parentToken = OnlyLive(QuestJournal::JobState(f.Owner()))->token;
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
  REQUIRE(InventoryOrder(f.Owner()) == 0);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "runs").at("value") == 1);
  REQUIRE(ChildField(f.Owner(), f.content.childQuest, "runs").at("value") ==
          1);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "firstresult").at("value") ==
          true);
  f.Tick();
  RequireReady(f.AckChild(5));
  FinishChild(f);
  REQUIRE(InventoryOrder(f.Owner()) == 2349);
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
    RequireReady(result);
    if (segment == 4)
      FinishChild(f);
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
    RequireReady(f.AckChild(3));
    FinishChild(f);
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
    RequireReady(result);
    if (index + 1 == expectedGold.size())
      FinishChild(f);
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
    REQUIRE(OnlyLive(QuestJournal::JobState(owner))->Current().cursor ==
            "done");
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
    REQUIRE(DrainStored(server, owner, kChildCommand, content.metadata,
                        content.Scripts())
              .status == Status::Completed);
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
  const auto active = OnlyLive(QuestJournal::JobState(f.Owner()));
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

namespace {
constexpr uint32_t kTimerQuest = 0x2d512;
constexpr auto kTimerCommand = "utility-timer";
struct TimerProgram
{
  Program program;
  QuestJobRunner::MetadataOverrides metadata;
  TimerProgram(std::vector<float> seconds = { 1.0f },
               const char* native = "Wait", bool fail = false,
               bool wrongSlot = false)
  {
    auto function = Reward(2);
    function.code.instructions.pop_back();
    if (wrongSlot)
      function.locals.push_back({ "wrongResult", "Bool" });
    for (size_t index = 0; index < seconds.size(); ++index) {
      function.code.instructions.push_back(
        { FunctionCode::kOp_CallStatic,
          { VarValue("Utility"), VarValue(native),
            Name(wrongSlot ? "wrongResult" : "temp"), VarValue(1),
            VarValue(static_cast<double>(seconds[index])) } });
      function.code.instructions.push_back(
        { FunctionCode::kOp_CallMethod,
          { VarValue("AddItem"), Name("target"), Name("temp"), VarValue(3),
            Name("gold"), VarValue(index ? 5 : 3), VarValue(true) } });
    }
    if (fail)
      function.code.instructions.push_back(
        { FunctionCode::kOp_CallMethod,
          { VarValue("UnsupportedTimerSuffix"), Name("self"), Name("temp"),
            VarValue(0) } });
    function.code.instructions.push_back(
      { FunctionCode::kOp_Return, { VarValue::None() } });
    program.script->objectTable[0].states = { { "",
                                                { { "Timer", function } } } };
    program.metadata.fragments = { { 5, 0, 1, "JobFixture", "Timer" } };
    metadata[kTimerQuest] = program.metadata;
  }
  std::vector<PexScript::Lazy> Scripts() const { return program.Scripts(); }
  void Enable(MpActor& actor) const
  {
    actor.SetPropertyValueDump(QuestJournal::kEnabled,
                               "[\"2d512:Skyrim.esm\"]", false, false);
  }
};

struct TimerFixture : Fixture
{
  TimerProgram content;
  std::shared_ptr<QuestJobTime> time =
    std::make_shared<QuestJobTime>(QuestJobTime{ 1000000, 5000 });
  TimerFixture(std::vector<float> seconds = { 1.0f },
               const char* native = "Wait", bool fail = false,
               bool wrongSlot = false)
    : content(std::move(seconds), native, fail, wrongSlot)
  {
    content.Enable(Owner());
    Jobs().SetClock([time = time] { return *time; });
  }
  QuestJobRunner::Result SubmitTimer(bool automatic = false)
  {
    return Jobs().Submit(Owner(), kTimerCommand, kTimerQuest, 5,
                         content.metadata, content.Scripts(), automatic);
  }
  QuestJobRunner::Result AdvanceTimer()
  {
    return Jobs().Advance(Owner(), kTimerCommand, content.metadata,
                          content.Scripts());
  }
  QuestJobRunner::Result InspectTimer()
  {
    return Jobs().Inspect(Owner(), kTimerCommand);
  }
  QuestJobRunner::Result AckTimer(size_t index)
  {
    storage->Complete(index);
    Tick();
    return InspectTimer();
  }
};

QuestJobRunner::Result FinishTimer(TimerFixture& f)
{
  const auto result =
    Drain(f, kTimerCommand, f.content.metadata, f.content.Scripts());
  REQUIRE(result.status == Status::Completed);
  return result;
}

void RequireSleeping(const QuestJobRunner::Result& result)
{
  REQUIRE(result.status == Status::Sleeping);
  REQUIRE(result.token.empty());
  REQUIRE(result.key.empty());
}
}

TEST_CASE("Managed Utility waits preserve native millisecond conversion and "
          "None results",
          "[Quest][QuestJobs][Timers]")
{
  const auto native = GENERATE("Wait", "WaitMenuMode");
  struct Case
  {
    float seconds;
    uint64_t duration;
    bool queues;
  };
  const std::vector<Case> cases{
    { -2.0f, 0, false },          { -0.0f, 0, false },
    { 0.0f, 0, false },           { 0.0001f, 0, true },
    { 0.0019f, 1, true },         { 1.2349f, 1234, true },
    { 86400.0f, 86400000, true }, { 1.0e20f, 86400000, true }
  };
  for (const auto& test : cases) {
    INFO(native << " seconds=" << test.seconds);
    TimerFixture f({ test.seconds }, native);
    size_t clockReads = 0;
    f.Jobs().SetClock([&] {
      ++clockReads;
      return *f.time;
    });
    RequirePending(f.SubmitTimer());
    if (!test.queues) {
      REQUIRE(clockReads == 0);
      REQUIRE(f.Gold() == 15);
      const auto jobs = QuestJournal::JobState(f.Owner());
      REQUIRE(OnlyLive(jobs));
      REQUIRE(OnlyLive(jobs)->cursor == "done");
      REQUIRE(OnlyLive(jobs)->completions.empty());
      REQUIRE(OnlyLive(jobs)->nextOperation == 1);
      f.Tick();
      RequireReady(f.AckTimer(0));
      FinishTimer(f);
      continue;
    }
    REQUIRE(f.Gold() == 12);
    const auto job = OnlyLive(QuestJournal::JobState(f.Owner()));
    REQUIRE(job);
    REQUIRE(job->Current().timer);
    REQUIRE(job->Current().timer->issuedUnixMs == 1000000);
    REQUIRE(job->Current().timer->durationMs == test.duration);
    REQUIRE(job->Current().timer->dueUnixMs == 1000000 + test.duration);
    const auto token = job->Current().token;
    REQUIRE_THROWS(f.Jobs().Resume(f.Owner(), kTimerCommand, token, true,
                                   f.content.metadata, f.content.Scripts()));
    f.Tick();
    const auto observed = f.AckTimer(0);
    if (test.duration)
      RequireSleeping(observed);
    else
      RequireReady(observed);
    f.time->steadyMs += test.duration;
    RequireReady(f.InspectTimer());
    REQUIRE_THROWS(f.Jobs().Resume(f.Owner(), kTimerCommand, token, false,
                                   f.content.metadata, f.content.Scripts()));
    RequirePending(f.AdvanceTimer());
    REQUIRE(f.Gold() == 15);
    f.Tick();
    RequireReady(f.AckTimer(1));
    FinishTimer(f);
    const auto completed = QuestJournal::JobState(f.Owner()).completed.back();
    REQUIRE(completed.completions.at(token).kind ==
            QuestJobState::Completion::Kind::None);
    REQUIRE(f.SubmitTimer().status == Status::Completed);
    REQUIRE(f.Gold() == 15);
  }
}

TEST_CASE(
  "Nonfinite waits and malformed None destinations cannot publish rewards",
  "[Quest][QuestJobs][Timers][Rollback]")
{
  const auto invalid = GENERATE(0, 1, 2, 3);
  const float seconds = invalid == 0 ? std::numeric_limits<float>::quiet_NaN()
    : invalid == 1                   ? std::numeric_limits<float>::infinity()
    : invalid == 2                   ? -std::numeric_limits<float>::infinity()
                                     : 1.0f;
  TimerFixture f({ seconds }, "Wait", false, invalid == 3);
  const auto before = QuestJournal::Snapshot(f.Owner());
  const auto jobs = QuestJournal::JobState(f.Owner()).Save();
  REQUIRE_THROWS(f.SubmitTimer());
  REQUIRE(f.Gold() == 10);
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == jobs);
  REQUIRE(QuestJournal::ScriptState(f.Owner()).empty());
}

TEST_CASE("Elapsed timers remain hidden behind failed saves without "
          "restarting the delay",
          "[Quest][QuestJobs][Timers][SaveReceipt]")
{
  TimerFixture f;
  RequirePending(f.SubmitTimer());
  f.time->steadyMs += 1500;
  f.time->unixMs += 1500;
  RequirePending(f.InspectTimer());
  RequirePending(f.AdvanceTimer());
  REQUIRE(f.Gold() == 12);
  f.Tick();
  const auto original = Serialized(f.storage->submissions[0].forms);
  f.storage->Complete(
    0, std::make_exception_ptr(std::runtime_error("timer save")));
  f.Tick();
  RequirePending(f.InspectTimer());
  REQUIRE(Serialized(f.storage->submissions[1].forms) == original);
  f.storage->Complete(0);
  f.Tick();
  RequirePending(f.InspectTimer());
  RequireReady(f.AckTimer(1));
  REQUIRE(f.Gold() == 12);
  RequirePending(f.AdvanceTimer());
  REQUIRE(f.Gold() == 15);
  f.Tick();
  RequireReady(f.AckTimer(2));
  FinishTimer(f);
}

TEST_CASE(
  "Live quest timers use steady time across UTC jumps and consecutive waits",
  "[Quest][QuestJobs][Timers]")
{
  TimerFixture f({ 1.0f, 2.0f }, "WaitMenuMode");
  f.SubmitTimer();
  f.Tick();
  RequireSleeping(f.AckTimer(0));
  f.time->unixMs += 86400000;
  RequireSleeping(f.InspectTimer());
  f.time->unixMs = 1;
  RequireSleeping(f.AdvanceTimer());
  f.time->steadyMs += 999;
  RequireSleeping(f.InspectTimer());
  ++f.time->steadyMs;
  RequireReady(f.InspectTimer());
  RequirePending(f.AdvanceTimer());
  REQUIRE(f.Gold() == 15);
  f.Tick();
  RequireSleeping(f.AckTimer(1));
  const auto second =
    OnlyLive(QuestJournal::JobState(f.Owner()))->Current().timer;
  REQUIRE(second);
  REQUIRE(second->issuedUnixMs == 1);
  REQUIRE(second->durationMs == 2000);
  f.time->unixMs = 999999999;
  f.time->steadyMs += 1999;
  RequireSleeping(f.InspectTimer());
  ++f.time->steadyMs;
  RequireReady(f.InspectTimer());
  f.AdvanceTimer();
  REQUIRE(f.Gold() == 20);
  f.Tick();
  RequireReady(f.AckTimer(2));
  FinishTimer(f);
}

TEST_CASE(
  "Parent and child timer waits retain their own deadlines and None slots",
  "[Quest][QuestJobs][Timers][ChildStages]")
{
  ChildFixture f;
  auto time = std::make_shared<QuestJobTime>(QuestJobTime{ 1000000, 5000 });
  f.Jobs().SetClock([time] { return *time; });
  auto& functions =
    f.content.program.script->objectTable[0].states[0].functions;
  auto& parent = functions[0].function.code.instructions;
  const auto stageCall =
    std::find_if(parent.begin(), parent.end(), [](const auto& op) {
      return op.op == FunctionCode::kOp_CallMethod &&
        std::string(static_cast<const char*>(op.args[0])) == "SetStage";
    });
  REQUIRE(stageCall != parent.end());
  parent.insert(stageCall,
                { FunctionCode::kOp_CallStatic,
                  { VarValue("Utility"), VarValue("Wait"), Name("temp"),
                    VarValue(1), VarValue(1.0) } });
  auto& child = functions[1].function.code.instructions;
  const auto childWait =
    std::find_if(child.begin(), child.end(), [](const auto& op) {
      return op.op == FunctionCode::kOp_CallStatic &&
        std::string(static_cast<const char*>(op.args[0])) == "FixtureAwait";
    });
  REQUIRE(childWait != child.end());
  *childWait = { FunctionCode::kOp_CallStatic,
                 { VarValue("Utility"), VarValue("WaitMenuMode"), Name("temp"),
                   VarValue(1), VarValue(1.0) } };
  f.SubmitChild();
  f.Tick();
  RequireSleeping(f.AckChild(0));
  REQUIRE(f.Gold() == 12);
  time->steadyMs += 1000;
  f.Advance();
  f.Tick();
  RequireReady(f.AckChild(1));
  REQUIRE(f.Gold() == 12);
  f.Advance();
  f.Tick();
  RequireSleeping(f.AckChild(2));
  REQUIRE(f.Gold() == 15);
  time->steadyMs += 999;
  RequireSleeping(f.InspectChild());
  ++time->steadyMs;
  RequireReady(f.InspectChild());
  f.Advance();
  f.Tick();
  RequireReady(f.AckChild(3));
  REQUIRE(f.Gold() == 19);
  f.Advance();
  f.Tick();
  RequireReady(f.AckChild(4));
  FinishChild(f);
  REQUIRE(f.Gold() == 28);
  const auto completions =
    QuestJournal::JobState(f.Owner()).completed.back().completions;
  REQUIRE(completions.size() == 3);
  size_t voids = 0, booleans = 0;
  for (const auto& [token, completion] : completions) {
    if (completion.kind == QuestJobState::Completion::Kind::None)
      ++voids;
    else {
      REQUIRE(completion.value);
      ++booleans;
    }
  }
  REQUIRE(voids == 2);
  REQUIRE(booleans == 1);
}

TEST_CASE(
  "SQLite timer recovery credits UTC downtime and clamps clock rollback",
  "[Quest][QuestJobs][Timers][SQLite]")
{
  const auto recovery = GENERATE(0, 1, 2);
  const uint64_t recoveredUnix = recovery == 0 ? 1004000
    : recovery == 1                            ? 1020000
                                               : 995000;
  const uint64_t remaining = recovery == 0 ? 6000 : recovery == 1 ? 0 : 10000;
  SqliteFiles files;
  TimerProgram content({ 10.0f });
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1000000, 5000 }; });
    Initialize(server);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    content.Enable(owner);
    jobs.Submit(owner, kTimerCommand, kTimerQuest, 5, content.metadata,
                content.Scripts());
    RequireSleeping(WaitForDurable(server, owner, kTimerCommand));
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 12);
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto time =
      std::make_shared<QuestJobTime>(QuestJobTime{ recoveredUnix, 100 });
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([time] { return *time; });
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    const auto observed = jobs.Inspect(owner, kTimerCommand);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 12);
    if (remaining) {
      RequireSleeping(observed);
      time->unixMs = 999999999;
      time->steadyMs += remaining - 1;
      RequireSleeping(jobs.Inspect(owner, kTimerCommand));
      ++time->steadyMs;
    } else
      RequireReady(observed);
    RequireReady(jobs.Inspect(owner, kTimerCommand));
    RequirePending(
      jobs.Advance(owner, kTimerCommand, content.metadata, content.Scripts()));
    REQUIRE(DrainStored(server, owner, kTimerCommand, content.metadata,
                        content.Scripts())
              .status == Status::Completed);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 15);
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = server.worldState.GetQuestJobs();
    const auto before = QuestJournal::Snapshot(owner);
    REQUIRE(jobs.Inspect(owner, kTimerCommand).status == Status::Completed);
    REQUIRE(jobs
              .Submit(owner, kTimerCommand, kTimerQuest, 5, content.metadata,
                      content.Scripts())
              .status == Status::Completed);
    REQUIRE(QuestJournal::Snapshot(owner) == before);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 15);
  }
}

TEST_CASE("Automatic timer jobs recover and finish through server ticks alone",
          "[Quest][QuestJobs][Timers][Scheduler][SQLite]")
{
  SqliteFiles files;
  TimerProgram content({ 1.0f });
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1000000, 5000 }; });
    jobs.SetSchedulerFixtureContent(content.metadata, content.Scripts());
    Initialize(server);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    content.Enable(owner);
    jobs.Submit(owner, kTimerCommand, kTimerQuest, 5, content.metadata,
                content.Scripts(), true);
    RequireSleeping(WaitForDurable(server, owner, kTimerCommand));
    REQUIRE(OnlyLive(QuestJournal::JobState(owner))->automatic);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 12);
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1005000, 100 }; });
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    server.worldState.Tick();
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 12);
    RequireReady(jobs.Inspect(owner, kTimerCommand));
    jobs.SetSchedulerFixtureContent(content.metadata, content.Scripts());
    const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (jobs.Inspect(owner, kTimerCommand).status != Status::Completed &&
           std::chrono::steady_clock::now() < deadline) {
      server.worldState.Tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(jobs.Inspect(owner, kTimerCommand).status == Status::Completed);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 15);
    const auto before = QuestJournal::Snapshot(owner);
    for (int index = 0; index < 4; ++index)
      server.worldState.Tick();
    REQUIRE(QuestJournal::Snapshot(owner) == before);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 15);
  }
}

namespace {
MpActor& TimerActor(PartOne& server, const TimerProgram& content, uint32_t id,
                    uint32_t gold = 10)
{
  server.CreateActor(id, { 1, 2, 3 }, 0, 0x3c);
  auto& actor = server.worldState.GetFormAt<MpActor>(id);
  actor.SetWorldInstance("campaign:" + std::to_string(id) + ":timer-pump");
  actor.SetInventory(Inventory().AddItem(0xf, gold));
  content.Enable(actor);
  return actor;
}
}

TEST_CASE(
  "Automatic quest scheduling shares bounded passes across 32 campaigns",
  "[Quest][QuestJobs][Timers][Scheduler]")
{
  TimerProgram content({ 1.0f, 1.0f });
  PartOne server;
  server.AttachEspm(&GetEspmLoader());
  auto storage = std::make_shared<ControlledStorage>();
  server.AttachSaveStorage(storage);
  auto time = std::make_shared<QuestJobTime>(QuestJobTime{ 1000000, 5000 });
  auto& jobs = server.worldState.GetQuestJobs();
  jobs.SetClock([time] { return *time; });
  jobs.SetSchedulerFixtureContent(content.metadata, content.Scripts());
  std::vector<MpActor*> actors;
  for (uint32_t index = 0; index < 32; ++index) {
    auto& actor = TimerActor(server, content, 0xff007000 + index);
    actors.push_back(&actor);
    RequirePending(jobs.Submit(actor, kTimerCommand, kTimerQuest, 5,
                               content.metadata, content.Scripts(), true));
  }
  // Hold every timer asleep while all exact initial save receipts drain.
  for (size_t index = 0; index < 32; ++index)
    server.worldState.Tick();
  for (auto* actor : actors) {
    RequireSleeping(jobs.Inspect(*actor, kTimerCommand));
    REQUIRE(actor->GetInventory().GetItemCount(0xf) == 12);
  }
  time->steadyMs += 1000;
  size_t advanced = 0;
  for (size_t pass = 0; pass < 8 && advanced < actors.size(); ++pass) {
    server.worldState.Tick();
    const auto nowAdvanced = static_cast<size_t>(
      std::count_if(actors.begin(), actors.end(), [](auto* actor) {
        return actor->GetInventory().GetItemCount(0xf) == 15;
      }));
    REQUIRE(nowAdvanced > advanced);
    REQUIRE(nowAdvanced - advanced <= 8);
    advanced = nowAdvanced;
    for (auto* actor : actors)
      REQUIRE(actor->GetInventory().GetItemCount(0xf) <= 15);
  }
  REQUIRE(advanced == 32);
  time->steadyMs += 1000;
  size_t completed = 0;
  for (size_t pass = 0; pass < 2048 && completed < actors.size(); ++pass) {
    server.worldState.Tick();
    completed = static_cast<size_t>(
      std::count_if(actors.begin(), actors.end(), [&](auto* actor) {
        return jobs.Inspect(*actor, kTimerCommand).status == Status::Completed;
      }));
  }
  REQUIRE(completed == 32);
  for (auto* actor : actors) {
    REQUIRE(actor->GetInventory().GetItemCount(0xf) == 20);
    REQUIRE(
      QuestJournal::JobState(*actor).completed.back().completions.size() == 2);
  }
}

TEST_CASE("One failing automatic quest is quarantined without starving other "
          "campaigns",
          "[Quest][QuestJobs][Timers][Scheduler][Rollback]")
{
  TimerProgram content;
  auto& function =
    content.program.script->objectTable[0].states[0].functions[0].function;
  function.locals.push_back({ "count", "Int" });
  function.locals.push_back({ "badCampaign", "Bool" });
  function.code.instructions.pop_back();
  const std::vector<FunctionCode::Instruction> conditionalFailure = {
    { FunctionCode::kOp_CallMethod,
      { VarValue("GetItemCount"), Name("target"), Name("count"), VarValue(1),
        Name("gold") } },
    { FunctionCode::kOp_CompareEQ,
      { Name("badCampaign"), Name("count"), VarValue(45) } },
    { FunctionCode::kOp_JumpF, { Name("badCampaign"), VarValue(2) } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("UnsupportedAutomaticSuffix"), Name("self"), Name("temp"),
        VarValue(0) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  function.code.instructions.insert(function.code.instructions.end(),
                                    conditionalFailure.begin(),
                                    conditionalFailure.end());
  PartOne server;
  server.AttachEspm(&GetEspmLoader());
  auto storage = std::make_shared<ControlledStorage>();
  server.AttachSaveStorage(storage);
  auto time = std::make_shared<QuestJobTime>(QuestJobTime{ 1000000, 5000 });
  auto& jobs = server.worldState.GetQuestJobs();
  jobs.SetClock([time] { return *time; });
  jobs.SetSchedulerFixtureContent(content.metadata, content.Scripts());
  auto& bad = TimerActor(server, content, 0xff007100, 40);
  auto& one = TimerActor(server, content, 0xff007101);
  auto& two = TimerActor(server, content, 0xff007102);
  for (auto* actor : { &bad, &one, &two })
    jobs.Submit(*actor, kTimerCommand, kTimerQuest, 5, content.metadata,
                content.Scripts(), true);
  for (size_t index = 0; index < 3; ++index)
    server.worldState.Tick();
  RequireSleeping(jobs.Inspect(bad, kTimerCommand));
  const auto checkpoint = QuestJournal::Snapshot(bad);
  const auto heap = QuestJournal::ScriptState(bad);
  time->steadyMs += 1000;
  for (size_t index = 0; index < 64; ++index)
    server.worldState.Tick();
  const auto failed = jobs.Inspect(bad, kTimerCommand);
  REQUIRE(failed.status == Status::Failed);
  REQUIRE(failed.error.find("UnsupportedAutomaticSuffix") !=
          std::string::npos);
  REQUIRE(failed.token.empty());
  REQUIRE(failed.key.empty());
  REQUIRE(bad.GetInventory().GetItemCount(0xf) == 42);
  REQUIRE(QuestJournal::Snapshot(bad) == checkpoint);
  REQUIRE(QuestJournal::ScriptState(bad) == heap);
  for (auto* actor : { &one, &two }) {
    REQUIRE(jobs.Inspect(*actor, kTimerCommand).status == Status::Completed);
    REQUIRE(actor->GetInventory().GetItemCount(0xf) == 15);
  }
  const auto submissions = storage->submissions.size();
  for (size_t index = 0; index < 8; ++index)
    server.worldState.Tick();
  REQUIRE(jobs.Inspect(bad, kTimerCommand).status == Status::Failed);
  REQUIRE(QuestJournal::Snapshot(bad) == checkpoint);
  REQUIRE(storage->submissions.size() == submissions);
}

TEST_CASE(
  "A failed automatic submission cannot retain an obsolete scheduler identity",
  "[Quest][QuestJobs][Timers][Scheduler][Rollback]")
{
  const bool changedInstance = GENERATE(false, true);
  TimerFixture f;
  f.storage->automatic = true;
  f.Jobs().SetSchedulerFixtureContent(f.content.metadata, f.content.Scripts());
  const auto nextJob = QuestJournal::JobState(f.Owner()).nextJob;
  REQUIRE_THROWS(f.Jobs().Submit(f.Owner(), "failed-A", kTimerQuest, 65535,
                                 f.content.metadata, f.content.Scripts(),
                                 true));
  REQUIRE(f.Gold() == 10);
  REQUIRE(QuestJournal::JobState(f.Owner()).nextJob == nextJob);
  if (changedInstance)
    f.Owner().SetWorldInstance("campaign:" + std::to_string(kOwner) +
                               ":timer-phase");
  const std::string command = changedInstance ? "failed-A" : "successful-B";
  // No Tick between attempts. The reused numeric ID must not retain the
  // previous command or, when the command is reused, its previous phase.
  const auto submitted =
    f.Jobs().Submit(f.Owner(), command, kTimerQuest, 5, f.content.metadata,
                    f.content.Scripts(), true);
  RequirePending(submitted);
  REQUIRE(submitted.job == nextJob);
  REQUIRE(f.Gold() == 12);
  f.time->steadyMs += 1000;
  for (size_t index = 0; index < 64; ++index)
    f.Tick();
  REQUIRE(f.Jobs().Inspect(f.Owner(), command).status == Status::Completed);
  if (!changedInstance)
    REQUIRE_THROWS(f.Jobs().Inspect(f.Owner(), "failed-A"));
  REQUIRE(f.Gold() == 15);
  REQUIRE(QuestJournal::JobState(f.Owner()).completed.size() == 1);
  REQUIRE(QuestJournal::JobState(f.Owner()).completed.back().command ==
          command);
  REQUIRE(QuestJournal::JobState(f.Owner()).completed.back().instance ==
          f.Owner().GetWorldInstance());
}

TEST_CASE(
  "Clearing the world inside a scheduled clock check cannot revive its work",
  "[Quest][QuestJobs][Timers][Scheduler][Clear]")
{
  TimerFixture f;
  f.storage->automatic = true;
  f.Jobs().SetSchedulerFixtureContent(f.content.metadata, f.content.Scripts());
  bool clearOnClock = false;
  f.Jobs().SetClock([&]() -> QuestJobTime {
    if (clearOnClock) {
      clearOnClock = false;
      f.server.worldState.Clear();
      throw std::runtime_error("world cleared during scheduled clock check");
    }
    return *f.time;
  });
  f.SubmitTimer(true);
  f.Tick();
  RequireSleeping(f.InspectTimer());
  const auto retained = f.server.worldState.LookupFormByIdNoLoad(kOwner);
  REQUIRE(retained);
  auto* oldActor = retained->AsActor();
  REQUIRE(oldActor);
  const auto before = MpChangeForm::ToJson(oldActor->GetChangeForm());
  f.time->steadyMs += 1000;
  clearOnClock = true;
  REQUIRE_NOTHROW(f.Tick());
  REQUIRE_FALSE(clearOnClock);
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(kOwner));
  REQUIRE(oldActor->GetInventory().GetItemCount(0xf) == 12);
  REQUIRE(MpChangeForm::ToJson(oldActor->GetChangeForm()) == before);
  REQUIRE(f.Jobs().Inspect(*oldActor, kTimerCommand).status == Status::Failed);
  f.server.CreateActor(kOwner, { 7, 8, 9 }, 0, 0x3c);
  auto& replacement = f.server.worldState.GetFormAt<MpActor>(kOwner);
  replacement.SetInventory(Inventory().AddItem(0xf, 100));
  for (size_t index = 0; index < 3; ++index)
    REQUIRE_NOTHROW(f.Tick());
  REQUIRE(replacement.GetInventory().GetItemCount(0xf) == 100);
  REQUIRE(QuestJournal::JobState(replacement).completed.empty());
  REQUIRE_FALSE(OnlyLive(QuestJournal::JobState(replacement)));
}

TEST_CASE(
  "Durably sleeping jobs keep their persistence backend until completion",
  "[Quest][QuestJobs][Timers][SaveReceipt]")
{
  TimerFixture f;
  f.SubmitTimer();
  f.Tick();
  RequireSleeping(f.AckTimer(0));
  auto replacement = std::make_shared<ControlledStorage>();
  REQUIRE_NOTHROW(f.server.worldState.AttachSaveStorage(f.storage));
  REQUIRE_THROWS(f.server.worldState.AttachSaveStorage(replacement));
  REQUIRE_THROWS(f.server.worldState.AttachSaveStorage(nullptr));
  RequireSleeping(f.InspectTimer());
  REQUIRE(f.Gold() == 12);
  f.time->steadyMs += 1000;
  f.AdvanceTimer();
  f.Tick();
  RequireReady(f.AckTimer(1));
  FinishTimer(f);
  REQUIRE(f.Gold() == 15);
  REQUIRE_NOTHROW(f.server.worldState.AttachSaveStorage(replacement));
  REQUIRE_NOTHROW(f.server.worldState.AttachSaveStorage(nullptr));
}

TEST_CASE("Automatic submissions and recovered timer continuations retain "
          "their content provider",
          "[Quest][QuestJobs][Timers][Scheduler]")
{
  SECTION(
    "automatic admission rejects mismatched scheduler content before rewards")
  {
    TimerFixture f;
    TimerProgram other({ 2.0f });
    f.Jobs().SetSchedulerFixtureContent(other.metadata, other.Scripts());
    const auto before = QuestJournal::Snapshot(f.Owner());
    REQUIRE_THROWS(f.SubmitTimer(true));
    REQUIRE(f.Gold() == 10);
    REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
    REQUIRE_FALSE(OnlyLive(QuestJournal::JobState(f.Owner())));
    REQUIRE(QuestJournal::ScriptState(f.Owner()).empty());
  }
  SECTION("fixture input cannot resume a saved owned timer with an identical "
          "content hash")
  {
    TimerFixture f;
    f.SubmitTimer();
    f.Tick();
    RequireSleeping(f.AckTimer(0));
    auto saved = f.Owner().GetChangeForm();
    auto journal =
      Json::parse(saved.dynamicFields.GetValueDump(QuestJournal::kProperty));
    auto jobState = QuestJobState::Read(journal.at("jobs"));
    REQUIRE(jobState.live.size() == 1);
    const auto hash = jobState.live.begin()->second.contentHash;
    // Only provider provenance differs. Matching bytes do not authorize a
    // fixture overload to enter production-owned execution.
    jobState.live.begin()->second.contentMode = "owned";
    journal["jobs"] = jobState.Save();
    saved.dynamicFields.SetValueDump(QuestJournal::kProperty, journal.dump());
    PartOne restarted;
    restarted.AttachEspm(&GetEspmLoader());
    auto storage = std::make_shared<ControlledStorage>();
    restarted.AttachSaveStorage(storage);
    restarted.worldState.LoadChangeForm(saved, FormCallbacks::DoNothing());
    auto& actor = restarted.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = restarted.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1005000, 100 }; });
    RequireReady(jobs.Inspect(actor, kTimerCommand));
    REQUIRE(OnlyLive(QuestJournal::JobState(actor))->contentHash == hash);
    const auto before = QuestJournal::Snapshot(actor);
    REQUIRE_THROWS(jobs.Advance(actor, kTimerCommand, f.content.metadata,
                                f.content.Scripts()));
    REQUIRE(actor.GetInventory().GetItemCount(0xf) == 12);
    REQUIRE(QuestJournal::Snapshot(actor) == before);
    REQUIRE(storage->submissions.empty());
  }
}
namespace {
constexpr auto kEventCommand = "durable-inventory-handler";

FunctionInfo EventFunction()
{
  FunctionInfo function;
  function.valid = true;
  function.returnType = "None";
  function.params = { { "item", "Form" },
                      { "count", "Int" },
                      { "reference", "ObjectReference" },
                      { "source", "ObjectReference" } };
  return function;
}

struct EventProgram
{
  Program program;
  QuestJobRunner::MetadataOverrides metadata;
  std::shared_ptr<PexScript> base, observer;
  explicit EventProgram(bool child = false, const char* wait = "Wait",
                        bool remove = false)
  {
    auto& quest = program.script->objectTable[0];
    quest.states[0].functions = { { "Root", Reward(5) } };
    if (remove) {
      auto& code = quest.states[0].functions[0].function.code.instructions;
      code.insert(
        code.end() - 1,
        { FunctionCode::kOp_CallMethod,
          { VarValue("RemoveItem"), Name("target"), Name("temp"), VarValue(4),
            Name("gold"), VarValue(1), VarValue(true), VarValue::None() } });
    }
    program.metadata.fragments = { { 10, 0, 1, "JobFixture", "Root" } };
    espm::QuestScriptData::Alias alias;
    alias.object.type = 1;
    alias.object.formId = kParentQuest;
    alias.object.alias = 3;
    alias.version = 5;
    alias.objectFormat = 2;
    alias.scripts = { { "EventDerived", 0, {} }, { "EventObserver", 0, {} } };
    program.metadata.aliases = { alias };
    metadata[kParentQuest] = program.metadata;

    base = std::make_shared<PexScript>();
    base->source = "EventBase";
    Object object;
    object.NameIndex = base->source;
    object.parentClassName = "ReferenceAlias";
    object.variables = { { "prefixes", "Int", 0, VarValue(0) },
                         { "suffixes", "Int", 0, VarValue(0) } };
    auto handler = EventFunction();
    handler.locals = { { "matches", "Bool" }, { "target", "ObjectReference" },
                       { "temp", "None" },    { "other", "EventObserver" },
                       { "quest", "Quest" },  { "accepted", "Bool" } };
    handler.code.instructions = {
      { FunctionCode::kOp_CompareEQ,
        { Name("matches"), Name("count"), VarValue(5) } },
      { FunctionCode::kOp_JumpT, { Name("matches"), VarValue(2) } },
      { FunctionCode::kOp_Return, { VarValue::None() } },
      { FunctionCode::kOp_IAdd,
        { Name("prefixes"), Name("prefixes"), VarValue(1) } },
      { FunctionCode::kOp_CallMethod,
        { VarValue("GetReference"), Name("self"), Name("target"),
          VarValue(0) } },
      { FunctionCode::kOp_CallMethod,
        { VarValue("AddItem"), Name("target"), Name("temp"), VarValue(3),
          Name("item"), VarValue(2), VarValue(true) } },
      { FunctionCode::kOp_Cast, { Name("other"), Name("self") } },
      { FunctionCode::kOp_CallMethod,
        { VarValue("SetBusy"), Name("other"), Name("temp"), VarValue(0) } },
      { FunctionCode::kOp_Assign, { Name("::State"), VarValue("idle") } }
    };
    if (child) {
      handler.code.instructions.push_back(
        { FunctionCode::kOp_CallStatic,
          { VarValue("Game"), VarValue("GetForm"), Name("quest"), VarValue(1),
            VarValue(static_cast<int32_t>(kTimerQuest)) } });
      handler.code.instructions.push_back(
        { FunctionCode::kOp_CallMethod,
          { VarValue("SetStage"), Name("quest"), Name("accepted"), VarValue(1),
            VarValue(5) } });
      auto childFunction = Reward(4);
      childFunction.code.instructions.pop_back();
      childFunction.code.instructions.push_back(
        { FunctionCode::kOp_CallStatic,
          { VarValue("Utility"), VarValue(wait), Name("temp"), VarValue(1),
            VarValue(1.0f) } });
      childFunction.code.instructions.push_back(
        { FunctionCode::kOp_CallMethod,
          { VarValue("AddItem"), Name("target"), Name("temp"), VarValue(3),
            Name("gold"), VarValue(6), VarValue(true) } });
      childFunction.code.instructions.push_back(
        { FunctionCode::kOp_Return, { VarValue::None() } });
      quest.states[0].functions.push_back({ "Child", childFunction });
      auto childMetadata = program.metadata;
      childMetadata.aliases.clear();
      childMetadata.fragments = { { 5, 0, 1, "JobFixture", "Child" } };
      metadata[kTimerQuest] = childMetadata;
    } else {
      handler.code.instructions.push_back(
        { FunctionCode::kOp_CallStatic,
          { VarValue("Utility"), VarValue(wait), Name("temp"), VarValue(1),
            VarValue(1.0f) } });
    }
    handler.code.instructions.push_back(
      { FunctionCode::kOp_IAdd,
        { Name("suffixes"), Name("suffixes"), VarValue(1) } });
    handler.code.instructions.push_back(
      { FunctionCode::kOp_CallMethod,
        { VarValue("AddItem"), Name("target"), Name("temp"), VarValue(3),
          Name("item"), VarValue(3), VarValue(true) } });
    handler.code.instructions.push_back(
      { FunctionCode::kOp_Return, { VarValue::None() } });
    auto empty = EventFunction();
    empty.code.instructions = { { FunctionCode::kOp_Return,
                                  { VarValue::None() } } };
    object.states = { { "busy", { { "OnItemAdded", handler } } },
                      { "idle", { { "OnItemAdded", empty } } } };
    base->objectTable.push_back(object);
    auto derived = std::make_shared<PexScript>();
    derived->source = "EventDerived";
    Object attached;
    attached.NameIndex = derived->source;
    attached.parentClassName = base->source;
    attached.autoStateName = "busy";
    derived->objectTable.push_back(attached);

    observer = std::make_shared<PexScript>();
    observer->source = "EventObserver";
    Object observed;
    observed.NameIndex = observer->source;
    observed.parentClassName = "ReferenceAlias";
    observed.variables = { { "order", "Int", 0, VarValue(0) },
                           { "selected", "Int", 0, VarValue(0) },
                           { "removed", "Int", 0, VarValue(0) } };
    auto observedEvent = EventFunction();
    observedEvent.code.instructions = {
      { FunctionCode::kOp_IMultiply,
        { Name("order"), Name("order"), VarValue(10) } },
      { FunctionCode::kOp_IAdd,
        { Name("order"), Name("order"), Name("count") } },
      { FunctionCode::kOp_Assign, { Name("selected"), VarValue(1) } },
      { FunctionCode::kOp_Return, { VarValue::None() } }
    };
    auto busy = observedEvent;
    busy.code.instructions[2].args[1] = VarValue(7);
    auto removed = busy;
    removed.code.instructions.insert(
      removed.code.instructions.end() - 1,
      { FunctionCode::kOp_IAdd,
        { Name("removed"), Name("removed"), Name("count") } });
    FunctionInfo setBusy;
    setBusy.valid = true;
    setBusy.returnType = "None";
    setBusy.code.instructions = {
      { FunctionCode::kOp_Assign, { Name("::State"), VarValue("busy") } },
      { FunctionCode::kOp_Return, { VarValue::None() } }
    };
    observed.states = {
      { "", { { "OnItemAdded", observedEvent }, { "SetBusy", setBusy } } },
      { "busy", { { "OnItemAdded", busy }, { "OnItemRemoved", removed } } }
    };
    observer->objectTable.push_back(observed);
    program.bases.push_back(base);
    program.bases.push_back(derived);
    program.bases.push_back(observer);
  }
  auto Scripts() const { return program.Scripts(); }
  void Enable(MpActor& actor) const
  {
    actor.SetPropertyValueDump(QuestJournal::kEnabled,
                               "[\"4e50d:Skyrim.esm\",\"2d512:Skyrim.esm\"]",
                               false, false);
    QuestJournal::Atomic(actor, [&] {
      QuestVmState state(QuestJournal::ScriptState(actor), {}, {});
      state.SetAliasBinding("4e50d:Skyrim.esm/alias:3", "14:Skyrim.esm");
      QuestJournal::SetScriptState(actor, state.Save());
    });
  }
};

int EventField(MpActor& actor, const char* attachment, const char* field)
{
  const auto heap = QuestJournal::ScriptState(actor);
  const std::string root =
    std::string("4e50d:Skyrim.esm/alias:3/") + attachment;
  if (!heap.contains("scripts") || !heap.at("scripts").contains(root))
    return 0;
  const auto& values = heap.at("scripts").at(root).at("values");
  return values.contains(field) ? values.at(field).at("value").get<int>() : 0;
}

struct EventFixture : Fixture
{
  EventProgram content;
  std::shared_ptr<QuestJobTime> time =
    std::make_shared<QuestJobTime>(QuestJobTime{ 1000000, 5000 });
  EventFixture(bool child = false, const char* wait = "Wait",
               bool remove = false)
    : content(child, wait, remove)
  {
    content.Enable(Owner());
    Jobs().SetClock([time = time] { return *time; });
  }
  QuestJobRunner::Result SubmitEvent()
  {
    return Jobs().Submit(Owner(), kEventCommand, kParentQuest, 10,
                         content.metadata, content.Scripts());
  }
  QuestJobRunner::Result AdvanceEvent()
  {
    return Jobs().Advance(Owner(), kEventCommand, content.metadata,
                          content.Scripts());
  }
  QuestJobRunner::Result InspectEvent()
  {
    return Jobs().Inspect(Owner(), kEventCommand);
  }
  void AckLatest()
  {
    Tick();
    REQUIRE_FALSE(storage->submissions.empty());
    storage->Complete(storage->submissions.size() - 1);
    Tick();
  }
  void SelectFirstHandler()
  {
    for (size_t boundary = 0; boundary < 32; ++boundary) {
      if (InspectEvent().status == Status::Pending)
        AckLatest();
      const auto job = OnlyLive(QuestJournal::JobState(Owner()));
      REQUIRE(job);
      if (job->handler)
        return;
      RequireReady(InspectEvent());
      RequirePending(AdvanceEvent());
    }
    FAIL("Inventory event never selected its first handler");
  }
  QuestJobRunner::Result DrainEvent()
  {
    return Drain(*this, kEventCommand, content.metadata, content.Scripts());
  }
};
}

TEST_CASE(
  "Inventory handler prefixes and selected states cross exact receipts",
  "[Quest][QuestJobs][InventoryHandlers][SaveReceipt]")
{
  const auto wait = GENERATE("Wait", "WaitMenuMode");
  EventFixture f(false, wait, true);
  RequirePending(f.SubmitEvent());
  REQUIRE(f.Gold() == 14);
  REQUIRE(EventField(f.Owner(), "eventderived", "eventbase/prefixes") == 0);
  f.SelectFirstHandler();
  auto selected = OnlyLive(QuestJournal::JobState(f.Owner()));
  REQUIRE(selected->eventHead == 0);
  REQUIRE(selected->handler->cursor == "begin");
  REQUIRE(selected->handler->handlerIdentity->rootScript == "EventDerived");
  REQUIRE(selected->handler->handlerIdentity->implementation == "EventBase");
  REQUIRE(selected->handler->handlerIdentity->state == "busy");
  RequirePending(f.AdvanceEvent());
  REQUIRE(f.Gold() == 16);
  REQUIRE(EventField(f.Owner(), "eventderived", "eventbase/prefixes") == 1);
  REQUIRE(EventField(f.Owner(), "eventobserver", "eventobserver/order") == 0);
  const auto waiting = OnlyLive(QuestJournal::JobState(f.Owner()));
  REQUIRE(waiting->handler->cursor == "waiting");
  REQUIRE(waiting->handler->timer);
  const auto token = waiting->handler->token;
  REQUIRE_THROWS(f.Jobs().Resume(f.Owner(), kEventCommand, token, true,
                                 f.content.metadata, f.content.Scripts()));
  f.Tick();
  const auto failed = f.storage->submissions.size() - 1;
  const auto bytes = Serialized(f.storage->submissions[failed].forms);
  f.time->steadyMs += 1000;
  f.storage->Complete(
    failed, std::make_exception_ptr(std::runtime_error("handler save")));
  f.Tick();
  REQUIRE(Serialized(f.storage->submissions.back().forms) == bytes);
  RequirePending(f.AdvanceEvent());
  REQUIRE(f.Gold() == 16);
  f.storage->Complete(failed);
  f.Tick();
  RequirePending(f.InspectEvent());
  f.AckLatest();
  RequireReady(f.InspectEvent());
  RequirePending(f.AdvanceEvent());
  REQUIRE(f.Gold() == 19);
  REQUIRE(EventField(f.Owner(), "eventderived", "eventbase/suffixes") == 1);
  REQUIRE(EventField(f.Owner(), "eventobserver", "eventobserver/order") == 0);
  REQUIRE(OnlyLive(QuestJournal::JobState(f.Owner()))->handler->cursor ==
          "done");
  REQUIRE(f.DrainEvent().status == Status::Completed);
  REQUIRE(f.Gold() == 19);
  REQUIRE(EventField(f.Owner(), "eventderived", "eventbase/prefixes") == 1);
  REQUIRE(EventField(f.Owner(), "eventderived", "eventbase/suffixes") == 1);
  REQUIRE(EventField(f.Owner(), "eventobserver", "eventobserver/order") ==
          5123);
  REQUIRE(EventField(f.Owner(), "eventobserver", "eventobserver/removed") ==
          1);
  REQUIRE(EventField(f.Owner(), "eventobserver", "eventobserver/selected") ==
          7);
  const auto complete = QuestJournal::JobState(f.Owner()).completed.back();
  REQUIRE(complete.eventHead == complete.events.size());
  REQUIRE(complete.events.size() == 4);
  REQUIRE(complete.completions.at(token).kind ==
          QuestJobState::Completion::Kind::None);
  const auto before = QuestJournal::Snapshot(f.Owner());
  REQUIRE(f.SubmitEvent().status == Status::Completed);
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
}

TEST_CASE(
  "An inventory handler's child timer preserves FIFO and durable unwind",
  "[Quest][QuestJobs][InventoryHandlers][ChildStages]")
{
  EventFixture f(true, "WaitMenuMode");
  f.SubmitEvent();
  f.SelectFirstHandler();
  RequirePending(f.AdvanceEvent());
  REQUIRE(f.Gold() == 17);
  auto job = OnlyLive(QuestJournal::JobState(f.Owner()));
  REQUIRE(job->handler->cursor == "waiting");
  REQUIRE(job->handler->awaitKind == "stage");
  REQUIRE(job->children.size() == 1);
  REQUIRE(job->children.back().cursor == "begin");
  const auto childToken = job->handler->token;
  REQUIRE(EventField(f.Owner(), "eventobserver", "eventobserver/order") == 0);
  f.AckLatest();
  RequireReady(f.InspectEvent());
  REQUIRE_THROWS(f.Jobs().Resume(f.Owner(), kEventCommand, childToken, true,
                                 f.content.metadata, f.content.Scripts()));
  // An already selected alias remains authorized throughout its child work.
  const auto checkpoint = QuestJournal::Snapshot(f.Owner());
  f.Owner().SetPropertyValueDump(QuestJournal::kEnabled,
                                 "[\"2d512:Skyrim.esm\"]", false, false);
  REQUIRE_THROWS(f.AdvanceEvent());
  REQUIRE(f.Gold() == 17);
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == checkpoint);
  f.Owner().SetPropertyValueDump(QuestJournal::kEnabled,
                                 "[\"4e50d:Skyrim.esm\",\"2d512:Skyrim.esm\"]",
                                 false, false);
  RequirePending(f.AdvanceEvent());
  REQUIRE(f.Gold() == 21);
  f.AckLatest();
  RequireSleeping(f.InspectEvent());
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations").size() ==
          2);
  REQUIRE(EventField(f.Owner(), "eventobserver", "eventobserver/order") == 0);
  f.time->steadyMs += 1000;
  RequirePending(f.AdvanceEvent());
  REQUIRE(f.Gold() == 27);
  REQUIRE(EventField(f.Owner(), "eventderived", "eventbase/suffixes") == 0);
  job = OnlyLive(QuestJournal::JobState(f.Owner()));
  REQUIRE(job->children.back().cursor == "done");
  f.Tick();
  const auto failed = f.storage->submissions.size() - 1;
  const auto bytes = Serialized(f.storage->submissions[failed].forms);
  f.storage->Complete(
    failed, std::make_exception_ptr(std::runtime_error("child receipt")));
  f.Tick();
  REQUIRE(Serialized(f.storage->submissions.back().forms) == bytes);
  RequirePending(f.AdvanceEvent());
  REQUIRE(f.Gold() == 27);
  f.AckLatest();
  RequirePending(f.AdvanceEvent());
  REQUIRE(f.Gold() == 30);
  REQUIRE(EventField(f.Owner(), "eventderived", "eventbase/suffixes") == 1);
  REQUIRE(EventField(f.Owner(), "eventobserver", "eventobserver/order") == 0);
  job = OnlyLive(QuestJournal::JobState(f.Owner()));
  REQUIRE(job->children.empty());
  REQUIRE(job->handler->cursor == "done");
  REQUIRE(job->completions.at(childToken).value);
  REQUIRE(f.DrainEvent().status == Status::Completed);
  REQUIRE(f.Gold() == 30);
  REQUIRE(EventField(f.Owner(), "eventderived", "eventbase/prefixes") == 1);
  REQUIRE(EventField(f.Owner(), "eventobserver", "eventobserver/order") ==
          52463);
  const auto done = QuestJournal::Snapshot(f.Owner());
  REQUIRE(f.Jobs()
            .Resume(f.Owner(), kEventCommand, childToken, true,
                    f.content.metadata, f.content.Scripts())
            .status == Status::Completed);
  REQUIRE_THROWS(f.Jobs().Resume(f.Owner(), kEventCommand, childToken, false,
                                 f.content.metadata, f.content.Scripts()));
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == done);
}

TEST_CASE("Inherited inventory handlers recover from SQLite before retirement",
          "[Quest][QuestJobs][InventoryHandlers][SQLite]")
{
  SqliteFiles files;
  EventProgram content;
  std::string token;
  Json selected;
  {
    PartOne server;
    AttachDatabase(server, files);
    Initialize(server);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    content.Enable(owner);
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1000000, 5000 }; });
    RequirePending(jobs.Submit(owner, kEventCommand, kParentQuest, 10,
                               content.metadata, content.Scripts()));
    RequireSleeping(DrainStored(server, owner, kEventCommand, content.metadata,
                                content.Scripts()));
    const auto job = OnlyLive(QuestJournal::JobState(owner));
    REQUIRE(job->handler);
    REQUIRE(job->handler->handlerIdentity->rootScript == "EventDerived");
    REQUIRE(job->handler->handlerIdentity->implementation == "EventBase");
    REQUIRE(job->handler->handlerIdentity->state == "busy");
    token = job->handler->token;
    selected = QuestJournal::ScriptState(owner).at("continuations").at(token);
    REQUIRE(selected.at("rootScript") == "EventDerived");
    REQUIRE(selected.at("script") == "EventBase");
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 17);
    REQUIRE(EventField(owner, "eventderived", "eventbase/prefixes") == 1);
    REQUIRE(EventField(owner, "eventobserver", "eventobserver/order") == 0);
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1002000, 100 }; });
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    RequireReady(jobs.Inspect(owner, kEventCommand));
    REQUIRE(QuestJournal::ScriptState(owner).at("continuations").at(token) ==
            selected);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 17);
    REQUIRE(EventField(owner, "eventderived", "eventbase/prefixes") == 1);
    const auto before = QuestJournal::Snapshot(owner);
    EventProgram changed;
    changed.base->objectTable[0]
      .states[0]
      .functions[0]
      .function.code.instructions[3]
      .args[2] = VarValue(2);
    REQUIRE_THROWS(
      jobs.Advance(owner, kEventCommand, changed.metadata, changed.Scripts()));
    REQUIRE(QuestJournal::Snapshot(owner) == before);
    REQUIRE_THROWS(jobs.Resume(owner, kEventCommand, token, false,
                               content.metadata, content.Scripts()));
    RequirePending(
      jobs.Advance(owner, kEventCommand, content.metadata, content.Scripts()));
    RequireReady(WaitForDurable(server, owner, kEventCommand));
    REQUIRE(OnlyLive(QuestJournal::JobState(owner))->handler->cursor ==
            "done");
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 20);
    REQUIRE(EventField(owner, "eventderived", "eventbase/prefixes") == 1);
    REQUIRE(EventField(owner, "eventderived", "eventbase/suffixes") == 1);
    REQUIRE(EventField(owner, "eventobserver", "eventobserver/order") == 0);
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    REQUIRE(OnlyLive(QuestJournal::JobState(owner))->handler->cursor ==
            "done");
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 20);
    REQUIRE(DrainStored(server, owner, kEventCommand, content.metadata,
                        content.Scripts())
              .status == Status::Completed);
    REQUIRE(EventField(owner, "eventderived", "eventbase/prefixes") == 1);
    REQUIRE(EventField(owner, "eventderived", "eventbase/suffixes") == 1);
    REQUIRE(EventField(owner, "eventobserver", "eventobserver/order") == 523);
    REQUIRE(EventField(owner, "eventobserver", "eventobserver/selected") == 7);
    REQUIRE(QuestJournal::ScriptState(owner).at("continuations").empty());
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    const auto before = QuestJournal::Snapshot(owner);
    REQUIRE(server.worldState.GetQuestJobs()
              .Submit(owner, kEventCommand, kParentQuest, 10, content.metadata,
                      content.Scripts())
              .status == Status::Completed);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 20);
    REQUIRE(QuestJournal::Snapshot(owner) == before);
  }
}

TEST_CASE("Inventory handlers share the stage instruction budget",
          "[Quest][QuestJobs][InventoryHandlers][Rollback]")
{
  EventFixture f;
  const auto addLoop = [](FunctionInfo& function) {
    function.locals.push_back({ "loop", "Int" });
    function.locals.push_back({ "again", "Bool" });
    const std::vector<FunctionCode::Instruction> loop{
      { FunctionCode::kOp_Assign, { Name("loop"), VarValue(0) } },
      { FunctionCode::kOp_IAdd, { Name("loop"), Name("loop"), VarValue(1) } },
      { FunctionCode::kOp_CompareLT,
        { Name("again"), Name("loop"), VarValue(20000) } },
      { FunctionCode::kOp_JumpT, { Name("again"), VarValue(-2) } }
    };
    function.code.instructions.insert(function.code.instructions.begin(),
                                      loop.begin(), loop.end());
  };
  addLoop(
    f.content.program.script->objectTable[0].states[0].functions[0].function);
  addLoop(f.content.base->objectTable[0].states[0].functions[0].function);
  f.SubmitEvent();
  f.SelectFirstHandler();
  REQUIRE(f.Gold() == 15);
  const auto job = OnlyLive(QuestJournal::JobState(f.Owner()));
  REQUIRE(job->instructions > 60000);
  REQUIRE(job->instructions < 65000);
  const auto before = QuestJournal::Snapshot(f.Owner());
  const auto heap = QuestJournal::ScriptState(f.Owner());
  const auto saves = f.storage->submissions.size();
  REQUIRE_THROWS(f.AdvanceEvent());
  REQUIRE(f.Gold() == 15);
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == heap);
  REQUIRE(EventField(f.Owner(), "eventderived", "eventbase/prefixes") == 0);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == saves);
  RequireReady(f.InspectEvent());
}

namespace {
constexpr uint32_t kRetentionQuest = 0x2aa57;
constexpr uint32_t kRetentionNoLogQuest = 0x1075b1;
using RequestId = QuestJobRunner::RequestId;

std::string RequestName(const RequestId& request)
{
  return "quest-request:" + std::to_string(request.generation) + ":" +
    std::to_string(request.sequence);
}

void RequireSameRequest(const RequestId& one, const RequestId& two)
{
  REQUIRE(one.owner == two.owner);
  REQUIRE(one.generation == two.generation);
  REQUIRE(one.sequence == two.sequence);
}

void RequireRetired(const QuestJobRunner::Result& result)
{
  REQUIRE(result.status == Status::Retired);
  REQUIRE_FALSE(result.accepted);
  REQUIRE(result.token.empty());
  REQUIRE(result.key.empty());
}

struct RetentionProgram
{
  Program program;
  QuestJobRunner::MetadataOverrides metadata;
  explicit RetentionProgram(bool await = false, bool fail = false)
  {
    auto reward = Reward(1);
    reward.code.instructions.pop_back();
    if (await) {
      reward.locals.push_back({ "completed", "Bool" });
      reward.code.instructions.push_back(
        { FunctionCode::kOp_CallStatic,
          { VarValue("FixtureAwait"), VarValue("AwaitBool"), Name("completed"),
            VarValue(1), VarValue("retained") } });
      reward.code.instructions.push_back(
        { FunctionCode::kOp_CallMethod,
          { VarValue("AddItem"), Name("target"), Name("temp"), VarValue(3),
            Name("gold"), VarValue(2), VarValue(true) } });
    }
    if (fail)
      reward.code.instructions.push_back(
        { FunctionCode::kOp_CallMethod,
          { VarValue("UnsupportedRetirementAdmission"), Name("self"),
            Name("temp"), VarValue(0) } });
    reward.code.instructions.push_back(
      { FunctionCode::kOp_Return, { VarValue::None() } });
    program.script->objectTable[0].states = { { "",
                                                { { "Reward", reward } } } };
    program.metadata.fragments = { { 10, 0, 1, "JobFixture", "Reward" } };
    metadata[kRetentionQuest] = program.metadata;
    auto noLogs = program.metadata;
    noLogs.fragments.clear();
    metadata[kRetentionNoLogQuest] = noLogs;
  }
  auto Scripts() const { return program.Scripts(); }
  void Enable(MpActor& actor) const
  {
    actor.SetPropertyValueDump(QuestJournal::kEnabled,
                               "[\"2aa57:Skyrim.esm\",\"1075b1:Skyrim.esm\"]",
                               false, false);
  }
};

struct RetentionFixture : Fixture
{
  RetentionProgram content;
  explicit RetentionFixture(bool await = false)
    : content(await)
  {
    content.Enable(Owner());
  }
  QuestJobRunner::Result SubmitRequest(const RequestId& request)
  {
    return Jobs().Submit(Owner(), request, kRetentionQuest, 10,
                         content.metadata, content.Scripts());
  }
  QuestJobRunner::Result DrainRequest(const RequestId& request)
  {
    return Drain(*this, request, content.metadata, content.Scripts());
  }
  RequestId CompleteNext()
  {
    const auto request = Jobs().NextRequest(Owner());
    RequirePending(SubmitRequest(request));
    REQUIRE(DrainRequest(request).status == Status::Completed);
    storage->submissions.clear();
    return request;
  }
};
}

TEST_CASE("Sequenced quest requests retain bounded history beyond 256 rewards",
          "[Quest][QuestJobs][Retention]")
{
  RetentionFixture f;
  const auto first = f.Jobs().NextRequest(f.Owner());
  RequireSameRequest(first, f.Jobs().NextRequest(f.Owner()));
  REQUIRE(first.sequence == 1);
  REQUIRE(first.owner == f.Owner().GetChangeForm().formDesc.ToString());
  std::vector<RequestId> requests;
  for (uint64_t index = 0; index < 300; ++index) {
    INFO("completed request " << index + 1);
    const auto request = f.CompleteNext();
    requests.push_back(request);
    REQUIRE(request.sequence == index + 1);
    REQUIRE(f.Gold() == 11 + index);
    const auto jobs = QuestJournal::JobState(f.Owner());
    REQUIRE_FALSE(OnlyLive(jobs));
    REQUIRE(jobs.completed.size() == std::min<uint64_t>(index + 1, 32));
    REQUIRE(std::all_of(jobs.completed.begin(), jobs.completed.end(),
                        [](const auto& job) { return job.sequenced; }));
    REQUIRE(MpChangeForm::ToJson(f.Owner().GetChangeForm()).dump().size() <
            256 * 1024);
  }
  const auto jobs = QuestJournal::JobState(f.Owner());
  REQUIRE(jobs.retiredThrough == 268);
  REQUIRE(jobs.nextJob == 301);
  REQUIRE(jobs.completed.front().id == 269);
  REQUIRE(jobs.completed.back().id == 300);
  REQUIRE(QuestJournal::Get(f.Owner(), kRetentionQuest).at("stages") ==
          Json::array({ 10 }));
  const auto before = QuestJournal::Snapshot(f.Owner());
  const auto beforeJobs = QuestJournal::JobState(f.Owner()).Save();
  auto wrongOwner = first;
  wrongOwner.owner = "14:Skyrim.esm";
  REQUIRE_THROWS(f.Jobs().Inspect(f.Owner(), wrongOwner));
  REQUIRE_THROWS(f.SubmitRequest(wrongOwner));
  auto wrongGeneration = first;
  ++wrongGeneration.generation;
  REQUIRE_THROWS(f.Jobs().Inspect(f.Owner(), wrongGeneration));
  REQUIRE_THROWS(f.SubmitRequest(wrongGeneration));
  RequireRetired(f.Jobs().Inspect(f.Owner(), first));
  RequireRetired(f.SubmitRequest(first));
  RequireRetired(f.Jobs().Advance(f.Owner(), first, f.content.metadata,
                                  f.content.Scripts()));
  RequireRetired(f.Jobs().Resume(f.Owner(), first, "forgotten-token", false,
                                 f.content.metadata, f.content.Scripts()));
  REQUIRE_NOTHROW(f.Jobs().Retry(f.Owner(), first));
  RequireRetired(f.Jobs().Inspect(f.Owner(), requests[267]));
  REQUIRE(f.SubmitRequest(requests[268]).status == Status::Completed);
  REQUIRE(f.SubmitRequest(requests.back()).status == Status::Completed);
  REQUIRE(f.Gold() == 310);
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == beforeJobs);
  f.Tick();
  REQUIRE(f.storage->submissions.empty());
}

TEST_CASE("Permanent legacy receipts cannot exhaust the sequenced namespace",
          "[Quest][QuestJobs][Retention]")
{
  RetentionFixture f;
  auto future = f.Jobs().NextRequest(f.Owner());
  future.sequence = 257;
  const auto sameText = RequestName(future);
  for (uint64_t index = 0; index < 256; ++index) {
    const std::string command =
      index == 0 ? sameText : "permanent-legacy-" + std::to_string(index);
    RequirePending(f.Jobs().Submit(f.Owner(), command, kRetentionNoLogQuest, 0,
                                   f.content.metadata, f.content.Scripts()));
    REQUIRE(
      Drain(f, command, f.content.metadata, f.content.Scripts()).status ==
      Status::Completed);
    f.storage->submissions.clear();
  }
  REQUIRE(f.Gold() == 10);
  REQUIRE(QuestJournal::JobState(f.Owner()).completed.size() == 256);
  RequireSameRequest(future, f.Jobs().NextRequest(f.Owner()));
  REQUIRE_THROWS(f.Jobs().Submit(f.Owner(), "legacy-over-cap",
                                 kRetentionNoLogQuest, 0, f.content.metadata,
                                 f.content.Scripts()));
  for (size_t index = 0; index < 34; ++index)
    f.CompleteNext();
  const auto jobs = QuestJournal::JobState(f.Owner());
  REQUIRE(jobs.completed.size() == 288);
  REQUIRE(jobs.retiredThrough == 258);
  REQUIRE(std::count_if(jobs.completed.begin(), jobs.completed.end(),
                        [](const auto& job) { return !job.sequenced; }) ==
          256);
  REQUIRE(f.Gold() == 44);
  const auto before = QuestJournal::Snapshot(f.Owner());
  RequireRetired(f.Jobs().Inspect(f.Owner(), future));
  REQUIRE(f.Jobs().Inspect(f.Owner(), sameText).status == Status::Completed);
  const auto legacy =
    f.Jobs().Submit(f.Owner(), sameText, kRetentionNoLogQuest, 0,
                    f.content.metadata, f.content.Scripts());
  REQUIRE(legacy.status == Status::Completed);
  REQUIRE_FALSE(legacy.accepted);
  auto legacyId = future;
  legacyId.sequence = 1;
  REQUIRE_THROWS(f.Jobs().Inspect(f.Owner(), legacyId));
  REQUIRE_THROWS(f.SubmitRequest(legacyId));
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
  REQUIRE(f.Gold() == 44);
}

TEST_CASE("Retirement publishes only with its exact admission receipt",
          "[Quest][QuestJobs][Retention][SaveReceipt][Rollback]")
{
  RetentionFixture f;
  std::vector<RequestId> old;
  for (size_t index = 0; index < 33; ++index)
    old.push_back(f.CompleteNext());
  RequireRetired(f.Jobs().Inspect(f.Owner(), old[0]));
  REQUIRE(f.Jobs().Inspect(f.Owner(), old[1]).status == Status::Completed);
  const auto request = f.Jobs().NextRequest(f.Owner());
  RetentionProgram broken(false, true);
  const auto before = QuestJournal::Snapshot(f.Owner());
  const auto beforeJobs = QuestJournal::JobState(f.Owner()).Save();
  const auto beforeHeap = QuestJournal::ScriptState(f.Owner());
  REQUIRE_THROWS(f.Jobs().Submit(f.Owner(), request, kRetentionQuest, 10,
                                 broken.metadata, broken.Scripts()));
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == beforeJobs);
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == beforeHeap);
  REQUIRE(f.Gold() == 43);
  RequireSameRequest(request, f.Jobs().NextRequest(f.Owner()));
  REQUIRE(f.Jobs().Inspect(f.Owner(), old[1]).status == Status::Completed);
  RequirePending(f.SubmitRequest(request));
  REQUIRE(f.Gold() == 44);
  RequireRetired(f.Jobs().Inspect(f.Owner(), old[0]));
  RequirePending(f.Jobs().Inspect(f.Owner(), old[1]));
  RequirePending(f.SubmitRequest(old[1]));
  REQUIRE(f.Jobs().Inspect(f.Owner(), old.back()).status == Status::Completed);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  const auto bytes = Serialized(f.storage->submissions[0].forms);
  f.storage->Complete(
    0, std::make_exception_ptr(std::runtime_error("retirement")));
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 2);
  REQUIRE(Serialized(f.storage->submissions[1].forms) == bytes);
  RequirePending(f.Jobs().Inspect(f.Owner(), old[1]));
  RequireRetired(f.Jobs().Inspect(f.Owner(), old[0]));
  REQUIRE(f.Gold() == 44);
  f.storage->Complete(0);
  f.Tick();
  RequirePending(f.Jobs().Inspect(f.Owner(), old[1]));
  f.storage->Complete(1);
  RequirePending(f.Jobs().Inspect(f.Owner(), old[1]));
  f.Tick();
  RequireRetired(f.Jobs().Inspect(f.Owner(), old[1]));
  RequireReady(f.Jobs().Inspect(f.Owner(), request));
  REQUIRE(f.DrainRequest(request).status == Status::Completed);
  const auto next = f.Jobs().NextRequest(f.Owner());
  RequirePending(f.SubmitRequest(next));
  RequireRetired(f.Jobs().Inspect(f.Owner(), old[0]));
  RequireRetired(f.Jobs().Inspect(f.Owner(), old[1]));
  RequirePending(f.Jobs().Inspect(f.Owner(), old[2]));
  REQUIRE(f.DrainRequest(next).status == Status::Completed);
  REQUIRE(f.Gold() == 45);
}

TEST_CASE("Prospective quest request keys reject gaps and conflicting reuse",
          "[Quest][QuestJobs][Retention][Rollback]")
{
  RetentionFixture f(true);
  const auto request = f.Jobs().NextRequest(f.Owner());
  RequireSameRequest(request, f.Jobs().NextRequest(f.Owner()));
  const auto before = QuestJournal::Snapshot(f.Owner());
  std::vector<RequestId> invalid;
  auto wrong = request;
  wrong.owner = "14:Skyrim.esm";
  invalid.push_back(wrong);
  wrong = request;
  ++wrong.generation;
  invalid.push_back(wrong);
  wrong = request;
  ++wrong.sequence;
  invalid.push_back(wrong);
  wrong.sequence = 0;
  invalid.push_back(wrong);
  wrong.sequence = 9007199254740992ULL;
  invalid.push_back(wrong);
  for (const auto& key : invalid) {
    REQUIRE_THROWS(f.SubmitRequest(key));
    REQUIRE(QuestJournal::Snapshot(f.Owner()) == before);
    REQUIRE(f.Gold() == 10);
  }
  RequirePending(f.SubmitRequest(request));
  REQUIRE(f.Gold() == 11);
  const auto waiting = f.DrainRequest(request);
  REQUIRE(waiting.status == Status::Waiting);
  REQUIRE(waiting.key == "retained");
  REQUIRE(f.SubmitRequest(request).token == waiting.token);
  const auto checkpoint = QuestJournal::Snapshot(f.Owner());
  REQUIRE_THROWS(f.Jobs().Submit(f.Owner(), request, kRetentionQuest, 20,
                                 f.content.metadata, f.content.Scripts()));
  REQUIRE_THROWS(f.Jobs().Submit(f.Owner(), request, kRetentionNoLogQuest, 0,
                                 f.content.metadata, f.content.Scripts()));
  auto following = request;
  ++following.sequence;
  REQUIRE_THROWS(f.SubmitRequest(following));
  RetentionProgram changed;
  REQUIRE_THROWS(f.Jobs().Resume(f.Owner(), request, waiting.token, true,
                                 changed.metadata, changed.Scripts()));
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == checkpoint);
  REQUIRE(f.Gold() == 11);
  const auto phase = f.Owner().GetWorldInstance();
  f.Owner().SetWorldInstance("campaign:" + std::to_string(kOwner) + ":other");
  REQUIRE_THROWS(f.SubmitRequest(request));
  REQUIRE_THROWS(f.Jobs().Resume(f.Owner(), request, waiting.token, true,
                                 f.content.metadata, f.content.Scripts()));
  f.Owner().SetWorldInstance(phase);
  RequirePending(f.Jobs().Resume(f.Owner(), request, waiting.token, true,
                                 f.content.metadata, f.content.Scripts()));
  REQUIRE(f.Gold() == 13);
  REQUIRE(f.DrainRequest(request).status == Status::Completed);
  const auto complete = QuestJournal::Snapshot(f.Owner());
  REQUIRE(f.Jobs()
            .Resume(f.Owner(), request, waiting.token, true,
                    f.content.metadata, f.content.Scripts())
            .status == Status::Completed);
  REQUIRE_THROWS(f.Jobs().Resume(f.Owner(), request, waiting.token, false,
                                 f.content.metadata, f.content.Scripts()));
  REQUIRE_THROWS(f.Jobs().Submit(f.Owner(), request, kRetentionQuest, 20,
                                 f.content.metadata, f.content.Scripts()));
  REQUIRE(QuestJournal::Snapshot(f.Owner()) == complete);
  REQUIRE(f.Gold() == 13);
}

TEST_CASE("SQLite restart preserves both sides of a quest retirement receipt",
          "[Quest][QuestJobs][Retention][SQLite]")
{
  SqliteFiles files;
  RetentionProgram immediate, waiting(true);
  RequestId oldest, admitted;
  std::string token;
  {
    PartOne server;
    AttachDatabase(server, files);
    Initialize(server);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    immediate.Enable(owner);
    auto& jobs = server.worldState.GetQuestJobs();
    oldest = jobs.NextRequest(owner);
    for (size_t index = 0; index < 32; ++index) {
      const auto request = jobs.NextRequest(owner);
      RequirePending(jobs.Submit(owner, request, kRetentionQuest, 10,
                                 immediate.metadata, immediate.Scripts()));
      REQUIRE(DrainStored(server, owner, request, immediate.metadata,
                          immediate.Scripts())
                .status == Status::Completed);
    }
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 42);
    REQUIRE(jobs.Inspect(owner, oldest).status == Status::Completed);
    admitted = jobs.NextRequest(owner);
    REQUIRE(admitted.sequence == 33);
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = server.worldState.GetQuestJobs();
    RequireSameRequest(jobs.NextRequest(owner), admitted);
    REQUIRE(jobs.Inspect(owner, oldest).status == Status::Completed);
    RequirePending(jobs.Submit(owner, admitted, kRetentionQuest, 10,
                               waiting.metadata, waiting.Scripts()));
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 43);
    RequirePending(jobs.Inspect(owner, oldest));
    // No Tick: neither the retirement nor the new prefix reached SQLite.
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = server.worldState.GetQuestJobs();
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 42);
    RequireSameRequest(jobs.NextRequest(owner), admitted);
    REQUIRE(jobs.Inspect(owner, oldest).status == Status::Completed);
    REQUIRE(QuestJournal::JobState(owner).retiredThrough == 0);
    RequirePending(jobs.Submit(owner, admitted, kRetentionQuest, 10,
                               waiting.metadata, waiting.Scripts()));
    const auto result = WaitForDurable(server, owner, admitted);
    REQUIRE(result.status == Status::Waiting);
    REQUIRE(result.key == "retained");
    token = result.token;
    RequireRetired(jobs.Inspect(owner, oldest));
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 43);
    REQUIRE(QuestJournal::JobState(owner).retiredThrough == oldest.sequence);
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = server.worldState.GetQuestJobs();
    RequireRetired(jobs.Inspect(owner, oldest));
    RequireRetired(jobs.Submit(owner, oldest, kRetentionQuest, 10,
                               immediate.metadata, immediate.Scripts()));
    REQUIRE(jobs.Inspect(owner, admitted).token == token);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 43);
    RequirePending(jobs.Resume(owner, admitted, token, true, waiting.metadata,
                               waiting.Scripts()));
    REQUIRE(
      DrainStored(server, owner, admitted, waiting.metadata, waiting.Scripts())
        .status == Status::Completed);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 45);
    REQUIRE(jobs
              .Resume(owner, admitted, token, true, waiting.metadata,
                      waiting.Scripts())
              .status == Status::Completed);
    REQUIRE_THROWS(jobs.Resume(owner, admitted, token, false, waiting.metadata,
                               waiting.Scripts()));
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = server.worldState.GetQuestJobs();
    const auto before = QuestJournal::Snapshot(owner);
    RequireRetired(
      jobs.Advance(owner, oldest, immediate.metadata, immediate.Scripts()));
    REQUIRE(jobs.Inspect(owner, admitted).status == Status::Completed);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 45);
    REQUIRE(QuestJournal::Snapshot(owner) == before);
  }
}

TEST_CASE(
  "Automatic typed requests retain their namespace through timer recovery",
  "[Quest][QuestJobs][Retention][SQLite][Scheduler]")
{
  SqliteFiles files;
  TimerProgram content;
  RequestId request;
  {
    PartOne server;
    AttachDatabase(server, files);
    Initialize(server);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    content.Enable(owner);
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1000000, 5000 }; });
    jobs.SetSchedulerFixtureContent(content.metadata, content.Scripts());
    request = jobs.NextRequest(owner);
    RequirePending(jobs.Submit(owner, request, kTimerQuest, 5,
                               content.metadata, content.Scripts(), true));
    RequireSleeping(WaitForDurable(server, owner, request));
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 12);
    REQUIRE(OnlyLive(QuestJournal::JobState(owner))->sequenced);
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1002000, 100 }; });
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    RequireReady(jobs.Inspect(owner, request));
    server.worldState.Tick();
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 12);
    jobs.SetSchedulerFixtureContent(content.metadata, content.Scripts());
    const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (jobs.Inspect(owner, request).status != Status::Completed &&
           std::chrono::steady_clock::now() < deadline) {
      server.worldState.Tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(jobs.Inspect(owner, request).status == Status::Completed);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 15);
    const auto command = RequestName(request);
    REQUIRE_THROWS(jobs.Inspect(owner, command));
    RequirePending(jobs.Submit(owner, command, kTimerQuest, 5,
                               content.metadata, content.Scripts()));
    REQUIRE(WaitForDurable(server, owner, command).status ==
            Status::Completed);
    REQUIRE_FALSE(jobs.Inspect(owner, command).accepted);
    REQUIRE(jobs.Inspect(owner, request).accepted);
    REQUIRE(QuestJournal::JobState(owner).completed.size() == 2);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 15);
    REQUIRE_NOTHROW(jobs.Retry(owner, request));
  }
}

namespace {
struct ConcurrentProgram
{
  Program program;
  QuestJobRunner::MetadataOverrides metadata;
  ConcurrentProgram(bool secondTimer = true, bool reassignArray = false,
                    bool failSecond = false)
  {
    auto& object = program.script->objectTable[0];
    object.variables = { { "ready", "Bool", 0, VarValue(false) },
                         { "scalar", "Int", 0, VarValue(7) },
                         { "shared", "Int[]", 0, VarValue::None() },
                         { "aRuns", "Int", 0, VarValue(0) },
                         { "bRuns", "Int", 0, VarValue(0) },
                         { "aCaptured", "Int", 0, VarValue(0) },
                         { "aSeen", "Int", 0, VarValue(0) },
                         { "aDone", "Bool", 0, VarValue(false) },
                         { "bDone", "Bool", 0, VarValue(false) } };
    auto first = Reward(2);
    first.locals.insert(first.locals.end(),
                        { { "captured", "Int" },
                          { "link", "Int[]" },
                          { "seen", "Int" },
                          { "total", "Int" } });
    first.code.instructions.pop_back();
    const std::vector<FunctionCode::Instruction> firstBody{
      { FunctionCode::kOp_IAdd,
        { Name("aRuns"), Name("aRuns"), VarValue(1) } },
      { FunctionCode::kOp_Assign, { Name("captured"), Name("scalar") } },
      { FunctionCode::kOp_ArrayCreate, { Name("shared"), VarValue(1) } },
      { FunctionCode::kOp_ArraySetElement,
        { Name("shared"), VarValue(0), VarValue(3) } },
      { FunctionCode::kOp_Assign, { Name("link"), Name("shared") } },
      { FunctionCode::kOp_JumpT, { Name("ready"), VarValue(3) } },
      { FunctionCode::kOp_CallStatic,
        { VarValue("Utility"), VarValue("Wait"), Name("temp"), VarValue(1),
          VarValue(1.0) } },
      { FunctionCode::kOp_Jump, { VarValue(-2) } },
      { FunctionCode::kOp_ArrayGetElement,
        { Name("seen"), Name("link"), VarValue(0) } },
      { FunctionCode::kOp_IAdd,
        { Name("total"), Name("captured"), Name("seen") } },
      { FunctionCode::kOp_CallMethod,
        { VarValue("AddItem"), Name("target"), Name("temp"), VarValue(3),
          Name("gold"), Name("total"), VarValue(true) } },
      { FunctionCode::kOp_Assign, { Name("aCaptured"), Name("captured") } },
      { FunctionCode::kOp_Assign, { Name("aSeen"), Name("seen") } },
      { FunctionCode::kOp_Assign, { Name("aDone"), VarValue(true) } },
      { FunctionCode::kOp_Return, { VarValue::None() } }
    };
    first.code.instructions.insert(first.code.instructions.end(),
                                   firstBody.begin(), firstBody.end());
    auto second = Reward(3);
    second.code.instructions.pop_back();
    second.code.instructions.insert(
      second.code.instructions.end(),
      { { FunctionCode::kOp_IAdd,
          { Name("bRuns"), Name("bRuns"), VarValue(1) } },
        { FunctionCode::kOp_Assign, { Name("scalar"), VarValue(99) } },
        { FunctionCode::kOp_ArraySetElement,
          { Name("shared"), VarValue(0), VarValue(9) } },
        { FunctionCode::kOp_Assign, { Name("ready"), VarValue(true) } } });
    if (reassignArray)
      second.code.instructions.insert(
        second.code.instructions.end(),
        { { FunctionCode::kOp_ArrayCreate, { Name("shared"), VarValue(1) } },
          { FunctionCode::kOp_ArraySetElement,
            { Name("shared"), VarValue(0), VarValue(99) } } });
    if (secondTimer) {
      second.code.instructions.push_back(
        { FunctionCode::kOp_CallStatic,
          { VarValue("Utility"), VarValue("WaitMenuMode"), Name("temp"),
            VarValue(1), VarValue(2.0) } });
      second.code.instructions.push_back(
        { FunctionCode::kOp_CallMethod,
          { VarValue("AddItem"), Name("target"), Name("temp"), VarValue(3),
            Name("gold"), VarValue(4), VarValue(true) } });
    }
    if (failSecond)
      second.code.instructions.push_back(
        { FunctionCode::kOp_CallMethod,
          { VarValue("UnsupportedConcurrentSuffix"), Name("self"),
            Name("temp"), VarValue(0) } });
    second.code.instructions.push_back(
      { FunctionCode::kOp_Assign, { Name("bDone"), VarValue(true) } });
    second.code.instructions.push_back(
      { FunctionCode::kOp_Return, { VarValue::None() } });
    object.states = {
      { "",
        { { "First", first }, { "Second", second }, { "Reward", Reward(1) } } }
    };
    program.metadata.fragments = { { 10, 0, 1, "JobFixture", "First" },
                                   { 20, 0, 1, "JobFixture", "Second" } };
    metadata[kParentQuest] = program.metadata;
    auto repeated = program.metadata;
    repeated.fragments = { { 10, 0, 1, "JobFixture", "Reward" } };
    metadata[kRetentionQuest] = repeated;
  }
  auto Scripts() const { return program.Scripts(); }
  void Enable(MpActor& actor) const
  {
    actor.SetPropertyValueDump(QuestJournal::kEnabled,
                               "[\"4e50d:Skyrim.esm\",\"2aa57:Skyrim.esm\"]",
                               false, false);
  }
};

struct ConcurrentFixture : Fixture
{
  ConcurrentProgram content;
  std::shared_ptr<QuestJobTime> time =
    std::make_shared<QuestJobTime>(QuestJobTime{ 1000000, 5000 });
  ConcurrentFixture(bool secondTimer = true, bool reassignArray = false,
                    bool failSecond = false)
    : content(secondTimer, reassignArray, failSecond)
  {
    content.Enable(Owner());
    Jobs().SetClock([time = time] { return *time; });
  }
  QuestJobRunner::Result SubmitRoot(const RequestId& request, uint32_t quest,
                                    uint16_t stage, bool automatic = false)
  {
    return Jobs().Submit(Owner(), request, quest, stage, content.metadata,
                         content.Scripts(), automatic);
  }
  QuestJobRunner::Result AdvanceRoot(const RequestId& request)
  {
    return Jobs().Advance(Owner(), request, content.metadata,
                          content.Scripts());
  }
  QuestJobRunner::Result InspectRoot(const RequestId& request)
  {
    return Jobs().Inspect(Owner(), request);
  }
  QuestJobRunner::Result DrainRoot(const RequestId& request)
  {
    return Drain(*this, request, content.metadata, content.Scripts());
  }
  void AckLatest()
  {
    Tick();
    REQUIRE_FALSE(storage->submissions.empty());
    storage->Complete(storage->submissions.size() - 1);
    Tick();
  }
  QuestJobState::Job Live(const RequestId& request)
  {
    const auto jobs = QuestJournal::JobState(Owner());
    const auto* job = jobs.FindLive(request.sequence);
    REQUIRE(job);
    return *job;
  }
};
}

TEST_CASE(
  "Independent stage timers observe shared fields but retain their locals",
  "[Quest][QuestJobs][ConcurrentStacks][SaveReceipt]")
{
  const bool reassign = GENERATE(false, true);
  ConcurrentFixture f(true, reassign);
  const auto first = f.Jobs().NextRequest(f.Owner());
  RequirePending(f.SubmitRoot(first, kParentQuest, 10));
  RequireSleeping(f.DrainRoot(first));
  REQUIRE(f.Gold() == 12);
  // Poll once before the other root changes ready. Only the timer rotates;
  // the stage prefix and its saved scalar/array locals remain the same.
  const auto initialToken = f.Live(first).token;
  f.time->steadyMs += 1000;
  RequirePending(f.AdvanceRoot(first));
  RequireSleeping(f.DrainRoot(first));
  const auto firstJob = f.Live(first);
  REQUIRE(firstJob.token != initialToken);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "aruns").at("value") == 1);
  const auto firstFrame = QuestJournal::ScriptState(f.Owner())
                            .at("continuations")
                            .at(firstJob.token);
  const auto originalArray = ChildField(f.Owner(), kParentQuest, "shared");
  const auto second = f.Jobs().NextRequest(f.Owner());
  ConcurrentProgram incompatible(true, !reassign);
  const auto before = QuestJournal::JobState(f.Owner()).Save();
  REQUIRE_THROWS(f.Jobs().Submit(f.Owner(), second, kParentQuest, 20,
                                 incompatible.metadata,
                                 incompatible.Scripts()));
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == before);
  REQUIRE(f.Gold() == 12);
  RequirePending(f.SubmitRoot(second, kParentQuest, 20));
  REQUIRE(f.Gold() == 15);
  REQUIRE(QuestJournal::JobState(f.Owner()).live.size() == 2);
  REQUIRE(f.Live(first).environmentHash == f.Live(second).environmentHash);
  REQUIRE(QuestJournal::ScriptState(f.Owner())
            .at("continuations")
            .at(firstJob.token) == firstFrame);
  REQUIRE(f.Live(first).instructions == firstJob.instructions);
  REQUIRE((ChildField(f.Owner(), kParentQuest, "shared") == originalArray) ==
          !reassign);
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("arrays").size() ==
          (reassign ? 2 : 1));
  f.time->steadyMs += 1000;
  RequireReady(f.InspectRoot(first));
  RequirePending(f.InspectRoot(second));
  const auto pending = QuestJournal::ScriptState(f.Owner());
  REQUIRE_THROWS(f.AdvanceRoot(first));
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == pending);
  REQUIRE(f.Gold() == 15);
  f.Tick();
  const auto failed = f.storage->submissions.size() - 1;
  const auto bytes = Serialized(f.storage->submissions[failed].forms);
  f.storage->Complete(
    failed, std::make_exception_ptr(std::runtime_error("second root")));
  f.Tick();
  REQUIRE(Serialized(f.storage->submissions.back().forms) == bytes);
  RequireReady(f.InspectRoot(first));
  RequirePending(f.InspectRoot(second));
  REQUIRE_THROWS(f.AdvanceRoot(first));
  f.storage->Complete(failed);
  f.Tick();
  RequirePending(f.InspectRoot(second));
  f.AckLatest();
  RequireReady(f.InspectRoot(first));
  RequireSleeping(f.InspectRoot(second));
  const auto secondJob = f.Live(second);
  const auto secondFrame = QuestJournal::ScriptState(f.Owner())
                             .at("continuations")
                             .at(secondJob.token);
  REQUIRE(firstJob.token != secondJob.token);
  REQUIRE_THROWS(f.Jobs().Resume(f.Owner(), second, firstJob.token, true,
                                 f.content.metadata, f.content.Scripts()));
  RequirePending(f.AdvanceRoot(first));
  REQUIRE(f.Gold() == 31);
  RequireSleeping(f.InspectRoot(second));
  REQUIRE(QuestJournal::ScriptState(f.Owner())
            .at("continuations")
            .at(secondJob.token) == secondFrame);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "acaptured").at("value") == 7);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "aseen").at("value") == 9);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "scalar").at("value") == 99);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "bruns").at("value") == 1);
  REQUIRE(f.DrainRoot(first).status == Status::Completed);
  REQUIRE(f.Live(second).instructions == secondJob.instructions);
  f.time->steadyMs += 1000;
  RequireReady(f.InspectRoot(second));
  RequirePending(f.AdvanceRoot(second));
  REQUIRE(f.DrainRoot(second).status == Status::Completed);
  REQUIRE(f.Gold() == 35);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "adone").at("value") == true);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "bdone").at("value") == true);
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations").empty());
  REQUIRE(QuestJournal::Get(f.Owner(), kParentQuest).at("stages") ==
          Json::array({ 10, 20 }));
}

TEST_CASE(
  "SQLite restores independent timers and the campaign's shared array heap",
  "[Quest][QuestJobs][ConcurrentStacks][SQLite]")
{
  SqliteFiles files;
  ConcurrentProgram content(true, true);
  RequestId first, second;
  std::string firstToken, secondToken;
  Json firstFrame, secondFrame;
  {
    PartOne server;
    AttachDatabase(server, files);
    Initialize(server);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    content.Enable(owner);
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1000000, 5000 }; });
    first = jobs.NextRequest(owner);
    jobs.Submit(owner, first, kParentQuest, 10, content.metadata,
                content.Scripts());
    RequireSleeping(
      DrainStored(server, owner, first, content.metadata, content.Scripts()));
    firstToken = QuestJournal::JobState(owner).FindLive(first.sequence)->token;
    firstFrame =
      QuestJournal::ScriptState(owner).at("continuations").at(firstToken);
    second = jobs.NextRequest(owner);
    jobs.Submit(owner, second, kParentQuest, 20, content.metadata,
                content.Scripts());
    RequireSleeping(
      DrainStored(server, owner, second, content.metadata, content.Scripts()));
    secondToken =
      QuestJournal::JobState(owner).FindLive(second.sequence)->token;
    secondFrame =
      QuestJournal::ScriptState(owner).at("continuations").at(secondToken);
    REQUIRE(
      QuestJournal::ScriptState(owner).at("continuations").at(firstToken) ==
      firstFrame);
    REQUIRE(QuestJournal::ScriptState(owner).at("arrays").size() == 2);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 15);
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& jobs = server.worldState.GetQuestJobs();
    auto time = std::make_shared<QuestJobTime>(QuestJobTime{ 1000500, 100 });
    jobs.SetClock([time] { return *time; });
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    REQUIRE(QuestJournal::JobState(owner).live.size() == 2);
    RequireSleeping(jobs.Inspect(owner, first));
    RequireSleeping(jobs.Inspect(owner, second));
    REQUIRE(
      QuestJournal::ScriptState(owner).at("continuations").at(firstToken) ==
      firstFrame);
    REQUIRE(
      QuestJournal::ScriptState(owner).at("continuations").at(secondToken) ==
      secondFrame);
    time->steadyMs += 500;
    RequireReady(jobs.Inspect(owner, first));
    RequireSleeping(jobs.Inspect(owner, second));
    jobs.Advance(owner, first, content.metadata, content.Scripts());
    REQUIRE(
      DrainStored(server, owner, first, content.metadata, content.Scripts())
        .status == Status::Completed);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 31);
    REQUIRE(ChildField(owner, kParentQuest, "acaptured").at("value") == 7);
    REQUIRE(ChildField(owner, kParentQuest, "aseen").at("value") == 9);
    REQUIRE(
      QuestJournal::ScriptState(owner).at("continuations").at(secondToken) ==
      secondFrame);
    REQUIRE(QuestJournal::ScriptState(owner).at("arrays").size() == 1);
    RequireSleeping(jobs.Inspect(owner, second));
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1003000, 50 }; });
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    REQUIRE(jobs.Inspect(owner, first).status == Status::Completed);
    RequireReady(jobs.Inspect(owner, second));
    jobs.Advance(owner, second, content.metadata, content.Scripts());
    REQUIRE(
      DrainStored(server, owner, second, content.metadata, content.Scripts())
        .status == Status::Completed);
    REQUIRE(jobs.Inspect(owner, first).status == Status::Completed);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 35);
    REQUIRE(ChildField(owner, kParentQuest, "adone").at("value") == true);
    REQUIRE(ChildField(owner, kParentQuest, "bdone").at("value") == true);
    REQUIRE_FALSE(QuestJournal::JobState(owner).HasLive());
    REQUIRE(QuestJournal::ScriptState(owner).at("continuations").empty());
  }
}

TEST_CASE(
  "Sparse retirement keeps a sleeping low request while later rewards finish",
  "[Quest][QuestJobs][ConcurrentStacks][Retention][SaveReceipt]")
{
  ConcurrentFixture f(false);
  const auto first = f.Jobs().NextRequest(f.Owner());
  f.SubmitRoot(first, kParentQuest, 10);
  RequireSleeping(f.DrainRoot(first));
  const auto sleeping = f.Live(first);
  const auto frame = QuestJournal::ScriptState(f.Owner())
                       .at("continuations")
                       .at(sleeping.token);
  f.storage->submissions.clear();
  RequestId earliestLater;
  for (size_t index = 0; index < 301; ++index) {
    const auto next = f.Jobs().NextRequest(f.Owner());
    if (index == 0)
      earliestLater = next;
    RequirePending(f.SubmitRoot(next, kRetentionQuest, 10));
    REQUIRE(f.DrainRoot(next).status == Status::Completed);
    REQUIRE(f.Gold() == 13 + index);
    const auto state = QuestJournal::JobState(f.Owner());
    REQUIRE(state.live.size() == 1);
    REQUIRE(state.completed.size() <= QuestJobState::MaxRecentCompleted);
    REQUIRE(state.live.size() <= QuestJobState::MaxLive);
    REQUIRE(state.retiredThrough == 0);
    REQUIRE_FALSE(state.IsRetired(first.sequence));
    REQUIRE(state.retiredRanges.size() <= 1);
    REQUIRE(state.FindLive(first.sequence)->instructions ==
            sleeping.instructions);
    REQUIRE(QuestJournal::ScriptState(f.Owner())
              .at("continuations")
              .at(sleeping.token) == frame);
    REQUIRE(MpChangeForm::ToJson(f.Owner().GetChangeForm()).dump().size() <
            256 * 1024);
    f.storage->submissions.clear();
  }
  const auto sparse = QuestJournal::JobState(f.Owner());
  REQUIRE(sparse.completed.size() == QuestJobState::MaxRecentCompleted);
  REQUIRE(sparse.retiredRanges.size() == 1);
  REQUIRE(sparse.retiredRanges[0].first == 2);
  REQUIRE(sparse.retiredRanges[0].last == 270);
  RequireRetired(f.InspectRoot(earliestLater));
  const auto wake = f.Jobs().NextRequest(f.Owner());
  RequirePending(f.SubmitRoot(wake, kParentQuest, 20));
  REQUIRE(f.DrainRoot(wake).status == Status::Completed);
  REQUIRE(f.Gold() == 316);
  RequireSleeping(f.InspectRoot(first));
  REQUIRE(QuestJournal::JobState(f.Owner()).FindCompleted(wake.sequence));
  f.time->steadyMs += 1000;
  RequirePending(f.AdvanceRoot(first));
  REQUIRE(f.DrainRoot(first).status == Status::Completed);
  REQUIRE(f.Gold() == 332);
  REQUIRE(QuestJournal::JobState(f.Owner()).completed.front().id ==
          first.sequence);
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations").empty());
  f.storage->submissions.clear();
  const auto retirement = f.Jobs().NextRequest(f.Owner());
  RequirePending(f.SubmitRoot(retirement, kRetentionQuest, 10));
  REQUIRE(f.Gold() == 333);
  RequirePending(f.InspectRoot(first));
  // Merging the new retirement into the frontier must not hide sparse IDs
  // whose own retirement receipts were already durable.
  RequireRetired(f.InspectRoot(earliestLater));
  REQUIRE(QuestJournal::JobState(f.Owner()).retiredThrough == 272);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  const auto bytes = Serialized(f.storage->submissions[0].forms);
  f.storage->Complete(
    0, std::make_exception_ptr(std::runtime_error("sparse retirement")));
  f.Tick();
  REQUIRE(Serialized(f.storage->submissions[1].forms) == bytes);
  RequirePending(f.InspectRoot(first));
  RequireRetired(f.InspectRoot(earliestLater));
  f.storage->Complete(0);
  f.Tick();
  RequirePending(f.InspectRoot(first));
  f.storage->Complete(1);
  f.Tick();
  RequireRetired(f.InspectRoot(first));
  RequireRetired(f.InspectRoot(earliestLater));
  REQUIRE(f.DrainRoot(retirement).status == Status::Completed);
  REQUIRE(f.Gold() == 333);
  const auto before = QuestJournal::JobState(f.Owner()).Save();
  RequireRetired(f.SubmitRoot(first, kParentQuest, 10));
  RequireRetired(f.SubmitRoot(earliestLater, kRetentionQuest, 10));
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == before);
  REQUIRE(f.Gold() == 333);
}

TEST_CASE(
  "A failed automatic root cannot quarantine a sibling in its campaign",
  "[Quest][QuestJobs][ConcurrentStacks][Scheduler][Rollback]")
{
  ConcurrentFixture f(true, false, true);
  f.storage->automatic = true;
  f.Jobs().SetSchedulerFixtureContent(f.content.metadata, f.content.Scripts());
  const auto first = f.Jobs().NextRequest(f.Owner());
  f.SubmitRoot(first, kParentQuest, 10, true);
  f.Tick();
  RequireSleeping(f.InspectRoot(first));
  const auto second = f.Jobs().NextRequest(f.Owner());
  f.SubmitRoot(second, kParentQuest, 20, true);
  f.Tick();
  RequireSleeping(f.InspectRoot(first));
  RequireSleeping(f.InspectRoot(second));
  REQUIRE(f.Gold() == 15);
  f.time->steadyMs += 1000;
  for (size_t pass = 0;
       pass < 64 && f.InspectRoot(first).status != Status::Completed; ++pass)
    f.Tick();
  REQUIRE(f.InspectRoot(first).status == Status::Completed);
  RequireSleeping(f.InspectRoot(second));
  REQUIRE(f.Gold() == 31);
  const auto before = QuestJournal::ScriptState(f.Owner());
  f.time->steadyMs += 1000;
  for (size_t pass = 0; pass < 8; ++pass)
    f.Tick();
  REQUIRE(f.InspectRoot(second).status == Status::Failed);
  REQUIRE(f.InspectRoot(second).error.find("UnsupportedConcurrentSuffix") !=
          std::string::npos);
  REQUIRE(f.InspectRoot(first).status == Status::Completed);
  REQUIRE(f.Gold() == 31);
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == before);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "adone").at("value") == true);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "bdone").at("value") == false);
  f.Jobs().Retry(f.Owner(), second);
  REQUIRE(f.InspectRoot(first).status == Status::Completed);
  for (size_t pass = 0; pass < 8; ++pass)
    f.Tick();
  REQUIRE(f.InspectRoot(second).status == Status::Failed);
  REQUIRE(f.Gold() == 31);
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == before);
}

namespace {
constexpr uint32_t kInteractionOriginal = 0x6debb;
constexpr uint32_t kInteractionCopy = 0xff006180;
constexpr uint32_t kInteractionNpc = 0xff006181;
constexpr auto kInteractionState =
  "reference:6debb:Skyrim.esm/defaultsetstagetrigscript";

struct InteractionProgram
{
  ConcurrentProgram concurrent;
  std::shared_ptr<PexScript> handlerBase;
  InteractionProgram(bool wake = true, bool waits = true, bool child = false)
    : concurrent(false)
  {
    auto& quest = concurrent.program.script->objectTable[0];
    FunctionInfo wakeFunction;
    wakeFunction.valid = true;
    wakeFunction.returnType = "None";
    wakeFunction.code.instructions = {
      { FunctionCode::kOp_Assign, { Name("scalar"), VarValue(99) } },
      { FunctionCode::kOp_ArraySetElement,
        { Name("shared"), VarValue(0), VarValue(9) } },
      { FunctionCode::kOp_Assign, { Name("ready"), VarValue(true) } },
      { FunctionCode::kOp_Return, { VarValue::None() } }
    };
    quest.states[0].functions.push_back({ "Wake", wakeFunction });
    if (child) {
      auto function = Reward(4);
      function.code.instructions.pop_back();
      function.code.instructions.push_back(
        { FunctionCode::kOp_CallStatic,
          { VarValue("Utility"), VarValue("WaitMenuMode"), Name("temp"),
            VarValue(1), VarValue(1.0) } });
      function.code.instructions.push_back(
        { FunctionCode::kOp_CallMethod,
          { VarValue("AddItem"), Name("target"), Name("temp"), VarValue(3),
            Name("gold"), VarValue(6), VarValue(true) } });
      function.code.instructions.push_back(
        { FunctionCode::kOp_Return, { VarValue::None() } });
      quest.states[0].functions[2].function = std::move(function);
    }

    handlerBase = std::make_shared<PexScript>();
    handlerBase->source = "InteractionBase";
    Object base;
    base.NameIndex = handlerBase->source;
    base.parentClassName = "ObjectReference";
    base.variables = { { "prefixes", "Int", 0, VarValue(0) },
                       { "suffixes", "Int", 0, VarValue(0) },
                       { "sourceId", "Int", 0, VarValue(0) },
                       { "lastSource", "ObjectReference", 0,
                         VarValue::None() } };
    auto handler = Reward(3);
    handler.params = { { "source", "ObjectReference" } };
    handler.locals.insert(handler.locals.end(),
                          { { "quest", "Quest" },
                            { "script", "JobFixture" },
                            { "accepted", "Bool" } });
    handler.code.instructions.pop_back();
    handler.code.instructions.push_back(
      { FunctionCode::kOp_IAdd,
        { Name("prefixes"), Name("prefixes"), VarValue(1) } });
    handler.code.instructions.push_back(
      { FunctionCode::kOp_Assign, { Name("lastSource"), Name("source") } });
    handler.code.instructions.push_back(
      { FunctionCode::kOp_CallMethod,
        { VarValue("GetFormID"), Name("source"), Name("sourceId"),
          VarValue(0) } });
    if (wake) {
      handler.code.instructions.push_back(
        { FunctionCode::kOp_CallStatic,
          { VarValue("Game"), VarValue("GetForm"), Name("quest"), VarValue(1),
            VarValue(static_cast<int32_t>(kParentQuest)) } });
      handler.code.instructions.push_back(
        { FunctionCode::kOp_Cast, { Name("script"), Name("quest") } });
      handler.code.instructions.push_back(
        { FunctionCode::kOp_CallMethod,
          { VarValue("Wake"), Name("script"), Name("temp"), VarValue(0) } });
    }
    handler.code.instructions.push_back(
      { FunctionCode::kOp_Assign, { Name("::State"), VarValue("idle") } });
    if (waits)
      handler.code.instructions.push_back(
        { FunctionCode::kOp_CallStatic,
          { VarValue("Utility"), VarValue("Wait"), Name("temp"), VarValue(1),
            VarValue(2.0) } });
    if (child) {
      handler.code.instructions.push_back(
        { FunctionCode::kOp_CallStatic,
          { VarValue("Game"), VarValue("GetForm"), Name("quest"), VarValue(1),
            VarValue(static_cast<int32_t>(kRetentionQuest)) } });
      handler.code.instructions.push_back(
        { FunctionCode::kOp_CallMethod,
          { VarValue("SetStage"), Name("quest"), Name("accepted"), VarValue(1),
            VarValue(10) } });
    }
    handler.code.instructions.push_back(
      { FunctionCode::kOp_IAdd,
        { Name("suffixes"), Name("suffixes"), VarValue(1) } });
    handler.code.instructions.push_back(
      { FunctionCode::kOp_CallMethod,
        { VarValue("AddItem"), Name("target"), Name("temp"), VarValue(3),
          Name("gold"), VarValue(5), VarValue(true) } });
    handler.code.instructions.push_back(
      { FunctionCode::kOp_Return, { VarValue::None() } });
    FunctionInfo ignored;
    ignored.valid = true;
    ignored.returnType = "None";
    ignored.params = { { "source", "ObjectReference" } };
    ignored.code.instructions = { { FunctionCode::kOp_Return,
                                    { VarValue::None() } } };
    base.states = {
      { "busy", { { "OnActivate", handler } } },
      { "idle", { { "OnActivate", ignored }, { "OnTriggerEnter", ignored } } }
    };
    handlerBase->objectTable.push_back(base);
    auto attached = std::make_shared<PexScript>();
    attached->source = "defaultSetStageTRIGScript";
    Object derived;
    derived.NameIndex = attached->source;
    derived.parentClassName = handlerBase->source;
    derived.autoStateName = "busy";
    derived.variables = { { "::stage_var", "Int", 0, VarValue(0) } };
    attached->objectTable.push_back(derived);
    concurrent.program.bases.push_back(handlerBase);
    concurrent.program.bases.push_back(attached);

    espm::QuestScriptData::Alias alias;
    alias.object.type = 1;
    alias.object.formId = kParentQuest;
    alias.object.alias = 3;
    alias.version = 5;
    alias.objectFormat = 2;
    alias.scripts = { { "InteractionInventoryObserver", 0, {} } };
    concurrent.metadata.at(kParentQuest).aliases = { alias };
    auto observer = std::make_shared<PexScript>();
    observer->source = "InteractionInventoryObserver";
    Object object;
    object.NameIndex = observer->source;
    object.parentClassName = "ReferenceAlias";
    object.variables = { { "order", "Int", 0, VarValue(0) },
                         { "calls", "Int", 0, VarValue(0) } };
    auto observed = EventFunction();
    observed.code.instructions = {
      { FunctionCode::kOp_IMultiply,
        { Name("order"), Name("order"), VarValue(10) } },
      { FunctionCode::kOp_IAdd,
        { Name("order"), Name("order"), Name("count") } },
      { FunctionCode::kOp_IAdd,
        { Name("calls"), Name("calls"), VarValue(1) } },
      { FunctionCode::kOp_Return, { VarValue::None() } }
    };
    object.states = { { "", { { "OnItemAdded", observed } } } };
    observer->objectTable.push_back(object);
    concurrent.program.bases.push_back(observer);
  }
  auto Scripts() const { return concurrent.Scripts(); }
  const auto& Metadata() const { return concurrent.metadata; }
  void Enable(MpActor& owner) const
  {
    concurrent.Enable(owner);
    QuestJournal::Atomic(owner, [&] {
      QuestVmState state(QuestJournal::ScriptState(owner), {}, {});
      state.SetAliasBinding("4e50d:Skyrim.esm/alias:3", "14:Skyrim.esm");
      QuestJournal::SetScriptState(owner, state.Save());
    });
  }
};

MpObjectReference& BindInteractionTarget(PartOne& server, MpActor& owner)
{
  auto& world = server.worldState;
  const auto original =
    world.GetEspm().GetBrowser().LookupById(kInteractionOriginal);
  const auto data = reinterpret_cast<const espm::REFR*>(original.rec)
                      ->GetData(world.GetEspmCache());
  const auto base = original.ToGlobalId(data.baseId);
  const auto type =
    world.GetEspm().GetBrowser().LookupById(base).rec->GetType().ToString();
  world.AddForm(
    std::make_unique<MpObjectReference>(
      LocationalData{ { 1, 2, 3 }, { 0, 0, 0 }, FormDesc::Tamriel() },
      FormCallbacks::DoNothing(), base, type.c_str()),
    kInteractionCopy);
  auto& copy = world.GetFormAt<MpObjectReference>(kInteractionCopy);
  copy.SetWorldInstance(owner.GetWorldInstance());
  copy.SetInventory(Inventory());
  CampaignReferenceBinding::Bind(owner, kInteractionOriginal, copy);
  return copy;
}

MpActor& BindInteractionNpc(PartOne& server, MpActor& owner)
{
  server.worldState.AddForm(
    std::make_unique<MpActor>(
      LocationalData{ { 1, 2, 3 }, { 0, 0, 0 }, FormDesc::Tamriel() },
      FormCallbacks::DoNothing(), 0x13bbd),
    kInteractionNpc);
  auto& copy = server.worldState.GetFormAt<MpActor>(kInteractionNpc);
  copy.SetWorldInstance(owner.GetWorldInstance());
  copy.SetInventory(Inventory().AddItem(0xf, 7));
  CampaignReferenceBinding::Bind(owner, 0x1a677, copy);
  return copy;
}

Json InteractionField(MpActor& owner, const char* field)
{
  std::string name = std::string("interactionbase/") + field;
  return QuestJournal::ScriptState(owner)
    .at("scripts")
    .at(kInteractionState)
    .at("values")
    .at(name)
    .at("value");
}

int InteractionOrder(MpActor& owner)
{
  return EventField(owner, "interactioninventoryobserver",
                    "interactioninventoryobserver/order");
}

struct InteractionFixture : Fixture
{
  InteractionProgram content;
  std::shared_ptr<QuestJobTime> time =
    std::make_shared<QuestJobTime>(QuestJobTime{ 1000000, 5000 });
  InteractionFixture(bool wake = true, bool waits = true, bool child = false,
                     bool initialize = true)
    : content(wake, waits, child)
  {
    content.Enable(Owner());
    BindInteractionTarget(server, Owner());
    if (initialize)
      QuestScriptRuntime::ExecuteReferenceLoad(Owner(), kInteractionOriginal,
                                               content.Scripts());
    storage->automatic = true;
    Tick();
    storage->submissions.clear();
    storage->automatic = false;
    Jobs().SetClock([time = time] { return *time; });
  }
  MpObjectReference& Target()
  {
    return server.worldState.GetFormAt<MpObjectReference>(kInteractionCopy);
  }
  QuestJobRunner::Result SubmitInteraction(
    const RequestId& request, MpActor& actor, MpObjectReference& source,
    const std::string& name = "OnActivate")
  {
    return Jobs().SubmitEvent(actor, request, Target(), name, source,
                              content.Metadata(), content.Scripts());
  }
  QuestJobRunner::Result SubmitInteraction(const RequestId& request)
  {
    return SubmitInteraction(request, Owner(), Owner());
  }
  QuestJobRunner::Result SubmitStage(const RequestId& request)
  {
    return Jobs().Submit(Owner(), request, kParentQuest, 10,
                         content.Metadata(), content.Scripts());
  }
  QuestJobRunner::Result Advance(const RequestId& request)
  {
    return Jobs().Advance(Owner(), request, content.Metadata(),
                          content.Scripts());
  }
  QuestJobRunner::Result Inspect(const RequestId& request)
  {
    return Jobs().Inspect(Owner(), request);
  }
  QuestJobRunner::Result DrainInteraction(const RequestId& request)
  {
    return Drain(*this, request, content.Metadata(), content.Scripts());
  }
  void AckLatest()
  {
    Tick();
    REQUIRE_FALSE(storage->submissions.empty());
    storage->Complete(storage->submissions.size() - 1);
    Tick();
  }
  QuestJobState::Job Live(const RequestId& request)
  {
    const auto state = QuestJournal::JobState(Owner());
    const auto* job = state.FindLive(request.sequence);
    REQUIRE(job);
    return *job;
  }
  void SelectHandler(const RequestId& request)
  {
    for (size_t boundary = 0; boundary < 32; ++boundary) {
      if (Inspect(request).status == Status::Pending)
        AckLatest();
      if (Live(request).handler)
        return;
      RequireReady(Inspect(request));
      RequirePending(Advance(request));
    }
    FAIL("Interaction did not select its handler");
  }
};

void RequireInteraction(const QuestJobRunner::Result& result)
{
  REQUIRE(result.kind == QuestJobRunner::ResultKind::Interaction);
  REQUIRE_FALSE(result.accepted);
}
}

TEST_CASE(
  "A saved OnActivate handler wakes a sleeping stage through the shared heap",
  "[Quest][QuestJobs][Interactions][ConcurrentStacks][SaveReceipt]")
{
  InteractionFixture f;
  REQUIRE(QuestJournal::ReferenceInitialized(f.Owner(), kInteractionOriginal));
  const auto stage = f.Jobs().NextRequest(f.Owner());
  f.SubmitStage(stage);
  RequireSleeping(f.DrainInteraction(stage));
  const auto stageToken = f.Live(stage).token;
  const auto frame =
    QuestJournal::ScriptState(f.Owner()).at("continuations").at(stageToken);
  const auto event = f.Jobs().NextRequest(f.Owner());
  RequirePending(f.SubmitInteraction(event));
  REQUIRE(f.Gold() == 12);
  REQUIRE_THROWS(f.SubmitStage(event));
  REQUIRE_THROWS(f.SubmitInteraction(stage));
  f.SelectHandler(event);
  const auto selected = f.Live(event);
  REQUIRE(selected.kind == QuestJobState::Kind::Interaction);
  REQUIRE(selected.quest.empty());
  REQUIRE(selected.contentHash.empty());
  REQUIRE(selected.stage == 0);
  REQUIRE_FALSE(selected.interactionDone);
  REQUIRE(selected.handler->handlerIdentity->rootScript ==
          "defaultSetStageTRIGScript");
  REQUIRE(selected.handler->handlerIdentity->implementation ==
          "InteractionBase");
  REQUIRE(selected.handler->handlerIdentity->state == "busy");
  RequirePending(f.Advance(event));
  REQUIRE(f.Gold() == 15);
  REQUIRE(InteractionField(f.Owner(), "prefixes") == 1);
  REQUIRE(InteractionField(f.Owner(), "suffixes") == 0);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "ready").at("value") == true);
  REQUIRE(
    QuestJournal::ScriptState(f.Owner()).at("continuations").at(stageToken) ==
    frame);
  REQUIRE(InteractionOrder(f.Owner()) == 0);
  f.time->steadyMs += 1000;
  RequireReady(f.Inspect(stage));
  RequirePending(f.Inspect(event));
  REQUIRE_THROWS(f.Advance(stage));
  f.Tick();
  const auto failed = f.storage->submissions.size() - 1;
  const auto bytes = Serialized(f.storage->submissions[failed].forms);
  f.storage->Complete(
    failed, std::make_exception_ptr(std::runtime_error("interaction prefix")));
  f.Tick();
  REQUIRE(Serialized(f.storage->submissions.back().forms) == bytes);
  REQUIRE_THROWS(f.Advance(stage));
  REQUIRE(f.Gold() == 15);
  f.storage->Complete(failed);
  f.Tick();
  RequirePending(f.Inspect(event));
  f.AckLatest();
  RequireSleeping(f.Inspect(event));
  RequireInteraction(f.Inspect(event));
  RequirePending(f.Advance(stage));
  const auto stageResult = f.DrainInteraction(stage);
  REQUIRE(stageResult.status == Status::Completed);
  REQUIRE(stageResult.kind == QuestJobRunner::ResultKind::Stage);
  REQUIRE(stageResult.accepted);
  REQUIRE(f.Gold() == 31);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "acaptured").at("value") == 7);
  REQUIRE(ChildField(f.Owner(), kParentQuest, "aseen").at("value") == 9);
  REQUIRE(InteractionOrder(f.Owner()) == 36);
  RequireSleeping(f.Inspect(event));
  f.time->steadyMs += 1000;
  RequirePending(f.Advance(event));
  REQUIRE(f.Gold() == 36);
  REQUIRE(InteractionOrder(f.Owner()) == 36);
  const auto complete = f.DrainInteraction(event);
  REQUIRE(complete.status == Status::Completed);
  RequireInteraction(complete);
  REQUIRE(InteractionOrder(f.Owner()) == 3635);
  REQUIRE(InteractionField(f.Owner(), "prefixes") == 1);
  REQUIRE(InteractionField(f.Owner(), "suffixes") == 1);
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations").empty());
  const auto before = QuestJournal::JobState(f.Owner()).Save();
  RequireInteraction(f.SubmitInteraction(event));
  REQUIRE(f.SubmitInteraction(event).status == Status::Completed);
  REQUIRE_THROWS(
    f.SubmitInteraction(event, f.Owner(), f.Owner(), "OnTriggerEnter"));
  REQUIRE_THROWS(f.SubmitInteraction(event, f.Owner(), f.Target()));
  REQUIRE_THROWS(f.SubmitStage(event));
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == before);
  REQUIRE(f.Gold() == 36);
}

TEST_CASE("Interaction roots preserve owner guest and NPC source identities",
          "[Quest][QuestJobs][Interactions]")
{
  const auto sourceKind = GENERATE(0, 1, 2);
  InteractionFixture f(false, false);
  MpActor* actor = &f.Owner();
  MpObjectReference* source = &f.Owner();
  MpActor* guest = nullptr;
  MpActor* npc = nullptr;
  if (sourceKind == 1) {
    f.server.CreateActor(kOwner + 1, { 1, 2, 3 }, 0, 0x3c);
    guest = &f.server.worldState.GetFormAt<MpActor>(kOwner + 1);
    guest->SetWorldInstance(f.Owner().GetWorldInstance());
    guest->SetInventory(Inventory().AddItem(0xf, 100));
    actor = guest;
    source = guest;
  } else if (sourceKind == 2) {
    npc = &BindInteractionNpc(f.server, f.Owner());
    source = npc;
  }
  const auto request = f.Jobs().NextRequest(*actor);
  RequirePending(f.SubmitInteraction(request, *actor, *source));
  for (size_t boundary = 0; boundary < 128; ++boundary) {
    const auto result = f.Jobs().Inspect(*actor, request);
    if (result.status == Status::Completed)
      break;
    if (result.status == Status::Pending)
      f.AckLatest();
    else {
      RequireReady(result);
      RequirePending(f.Jobs().Advance(*actor, request, f.content.Metadata(),
                                      f.content.Scripts()));
    }
  }
  const auto result = f.Jobs().Inspect(*actor, request);
  REQUIRE(result.status == Status::Completed);
  RequireInteraction(result);
  REQUIRE(f.Gold() == 18);
  REQUIRE(InteractionField(f.Owner(), "prefixes") == 1);
  REQUIRE(InteractionField(f.Owner(), "suffixes") == 1);
  const std::string original =
    sourceKind == 2 ? "1a677:Skyrim.esm" : "14:Skyrim.esm";
  REQUIRE(InteractionField(f.Owner(), "sourceid") ==
          (sourceKind == 2 ? 0x1a677 : 0x14));
  REQUIRE(InteractionField(f.Owner(), "lastsource") == original);
  const auto saved = QuestJournal::JobState(f.Owner());
  const auto* complete = saved.FindCompleted(request.sequence);
  REQUIRE(complete);
  REQUIRE(complete->actor == actor->GetChangeForm().formDesc.ToString());
  REQUIRE(complete->interaction->source == original);
  REQUIRE(complete->interaction->sourceReference ==
          source->GetChangeForm().formDesc.ToString());
  REQUIRE(complete->interaction->target == "6debb:Skyrim.esm");
  REQUIRE(complete->interaction->targetReference ==
          f.Target().GetChangeForm().formDesc.ToString());
  if (guest) {
    REQUIRE(guest->GetInventory().GetItemCount(0xf) == 100);
    REQUIRE_THROWS(f.Jobs().Inspect(f.Owner(), request));
  }
  if (npc)
    REQUIRE(npc->GetInventory().GetItemCount(0xf) == 7);
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(0x1a677));
}

TEST_CASE(
  "Interaction handler child waits recover through SQLite without replay",
  "[Quest][QuestJobs][Interactions][ChildStages][SQLite]")
{
  SqliteFiles files;
  InteractionProgram content(true, true, true);
  RequestId stage, event;
  std::string childToken;
  {
    PartOne server;
    AttachDatabase(server, files);
    Initialize(server);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    content.Enable(owner);
    auto& target = BindInteractionTarget(server, owner);
    QuestScriptRuntime::ExecuteReferenceLoad(owner, kInteractionOriginal,
                                             content.Scripts());
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1000000, 5000 }; });
    stage = jobs.NextRequest(owner);
    jobs.Submit(owner, stage, kParentQuest, 10, content.Metadata(),
                content.Scripts());
    RequireSleeping(DrainStored(server, owner, stage, content.Metadata(),
                                content.Scripts()));
    event = jobs.NextRequest(owner);
    jobs.SubmitEvent(owner, event, target, "OnActivate", owner,
                     content.Metadata(), content.Scripts());
    const auto waiting =
      DrainStored(server, owner, event, content.Metadata(), content.Scripts());
    RequireSleeping(waiting);
    RequireInteraction(waiting);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 15);
    REQUIRE(InteractionField(owner, "prefixes") == 1);
    REQUIRE(InteractionOrder(owner) == 0);
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& jobs = server.worldState.GetQuestJobs();
    auto time = std::make_shared<QuestJobTime>(QuestJobTime{ 1001000, 100 });
    jobs.SetClock([time] { return *time; });
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    RequireReady(jobs.Inspect(owner, stage));
    RequireSleeping(jobs.Inspect(owner, event));
    jobs.Advance(owner, stage, content.Metadata(), content.Scripts());
    REQUIRE(
      DrainStored(server, owner, stage, content.Metadata(), content.Scripts())
        .status == Status::Completed);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 31);
    REQUIRE(InteractionOrder(owner) == 36);
    time->steadyMs += 1000;
    jobs.Advance(owner, event, content.Metadata(), content.Scripts());
    RequireReady(WaitForDurable(server, owner, event));
    const auto state = QuestJournal::JobState(owner);
    const auto* job = state.FindLive(event.sequence);
    REQUIRE(job);
    REQUIRE(job->children.size() == 1);
    REQUIRE(job->children.back().cursor == "begin");
    REQUIRE(job->handler->cursor == "waiting");
    REQUIRE(job->handler->handlerIdentity->state == "busy");
    REQUIRE_FALSE(job->interactionDone);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 31);
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1002000, 100 }; });
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    RequireReady(jobs.Inspect(owner, event));
    jobs.Advance(owner, event, content.Metadata(), content.Scripts());
    RequireSleeping(WaitForDurable(server, owner, event));
    const auto state = QuestJournal::JobState(owner);
    const auto* job = state.FindLive(event.sequence);
    REQUIRE(job->children.back().cursor == "waiting");
    childToken = job->children.back().token;
    REQUIRE(QuestJournal::ScriptState(owner).at("continuations").size() == 2);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 35);
    REQUIRE(InteractionOrder(owner) == 36);
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1004000, 100 }; });
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    REQUIRE_THROWS(jobs.Resume(owner, event, childToken, true,
                               content.Metadata(), content.Scripts()));
    jobs.Advance(owner, event, content.Metadata(), content.Scripts());
    RequireReady(WaitForDurable(server, owner, event));
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 41);
    REQUIRE(InteractionField(owner, "suffixes") == 0);
    REQUIRE(InteractionOrder(owner) == 36);
    jobs.Advance(owner, event, content.Metadata(), content.Scripts());
    RequireReady(WaitForDurable(server, owner, event));
    const auto state = QuestJournal::JobState(owner);
    REQUIRE(state.FindLive(event.sequence)->handler->cursor == "done");
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 46);
    REQUIRE(InteractionField(owner, "prefixes") == 1);
    REQUIRE(InteractionField(owner, "suffixes") == 1);
    REQUIRE(InteractionOrder(owner) == 36);
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = server.worldState.GetQuestJobs();
    const auto complete =
      DrainStored(server, owner, event, content.Metadata(), content.Scripts());
    REQUIRE(complete.status == Status::Completed);
    RequireInteraction(complete);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 46);
    REQUIRE(InteractionOrder(owner) == 363465);
    REQUIRE(InteractionField(owner, "prefixes") == 1);
    REQUIRE(InteractionField(owner, "suffixes") == 1);
    const auto state = QuestJournal::JobState(owner);
    const auto* saved = state.FindCompleted(event.sequence);
    REQUIRE(saved->interactionDone);
    REQUIRE(saved->events.size() == 4);
    REQUIRE(saved->eventHead == saved->events.size());
    REQUIRE(saved->completions.size() == 3);
    REQUIRE(QuestJournal::ScriptState(owner).at("continuations").empty());
    auto& target =
      server.worldState.GetFormAt<MpObjectReference>(kInteractionCopy);
    REQUIRE(jobs
              .SubmitEvent(owner, event, target, "OnActivate", owner,
                           content.Metadata(), content.Scripts())
              .status == Status::Completed);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 46);
  }
}

TEST_CASE(
  "Interaction admission rejects unavailable context before publication",
  "[Quest][QuestJobs][Interactions][Rollback]")
{
  InteractionFixture f(false, false, false, false);
  const auto request = f.Jobs().NextRequest(f.Owner());
  const auto before = QuestJournal::JobState(f.Owner()).Save();
  REQUIRE_THROWS(f.SubmitInteraction(request));
  REQUIRE_FALSE(
    QuestJournal::ReferenceInitialized(f.Owner(), kInteractionOriginal));
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == before);
  REQUIRE(f.Gold() == 10);
  QuestScriptRuntime::ExecuteReferenceLoad(f.Owner(), kInteractionOriginal,
                                           f.content.Scripts());
  const auto initialized = QuestJournal::ScriptState(f.Owner());
  REQUIRE_THROWS(
    f.SubmitInteraction(request, f.Owner(), f.Owner(), "OnDeath"));
  REQUIRE_THROWS(f.Jobs().SubmitEvent(
    f.Owner(), request, f.Owner(), "OnActivate", f.Owner(),
    f.content.Metadata(), f.content.Scripts()));
  f.server.CreateActor(kOwner + 1, { 1, 2, 3 }, 0, 0x3c);
  auto& stranger = f.server.worldState.GetFormAt<MpActor>(kOwner + 1);
  stranger.SetWorldInstance("campaign:" + std::to_string(kOwner + 1) +
                            ":other");
  REQUIRE_THROWS(f.SubmitInteraction(request, f.Owner(), stranger));
  stranger.SetWorldInstance(f.Owner().GetWorldInstance());
  REQUIRE_THROWS(f.SubmitInteraction(request, f.Owner(), stranger));
  const auto phase = f.Target().GetWorldInstance();
  f.Target().SetWorldInstance("campaign:" + std::to_string(kOwner) + ":other");
  REQUIRE_THROWS(f.SubmitInteraction(request));
  f.Target().SetWorldInstance(phase);
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == before);
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == initialized);
  REQUIRE(f.Gold() == 10);
  RequirePending(f.SubmitInteraction(request));
  f.AckLatest();
  const auto checkpoint = QuestJournal::JobState(f.Owner()).Save();
  const auto heap = QuestJournal::ScriptState(f.Owner());
  f.Target().SetWorldInstance("campaign:" + std::to_string(kOwner) + ":other");
  REQUIRE_THROWS(f.Advance(request));
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == checkpoint);
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == heap);
  f.Target().SetWorldInstance(phase);
  const auto retained =
    f.server.worldState.LookupFormByIdNoLoad(kInteractionCopy);
  REQUIRE(retained);
  f.server.worldState.DestroyForm(kInteractionCopy);
  REQUIRE_THROWS(f.Advance(request));
  REQUIRE_THROWS(f.Jobs().SubmitEvent(
    f.Owner(), request, *retained->AsObjectReference(), "OnActivate",
    f.Owner(), f.content.Metadata(), f.content.Scripts()));
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == checkpoint);
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == heap);
  REQUIRE(f.Gold() == 10);
}

TEST_CASE(
  "Recovered interaction physical identities and templates remain pinned",
  "[Quest][QuestJobs][Interactions][Rollback]")
{
  const auto corruption = GENERATE(0, 1, 2);
  const bool ownerFirst = GENERATE(false, true);
  InteractionFixture f(false, false);
  const auto request = f.Jobs().NextRequest(f.Owner());
  f.SubmitInteraction(request);
  f.AckLatest();
  auto ownerSave = f.Owner().GetChangeForm();
  const auto targetSave = f.Target().GetChangeForm();
  auto journal =
    Json::parse(ownerSave.dynamicFields.GetValueDump(QuestJournal::kProperty));
  auto state = QuestJobState::Read(journal.at("jobs"));
  auto* job = state.FindLive(request.sequence);
  REQUIRE(job);
  if (corruption == 0)
    job->interaction->targetReference =
      f.Owner().GetChangeForm().formDesc.ToString();
  else if (corruption == 1)
    job->interaction->targetTemplates.push_back("13bbd:Skyrim.esm");
  else
    job->interaction->sourceReference =
      f.Target().GetChangeForm().formDesc.ToString();
  journal["jobs"] = state.Save();
  ownerSave.dynamicFields.SetValueDump(QuestJournal::kProperty,
                                       journal.dump());
  PartOne restarted;
  restarted.AttachEspm(&GetEspmLoader());
  auto storage = std::make_shared<ControlledStorage>();
  restarted.AttachSaveStorage(storage);
  // Saved trigger occupancy must be silent in either database iteration order.
  restarted.worldState.LoadChangeForm(ownerFirst ? ownerSave : targetSave,
                                      FormCallbacks::DoNothing());
  restarted.worldState.LoadChangeForm(ownerFirst ? targetSave : ownerSave,
                                      FormCallbacks::DoNothing());
  auto& owner = restarted.worldState.GetFormAt<MpActor>(kOwner);
  auto& jobs = restarted.worldState.GetQuestJobs();
  const auto before = QuestJournal::JobState(owner).Save();
  const auto heap = QuestJournal::ScriptState(owner);
  REQUIRE_THROWS(
    jobs.Advance(owner, request, f.content.Metadata(), f.content.Scripts()));
  REQUIRE(QuestJournal::JobState(owner).Save() == before);
  REQUIRE(QuestJournal::ScriptState(owner) == heap);
  REQUIRE(owner.GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(storage->submissions.empty());
}

TEST_CASE("Retired interaction requests retain opaque terminal identity",
          "[Quest][QuestJobs][Interactions][Retention]")
{
  InteractionFixture f(false, false);
  const auto first = f.Jobs().NextRequest(f.Owner());
  f.SubmitInteraction(first);
  const auto complete = f.DrainInteraction(first);
  REQUIRE(complete.status == Status::Completed);
  RequireInteraction(complete);
  REQUIRE(f.Gold() == 18);
  // A legacy command with identical diagnostic text is a distinct request.
  const auto sameName = RequestName(first);
  f.Jobs().SubmitEvent(f.Owner(), sameName, f.Target(), "OnActivate",
                       f.Owner(), f.content.Metadata(), f.content.Scripts());
  const auto legacy =
    Drain(f, sameName, f.content.Metadata(), f.content.Scripts());
  REQUIRE(legacy.status == Status::Completed);
  RequireInteraction(legacy);
  for (size_t index = 0; index < 32; ++index) {
    const auto next = f.Jobs().NextRequest(f.Owner());
    f.SubmitInteraction(next);
    REQUIRE(f.DrainInteraction(next).status == Status::Completed);
    f.storage->submissions.clear();
  }
  const auto before = QuestJournal::JobState(f.Owner()).Save();
  const auto retired = f.Inspect(first);
  RequireRetired(retired);
  REQUIRE(retired.kind == QuestJobRunner::ResultKind::Unknown);
  RequireRetired(f.SubmitInteraction(first, f.Owner(), f.Owner(), "OnDeath"));
  RequireRetired(f.SubmitStage(first));
  REQUIRE(f.Jobs().Inspect(f.Owner(), sameName).status == Status::Completed);
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == before);
  REQUIRE(f.Gold() == 18);
  REQUIRE(InteractionField(f.Owner(), "prefixes") == 1);
  REQUIRE(InteractionField(f.Owner(), "suffixes") == 1);
}

namespace {
using LoadPhase = QuestJobState::LoadPhase;

struct LoadProgram
{
  InteractionProgram interaction;
  LoadProgram(bool waitInit = true, bool child = false, bool pollLoad = false)
    : interaction(false, false, child)
  {
    auto& base = interaction.handlerBase->objectTable[0];
    base.variables = { { "initPrefixes", "Int", 0, VarValue(0) },
                       { "initSuffixes", "Int", 0, VarValue(0) },
                       { "cells", "Int", 0, VarValue(0) },
                       { "loads", "Int", 0, VarValue(0) },
                       { "interactions", "Int", 0, VarValue(0) },
                       { "ready", "Bool", 0, VarValue(false) } };
    auto init = Reward(2);
    init.locals.insert(init.locals.end(),
                       { { "quest", "Quest" }, { "accepted", "Bool" } });
    init.code.instructions.pop_back();
    init.code.instructions.push_back(
      { FunctionCode::kOp_IAdd,
        { Name("initPrefixes"), Name("initPrefixes"), VarValue(1) } });
    if (waitInit)
      init.code.instructions.push_back(
        { FunctionCode::kOp_CallStatic,
          { VarValue("Utility"), VarValue("Wait"), Name("temp"), VarValue(1),
            VarValue(1.0) } });
    if (child) {
      init.code.instructions.push_back(
        { FunctionCode::kOp_CallStatic,
          { VarValue("Game"), VarValue("GetForm"), Name("quest"), VarValue(1),
            VarValue(static_cast<int32_t>(kRetentionQuest)) } });
      init.code.instructions.push_back(
        { FunctionCode::kOp_CallMethod,
          { VarValue("SetStage"), Name("quest"), Name("accepted"), VarValue(1),
            VarValue(10) } });
    }
    init.code.instructions.push_back(
      { FunctionCode::kOp_IAdd,
        { Name("initSuffixes"), Name("initSuffixes"), VarValue(1) } });
    init.code.instructions.push_back(
      { FunctionCode::kOp_CallMethod,
        { VarValue("AddItem"), Name("target"), Name("temp"), VarValue(3),
          Name("gold"), VarValue(3), VarValue(true) } });
    init.code.instructions.push_back(
      { FunctionCode::kOp_Return, { VarValue::None() } });
    auto cell = Reward(5);
    cell.code.instructions.insert(
      cell.code.instructions.begin(),
      { FunctionCode::kOp_IAdd,
        { Name("cells"), Name("cells"), VarValue(1) } });
    auto load = Reward(7);
    load.code.instructions.insert(
      load.code.instructions.begin(),
      { FunctionCode::kOp_IAdd,
        { Name("loads"), Name("loads"), VarValue(1) } });
    if (pollLoad) {
      load.code.instructions.pop_back();
      load.code.instructions.insert(
        load.code.instructions.end(),
        { { FunctionCode::kOp_JumpT, { Name("ready"), VarValue(3) } },
          { FunctionCode::kOp_CallStatic,
            { VarValue("Utility"), VarValue("WaitMenuMode"), Name("temp"),
              VarValue(1), VarValue(1.0) } },
          { FunctionCode::kOp_Jump, { VarValue(-2) } },
          { FunctionCode::kOp_Return, { VarValue::None() } } });
    }
    FunctionInfo activate;
    activate.valid = true;
    activate.returnType = "None";
    activate.params = { { "source", "ObjectReference" } };
    activate.code.instructions = {
      { FunctionCode::kOp_Assign, { Name("ready"), VarValue(true) } },
      { FunctionCode::kOp_IAdd,
        { Name("interactions"), Name("interactions"), VarValue(1) } },
      { FunctionCode::kOp_Return, { VarValue::None() } }
    };
    base.states = { { "busy",
                      { { "OnInit", init },
                        { "OnCellLoad", cell },
                        { "OnLoad", load },
                        { "OnActivate", activate } } } };

    espm::QuestScriptData::Alias alias;
    alias.object.type = 1;
    alias.object.formId = kParentQuest;
    alias.object.alias = 1;
    alias.version = 5;
    alias.objectFormat = 2;
    alias.scripts = { { "LoadAlias", 0, {} } };
    interaction.concurrent.metadata.at(kParentQuest).aliases.push_back(alias);
    auto script = std::make_shared<PexScript>();
    script->source = "LoadAlias";
    Object object;
    object.NameIndex = script->source;
    object.parentClassName = "ReferenceAlias";
    object.variables = { { "inits", "Int", 0, VarValue(0) },
                         { "cells", "Int", 0, VarValue(0) },
                         { "loads", "Int", 0, VarValue(0) } };
    auto aliasInit = Reward(100);
    aliasInit.code.instructions.insert(
      aliasInit.code.instructions.begin(),
      { FunctionCode::kOp_IAdd,
        { Name("inits"), Name("inits"), VarValue(1) } });
    auto aliasCell = Reward(11);
    aliasCell.code.instructions.insert(
      aliasCell.code.instructions.begin(),
      { FunctionCode::kOp_IAdd,
        { Name("cells"), Name("cells"), VarValue(1) } });
    auto aliasLoad = Reward(13);
    aliasLoad.code.instructions.insert(
      aliasLoad.code.instructions.begin(),
      { FunctionCode::kOp_IAdd,
        { Name("loads"), Name("loads"), VarValue(1) } });
    object.states = { { "",
                        { { "OnInit", aliasInit },
                          { "OnCellLoad", aliasCell },
                          { "OnLoad", aliasLoad } } } };
    script->objectTable.push_back(object);
    interaction.concurrent.program.bases.push_back(script);
  }
  auto Scripts() const { return interaction.Scripts(); }
  const auto& Metadata() const { return interaction.Metadata(); }
  void Enable(MpActor& owner) const
  {
    interaction.Enable(owner);
    // Fixture preparation only: aliases belong to an already running quest.
    REQUIRE(QuestJournal::Apply(
      owner, { { "quest", kParentQuest }, { "op", "start" } }));
    QuestJournal::Atomic(owner, [&] {
      QuestVmState state(QuestJournal::ScriptState(owner), {}, {});
      state.SetAliasBinding("4e50d:Skyrim.esm/alias:1", "6debb:Skyrim.esm");
      QuestJournal::SetScriptState(owner, state.Save());
    });
  }
};

int LoadCounterBeforeDispatch(MpActor& owner, const LoadProgram& content,
                              const char* name)
{
  const auto& variables =
    content.interaction.handlerBase->objectTable[0].variables;
  const auto declaration =
    std::find_if(variables.begin(), variables.end(),
                 [&](const auto& variable) { return variable.name == name; });
  REQUIRE(declaration != variables.end());
  REQUIRE(declaration->typeName == "Int");
  REQUIRE(declaration->value.GetType() == VarValue::kType_Integer);
  REQUIRE(static_cast<int32_t>(declaration->value) == 0);
  const auto values = QuestJournal::ScriptState(owner)
                        .at("scripts")
                        .at(kInteractionState)
                        .at("values");
  const std::string key = std::string("interactionbase/") + name;
  // Variables persists requested slots lazily. Before a callback is entered,
  // its declared zero may be absent; completed-callback checks stay strict.
  return values.contains(key) ? values.at(key).at("value").get<int>() : 0;
}

int LoadAliasField(MpActor& owner, const char* field)
{
  const auto heap = QuestJournal::ScriptState(owner);
  const auto root = "4e50d:Skyrim.esm/alias:1/loadalias";
  if (!heap.contains("scripts") || !heap.at("scripts").contains(root))
    return 0;
  const auto& values = heap.at("scripts").at(root).at("values");
  const std::string key = std::string("loadalias/") + field;
  return values.contains(key) ? values.at(key).at("value").get<int>() : 0;
}

void RequireReferenceLoad(const QuestJobRunner::Result& result)
{
  REQUIRE(result.kind == QuestJobRunner::ResultKind::ReferenceLoad);
  REQUIRE_FALSE(result.accepted);
}

struct LoadFixture : Fixture
{
  LoadProgram content;
  std::shared_ptr<QuestJobTime> time =
    std::make_shared<QuestJobTime>(QuestJobTime{ 1000000, 5000 });
  LoadFixture(bool waitInit = true, bool child = false, bool pollLoad = false)
    : content(waitInit, child, pollLoad)
  {
    content.Enable(Owner());
    BindInteractionTarget(server, Owner());
    storage->automatic = true;
    Tick();
    storage->submissions.clear();
    storage->automatic = false;
    Jobs().SetClock([time = time] { return *time; });
  }
  MpObjectReference& Target()
  {
    return server.worldState.GetFormAt<MpObjectReference>(kInteractionCopy);
  }
  QuestJobRunner::Result SubmitLoad(const RequestId& request,
                                    bool automatic = false)
  {
    return Jobs().SubmitLoad(Owner(), request, Target(), content.Metadata(),
                             content.Scripts(), automatic);
  }
  QuestJobRunner::Result AdvanceLoad(const RequestId& request)
  {
    return Jobs().Advance(Owner(), request, content.Metadata(),
                          content.Scripts());
  }
  QuestJobRunner::Result InspectLoad(const RequestId& request)
  {
    return Jobs().Inspect(Owner(), request);
  }
  QuestJobRunner::Result DrainLoad(const RequestId& request)
  {
    return Drain(*this, request, content.Metadata(), content.Scripts());
  }
  QuestJobState::Job Live(const RequestId& request)
  {
    const auto state = QuestJournal::JobState(Owner());
    const auto* job = state.FindLive(request.sequence);
    REQUIRE(job);
    return *job;
  }
  void AckLatest()
  {
    Tick();
    REQUIRE_FALSE(storage->submissions.empty());
    storage->Complete(storage->submissions.size() - 1);
    Tick();
  }
  void Until(const RequestId& request,
             const std::function<bool(const QuestJobState::Job&)>& reached)
  {
    for (size_t boundary = 0; boundary < 256; ++boundary) {
      if (reached(Live(request)))
        return;
      const auto result = InspectLoad(request);
      if (result.status == Status::Pending)
        AckLatest();
      else {
        RequireReady(result);
        RequirePending(AdvanceLoad(request));
      }
    }
    FAIL("Reference load did not reach its expected host boundary");
  }
  void UntilTerminalPending(const RequestId& request)
  {
    for (size_t boundary = 0; boundary < 512; ++boundary) {
      if (QuestJournal::JobState(Owner()).FindCompleted(request.sequence)) {
        RequirePending(InspectLoad(request));
        return;
      }
      const auto result = InspectLoad(request);
      if (result.status == Status::Pending)
        AckLatest();
      else {
        RequireReady(result);
        RequirePending(AdvanceLoad(request));
      }
    }
    FAIL("Reference load never reached its final receipt");
  }
};
}

TEST_CASE(
  "Reference initialization and later load phases require exact receipts",
  "[Quest][QuestJobs][ReferenceLoad][SaveReceipt]")
{
  LoadFixture f;
  const auto request = f.Jobs().NextRequest(f.Owner());
  RequirePending(f.SubmitLoad(request));
  const auto waiting = f.DrainLoad(request);
  RequireSleeping(waiting);
  RequireReferenceLoad(waiting);
  REQUIRE(f.Live(request).loadPhase == LoadPhase::Init);
  REQUIRE(f.Gold() == 12);
  REQUIRE_FALSE(
    QuestJournal::ReferenceInitialized(f.Owner(), kInteractionOriginal));
  REQUIRE(InteractionField(f.Owner(), "initprefixes") == 1);
  REQUIRE(InteractionField(f.Owner(), "initsuffixes") == 0);
  const auto earlyState = QuestJournal::JobState(f.Owner()).Save();
  const auto earlyHeap = QuestJournal::ScriptState(f.Owner());
  REQUIRE_THROWS(QuestJournal::AtomicJob(f.Owner(), request.sequence, [&] {
    QuestJournal::MarkReferenceInitialized(f.Owner(), kInteractionOriginal);
  }));
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == earlyState);
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == earlyHeap);
  REQUIRE_FALSE(
    QuestJournal::ReferenceInitialized(f.Owner(), kInteractionOriginal));
  REQUIRE(LoadAliasField(f.Owner(), "inits") == 0);
  REQUIRE(InteractionOrder(f.Owner()) == 0);
  REQUIRE(f.SubmitLoad(request).status == Status::Sleeping);
  const auto another = f.Jobs().NextRequest(f.Owner());
  REQUIRE_THROWS(f.SubmitLoad(another));
  REQUIRE_THROWS(f.Jobs().SubmitEvent(
    f.Owner(), another, f.Target(), "OnActivate", f.Owner(),
    f.content.Metadata(), f.content.Scripts()));
  f.time->steadyMs += 1000;
  RequirePending(f.AdvanceLoad(request));
  REQUIRE(f.Live(request).handler->cursor == "done");
  REQUIRE(f.Gold() == 15);
  REQUIRE_FALSE(
    QuestJournal::ReferenceInitialized(f.Owner(), kInteractionOriginal));
  f.AckLatest();
  REQUIRE_FALSE(
    QuestJournal::ReferenceInitialized(f.Owner(), kInteractionOriginal));
  f.Until(request, [&](const auto& job) {
    if (job.loadPhase == LoadPhase::Init)
      REQUIRE_FALSE(
        QuestJournal::ReferenceInitialized(f.Owner(), kInteractionOriginal));
    return job.loadPhase == LoadPhase::CellLoad;
  });
  RequirePending(f.InspectLoad(request));
  REQUIRE(QuestJournal::ReferenceInitialized(f.Owner(), kInteractionOriginal));
  REQUIRE(LoadCounterBeforeDispatch(f.Owner(), f.content, "cells") == 0);
  REQUIRE(LoadAliasField(f.Owner(), "cells") == 0);
  REQUIRE(InteractionOrder(f.Owner()) == 0);
  REQUIRE_THROWS(f.Jobs().SubmitEvent(
    f.Owner(), another, f.Target(), "OnActivate", f.Owner(),
    f.content.Metadata(), f.content.Scripts()));
  f.Tick();
  const auto failed = f.storage->submissions.size() - 1;
  const auto bytes = Serialized(f.storage->submissions[failed].forms);
  f.storage->Complete(
    failed, std::make_exception_ptr(std::runtime_error("init phase receipt")));
  f.Tick();
  REQUIRE(Serialized(f.storage->submissions.back().forms) == bytes);
  RequirePending(f.AdvanceLoad(request));
  REQUIRE(LoadCounterBeforeDispatch(f.Owner(), f.content, "cells") == 0);
  f.storage->Complete(failed);
  f.Tick();
  RequirePending(f.InspectLoad(request));
  f.AckLatest();
  f.UntilTerminalPending(request);
  REQUIRE(f.Gold() == 51);
  REQUIRE(InteractionField(f.Owner(), "initprefixes") == 1);
  REQUIRE(InteractionField(f.Owner(), "initsuffixes") == 1);
  REQUIRE(InteractionField(f.Owner(), "cells") == 1);
  REQUIRE(InteractionField(f.Owner(), "loads") == 1);
  REQUIRE(LoadAliasField(f.Owner(), "inits") == 0);
  REQUIRE(LoadAliasField(f.Owner(), "cells") == 1);
  REQUIRE(LoadAliasField(f.Owner(), "loads") == 1);
  REQUIRE(EventField(f.Owner(), "interactioninventoryobserver",
                     "interactioninventoryobserver/calls") == 6);
  const auto terminal = QuestJournal::JobState(f.Owner());
  REQUIRE(terminal.FindCompleted(request.sequence)->loadPhase ==
          LoadPhase::Done);
  RequirePending(f.SubmitLoad(request));
  f.Tick();
  const auto finalWrite = f.storage->submissions.size() - 1;
  const auto finalBytes = Serialized(f.storage->submissions[finalWrite].forms);
  f.storage->Complete(
    finalWrite,
    std::make_exception_ptr(std::runtime_error("final load receipt")));
  f.Tick();
  REQUIRE(Serialized(f.storage->submissions.back().forms) == finalBytes);
  RequirePending(f.InspectLoad(request));
  REQUIRE(f.Gold() == 51);
  f.AckLatest();
  const auto complete = f.InspectLoad(request);
  REQUIRE(complete.status == Status::Completed);
  RequireReferenceLoad(complete);
  const auto before = QuestJournal::JobState(f.Owner()).Save();
  REQUIRE(f.SubmitLoad(request).status == Status::Completed);
  REQUIRE_THROWS(f.Jobs().SubmitEvent(
    f.Owner(), request, f.Target(), "OnActivate", f.Owner(),
    f.content.Metadata(), f.content.Scripts()));
  REQUIRE_THROWS(f.Jobs().Submit(f.Owner(), request, kParentQuest, 10,
                                 f.content.Metadata(), f.content.Scripts()));
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == before);
  // A fresh load cycle skips Init but still delivers both later phases.
  const auto repeated = f.Jobs().NextRequest(f.Owner());
  RequirePending(f.SubmitLoad(repeated));
  REQUIRE(f.Gold() == 51);
  const auto repeat = f.DrainLoad(repeated);
  REQUIRE(repeat.status == Status::Completed);
  RequireReferenceLoad(repeat);
  REQUIRE(f.Gold() == 87);
  REQUIRE(InteractionField(f.Owner(), "initprefixes") == 1);
  REQUIRE(InteractionField(f.Owner(), "initsuffixes") == 1);
  REQUIRE(InteractionField(f.Owner(), "cells") == 2);
  REQUIRE(InteractionField(f.Owner(), "loads") == 2);
  REQUIRE(LoadAliasField(f.Owner(), "inits") == 0);
  REQUIRE(LoadAliasField(f.Owner(), "cells") == 2);
  REQUIRE(LoadAliasField(f.Owner(), "loads") == 2);
}

TEST_CASE(
  "An initialized OnLoad stack can be woken by a same-target interaction",
  "[Quest][QuestJobs][ReferenceLoad][Interactions][ConcurrentStacks]")
{
  LoadFixture f(false, false, true);
  const auto load = f.Jobs().NextRequest(f.Owner());
  f.SubmitLoad(load);
  RequireSleeping(f.DrainLoad(load));
  REQUIRE(f.Live(load).loadPhase == LoadPhase::Load);
  REQUIRE(QuestJournal::ReferenceInitialized(f.Owner(), kInteractionOriginal));
  REQUIRE(f.Gold() == 38);
  REQUIRE(InteractionField(f.Owner(), "loads") == 1);
  REQUIRE(LoadAliasField(f.Owner(), "loads") == 0);
  REQUIRE(InteractionOrder(f.Owner()) == 0);
  const auto token = f.Live(load).handler->token;
  const auto frame =
    QuestJournal::ScriptState(f.Owner()).at("continuations").at(token);
  const auto event = f.Jobs().NextRequest(f.Owner());
  RequirePending(
    f.Jobs().SubmitEvent(f.Owner(), event, f.Target(), "OnActivate", f.Owner(),
                         f.content.Metadata(), f.content.Scripts()));
  const auto activated =
    Drain(f, event, f.content.Metadata(), f.content.Scripts());
  REQUIRE(activated.status == Status::Completed);
  RequireInteraction(activated);
  REQUIRE(InteractionField(f.Owner(), "ready") == true);
  REQUIRE(InteractionField(f.Owner(), "interactions") == 1);
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations").at(token) ==
          frame);
  REQUIRE(f.Gold() == 38);
  RequireSleeping(f.InspectLoad(load));
  f.time->steadyMs += 1000;
  RequirePending(f.AdvanceLoad(load));
  const auto complete = f.DrainLoad(load);
  REQUIRE(complete.status == Status::Completed);
  RequireReferenceLoad(complete);
  REQUIRE(f.Gold() == 51);
  REQUIRE(InteractionField(f.Owner(), "loads") == 1);
  REQUIRE(LoadAliasField(f.Owner(), "loads") == 1);
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations").empty());
}

TEST_CASE("OnInit child waits and phase markers survive SQLite restarts",
          "[Quest][QuestJobs][ReferenceLoad][ChildStages][SQLite]")
{
  SqliteFiles files;
  LoadProgram content(true, true);
  RequestId request;
  {
    PartOne server;
    AttachDatabase(server, files);
    Initialize(server);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    content.Enable(owner);
    auto& target = BindInteractionTarget(server, owner);
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1000000, 5000 }; });
    request = jobs.NextRequest(owner);
    jobs.SubmitLoad(owner, request, target, content.Metadata(),
                    content.Scripts());
    RequireSleeping(DrainStored(server, owner, request, content.Metadata(),
                                content.Scripts()));
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 12);
    REQUIRE_FALSE(
      QuestJournal::ReferenceInitialized(owner, kInteractionOriginal));
    REQUIRE(InteractionField(owner, "initprefixes") == 1);
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1002000, 100 }; });
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    RequireReady(jobs.Inspect(owner, request));
    jobs.Advance(owner, request, content.Metadata(), content.Scripts());
    RequireReady(WaitForDurable(server, owner, request));
    REQUIRE(QuestJournal::JobState(owner)
              .FindLive(request.sequence)
              ->children.back()
              .cursor == "begin");
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 12);
    REQUIRE_FALSE(
      QuestJournal::ReferenceInitialized(owner, kInteractionOriginal));
    jobs.Advance(owner, request, content.Metadata(), content.Scripts());
    RequireSleeping(WaitForDurable(server, owner, request));
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 16);
    REQUIRE(QuestJournal::ScriptState(owner).at("continuations").size() == 2);
    REQUIRE_FALSE(
      QuestJournal::ReferenceInitialized(owner, kInteractionOriginal));
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1004000, 100 }; });
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    jobs.Advance(owner, request, content.Metadata(), content.Scripts());
    RequireReady(WaitForDurable(server, owner, request));
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 22);
    REQUIRE_FALSE(
      QuestJournal::ReferenceInitialized(owner, kInteractionOriginal));
    jobs.Advance(owner, request, content.Metadata(), content.Scripts());
    RequireReady(WaitForDurable(server, owner, request));
    REQUIRE(QuestJournal::JobState(owner)
              .FindLive(request.sequence)
              ->handler->cursor == "done");
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 25);
    REQUIRE(InteractionField(owner, "initsuffixes") == 1);
    REQUIRE_FALSE(
      QuestJournal::ReferenceInitialized(owner, kInteractionOriginal));
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& jobs = server.worldState.GetQuestJobs();
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    REQUIRE_FALSE(
      QuestJournal::ReferenceInitialized(owner, kInteractionOriginal));
    for (size_t boundary = 0; boundary < 32; ++boundary) {
      const auto state = QuestJournal::JobState(owner);
      if (state.FindLive(request.sequence)->loadPhase == LoadPhase::CellLoad)
        break;
      REQUIRE_FALSE(
        QuestJournal::ReferenceInitialized(owner, kInteractionOriginal));
      jobs.Advance(owner, request, content.Metadata(), content.Scripts());
      RequireReady(WaitForDurable(server, owner, request));
    }
    REQUIRE(
      QuestJournal::JobState(owner).FindLive(request.sequence)->loadPhase ==
      LoadPhase::CellLoad);
    REQUIRE(QuestJournal::ReferenceInitialized(owner, kInteractionOriginal));
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 25);
    REQUIRE(LoadCounterBeforeDispatch(owner, content, "cells") == 0);
    REQUIRE(LoadAliasField(owner, "inits") == 0);
    REQUIRE(InteractionOrder(owner) == 0);
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    const auto complete = DrainStored(server, owner, request,
                                      content.Metadata(), content.Scripts());
    REQUIRE(complete.status == Status::Completed);
    RequireReferenceLoad(complete);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 61);
    REQUIRE(InteractionField(owner, "initprefixes") == 1);
    REQUIRE(InteractionField(owner, "initsuffixes") == 1);
    REQUIRE(LoadAliasField(owner, "inits") == 0);
    REQUIRE(LoadAliasField(owner, "cells") == 1);
    REQUIRE(LoadAliasField(owner, "loads") == 1);
    REQUIRE(EventField(owner, "interactioninventoryobserver",
                       "interactioninventoryobserver/calls") == 8);
    REQUIRE(QuestJournal::ScriptState(owner).at("continuations").empty());
    const auto state = QuestJournal::JobState(owner);
    REQUIRE(state.FindCompleted(request.sequence)->loadPhase ==
            LoadPhase::Done);
    REQUIRE(state.FindCompleted(request.sequence)->completions.size() == 3);
  }
}

TEST_CASE(
  "Reference load admission and recovered target pins fail before handlers",
  "[Quest][QuestJobs][ReferenceLoad][Rollback]")
{
  const auto corruption = GENERATE(0, 1);
  const bool ownerFirst = GENERATE(false, true);
  LoadFixture f(false);
  const auto request = f.Jobs().NextRequest(f.Owner());
  f.server.CreateActor(kOwner + 1, { 1, 2, 3 }, 0, 0x3c);
  auto& guest = f.server.worldState.GetFormAt<MpActor>(kOwner + 1);
  guest.SetWorldInstance(f.Owner().GetWorldInstance());
  const auto before = QuestJournal::JobState(f.Owner()).Save();
  const auto heapBefore = QuestJournal::ScriptState(f.Owner());
  REQUIRE_THROWS(f.Jobs().SubmitLoad(
    guest, request, f.Target(), f.content.Metadata(), f.content.Scripts()));
  REQUIRE_THROWS(f.Jobs().SubmitLoad(
    f.Owner(), request, f.Owner(), f.content.Metadata(), f.content.Scripts()));
  const auto phase = f.Target().GetWorldInstance();
  f.Target().SetWorldInstance("campaign:" + std::to_string(kOwner) + ":other");
  REQUIRE_THROWS(f.SubmitLoad(request));
  f.Target().SetWorldInstance(phase);
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == before);
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == heapBefore);
  REQUIRE(f.Gold() == 10);
  f.SubmitLoad(request);
  f.AckLatest();
  auto ownerSave = f.Owner().GetChangeForm();
  const auto targetSave = f.Target().GetChangeForm();
  auto journal =
    Json::parse(ownerSave.dynamicFields.GetValueDump(QuestJournal::kProperty));
  auto state = QuestJobState::Read(journal.at("jobs"));
  auto* job = state.FindLive(request.sequence);
  REQUIRE(job);
  REQUIRE(job->kind == QuestJobState::Kind::ReferenceLoad);
  REQUIRE(job->referenceLoad->target == "6debb:Skyrim.esm");
  if (corruption == 0)
    job->referenceLoad->reference =
      f.Owner().GetChangeForm().formDesc.ToString();
  else
    job->referenceLoad->templates.push_back("13bbd:Skyrim.esm");
  journal["jobs"] = state.Save();
  ownerSave.dynamicFields.SetValueDump(QuestJournal::kProperty,
                                       journal.dump());
  PartOne restarted;
  restarted.AttachEspm(&GetEspmLoader());
  auto storage = std::make_shared<ControlledStorage>();
  restarted.AttachSaveStorage(storage);
  restarted.worldState.LoadChangeForm(ownerFirst ? ownerSave : targetSave,
                                      FormCallbacks::DoNothing());
  restarted.worldState.LoadChangeForm(ownerFirst ? targetSave : ownerSave,
                                      FormCallbacks::DoNothing());
  auto& owner = restarted.worldState.GetFormAt<MpActor>(kOwner);
  const auto checkpoint = QuestJournal::JobState(owner).Save();
  const auto heap = QuestJournal::ScriptState(owner);
  REQUIRE_THROWS(restarted.worldState.GetQuestJobs().Advance(
    owner, request, f.content.Metadata(), f.content.Scripts()));
  REQUIRE(QuestJournal::JobState(owner).Save() == checkpoint);
  REQUIRE(QuestJournal::ScriptState(owner) == heap);
  REQUIRE_FALSE(
    QuestJournal::ReferenceInitialized(owner, kInteractionOriginal));
  REQUIRE(owner.GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(storage->submissions.empty());
}

TEST_CASE("Automatic reference loads recover initialization without replay",
          "[Quest][QuestJobs][ReferenceLoad][SQLite][Scheduler]")
{
  SqliteFiles files;
  LoadProgram content;
  RequestId request;
  {
    PartOne server;
    AttachDatabase(server, files);
    Initialize(server);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    content.Enable(owner);
    auto& target = BindInteractionTarget(server, owner);
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1000000, 5000 }; });
    jobs.SetSchedulerFixtureContent(content.Metadata(), content.Scripts());
    request = jobs.NextRequest(owner);
    jobs.SubmitLoad(owner, request, target, content.Metadata(),
                    content.Scripts(), true);
    const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (jobs.Inspect(owner, request).status != Status::Sleeping &&
           std::chrono::steady_clock::now() < deadline) {
      server.worldState.Tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RequireSleeping(jobs.Inspect(owner, request));
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 12);
    REQUIRE_FALSE(
      QuestJournal::ReferenceInitialized(owner, kInteractionOriginal));
    REQUIRE(
      QuestJournal::JobState(owner).FindLive(request.sequence)->automatic);
  }
  {
    PartOne server;
    AttachDatabase(server, files);
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1002000, 100 }; });
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    RequireReady(jobs.Inspect(owner, request));
    server.worldState.Tick();
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 12);
    REQUIRE_FALSE(
      QuestJournal::ReferenceInitialized(owner, kInteractionOriginal));
    jobs.SetSchedulerFixtureContent(content.Metadata(), content.Scripts());
    const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (jobs.Inspect(owner, request).status != Status::Completed &&
           std::chrono::steady_clock::now() < deadline) {
      server.worldState.Tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto complete = jobs.Inspect(owner, request);
    REQUIRE(complete.status == Status::Completed);
    RequireReferenceLoad(complete);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 51);
    REQUIRE(QuestJournal::ReferenceInitialized(owner, kInteractionOriginal));
    REQUIRE(InteractionField(owner, "initprefixes") == 1);
    REQUIRE(InteractionField(owner, "initsuffixes") == 1);
    REQUIRE(InteractionField(owner, "cells") == 1);
    REQUIRE(InteractionField(owner, "loads") == 1);
    REQUIRE(LoadAliasField(owner, "inits") == 0);
    REQUIRE(LoadAliasField(owner, "cells") == 1);
    REQUIRE(LoadAliasField(owner, "loads") == 1);
  }
}

namespace {
using OccurrenceId = QuestJobRunner::OccurrenceId;
using CapturedOccurrence = QuestJobRunner::CapturedOccurrence;
using AdmissionStatus = QuestJobRunner::AdmissionStatus;

struct OccurrenceProgram
{
  InteractionProgram interaction{ false, false };
  OccurrenceProgram()
  {
    // These are actual reference event roots. Both handlers stay selected in
    // the same state so replay cannot hide behind a later ignored handler.
    auto& object = interaction.handlerBase->objectTable[0];
    object.variables = { { "enters", "Int", 0, VarValue(0) },
                         { "leaves", "Int", 0, VarValue(0) },
                         { "suffixes", "Int", 0, VarValue(0) },
                         { "sourceId", "Int", 0, VarValue(0) } };
    auto handler = [&](int reward, const char* counter, bool wait) {
      auto function = Reward(reward);
      function.params = { { "source", "ObjectReference" } };
      function.code.instructions.pop_back();
      function.code.instructions.insert(
        function.code.instructions.end(),
        { { FunctionCode::kOp_IAdd,
            { Name(counter), Name(counter), VarValue(1) } },
          { FunctionCode::kOp_CallMethod,
            { VarValue("GetFormID"), Name("source"), Name("sourceId"),
              VarValue(0) } } });
      if (wait) {
        function.code.instructions.insert(
          function.code.instructions.end(),
          { { FunctionCode::kOp_CallStatic,
              { VarValue("Utility"), VarValue("Wait"), Name("temp"),
                VarValue(1), VarValue(2.0) } },
            { FunctionCode::kOp_IAdd,
              { Name("suffixes"), Name("suffixes"), VarValue(1) } },
            { FunctionCode::kOp_CallMethod,
              { VarValue("AddItem"), Name("target"), Name("temp"), VarValue(3),
                Name("gold"), VarValue(5), VarValue(true) } } });
      }
      function.code.instructions.push_back(
        { FunctionCode::kOp_Return, { VarValue::None() } });
      return function;
    };
    object.states = {
      { "busy",
        { { "OnTriggerEnter", handler(3, "enters", true) },
          { "OnTriggerLeave", handler(2, "leaves", false) } } }
    };
    // The long retention test observes actual inventory rewards without an
    // unrelated decimal-order observer overflowing after hundreds of events.
    interaction.concurrent.metadata.at(kParentQuest).aliases.clear();
  }
  auto Scripts() const { return interaction.Scripts(); }
  const auto& Metadata() const { return interaction.Metadata(); }
  void Enable(MpActor& owner) const { interaction.Enable(owner); }
};

RequestId RequireAdmission(const QuestJobRunner::AdmissionResult& result,
                           AdmissionStatus expected)
{
  REQUIRE(result.status == expected);
  REQUIRE(result.error.empty());
  REQUIRE(result.request);
  return *result.request;
}

void RequireConsumed(const QuestJobRunner::AdmissionResult& result)
{
  REQUIRE(result.status == AdmissionStatus::Durable);
  REQUIRE_FALSE(result.request);
  REQUIRE(result.error.empty());
}

struct OccurrenceFixture : Fixture
{
  OccurrenceProgram content;
  std::shared_ptr<QuestJobTime> time =
    std::make_shared<QuestJobTime>(QuestJobTime{ 1000000, 5000 });
  OccurrenceFixture()
  {
    content.Enable(Owner());
    BindInteractionTarget(server, Owner());
    QuestScriptRuntime::ExecuteReferenceLoad(Owner(), kInteractionOriginal,
                                             content.Scripts());
    storage->automatic = true;
    Tick();
    storage->submissions.clear();
    storage->automatic = false;
    Jobs().SetClock([time = time] { return *time; });
  }
  MpObjectReference& Target()
  {
    return server.worldState.GetFormAt<MpObjectReference>(kInteractionCopy);
  }
  CapturedOccurrence Capture(const std::string& name = "OnTriggerEnter")
  {
    return Jobs().CaptureOccurrence(Owner(), Jobs().NextOccurrence(Owner()),
                                    Target(), name, Owner());
  }
  QuestJobRunner::AdmissionResult Admit(const CapturedOccurrence& captured)
  {
    return Jobs().AdmitOccurrence(Owner(), captured, content.Metadata(),
                                  content.Scripts());
  }
  QuestJobRunner::AdmissionResult Admission(const OccurrenceId& id)
  {
    return Jobs().InspectOccurrence(Owner(), id);
  }
  QuestJobRunner::Result Advance(const RequestId& request)
  {
    return Jobs().Advance(Owner(), request, content.Metadata(),
                          content.Scripts());
  }
  QuestJobRunner::Result DrainOccurrence(const RequestId& request)
  {
    return Drain(*this, request, content.Metadata(), content.Scripts());
  }
  void AckLatest()
  {
    Tick();
    REQUIRE_FALSE(storage->submissions.empty());
    storage->Complete(storage->submissions.size() - 1);
    Tick();
  }
  void SelectHandler(const RequestId& request)
  {
    for (size_t boundary = 0; boundary < 32; ++boundary) {
      const auto result = Jobs().Inspect(Owner(), request);
      if (result.status == Status::Pending)
        AckLatest();
      const auto jobs = QuestJournal::JobState(Owner());
      const auto* job = jobs.FindLive(request.sequence);
      REQUIRE(job);
      if (job->handler)
        return;
      RequirePending(Advance(request));
    }
    FAIL("Occurrence did not select its reference handler");
  }
};
}

TEST_CASE(
  "Occurrence admission keeps one identity through exact receipts and waits",
  "[Quest][QuestJobs][Occurrences][QuestIngress][SaveReceipt]")
{
  OccurrenceFixture f;
  const auto before = QuestJournal::JobState(f.Owner()).Save();
  const auto heapBefore = QuestJournal::ScriptState(f.Owner());
  const auto physical = f.Owner().GetChangeForm();
  const auto enter = f.Capture();
  const auto same = f.Capture();
  REQUIRE(enter.id == same.id);
  REQUIRE(enter.interaction == same.interaction);
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == before);
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == heapBefore);
  REQUIRE(f.Owner().GetChangeForm() == physical);
  REQUIRE(f.storage->submissions.empty());
  const auto request =
    RequireAdmission(f.Admit(enter), AdmissionStatus::Pending);
  RequireSameRequest(
    request, RequireAdmission(f.Admit(same), AdmissionStatus::Pending));
  const auto admitted = QuestJournal::JobState(f.Owner()).Save();
  const auto admittedHeap = QuestJournal::ScriptState(f.Owner());
  RequirePending(f.Advance(request));
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == admitted);
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == admittedHeap);
  REQUIRE(f.storage->submissions.empty());
  RequireAdmission(f.Admission(enter.id), AdmissionStatus::Pending);
  REQUIRE(f.Gold() == 10);
  f.Tick();
  const auto failed = f.storage->submissions.size() - 1;
  const auto bytes = Serialized(f.storage->submissions[failed].forms);
  f.storage->Complete(
    failed,
    std::make_exception_ptr(std::runtime_error("occurrence admission")));
  f.Tick();
  REQUIRE(Serialized(f.storage->submissions.back().forms) == bytes);
  RequireAdmission(f.Admission(enter.id), AdmissionStatus::Pending);
  f.storage->Complete(
    failed); // An old successful callback cannot acknowledge the retry.
  f.Tick();
  RequireAdmission(f.Admit(enter), AdmissionStatus::Pending);
  f.AckLatest();
  RequireSameRequest(
    request,
    RequireAdmission(f.Admission(enter.id), AdmissionStatus::Durable));

  f.SelectHandler(request);
  RequirePending(f.Advance(request));
  REQUIRE(f.Gold() == 13);
  REQUIRE(InteractionField(f.Owner(), "enters") == 1);
  RequirePending(f.Jobs().Inspect(f.Owner(), request));
  // Admission is already durable even while a later handler segment is not.
  RequireSameRequest(
    request, RequireAdmission(f.Admit(enter), AdmissionStatus::Durable));
  f.AckLatest();
  RequireSleeping(f.Jobs().Inspect(f.Owner(), request));
  const auto leave = f.Capture("OnTriggerLeave");
  const auto leaveRequest =
    RequireAdmission(f.Admit(leave), AdmissionStatus::Pending);
  RequireAdmission(f.Admission(enter.id), AdmissionStatus::Durable);
  f.time->steadyMs += 2000;
  RequireReady(f.Jobs().Inspect(f.Owner(), request));
  const auto blocked = QuestJournal::JobState(f.Owner()).Save();
  const auto blockedHeap = QuestJournal::ScriptState(f.Owner());
  REQUIRE_THROWS(f.Advance(request));
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == blocked);
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == blockedHeap);
  REQUIRE(f.Gold() == 13);
  REQUIRE(f.DrainOccurrence(leaveRequest).status == Status::Completed);
  REQUIRE(f.Gold() == 15);
  REQUIRE(InteractionField(f.Owner(), "leaves") == 1);
  RequireReady(f.Jobs().Inspect(f.Owner(), request));
  REQUIRE(f.DrainOccurrence(request).status == Status::Completed);
  REQUIRE(f.Gold() == 20);
  REQUIRE(InteractionField(f.Owner(), "enters") == 1);
  REQUIRE(InteractionField(f.Owner(), "suffixes") == 1);
  const auto complete = QuestJournal::JobState(f.Owner()).Save();
  RequireSameRequest(
    request, RequireAdmission(f.Admit(enter), AdmissionStatus::Durable));
  auto conflict = enter;
  conflict.interaction.name = "OnTriggerLeave";
  REQUIRE_THROWS(f.Admit(conflict));
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == complete);
  REQUIRE(f.Gold() == 20);
  const auto publicState = QuestJournal::Snapshot(f.Owner());
  REQUIRE_FALSE(publicState.contains("ingress"));
  REQUIRE_FALSE(publicState.contains("jobs"));
  REQUIRE(publicState.dump().find(enter.id.campaignIdentity) ==
          std::string::npos);
}

TEST_CASE(
  "Occurrence retirement remains bounded around a sleeping earlier event",
  "[Quest][QuestJobs][Occurrences][QuestIngress][Retention]")
{
  OccurrenceFixture f;
  const auto sleeping = f.Capture();
  const auto sleepingRequest =
    RequireAdmission(f.Admit(sleeping), AdmissionStatus::Pending);
  RequireSleeping(f.DrainOccurrence(sleepingRequest));
  const auto stack = QuestJournal::ScriptState(f.Owner()).at("continuations");
  // Ordinary numbered stages share job IDs, but never consume producer IDs.
  const auto beforeStage = f.Jobs().NextOccurrence(f.Owner());
  const auto stage = f.Jobs().NextRequest(f.Owner());
  f.Jobs().Submit(f.Owner(), stage, kRetentionQuest, 10, f.content.Metadata(),
                  f.content.Scripts());
  REQUIRE(f.DrainOccurrence(stage).status == Status::Completed);
  REQUIRE(f.Jobs().NextOccurrence(f.Owner()) == beforeStage);
  CapturedOccurrence oldest, newest;
  RequestId newestRequest;
  for (size_t index = 0; index < 300; ++index) {
    const auto event = f.Capture("OnTriggerLeave");
    const auto request =
      RequireAdmission(f.Admit(event), AdmissionStatus::Pending);
    REQUIRE(request.sequence != event.id.sequence);
    if (index == 0)
      oldest = event;
    newest = event;
    newestRequest = request;
    REQUIRE(f.DrainOccurrence(request).status == Status::Completed);
    const auto state = QuestJournal::JobState(f.Owner());
    REQUIRE(state.FindOccurrence(sleeping.id.sequence));
    REQUIRE(state.FindOccurrence(sleeping.id.sequence)->id ==
            sleepingRequest.sequence);
    REQUIRE(state.ingress.retiredThrough == 0);
    REQUIRE(state.ingress.retiredRanges.size() <= 1);
    REQUIRE(state.completed.size() <= QuestJobState::MaxRecentCompleted);
    REQUIRE(state.live.size() <= QuestJobState::MaxLive);
    REQUIRE(state.Save().dump().size() < 2 * 1024 * 1024);
    f.storage->submissions.clear();
  }
  const auto state = QuestJournal::JobState(f.Owner());
  REQUIRE(state.IsOccurrenceRetired(oldest.id.sequence));
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations") == stack);
  REQUIRE(f.Gold() == 614);
  RequireConsumed(f.Admit(oldest));
  RequireSameRequest(
    newestRequest,
    RequireAdmission(f.Admit(newest), AdmissionStatus::Durable));
  f.time->steadyMs += 2000;
  REQUIRE(f.DrainOccurrence(sleepingRequest).status == Status::Completed);
  REQUIRE(f.Gold() == 619);
  const auto next = f.Capture("OnTriggerLeave");
  const auto nextRequest =
    RequireAdmission(f.Admit(next), AdmissionStatus::Pending);
  // Admission A remains acknowledged when B retires A's detailed result.
  RequireConsumed(f.Admission(sleeping.id));
  RequireConsumed(f.Admission(oldest.id));
  REQUIRE(f.DrainOccurrence(nextRequest).status == Status::Completed);
  const auto retained = QuestJournal::JobState(f.Owner()).Save();
  const auto gold = f.Gold();
  f.server.worldState.DestroyForm(kInteractionCopy);
  auto opaque = oldest;
  opaque.interaction.name = "OnDeath";
  opaque.interaction.targetReference = "not a reference";
  // No target or owned PEX storage is needed to report an opaque consumption.
  RequireConsumed(f.Jobs().AdmitOccurrence(f.Owner(), opaque));
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == retained);
  REQUIRE(f.Gold() == gold);
}

TEST_CASE("Occurrence envelopes reject mismatched identity and physical "
          "context atomically",
          "[Quest][QuestJobs][Occurrences][QuestIngress][Rollback]")
{
  OccurrenceFixture f;
  const auto captured = f.Capture("OnTriggerLeave");
  const auto before = QuestJournal::JobState(f.Owner()).Save();
  const auto heap = QuestJournal::ScriptState(f.Owner());
  const std::vector<std::function<void(CapturedOccurrence&)>> corrupt = {
    [](auto& v) { ++v.id.generation; },
    [](auto& v) { v.id.owner = "ff006199"; },
    [](auto& v) {
      v.id.campaignIdentity[0] = v.id.campaignIdentity[0] == 'a' ? 'b' : 'a';
    },
    [](auto& v) { ++v.id.sequence; },
    [](auto& v) { v.actor = "ff006199"; },
    [](auto& v) { v.instance += ":different"; },
    [](auto& v) { v.interaction.name = "OnActivate"; },
    [](auto& v) {
      v.interaction.targetTemplates.push_back("13bbd:Skyrim.esm");
    },
    [](auto& v) {
      v.interaction.sourceReference = v.interaction.targetReference;
    }
  };
  for (size_t index = 0; index < corrupt.size(); ++index) {
    INFO("Occurrence envelope corruption " << index);
    auto invalid = captured;
    corrupt[index](invalid);
    REQUIRE_THROWS(f.Admit(invalid));
    REQUIRE(QuestJournal::JobState(f.Owner()).Save() == before);
    REQUIRE(QuestJournal::ScriptState(f.Owner()) == heap);
    REQUIRE(f.Gold() == 10);
    REQUIRE(f.storage->submissions.empty());
  }
  auto unknown = captured.id;
  ++unknown.sequence;
  REQUIRE_THROWS(f.Admission(unknown));
  REQUIRE_THROWS(f.Jobs().CaptureOccurrence(f.Owner(), captured.id, f.Target(),
                                            "OnActivate", f.Owner()));
  const auto originalPhase = f.Target().GetWorldInstance();
  f.Target().SetWorldInstance("campaign:" + std::to_string(kOwner) +
                              ":elsewhere");
  REQUIRE_THROWS(f.Admit(captured));
  f.Target().SetWorldInstance(originalPhase);
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == before);
  const auto request =
    RequireAdmission(f.Admit(captured), AdmissionStatus::Pending);
  REQUIRE(f.DrainOccurrence(request).status == Status::Completed);
  REQUIRE(f.Gold() == 12);
  const auto originalOwnerPhase = f.Owner().GetWorldInstance();
  f.Owner().SetWorldInstance("campaign:" + std::to_string(kOwner) +
                             ":departed");
  RequireSameRequest(
    request,
    RequireAdmission(f.Jobs().AdmitOccurrence(f.Owner(), captured),
                     AdmissionStatus::Durable));
  REQUIRE_THROWS(f.Jobs().Inspect(f.Owner(), request));
  REQUIRE_THROWS(f.Advance(request));
  f.Owner().SetWorldInstance(originalOwnerPhase);
}

TEST_CASE(
  "Occurrence source pins distinguish a visiting player from the owner",
  "[Quest][QuestJobs][Occurrences][QuestIngress]")
{
  OccurrenceFixture f;
  f.server.CreateActor(kOwner + 1, { 1, 2, 3 }, 0, 0x3c);
  auto& guest = f.server.worldState.GetFormAt<MpActor>(kOwner + 1);
  guest.SetWorldInstance(f.Owner().GetWorldInstance());
  const auto id = f.Jobs().NextOccurrence(guest);
  const auto ownerCapture = f.Jobs().CaptureOccurrence(
    f.Owner(), id, f.Target(), "OnTriggerLeave", f.Owner());
  const auto guestCapture =
    f.Jobs().CaptureOccurrence(guest, id, f.Target(), "OnTriggerLeave", guest);
  REQUIRE(ownerCapture.interaction.source == guestCapture.interaction.source);
  REQUIRE(ownerCapture.interaction.sourceReference !=
          guestCapture.interaction.sourceReference);
  REQUIRE_THROWS(f.Jobs().CaptureOccurrence(f.Owner(), id, f.Target(),
                                            "OnTriggerLeave", guest));
  const auto before = QuestJournal::JobState(f.Owner()).Save();
  REQUIRE_THROWS(f.Admit(guestCapture));
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == before);
  const auto result = f.Jobs().AdmitOccurrence(
    guest, guestCapture, f.content.Metadata(), f.content.Scripts());
  const auto request = RequireAdmission(result, AdmissionStatus::Pending);
  f.AckLatest();
  RequireSameRequest(request,
                     RequireAdmission(f.Jobs().InspectOccurrence(guest, id),
                                      AdmissionStatus::Durable));
  REQUIRE(QuestJournal::JobState(guest).FindLive(request.sequence)->actor ==
          guest.GetChangeForm().formDesc.ToString());
  REQUIRE_THROWS(f.Admit(ownerCapture));
  REQUIRE(f.Gold() == 10);
}

TEST_CASE("Occurrence admission refuses capacity and save backpressure "
          "without consuming identity",
          "[Quest][QuestJobs][Occurrences][QuestIngress][SaveReceipt]")
{
  OccurrenceFixture f;
  SECTION("No storage")
  {
    const auto captured = f.Capture("OnTriggerLeave");
    const auto before = QuestJournal::JobState(f.Owner()).Save();
    f.server.worldState.AttachSaveStorage(nullptr);
    REQUIRE_THROWS(f.Admit(captured));
    REQUIRE(QuestJournal::JobState(f.Owner()).Save() == before);
    REQUIRE(f.Gold() == 10);
    f.server.worldState.AttachSaveStorage(f.storage);
    RequireAdmission(f.Admit(captured), AdmissionStatus::Pending);
  }
  SECTION("Live roots")
  {
    std::vector<RequestId> requests;
    for (size_t index = 0; index < QuestJobState::MaxLive; ++index) {
      requests.push_back(RequireAdmission(f.Admit(f.Capture("OnTriggerLeave")),
                                          AdmissionStatus::Pending));
      f.AckLatest();
    }
    const auto captured = f.Capture("OnTriggerLeave");
    const auto before = QuestJournal::JobState(f.Owner()).Save();
    REQUIRE_THROWS(f.Admit(captured));
    REQUIRE(QuestJournal::JobState(f.Owner()).Save() == before);
    REQUIRE(f.Jobs().NextOccurrence(f.Owner()) == captured.id);
    REQUIRE(f.Gold() == 10);
    REQUIRE(f.DrainOccurrence(requests.front()).status == Status::Completed);
    RequireAdmission(f.Admit(captured), AdmissionStatus::Pending);
  }
  SECTION("Immutable save queue")
  {
    const auto captured = f.Capture("OnTriggerLeave");
    for (size_t index = 0; index < 256; ++index)
      QuestJournal::AtomicDurable(f.Owner(), [] {});
    const auto before = QuestJournal::JobState(f.Owner()).Save();
    const auto heap = QuestJournal::ScriptState(f.Owner());
    REQUIRE_THROWS(f.Admit(captured));
    REQUIRE(QuestJournal::JobState(f.Owner()).Save() == before);
    REQUIRE(QuestJournal::ScriptState(f.Owner()) == heap);
    REQUIRE(f.Jobs().NextOccurrence(f.Owner()) == captured.id);
    REQUIRE(f.Gold() == 10);
    REQUIRE(f.storage->submissions.empty());
    f.AckLatest();
    RequireAdmission(f.Admit(captured), AdmissionStatus::Pending);
  }
}

TEST_CASE(
  "Native occurrence eligibility waits for the exact initialization receipt",
  "[Quest][QuestJobs][Occurrences][QuestIngress][ReferenceLoad][SaveReceipt]")
{
  LoadFixture f(false, false, true);
  const auto id = f.Jobs().NextOccurrence(f.Owner());
  REQUIRE_THROWS(f.Jobs().CaptureOccurrence(f.Owner(), id, f.Target(),
                                            "OnTriggerEnter", f.Owner()));
  const auto load = f.Jobs().NextRequest(f.Owner());
  f.SubmitLoad(load);
  f.Until(load, [](const auto& job) {
    return job.loadPhase == LoadPhase::CellLoad;
  });
  REQUIRE(QuestJournal::ReferenceInitialized(f.Owner(), kInteractionOriginal));
  RequirePending(f.InspectLoad(load));
  const CapturedOccurrence planned{
    id, f.Owner().GetChangeForm().formDesc.ToString(),
    f.Owner().GetWorldInstance(),
    QuestScriptRuntime::CaptureInteraction(f.Owner(), f.Target(),
                                           "OnTriggerEnter", f.Owner())
  };
  const auto before = QuestJournal::JobState(f.Owner()).Save();
  const auto refuse = [&] {
    REQUIRE_THROWS(f.Jobs().CaptureOccurrence(f.Owner(), id, f.Target(),
                                              "OnTriggerEnter", f.Owner()));
    REQUIRE_THROWS(f.Jobs().AdmitOccurrence(
      f.Owner(), planned, f.content.Metadata(), f.content.Scripts()));
    REQUIRE(QuestJournal::JobState(f.Owner()).Save() == before);
    REQUIRE(f.Jobs().NextOccurrence(f.Owner()) == id);
  };
  refuse();
  f.Tick();
  const auto failed = f.storage->submissions.size() - 1;
  f.storage->Complete(
    failed,
    std::make_exception_ptr(std::runtime_error("initialization receipt")));
  f.Tick();
  refuse();
  f.storage->Complete(failed);
  f.Tick();
  refuse();
  f.AckLatest();
  RequireSleeping(f.DrainLoad(load));
  REQUIRE(f.Live(load).loadPhase == LoadPhase::Load);
  const auto captured = f.Jobs().CaptureOccurrence(
    f.Owner(), id, f.Target(), "OnTriggerEnter", f.Owner());
  const auto event = RequireAdmission(
    f.Jobs().AdmitOccurrence(f.Owner(), captured, f.content.Metadata(),
                             f.content.Scripts()),
    AdmissionStatus::Pending);
  REQUIRE(Drain(f, event, f.content.Metadata(), f.content.Scripts()).status ==
          Status::Completed);
  RequireSleeping(f.InspectLoad(load));
  RequireAdmission(f.Jobs().InspectOccurrence(f.Owner(), id),
                   AdmissionStatus::Durable);
}

namespace {
void AttachOccurrenceDatabase(PartOne& server, const SqliteFiles& files,
                              bool ownerFirst)
{
  server.AttachEspm(&GetEspmLoader());
  auto database = std::make_shared<SqliteDatabase>(files.File());
  std::vector<MpChangeForm> saved;
  database->Iterate([&](const MpChangeForm& form) { saved.push_back(form); },
                    std::nullopt);
  const auto owner = FormDesc::FromFormId(kOwner, server.worldState.espmFiles);
  std::stable_sort(
    saved.begin(), saved.end(), [&](const auto& a, const auto& b) {
      const bool aOwner = a.formDesc == owner, bOwner = b.formDesc == owner;
      return aOwner != bOwner ? aOwner == ownerFirst
                              : a.formDesc.ToString() < b.formDesc.ToString();
    });
  using Storage =
    Viet::AsyncSaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>;
  // Read through an independent SQLite connection and replay its real rows
  // in either order. This intentionally does not force the owner disabled.
  server.worldState.AttachSaveStorage(std::make_shared<Storage>(
    database, spdlog::default_logger(), "occurrence-restart", 1));
  WorldState::RestorationScope restoration(server.worldState);
  for (const auto& form : saved)
    server.worldState.LoadChangeForm(form, FormCallbacks::DoNothing());
}

void FlushOccurrenceSetup(PartOne& server, MpActor& owner)
{
  const auto receipt = QuestJournal::AtomicDurable(owner, [] {});
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (receipt->GetState() == SaveReceipt::State::Pending &&
         std::chrono::steady_clock::now() < deadline) {
    server.worldState.Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(receipt->GetState() == SaveReceipt::State::Durable);
}
}

TEST_CASE("SQLite occurrence recovery preserves admission and suspended "
          "handler effects in either restore order",
          "[Quest][QuestJobs][Occurrences][QuestIngress][SQLite]")
{
  const bool ownerFirst = GENERATE(false, true);
  SqliteFiles files;
  OccurrenceProgram content;
  CapturedOccurrence captured;
  RequestId request;
  {
    PartOne server;
    AttachDatabase(server, files);
    Initialize(server);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    content.Enable(owner);
    auto& target = BindInteractionTarget(server, owner);
    QuestScriptRuntime::ExecuteReferenceLoad(owner, kInteractionOriginal,
                                             content.Scripts());
    FlushOccurrenceSetup(server, owner);
    auto& jobs = server.worldState.GetQuestJobs();
    captured = jobs.CaptureOccurrence(owner, jobs.NextOccurrence(owner),
                                      target, "OnTriggerEnter", owner);
    request = RequireAdmission(jobs.AdmitOccurrence(owner, captured,
                                                    content.Metadata(),
                                                    content.Scripts()),
                               AdmissionStatus::Pending);
    // No Tick: neither the new producer cursor nor its job reached SQLite.
  }
  {
    PartOne server;
    AttachOccurrenceDatabase(server, files, ownerFirst);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = server.worldState.GetQuestJobs();
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 10);
    REQUIRE(jobs.NextOccurrence(owner) == captured.id);
    REQUIRE_THROWS(jobs.InspectOccurrence(owner, captured.id));
    RequireSameRequest(request,
                       RequireAdmission(jobs.AdmitOccurrence(
                                          owner, captured, content.Metadata(),
                                          content.Scripts()),
                                        AdmissionStatus::Pending));
    RequireReady(WaitForDurable(server, owner, request));
    RequireSameRequest(
      request,
      RequireAdmission(jobs.InspectOccurrence(owner, captured.id),
                       AdmissionStatus::Durable));
  }
  {
    PartOne server;
    AttachOccurrenceDatabase(server, files, !ownerFirst);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1000000, 5000 }; });
    // Retained duplicate admission also needs no new content load.
    RequireSameRequest(request,
                       RequireAdmission(jobs.AdmitOccurrence(owner, captured),
                                        AdmissionStatus::Durable));
    RequireSleeping(DrainStored(server, owner, request, content.Metadata(),
                                content.Scripts()));
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 13);
    REQUIRE(InteractionField(owner, "enters") == 1);
    REQUIRE(QuestJournal::JobState(owner).ingress.next ==
            captured.id.sequence + 1);
  }
  {
    PartOne server;
    AttachOccurrenceDatabase(server, files, ownerFirst);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = server.worldState.GetQuestJobs();
    jobs.SetClock([] { return QuestJobTime{ 1003000, 100 }; });
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 13);
    RequireAdmission(jobs.InspectOccurrence(owner, captured.id),
                     AdmissionStatus::Durable);
    RequireSameRequest(request,
                       RequireAdmission(jobs.AdmitOccurrence(owner, captured),
                                        AdmissionStatus::Durable));
    REQUIRE(DrainStored(server, owner, request, content.Metadata(),
                        content.Scripts())
              .status == Status::Completed);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 18);
    REQUIRE(InteractionField(owner, "enters") == 1);
    REQUIRE(InteractionField(owner, "suffixes") == 1);
  }
  {
    PartOne server;
    AttachOccurrenceDatabase(server, files, !ownerFirst);
    auto& owner = server.worldState.GetFormAt<MpActor>(kOwner);
    auto& jobs = server.worldState.GetQuestJobs();
    RequireSameRequest(request,
                       RequireAdmission(jobs.AdmitOccurrence(owner, captured),
                                        AdmissionStatus::Durable));
    REQUIRE(jobs.Inspect(owner, request).status == Status::Completed);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 18);
    REQUIRE(QuestJournal::ScriptState(owner).at("continuations").empty());
  }
}

TEST_CASE(
  "Automatic occurrence execution preserves its durable admission identity",
  "[Quest][QuestJobs][Occurrences][QuestIngress][Scheduler]")
{
  OccurrenceFixture f;
  f.Jobs().SetSchedulerFixtureContent(f.content.Metadata(),
                                      f.content.Scripts());
  const auto captured = f.Capture("OnTriggerLeave");
  const auto request = RequireAdmission(
    f.Jobs().AdmitOccurrence(f.Owner(), captured, f.content.Metadata(),
                             f.content.Scripts(), true),
    AdmissionStatus::Pending);
  f.storage->automatic = true;
  for (size_t pass = 0; pass < 256 &&
       f.Jobs().Inspect(f.Owner(), request).status != Status::Completed;
       ++pass)
    f.Tick();
  REQUIRE(f.Jobs().Inspect(f.Owner(), request).status == Status::Completed);
  RequireSameRequest(
    request,
    RequireAdmission(f.Admission(captured.id), AdmissionStatus::Durable));
  RequireSameRequest(
    request,
    RequireAdmission(f.Jobs().AdmitOccurrence(f.Owner(), captured),
                     AdmissionStatus::Durable));
  REQUIRE(f.Gold() == 12);
  REQUIRE(InteractionField(f.Owner(), "leaves") == 1);
}

TEST_CASE("Durable admission survives reentrant observation of later commits",
          "[Quest][QuestJobs][Occurrences][QuestIngress][SaveReceipt]")
{
  struct ObservingStorage : ControlledStorage
  {
    std::function<void()> observe;
    bool SupportsAtomicBatches() const noexcept override
    {
      if (observe)
        observe();
      return true;
    }
  };
  OccurrenceFixture f;
  auto storage = std::make_shared<ObservingStorage>();
  storage->automatic = false;
  f.server.worldState.AttachSaveStorage(storage);
  f.storage = storage;
  const auto first = f.Capture();
  const auto firstRequest =
    RequireAdmission(f.Admit(first), AdmissionStatus::Pending);
  f.AckLatest();
  f.SelectHandler(firstRequest);
  std::vector<AdmissionStatus> observed;
  std::exception_ptr error;
  // The storage capability callback runs at transaction admission and again
  // during commit while the runner's owner barrier is busy. It cannot throw.
  storage->observe = [&] {
    try {
      observed.push_back(f.Admission(first.id).status);
    } catch (...) {
      error = std::current_exception();
    }
  };
  RequirePending(f.Advance(firstRequest));
  REQUIRE_FALSE(error);
  REQUIRE(observed.size() >= 2);
  REQUIRE(std::all_of(observed.begin(), observed.end(), [](const auto status) {
    return status == AdmissionStatus::Durable;
  }));
  storage->observe = {};
  f.AckLatest();
  const auto second = f.Capture("OnTriggerLeave");
  observed.clear();
  storage->observe = [&] {
    try {
      observed.push_back(f.Admission(first.id).status);
    } catch (...) {
      error = std::current_exception();
    }
  };
  RequireAdmission(f.Admit(second), AdmissionStatus::Pending);
  storage->observe = {};
  REQUIRE_FALSE(error);
  REQUIRE(observed.size() >= 2);
  REQUIRE(std::all_of(observed.begin(), observed.end(), [](const auto status) {
    return status == AdmissionStatus::Durable;
  }));
  RequireAdmission(f.Admission(first.id), AdmissionStatus::Durable);
  RequireAdmission(f.Admission(second.id), AdmissionStatus::Pending);
}
