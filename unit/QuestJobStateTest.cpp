#include "QuestJobState.h"
#include "QuestJournal.h"
#include "TestUtils.hpp"
#include "save_storages/ISaveStorage.h"
#include <catch2/catch_all.hpp>
#include <functional>

extern espm::Loader& GetEspmLoader();

namespace {
using Json = nlohmann::json;
constexpr uint32_t kJobQuest = 0x4e50d;

QuestJobState Begin()
{
  QuestJobState state;
  QuestJobState::Job job;
  job.id = 1;
  job.command = "command-one";
  job.actor = job.owner = "3000";
  job.quest = "4e50d:Skyrim.esm";
  job.contentHash = std::string(64, 'a');
  job.stage = 10;
  state.active = job;
  state.nextJob = 2;
  return state;
}

QuestJobState Waiting()
{
  auto state = Begin();
  auto& job = *state.active;
  job.cursor = "waiting";
  job.accepted = true;
  job.segment = 1;
  job.instructions = 3;
  job.steps = 2;
  job.script = "QF_Test";
  job.function = "Fragment_0";
  job.awaitKey = "operation";
  job.awaitKind = "bool";
  job.token = job.NextToken();
  ++job.nextOperation;
  job.events.push_back({ "14:Skyrim.esm", "f:Skyrim.esm", "", 2, true });
  return state;
}

QuestJobState Done(QuestJobState state)
{
  auto job = *state.active;
  if (!job.token.empty())
    job.completions[job.token] = job.awaitKind == "timer"
      ? QuestJobState::Completion::None()
      : QuestJobState::Completion(true);
  job.cursor = "done";
  job.log = 0;
  job.script.clear();
  job.function.clear();
  job.token.clear();
  job.awaitKey.clear();
  job.awaitKind.clear();
  job.timer.reset();
  job.failure.clear();
  job.eventHead = static_cast<uint32_t>(job.events.size());
  state.active.reset();
  state.completed.push_back(job);
  return state;
}

Json Heap(const QuestJobState::Job& job,
          const QuestJobState::Stage* selected = nullptr,
          uint32_t instructions = 0)
{
  const auto& stage =
    selected ? *selected : static_cast<const QuestJobState::Stage&>(job);
  const auto* identity =
    stage.handlerIdentity ? &*stage.handlerIdentity : nullptr;
  Json frame = {
    { "schemaVersion", 2 },
    { "scope", job.Scope(stage) },
    { "receiver",
      { { "type", 0 },
        { "value", identity ? identity->receiver : Json(stage.quest) } } },
    { "kind", "method" },
    { "rootScript", identity ? identity->rootScript : stage.script },
    { "script", stage.script },
    { "state", identity ? identity->state : "" },
    { "function", stage.function },
    { "contentHashes",
      Json::array(
        { { { "script", stage.script }, { "hash", stage.contentHash } } }) },
    { "resultType", stage.awaitKind == "timer" ? "None" : "Bool" },
    { "instruction", 1 },
    { "executedInstructions", instructions ? instructions : job.instructions },
    { "locals", Json::array() },
    { "callers", Json::array() }
  };
  return { { "schemaVersion", 6 },
           { "scripts", Json::object() },
           { "arrays", Json::object() },
           { "globals", Json::object() },
           { "aliases", Json::object() },
           { "continuations", { { stage.token, frame } } } };
}

QuestJobState Nested(bool waitingChild = false)
{
  auto state = Waiting();
  auto& job = *state.active;
  job.child = 1;
  job.awaitKey = "quest-stage";
  job.awaitKind = "stage";
  QuestJobState::Stage child;
  child.node = 1;
  child.quest = "2aa57:Skyrim.esm";
  child.contentHash = std::string(64, 'b');
  child.stage = 20;
  if (waitingChild) {
    child.cursor = "waiting";
    child.accepted = true;
    child.script = "QF_Child";
    child.function = "Fragment_1";
    child.awaitKey = "external";
    child.awaitKind = "bool";
    child.token = job.NextToken();
    ++job.nextOperation;
    job.segment = 2;
    job.instructions = 7;
    job.steps = 4;
  }
  job.children.push_back(child);
  return state;
}

Json NestedHeap(const QuestJobState::Job& job)
{
  auto heap = Heap(job, &job, 3);
  for (const auto& child : job.children) {
    if (child.cursor == "waiting") {
      const auto childHeap = Heap(job, &child);
      heap["continuations"].update(childHeap["continuations"]);
    }
  }
  return heap;
}

void FinishChild(QuestJobState::Job& job)
{
  auto& child = job.children.back();
  if (!child.token.empty())
    job.completions[child.token] = true;
  const auto node = child.node;
  const auto quest = child.quest;
  const auto hash = child.contentHash;
  const auto stage = child.stage;
  child = QuestJobState::Stage{};
  child.node = node;
  child.quest = quest;
  child.contentHash = hash;
  child.stage = stage;
  child.cursor = "done";
  child.accepted = true;
  ++job.segment;
  ++job.instructions;
}

Json Legacy(const QuestJobState& state, uint64_t version = 1)
{
  auto saved = state.Save();
  saved["schemaVersion"] = version;
  saved.erase("retiredThrough");
  auto strip = [version](Json& job) {
    job.erase("sequenced");
    if (version < 4) {
      for (const char* field : { "handlerIdentity", "eventHead", "nextHandler",
                                 "dispatch", "handler" })
        job.erase(field);
      for (auto& child : job["children"])
        child.erase("handlerIdentity");
      if (job["cursor"] == "done")
        job["events"] = Json::array();
    }
    if (version < 3) {
      job.erase("automatic");
      job.erase("contentMode");
      job.erase("awaitKind");
      job.erase("timer");
      for (auto& child : job["children"]) {
        child.erase("awaitKind");
        child.erase("timer");
      }
      for (auto& completion : job["completions"])
        completion = completion.at("value").get<bool>();
    }
    if (version == 1)
      for (const char* field :
           { "node", "child", "children", "nextOperation" })
        job.erase(field);
  };
  if (!saved["active"].is_null())
    strip(saved["active"]);
  for (auto& job : saved["completed"])
    strip(job);
  return saved;
}

QuestJobState MigratedLegacy(QuestJobState state, uint64_t version = 1)
{
  state.retiredThrough = 0;
  if (state.active) {
    state.active->sequenced = false;
    if (version < 3)
      state.active->contentMode = "legacy";
  }
  for (auto& job : state.completed) {
    job.sequenced = false;
    if (version < 3)
      job.contentMode = "legacy";
    if (version < 4) {
      job.events.clear();
      job.eventHead = 0;
    }
  }
  return state;
}

QuestJobState TimerWaiting()
{
  auto state = Waiting();
  state.active->awaitKind = "timer";
  state.active->awaitKey = "Utility.Wait";
  state.active->timer =
    QuestJobState::Timer{ 1700000000000, 1700000000100, 100 };
  return state;
}

QuestJobState BodyDone()
{
  auto state = Begin();
  auto& job = *state.active;
  job.cursor = "done";
  job.accepted = true;
  job.segment = job.steps = job.instructions = 1;
  job.events.push_back({ "14:Skyrim.esm", "f:Skyrim.esm", "", 2, true });
  return state;
}

void HostStep(QuestJobState::Job& job, uint32_t instructions = 0)
{
  ++job.segment;
  ++job.steps;
  job.instructions += instructions;
}

QuestJobState Dispatching(bool captured = false)
{
  auto state = BodyDone();
  auto& job = *state.active;
  job.dispatch = QuestJobState::Dispatch{};
  job.dispatch->recipients = { job.events.front().target };
  HostStep(job, 1);
  if (captured) {
    job.dispatch->captured = true;
    job.dispatch->roots = { "ChildScript" };
    job.dispatch->templates = { "7:Skyrim.esm" };
    job.dispatch->reference = "3000";
    HostStep(job);
  }
  return state;
}

QuestJobState SelectedHandler(bool waiting = false, bool alias = false)
{
  auto state = Dispatching(true);
  auto& job = *state.active;
  auto& dispatch = *job.dispatch;
  if (alias) {
    dispatch.recipients.push_back(Json{
      { "kind", "alias" }, { "quest", job.quest }, { "id", 2147483647 } });
    dispatch.recipient = 1;
    dispatch.templates.clear();
    dispatch.reference.clear();
  }
  QuestJobState::Stage handler;
  handler.quest = job.quest;
  handler.contentHash = job.contentHash;
  handler.stage = job.stage;
  handler.handlerIdentity =
    QuestJobState::HandlerIdentity{ dispatch.recipients[dispatch.recipient],
                                    "ChildScript",
                                    "ParentScript",
                                    "busy",
                                    "OnItemAdded",
                                    job.nextHandler++ };
  job.handler = handler;
  HostStep(job, 1);
  if (waiting) {
    auto& work = *job.handler;
    work.cursor = "waiting";
    work.accepted = true;
    work.script = "ParentScript";
    work.function = "OnItemAdded";
    work.token = job.NextToken();
    ++job.nextOperation;
    work.awaitKind = "bool";
    work.awaitKey = "fixture";
    HostStep(job, 2);
  }
  return state;
}

Json DispatchHeap(const QuestJobState::Job& job)
{
  auto heap = Heap(*Waiting().active);
  heap["continuations"].clear();
  const auto add = [&](const QuestJobState::Stage& stage) {
    if (stage.cursor != "waiting")
      return;
    const auto saved =
      Heap(job, &stage, &stage == &job.Current() ? job.instructions : 1);
    heap["continuations"].update(saved["continuations"]);
  };
  add(job);
  if (job.handler)
    add(*job.handler);
  for (const auto& child : job.children)
    add(child);
  return heap;
}

QuestJobState::Job CompletedRecord(uint64_t id, bool sequenced)
{
  auto job = Done(Waiting()).completed.front();
  job.id = id;
  job.sequenced = sequenced;
  job.command = sequenced ? "quest-request:" + std::to_string(job.generation) +
      ":" + std::to_string(id)
                          : "legacy-" + std::to_string(id);
  job.completions.clear();
  job.completions[job.Scope() + ":1"] = true;
  return job;
}

QuestJobState History(size_t legacy, size_t recent)
{
  QuestJobState state;
  for (size_t index = 0; index < legacy + recent; ++index)
    state.completed.push_back(
      CompletedRecord(state.nextJob++, index >= legacy));
  return state;
}

QuestJobState::Job LargeCompletion(uint64_t id, bool sequenced)
{
  auto job = CompletedRecord(id, sequenced);
  const auto plugin = std::string(240, 'x') + ".esm";
  job.events.assign(
    1024, QuestJobState::Event{ "14:" + plugin, "f:" + plugin, "", 1, true });
  job.eventHead = static_cast<uint32_t>(job.events.size());
  return job;
}

QuestJobState ByteHeavyHistory()
{
  QuestJobState state;
  state.completed.push_back(LargeCompletion(state.nextJob++, false));
  state.completed.push_back(LargeCompletion(state.nextJob++, true));
  state.completed.push_back(CompletedRecord(state.nextJob++, false));
  state.completed.push_back(LargeCompletion(state.nextJob++, true));
  state.completed.push_back(LargeCompletion(state.nextJob++, true));
  state.completed.push_back(CompletedRecord(state.nextJob++, true));
  return state;
}

void AddPending(QuestJobState& state, bool sequenced)
{
  auto job = *Begin().active;
  job.id = state.nextJob++;
  job.generation = state.generation;
  job.sequenced = sequenced;
  job.command = sequenced ? "quest-request:" + std::to_string(job.generation) +
      ":" + std::to_string(job.id)
                          : "legacy-" + std::to_string(job.id);
  state.active = job;
}

struct JobStorage
  : Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>
{
  bool SupportsAtomicBatches() const noexcept override { return true; }
  bool SupportsUpsertResults() const noexcept override { return true; }
  void IterateSync(const IterateSyncCallback&) override {}
  void Iterate(const IterateCallback& cb,
               const std::optional<std::vector<FormDesc>>&) override
  {
    cb({});
  }
  void Upsert(std::vector<std::optional<MpChangeForm>>&&,
              const UpsertCallback& cb) override
  {
    cb();
  }
  void UpsertWithResult(std::vector<std::optional<MpChangeForm>>&&,
                        const UpsertResultCallback& cb) override
  {
    cb({});
  }
  uint32_t GetNumFinishedUpserts() const override { return 0; }
  uint32_t GetNumFinishedIterates() const override { return 0; }
  void Tick() override {}
  bool GetRecycledChangeFormsBuffer(
    std::vector<std::optional<MpChangeForm>>&) override
  {
    return false;
  }
  const std::string& GetName() const override
  {
    static const std::string name = "job-state-test";
    return name;
  }
};

struct Fixture
{
  PartOne server;
  MpActor* actor;
  Fixture()
  {
    server.AttachEspm(&GetEspmLoader());
    server.AttachSaveStorage(std::make_shared<JobStorage>());
    server.CreateActor(0xff003000, { 1, 2, 3 }, 0, 0x3c);
    actor = &server.worldState.GetFormAt<MpActor>(0xff003000);
    actor->SetPropertyValueDump(QuestJournal::kEnabled,
                                "[\"4e50d:Skyrim.esm\"]", false, false);
  }
  Json Saved() const
  {
    return Json::parse(
      actor->GetDynamicFields().GetValueDump(QuestJournal::kProperty));
  }
  void SeedJobs(const QuestJobState& jobs)
  {
    jobs.ValidateHeap(Json::object());
    const Json ledger = { { "schemaVersion", 2 },
                          { "revision", uint64_t(1) },
                          { "quests", Json::object() },
                          { "jobs", jobs.Save() } };
    actor->SetPropertyValueDump(QuestJournal::kProperty, ledger.dump(), false,
                                false);
  }
  void PublishJobs(const QuestJobState& jobs)
  {
    const auto id = jobs.active ? jobs.active->id : jobs.completed.back().id;
    const auto receipt = QuestJournal::AtomicJob(
      *actor, id, [&] { QuestJournal::SetJobState(*actor, jobs); });
    REQUIRE(receipt);
    for (size_t tick = 0;
         tick < 4 && receipt->GetState() == SaveReceipt::State::Pending;
         ++tick)
      server.worldState.Tick();
    REQUIRE(receipt->GetState() == SaveReceipt::State::Durable);
  }
  void Start()
  {
    QuestJournal::AtomicJob(
      *actor, 1, [&] { QuestJournal::SetJobState(*actor, Begin()); });
  }
};
}

TEST_CASE("Quest job codec round trips begin waiting and completed work",
          "[Quest][QuestJob]")
{
  REQUIRE(QuestJobState::Read(QuestJobState{}.Save()).Save() ==
          QuestJobState{}.Save());
  for (const auto& state : { Begin(), Waiting(), Done(Waiting()) })
    REQUIRE(QuestJobState::Read(state.Save()).Save() == state.Save());
  auto state = Waiting();
  REQUIRE(state.active->Scope() == "quest-job:1:1");
  REQUIRE(state.active->NextToken() == "quest-job:1:1:2");
  state.active->completions[state.active->token] = false;
  state.active->token = state.active->NextToken();
  ++state.active->nextOperation;
  state.active->segment = 2;
  REQUIRE(state.active->token == "quest-job:1:1:2");
  REQUIRE_NOTHROW(state.Save());
}

TEST_CASE("Quest jobs reject corrupt exact fields identities and cursor state",
          "[Quest][QuestJob]")
{
  const auto original = Waiting().Save();
  std::vector<std::function<void(Json&)>> corrupt = {
    [](Json& v) { v["unknown"] = 1; },
    [](Json& v) { v.erase("active"); },
    [](Json& v) { v["schemaVersion"] = 6; },
    [](Json& v) { v["generation"] = 0; },
    [](Json& v) { v["nextJob"] = 1; },
    [](Json& v) { v["nextJob"] = UINT64_MAX; },
    [](Json& v) { v["active"]["id"] = 1.0; },
    [](Json& v) { v["active"]["generation"] = 2; },
    [](Json& v) { v["active"]["actor"] = "03000"; },
    [](Json& v) { v["active"]["owner"] = "3000:Skyrim.esm"; },
    [](Json& v) { v["active"]["quest"] = "4e50d"; },
    [](Json& v) { v["active"]["quest"] = "1000000:Skyrim.esm"; },
    [](Json& v) { v["active"]["command"] = ""; },
    [](Json& v) { v["active"]["command"] = std::string(129, 'a'); },
    [](Json& v) { v["active"]["instance"] = std::string(301, 'a'); },
    [](Json& v) { v["active"]["contentHash"] = std::string(64, 'A'); },
    [](Json& v) { v["active"]["script"] = std::string("a\0b", 3); },
    [](Json& v) { v["active"]["function"] = ""; },
    [](Json& v) { v["active"]["awaitKey"] = ""; },
    [](Json& v) { v["active"]["accepted"] = false; },
    [](Json& v) { v["active"]["segment"] = 0; },
    [](Json& v) { v["active"]["instructions"] = 0; },
    [](Json& v) { v["active"]["instructions"] = 100001; },
    [](Json& v) { v["active"]["steps"] = 100001; },
    [](Json& v) { v["active"]["log"] = -1; },
    [](Json& v) { v["active"]["token"] = "quest-job:1:2:1"; },
    [](Json& v) { v["active"]["cursor"] = "begin"; },
    [](Json& v) { v["active"]["cursor"] = "done"; },
    [](Json& v) { v["active"]["completions"]["quest-job:1:1:2"] = true; },
    [](Json& v) { v["active"]["events"][0]["target"] = "3000"; },
    [](Json& v) { v["active"]["events"][0]["count"] = 0; },
    [](Json& v) {
      v["active"]["events"][0]["count"] = uint64_t(INT32_MAX) + 1;
    },
    [](Json& v) { v["active"]["events"][0]["added"] = 1; },
    [](Json& v) { v["active"]["events"][0]["extra"] = false; }
  };
  for (size_t index = 0; index < corrupt.size(); ++index) {
    INFO("Corruption " << index);
    auto value = original;
    corrupt[index](value);
    REQUIRE_THROWS(QuestJobState::Read(value));
  }
}

TEST_CASE("Typed quest job saves enforce history uniqueness and limits",
          "[Quest][QuestJob]")
{
  auto state = Waiting();
  SECTION("invalid typed identity")
  {
    state.active->id = 0;
  }
  SECTION("pending event overflow")
  {
    state.active->events.resize(4097);
  }
  SECTION("operation overflow")
  {
    for (size_t n = 1; n <= 257; ++n)
      state.active
        ->completions[state.active->Scope() + ":" + std::to_string(n)] = true;
  }
  SECTION("completed history overflow")
  {
    state.completed.resize(257);
  }
  SECTION("duplicate command")
  {
    auto old = Done(Begin()).completed.front();
    state.completed.push_back(old);
    state.active->id = 2;
    state.active->token = state.active->Scope() + ":1";
    state.nextJob = 3;
  }
  SECTION("completed work cannot retain a failure")
  {
    state = Done(Waiting());
    state.completed.front().failure = "failure";
  }
  REQUIRE_THROWS(state.Save());
}

TEST_CASE("Job heap cross links require the unique owning outer frame",
          "[Quest][QuestJob]")
{
  const auto state = Waiting();
  const auto job = *state.active;
  auto heap = Heap(job);
  REQUIRE_NOTHROW(state.ValidateHeap(heap));
  SECTION("missing continuation")
  {
    heap["continuations"].clear();
  }
  SECTION("wrong scope")
  {
    heap["continuations"][job.token]["scope"] = "other";
  }
  SECTION("wrong budget")
  {
    heap["continuations"][job.token]["executedInstructions"] = 4;
  }
  SECTION("wrong receiver")
  {
    heap["continuations"][job.token]["receiver"]["value"] = "14:Skyrim.esm";
  }
  SECTION("wrong outer method")
  {
    heap["continuations"][job.token]["function"] = "other";
  }
  SECTION("extra stack")
  {
    heap["continuations"]["orphan"] = heap["continuations"][job.token];
  }
  SECTION("corrupt codec")
  {
    heap["continuations"][job.token]["locals"] = 1;
  }
  SECTION("caller must match even when leaf matches")
  {
    auto caller = heap["continuations"][job.token];
    for (const char* field :
         { "schemaVersion", "scope", "executedInstructions", "callers" })
      caller.erase(field);
    caller["function"] = "unrelated";
    heap["continuations"][job.token]["callers"].push_back(caller);
  }
  REQUIRE_THROWS(state.ValidateHeap(heap));
}

TEST_CASE("Only untouched runner state preserves standalone VM continuations",
          "[Quest][QuestJob]")
{
  const auto waiting = Waiting();
  const auto heap = Heap(*waiting.active);
  REQUIRE_NOTHROW(QuestJobState{}.ValidateHeap(heap));
  REQUIRE_THROWS(Begin().ValidateHeap(heap));
  REQUIRE_THROWS(Done(waiting).ValidateHeap(heap));
  auto used = Done(Begin());
  REQUIRE_THROWS(used.ValidateHeap(heap));
  REQUIRE_NOTHROW(used.ValidateHeap(Json::object()));
}

TEST_CASE("Job heap ownership follows the outer frame of nested calls",
          "[Quest][QuestJob]")
{
  const auto state = Waiting();
  const auto& job = *state.active;
  auto heap = Heap(job);
  auto& leaf = heap["continuations"][job.token];
  auto caller = leaf;
  for (const char* field :
       { "schemaVersion", "scope", "executedInstructions", "callers" })
    caller.erase(field);
  leaf["rootScript"] = "Helper";
  leaf["script"] = "Helper";
  leaf["function"] = "Nested";
  leaf["receiver"]["value"] = "14:Skyrim.esm";
  leaf["callers"].push_back(caller);
  REQUIRE_NOTHROW(state.ValidateHeap(heap));
}

TEST_CASE("Job fragment ownership follows Papyrus case insensitive names",
          "[Quest][QuestJob]")
{
  const auto state = Waiting();
  const auto& job = *state.active;
  auto heap = Heap(job);
  auto& frame = heap["continuations"][job.token];
  frame["rootScript"] = "qf_test";
  frame["script"] = "qF_tEsT";
  frame["function"] = "fragment_0";
  REQUIRE_NOTHROW(state.ValidateHeap(heap));
  // Fragment entry currently resolves on the attachment itself, not parents.
  // A different implementation must not bypass that entry binding.
  frame["script"] = "ParentScript";
  REQUIRE_THROWS(state.ValidateHeap(heap));
}

TEST_CASE(
  "Private job migration keeps schema one public snapshots and locks writers",
  "[Quest][QuestJob]")
{
  Fixture f;
  auto initialHeap = Heap(*Waiting().active);
  initialHeap["continuations"].clear();
  QuestJournal::Atomic(
    *f.actor, [&] { QuestJournal::SetScriptState(*f.actor, initialHeap); });
  REQUIRE(f.Saved()["schemaVersion"] == 1);
  REQUIRE_FALSE(f.Saved().contains("jobs"));
  const auto beforeRevision =
    QuestJournal::Snapshot(*f.actor)["revision"].get<uint64_t>();
  f.Start();
  REQUIRE(f.Saved()["schemaVersion"] == 2);
  REQUIRE(f.Saved()["jobs"] == Begin().Save());
  const auto snapshot = QuestJournal::Snapshot(*f.actor);
  REQUIRE(snapshot["schemaVersion"] == 1);
  REQUIRE(snapshot["revision"].get<uint64_t>() > beforeRevision);
  REQUIRE_FALSE(snapshot.contains("jobs"));
  REQUIRE_FALSE(snapshot.contains("vm"));
  REQUIRE_FALSE(snapshot.contains("referenceInit"));
  REQUIRE(snapshot.size() == 7);
  const auto before = f.Saved();
  bool executed = false;
  REQUIRE_THROWS(QuestJournal::Atomic(*f.actor, [&] { executed = true; }));
  REQUIRE_THROWS(
    QuestJournal::AtomicDurable(*f.actor, [&] { executed = true; }));
  REQUIRE_THROWS(
    QuestJournal::AtomicJob(*f.actor, 2, [&] { executed = true; }));
  REQUIRE_FALSE(executed);
  REQUIRE_THROWS(QuestJournal::RequireJobAccess(*f.actor));
  REQUIRE_THROWS(QuestJournal::Apply(
    *f.actor,
    { { "quest", kJobQuest }, { "op", "setStage" }, { "index", 10 } }));
  REQUIRE_THROWS(QuestJournal::RecordRepeatedStage(*f.actor, kJobQuest, 10));
  REQUIRE_THROWS(QuestJournal::SetJobState(*f.actor, Waiting()));
  REQUIRE_THROWS(QuestJournal::SetScriptState(*f.actor, Json::object()));
  REQUIRE(f.Saved() == before);
}

TEST_CASE("Owning jobs permit intermediate cursor heap changes but commit "
          "validates both",
          "[Quest][QuestJob]")
{
  Fixture f;
  f.Start();
  auto waiting = Waiting();
  waiting.active->events.clear();
  const auto before = f.Saved();
  REQUIRE_THROWS(QuestJournal::AtomicJob(
    *f.actor, 1, [&] { QuestJournal::SetJobState(*f.actor, waiting); }));
  REQUIRE(f.Saved() == before);
  REQUIRE_NOTHROW(QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetScriptState(*f.actor, Heap(*waiting.active));
    QuestJournal::Apply(
      *f.actor,
      { { "quest", kJobQuest }, { "op", "setStage" }, { "index", 10 } });
    QuestJournal::SetJobState(*f.actor, waiting);
  }));
  REQUIRE(QuestJournal::JobState(*f.actor).Save() == waiting.Save());
  const auto done = Done(waiting);
  REQUIRE_NOTHROW(QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetJobState(*f.actor, done);
    QuestJournal::SetScriptState(*f.actor, Json::object());
  }));
  REQUIRE(QuestJournal::JobState(*f.actor).Save() == done.Save());
  REQUIRE_NOTHROW(QuestJournal::Atomic(*f.actor, [] {}));
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, 1, [] {}));
}

TEST_CASE("New job transactions require a matching saved job and hide other "
          "private fields",
          "[Quest][QuestJob]")
{
  Fixture f;
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, 1, [] {}));
  auto privateLedger = Json{ { "schemaVersion", 1 },
                             { "revision", uint64_t(0) },
                             { "quests", Json::object() },
                             { "privateFutureField", "secret" } };
  f.actor->SetPropertyValueDump(QuestJournal::kProperty, privateLedger.dump(),
                                false, false);
  REQUIRE_FALSE(
    QuestJournal::Snapshot(*f.actor).contains("privateFutureField"));
  REQUIRE_THROWS(QuestJournal::Atomic(
    *f.actor, [&] { QuestJournal::SetJobState(*f.actor, Begin()); }));
}

TEST_CASE("Full legacy history rejects new named commands without evicting "
          "deduplication",
          "[Quest][QuestJob]")
{
  Fixture f;
  QuestJobState full;
  auto job = Done(Begin()).completed.front();
  for (uint64_t id = 1; id <= QuestJobState::MaxCompleted; ++id) {
    job.id = id;
    job.command = "command-" + std::to_string(id);
    full.completed.push_back(job);
  }
  full.nextJob = full.completed.size() + 1;
  const Json ledger = { { "schemaVersion", 2 },
                        { "revision", uint64_t(1) },
                        { "quests", Json::object() },
                        { "jobs", full.Save() } };
  f.actor->SetPropertyValueDump(QuestJournal::kProperty, ledger.dump(), false,
                                false);
  auto admission = full;
  AddPending(admission, false);
  bool ran = false;
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, full.nextJob, [&] {
    QuestJournal::SetJobState(*f.actor, admission);
    ran = true;
  }));
  REQUIRE_FALSE(ran);
  REQUIRE(f.Saved() == ledger);
  auto missing = full.Save();
  missing["completed"].erase(missing["completed"].begin());
  REQUIRE_THROWS(QuestJobState::Read(missing));
}

TEST_CASE("Personal job ownership remains locked while visiting another host",
          "[Quest][QuestJob]")
{
  Fixture f;
  f.Start();
  constexpr uint32_t hostId = 0xff003001;
  f.server.CreateActor(hostId, { 1, 2, 3 }, 0, 0x3c);
  auto& host = f.server.worldState.GetFormAt<MpActor>(hostId);
  const auto instance = "campaign:" + std::to_string(hostId) + ":visit";
  host.SetWorldInstance(instance);
  f.actor->SetWorldInstance(instance);
  REQUIRE_NOTHROW(QuestJournal::RequireJobAccess(*f.actor));
  REQUIRE_NOTHROW(QuestJournal::RequirePersonalJobAccess(host));
  REQUIRE_THROWS(QuestJournal::RequirePersonalJobAccess(*f.actor));
  REQUIRE_FALSE(QuestJournal::JobState(*f.actor).active);
  REQUIRE(QuestJournal::PersonalJobState(*f.actor).Save() == Begin().Save());
  REQUIRE_FALSE(QuestJournal::PersonalJobState(host).active);
}

TEST_CASE(
  "Legacy quest jobs migrate operation identities without losing history",
  "[Quest][QuestJob]")
{
  for (const auto& state : { Begin(), Waiting(), Done(Waiting()) }) {
    const auto legacy = Legacy(state);
    const auto migrated = QuestJobState::Read(legacy);
    REQUIRE(migrated.Save() == MigratedLegacy(state).Save());
    REQUIRE(migrated.Save()["schemaVersion"] == 5);
  }
  auto external = Waiting();
  external.active->awaitKey = "quest-stage";
  REQUIRE(QuestJobState::Read(Legacy(external)).Save() ==
          MigratedLegacy(external).Save());
  auto legacy = Legacy(Waiting());
  SECTION("extra new field is not accepted in legacy schema")
  {
    legacy["active"]["nextOperation"] = 2;
  }
  SECTION("legacy pending token must follow completions")
  {
    legacy["active"]["token"] = "quest-job:1:1:2";
  }
  SECTION("legacy completion sequence cannot have a gap")
  {
    legacy["active"]["completions"]["quest-job:1:1:3"] = true;
    legacy["active"]["token"] = "quest-job:1:1:2";
  }
  REQUIRE_THROWS(QuestJobState::Read(legacy));
}

TEST_CASE("Nested quest stages retain separate scopes and one global "
          "operation sequence",
          "[Quest][QuestJob]")
{
  for (auto state : { Nested(), Nested(true) }) {
    const auto& job = *state.active;
    REQUIRE(&job.Current() == &job.children.back());
    REQUIRE(job.Scope(job) == "quest-job:1:1");
    REQUIRE(job.Scope(job.Current()) == "quest-job:1:1:node:1");
    REQUIRE(QuestJobState::Read(state.Save()).Save() == state.Save());
    REQUIRE_NOTHROW(state.ValidateHeap(NestedHeap(job)));
  }
  auto state = Nested(true);
  FinishChild(*state.active);
  REQUIRE_NOTHROW(state.Save());
  REQUIRE_NOTHROW(state.ValidateHeap(NestedHeap(*state.active)));
  REQUIRE_FALSE(state.active->events.empty());
  REQUIRE(state.active->completions.at("quest-job:1:1:2") ==
          QuestJobState::Completion(true));
  REQUIRE(state.active->NextToken() == "quest-job:1:1:3");
}

TEST_CASE("Nested stage codecs reject broken links reused tokens and "
          "incomplete histories",
          "[Quest][QuestJob]")
{
  const auto original = Nested(true).Save();
  const std::vector<std::function<void(Json&)>> corrupt = {
    [](Json& v) { v["active"]["node"] = 1; },
    [](Json& v) { v["active"]["child"] = 2; },
    [](Json& v) { v["active"]["awaitKey"] = "external"; },
    [](Json& v) { v["active"]["children"][0]["node"] = 0; },
    [](Json& v) { v["active"]["children"][0]["node"] = 2; },
    [](Json& v) { v["active"]["children"][0]["child"] = 2; },
    [](Json& v) { v["active"]["children"][0]["extra"] = 1; },
    [](Json& v) { v["active"]["children"][0]["token"] = "quest-job:1:1:1"; },
    [](Json& v) {
      v["active"]["children"][0]["token"] = "quest-job:1:1:node:1:2";
    },
    [](Json& v) { v["active"]["children"][0]["token"] = "quest-job:1:1:02"; },
    [](Json& v) { v["active"]["children"].clear(); },
    [](Json& v) { v["active"]["nextOperation"] = 4; },
    [](Json& v) { v["active"]["nextOperation"] = 258; },
    [](Json& v) { v["active"]["completions"]["quest-job:1:1:2"] = true; },
    [](Json& v) { v["active"]["children"][0]["cursor"] = "begin"; },
    [](Json& v) { v["active"]["children"][0]["cursor"] = "done"; },
    [](Json& v) { v["active"]["children"][0]["quest"] = "3000"; }
  };
  for (size_t index = 0; index < corrupt.size(); ++index) {
    INFO("Nested corruption " << index);
    auto value = original;
    corrupt[index](value);
    REQUIRE_THROWS(QuestJobState::Read(value));
  }
}

TEST_CASE("Nested heap validation requires every waiting node and exact "
          "current budget",
          "[Quest][QuestJob]")
{
  const auto state = Nested(true);
  const auto& job = *state.active;
  const auto childToken = job.children.back().token;
  auto heap = NestedHeap(job);
  REQUIRE(heap["continuations"][job.token]["executedInstructions"] == 3);
  REQUIRE(heap["continuations"][childToken]["executedInstructions"] == 7);
  REQUIRE_NOTHROW(state.ValidateHeap(heap));
  SECTION("missing child")
  {
    heap["continuations"].erase(childToken);
  }
  SECTION("missing parent")
  {
    heap["continuations"].erase(job.token);
  }
  SECTION("child using parent scope")
  {
    heap["continuations"][childToken]["scope"] = job.Scope();
  }
  SECTION("child using parent receiver")
  {
    heap["continuations"][childToken]["receiver"]["value"] = job.quest;
  }
  SECTION("parent budget exceeds aggregate")
  {
    heap["continuations"][job.token]["executedInstructions"] = 8;
  }
  SECTION("current child budget differs from aggregate")
  {
    heap["continuations"][childToken]["executedInstructions"] = 6;
  }
  REQUIRE_THROWS(state.ValidateHeap(heap));
}

TEST_CASE("Quest stage depth and operation bounds include pending ancestors",
          "[Quest][QuestJob]")
{
  auto state = Waiting();
  auto& job = *state.active;
  for (uint64_t node = 1; node < QuestJobState::MaxDepth; ++node) {
    auto& parent = job.Current();
    parent.child = node;
    parent.awaitKey = "quest-stage";
    parent.awaitKind = "stage";
    QuestJobState::Stage child = parent;
    child.node = node;
    child.child = 0;
    child.awaitKey = "external";
    child.awaitKind = "bool";
    child.token = job.NextToken();
    ++job.nextOperation;
    job.children.push_back(child);
  }
  REQUIRE(job.children.size() == 127);
  REQUIRE_NOTHROW(state.Save());
  auto& parent = job.Current();
  parent.child = 128;
  parent.awaitKey = "quest-stage";
  parent.awaitKind = "stage";
  auto child = parent;
  child.node = 128;
  child.child = 0;
  child.awaitKey = "external";
  child.awaitKind = "bool";
  child.token = job.NextToken();
  ++job.nextOperation;
  job.children.push_back(child);
  REQUIRE_THROWS(state.Save());
}

TEST_CASE(
  "Journal advances one durable child boundary and permits a new sibling",
  "[Quest][QuestJob]")
{
  Fixture f;
  f.Start();
  const auto publish = [&](const QuestJobState& state) {
    QuestJournal::AtomicJob(*f.actor, 1, [&] {
      QuestJournal::SetScriptState(*f.actor, NestedHeap(*state.active));
      QuestJournal::SetJobState(*f.actor, state);
    });
  };
  auto state = Nested();
  REQUIRE_NOTHROW(publish(state));
  state = Nested(true);
  REQUIRE_NOTHROW(publish(state));
  FinishChild(*state.active);
  REQUIRE_NOTHROW(publish(state));
  auto& job = *state.active;
  job.completions[job.token] = job.children.back().accepted;
  job.children.pop_back();
  job.child = job.nextOperation;
  job.token = job.NextToken();
  ++job.nextOperation;
  ++job.segment;
  ++job.instructions;
  QuestJobState::Stage sibling;
  sibling.node = job.child;
  sibling.quest = "2aa58:Skyrim.esm";
  sibling.contentHash = std::string(64, 'c');
  sibling.stage = 30;
  job.children.push_back(sibling);
  REQUIRE_NOTHROW(publish(state));
  REQUIRE(QuestJournal::JobState(*f.actor).Save() == state.Save());
  REQUIRE(job.completions.size() == 2);
  REQUIRE(job.children.back().node == 3);
}

TEST_CASE("Journal refuses rewriting live children ancestors or unrequested "
          "completions",
          "[Quest][QuestJob]")
{
  Fixture f;
  f.Start();
  auto state = Nested(true);
  // New children must first commit their begin cursor.
  const auto begin = Nested();
  QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetScriptState(*f.actor, NestedHeap(*begin.active));
    QuestJournal::SetJobState(*f.actor, begin);
  });
  QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetScriptState(*f.actor, NestedHeap(*state.active));
    QuestJournal::SetJobState(*f.actor, state);
  });
  const auto before = f.Saved();
  auto& job = *state.active;
  SECTION("live child identity")
  {
    job.children.back().quest = "2aa58:Skyrim.esm";
  }
  SECTION("ancestor cursor metadata")
  {
    job.log = 1;
  }
  SECTION("drop unfinished child")
  {
    job.completions[job.token] = true;
    job.completions[job.children.back().token] = true;
    job.children.clear();
    job.child = 0;
    job.token = job.NextToken();
    ++job.nextOperation;
    job.awaitKey = "external";
    job.awaitKind = "bool";
  }
  SECTION("invent completed operation while keeping current wait")
  {
    job.completions[job.NextToken()] = true;
    ++job.nextOperation;
  }
  SECTION("allocate multiple operations in one segment")
  {
    job.completions[job.children.back().token] = true;
    job.completions[job.NextToken()] = true;
    ++job.nextOperation;
    job.children.back().token = job.NextToken();
    ++job.nextOperation;
  }
  REQUIRE_NOTHROW(state.Save());
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetScriptState(*f.actor, NestedHeap(job));
    QuestJournal::SetJobState(*f.actor, state);
  }));
  REQUIRE(f.Saved() == before);
}

TEST_CASE("A job transaction cannot cross two durable child boundaries",
          "[Quest][QuestJob]")
{
  Fixture f;
  const auto begin = Begin();
  const auto nested = Nested();
  // Submit may expose its pristine root before publishing the first segment.
  REQUIRE_NOTHROW(QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetJobState(*f.actor, begin);
    QuestJournal::SetJobState(*f.actor, begin);
    QuestJournal::SetScriptState(*f.actor, NestedHeap(*nested.active));
    QuestJournal::SetJobState(*f.actor, nested);
    QuestJournal::SetJobState(*f.actor, nested);
  }));
  const auto before = f.Saved();
  auto doneChild = nested;
  FinishChild(*doneChild.active);
  auto sibling = doneChild;
  auto& job = *sibling.active;
  job.completions[job.token] = true;
  job.children.pop_back();
  job.child = job.nextOperation;
  job.token = job.NextToken();
  ++job.nextOperation;
  auto child = nested.active->children.front();
  child.node = job.child;
  job.children.push_back(child);
  REQUIRE_NOTHROW(doneChild.Save());
  REQUIRE_NOTHROW(sibling.Save());
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetJobState(*f.actor, doneChild);
    QuestJournal::SetJobState(*f.actor, sibling);
    QuestJournal::SetScriptState(*f.actor, NestedHeap(job));
  }));
  REQUIRE(f.Saved() == before);
}

TEST_CASE(
  "Consuming a wait cannot rewind stage log effects or change its fragment",
  "[Quest][QuestJob]")
{
  Fixture f;
  f.Start();
  auto state = Waiting();
  state.active->log = 10;
  QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetScriptState(*f.actor, Heap(*state.active));
    QuestJournal::SetJobState(*f.actor, state);
  });
  const auto before = f.Saved();
  auto& job = *state.active;
  job.completions[job.token] = true;
  job.token = job.NextToken();
  ++job.nextOperation;
  SECTION("earlier log")
  {
    job.log = 9;
  }
  SECTION("same log different script")
  {
    job.script = "Different";
  }
  SECTION("same log different function")
  {
    job.function = "Fragment_1";
  }
  REQUIRE_NOTHROW(state.Save());
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetScriptState(*f.actor, Heap(job));
    QuestJournal::SetJobState(*f.actor, state);
  }));
  REQUIRE(f.Saved() == before);
}

TEST_CASE(
  "Completed and pending operations together respect the bounded capacity",
  "[Quest][QuestJob]")
{
  auto state = Waiting();
  auto& job = *state.active;
  for (uint64_t operation = 1; operation < QuestJobState::MaxOperations;
       ++operation)
    job.completions[job.Scope() + ":" + std::to_string(operation)] = true;
  job.token = job.Scope() + ":256";
  job.nextOperation = 257;
  job.segment = 513;
  REQUIRE_NOTHROW(state.Save());
  job.completions[job.token] = true;
  job.token = job.NextToken();
  ++job.nextOperation;
  REQUIRE_THROWS(state.Save());
}

TEST_CASE("Schema two jobs migrate typed completions and pending kinds",
          "[Quest][QuestJob]")
{
  for (const auto& state :
       { Begin(), Waiting(), Done(Waiting()), Nested(), Nested(true) }) {
    const auto migrated = QuestJobState::Read(Legacy(state, 2));
    REQUIRE(migrated.Save() == MigratedLegacy(state).Save());
    const auto& job =
      migrated.active ? *migrated.active : migrated.completed.back();
    REQUIRE_FALSE(job.automatic);
    REQUIRE_FALSE(job.timer);
    for (const auto& [token, completion] : job.completions)
      REQUIRE(completion.kind == QuestJobState::Completion::Kind::Boolean);
  }
  auto external = Waiting();
  external.active->awaitKey = "quest-stage";
  REQUIRE(QuestJobState::Read(Legacy(external, 2)).active->awaitKind ==
          "bool");
  auto corrupt = Legacy(Nested(), 2);
  corrupt["active"]["children"][0]["timer"] = nullptr;
  REQUIRE_THROWS(QuestJobState::Read(corrupt));
}

TEST_CASE("Timer jobs round trip UTC deadlines and typed None completions",
          "[Quest][QuestJob]")
{
  auto state = TimerWaiting();
  state.active->automatic = true;
  for (const auto& value : { state, Done(state) })
    REQUIRE(QuestJobState::Read(value.Save()).Save() == value.Save());
  const auto saved = state.Save();
  REQUIRE(saved["schemaVersion"] == 5);
  REQUIRE(saved["active"]["timer"]["policy"] == "utc-deadline-v1");
  const auto done = Done(state);
  const auto& job = done.completed.front();
  REQUIRE(job.completions.at("quest-job:1:1:1") ==
          QuestJobState::Completion::None());
  REQUIRE((done.Save()["completed"][0]["completions"]["quest-job:1:1:1"] ==
           Json{ { "kind", "none" } }));
  REQUIRE_NOTHROW(state.ValidateHeap(Heap(*state.active)));
  REQUIRE_NOTHROW(done.ValidateHeap(Json::object()));
  constexpr uint64_t max = 9007199254740991ULL;
  state.active->timer = QuestJobState::Timer{ max - 86400000, max, 86400000 };
  REQUIRE_NOTHROW(state.Save());
  state.active->timer = QuestJobState::Timer{ max, max, 0 };
  REQUIRE_NOTHROW(state.Save());
}

TEST_CASE(
  "Timer codecs reject inconsistent deadlines wait kinds and exact fields",
  "[Quest][QuestJob]")
{
  const auto original = TimerWaiting().Save();
  const std::vector<std::function<void(Json&)>> corrupt = {
    [](Json& v) { v["active"]["automatic"] = 1; },
    [](Json& v) { v["active"].erase("automatic"); },
    [](Json& v) { v["active"]["awaitKind"] = ""; },
    [](Json& v) { v["active"]["awaitKind"] = "other"; },
    [](Json& v) { v["active"]["awaitKind"] = "bool"; },
    [](Json& v) { v["active"]["awaitKind"] = "stage"; },
    [](Json& v) { v["active"]["child"] = 1; },
    [](Json& v) { v["active"]["timer"] = nullptr; },
    [](Json& v) { v["active"]["timer"] = false; },
    [](Json& v) { v["active"]["timer"]["extra"] = 0; },
    [](Json& v) { v["active"]["timer"].erase("policy"); },
    [](Json& v) { v["active"]["timer"]["policy"] = "game-time"; },
    [](Json& v) { v["active"]["timer"]["issuedUnixMs"] = -1; },
    [](Json& v) { v["active"]["timer"]["issuedUnixMs"] = 1.0; },
    [](Json& v) { v["active"]["timer"]["dueUnixMs"] = 1700000000101ULL; },
    [](Json& v) { v["active"]["timer"]["dueUnixMs"] = 9007199254740992ULL; },
    [](Json& v) { v["active"]["timer"]["durationMs"] = 86400001; },
    [](Json& v) {
      v["active"]["timer"]["issuedUnixMs"] = 9007199254740991ULL;
      v["active"]["timer"]["dueUnixMs"] = 9007199254740991ULL;
    }
  };
  for (size_t index = 0; index < corrupt.size(); ++index) {
    INFO("Timer corruption " << index);
    auto value = original;
    corrupt[index](value);
    REQUIRE_THROWS(QuestJobState::Read(value));
  }
  auto begin = Begin();
  begin.active->timer = TimerWaiting().active->timer;
  REQUIRE_THROWS(begin.Save());
  auto done = Done(TimerWaiting());
  done.completed.front().awaitKind = "timer";
  REQUIRE_THROWS(done.Save());
}

TEST_CASE("Completion codecs reject malformed kinds and hidden None values",
          "[Quest][QuestJob]")
{
  auto state = Done(Waiting());
  const auto original = state.Save();
  const std::vector<Json> corrupt = {
    true,
    Json{ { "kind", "bool" } },
    Json{ { "kind", "bool" }, { "value", 1 } },
    Json{ { "kind", "none" }, { "value", false } },
    Json{ { "kind", "number" }, { "value", 1 } },
    Json{ { "kind", "bool" }, { "value", true }, { "extra", 0 } }
  };
  for (const auto& completion : corrupt) {
    auto value = original;
    value["completed"][0]["completions"]["quest-job:1:1:1"] = completion;
    REQUIRE_THROWS(QuestJobState::Read(value));
  }
  auto& completion = state.completed.front().completions.at("quest-job:1:1:1");
  completion = QuestJobState::Completion::None();
  completion.value = true;
  REQUIRE_THROWS(state.Save());
  completion.value = false;
  completion.kind = static_cast<QuestJobState::Completion::Kind>(255);
  REQUIRE_THROWS(state.Save());
}

TEST_CASE(
  "The terminal continuation result type must match the durable wait kind",
  "[Quest][QuestJob]")
{
  for (const auto& state : { Waiting(), TimerWaiting(), Nested() }) {
    const auto& job = *state.active;
    auto heap = Heap(job);
    REQUIRE_NOTHROW(state.ValidateHeap(heap));
    auto& leaf = heap["continuations"][job.token];
    auto caller = leaf;
    for (const char* field :
         { "schemaVersion", "scope", "executedInstructions", "callers" })
      caller.erase(field);
    caller["resultType"] = "None";
    leaf["callers"].push_back(caller);
    REQUIRE_NOTHROW(state.ValidateHeap(heap));
    leaf["resultType"] = job.awaitKind == "timer" ? "Bool" : "None";
    REQUIRE_THROWS(state.ValidateHeap(heap));
  }
}

TEST_CASE("Journal consumes timer waits only with typed None results",
          "[Quest][QuestJob]")
{
  Fixture f;
  f.Start();
  auto waiting = TimerWaiting();
  waiting.active->events.clear();
  QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetScriptState(*f.actor, Heap(*waiting.active));
    QuestJournal::SetJobState(*f.actor, waiting);
  });
  const auto before = f.Saved();
  auto done = Done(waiting);
  done.completed.front().completions[waiting.active->token] = false;
  REQUIRE_NOTHROW(done.Save());
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetScriptState(*f.actor, Json::object());
    QuestJournal::SetJobState(*f.actor, done);
  }));
  REQUIRE(f.Saved() == before);
  done.completed.front().completions[waiting.active->token] =
    QuestJobState::Completion::None();
  REQUIRE_NOTHROW(QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetScriptState(*f.actor, Json::object());
    QuestJournal::SetJobState(*f.actor, done);
  }));
  REQUIRE(QuestJournal::JobState(*f.actor).Save() == done.Save());
}

TEST_CASE(
  "Journal cannot change a pending timer or automatic scheduling policy",
  "[Quest][QuestJob]")
{
  Fixture f;
  f.Start();
  auto state = TimerWaiting();
  QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetScriptState(*f.actor, Heap(*state.active));
    QuestJournal::SetJobState(*f.actor, state);
  });
  const auto before = f.Saved();
  SECTION("deadline")
  {
    ++state.active->timer->issuedUnixMs;
    ++state.active->timer->dueUnixMs;
  }
  SECTION("duration")
  {
    ++state.active->timer->durationMs;
    ++state.active->timer->dueUnixMs;
  }
  SECTION("wait kind")
  {
    state.active->awaitKind = "bool";
    state.active->timer.reset();
  }
  SECTION("automatic policy")
  {
    state.active->automatic = true;
  }
  SECTION("content provenance")
  {
    state.active->contentMode = "fixture";
  }
  REQUIRE_NOTHROW(state.Save());
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetScriptState(*f.actor, Heap(*state.active));
    QuestJournal::SetJobState(*f.actor, state);
  }));
  REQUIRE(f.Saved() == before);
}

TEST_CASE("Journal rejects None for Boolean waits and child-stage results",
          "[Quest][QuestJob]")
{
  Fixture f;
  f.Start();
  QuestJobState waiting;
  SECTION("external Boolean")
  {
    waiting = Waiting();
    waiting.active->events.clear();
    QuestJournal::AtomicJob(*f.actor, 1, [&] {
      QuestJournal::SetScriptState(*f.actor, Heap(*waiting.active));
      QuestJournal::SetJobState(*f.actor, waiting);
    });
  }
  SECTION("child stage")
  {
    waiting = Nested();
    waiting.active->events.clear();
    QuestJournal::AtomicJob(*f.actor, 1, [&] {
      QuestJournal::SetScriptState(*f.actor, NestedHeap(*waiting.active));
      QuestJournal::SetJobState(*f.actor, waiting);
    });
    FinishChild(*waiting.active);
    QuestJournal::AtomicJob(*f.actor, 1, [&] {
      QuestJournal::SetScriptState(*f.actor, NestedHeap(*waiting.active));
      QuestJournal::SetJobState(*f.actor, waiting);
    });
  }
  const auto before = f.Saved();
  auto candidate = waiting;
  candidate.active->children.clear();
  candidate.active->child = 0;
  candidate = Done(candidate);
  candidate.completed.front().completions[waiting.active->token] =
    QuestJobState::Completion::None();
  REQUIRE_NOTHROW(candidate.Save());
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetScriptState(*f.actor, Json::object());
    QuestJournal::SetJobState(*f.actor, candidate);
  }));
  REQUIRE(f.Saved() == before);
}

TEST_CASE(
  "Content provenance survives saves and legacy jobs cannot become automatic",
  "[Quest][QuestJob]")
{
  auto state = TimerWaiting();
  for (const auto* mode : { "owned", "fixture", "legacy" }) {
    state.active->contentMode = mode;
    REQUIRE(QuestJobState::Read(state.Save()).Save() == state.Save());
  }
  state.active->automatic = true;
  REQUIRE_THROWS(state.Save());
  state.active->contentMode = "fixture";
  REQUIRE_NOTHROW(state.Save());
  auto missing = state.Save();
  missing["active"].erase("contentMode");
  REQUIRE_THROWS(QuestJobState::Read(missing));
  for (const auto* invalid : { "", "mod", "OWNED" }) {
    state.active->contentMode = invalid;
    REQUIRE_THROWS(state.Save());
  }
}

TEST_CASE("Saved event dispatch keeps the completed quest body active",
          "[Quest][QuestJob]")
{
  for (const auto& state :
       { BodyDone(), Dispatching(), Dispatching(true), SelectedHandler(),
         SelectedHandler(true), SelectedHandler(true, true) }) {
    const auto& job = *state.active;
    REQUIRE(job.cursor == "done");
    REQUIRE_FALSE(job.IsComplete());
    REQUIRE(QuestJobState::Read(state.Save()).Save() == state.Save());
    REQUIRE_NOTHROW(state.ValidateHeap(DispatchHeap(job)));
  }
  const auto state = SelectedHandler();
  REQUIRE(&state.active->Current() == &*state.active->handler);
  REQUIRE(state.active->Scope(*state.active->handler) ==
          "quest-job:1:1:handler:1");
  REQUIRE(state.active->NextToken() == "quest-job:1:1:1");
}

TEST_CASE(
  "Schema three jobs migrate without changing pending native identities",
  "[Quest][QuestJob]")
{
  for (const auto& state :
       { Begin(), Waiting(), TimerWaiting(), Nested(true), Done(Waiting()) }) {
    const auto migrated = QuestJobState::Read(Legacy(state, 3));
    REQUIRE(migrated.Save() == MigratedLegacy(state, 3).Save());
    const auto& job =
      migrated.active ? *migrated.active : migrated.completed.back();
    REQUIRE_FALSE(job.handler);
    REQUIRE_FALSE(job.dispatch);
    REQUIRE(job.eventHead == 0);
    REQUIRE(job.nextHandler == 1);
  }
  auto legacy = Legacy(Waiting(), 3);
  legacy["active"]["handlerIdentity"] = nullptr;
  REQUIRE_THROWS(QuestJobState::Read(legacy));
}

TEST_CASE(
  "Dispatch codecs reject forged membership bindings and handler identities",
  "[Quest][QuestJob]")
{
  const auto original = SelectedHandler(true).Save();
  const std::vector<std::function<void(Json&)>> corrupt = {
    [](Json& v) { v["active"]["dispatch"]["recipients"] = Json::array(); },
    [](Json& v) {
      v["active"]["dispatch"]["recipients"][0] = "15:Skyrim.esm";
    },
    [](Json& v) {
      v["active"]["dispatch"]["recipients"].push_back("15:Skyrim.esm");
    },
    [](Json& v) {
      v["active"]["dispatch"]["recipients"].push_back("14:Skyrim.esm");
    },
    [](Json& v) { v["active"]["dispatch"]["recipient"] = 1; },
    [](Json& v) { v["active"]["dispatch"]["attachment"] = 1; },
    [](Json& v) { v["active"]["dispatch"]["captured"] = false; },
    [](Json& v) { v["active"]["dispatch"]["reference"] = "14:Skyrim.esm"; },
    [](Json& v) { v["active"]["dispatch"]["reference"] = ""; },
    [](Json& v) { v["active"]["dispatch"]["templates"][0] = "3000"; },
    [](Json& v) { v["active"]["dispatch"]["roots"].push_back("childscript"); },
    [](Json& v) { v["active"]["dispatch"]["unknown"] = true; },
    [](Json& v) { v["active"]["eventHead"] = 1; },
    [](Json& v) { v["active"]["nextHandler"] = 1; },
    [](Json& v) { v["active"]["nextHandler"] = 4098; },
    [](Json& v) { v["active"]["handler"]["node"] = 1; },
    [](Json& v) { v["active"]["handler"]["quest"] = "2aa57:Skyrim.esm"; },
    [](Json& v) { v["active"]["handler"]["handlerIdentity"] = nullptr; },
    [](Json& v) {
      v["active"]["handler"]["handlerIdentity"]["rootScript"] = "Other";
    },
    [](Json& v) { v["active"]["handler"]["handlerIdentity"]["sequence"] = 2; },
    [](Json& v) {
      v["active"]["handler"]["handlerIdentity"]["receiver"] = "3000";
    },
    [](Json& v) {
      v["active"]["handler"]["handlerIdentity"]["function"] = "OnItemRemoved";
    },
    [](Json& v) { v["active"]["handler"]["script"] = "Other"; },
    [](Json& v) {
      v["active"]["handlerIdentity"] =
        v["active"]["handler"]["handlerIdentity"];
    }
  };
  for (size_t index = 0; index < corrupt.size(); ++index) {
    INFO("Dispatch corruption " << index);
    auto value = original;
    corrupt[index](value);
    REQUIRE_THROWS(QuestJobState::Read(value));
  }
  auto alias = SelectedHandler(true, true).Save();
  alias["active"]["dispatch"]["reference"] = "3000";
  REQUIRE_THROWS(QuestJobState::Read(alias));
  alias = SelectedHandler(true, true).Save();
  alias["active"]["dispatch"]["recipients"][1]["id"] = 2147483648ULL;
  REQUIRE_THROWS(QuestJobState::Read(alias));
}

TEST_CASE(
  "Handler continuations pin inherited implementation state and receiver",
  "[Quest][QuestJob]")
{
  const auto state = SelectedHandler(true, true);
  const auto& job = *state.active;
  auto heap = DispatchHeap(job);
  REQUIRE_NOTHROW(state.ValidateHeap(heap));
  auto& frame = heap["continuations"][job.handler->token];
  SECTION("wrong scope")
  {
    frame["scope"] = job.Scope();
  }
  SECTION("wrong attachment")
  {
    frame["rootScript"] = "ParentScript";
  }
  SECTION("wrong implementation")
  {
    frame["script"] = "ChildScript";
  }
  SECTION("wrong state")
  {
    frame["state"] = "";
  }
  SECTION("wrong event")
  {
    frame["function"] = "OnItemRemoved";
  }
  SECTION("wrong receiver")
  {
    frame["receiver"]["value"] = "14:Skyrim.esm";
  }
  REQUIRE_THROWS(state.ValidateHeap(heap));
}

TEST_CASE(
  "Handlers parent native child stages without sharing host scope identities",
  "[Quest][QuestJob]")
{
  auto state = SelectedHandler(true);
  auto& job = *state.active;
  job.handler->child = 1;
  job.handler->awaitKind = "stage";
  job.handler->awaitKey = "quest-stage";
  auto child = Nested().active->children.front();
  job.children.push_back(child);
  REQUIRE_NOTHROW(state.Save());
  REQUIRE_NOTHROW(state.ValidateHeap(DispatchHeap(job)));
  REQUIRE(&job.Current() == &job.children.back());
  REQUIRE(job.Scope(*job.handler) == "quest-job:1:1:handler:1");
  REQUIRE(job.Scope(job.Current()) == "quest-job:1:1:node:1");
  auto invalid = state;
  invalid.active->children.back().handlerIdentity =
    job.handler->handlerIdentity;
  REQUIRE_THROWS(invalid.Save());
  invalid = state;
  invalid.active->handler.reset();
  REQUIRE_THROWS(invalid.Save());
}

TEST_CASE(
  "Journal saves each event delivery boundary and retains its payload audit",
  "[Quest][QuestJob]")
{
  Fixture f;
  f.Start();
  const auto publish = [&](const QuestJobState& state) {
    QuestJournal::AtomicJob(*f.actor, 1, [&] {
      QuestJournal::SetJobState(*f.actor, state);
      QuestJournal::SetScriptState(
        *f.actor, state.active ? DispatchHeap(*state.active) : Json::object());
    });
  };
  auto state = BodyDone();
  REQUIRE_NOTHROW(publish(state));
  state = Dispatching();
  REQUIRE_NOTHROW(publish(state));
  state = Dispatching(true);
  REQUIRE_NOTHROW(publish(state));
  SECTION("selected handler executes and suspends")
  {
    state = SelectedHandler();
    REQUIRE_NOTHROW(publish(state));
    state = SelectedHandler(true);
    REQUIRE_NOTHROW(publish(state));
    auto& job = *state.active;
    job.completions[job.handler->token] = true;
    auto complete = *SelectedHandler().active->handler;
    complete.cursor = "done";
    complete.accepted = true;
    job.handler = complete;
    HostStep(job, 1);
    REQUIRE_NOTHROW(publish(state));
    job.handler.reset();
    ++job.dispatch->attachment;
    HostStep(job);
    REQUIRE_NOTHROW(publish(state));
  }
  SECTION("absent handler advances exactly one attachment")
  {
    ++state.active->dispatch->attachment;
    HostStep(*state.active, 1);
    REQUIRE_NOTHROW(publish(state));
  }
  auto& job = *state.active;
  ++job.dispatch->recipient;
  job.dispatch->attachment = 0;
  job.dispatch->captured = false;
  job.dispatch->roots.clear();
  job.dispatch->templates.clear();
  job.dispatch->reference.clear();
  HostStep(job);
  REQUIRE_NOTHROW(publish(state));
  job.dispatch.reset();
  ++job.eventHead;
  HostStep(job);
  REQUIRE(job.IsComplete());
  state.completed.push_back(job);
  state.active.reset();
  REQUIRE_NOTHROW(publish(state));
  const auto saved = QuestJournal::JobState(*f.actor);
  REQUIRE(saved.completed.front().events.size() == 1);
  REQUIRE(saved.completed.front().eventHead == 1);
  REQUIRE(saved.Save() == state.Save());
}

TEST_CASE(
  "Journal refuses event payload roster binding and selected-handler rewrites",
  "[Quest][QuestJob]")
{
  Fixture f;
  f.Start();
  const auto publish = [&](const QuestJobState& state) {
    QuestJournal::AtomicJob(*f.actor, 1, [&] {
      QuestJournal::SetJobState(*f.actor, state);
      QuestJournal::SetScriptState(*f.actor, DispatchHeap(*state.active));
    });
  };
  for (const auto& state : { BodyDone(), Dispatching(), Dispatching(true),
                             SelectedHandler(), SelectedHandler(true) })
    publish(state);
  const auto before = f.Saved();
  auto state = SelectedHandler(true);
  auto& job = *state.active;
  SECTION("payload count")
  {
    ++job.events.front().count;
  }
  SECTION("physical reference")
  {
    job.dispatch->reference = "3001";
  }
  SECTION("template chain")
  {
    job.dispatch->templates.push_back("8:Skyrim.esm");
  }
  SECTION("recipient membership")
  {
    job.dispatch->recipients.push_back(
      Json{ { "kind", "alias" }, { "quest", job.quest }, { "id", 1 } });
  }
  SECTION("selected state")
  {
    job.handler->handlerIdentity->state = "other";
  }
  SECTION("implementation")
  {
    job.handler->handlerIdentity->implementation = "Other";
    job.handler->script = "Other";
  }
  SECTION("root context")
  {
    ++job.stage;
    ++job.handler->stage;
  }
  REQUIRE_NOTHROW(state.Save());
  REQUIRE_THROWS(publish(state));
  REQUIRE(f.Saved() == before);
}

TEST_CASE("Journal disallows skipping event host saves or installing advanced "
          "new work",
          "[Quest][QuestJob]")
{
  Fixture f;
  REQUIRE_THROWS(QuestJournal::AtomicJob(
    *f.actor, 1, [&] { QuestJournal::SetJobState(*f.actor, BodyDone()); }));
  f.Start();
  QuestJournal::AtomicJob(
    *f.actor, 1, [&] { QuestJournal::SetJobState(*f.actor, BodyDone()); });
  const auto before = f.Saved();
  SECTION("snapshot and capture combined")
  {
    REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, 1, [&] {
      QuestJournal::SetJobState(*f.actor, Dispatching(true));
    }));
  }
  SECTION("two host transitions in one transaction")
  {
    REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, 1, [&] {
      QuestJournal::SetJobState(*f.actor, Dispatching());
      QuestJournal::SetJobState(*f.actor, Dispatching(true));
    }));
  }
  SECTION("snapshot omitted shared instruction accounting")
  {
    auto state = Dispatching();
    --state.active->instructions;
    REQUIRE_THROWS(QuestJournal::AtomicJob(
      *f.actor, 1, [&] { QuestJournal::SetJobState(*f.actor, state); }));
  }
  REQUIRE(f.Saved() == before);
}

TEST_CASE(
  "Consumed event history remains bounded and cannot become pending again",
  "[Quest][QuestJob]")
{
  auto state = Done(Waiting());
  auto& job = state.completed.front();
  REQUIRE(job.IsComplete());
  REQUIRE(job.events.size() == job.eventHead);
  job.events.resize(QuestJobState::MaxEvents + 1);
  job.eventHead = static_cast<uint32_t>(job.events.size());
  REQUIRE_THROWS(state.Save());
  state = Done(Waiting());
  --state.completed.front().eventHead;
  REQUIRE_THROWS(state.Save());
  auto active = SelectedHandler();
  active.active->dispatch->roots.resize(QuestJobState::MaxAttachments + 1,
                                        "Root");
  REQUIRE_THROWS(active.Save());
  active = SelectedHandler();
  active.active->segment = QuestJobState::MaxSegments + 1;
  REQUIRE_THROWS(active.Save());
}

TEST_CASE(
  "Schema four histories migrate without reclassifying commands or stacks",
  "[Quest][QuestJob]")
{
  for (const auto& original :
       { Begin(), Waiting(), TimerWaiting(), Nested(true),
         SelectedHandler(true, true), Done(Waiting()), History(2, 0) }) {
    auto legacy = Legacy(original, 4);
    auto migrated = QuestJobState::Read(legacy);
    REQUIRE(migrated.Save() == original.Save());
    REQUIRE(migrated.retiredThrough == 0);
    REQUIRE(migrated.Save()["schemaVersion"] == 5);
    if (migrated.active) {
      REQUIRE_FALSE(migrated.active->sequenced);
      REQUIRE(migrated.active->Scope() == original.active->Scope());
      REQUIRE(migrated.active->NextToken() == original.active->NextToken());
      REQUIRE_NOTHROW(migrated.ValidateHeap(DispatchHeap(*migrated.active)));
    }
    for (const auto& job : migrated.completed)
      REQUIRE_FALSE(job.sequenced);
  }
  auto legacy = Legacy(Waiting(), 4);
  legacy["active"]["command"] = "quest-request:1:1";
  REQUIRE_FALSE(QuestJobState::Read(legacy).active->sequenced);
  legacy["active"]["sequenced"] = false;
  REQUIRE_THROWS(QuestJobState::Read(legacy));
}

TEST_CASE("Sequenced commands are canonical while legacy command names stay "
          "independent",
          "[Quest][QuestJob]")
{
  auto state = Waiting();
  state.active->sequenced = true;
  state.active->command = "quest-request:1:1";
  REQUIRE_NOTHROW(state.Save());
  REQUIRE_NOTHROW(state.ValidateHeap(Heap(*state.active)));
  for (const auto* command :
       { "", "quest-request:01:1", "quest-request:1:01", "quest-request:1:2",
         "quest-request:2:1", "quest-request:+1:1", "Quest-request:1:1",
         "command-one" }) {
    auto invalid = state;
    invalid.active->command = command;
    REQUIRE_THROWS(invalid.Save());
  }
  auto value = state.Save();
  value["active"]["sequenced"] = 1;
  REQUIRE_THROWS(QuestJobState::Read(value));
  auto mixed = History(1, 1);
  mixed.completed.front().command = mixed.completed.back().command;
  REQUIRE_NOTHROW(mixed.Save());
  REQUIRE(QuestJobState::Read(mixed.Save()).Save() == mixed.Save());
  mixed.completed.push_back(CompletedRecord(mixed.nextJob++, false));
  mixed.completed.back().command = mixed.completed.front().command;
  REQUIRE_THROWS(mixed.Save());
}

TEST_CASE("Retired histories preserve sorted permanent legacy records and a "
          "contiguous recent tail",
          "[Quest][QuestJob]")
{
  QuestJobState state;
  for (uint64_t id = 1; id <= 6; ++id)
    state.completed.push_back(CompletedRecord(state.nextJob++, id % 2 == 0));
  state.completed.erase(state.completed.begin() + 1);
  state.retiredThrough = 2;
  REQUIRE_NOTHROW(state.Save());
  AddPending(state, true);
  REQUIRE_NOTHROW(state.Save());
  const auto original = state.Save();
  const std::vector<std::function<void(Json&)>> corrupt = {
    [](Json& v) { v["retiredThrough"] = 0; },
    [](Json& v) { v["retiredThrough"] = 3; },
    [](Json& v) { v["retiredThrough"] = v["nextJob"]; },
    [](Json& v) { v["retiredThrough"] = -1; },
    [](Json& v) { v["retiredThrough"] = 2.0; },
    [](Json& v) { v.erase("retiredThrough"); },
    [](Json& v) { v["nextJob"] = 9; },
    [](Json& v) { v["completed"].erase(v["completed"].begin() + 2); },
    [](Json& v) { std::swap(v["completed"][0], v["completed"][1]); },
    [](Json& v) {
      v["completed"][0]["sequenced"] = true;
      v["completed"][0]["command"] = "quest-request:1:1";
    },
    [](Json& v) {
      v["active"]["id"] = 2;
      v["active"]["command"] = "quest-request:1:2";
    },
    [](Json& v) { v["completed"][1]["id"] = 1; },
    [](Json& v) { v["completed"][2]["generation"] = 2; }
  };
  for (size_t index = 0; index < corrupt.size(); ++index) {
    INFO("Retention corruption " << index);
    auto value = original;
    corrupt[index](value);
    REQUIRE_THROWS(QuestJobState::Read(value));
  }
}

TEST_CASE(
  "Legacy and sequenced history capacities apply to their own namespaces",
  "[Quest][QuestJob]")
{
  auto state =
    History(QuestJobState::MaxCompleted, QuestJobState::MaxRecentCompleted);
  REQUIRE(state.completed.size() == 288);
  REQUIRE_NOTHROW(state.Save());
  auto activeSequence = state;
  AddPending(activeSequence, true);
  REQUIRE_THROWS(activeSequence.Save());
  auto activeLegacy = state;
  AddPending(activeLegacy, false);
  REQUIRE_THROWS(activeLegacy.Save());
  state.RetireForAdmission();
  AddPending(state, true);
  REQUIRE_NOTHROW(state.Save());
  auto legacySlot = History(QuestJobState::MaxCompleted - 1,
                            QuestJobState::MaxRecentCompleted);
  AddPending(legacySlot, false);
  REQUIRE_NOTHROW(legacySlot.Save());
  auto tooManyRecent = History(0, QuestJobState::MaxRecentCompleted + 1);
  REQUIRE_THROWS(tooManyRecent.Save());
  auto tooManyLegacy = History(QuestJobState::MaxCompleted + 1, 0);
  REQUIRE_THROWS(tooManyLegacy.Save());
}

TEST_CASE("Admission retirement removes only the oldest sequenced completion",
          "[Quest][QuestJob]")
{
  QuestJobState state;
  state.completed.push_back(CompletedRecord(state.nextJob++, false));
  state.completed.push_back(CompletedRecord(state.nextJob++, true));
  state.completed.push_back(CompletedRecord(state.nextJob++, false));
  for (size_t index = 1; index < QuestJobState::MaxRecentCompleted; ++index)
    state.completed.push_back(CompletedRecord(state.nextJob++, true));
  const auto before = state.Save();
  auto expected = before;
  expected["completed"].erase(expected["completed"].begin() + 1);
  expected["retiredThrough"] = 2;
  state.RetireForAdmission();
  REQUIRE(state.Save() == expected);
  REQUIRE(state.completed.front().id == 1);
  REQUIRE(state.completed[1].id == 3);
  REQUIRE(state.nextJob == before["nextJob"]);
  state.RetireForAdmission();
  REQUIRE(state.Save() == expected);
  // Filling the recent window again advances past another permanent legacy
  // record without dropping or rewriting that record's full audit payload.
  state.completed.push_back(CompletedRecord(state.nextJob++, true));
  const auto refill = state.Save();
  expected = refill;
  expected["completed"].erase(expected["completed"].begin() + 2);
  expected["retiredThrough"] = 4;
  state.RetireForAdmission();
  REQUIRE(state.Save() == expected);
  REQUIRE(state.Save()["completed"][0] == before["completed"][0]);
  REQUIRE(state.Save()["completed"][1] == before["completed"][2]);
}

TEST_CASE("Retirement refuses active or malformed typed state without "
          "changing history",
          "[Quest][QuestJob]")
{
  auto active = History(1, QuestJobState::MaxRecentCompleted - 1);
  AddPending(active, true);
  const auto before = active.Save();
  REQUIRE_THROWS(active.RetireForAdmission());
  REQUIRE(active.Save() == before);
  auto state = History(2, 4);
  const auto untouched = state.Save();
  state.RetireForAdmission();
  REQUIRE(state.Save() == untouched);
  auto malformed = History(1, QuestJobState::MaxRecentCompleted);
  malformed.completed.back().command = "malformed";
  const auto idsBefore = malformed.nextJob;
  REQUIRE_THROWS(malformed.RetireForAdmission());
  REQUIRE(malformed.retiredThrough == 0);
  REQUIRE(malformed.nextJob == idsBefore);
  REQUIRE(malformed.completed.size() == 33);
  REQUIRE(malformed.completed.back().command == "malformed");
}

TEST_CASE(
  "Retired runner state cannot regain the standalone continuation exemption",
  "[Quest][QuestJob]")
{
  auto state = History(0, QuestJobState::MaxRecentCompleted);
  state.RetireForAdmission();
  REQUIRE(state.retiredThrough == 1);
  REQUIRE_NOTHROW(state.ValidateHeap(Json::object()));
  REQUIRE_THROWS(state.ValidateHeap(Heap(*Waiting().active)));
  const auto saved = state.Save();
  auto migrated = QuestJobState::Read(saved);
  REQUIRE(migrated.retiredThrough == state.retiredThrough);
  REQUIRE(migrated.nextJob == state.nextJob);
  REQUIRE(migrated.Save() == saved);
}

TEST_CASE("Active Journal jobs cannot retire history or change permanent "
          "completion results",
          "[Quest][QuestJob]")
{
  Fixture f;
  auto history = History(2, QuestJobState::MaxRecentCompleted);
  history.RetireForAdmission();
  f.SeedJobs(history);
  auto candidate = history;
  AddPending(candidate, true);
  f.PublishJobs(candidate);
  const auto before = f.Saved();
  SECTION("advance frontier and remove another sequenced completion")
  {
    candidate.retiredThrough = candidate.completed[2].id;
    candidate.completed.erase(candidate.completed.begin() + 2);
  }
  SECTION("forget retained legacy command below frontier")
  {
    candidate.completed.erase(candidate.completed.begin());
  }
  SECTION("rewrite retained completion result")
  {
    candidate.completed.front().completions.begin()->second = false;
  }
  SECTION("reclassify active sequenced request as legacy")
  {
    candidate.active->sequenced = false;
  }
  // These candidates are individually well-formed: the Journal must reject
  // their relationship to the already-published history.
  REQUIRE_NOTHROW(candidate.Save());
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, candidate.active->id, [&] {
    QuestJournal::SetJobState(*f.actor, candidate);
  }));
  REQUIRE(f.Saved() == before);
}

TEST_CASE("Journal admission permits only the exact required oldest-request "
          "retirement",
          "[Quest][QuestJob]")
{
  Fixture f;
  const auto initial = History(2, QuestJobState::MaxRecentCompleted);
  f.SeedJobs(initial);
  const auto before = f.Saved();
  auto candidate = initial;
  candidate.RetireForAdmission();
  AddPending(candidate, true);
  bool validCodec = true;
  SECTION("omit required retirement")
  {
    candidate = initial;
    AddPending(candidate, true);
    validCodec = false;
  }
  SECTION("retire two sequenced results")
  {
    candidate.retiredThrough = candidate.completed[2].id;
    candidate.completed.erase(candidate.completed.begin() + 2);
  }
  SECTION("retire the wrong sequenced result")
  {
    candidate = initial;
    candidate.retiredThrough = candidate.completed[3].id;
    candidate.completed.erase(candidate.completed.begin() + 3);
    AddPending(candidate, true);
    validCodec = false;
  }
  SECTION("drop a permanent legacy result with the correct frontier")
  {
    candidate.completed.erase(candidate.completed.begin());
  }
  SECTION("rewrite a retained sequenced result")
  {
    candidate.completed[2].completions.begin()->second = false;
  }
  if (validCodec)
    REQUIRE_NOTHROW(candidate.Save());
  else
    REQUIRE_THROWS(candidate.Save());
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, candidate.active->id, [&] {
    QuestJournal::SetJobState(*f.actor, candidate);
  }));
  REQUIRE(f.Saved() == before);
}

TEST_CASE("Journal admits sequenced work beyond all permanent legacy slots "
          "and retires durably",
          "[Quest][QuestJob]")
{
  Fixture f;
  auto state =
    History(QuestJobState::MaxCompleted, QuestJobState::MaxRecentCompleted);
  f.SeedJobs(state);
  const auto permanent = state.Save()["completed"];
  for (uint64_t admission = 0; admission < 2; ++admission) {
    state.RetireForAdmission();
    AddPending(state, true);
    const auto id = state.active->id;
    REQUIRE(id > QuestJobState::MaxCompleted);
    REQUIRE_NOTHROW(f.PublishJobs(state));
    REQUIRE(QuestJournal::JobState(*f.actor).Save() == state.Save());
    state = Done(state); // A pristine body has no pending event dispatch.
    REQUIRE_NOTHROW(f.PublishJobs(state));
    REQUIRE(state.retiredThrough ==
            QuestJobState::MaxCompleted + admission + 1);
    const auto saved = QuestJournal::JobState(*f.actor).Save();
    REQUIRE(saved == state.Save());
    REQUIRE(saved["completed"].size() ==
            QuestJobState::MaxCompleted + QuestJobState::MaxRecentCompleted);
    for (size_t index = 0; index < QuestJobState::MaxCompleted; ++index)
      REQUIRE(saved["completed"][index] == permanent[index]);
  }
}

TEST_CASE("Sequenced admission retires by canonical byte budget before "
          "filling the count window",
          "[Quest][QuestJob]")
{
  auto state = ByteHeavyHistory();
  REQUIRE(state.completed.size() < QuestJobState::MaxRecentCompleted);
  const auto original = state.Save();
  size_t canonicalBytes = 0;
  for (const auto& job : original["completed"])
    if (job["sequenced"].get<bool>())
      canonicalBytes += job.dump().size();
  REQUIRE(state.RetainedSequencedBytes() == canonicalBytes);
  REQUIRE(canonicalBytes > QuestJobState::MaxRecentBytes);
  // A just-completed large result may exceed the window while still at rest.
  REQUIRE(QuestJobState::Read(original).Save() == original);
  auto admission = state;
  AddPending(admission, true);
  REQUIRE_THROWS(admission.Save());
  auto legacyAdmission = state;
  AddPending(legacyAdmission, false);
  REQUIRE_NOTHROW(legacyAdmission.Save());
  state.RetireForAdmission();
  auto expected = original;
  expected["completed"].erase(expected["completed"].begin() + 3);
  expected["completed"].erase(expected["completed"].begin() + 1);
  expected["retiredThrough"] = 4;
  REQUIRE(state.Save() == expected);
  REQUIRE(state.RetainedSequencedBytes() <= QuestJobState::MaxRecentBytes);
  REQUIRE(state.completed.size() == 4);
  // The large permanent legacy audit is excluded from the recent byte window.
  REQUIRE(state.Save().dump().size() > QuestJobState::MaxRecentBytes);
  AddPending(state, true);
  REQUIRE_NOTHROW(state.Save());
}

TEST_CASE("Journal admits only the exact byte-triggered retirement and "
          "preserves legacy audit",
          "[Quest][QuestJob]")
{
  Fixture f;
  const auto initial = ByteHeavyHistory();
  f.SeedJobs(initial);
  const auto before = f.Saved();
  auto admitted = initial;
  admitted.RetireForAdmission();
  AddPending(admitted, true);
  auto excessive = admitted;
  excessive.retiredThrough = excessive.completed[2].id;
  excessive.completed.erase(excessive.completed.begin() + 2);
  REQUIRE_NOTHROW(excessive.Save());
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, excessive.active->id, [&] {
    QuestJournal::SetJobState(*f.actor, excessive);
  }));
  REQUIRE(f.Saved() == before);
  REQUIRE_NOTHROW(f.PublishJobs(admitted));
  const auto saved = QuestJournal::JobState(*f.actor);
  REQUIRE(saved.Save() == admitted.Save());
  REQUIRE(saved.retiredThrough == 4);
  REQUIRE(saved.RetainedSequencedBytes() <= QuestJobState::MaxRecentBytes);
  REQUIRE(saved.Save()["completed"][0] == before["jobs"]["completed"][0]);
  REQUIRE(saved.Save()["completed"][1] == before["jobs"]["completed"][2]);
}
