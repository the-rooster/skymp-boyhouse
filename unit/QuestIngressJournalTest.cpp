#include "CampaignReferenceBinding.h"
#include "QuestJobState.h"
#include "QuestJournal.h"
#include "TestUtils.hpp"
#include "save_storages/ISaveStorage.h"
#include <catch2/catch_all.hpp>

extern espm::Loader& GetEspmLoader();

namespace {
using Json = nlohmann::json;
constexpr uint32_t kOriginal = 0x1a677;
const std::string kTarget = "1a677:Skyrim.esm";
const std::string kIdentity(32, 'a');

struct IngressStorage
  : Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>
{
  using Forms = std::vector<std::optional<MpChangeForm>>;
  bool SupportsAtomicBatches() const noexcept override { return true; }
  bool SupportsUpsertResults() const noexcept override { return true; }
  void IterateSync(const IterateSyncCallback&) override {}
  void Iterate(const IterateCallback& cb,
               const std::optional<std::vector<FormDesc>>&) override
  {
    cb({});
  }
  void Upsert(Forms&&, const UpsertCallback& cb) override { cb(); }
  void UpsertWithResult(Forms&&, const UpsertResultCallback& cb) override
  {
    cb({});
  }
  uint32_t GetNumFinishedUpserts() const override { return 0; }
  uint32_t GetNumFinishedIterates() const override { return 0; }
  void Tick() override {}
  bool GetRecycledChangeFormsBuffer(Forms&) override { return false; }
  const std::string& GetName() const override
  {
    static const std::string name = "ingress-journal-test";
    return name;
  }
};

struct IngressJournalFixture
{
  PartOne server;
  MpActor* owner;
  IngressJournalFixture()
  {
    server.AttachEspm(&GetEspmLoader());
    server.AttachSaveStorage(std::make_shared<IngressStorage>());
    server.CreateActor(0xff003100, { 1, 2, 3 }, 0, 0x3c);
    owner = &server.worldState.GetFormAt<MpActor>(0xff003100);
    owner->SetWorldInstance("campaign:" + std::to_string(owner->GetFormId()) +
                            ":ingress");
    owner->SetPropertyValueDump("private.boyhouse.campaignIdentity",
                                Json(kIdentity).dump(), false, false);
    Seed({});
  }
  void Seed(const QuestJobState& jobs, bool initialized = true)
  {
    Json ledger = { { "schemaVersion", 2 },
                    { "revision", uint64_t(1) },
                    { "quests", Json::object() },
                    { "jobs", jobs.Save() } };
    if (initialized)
      ledger["referenceInit"] = { { kTarget, true } };
    owner->SetPropertyValueDump(QuestJournal::kProperty, ledger.dump(), false,
                                false);
  }
  std::string Saved() const
  {
    return owner->GetDynamicFields().GetValueDump(QuestJournal::kProperty);
  }
  QuestJobState::Job Root(uint64_t id,
                          std::optional<uint64_t> occurrence = {}) const
  {
    QuestJobState::Job job;
    job.kind = QuestJobState::Kind::Interaction;
    job.cursor = "done";
    job.interactionDone = false;
    job.id = id;
    job.sequenced = true;
    job.command = "quest-request:1:" + std::to_string(id);
    job.actor = job.owner = owner->GetChangeForm().formDesc.ToString();
    job.instance = owner->GetWorldInstance();
    job.environmentHash = std::string(64, 'e');
    job.interaction = QuestJobState::Interaction{
      "OnTriggerEnter",       kTarget, "14:Skyrim.esm", "9000", job.actor,
      { "13bba:Skyrim.esm" }, {}
    };
    job.occurrence = occurrence;
    return job;
  }
  QuestJobState Next(QuestJobState state, bool native = true) const
  {
    state.RetireForAdmission();
    auto job = Root(state.nextJob++);
    if (native) {
      state.ingress.identity = kIdentity;
      job.occurrence = state.ingress.next++;
    }
    state.live.emplace(job.id, std::move(job));
    return state;
  }
  void Publish(const QuestJobState& state)
  {
    const auto id = state.nextJob - 1;
    auto receipt = QuestJournal::AtomicJob(
      *owner, id, [&] { QuestJournal::SetJobState(*owner, state); });
    for (unsigned i = 0;
         i < 5 && receipt->GetState() == SaveReceipt::State::Pending; ++i)
      server.worldState.Tick();
    REQUIRE(receipt->GetState() == SaveReceipt::State::Durable);
  }
  void InitializeSnapshot(uint64_t id)
  {
    auto next = Next(QuestJournal::JobState(*owner));
    REQUIRE(next.nextJob - 1 == id);
    QuestJournal::SetJobState(*owner, next);
    auto& job = next.live.at(id);
    job.dispatch = QuestJobState::Dispatch{};
    job.dispatch->recipients = { kTarget };
    job.segment = job.steps = job.instructions = 1;
    QuestJournal::SetJobState(*owner, next);
  }
};
}

TEST_CASE("Journal installs one native occurrence with its selected root",
          "[Quest][QuestIngress]")
{
  IngressJournalFixture f;
  REQUIRE(CampaignReferenceBinding::GetIdentity(*f.owner) == kIdentity);
  const auto initial = f.Saved();
  auto next = f.Next({});
  SECTION("Valid admission advances the independent ingress cursor")
  {
    f.Publish(next);
    const auto saved = QuestJournal::JobState(*f.owner);
    REQUIRE(saved.ingress.next == 2);
    REQUIRE(saved.ingress.identity == kIdentity);
    REQUIRE(saved.FindOccurrence(1));
    REQUIRE(saved.FindOccurrence(1)->id == 1);
    const auto snapshot = QuestJournal::Snapshot(*f.owner);
    REQUIRE_FALSE(snapshot.contains("jobs"));
    REQUIRE_FALSE(snapshot.contains("ingress"));
    REQUIRE(snapshot.dump().find(kIdentity) == std::string::npos);
  }
  SECTION("Validly shaped replacement campaign identity is refused")
  {
    next.ingress.identity = std::string(32, 'b');
    REQUIRE_NOTHROW(next.Save());
    REQUIRE_THROWS(f.Publish(next));
    REQUIRE(f.Saved() == initial);
  }
  SECTION("Initialization must exist before the admission transaction")
  {
    f.Seed({}, false);
    const auto uninitialized = f.Saved();
    REQUIRE_THROWS(f.Publish(next));
    REQUIRE(f.Saved() == uninitialized);
    REQUIRE_FALSE(QuestJournal::ReferenceInitialized(*f.owner, kOriginal));
  }
}

TEST_CASE("Journal does not let an existing root reset ingress provenance",
          "[Quest][QuestIngress]")
{
  IngressJournalFixture f;
  auto saved = f.Next({});
  f.Publish(saved);
  const auto original = f.Saved();
  saved.live.at(1).occurrence.reset();
  saved.ingress = {};
  REQUIRE_NOTHROW(
    saved.Save()); // Independently well-formed, wrong transition.
  REQUIRE_THROWS(QuestJournal::AtomicJob(
    *f.owner, 1, [&] { QuestJournal::SetJobState(*f.owner, saved); }));
  REQUIRE(f.Saved() == original);
  REQUIRE(QuestJournal::JobState(*f.owner).FindOccurrence(1));
}

TEST_CASE("Ordinary numbered admission compacts exact native history only",
          "[Quest][QuestIngress]")
{
  IngressJournalFixture f;
  QuestJobState before;
  before.ingress.identity = kIdentity;
  for (uint64_t id = 1; id <= QuestJobState::MaxRecentCompleted; ++id) {
    auto job = f.Root(before.nextJob++, before.ingress.next++);
    job.interactionDone = true;
    job.segment = 1;
    job.steps = 1;
    before.completed.push_back(std::move(job));
  }
  f.Seed(before);
  const auto original = f.Saved();
  auto next = f.Next(before, false);
  REQUIRE(next.ingress.next == before.ingress.next);
  REQUIRE(next.ingress.retiredThrough == 1);
  SECTION("The selected ordinary root may retire the oldest native result")
  {
    f.Publish(next);
    const auto state = QuestJournal::JobState(*f.owner);
    REQUIRE(state.IsOccurrenceRetired(1));
    REQUIRE_FALSE(state.FindOccurrence(1));
    REQUIRE(state.FindOccurrence(2));
    REQUIRE_FALSE(state.live.at(state.nextJob - 1).occurrence);
  }
  SECTION("Erasing all native provenance is not ordinary retirement")
  {
    next.ingress = {};
    for (auto& job : next.completed)
      job.occurrence.reset();
    REQUIRE_NOTHROW(next.Save());
    REQUIRE_THROWS(f.Publish(next));
    REQUIRE(f.Saved() == original);
  }
}

TEST_CASE(
  "Batch journal leases require a complete initial snapshot for each ID",
  "[Quest][QuestIngress][QuestOccurrenceBatch]")
{
  IngressJournalFixture f;
  f.Publish(f.Next({}));
  const auto oldRoot = QuestJournal::JobState(*f.owner).live.at(1);
  const auto initial = f.Saved();
  SECTION("Two fresh roots share one receipt and preserve the existing root")
  {
    const auto receipt = QuestJournal::AtomicJobAdmissions(
      *f.owner, { 2, 3 }, [] {},
      [&](uint64_t id) { f.InitializeSnapshot(id); });
    REQUIRE(receipt);
    REQUIRE(receipt->GetState() == SaveReceipt::State::Pending);
    const auto jobs = QuestJournal::JobState(*f.owner);
    REQUIRE(jobs.live.at(1) == oldRoot);
    REQUIRE(jobs.nextJob == 4);
    REQUIRE(jobs.ingress.next == 4);
    for (const auto id : { 2, 3 }) {
      REQUIRE(jobs.live.at(id).dispatch);
      REQUIRE(jobs.live.at(id).segment == 1);
      REQUIRE(jobs.live.at(id).instructions == 1);
      REQUIRE_FALSE(jobs.live.at(id).handler);
    }
    for (unsigned i = 0;
         i < 5 && receipt->GetState() == SaveReceipt::State::Pending; ++i)
      f.server.worldState.Tick();
    REQUIRE(receipt->GetState() == SaveReceipt::State::Durable);
  }
  SECTION("An uninitialized last root rolls the whole batch back")
  {
    REQUIRE_THROWS(QuestJournal::AtomicJobAdmissions(
      *f.owner, { 2, 3 }, [] {},
      [&](uint64_t id) {
        if (id == 2)
          f.InitializeSnapshot(id);
        else
          QuestJournal::SetJobState(*f.owner,
                                    f.Next(QuestJournal::JobState(*f.owner)));
      }));
    REQUIRE(f.Saved() == initial);
  }
  SECTION("A later callback cannot edit an earlier admitted root")
  {
    REQUIRE_THROWS(QuestJournal::AtomicJobAdmissions(
      *f.owner, { 2, 3 }, [] {},
      [&](uint64_t id) {
        f.InitializeSnapshot(id);
        if (id == 3) {
          auto state = QuestJournal::JobState(*f.owner);
          state.live.at(2).automatic = true;
          QuestJournal::SetJobState(*f.owner, state);
        }
      }));
    REQUIRE(f.Saved() == initial);
  }
  SECTION("An old live root cannot be selected through the batch")
  {
    bool called = false;
    REQUIRE_THROWS(QuestJournal::AtomicJobAdmissions(
      *f.owner, { 1, 2 }, [&] { called = true; },
      [&](uint64_t id) { f.InitializeSnapshot(id); }));
    REQUIRE_FALSE(called);
    REQUIRE(f.Saved() == initial);
  }
  SECTION("A skipped ID is refused before preparation")
  {
    bool called = false;
    REQUIRE_THROWS(QuestJournal::AtomicJobAdmissions(
      *f.owner, { 2, 4 }, [&] { called = true; },
      [&](uint64_t id) { f.InitializeSnapshot(id); }));
    REQUIRE_FALSE(called);
    REQUIRE(f.Saved() == initial);
  }
  SECTION("Empty admission seals existing state without taking a root lease")
  {
    bool prepared = false, initialized = false;
    const auto receipt = QuestJournal::AtomicJobAdmissions(
      *f.owner, {}, [&] { prepared = true; },
      [&](uint64_t) { initialized = true; });
    REQUIRE(prepared);
    REQUIRE_FALSE(initialized);
    REQUIRE(receipt);
    REQUIRE(receipt->GetState() == SaveReceipt::State::Pending);
    REQUIRE(f.Saved() == initial);
  }
}

TEST_CASE("Admission runtime entry cannot advance beyond its pristine capture",
          "[Quest][QuestIngress][QuestOccurrenceBatch]")
{
  IngressJournalFixture f;
  const auto receipt = QuestJournal::AtomicJobAdmissions(
    *f.owner, { 1 }, [] {},
    [&](uint64_t id) {
      auto state = f.Next(QuestJournal::JobState(*f.owner));
      QuestJournal::SetJobState(*f.owner, state);
      auto& job = state.live.at(id);
      auto advanced = job;
      advanced.dispatch = QuestJobState::Dispatch{};
      advanced.dispatch->recipients = { kTarget };
      advanced.segment = advanced.steps = advanced.instructions = 1;
      // A mutable cursor cannot borrow a pristine saved root's admission
      // lease to enter later attachment/handler work.
      REQUIRE_THROWS(QuestJournal::RequireJobExecution(*f.owner, advanced));
      REQUIRE_NOTHROW(QuestJournal::RequireJobExecution(*f.owner, job));
      REQUIRE_THROWS(QuestJournal::RequireJobExecution(*f.owner, job));
      job = std::move(advanced);
      QuestJournal::SetJobState(*f.owner, state);
    });
  REQUIRE(receipt);
  REQUIRE(receipt->GetState() == SaveReceipt::State::Pending);
  REQUIRE(QuestJournal::JobState(*f.owner).live.at(1).segment == 1);
}

TEST_CASE(
  "Admission can add defaults without changing existing shared VM values",
  "[Quest][QuestIngress][QuestOccurrenceBatch]")
{
  IngressJournalFixture f;
  Json heap = {
    { "schemaVersion", 6 },        { "scripts", Json::object() },
    { "arrays", Json::object() },  { "globals", Json::object() },
    { "aliases", Json::object() }, { "continuations", Json::object() }
  };
  heap["scripts"]["shared"] = {
    { "schemaVersion", 1 },
    { "values",
      { { "counter", { { "type", 3 }, { "value", 7 } } },
        { "array", { { "type", 13 }, { "array", "1" } } } } }
  };
  heap["arrays"]["1"] = {
    { "type", 13 },
    { "values", Json::array({ { { "type", 3 }, { "value", 9 } } }) }
  };
  auto ledger = Json::parse(f.Saved());
  ledger["vm"] = heap;
  f.owner->SetPropertyValueDump(QuestJournal::kProperty, ledger.dump(), false,
                                false);
  const auto initial = f.Saved();
  auto changed = heap;
  bool allowed = false;
  SECTION("A newly discovered default is permitted")
  {
    changed["scripts"]["shared"]["values"]["newDefault"] = { { "type", 3 },
                                                             { "value", 0 } };
    allowed = true;
  }
  SECTION("An existing scalar cannot be used as a hidden handler effect")
  {
    changed["scripts"]["shared"]["values"]["counter"]["value"] = 8;
  }
  SECTION("An existing shared array node is immutable during admission")
  {
    changed["arrays"]["1"]["values"][0]["value"] = 10;
  }
  const auto admit = [&] {
    return QuestJournal::AtomicJobAdmissions(
      *f.owner, { 1 }, [] {},
      [&](uint64_t id) {
        f.InitializeSnapshot(id);
        QuestJournal::SetScriptState(*f.owner, changed);
      });
  };
  if (allowed) {
    REQUIRE(admit()->GetState() == SaveReceipt::State::Pending);
    REQUIRE(QuestJournal::ScriptState(*f.owner) == changed);
    REQUIRE(QuestJournal::JobState(*f.owner).ingress.next == 2);
  } else {
    REQUIRE_THROWS(admit());
    REQUIRE(f.Saved() == initial);
    REQUIRE(QuestJournal::ScriptState(*f.owner) == heap);
    REQUIRE(QuestJournal::JobState(*f.owner).ingress.next == 1);
  }
}
