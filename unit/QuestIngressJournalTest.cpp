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
