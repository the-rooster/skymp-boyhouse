#include "CampaignMovement.h"
#include "CampaignReferenceBinding.h"
#include "QuestJobContent.h"
#include "QuestJobRunner.h"
#include "QuestJournal.h"
#include "QuestScriptRuntime.h"
#include "TestUtils.hpp"
#include "libespm/REFR.h"
#include "save_storages/ISaveStorage.h"
#include <catch2/catch_all.hpp>
#include <chrono>
#include <functional>
#include <iostream>
#include <utility>

extern espm::Loader& GetEspmLoader();

namespace {
using Json = nlohmann::json;
using Occurrence = QuestJobRunner::CapturedOccurrence;
using Endpoint = QuestJobRunner::OccurrenceTarget;
constexpr uint32_t kOwner = 0xff009a00, kGuest = kOwner + 1, kNpc = kOwner + 2;
// Distinct Skyrim REFRs with defaultSetStageTRIGScript and real XPRM bounds;
// none is overridden by the four later official masters. Winning records
// supply bounds/base data at runtime; copies never share an original binding.
const std::vector<uint32_t> kOriginals = {
  0x6debb, 0x2b363, 0xf6789, 0xf678b, 0xf678a, 0x101d7a, 0xddfeb, 0x8443b,
  0x8443a, 0x77c5e, 0x9439a, 0x9439b, 0x81305, 0x87166,  0x87165, 0x87164
};
const NiPoint3 kLeft{ 1000, 1000, 1000 }, kRight{ 1600, 1000, 1000 };
const NiPoint3 kOutside{ 2500, 1000, 1000 };

struct CaptureProgram
{
  std::vector<std::shared_ptr<PexScript>> sources;
  CaptureProgram()
  {
    for (const auto& [name, parent] :
         std::vector<std::pair<std::string, std::string>>{
           { "defaultSetStageTRIGScript", "ObjectReference" },
           { "Form", "" },
           { "ObjectReference", "Form" },
           { "Actor", "ObjectReference" },
           { "Quest", "Form" } }) {
      auto script = std::make_shared<PexScript>();
      script->source = name;
      Object object;
      object.NameIndex = name;
      object.parentClassName = parent;
      if (name == "defaultSetStageTRIGScript") {
        object.variables = { { "::stage_var", "Int", 0, VarValue(0) } };
        FunctionInfo init;
        init.valid = true;
        init.returnType = "None";
        init.code.instructions = { { FunctionCode::kOp_Return,
                                     { VarValue::None() } } };
        object.states = { { "", { { "OnInit", init } } } };
      }
      script->objectTable.push_back(std::move(object));
      sources.push_back(std::move(script));
    }
  }
  std::vector<PexScript::Lazy> Scripts(std::function<void()> onLoad = {}) const
  {
    std::vector<PexScript::Lazy> result;
    auto fired = std::make_shared<bool>(false);
    for (const auto& script : sources)
      result.push_back({ script->source, [script, fired, onLoad] {
                          if (onLoad && !std::exchange(*fired, true))
                            onLoad();
                          return script;
                        } });
    return result;
  }
};

struct CaptureStorage
  : Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>
{
  using Forms = std::vector<std::optional<MpChangeForm>>;
  size_t submissions = 0;
  bool SupportsAtomicBatches() const noexcept override { return true; }
  bool SupportsUpsertResults() const noexcept override { return true; }
  void IterateSync(const IterateSyncCallback&) override {}
  void Iterate(const IterateCallback& callback,
               const std::optional<std::vector<FormDesc>>&) override
  {
    callback({});
  }
  void Upsert(Forms&&, const UpsertCallback& callback) override
  {
    ++submissions;
    callback();
  }
  void UpsertWithResult(Forms&&, const UpsertResultCallback& callback) override
  {
    ++submissions;
    callback({});
  }
  uint32_t GetNumFinishedUpserts() const override
  {
    return static_cast<uint32_t>(submissions);
  }
  uint32_t GetNumFinishedIterates() const override { return 0; }
  void Tick() override {}
  bool GetRecycledChangeFormsBuffer(Forms&) override { return false; }
  const std::string& GetName() const override
  {
    static const std::string name = "capture-context";
    return name;
  }
};

struct CaptureFixture
{
  PartOne server;
  CaptureProgram program;
  std::shared_ptr<CaptureStorage> storage = std::make_shared<CaptureStorage>();
  size_t count;
  explicit CaptureFixture(size_t primitives = 2, bool outside = false)
    : count(primitives)
  {
    REQUIRE(count > 0);
    REQUIRE(count <= kOriginals.size());
    server.AttachEspm(&GetEspmLoader());
    server.AttachSaveStorage(storage);
    const auto phase = "campaign:" + std::to_string(kOwner) + ":capture";
    server.CreateActor(kOwner, outside ? kOutside : kLeft, 0, 0x3c);
    Owner().SetWorldInstance(phase);
    Owner().SetInventory(Inventory().AddItem(0xf, 10));
    server.CreateActor(kGuest, kOutside, 0, 0x3c);
    Guest().SetWorldInstance(phase);
    Guest().SetInventory(Inventory().AddItem(0xf, 77));
    auto& world = server.worldState;
    {
      WorldState::RestorationScope restoring(world);
      for (size_t index = 0; index < count; ++index) {
        auto& target = AddTarget(index, CopyId(index));
        CampaignReferenceBinding::Bind(Owner(), kOriginals[index], target);
      }
      world.AddForm(std::make_unique<MpActor>(
                      LocationalData{ kOutside, {}, FormDesc::Tamriel() },
                      FormCallbacks::DoNothing(), 0x13bbd),
                    kNpc);
      Npc().SetWorldInstance(phase);
      Npc().SetInventory(Inventory().AddItem(0xf, 7));
      CampaignReferenceBinding::Bind(Owner(), 0x1a677, Npc());
    }
    for (size_t index = 0; index < count; ++index)
      QuestScriptRuntime::ExecuteReferenceLoad(Owner(), kOriginals[index],
                                               program.Scripts());
    Owner().ForceSubscriptionsUpdate();
    Guest().ForceSubscriptionsUpdate();
    Npc().ForceSubscriptionsUpdate();
    server.Tick();
  }
  static uint32_t CopyId(size_t index) { return kOwner + 16 + index; }
  MpActor& Owner() { return server.worldState.GetFormAt<MpActor>(kOwner); }
  MpActor& Guest() { return server.worldState.GetFormAt<MpActor>(kGuest); }
  MpActor& Npc() { return server.worldState.GetFormAt<MpActor>(kNpc); }
  MpObjectReference& Target(size_t index = 0)
  {
    return server.worldState.GetFormAt<MpObjectReference>(CopyId(index));
  }
  QuestJobRunner& Jobs() { return server.worldState.GetQuestJobs(); }
  MpObjectReference& AddTarget(size_t index, uint32_t id)
  {
    auto& world = server.worldState;
    const auto record =
      world.GetEspm().GetBrowser().LookupById(kOriginals[index]);
    REQUIRE(record.rec);
    REQUIRE(record.rec->GetType() == "REFR");
    const auto data = reinterpret_cast<const espm::REFR*>(record.rec)
                        ->GetData(world.GetEspmCache());
    REQUIRE(data.boundsDiv2);
    const auto base = record.ToGlobalId(data.baseId);
    const auto type =
      world.GetEspm().GetBrowser().LookupById(base).rec->GetType().ToString();
    world.AddForm(
      std::make_unique<MpObjectReference>(
        LocationalData{ index % 2 ? kRight : kLeft, {}, FormDesc::Tamriel() },
        FormCallbacks::DoNothing(), base, type.c_str(),
        NiPoint3{ data.boundsDiv2[0], data.boundsDiv2[1],
                  data.boundsDiv2[2] }),
      id);
    auto& copy = world.GetFormAt<MpObjectReference>(id);
    copy.SetWorldInstance(Owner().GetWorldInstance());
    copy.SetInventory(Inventory());
    return copy;
  }
  std::vector<Endpoint> Endpoints()
  {
    std::vector<Endpoint> result;
    for (size_t index = 0; index < count; ++index)
      result.push_back({ &Target(index), "OnTrigger" });
    return result;
  }
  std::vector<Occurrence> Scalar(
    MpActor& actor, const std::vector<Endpoint>& endpoints,
    MpObjectReference& source,
    std::optional<QuestJobRunner::OccurrenceId> start = {})
  {
    auto next = start ? *start : Jobs().NextOccurrence(actor);
    std::vector<Occurrence> result;
    for (const auto& endpoint : endpoints) {
      result.push_back(Jobs().CaptureOccurrence(actor, next, *endpoint.target,
                                                endpoint.name, source));
      ++next.sequence;
    }
    return result;
  }
  Json Saved() { return MpChangeForm::ToJson(Owner().GetChangeForm()); }
  Json Ledger()
  {
    return Json::parse(
      Owner().GetDynamicFields().GetValueDump(QuestJournal::kProperty));
  }
};
}

TEST_CASE(
  "Batch capture preserves scalar identities order and read-only state",
  "[QuestCaptureContext][QuestIngress]")
{
  const int sourceKind = GENERATE(0, 1, 2);
  CaptureFixture f;
  auto& actor = sourceKind == 1 ? f.Guest() : f.Owner();
  MpObjectReference& source = sourceKind == 2 ? f.Npc() : actor;
  auto endpoints = f.Endpoints();
  endpoints.push_back({ &f.Target(), "OnTriggerEnter" });
  endpoints.push_back({ &f.Target(1), "OnTriggerLeave" });
  endpoints.push_back({ &f.Target(), "OnTrigger" });
  const auto before = f.Saved();
  const auto guestBefore = MpChangeForm::ToJson(f.Guest().GetChangeForm());
  const auto npcBefore = MpChangeForm::ToJson(f.Npc().GetChangeForm());
  const auto next = f.Jobs().NextOccurrence(actor);
  const auto context = QuestJournal::CaptureReadContext(actor);
  const auto scalar = f.Scalar(actor, endpoints, source);
  const auto batch =
    f.Jobs().CaptureOccurrences(actor, context, endpoints, source);
  REQUIRE(batch == scalar);
  REQUIRE(f.Jobs().CaptureOccurrences(actor, endpoints, source) == scalar);
  REQUIRE(f.Jobs().CaptureOccurrences(actor, context, {}, source).empty());
  REQUIRE(batch.front().id == next);
  REQUIRE(batch.back().id.sequence == next.sequence + endpoints.size() - 1);
  for (const auto& occurrence : batch) {
    REQUIRE(occurrence.interaction.source ==
            (sourceKind == 2 ? "1a677:Skyrim.esm" : "14:Skyrim.esm"));
    REQUIRE(occurrence.interaction.sourceReference ==
            source.GetChangeForm().formDesc.ToString());
  }
  REQUIRE(f.Jobs().NextOccurrence(actor) == next);
  REQUIRE(f.Saved() == before);
  REQUIRE(MpChangeForm::ToJson(f.Guest().GetChangeForm()) == guestBefore);
  REQUIRE(MpChangeForm::ToJson(f.Npc().GetChangeForm()) == npcBefore);
}

TEST_CASE("Capture contexts reject actor replacement and phase round trips",
          "[QuestCaptureContext][QuestIngress][Lifetime]")
{
  const bool replace = GENERATE(false, true);
  CaptureFixture f;
  const auto context = QuestJournal::CaptureReadContext(f.Guest());
  const auto before = f.Saved();
  const auto original = f.server.worldState.LookupFormByIdNoLoad(kGuest);
  if (replace) {
    const auto saved = f.Guest().GetChangeForm();
    f.server.DestroyActor(kGuest);
    f.server.worldState.LoadChangeForm(saved, FormCallbacks::DoNothing());
    REQUIRE(f.server.worldState.LookupFormByIdNoLoad(kGuest) != original);
  } else {
    const auto instance = f.Guest().GetWorldInstance();
    const auto epoch = f.Guest().GetWorldInstanceEpoch();
    f.Guest().SetWorldInstance(instance + "-away");
    f.Guest().SetWorldInstance(instance);
    REQUIRE(f.Guest().GetWorldInstanceEpoch() > epoch);
  }
  REQUIRE_THROWS(context.Validate(f.Guest()));
  REQUIRE_THROWS(
    f.Jobs().CaptureOccurrences(f.Guest(), context, f.Endpoints(), f.Guest()));
  REQUIRE(f.Jobs().CaptureOccurrences(f.Guest(), f.Endpoints(), f.Guest()) ==
          f.Scalar(f.Guest(), f.Endpoints(), f.Guest()));
  REQUIRE(f.Saved() == before);
}

TEST_CASE("Capture resolves current binding uniqueness and exact endpoints",
          "[QuestCaptureContext][QuestIngress][Lifetime]")
{
  const bool duplicate = GENERATE(false, true);
  CaptureFixture f;
  const auto context = QuestJournal::CaptureReadContext(f.Owner());
  const auto original =
    f.server.worldState.LookupFormByIdNoLoad(CaptureFixture::CopyId(0));
  auto* oldTarget = original->AsObjectReference();
  REQUIRE(oldTarget);
  const auto saved = oldTarget->GetChangeForm();
  if (duplicate) {
    WorldState::RestorationScope restoration(f.server.worldState);
    auto& copy = f.AddTarget(0, kOwner + 100);
    copy.RegisterPrivateIndexedProperty(
      CampaignReferenceBinding::kProperty,
      saved.dynamicFields.GetValueDump(CampaignReferenceBinding::kProperty));
  } else {
    f.server.worldState.DestroyForm(oldTarget->GetFormId());
    f.server.worldState.LoadChangeForm(saved, FormCallbacks::DoNothing());
    REQUIRE(&f.Target() != oldTarget);
  }
  const auto before = f.Saved();
  const std::vector<Endpoint> endpoints = { { &f.Target(1), "OnTrigger" },
                                            { oldTarget, "OnTriggerEnter" } };
  REQUIRE(f.Jobs().CaptureOccurrences(f.Owner(), context,
                                      { endpoints.front() }, f.Owner()) ==
          f.Scalar(f.Owner(), { endpoints.front() }, f.Owner()));
  REQUIRE_THROWS(QuestScriptRuntime::CaptureInteraction(
    f.Owner(), *oldTarget, "OnTriggerEnter", f.Owner(), context));
  REQUIRE_THROWS(f.Scalar(f.Owner(), endpoints, f.Owner()));
  REQUIRE_THROWS(
    f.Jobs().CaptureOccurrences(f.Owner(), context, endpoints, f.Owner()));
  REQUIRE(f.Saved() == before);
  if (!duplicate) {
    // A fresh pointer to the same saved logical copy is valid recovery. An
    // old native pointer must never silently resolve to that replacement.
    REQUIRE(f.Jobs().CaptureOccurrences(f.Owner(), f.Endpoints(), f.Owner()) ==
            f.Scalar(f.Owner(), f.Endpoints(), f.Owner()));
  }
}

TEST_CASE("Capture context cannot cross a journal transaction or byte change",
          "[QuestCaptureContext][QuestIngress][Rollback]")
{
  CaptureFixture f;
  const auto context = QuestJournal::CaptureReadContext(f.Owner());
  const auto original = f.Ledger();
  SECTION("An external snapshot cannot lend authority inside a transaction")
  {
    QuestJournal::Atomic(f.Owner(), [&] {
      REQUIRE_THROWS(context.Validate(f.Owner()));
      REQUIRE_THROWS(QuestJournal::CaptureReadContext(f.Owner()));
      REQUIRE_THROWS(f.Jobs().CaptureOccurrences(f.Owner(), context,
                                                 f.Endpoints(), f.Owner()));
      REQUIRE(QuestJournal::ReferenceInitialized(f.Owner(), kOriginals[0]));
    });
    REQUIRE(f.Ledger() == original);
  }
  SECTION("Same revision does not preserve a removed initialization marker")
  {
    auto changed = original;
    changed["referenceInit"].erase("6debb:Skyrim.esm");
    REQUIRE(changed.at("revision") == original.at("revision"));
    f.Owner().SetPropertyValueDump(QuestJournal::kProperty, changed.dump(),
                                   false, false);
    REQUIRE_THROWS(context.Validate(f.Owner()));
    REQUIRE_THROWS(f.Jobs().CaptureOccurrences(f.Owner(), context,
                                               f.Endpoints(), f.Owner()));
    const auto fresh = QuestJournal::CaptureReadContext(f.Owner());
    REQUIRE_THROWS(
      f.Jobs().CaptureOccurrences(f.Owner(), fresh, f.Endpoints(), f.Owner()));
    REQUIRE_THROWS(f.Scalar(f.Owner(), f.Endpoints(), f.Owner()));
    REQUIRE(f.Ledger() == changed);
  }
}

TEST_CASE(
  "Loader reentry cannot reuse an earlier capture initialization snapshot",
  "[QuestCaptureContext][QuestIngress][Rollback]")
{
  CaptureFixture f;
  const auto context = QuestJournal::CaptureReadContext(f.Owner());
  const auto events =
    f.Jobs().CaptureOccurrences(f.Owner(), context, f.Endpoints(), f.Owner());
  auto changed = f.Ledger();
  const auto revision = changed.at("revision");
  changed["referenceInit"].erase("6debb:Skyrim.esm");
  bool loaded = false;
  const auto scripts = f.program.Scripts([&] {
    loaded = true;
    f.Owner().SetPropertyValueDump(QuestJournal::kProperty, changed.dump(),
                                   false, false);
    REQUIRE_THROWS(context.Validate(f.Owner()));
    REQUIRE_THROWS(QuestScriptRuntime::CaptureInteraction(
      f.Owner(), f.Target(), "OnActivate", f.Owner(), context));
  });
  REQUIRE_THROWS(f.Jobs().AdmitOccurrences(
    f.Owner(), events, QuestJobRunner::MetadataOverrides{}, scripts));
  REQUIRE(loaded);
  REQUIRE(changed.at("revision") == revision);
  REQUIRE(f.Ledger() == changed);
  REQUIRE_FALSE(QuestJournal::JobState(f.Owner()).HasLive());
  REQUIRE(QuestJournal::JobState(f.Owner()).ingress.next ==
          events.front().id.sequence);
  REQUIRE(f.Owner().GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(f.Owner().GetDynamicFields().GetValueDump(
            CampaignMovementPlan::kProperty) == "null");
}

TEST_CASE("Measure capture contexts endpoints and native movement plans",
          "[.][QuestCapturePerformance]")
{
  // Opt-in server CPU measurements with real owned primitive records and
  // generated initialization PEX. No admission, receipt, handler, network,
  // disk, crowd movement or game-client work is included in these timings.
  using Clock = std::chrono::steady_clock;
  const auto measure = [](const auto& operation) {
    const auto begin = Clock::now();
    operation();
    return std::chrono::duration<double, std::micro>(Clock::now() - begin)
      .count();
  };
  for (const size_t count : { 1u, 2u, 16u }) {
    for (const bool outside : { true, false }) {
      for (size_t sample = 0; sample < 3; ++sample) {
        CAPTURE(count, outside, sample);
        CaptureFixture f(count, outside);
        const auto endpoints = f.Endpoints();
        const NiPoint3 destination =
          outside ? NiPoint3{ 2501, 1000, 1000 } : kRight;
        const NiPoint3 angle{ 0, 0, 30 };
        const auto before = f.Saved();
        // Materialize all setup and warm immutable record caches before the
        // measured reads. Recreate the same baseline for every sample.
        f.Scalar(f.Owner(), endpoints, f.Owner());
        f.Owner().PrepareCampaignMovement(f.Owner(), destination, angle);
        const auto jobs = QuestJournal::JobState(f.Owner());
        const auto heap = QuestJournal::ScriptState(f.Owner());
        std::optional<QuestJournal::CaptureContext> context;
        const auto contextUs = measure([&] {
          context.emplace(QuestJournal::CaptureReadContext(f.Owner()));
        });
        std::vector<Occurrence> batch, scalar;
        const auto batchUs = measure([&] {
          batch = f.Jobs().CaptureOccurrences(f.Owner(), *context, endpoints,
                                              f.Owner());
        });
        // The scalar reference includes each call's own fresh context, but
        // excludes this one prospective-ID lookup. Batch context creation is
        // reported separately; plan preparation includes its fresh context.
        const auto next = f.Jobs().NextOccurrence(f.Owner());
        const auto scalarUs = measure(
          [&] { scalar = f.Scalar(f.Owner(), endpoints, f.Owner(), next); });
        REQUIRE(batch == scalar);
        REQUIRE(batch.size() == count);
        std::shared_ptr<CampaignMovementPlan> plan;
        const auto planUs = measure([&] {
          plan =
            f.Owner().PrepareCampaignMovement(f.Owner(), destination, angle);
        });
        REQUIRE(plan);
        const auto validateUs = measure([&] { plan->Validate(f.Owner()); });
        const auto expectedEdges = outside ? 0 : count;
        const auto expectedLevels = outside ? 0 : count / 2;
        REQUIRE(plan->GetEvents().size() == expectedEdges + expectedLevels);
        const auto baseline = Json::parse(plan->GetBaselineDump());
        REQUIRE(baseline.at("primitives").size() == count);
        REQUIRE(plan->GetPosition() == destination);
        REQUIRE(plan->GetAngle() == angle);
        std::vector<Endpoint> physical;
        size_t enters = 0, leaves = 0, levels = 0;
        for (const auto& event : plan->GetEvents()) {
          auto* target = event.target->AsObjectReference();
          REQUIRE(target);
          physical.push_back({ target, event.name });
          enters += event.name == "OnTriggerEnter";
          leaves += event.name == "OnTriggerLeave";
          levels += event.name == "OnTrigger";
        }
        // Check the complete physical event sequence independently of timing,
        // including the empty event list and repeated target level events.
        REQUIRE(f.Jobs().CaptureOccurrences(f.Owner(), *context, physical,
                                            f.Owner()) ==
                f.Scalar(f.Owner(), physical, f.Owner()));
        REQUIRE(enters == (outside ? 0 : count / 2));
        REQUIRE(leaves == (outside ? 0 : (count + 1) / 2));
        REQUIRE(levels == expectedLevels);
        REQUIRE(f.Saved() == before);
        const Json output = {
          { "benchmark", "quest_capture_context_v1" },
          { "sample", sample },
          { "unit", "microseconds" },
          { "clock", "steady_clock" },
          { "timed_scope", "read_only_server_cpu" },
          { "fixture", "owned_primitives_generated_init_pex" },
          { "timing_boundaries",
            { { "setup_and_initialization", "excluded" },
              { "scalar_prospective_id_lookup", "excluded" },
              { "scalar_per_call_context", "included" },
              { "batch_context_creation", "reported_separately" },
              { "plan_preparation_context", "included" },
              { "plan_revalidation", "fresh_plan_reconstruction" } } },
          { "primitive_count", baseline.at("primitives").size() },
          { "endpoint_count", endpoints.size() },
          { "layout", outside ? "outside_all" : "alternating_crossings" },
          { "events",
            { { "enter", enters },
              { "leave", leaves },
              { "level", levels },
              { "total", physical.size() } } },
          { "baseline",
            { { "registered_actors", 3 },
              { "live_jobs", jobs.live.size() },
              { "journal_json_bytes",
                f.Owner()
                  .GetDynamicFields()
                  .GetValueDump(QuestJournal::kProperty)
                  .size() },
              { "heap_json_bytes", heap.dump().size() } } },
          { "context_create_us", contextUs },
          { "batch_endpoint_capture_us", batchUs },
          { "scalar_endpoint_capture_us", scalarUs },
          { "plan_prepare_us", planUs },
          { "plan_revalidate_us", validateUs },
          { "plan_baseline_json_bytes", plan->GetBaselineDump().size() }
        };
        std::cout << output.dump() << std::endl;
      }
    }
  }
}
