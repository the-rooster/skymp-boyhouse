#include "QuestJobState.h"
#include "QuestJournal.h"
#include "TestUtils.hpp"
#include "save_storages/ISaveStorage.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <functional>

extern espm::Loader& GetEspmLoader();

namespace {
using Json = nlohmann::json;
constexpr uint32_t kJobQuest = 0x4e50d;

QuestJobState::Job* Single(QuestJobState& state)
{
  return state.live.empty() ? nullptr : &state.live.begin()->second;
}

const QuestJobState::Job* Single(const QuestJobState& state)
{
  return state.live.empty() ? nullptr : &state.live.begin()->second;
}

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
  state.live.emplace(job.id, job);
  state.nextJob = 2;
  return state;
}

QuestJobState Waiting()
{
  auto state = Begin();
  auto& job = *Single(state);
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
  auto job = *Single(state);
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
  state.live.clear();
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
      Json::array({ { { "script", stage.script },
                      { "hash",
                        stage.contentHash.empty() ? std::string(64, 'a')
                                                  : stage.contentHash } } }) },
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
  auto& job = *Single(state);
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
  if (version < 6) {
    REQUIRE(saved["live"].size() <= 1);
    saved["active"] = saved["live"].empty() ? Json(nullptr) : saved["live"][0];
    saved.erase("live");
    saved.erase("retiredRanges");
  }
  if (version < 5)
    saved.erase("retiredThrough");
  auto strip = [version](Json& job) {
    REQUIRE(job["kind"] == "stage");
    job.erase("kind");
    job.erase("interaction");
    job.erase("interactionDone");
    if (version < 6)
      job.erase("environmentHash");
    if (version < 5)
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
  if (version < 6) {
    if (!saved["active"].is_null())
      strip(saved["active"]);
  } else {
    for (auto& job : saved["live"])
      strip(job);
  }
  for (auto& job : saved["completed"])
    strip(job);
  return saved;
}

QuestJobState MigratedLegacy(QuestJobState state, uint64_t version = 1)
{
  if (version < 5)
    state.retiredThrough = 0;
  for (auto& [id, job] : state.live) {
    if (version < 6)
      job.environmentHash.clear();
    if (version < 5)
      job.sequenced = false;
    if (version < 3)
      job.contentMode = "legacy";
  }
  for (auto& job : state.completed) {
    if (version < 6)
      job.environmentHash.clear();
    if (version < 5)
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
  Single(state)->awaitKind = "timer";
  Single(state)->awaitKey = "Utility.Wait";
  Single(state)->timer =
    QuestJobState::Timer{ 1700000000000, 1700000000100, 100 };
  return state;
}

QuestJobState BodyDone()
{
  auto state = Begin();
  auto& job = *Single(state);
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
  auto& job = *Single(state);
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
  auto& job = *Single(state);
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
  auto heap = Heap(*Single(Waiting()));
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
  auto job = *Single(Begin());
  job.id = state.nextJob++;
  job.generation = state.generation;
  job.sequenced = sequenced;
  job.command = sequenced ? "quest-request:" + std::to_string(job.generation) +
      ":" + std::to_string(job.id)
                          : "legacy-" + std::to_string(job.id);
  state.live.emplace(job.id, job);
}

QuestJobState InteractionBegin()
{
  auto state = Begin();
  auto& job = *Single(state);
  static_cast<QuestJobState::Stage&>(job) = QuestJobState::Stage{};
  job.cursor = "done";
  job.kind = QuestJobState::Kind::Interaction;
  job.interactionDone = false;
  job.environmentHash = std::string(64, 'e');
  job.interaction = QuestJobState::Interaction{
    "OnActivate", "10:Skyrim.esm",    "14:Skyrim.esm",   "3001",
    "3000",       { "7:Skyrim.esm" }, { "8:Skyrim.esm" }
  };
  return state;
}

QuestJobState InteractionHandler(bool waiting = false, bool alias = false)
{
  auto state = InteractionBegin();
  auto& job = *Single(state);
  job.dispatch = QuestJobState::Dispatch{};
  auto& dispatch = *job.dispatch;
  dispatch.recipients = { job.interaction->target };
  dispatch.captured = true;
  dispatch.reference = job.interaction->targetReference;
  dispatch.templates = job.interaction->targetTemplates;
  dispatch.roots = { "ChildScript" };
  if (alias) {
    dispatch.recipients.push_back(Json{
      { "kind", "alias" }, { "quest", "4e50d:Skyrim.esm" }, { "id", 1 } });
    dispatch.recipient = 1;
    dispatch.reference.clear();
    dispatch.templates.clear();
  }
  QuestJobState::Stage handler;
  handler.handlerIdentity =
    QuestJobState::HandlerIdentity{ dispatch.recipients[dispatch.recipient],
                                    "ChildScript",
                                    "ParentScript",
                                    "busy",
                                    job.interaction->name,
                                    job.nextHandler++ };
  job.handler = handler;
  job.segment = job.steps = job.instructions = 3;
  if (waiting) {
    auto& current = *job.handler;
    current.cursor = "waiting";
    current.accepted = true;
    current.script = "ParentScript";
    current.function = job.interaction->name;
    current.awaitKind = "timer";
    current.awaitKey = "Utility.Wait";
    current.timer = QuestJobState::Timer{ 1700000000000, 1700000000100, 100 };
    current.token = job.NextToken();
    ++job.nextOperation;
    HostStep(job, 2);
  }
  return state;
}

QuestJobState::Job Numbered(QuestJobState::Job job, uint64_t id,
                            bool sequenced = true)
{
  const auto oldScope = job.Scope();
  job.id = id;
  job.sequenced = sequenced;
  job.command = sequenced ? "quest-request:" + std::to_string(job.generation) +
      ":" + std::to_string(id)
                          : "legacy-" + std::to_string(id);
  job.environmentHash = std::string(64, 'e');
  const auto remap = [&](std::string token) {
    return token.empty() ? token : job.Scope() + token.substr(oldScope.size());
  };
  job.token = remap(job.token);
  if (job.handler)
    job.handler->token = remap(job.handler->token);
  for (auto& child : job.children)
    child.token = remap(child.token);
  std::map<std::string, QuestJobState::Completion> completions;
  for (const auto& [token, result] : job.completions)
    completions.emplace(remap(token), result);
  job.completions = std::move(completions);
  return job;
}

void CompleteLive(QuestJobState& state, uint64_t id)
{
  auto job = state.live.at(id);
  REQUIRE(job.children.empty());
  REQUIRE_FALSE(job.handler);
  REQUIRE_FALSE(job.dispatch);
  if (!job.token.empty())
    job.completions[job.token] = job.awaitKind == "timer"
      ? QuestJobState::Completion::None()
      : QuestJobState::Completion(true);
  QuestJobState::Stage done;
  done.quest = job.quest;
  done.contentHash = job.contentHash;
  done.stage = job.stage;
  done.cursor = "done";
  done.accepted = job.accepted;
  static_cast<QuestJobState::Stage&>(job) = done;
  job.eventHead = static_cast<uint32_t>(job.events.size());
  state.live.erase(id);
  state.completed.insert(
    std::lower_bound(
      state.completed.begin(), state.completed.end(), id,
      [](const auto& item, uint64_t wanted) { return item.id < wanted; }),
    std::move(job));
}

Json AllLiveHeap(const QuestJobState& state)
{
  auto heap = Heap(*Single(Waiting()));
  heap["continuations"].clear();
  for (const auto& [id, job] : state.live)
    heap["continuations"].update(DispatchHeap(job)["continuations"]);
  return heap;
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
    const auto id = Single(jobs) ? Single(jobs)->id : jobs.completed.back().id;
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
  REQUIRE(Single(state)->Scope() == "quest-job:1:1");
  REQUIRE(Single(state)->NextToken() == "quest-job:1:1:2");
  Single(state)->completions[Single(state)->token] = false;
  Single(state)->token = Single(state)->NextToken();
  ++Single(state)->nextOperation;
  Single(state)->segment = 2;
  REQUIRE(Single(state)->token == "quest-job:1:1:2");
  REQUIRE_NOTHROW(state.Save());
}

TEST_CASE("Quest jobs reject corrupt exact fields identities and cursor state",
          "[Quest][QuestJob]")
{
  const auto original = Waiting().Save();
  std::vector<std::function<void(Json&)>> corrupt = {
    [](Json& v) { v["unknown"] = 1; },
    [](Json& v) { v.erase("live"); },
    [](Json& v) { v["schemaVersion"] = 8; },
    [](Json& v) { v["generation"] = 0; },
    [](Json& v) { v["nextJob"] = 1; },
    [](Json& v) { v["nextJob"] = UINT64_MAX; },
    [](Json& v) { v["live"][0]["id"] = 1.0; },
    [](Json& v) { v["live"][0]["generation"] = 2; },
    [](Json& v) { v["live"][0]["actor"] = "03000"; },
    [](Json& v) { v["live"][0]["owner"] = "3000:Skyrim.esm"; },
    [](Json& v) { v["live"][0]["quest"] = "4e50d"; },
    [](Json& v) { v["live"][0]["quest"] = "1000000:Skyrim.esm"; },
    [](Json& v) { v["live"][0]["command"] = ""; },
    [](Json& v) { v["live"][0]["command"] = std::string(129, 'a'); },
    [](Json& v) { v["live"][0]["instance"] = std::string(301, 'a'); },
    [](Json& v) { v["live"][0]["contentHash"] = std::string(64, 'A'); },
    [](Json& v) { v["live"][0]["script"] = std::string("a\0b", 3); },
    [](Json& v) { v["live"][0]["function"] = ""; },
    [](Json& v) { v["live"][0]["awaitKey"] = ""; },
    [](Json& v) { v["live"][0]["accepted"] = false; },
    [](Json& v) { v["live"][0]["segment"] = 0; },
    [](Json& v) { v["live"][0]["instructions"] = 0; },
    [](Json& v) { v["live"][0]["instructions"] = 100001; },
    [](Json& v) { v["live"][0]["steps"] = 100001; },
    [](Json& v) { v["live"][0]["log"] = -1; },
    [](Json& v) { v["live"][0]["token"] = "quest-job:1:2:1"; },
    [](Json& v) { v["live"][0]["cursor"] = "begin"; },
    [](Json& v) { v["live"][0]["cursor"] = "done"; },
    [](Json& v) { v["live"][0]["completions"]["quest-job:1:1:2"] = true; },
    [](Json& v) { v["live"][0]["events"][0]["target"] = "3000"; },
    [](Json& v) { v["live"][0]["events"][0]["count"] = 0; },
    [](Json& v) {
      v["live"][0]["events"][0]["count"] = uint64_t(INT32_MAX) + 1;
    },
    [](Json& v) { v["live"][0]["events"][0]["added"] = 1; },
    [](Json& v) { v["live"][0]["events"][0]["extra"] = false; }
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
    Single(state)->id = 0;
  }
  SECTION("pending event overflow")
  {
    Single(state)->events.resize(4097);
  }
  SECTION("operation overflow")
  {
    for (size_t n = 1; n <= 257; ++n)
      Single(state)
        ->completions[Single(state)->Scope() + ":" + std::to_string(n)] = true;
  }
  SECTION("completed history overflow")
  {
    state.completed.resize(257);
  }
  SECTION("duplicate command")
  {
    auto old = Done(Begin()).completed.front();
    state.completed.push_back(old);
    Single(state)->id = 2;
    Single(state)->token = Single(state)->Scope() + ":1";
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
  const auto job = *Single(state);
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
  const auto heap = Heap(*Single(waiting));
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
  const auto& job = *Single(state);
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
  const auto& job = *Single(state);
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
  auto initialHeap = Heap(*Single(Waiting()));
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
  Single(waiting)->events.clear();
  const auto before = f.Saved();
  REQUIRE_THROWS(QuestJournal::AtomicJob(
    *f.actor, 1, [&] { QuestJournal::SetJobState(*f.actor, waiting); }));
  REQUIRE(f.Saved() == before);
  REQUIRE_NOTHROW(QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetScriptState(*f.actor, Heap(*Single(waiting)));
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
  REQUIRE_FALSE(QuestJournal::JobState(*f.actor).HasLive());
  REQUIRE(QuestJournal::PersonalJobState(*f.actor).Save() == Begin().Save());
  REQUIRE_FALSE(QuestJournal::PersonalJobState(host).HasLive());
}

TEST_CASE(
  "Legacy quest jobs migrate operation identities without losing history",
  "[Quest][QuestJob]")
{
  for (const auto& state : { Begin(), Waiting(), Done(Waiting()) }) {
    const auto legacy = Legacy(state);
    const auto migrated = QuestJobState::Read(legacy);
    REQUIRE(migrated.Save() == MigratedLegacy(state).Save());
    REQUIRE(migrated.Save()["schemaVersion"] == 7);
  }
  auto external = Waiting();
  Single(external)->awaitKey = "quest-stage";
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
    const auto& job = *Single(state);
    REQUIRE(&job.Current() == &job.children.back());
    REQUIRE(job.Scope(job) == "quest-job:1:1");
    REQUIRE(job.Scope(job.Current()) == "quest-job:1:1:node:1");
    REQUIRE(QuestJobState::Read(state.Save()).Save() == state.Save());
    REQUIRE_NOTHROW(state.ValidateHeap(NestedHeap(job)));
  }
  auto state = Nested(true);
  FinishChild(*Single(state));
  REQUIRE_NOTHROW(state.Save());
  REQUIRE_NOTHROW(state.ValidateHeap(NestedHeap(*Single(state))));
  REQUIRE_FALSE(Single(state)->events.empty());
  REQUIRE(Single(state)->completions.at("quest-job:1:1:2") ==
          QuestJobState::Completion(true));
  REQUIRE(Single(state)->NextToken() == "quest-job:1:1:3");
}

TEST_CASE("Nested stage codecs reject broken links reused tokens and "
          "incomplete histories",
          "[Quest][QuestJob]")
{
  const auto original = Nested(true).Save();
  const std::vector<std::function<void(Json&)>> corrupt = {
    [](Json& v) { v["live"][0]["node"] = 1; },
    [](Json& v) { v["live"][0]["child"] = 2; },
    [](Json& v) { v["live"][0]["awaitKey"] = "external"; },
    [](Json& v) { v["live"][0]["children"][0]["node"] = 0; },
    [](Json& v) { v["live"][0]["children"][0]["node"] = 2; },
    [](Json& v) { v["live"][0]["children"][0]["child"] = 2; },
    [](Json& v) { v["live"][0]["children"][0]["extra"] = 1; },
    [](Json& v) { v["live"][0]["children"][0]["token"] = "quest-job:1:1:1"; },
    [](Json& v) {
      v["live"][0]["children"][0]["token"] = "quest-job:1:1:node:1:2";
    },
    [](Json& v) { v["live"][0]["children"][0]["token"] = "quest-job:1:1:02"; },
    [](Json& v) { v["live"][0]["children"].clear(); },
    [](Json& v) { v["live"][0]["nextOperation"] = 4; },
    [](Json& v) { v["live"][0]["nextOperation"] = 258; },
    [](Json& v) { v["live"][0]["completions"]["quest-job:1:1:2"] = true; },
    [](Json& v) { v["live"][0]["children"][0]["cursor"] = "begin"; },
    [](Json& v) { v["live"][0]["children"][0]["cursor"] = "done"; },
    [](Json& v) { v["live"][0]["children"][0]["quest"] = "3000"; }
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
  const auto& job = *Single(state);
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
  auto& job = *Single(state);
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
      QuestJournal::SetScriptState(*f.actor, NestedHeap(*Single(state)));
      QuestJournal::SetJobState(*f.actor, state);
    });
  };
  auto state = Nested();
  REQUIRE_NOTHROW(publish(state));
  state = Nested(true);
  REQUIRE_NOTHROW(publish(state));
  FinishChild(*Single(state));
  REQUIRE_NOTHROW(publish(state));
  auto& job = *Single(state);
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
    QuestJournal::SetScriptState(*f.actor, NestedHeap(*Single(begin)));
    QuestJournal::SetJobState(*f.actor, begin);
  });
  QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetScriptState(*f.actor, NestedHeap(*Single(state)));
    QuestJournal::SetJobState(*f.actor, state);
  });
  const auto before = f.Saved();
  auto& job = *Single(state);
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
    QuestJournal::SetScriptState(*f.actor, NestedHeap(*Single(nested)));
    QuestJournal::SetJobState(*f.actor, nested);
    QuestJournal::SetJobState(*f.actor, nested);
  }));
  const auto before = f.Saved();
  auto doneChild = nested;
  FinishChild(*Single(doneChild));
  auto sibling = doneChild;
  auto& job = *Single(sibling);
  job.completions[job.token] = true;
  job.children.pop_back();
  job.child = job.nextOperation;
  job.token = job.NextToken();
  ++job.nextOperation;
  auto child = Single(nested)->children.front();
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
  Single(state)->log = 10;
  QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetScriptState(*f.actor, Heap(*Single(state)));
    QuestJournal::SetJobState(*f.actor, state);
  });
  const auto before = f.Saved();
  auto& job = *Single(state);
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
  auto& job = *Single(state);
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
      Single(migrated) ? *Single(migrated) : migrated.completed.back();
    REQUIRE_FALSE(job.automatic);
    REQUIRE_FALSE(job.timer);
    for (const auto& [token, completion] : job.completions)
      REQUIRE(completion.kind == QuestJobState::Completion::Kind::Boolean);
  }
  auto external = Waiting();
  Single(external)->awaitKey = "quest-stage";
  REQUIRE(Single(QuestJobState::Read(Legacy(external, 2)))->awaitKind ==
          "bool");
  auto corrupt = Legacy(Nested(), 2);
  corrupt["active"]["children"][0]["timer"] = nullptr;
  REQUIRE_THROWS(QuestJobState::Read(corrupt));
}

TEST_CASE("Timer jobs round trip UTC deadlines and typed None completions",
          "[Quest][QuestJob]")
{
  auto state = TimerWaiting();
  Single(state)->automatic = true;
  for (const auto& value : { state, Done(state) })
    REQUIRE(QuestJobState::Read(value.Save()).Save() == value.Save());
  const auto saved = state.Save();
  REQUIRE(saved["schemaVersion"] == 7);
  REQUIRE(saved["live"][0]["timer"]["policy"] == "utc-deadline-v1");
  const auto done = Done(state);
  const auto& job = done.completed.front();
  REQUIRE(job.completions.at("quest-job:1:1:1") ==
          QuestJobState::Completion::None());
  REQUIRE((done.Save()["completed"][0]["completions"]["quest-job:1:1:1"] ==
           Json{ { "kind", "none" } }));
  REQUIRE_NOTHROW(state.ValidateHeap(Heap(*Single(state))));
  REQUIRE_NOTHROW(done.ValidateHeap(Json::object()));
  constexpr uint64_t max = 9007199254740991ULL;
  Single(state)->timer = QuestJobState::Timer{ max - 86400000, max, 86400000 };
  REQUIRE_NOTHROW(state.Save());
  Single(state)->timer = QuestJobState::Timer{ max, max, 0 };
  REQUIRE_NOTHROW(state.Save());
}

TEST_CASE(
  "Timer codecs reject inconsistent deadlines wait kinds and exact fields",
  "[Quest][QuestJob]")
{
  const auto original = TimerWaiting().Save();
  const std::vector<std::function<void(Json&)>> corrupt = {
    [](Json& v) { v["live"][0]["automatic"] = 1; },
    [](Json& v) { v["live"][0].erase("automatic"); },
    [](Json& v) { v["live"][0]["awaitKind"] = ""; },
    [](Json& v) { v["live"][0]["awaitKind"] = "other"; },
    [](Json& v) { v["live"][0]["awaitKind"] = "bool"; },
    [](Json& v) { v["live"][0]["awaitKind"] = "stage"; },
    [](Json& v) { v["live"][0]["child"] = 1; },
    [](Json& v) { v["live"][0]["timer"] = nullptr; },
    [](Json& v) { v["live"][0]["timer"] = false; },
    [](Json& v) { v["live"][0]["timer"]["extra"] = 0; },
    [](Json& v) { v["live"][0]["timer"].erase("policy"); },
    [](Json& v) { v["live"][0]["timer"]["policy"] = "game-time"; },
    [](Json& v) { v["live"][0]["timer"]["issuedUnixMs"] = -1; },
    [](Json& v) { v["live"][0]["timer"]["issuedUnixMs"] = 1.0; },
    [](Json& v) { v["live"][0]["timer"]["dueUnixMs"] = 1700000000101ULL; },
    [](Json& v) { v["live"][0]["timer"]["dueUnixMs"] = 9007199254740992ULL; },
    [](Json& v) { v["live"][0]["timer"]["durationMs"] = 86400001; },
    [](Json& v) {
      v["live"][0]["timer"]["issuedUnixMs"] = 9007199254740991ULL;
      v["live"][0]["timer"]["dueUnixMs"] = 9007199254740991ULL;
    }
  };
  for (size_t index = 0; index < corrupt.size(); ++index) {
    INFO("Timer corruption " << index);
    auto value = original;
    corrupt[index](value);
    REQUIRE_THROWS(QuestJobState::Read(value));
  }
  auto begin = Begin();
  Single(begin)->timer = Single(TimerWaiting())->timer;
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
    const auto& job = *Single(state);
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
  Single(waiting)->events.clear();
  QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetScriptState(*f.actor, Heap(*Single(waiting)));
    QuestJournal::SetJobState(*f.actor, waiting);
  });
  const auto before = f.Saved();
  auto done = Done(waiting);
  done.completed.front().completions[Single(waiting)->token] = false;
  REQUIRE_NOTHROW(done.Save());
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetScriptState(*f.actor, Json::object());
    QuestJournal::SetJobState(*f.actor, done);
  }));
  REQUIRE(f.Saved() == before);
  done.completed.front().completions[Single(waiting)->token] =
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
    QuestJournal::SetScriptState(*f.actor, Heap(*Single(state)));
    QuestJournal::SetJobState(*f.actor, state);
  });
  const auto before = f.Saved();
  SECTION("deadline")
  {
    ++Single(state)->timer->issuedUnixMs;
    ++Single(state)->timer->dueUnixMs;
  }
  SECTION("duration")
  {
    ++Single(state)->timer->durationMs;
    ++Single(state)->timer->dueUnixMs;
  }
  SECTION("wait kind")
  {
    Single(state)->awaitKind = "bool";
    Single(state)->timer.reset();
  }
  SECTION("automatic policy")
  {
    Single(state)->automatic = true;
  }
  SECTION("content provenance")
  {
    Single(state)->contentMode = "fixture";
  }
  REQUIRE_NOTHROW(state.Save());
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, 1, [&] {
    QuestJournal::SetScriptState(*f.actor, Heap(*Single(state)));
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
    Single(waiting)->events.clear();
    QuestJournal::AtomicJob(*f.actor, 1, [&] {
      QuestJournal::SetScriptState(*f.actor, Heap(*Single(waiting)));
      QuestJournal::SetJobState(*f.actor, waiting);
    });
  }
  SECTION("child stage")
  {
    waiting = Nested();
    Single(waiting)->events.clear();
    QuestJournal::AtomicJob(*f.actor, 1, [&] {
      QuestJournal::SetScriptState(*f.actor, NestedHeap(*Single(waiting)));
      QuestJournal::SetJobState(*f.actor, waiting);
    });
    FinishChild(*Single(waiting));
    QuestJournal::AtomicJob(*f.actor, 1, [&] {
      QuestJournal::SetScriptState(*f.actor, NestedHeap(*Single(waiting)));
      QuestJournal::SetJobState(*f.actor, waiting);
    });
  }
  const auto before = f.Saved();
  auto candidate = waiting;
  Single(candidate)->children.clear();
  Single(candidate)->child = 0;
  candidate = Done(candidate);
  candidate.completed.front().completions[Single(waiting)->token] =
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
    Single(state)->contentMode = mode;
    REQUIRE(QuestJobState::Read(state.Save()).Save() == state.Save());
  }
  Single(state)->automatic = true;
  REQUIRE_THROWS(state.Save());
  Single(state)->contentMode = "fixture";
  REQUIRE_NOTHROW(state.Save());
  auto missing = state.Save();
  missing["live"][0].erase("contentMode");
  REQUIRE_THROWS(QuestJobState::Read(missing));
  for (const auto* invalid : { "", "mod", "OWNED" }) {
    Single(state)->contentMode = invalid;
    REQUIRE_THROWS(state.Save());
  }
}

TEST_CASE("Saved event dispatch keeps the completed quest body active",
          "[Quest][QuestJob]")
{
  for (const auto& state :
       { BodyDone(), Dispatching(), Dispatching(true), SelectedHandler(),
         SelectedHandler(true), SelectedHandler(true, true) }) {
    const auto& job = *Single(state);
    REQUIRE(job.cursor == "done");
    REQUIRE_FALSE(job.IsComplete());
    REQUIRE(QuestJobState::Read(state.Save()).Save() == state.Save());
    REQUIRE_NOTHROW(state.ValidateHeap(DispatchHeap(job)));
  }
  const auto state = SelectedHandler();
  REQUIRE(&Single(state)->Current() == &*Single(state)->handler);
  REQUIRE(Single(state)->Scope(*Single(state)->handler) ==
          "quest-job:1:1:handler:1");
  REQUIRE(Single(state)->NextToken() == "quest-job:1:1:1");
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
      Single(migrated) ? *Single(migrated) : migrated.completed.back();
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
    [](Json& v) { v["live"][0]["dispatch"]["recipients"] = Json::array(); },
    [](Json& v) {
      v["live"][0]["dispatch"]["recipients"][0] = "15:Skyrim.esm";
    },
    [](Json& v) {
      v["live"][0]["dispatch"]["recipients"].push_back("15:Skyrim.esm");
    },
    [](Json& v) {
      v["live"][0]["dispatch"]["recipients"].push_back("14:Skyrim.esm");
    },
    [](Json& v) { v["live"][0]["dispatch"]["recipient"] = 1; },
    [](Json& v) { v["live"][0]["dispatch"]["attachment"] = 1; },
    [](Json& v) { v["live"][0]["dispatch"]["captured"] = false; },
    [](Json& v) { v["live"][0]["dispatch"]["reference"] = "14:Skyrim.esm"; },
    [](Json& v) { v["live"][0]["dispatch"]["reference"] = ""; },
    [](Json& v) { v["live"][0]["dispatch"]["templates"][0] = "3000"; },
    [](Json& v) {
      v["live"][0]["dispatch"]["roots"].push_back("childscript");
    },
    [](Json& v) { v["live"][0]["dispatch"]["unknown"] = true; },
    [](Json& v) { v["live"][0]["eventHead"] = 1; },
    [](Json& v) { v["live"][0]["nextHandler"] = 1; },
    [](Json& v) { v["live"][0]["nextHandler"] = 4098; },
    [](Json& v) { v["live"][0]["handler"]["node"] = 1; },
    [](Json& v) { v["live"][0]["handler"]["quest"] = "2aa57:Skyrim.esm"; },
    [](Json& v) { v["live"][0]["handler"]["handlerIdentity"] = nullptr; },
    [](Json& v) {
      v["live"][0]["handler"]["handlerIdentity"]["rootScript"] = "Other";
    },
    [](Json& v) {
      v["live"][0]["handler"]["handlerIdentity"]["sequence"] = 2;
    },
    [](Json& v) {
      v["live"][0]["handler"]["handlerIdentity"]["receiver"] = "3000";
    },
    [](Json& v) {
      v["live"][0]["handler"]["handlerIdentity"]["function"] = "OnItemRemoved";
    },
    [](Json& v) { v["live"][0]["handler"]["script"] = "Other"; },
    [](Json& v) {
      v["live"][0]["handlerIdentity"] =
        v["live"][0]["handler"]["handlerIdentity"];
    }
  };
  for (size_t index = 0; index < corrupt.size(); ++index) {
    INFO("Dispatch corruption " << index);
    auto value = original;
    corrupt[index](value);
    REQUIRE_THROWS(QuestJobState::Read(value));
  }
  auto alias = SelectedHandler(true, true).Save();
  alias["live"][0]["dispatch"]["reference"] = "3000";
  REQUIRE_THROWS(QuestJobState::Read(alias));
  alias = SelectedHandler(true, true).Save();
  alias["live"][0]["dispatch"]["recipients"][1]["id"] = 2147483648ULL;
  REQUIRE_THROWS(QuestJobState::Read(alias));
}

TEST_CASE(
  "Handler continuations pin inherited implementation state and receiver",
  "[Quest][QuestJob]")
{
  const auto state = SelectedHandler(true, true);
  const auto& job = *Single(state);
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
  auto& job = *Single(state);
  job.handler->child = 1;
  job.handler->awaitKind = "stage";
  job.handler->awaitKey = "quest-stage";
  auto child = Single(Nested())->children.front();
  job.children.push_back(child);
  REQUIRE_NOTHROW(state.Save());
  REQUIRE_NOTHROW(state.ValidateHeap(DispatchHeap(job)));
  REQUIRE(&job.Current() == &job.children.back());
  REQUIRE(job.Scope(*job.handler) == "quest-job:1:1:handler:1");
  REQUIRE(job.Scope(job.Current()) == "quest-job:1:1:node:1");
  auto invalid = state;
  Single(invalid)->children.back().handlerIdentity =
    job.handler->handlerIdentity;
  REQUIRE_THROWS(invalid.Save());
  invalid = state;
  Single(invalid)->handler.reset();
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
      QuestJournal::SetScriptState(*f.actor,
                                   Single(state) ? DispatchHeap(*Single(state))
                                                 : Json::object());
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
    auto& job = *Single(state);
    job.completions[job.handler->token] = true;
    auto complete = *Single(SelectedHandler())->handler;
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
    ++Single(state)->dispatch->attachment;
    HostStep(*Single(state), 1);
    REQUIRE_NOTHROW(publish(state));
  }
  auto& job = *Single(state);
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
  state.live.clear();
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
      QuestJournal::SetScriptState(*f.actor, DispatchHeap(*Single(state)));
    });
  };
  for (const auto& state : { BodyDone(), Dispatching(), Dispatching(true),
                             SelectedHandler(), SelectedHandler(true) })
    publish(state);
  const auto before = f.Saved();
  auto state = SelectedHandler(true);
  auto& job = *Single(state);
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
    --Single(state)->instructions;
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
  Single(active)->dispatch->roots.resize(QuestJobState::MaxAttachments + 1,
                                         "Root");
  REQUIRE_THROWS(active.Save());
  active = SelectedHandler();
  Single(active)->segment = QuestJobState::MaxSegments + 1;
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
    REQUIRE(migrated.Save()["schemaVersion"] == 7);
    if (Single(migrated)) {
      REQUIRE_FALSE(Single(migrated)->sequenced);
      REQUIRE(Single(migrated)->Scope() == Single(original)->Scope());
      REQUIRE(Single(migrated)->NextToken() == Single(original)->NextToken());
      REQUIRE_NOTHROW(migrated.ValidateHeap(DispatchHeap(*Single(migrated))));
    }
    for (const auto& job : migrated.completed)
      REQUIRE_FALSE(job.sequenced);
  }
  auto legacy = Legacy(Waiting(), 4);
  legacy["active"]["command"] = "quest-request:1:1";
  REQUIRE_FALSE(Single(QuestJobState::Read(legacy))->sequenced);
  legacy["active"]["sequenced"] = false;
  REQUIRE_THROWS(QuestJobState::Read(legacy));
}

TEST_CASE("Sequenced commands are canonical while legacy command names stay "
          "independent",
          "[Quest][QuestJob]")
{
  auto state = Waiting();
  Single(state)->sequenced = true;
  Single(state)->command = "quest-request:1:1";
  REQUIRE_NOTHROW(state.Save());
  REQUIRE_NOTHROW(state.ValidateHeap(Heap(*Single(state))));
  for (const auto* command :
       { "", "quest-request:01:1", "quest-request:1:01", "quest-request:1:2",
         "quest-request:2:1", "quest-request:+1:1", "Quest-request:1:1",
         "command-one" }) {
    auto invalid = state;
    Single(invalid)->command = command;
    REQUIRE_THROWS(invalid.Save());
  }
  auto value = state.Save();
  value["live"][0]["sequenced"] = 1;
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
      v["live"][0]["id"] = 2;
      v["live"][0]["command"] = "quest-request:1:2";
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

TEST_CASE("Retirement preserves live work and refuses malformed typed state",
          "[Quest][QuestJob]")
{
  auto active = History(1, QuestJobState::MaxRecentCompleted - 1);
  AddPending(active, true);
  const auto before = active.Save();
  REQUIRE(active.RetireForAdmission() == std::vector<uint64_t>{ 2 });
  REQUIRE(active.Save()["live"] == before["live"]);
  REQUIRE(active.retiredThrough == 2);
  REQUIRE(active.completed.size() + 1 == before["completed"].size());
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
  REQUIRE_THROWS(state.ValidateHeap(Heap(*Single(Waiting()))));
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
    Single(candidate)->sequenced = false;
  }
  // These candidates are individually well-formed: the Journal must reject
  // their relationship to the already-published history.
  REQUIRE_NOTHROW(candidate.Save());
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, Single(candidate)->id, [&] {
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
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, Single(candidate)->id, [&] {
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
    const auto id = Single(state)->id;
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
  // A completion may exceed the byte window even while another root is live.
  // Exact admission-time retirement is enforced by Journal transitions.
  REQUIRE(QuestJobState::Read(original).Save() == original);
  auto admission = state;
  AddPending(admission, true);
  REQUIRE_NOTHROW(admission.Save());
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
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, Single(excessive)->id, [&] {
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

TEST_CASE(
  "Schema five migration preserves numbered identities and retired history",
  "[Quest][QuestJob]")
{
  auto history = History(2, QuestJobState::MaxRecentCompleted);
  history.RetireForAdmission();
  AddPending(history, true);
  for (const auto& original :
       { history, Waiting(), Nested(true), SelectedHandler(true) }) {
    const auto legacy = Legacy(original, 5);
    const auto migrated = QuestJobState::Read(legacy);
    REQUIRE(migrated.Save() == MigratedLegacy(original, 5).Save());
    REQUIRE(migrated.retiredRanges.empty());
    REQUIRE(migrated.live.size() == 1);
    REQUIRE(Single(migrated)->environmentHash.empty());
    REQUIRE(Single(migrated)->Scope() == Single(original)->Scope());
    REQUIRE_NOTHROW(migrated.ValidateHeap(AllLiveHeap(migrated)));
  }
  auto legacy = Legacy(history, 5);
  legacy["active"]["environmentHash"] = "";
  REQUIRE_THROWS(QuestJobState::Read(legacy));
  auto numbered = Waiting();
  Single(numbered)->sequenced = true;
  Single(numbered)->command = "quest-request:1:1";
  auto migrated = QuestJobState::Read(Legacy(numbered, 5));
  auto newJob = Numbered(*Single(Begin()), migrated.nextJob++);
  migrated.live.emplace(newJob.id, newJob);
  REQUIRE_THROWS(
    migrated.Save()); // An unpinned migrated root must finish alone.
}

TEST_CASE("Independent live roots round trip and complete out of order",
          "[Quest][QuestJob]")
{
  QuestJobState state;
  for (uint64_t id = 1; id <= 3; ++id) {
    state.live.emplace(id, Numbered(*Single(Begin()), id));
    ++state.nextJob;
  }
  const auto original = state.Save();
  REQUIRE(original["live"].size() == 3);
  REQUIRE(QuestJobState::Read(original).Save() == original);
  REQUIRE(state.HasLive());
  const auto first = *state.FindLive(1);
  CompleteLive(state, 3);
  CompleteLive(state, 2);
  REQUIRE(state.FindLive(1));
  REQUIRE(*state.FindLive(1) == first);
  REQUIRE_FALSE(state.FindLive(2));
  REQUIRE(state.FindCompleted(2)->id == 2);
  REQUIRE(state.completed[0].id == 2);
  REQUIRE(state.completed[1].id == 3);
  REQUIRE_NOTHROW(state.Save());
  CompleteLive(state, 1);
  REQUIRE_FALSE(state.HasLive());
  REQUIRE(state.completed.front().id == 1);
  REQUIRE(QuestJobState::Read(state.Save()).Save() == state.Save());
}

TEST_CASE(
  "Concurrent roots require one pinned environment and bounded capacity",
  "[Quest][QuestJob]")
{
  QuestJobState state;
  for (uint64_t id = 1; id <= QuestJobState::MaxStageLive; ++id) {
    state.live.emplace(id, Numbered(*Single(Begin()), id));
    ++state.nextJob;
  }
  REQUIRE_NOTHROW(state.Save());
  const auto valid = state.Save();
  const std::vector<std::function<void(Json&)>> corrupt = {
    [](Json& v) { v["live"][0]["environmentHash"] = ""; },
    [](Json& v) { v["live"][1]["environmentHash"] = std::string(64, 'f'); },
    [](Json& v) { v["live"][1]["environmentHash"] = std::string(64, 'E'); },
    [](Json& v) { v["live"][1]["environmentHash"] = std::string(63, 'e'); },
    [](Json& v) { v["live"][1]["contentMode"] = "fixture"; },
    [](Json& v) { v["live"][1]["owner"] = "3001"; },
    [](Json& v) { std::swap(v["live"][0], v["live"][1]); },
    [](Json& v) { v["live"][1] = v["live"][0]; },
    [](Json& v) { v["live"] = Json::object(); },
    [](Json& v) { v["live"][0].erase("environmentHash"); }
  };
  for (size_t index = 0; index < corrupt.size(); ++index) {
    INFO("Concurrent root corruption " << index);
    auto saved = valid;
    corrupt[index](saved);
    REQUIRE_THROWS(QuestJobState::Read(saved));
  }
  auto tooMany = state;
  auto job = Numbered(*Single(Begin()), tooMany.nextJob++);
  tooMany.live.emplace(job.id, job);
  REQUIRE_THROWS(tooMany.Save());
  auto wrongKey = state;
  wrongKey.live.at(1).id = 20;
  REQUIRE_THROWS(wrongKey.Save());
  auto reserved = state;
  for (size_t index = 0;
       index < QuestJobState::MaxRecentCompleted - QuestJobState::MaxStageLive;
       ++index)
    reserved.completed.push_back(CompletedRecord(reserved.nextJob++, true));
  REQUIRE_NOTHROW(reserved.Save());
  CompleteLive(reserved, 2); // Completion consumes its already-reserved slot.
  REQUIRE_NOTHROW(reserved.Save());
  reserved.completed.push_back(CompletedRecord(reserved.nextJob++, true));
  REQUIRE_THROWS(reserved.Save());
}

TEST_CASE("Sparse retirement lets a low numbered root outlive hundreds of "
          "later results",
          "[Quest][QuestJob]")
{
  QuestJobState state;
  state.live.emplace(1, Numbered(*Single(TimerWaiting()), 1));
  state.nextJob = 2;
  const auto sleeping = *state.FindLive(1);
  for (uint64_t id = 2; id <= 340; ++id) {
    state.RetireForAdmission();
    REQUIRE(state.nextJob == id);
    state.live.emplace(id, Numbered(*Single(Begin()), state.nextJob++));
    CompleteLive(state, id);
    REQUIRE(state.completed.size() < QuestJobState::MaxRecentCompleted);
  }
  REQUIRE(*state.FindLive(1) == sleeping);
  REQUIRE(state.retiredThrough == 0);
  REQUIRE(state.retiredRanges ==
          std::vector<QuestJobState::RetiredRange>{ { 2, 309 } });
  REQUIRE_FALSE(state.IsRetired(0));
  REQUIRE_FALSE(state.IsRetired(1));
  REQUIRE(state.IsRetired(2));
  REQUIRE(state.IsRetired(309));
  REQUIRE_FALSE(state.IsRetired(310));
  REQUIRE_FALSE(state.IsRetired(state.nextJob));
  REQUIRE_NOTHROW(state.ValidateHeap(AllLiveHeap(state)));
  REQUIRE(QuestJobState::Read(state.Save()).Save() == state.Save());
  CompleteLive(state, 1);
  REQUIRE(state.completed.front().id == 1);
  const auto delta = state.RetireForAdmission();
  REQUIRE(delta == std::vector<uint64_t>{ 1 });
  REQUIRE(state.retiredThrough == 309);
  REQUIRE(state.retiredRanges.empty());
  REQUIRE(state.IsRetired(1));
  REQUIRE(state.IsRetired(309));
  REQUIRE_NOTHROW(state.Save());
}

TEST_CASE("Sparse retirement normalizes legacy bridges without retiring them",
          "[Quest][QuestJob]")
{
  QuestJobState state;
  state.nextJob = 7;
  state.live.emplace(3, Numbered(*Single(Begin()), 3, false));
  state.retiredRanges = { { 1, 2 }, { 4, 6 } };
  REQUIRE_NOTHROW(state.Save());
  REQUIRE(state.RetireForAdmission().empty());
  REQUIRE(state.retiredThrough == 2);
  REQUIRE(state.retiredRanges ==
          std::vector<QuestJobState::RetiredRange>{ { 4, 6 } });
  REQUIRE_FALSE(state.IsRetired(3));
  CompleteLive(state, 3);
  const auto legacy = *state.FindCompleted(3);
  REQUIRE_NOTHROW(
    state.Save()); // A non-maximal prefix is valid between admissions.
  REQUIRE(state.RetireForAdmission().empty());
  REQUIRE(state.retiredThrough == 6);
  REQUIRE(state.retiredRanges.empty());
  REQUIRE(*state.FindCompleted(3) == legacy);
  REQUIRE_FALSE(state.IsRetired(3));
  REQUIRE(state.IsRetired(4));
  REQUIRE_NOTHROW(state.Save());
}

TEST_CASE(
  "Sparse ranges reject gaps overlaps noncanonical bounds and hidden records",
  "[Quest][QuestJob]")
{
  QuestJobState state;
  state.nextJob = 11;
  state.live.emplace(1, Numbered(*Single(Begin()), 1));
  state.live.emplace(10, Numbered(*Single(Begin()), 10));
  state.completed.push_back(CompletedRecord(6, false));
  state.retiredRanges = { { 2, 5 }, { 7, 9 } };
  const auto valid = state.Save();
  const std::vector<std::function<void(Json&)>> corrupt = {
    [](Json& v) { v["retiredThrough"] = 1; },
    [](Json& v) { v["retiredThrough"] = 6; },
    [](Json& v) { v["retiredRanges"][0]["first"] = 0; },
    [](Json& v) { v["retiredRanges"][0]["last"] = 1; },
    [](Json& v) { v["retiredRanges"][0]["first"] = 3; },
    [](Json& v) { v["retiredRanges"][0]["last"] = 6; },
    [](Json& v) { v["retiredRanges"][0]["last"] = 7; },
    [](Json& v) { v["retiredRanges"][1]["last"] = 10; },
    [](Json& v) { v["retiredRanges"][1]["last"] = 11; },
    [](Json& v) { v["retiredRanges"][0]["first"] = 2.0; },
    [](Json& v) { v["retiredRanges"][0]["extra"] = 0; },
    [](Json& v) { v["retiredRanges"][0].erase("last"); },
    [](Json& v) { std::swap(v["retiredRanges"][0], v["retiredRanges"][1]); },
    [](Json& v) {
      v["retiredRanges"] = Json::array({ { { "first", 2 }, { "last", 3 } },
                                         { { "first", 4 }, { "last", 5 } },
                                         { { "first", 7 }, { "last", 9 } } });
    },
    [](Json& v) { v["completed"].clear(); },
    [](Json& v) { v["retiredRanges"] = Json::array(); },
    [](Json& v) { v["retiredRanges"] = Json::object(); }
  };
  for (size_t index = 0; index < corrupt.size(); ++index) {
    INFO("Sparse corruption " << index);
    auto saved = valid;
    corrupt[index](saved);
    REQUIRE_THROWS(QuestJobState::Read(saved));
  }
  constexpr uint64_t max = 9007199254740991ULL;
  state.live.erase(10);
  state.completed.clear();
  state.nextJob = max;
  state.retiredRanges = { { 2, max - 2 } };
  state.completed.push_back(CompletedRecord(max - 1, true));
  REQUIRE_NOTHROW(
    state.Save()); // Validation work does not scale with the range span.
  REQUIRE(state.IsRetired(max - 2));
  REQUIRE_FALSE(state.IsRetired(max - 1));
  auto oversized = state;
  oversized.retiredRanges.resize(QuestJobState::MaxRetiredRanges + 1);
  REQUIRE_THROWS(oversized.Save());
}

TEST_CASE(
  "Shared heap validates every live root against its own stack and budget",
  "[Quest][QuestJob]")
{
  QuestJobState state;
  state.nextJob = 4;
  state.live.emplace(1, Numbered(*Single(TimerWaiting()), 1));
  state.live.emplace(2, Numbered(*Single(Nested(true)), 2));
  state.live.emplace(3, Numbered(*Single(SelectedHandler(true, true)), 3));
  const auto valid = AllLiveHeap(state);
  REQUIRE(valid["continuations"].size() == 4);
  REQUIRE_NOTHROW(state.ValidateHeap(valid));
  const auto timerToken = state.live.at(1).token;
  const auto childToken = state.live.at(2).Current().token;
  const auto handlerToken = state.live.at(3).Current().token;
  const std::vector<std::function<void(Json&)>> corrupt = {
    [&](Json& v) { v["continuations"].erase(timerToken); },
    [&](Json& v) {
      v["continuations"][timerToken]["scope"] = state.live.at(2).Scope();
    },
    [&](Json& v) {
      v["continuations"][childToken]["executedInstructions"] = 3;
    },
    [&](Json& v) {
      v["continuations"][timerToken]["executedInstructions"] = 7;
    },
    [&](Json& v) { v["continuations"][timerToken]["resultType"] = "Bool"; },
    [&](Json& v) {
      v["continuations"][handlerToken]["receiver"]["value"]["id"] = 0;
    },
    [&](Json& v) {
      v["continuations"][handlerToken]["rootScript"] = "AnotherRoot";
    }
  };
  for (size_t index = 0; index < corrupt.size(); ++index) {
    INFO("Multi-root heap corruption " << index);
    auto heap = valid;
    corrupt[index](heap);
    REQUIRE_THROWS(state.ValidateHeap(heap));
  }
  CompleteLive(state, 1);
  REQUIRE_THROWS(state.ValidateHeap(valid));
  auto completedHeap = valid;
  completedHeap["continuations"].erase(timerToken);
  REQUIRE_NOTHROW(state.ValidateHeap(completedHeap));
}

TEST_CASE("Schema five retains its single-live ordering and byte restrictions",
          "[Quest][QuestJob]")
{
  auto oversized = ByteHeavyHistory();
  AddPending(oversized, true);
  REQUIRE_NOTHROW(oversized.Save());
  REQUIRE_THROWS(QuestJobState::Read(Legacy(oversized, 5)));
  auto outOfOrder = Waiting();
  outOfOrder.completed.push_back(CompletedRecord(outOfOrder.nextJob++, true));
  REQUIRE_NOTHROW(outOfOrder.Save());
  REQUIRE_THROWS(QuestJobState::Read(Legacy(outOfOrder, 5)));
  // The old limit counts the old JSON shape, not the newly introduced field.
  auto boundary = History(0, 1);
  auto& record = boundary.completed.front();
  record.events.assign(
    QuestJobState::MaxEvents,
    QuestJobState::Event{ "14:Skyrim.esm", "f:Skyrim.esm", "", 1, true });
  record.eventHead = static_cast<uint32_t>(record.events.size());
  AddPending(boundary, true);
  auto legacy = Legacy(boundary, 5);
  auto& events = legacy["completed"][0]["events"];
  size_t remaining =
    QuestJobState::MaxRecentBytes - legacy["completed"][0].dump().size();
  for (auto& event : events) {
    if (!remaining)
      break;
    const auto length = std::min<size_t>(remaining, 240);
    event["item"] = "f:" + std::string(length, 'x') + "Skyrim.esm";
    remaining -= length;
  }
  REQUIRE(remaining == 0);
  REQUIRE(legacy["completed"][0].dump().size() ==
          QuestJobState::MaxRecentBytes);
  const auto migrated = QuestJobState::Read(legacy);
  REQUIRE(migrated.RetainedSequencedBytes() > QuestJobState::MaxRecentBytes);
  REQUIRE_NOTHROW(migrated.Save());
}

TEST_CASE("Byte retirement preserves several live roots and returns only "
          "removed receipts",
          "[Quest][QuestJob]")
{
  QuestJobState state;
  state.nextJob = 5;
  state.live.emplace(1, Numbered(*Single(TimerWaiting()), 1));
  state.live.emplace(4, Numbered(*Single(Begin()), 4));
  state.completed = { LargeCompletion(2, true), LargeCompletion(3, true) };
  REQUIRE(state.RetainedSequencedBytes() > QuestJobState::MaxRecentBytes);
  REQUIRE_NOTHROW(state.Save());
  const auto sleeping = state.live;
  const auto newest = state.completed.back();
  REQUIRE(state.RetireForAdmission() == std::vector<uint64_t>{ 2 });
  REQUIRE(state.live == sleeping);
  REQUIRE(state.completed == std::vector<QuestJobState::Job>{ newest });
  REQUIRE(state.retiredThrough == 0);
  REQUIRE(state.retiredRanges ==
          std::vector<QuestJobState::RetiredRange>{ { 2, 2 } });
  REQUIRE(state.RetainedSequencedBytes() <= QuestJobState::MaxRecentBytes);
  REQUIRE_NOTHROW(state.ValidateHeap(AllLiveHeap(state)));
}

TEST_CASE("Schema six histories migrate as stages without changing stacks or "
          "retirement",
          "[Quest][QuestJob]")
{
  auto multiple = Waiting();
  multiple.live.at(1).environmentHash = std::string(64, 'e');
  multiple.live.emplace(2, Numbered(*Single(Nested(true)), 2));
  multiple.nextJob = 5;
  multiple.retiredRanges = { { 3, 4 } };
  for (const auto& original : { Begin(), Waiting(), Done(Waiting()),
                                SelectedHandler(true), multiple }) {
    const auto migrated = QuestJobState::Read(Legacy(original, 6));
    REQUIRE(migrated.Save() == original.Save());
    REQUIRE(migrated.Save()["schemaVersion"] == 7);
    for (const auto& [id, job] : migrated.live) {
      REQUIRE(job.kind == QuestJobState::Kind::Stage);
      REQUIRE_FALSE(job.interaction);
      REQUIRE(job.interactionDone);
    }
    REQUIRE_NOTHROW(migrated.ValidateHeap(AllLiveHeap(migrated)));
  }
  auto old = Legacy(Waiting(), 6);
  old["live"][0]["kind"] = "stage";
  REQUIRE_THROWS(QuestJobState::Read(old));
}

TEST_CASE("Interaction roots carry real events and no synthetic quest stage",
          "[Quest][QuestJob]")
{
  for (const auto* name : { "OnActivate", "OnTriggerEnter", "OnTriggerLeave",
                            "OnTrigger", "SkympOnActivateClose" }) {
    auto state = InteractionBegin();
    auto& job = state.live.at(1);
    job.interaction->name = name;
    REQUIRE(job.IsPristine());
    REQUIRE_FALSE(job.IsComplete());
    REQUIRE(job.quest.empty());
    REQUIRE(job.contentHash.empty());
    REQUIRE(job.stage == 0);
    REQUIRE(job.cursor == "done");
    REQUIRE_FALSE(job.accepted);
    const auto saved = state.Save();
    REQUIRE(saved["live"][0]["kind"] == "interaction");
    REQUIRE(QuestJobState::Read(saved).Save() == saved);
    REQUIRE_NOTHROW(state.ValidateHeap(Json::object()));
    job.interactionDone = true;
    HostStep(job, 1);
    REQUIRE(job.IsComplete());
    REQUIRE_FALSE(job.IsPristine());
    CompleteLive(state, 1);
    REQUIRE(QuestJobState::Read(state.Save()).Save() == state.Save());
  }
  REQUIRE(Single(Begin())->IsPristine());
  REQUIRE_FALSE(Single(Waiting())->IsPristine());
  REQUIRE_FALSE(Single(BodyDone())->IsPristine());
}

TEST_CASE("Interaction payload codec rejects ambiguous identities provenance "
          "and unsupported names",
          "[Quest][QuestJob]")
{
  const auto original = InteractionBegin().Save();
  const std::vector<std::function<void(Json&)>> corrupt = {
    [](Json& v) { v["live"][0]["kind"] = "event"; },
    [](Json& v) { v["live"][0]["kind"] = 1; },
    [](Json& v) { v["live"][0]["interaction"] = nullptr; },
    [](Json& v) { v["live"][0].erase("interactionDone"); },
    [](Json& v) { v["live"][0]["interactionDone"] = 0; },
    [](Json& v) { v["live"][0]["environmentHash"] = ""; },
    [](Json& v) { v["live"][0]["contentMode"] = "legacy"; },
    [](Json& v) { v["live"][0]["interaction"]["name"] = "onactivate"; },
    [](Json& v) { v["live"][0]["interaction"]["name"] = "OnItemAdded"; },
    [](Json& v) { v["live"][0]["interaction"]["name"] = ""; },
    [](Json& v) { v["live"][0]["interaction"]["target"] = "3001"; },
    [](Json& v) { v["live"][0]["interaction"]["target"] = "010:Skyrim.esm"; },
    [](Json& v) { v["live"][0]["interaction"]["source"] = ""; },
    [](Json& v) { v["live"][0]["interaction"]["source"] = "3000"; },
    [](Json& v) { v["live"][0]["interaction"]["source"] = "0:Skyrim.esm"; },
    [](Json& v) {
      v["live"][0]["interaction"]["targetReference"] = "10:Skyrim.esm";
    },
    [](Json& v) { v["live"][0]["interaction"]["sourceReference"] = "03000"; },
    [](Json& v) {
      v["live"][0]["interaction"]["targetTemplates"] =
        Json::array({ "7:Skyrim.esm", "7:Skyrim.esm" });
    },
    [](Json& v) {
      v["live"][0]["interaction"]["sourceTemplates"] = Json::array({ "3000" });
    },
    [](Json& v) {
      v["live"][0]["interaction"]["targetTemplates"] = Json::object();
    },
    [](Json& v) {
      v["live"][0]["interaction"]["sourceTemplates"] =
        std::vector<std::string>(129, "7:Skyrim.esm");
    },
    [](Json& v) { v["live"][0]["interaction"]["extra"] = true; },
    [](Json& v) { v["live"][0]["interaction"].erase("sourceReference"); }
  };
  for (size_t index = 0; index < corrupt.size(); ++index) {
    INFO("Interaction payload corruption " << index);
    auto value = original;
    corrupt[index](value);
    REQUIRE_THROWS(QuestJobState::Read(value));
  }
  auto stage = Begin();
  stage.live.at(1).interaction = InteractionBegin().live.at(1).interaction;
  REQUIRE_THROWS(stage.Save());
  stage = Begin();
  stage.live.at(1).interactionDone = false;
  REQUIRE_THROWS(stage.Save());
  auto invalidKind = InteractionBegin();
  invalidKind.live.at(1).kind = static_cast<QuestJobState::Kind>(99);
  REQUIRE_THROWS(invalidKind.Save());
  auto tooMany = InteractionBegin();
  tooMany.live.at(1).interaction->targetTemplates.resize(129);
  REQUIRE_THROWS(tooMany.Save());
}

TEST_CASE("Interaction inherited stage is inert and its initial host cursor "
          "is pristine",
          "[Quest][QuestJob]")
{
  const auto original = InteractionBegin().Save();
  const std::vector<std::function<void(Json&)>> corrupt = {
    [](Json& v) { v["live"][0]["quest"] = "4e50d:Skyrim.esm"; },
    [](Json& v) { v["live"][0]["contentHash"] = std::string(64, 'a'); },
    [](Json& v) { v["live"][0]["stage"] = 10; },
    [](Json& v) { v["live"][0]["node"] = 1; },
    [](Json& v) { v["live"][0]["child"] = 1; },
    [](Json& v) { v["live"][0]["cursor"] = "begin"; },
    [](Json& v) { v["live"][0]["accepted"] = true; },
    [](Json& v) { v["live"][0]["log"] = 1; },
    [](Json& v) { v["live"][0]["script"] = "QF_Test"; },
    [](Json& v) { v["live"][0]["function"] = "Fragment_0"; },
    [](Json& v) { v["live"][0]["token"] = "quest-job:1:1:1"; },
    [](Json& v) { v["live"][0]["awaitKind"] = "bool"; },
    [](Json& v) { v["live"][0]["awaitKey"] = "fixture"; },
    [](Json& v) { v["live"][0]["failure"] = "failed"; },
    [](Json& v) { v["live"][0]["instructions"] = 1; },
    [](Json& v) { v["live"][0]["steps"] = 1; },
    [](Json& v) { v["live"][0]["nextHandler"] = 2; },
    [](Json& v) { v["live"][0]["interactionDone"] = true; },
    [](Json& v) {
      v["live"][0]["events"] = Waiting().Save()["live"][0]["events"];
    }
  };
  for (size_t index = 0; index < corrupt.size(); ++index) {
    INFO("Interaction root corruption " << index);
    auto value = original;
    corrupt[index](value);
    REQUIRE_THROWS(QuestJobState::Read(value));
  }
  auto pending = InteractionBegin();
  pending.live.at(1).dispatch = QuestJobState::Dispatch{};
  pending.live.at(1).dispatch->recipients = {
    pending.live.at(1).interaction->target
  };
  REQUIRE_FALSE(pending.live.at(1).IsPristine());
  REQUIRE_THROWS(pending.Save());
  HostStep(pending.live.at(1), 1);
  REQUIRE_NOTHROW(pending.Save());
  REQUIRE_FALSE(pending.live.at(1).IsComplete());
}

TEST_CASE("Interaction handlers own exact empty-anchor continuations for "
          "references and aliases",
          "[Quest][QuestJob]")
{
  for (bool alias : { false, true }) {
    auto state = InteractionHandler(true, alias);
    auto& job = state.live.at(1);
    REQUIRE(job.handler->quest.empty());
    REQUIRE(job.handler->contentHash.empty());
    REQUIRE(job.handler->stage == 0);
    REQUIRE(job.handler->handlerIdentity->function == "OnActivate");
    REQUIRE(&job.Current() == &*job.handler);
    REQUIRE(job.Scope(*job.handler) == "quest-job:1:1:handler:1");
    REQUIRE(QuestJobState::Read(state.Save()).Save() == state.Save());
    const auto heap = AllLiveHeap(state);
    REQUIRE_NOTHROW(state.ValidateHeap(heap));
    auto wrong = heap;
    wrong["continuations"][job.handler->token]["function"] = "OnItemAdded";
    REQUIRE_THROWS(state.ValidateHeap(wrong));
    wrong = heap;
    wrong["continuations"][job.handler->token]["resultType"] = "Bool";
    REQUIRE_THROWS(state.ValidateHeap(wrong));
    auto changed = state;
    changed.live.at(1).handler->quest = "4e50d:Skyrim.esm";
    REQUIRE_THROWS(changed.Save());
    changed = state;
    changed.live.at(1).handler->contentHash = std::string(64, 'a');
    REQUIRE_THROWS(changed.Save());
    changed = state;
    changed.live.at(1).handler->stage = 1;
    REQUIRE_THROWS(changed.Save());
  }
  auto ordinary = SelectedHandler();
  ordinary.live.at(1).handler->quest.clear();
  ordinary.live.at(1).handler->contentHash.clear();
  ordinary.live.at(1).handler->stage = 0;
  REQUIRE_THROWS(ordinary.Save());
  auto captured = InteractionHandler();
  auto original = captured.Save();
  for (const auto* field : { "reference", "templates" }) {
    auto corrupt = original;
    corrupt["live"][0]["dispatch"][field] = std::string(field) == "reference"
      ? Json("3002")
      : Json::array({ "8:Skyrim.esm" });
    REQUIRE_THROWS(QuestJobState::Read(corrupt));
  }
}

TEST_CASE("Interaction handler native children remain real quest stages",
          "[Quest][QuestJob]")
{
  auto state = InteractionHandler(true);
  auto& job = state.live.at(1);
  job.handler->awaitKind = "stage";
  job.handler->awaitKey = "quest-stage";
  job.handler->timer.reset();
  job.handler->child = 1;
  job.children.push_back(Single(Nested(true))->children.front());
  job.nextOperation = 3;
  job.instructions = 7;
  HostStep(job);
  REQUIRE(QuestJobState::Read(state.Save()).Save() == state.Save());
  REQUIRE(job.Current().quest == "2aa57:Skyrim.esm");
  REQUIRE_NOTHROW(state.ValidateHeap(AllLiveHeap(state)));
  auto corrupt = state;
  corrupt.live.at(1).children.back().quest.clear();
  corrupt.live.at(1).children.back().contentHash.clear();
  corrupt.live.at(1).children.back().stage = 0;
  REQUIRE_THROWS(corrupt.Save());
  FinishChild(job);
  REQUIRE_NOTHROW(state.Save());
  REQUIRE_NOTHROW(state.ValidateHeap(AllLiveHeap(state)));
  REQUIRE_FALSE(job.IsComplete());
}

TEST_CASE("Interaction primary delivery precedes its generated inventory FIFO",
          "[Quest][QuestJob]")
{
  auto state = InteractionHandler(true);
  auto& job = state.live.at(1);
  job.events.push_back({ "14:Skyrim.esm", "f:Skyrim.esm", "", 1, true });
  REQUIRE_NOTHROW(state.Save());
  auto corrupt = state;
  corrupt.live.at(1).eventHead = 1;
  REQUIRE_THROWS(corrupt.Save());
  corrupt = state;
  corrupt.live.at(1).interactionDone = true;
  REQUIRE_THROWS(corrupt.Save());
  job.completions[job.handler->token] = QuestJobState::Completion::None();
  job.handler.reset();
  job.dispatch.reset();
  job.interactionDone = true;
  HostStep(job);
  REQUIRE_FALSE(job.IsComplete());
  REQUIRE_NOTHROW(state.Save());
  auto inventory = SelectedHandler();
  job.dispatch = inventory.live.at(1).dispatch;
  job.handler = inventory.live.at(1).handler;
  job.handler->quest.clear();
  job.handler->contentHash.clear();
  job.handler->stage = 0;
  job.handler->handlerIdentity->sequence = job.nextHandler++;
  HostStep(job, 2);
  REQUIRE(job.handler->handlerIdentity->function == "OnItemAdded");
  REQUIRE_NOTHROW(state.Save());
  job.handler.reset();
  job.dispatch.reset();
  job.eventHead = 1;
  HostStep(job);
  REQUIRE(job.IsComplete());
  CompleteLive(state, 1);
  REQUIRE(QuestJobState::Read(state.Save()).Save() == state.Save());
}

TEST_CASE(
  "Interaction roots reserve admission capacity beyond sixteen stage roots",
  "[Quest][QuestJob]")
{
  QuestJobState state;
  for (uint64_t id = 1; id <= QuestJobState::MaxLive; ++id) {
    const auto initial =
      id <= QuestJobState::MaxStageLive ? Begin() : InteractionBegin();
    state.live.emplace(id, Numbered(*Single(initial), state.nextJob++));
  }
  REQUIRE(state.live.size() == 20);
  REQUIRE_NOTHROW(state.Save());
  REQUIRE_NOTHROW(state.ValidateHeap(Json::object()));
  auto tooManyStages = state;
  tooManyStages.live.at(17) = Numbered(*Single(Begin()), 17);
  REQUIRE_THROWS(tooManyStages.Save());
  auto tooManyRoots = state;
  const auto extra =
    Numbered(*Single(InteractionBegin()), tooManyRoots.nextJob++);
  tooManyRoots.live.emplace(extra.id, extra);
  REQUIRE_THROWS(tooManyRoots.Save());
  for (auto& [id, job] : state.live)
    job = Numbered(*Single(InteractionBegin()), id);
  REQUIRE_NOTHROW(state.Save()); // The reserved slots do not impose an
                                 // interaction-only ceiling.
  auto old = Legacy(Begin(), 6);
  const auto oldJob = old["live"][0];
  for (uint64_t id = 2; id <= 17; ++id) {
    auto job = oldJob;
    job["id"] = id;
    job["command"] = "legacy-" + std::to_string(id);
    job["environmentHash"] = std::string(64, 'e');
    old["live"].push_back(job);
  }
  old["live"][0]["environmentHash"] = std::string(64, 'e');
  old["nextJob"] = 18;
  REQUIRE_THROWS(QuestJobState::Read(old));
}
