#include "Inventory.h"
#include "QuestJobState.h"
#include "QuestJournal.h"
#include "TestUtils.hpp"
#include "save_storages/ISaveStorage.h"
#include <algorithm>
#include <catch2/catch_all.hpp>

extern espm::Loader& GetEspmLoader();

namespace {
using Json = nlohmann::json;
constexpr uint32_t OwnerId = 0xff007001;

QuestJobState::Job WaitingJob(uint64_t id)
{
  QuestJobState::Job job;
  job.id = id;
  job.command = "quest-request:1:" + std::to_string(id);
  job.sequenced = true;
  job.actor = job.owner = "7001";
  job.quest = "4e50d:Skyrim.esm";
  job.contentHash = std::string(64, 'a');
  job.environmentHash = std::string(64, 'e');
  job.contentMode = "fixture";
  job.stage = static_cast<uint16_t>(id * 10);
  job.cursor = "waiting";
  job.accepted = true;
  job.segment = 1;
  job.instructions = 3;
  job.steps = 2;
  job.script = "QF_Shared";
  job.function = "Fragment_0";
  job.awaitKey = "external";
  job.awaitKind = "bool";
  job.token = job.NextToken();
  ++job.nextOperation;
  return job;
}

QuestJobState TwoJobs()
{
  QuestJobState jobs;
  jobs.nextJob = 3;
  jobs.live.emplace(1, WaitingJob(1));
  jobs.live.emplace(2, WaitingJob(2));
  return jobs;
}

Json Heap(const QuestJobState& jobs)
{
  Json heap = {
    { "schemaVersion", 6 },        { "scripts", Json::object() },
    { "arrays", Json::object() },  { "globals", Json::object() },
    { "aliases", Json::object() }, { "continuations", Json::object() }
  };
  heap["scripts"]["shared"] = {
    { "schemaVersion", 1 },
    { "values",
      { { "counter", { { "type", 3 }, { "value", 0 } } },
        { "array", { { "type", 13 }, { "array", "1" } } } } }
  };
  heap["arrays"]["1"] = {
    { "type", 13 },
    { "values", Json::array({ { { "type", 3 }, { "value", 1 } } }) }
  };
  for (const auto& [id, job] : jobs.live) {
    heap["continuations"][job.token] = {
      { "schemaVersion", 2 },
      { "scope", job.Scope() },
      { "receiver", { { "type", 0 }, { "value", job.quest } } },
      { "kind", "method" },
      { "rootScript", job.script },
      { "script", job.script },
      { "state", "" },
      { "function", job.function },
      { "contentHashes",
        Json::array(
          { { { "script", job.script }, { "hash", job.contentHash } } }) },
      { "resultType", "Bool" },
      { "instruction", 1 },
      { "executedInstructions", job.instructions },
      { "locals",
        Json::array({ { { "name", "sharedArray" },
                        { "declaredType", "Int[]" },
                        { "value", { { "type", 13 }, { "array", "1" } } } },
                      { { "name", "captured" },
                        { "declaredType", "Int" },
                        { "value", { { "type", 3 }, { "value", 7 } } } } }) },
      { "callers", Json::array() }
    };
  }
  return heap;
}

void Complete(QuestJobState& jobs, Json& heap, uint64_t id)
{
  auto job = jobs.live.at(id);
  heap["continuations"].erase(job.token);
  job.completions.emplace(job.token, true);
  job.cursor = "done";
  job.script.clear();
  job.function.clear();
  job.token.clear();
  job.awaitKey.clear();
  job.awaitKind.clear();
  ++job.segment;
  ++job.instructions;
  ++job.steps;
  jobs.live.erase(id);
  const auto position = std::lower_bound(
    jobs.completed.begin(), jobs.completed.end(), id,
    [](const auto& previous, uint64_t next) { return previous.id < next; });
  jobs.completed.insert(position, std::move(job));
}

struct Storage
  : Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>
{
  bool SupportsAtomicBatches() const noexcept override { return true; }
  bool SupportsUpsertResults() const noexcept override { return true; }
  void IterateSync(const IterateSyncCallback&) override {}
  void Iterate(const IterateCallback& callback,
               const std::optional<std::vector<FormDesc>>&) override
  {
    callback({});
  }
  void Upsert(std::vector<std::optional<MpChangeForm>>&&,
              const UpsertCallback& callback) override
  {
    callback();
  }
  void UpsertWithResult(std::vector<std::optional<MpChangeForm>>&&,
                        const UpsertResultCallback& callback) override
  {
    callback({});
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
    static const std::string name = "multi-job-journal-test";
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
    server.AttachSaveStorage(std::make_shared<Storage>());
    server.CreateActor(OwnerId, { 1, 2, 3 }, 0, 0x3c);
    actor = &server.worldState.GetFormAt<MpActor>(OwnerId);
    actor->SetPropertyValueDump(QuestJournal::kEnabled,
                                "[\"4e50d:Skyrim.esm\"]", false, false);
  }
  void Seed(const QuestJobState& jobs, const Json& heap)
  {
    jobs.ValidateHeap(heap);
    const Json ledger = { { "schemaVersion", 2 },
                          { "revision", 1 },
                          { "quests", Json::object() },
                          { "jobs", jobs.Save() },
                          { "vm", heap } };
    actor->SetPropertyValueDump(QuestJournal::kProperty, ledger.dump(), false,
                                false);
  }
  Json Saved() const
  {
    return Json::parse(
      actor->GetDynamicFields().GetValueDump(QuestJournal::kProperty));
  }
  void Commit(uint64_t id, const std::function<void()>& callback)
  {
    const auto receipt = QuestJournal::AtomicJob(*actor, id, callback);
    REQUIRE(receipt);
    for (size_t tick = 0;
         tick < 4 && receipt->GetState() == SaveReceipt::State::Pending;
         ++tick)
      server.worldState.Tick();
    REQUIRE(receipt->GetState() == SaveReceipt::State::Durable);
  }
};
}

TEST_CASE(
  "Independent Journal roots share heap updates and complete out of order",
  "[Quest][QuestJob][MultiJob]")
{
  Fixture f;
  auto jobs = TwoJobs();
  auto heap = Heap(jobs);
  f.Seed(jobs, heap);
  const auto initialGold = f.actor->GetInventory().GetItemCount(0xf);
  const auto first = jobs.live.at(1);
  const auto firstStack = heap["continuations"][first.token];
  Complete(jobs, heap, 2);
  heap["scripts"]["shared"]["values"]["counter"]["value"] = 42;
  heap["arrays"]["1"]["values"][0]["value"] = 9;
  f.Commit(2, [&] {
    QuestJournal::RequireJobExecution(*f.actor, 2);
    QuestJournal::SetScriptState(*f.actor, heap);
    QuestJournal::SetJobState(*f.actor, jobs);
    // Commit-time adapters retain this root's lease even after it became a
    // completed record while another independent root is still sleeping.
    QuestJournal::EditInventory(*f.actor, *f.actor,
                                [](Inventory& inv) { inv.AddItem(0xf, 1); });
  });
  REQUIRE(QuestJournal::JobState(*f.actor).live.at(1) == first);
  REQUIRE(QuestJournal::ScriptState(*f.actor)["continuations"][first.token] ==
          firstStack);
  REQUIRE(QuestJournal::ScriptState(
            *f.actor)["arrays"]["1"]["values"][0]["value"] == 9);
  REQUIRE(f.actor->GetInventory().GetItemCount(0xf) == initialGold + 1);
  Complete(jobs, heap, 1);
  f.Commit(1, [&] {
    QuestJournal::SetJobState(*f.actor, jobs);
    QuestJournal::SetScriptState(*f.actor, heap);
  });
  const auto saved = QuestJournal::JobState(*f.actor);
  REQUIRE_FALSE(saved.HasLive());
  REQUIRE(saved.completed.size() == 2);
  REQUIRE(saved.completed[0].id == 1);
  REQUIRE(saved.completed[1].id == 2);
  REQUIRE(QuestJournal::ScriptState(
            *f.actor)["scripts"]["shared"]["values"]["counter"]["value"] ==
          42);
}

TEST_CASE("A Journal job cannot rewrite another live root or its saved stack",
          "[Quest][QuestJob][MultiJob][Rollback]")
{
  Fixture f;
  auto jobs = TwoJobs();
  auto heap = Heap(jobs);
  f.Seed(jobs, heap);
  const auto before = f.Saved();
  const auto token = jobs.live.at(1).token;
  Complete(jobs, heap, 2);
  SECTION("another root counter and matching continuation")
  {
    ++jobs.live.at(1).instructions;
    heap["continuations"][token]["executedInstructions"] =
      jobs.live.at(1).instructions;
  }
  SECTION("another stack captured scalar")
  {
    heap["continuations"][token]["locals"][1]["value"]["value"] = 99;
  }
  SECTION("another stack instruction")
  {
    heap["continuations"][token]["instruction"] = 0;
  }
  SECTION("another stack content digest")
  {
    heap["continuations"][token]["contentHashes"][0]["hash"] =
      std::string(64, 'b');
  }
  // These are structurally valid snapshots. The transaction must enforce
  // selected-root ownership beyond the heap/job bijection alone.
  REQUIRE_NOTHROW(jobs.ValidateHeap(heap));
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, 2, [&] {
    QuestJournal::SetScriptState(*f.actor, heap);
    QuestJournal::SetJobState(*f.actor, jobs);
  }));
  REQUIRE(f.Saved() == before);
}

TEST_CASE(
  "Runtime entry requires the selected Journal lease, not another live ID",
  "[Quest][QuestJob][MultiJob][Rollback]")
{
  Fixture f;
  const auto jobs = TwoJobs();
  f.Seed(jobs, Heap(jobs));
  const auto before = f.Saved();
  REQUIRE_THROWS(QuestJournal::RequireJobExecution(*f.actor, 1));
  REQUIRE_THROWS(QuestJournal::Atomic(*f.actor, [] {}));
  REQUIRE_THROWS(QuestJournal::AtomicJob(
    *f.actor, 2, [&] { QuestJournal::RequireJobExecution(*f.actor, 1); }));
  REQUIRE(f.Saved() == before);
  f.Commit(2, [&] { QuestJournal::RequireJobExecution(*f.actor, 2); });
  REQUIRE(f.Saved() == before);
}

TEST_CASE("Admission preserves sleeping roots and their shared heap",
          "[Quest][QuestJob][MultiJob]")
{
  Fixture f;
  auto jobs = TwoJobs();
  auto heap = Heap(jobs);
  f.Seed(jobs, heap);
  const auto before = jobs.live;
  auto admitted = WaitingJob(jobs.nextJob++);
  static_cast<QuestJobState::Stage&>(admitted) = QuestJobState::Stage{};
  admitted.quest = "4e50d:Skyrim.esm";
  admitted.contentHash = std::string(64, 'a');
  admitted.stage = 30;
  admitted.segment = admitted.instructions = admitted.steps = 0;
  admitted.nextOperation = 1;
  jobs.live.emplace(admitted.id, admitted);
  f.Commit(admitted.id, [&] {
    QuestJournal::SetJobState(*f.actor, jobs);
    QuestJournal::RequireJobExecution(*f.actor, admitted.id);
  });
  const auto saved = QuestJournal::JobState(*f.actor);
  REQUIRE(saved.live.size() == 3);
  REQUIRE(saved.live.at(1) == before.at(1));
  REQUIRE(saved.live.at(2) == before.at(2));
  REQUIRE(QuestJournal::ScriptState(*f.actor) == heap);
}

TEST_CASE("Transaction job snapshots publish only successful owned updates",
          "[Quest][QuestJob][QuestTransactionSnapshot]")
{
  Fixture f;
  const auto initial = TwoJobs();
  auto heap = Heap(initial);
  f.Seed(initial, heap);
  f.Commit(2, [&] {
    auto detached = QuestJournal::JobState(*f.actor);
    detached.live.erase(1);
    REQUIRE(QuestJournal::PersonalJobState(*f.actor).live == initial.live);
    REQUIRE_NOTHROW(QuestJournal::RequireJobExecution(*f.actor, 2));

    auto rejected = initial;
    ++rejected.live.at(1).instructions;
    REQUIRE_NOTHROW(rejected.Save());
    REQUIRE_THROWS(QuestJournal::SetJobState(*f.actor, rejected));
    REQUIRE(QuestJournal::JobState(*f.actor).Save() == initial.Save());

    auto completed = initial;
    Complete(completed, heap, 2);
    QuestJournal::SetJobState(*f.actor, completed);
    REQUIRE_FALSE(QuestJournal::JobState(*f.actor).FindLive(2));
    REQUIRE(QuestJournal::PersonalJobState(*f.actor).FindCompleted(2));
    REQUIRE_THROWS(QuestJournal::RequireJobExecution(*f.actor, 2));
    REQUIRE_NOTHROW(QuestJournal::RequireJobAccess(*f.actor));
    // Caller mutation after publication cannot change either the saved JSON
    // or the transaction's selected-root/other-root guards.
    ++completed.live.at(1).instructions;
    REQUIRE(QuestJournal::JobState(*f.actor).live.at(1) == initial.live.at(1));
    REQUIRE(detached.FindLive(2));
    QuestJournal::SetScriptState(*f.actor, heap);
    REQUIRE(QuestJournal::JobState(*f.actor).FindCompleted(2));
  });
  REQUIRE(QuestJournal::JobState(*f.actor).live.at(1) == initial.live.at(1));
  REQUIRE(QuestJournal::JobState(*f.actor).FindCompleted(2));
}

TEST_CASE("Transaction snapshot rollback leaves the next lease fresh",
          "[Quest][QuestJob][QuestTransactionSnapshot][Rollback]")
{
  Fixture f;
  const auto initial = TwoJobs();
  auto heap = Heap(initial);
  f.Seed(initial, heap);
  SECTION("Current job schema")
  {
  }
  SECTION("Older job schema is decoded before caching")
  {
    auto legacy = f.Saved();
    legacy["jobs"]["schemaVersion"] = 9;
    f.actor->SetPropertyValueDump(QuestJournal::kProperty, legacy.dump(),
                                  false, false);
  }
  const auto before = f.Saved();
  auto completed = initial;
  Complete(completed, heap, 2);
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, 2, [&] {
    QuestJournal::SetJobState(*f.actor, completed);
    QuestJournal::SetScriptState(*f.actor, heap);
    REQUIRE(QuestJournal::JobState(*f.actor).FindCompleted(2));
    throw std::runtime_error("Abort after publishing a transaction snapshot");
  }));
  REQUIRE(f.Saved() == before);
  REQUIRE(QuestJournal::JobState(*f.actor).Save() == initial.Save());
  f.Commit(2, [&] {
    REQUIRE(QuestJournal::JobState(*f.actor).Save() == initial.Save());
    QuestJournal::SetJobState(*f.actor, completed);
    QuestJournal::SetScriptState(*f.actor, heap);
  });
  REQUIRE(QuestJournal::JobState(*f.actor).FindCompleted(2));
}

TEST_CASE("A validated job snapshot cannot bypass final heap validation",
          "[Quest][QuestJob][QuestTransactionSnapshot][Rollback]")
{
  Fixture f;
  auto jobs = TwoJobs();
  auto heap = Heap(jobs);
  f.Seed(jobs, heap);
  const auto before = f.Saved();
  Complete(jobs, heap, 2);
  REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, 2, [&] {
    QuestJournal::SetJobState(*f.actor, jobs);
    REQUIRE(QuestJournal::JobState(*f.actor).FindCompleted(2));
    // Deliberately retain the old heap's continuation for the now-completed
    // root. Typed cursor validity cannot authorize this inconsistent commit.
  }));
  REQUIRE(f.Saved() == before);
  REQUIRE(QuestJournal::JobState(*f.actor).FindLive(2));
}

TEST_CASE("External same-revision changes are never a cached job authority",
          "[Quest][QuestJob][QuestTransactionSnapshot][Rollback]")
{
  Fixture f;
  const auto initial = TwoJobs();
  f.Seed(initial, Heap(initial));
  const auto before = f.Saved();
  auto changed = initial;
  changed.live.at(1).automatic = true;
  auto replacement = before;
  replacement["jobs"] = changed.Save();
  REQUIRE(replacement.at("revision") == before.at("revision"));
  SECTION("Outside getters decode a replacement despite identical revision")
  {
    REQUIRE(QuestJournal::JobState(*f.actor).live.at(1).automatic == false);
    f.actor->SetPropertyValueDump(QuestJournal::kProperty, replacement.dump(),
                                  false, false);
    REQUIRE(QuestJournal::JobState(*f.actor).live.at(1).automatic);
    f.Commit(2, [&] {
      REQUIRE(QuestJournal::JobState(*f.actor).live.at(1).automatic);
    });
  }
  SECTION("External mutation during a lease still aborts the commit")
  {
    const auto gold = f.actor->GetInventory().GetItemCount(0xf);
    REQUIRE_THROWS(QuestJournal::AtomicJob(*f.actor, 2, [&] {
      QuestJournal::EditInventory(*f.actor, *f.actor,
                                  [](Inventory& inv) { inv.AddItem(0xf, 1); });
      f.actor->SetPropertyValueDump(QuestJournal::kProperty,
                                    replacement.dump(), false, false);
      // Transactional reads remain on their own snapshot, as before caching;
      // the exact live-source read guard must reject its eventual publication.
      REQUIRE_FALSE(QuestJournal::JobState(*f.actor).live.at(1).automatic);
    }));
    REQUIRE(f.actor->GetInventory().GetItemCount(0xf) == gold);
    REQUIRE(f.Saved() == replacement);
    REQUIRE(QuestJournal::JobState(*f.actor).live.at(1).automatic);
  }
  SECTION("Malformed replacement still fails before invoking a new lease")
  {
    replacement["jobs"]["live"][0]["unexpected"] = true;
    f.actor->SetPropertyValueDump(QuestJournal::kProperty, replacement.dump(),
                                  false, false);
    REQUIRE_THROWS(QuestJournal::JobState(*f.actor));
    bool entered = false;
    REQUIRE_THROWS(
      QuestJournal::AtomicJob(*f.actor, 2, [&] { entered = true; }));
    REQUIRE_FALSE(entered);
  }
}

TEST_CASE("A transaction snapshot cannot supply another campaign's reads",
          "[Quest][QuestJob][QuestTransactionSnapshot]")
{
  Fixture f;
  const auto jobs = TwoJobs();
  f.Seed(jobs, Heap(jobs));
  f.server.CreateActor(OwnerId + 1, { 1, 2, 3 }, 0, 0x3c);
  auto& other = f.server.worldState.GetFormAt<MpActor>(OwnerId + 1);
  f.Commit(2, [&] {
    REQUIRE_THROWS(QuestJournal::PersonalJobState(other));
    REQUIRE_THROWS(QuestJournal::JobState(other));
    REQUIRE_THROWS(QuestJournal::RequirePersonalJobAccess(other));
    REQUIRE(QuestJournal::JobState(*f.actor).Save() == jobs.Save());
  });
  REQUIRE_FALSE(QuestJournal::JobState(other).HasLive());
}
