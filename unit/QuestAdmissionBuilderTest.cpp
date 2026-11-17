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

extern espm::Loader& GetEspmLoader();

namespace {
using Json = nlohmann::json;
using Request = QuestJobRunner::RequestId;
using Status = QuestJobRunner::Status;
constexpr uint32_t kOwner = 0xff009800, kTrigger = 0x6debb;
const NiPoint3 kStart{ 1000, 1000, 1000 }, kFinish{ 1600, 1000, 1000 };

VarValue Id(const char* name)
{
  return VarValue(VarValue::kType_Identifier, name);
}

struct BuilderProgram
{
  std::vector<std::shared_ptr<PexScript>> sources;
  BuilderProgram()
  {
    auto script = std::make_shared<PexScript>();
    script->source = "defaultSetStageTRIGScript";
    Object object;
    object.NameIndex = script->source;
    object.parentClassName = "ObjectReference";
    object.variables = { { "::stage_var", "Int", 0, VarValue(0) },
                         { "prefixes", "Int", 0, VarValue(0) },
                         { "suffixes", "Int", 0, VarValue(0) } };
    FunctionInfo init;
    init.valid = true;
    init.returnType = "None";
    init.code.instructions = {
      { FunctionCode::kOp_Assign, { Id("prefixes"), VarValue(0) } },
      { FunctionCode::kOp_Assign, { Id("suffixes"), VarValue(0) } },
      { FunctionCode::kOp_Return, { VarValue::None() } }
    };
    FunctionInfo enter;
    enter.valid = true;
    enter.returnType = "None";
    enter.params = { { "source", "ObjectReference" } };
    enter.locals = { { "temp", "None" }, { "captured", "Int" } };
    enter.code.instructions = {
      { FunctionCode::kOp_IAdd,
        { Id("prefixes"), Id("prefixes"), VarValue(1) } },
      { FunctionCode::kOp_Assign, { Id("captured"), Id("prefixes") } },
      { FunctionCode::kOp_CallStatic,
        { VarValue("Utility"), VarValue("Wait"), Id("temp"), VarValue(1),
          VarValue(1.0) } },
      { FunctionCode::kOp_IAdd,
        { Id("suffixes"), Id("suffixes"), Id("captured") } },
      { FunctionCode::kOp_Return, { VarValue::None() } }
    };
    object.states = {
      { "", { { "OnInit", init }, { "OnTriggerEnter", enter } } }
    };
    script->objectTable.push_back(std::move(object));
    sources.push_back(std::move(script));
    for (const auto& [name, parent] :
         std::vector<std::pair<std::string, std::string>>{
           { "Form", "" },
           { "ObjectReference", "Form" },
           { "Actor", "ObjectReference" },
           { "Quest", "Form" } }) {
      auto base = std::make_shared<PexScript>();
      base->source = name;
      Object native;
      native.NameIndex = name;
      native.parentClassName = parent;
      base->objectTable.push_back(std::move(native));
      sources.push_back(std::move(base));
    }
  }
  std::vector<PexScript::Lazy> Scripts() const
  {
    std::vector<PexScript::Lazy> result;
    for (const auto& source : sources)
      result.push_back({ source->source, [source] { return source; } });
    return result;
  }
};

struct BuilderStorage
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
    static const std::string name = "admission-builder";
    return name;
  }
};

struct BuilderFixture
{
  BuilderProgram program;
  PartOne server;
  std::shared_ptr<BuilderStorage> storage = std::make_shared<BuilderStorage>();
  std::shared_ptr<QuestJobTime> time =
    std::make_shared<QuestJobTime>(QuestJobTime{ 1000000, 5000 });
  BuilderFixture()
  {
    server.AttachEspm(&GetEspmLoader());
    server.AttachSaveStorage(storage);
    server.CreateActor(kOwner, kStart, 0, 0x3c);
    Actor().SetWorldInstance("campaign:" + std::to_string(kOwner) +
                             ":builder");
    Actor().SetInventory(Inventory().AddItem(0xf, 10));
    auto& world = server.worldState;
    const auto record = world.GetEspm().GetBrowser().LookupById(kTrigger);
    REQUIRE(record.rec);
    const auto data = reinterpret_cast<const espm::REFR*>(record.rec)
                        ->GetData(world.GetEspmCache());
    REQUIRE(data.boundsDiv2);
    const auto base = record.ToGlobalId(data.baseId);
    const auto type =
      world.GetEspm().GetBrowser().LookupById(base).rec->GetType().ToString();
    {
      WorldState::RestorationScope restoring(world);
      world.AddForm(std::make_unique<MpObjectReference>(
                      LocationalData{ kStart, {}, FormDesc::Tamriel() },
                      FormCallbacks::DoNothing(), base, type.c_str(),
                      NiPoint3{ data.boundsDiv2[0], data.boundsDiv2[1],
                                data.boundsDiv2[2] }),
                    kOwner + 1);
      Target().SetWorldInstance(Actor().GetWorldInstance());
      Target().SetInventory(Inventory());
      CampaignReferenceBinding::Bind(Actor(), kTrigger, Target());
    }
    QuestScriptRuntime::ExecuteReferenceLoad(Actor(), kTrigger,
                                             program.Scripts());
    Actor().ForceSubscriptionsUpdate();
    Tick();
    Jobs().SetClock([time = time] { return *time; });
  }
  MpActor& Actor() { return server.worldState.GetFormAt<MpActor>(kOwner); }
  MpObjectReference& Target()
  {
    return server.worldState.GetFormAt<MpObjectReference>(kOwner + 1);
  }
  QuestJobRunner& Jobs() { return server.worldState.GetQuestJobs(); }
  void Tick() { server.Tick(); }
  Status Drain(const Request& request)
  {
    for (size_t step = 0; step < 64; ++step) {
      const auto result = Jobs().Inspect(Actor(), request);
      if (result.status == Status::Pending)
        Tick();
      else if (result.status == Status::Ready)
        Jobs().Advance(Actor(), request, QuestJobRunner::MetadataOverrides{},
                       program.Scripts());
      else
        return result.status;
    }
    FAIL("Admission fixture failed to reach a wait or completion");
    return Status::Failed;
  }
  Request Submit(const std::string& event)
  {
    const auto id = Jobs().NextOccurrence(Actor());
    const auto occurrence =
      Jobs().CaptureOccurrence(Actor(), id, Target(), event, Actor());
    const auto admitted = Jobs().AdmitOccurrences(
      Actor(), { occurrence }, QuestJobRunner::MetadataOverrides{},
      program.Scripts());
    REQUIRE(admitted.results.size() == 1);
    REQUIRE(admitted.results.front().request);
    const auto request = *admitted.results.front().request;
    return request;
  }
  Request Sleep()
  {
    const auto request = Submit("OnTriggerEnter");
    REQUIRE(Drain(request) == Status::Sleeping);
    REQUIRE(QuestJournal::ScriptState(Actor()).at("continuations").size() ==
            1);
    return request;
  }
  Json Saved() { return MpChangeForm::ToJson(Actor().GetChangeForm()); }
  std::string Manifest()
  {
    return QuestJobContent::ScriptManifest(program.Scripts());
  }
  std::vector<QuestJobState::Job> Roots(size_t count)
  {
    const auto occurrence = Jobs().NextOccurrence(Actor());
    const auto state = QuestJournal::JobState(Actor());
    std::vector<QuestJobState::Job> result;
    for (size_t index = 0; index < count; ++index) {
      QuestJobState::Job job;
      job.id = state.nextJob + index;
      job.generation = state.generation;
      job.owner = occurrence.owner;
      job.actor = Actor().GetChangeForm().formDesc.ToString();
      job.instance = Actor().GetWorldInstance();
      job.command = "quest-request:" + std::to_string(job.generation) + ":" +
        std::to_string(job.id);
      job.sequenced = true;
      job.kind = QuestJobState::Kind::Interaction;
      job.cursor = "done";
      job.interactionDone = false;
      job.interaction = QuestScriptRuntime::CaptureInteraction(
        Actor(), Target(), "OnTriggerLeave", Actor());
      job.occurrence = occurrence.sequence + index;
      job.contentMode = "fixture";
      job.environmentHash =
        QuestJobContent::Environment(server.worldState, Manifest());
      result.push_back(std::move(job));
    }
    return result;
  }
  void Capture(QuestJobState::Job& job)
  {
    QuestScriptRuntime::ExecuteJobSegment(Actor(), job, {}, program.Scripts(),
                                          {}, true, {}, Manifest(),
                                          [time = time] { return *time; });
    ++job.segment;
  }
  template <class Operation>
  void RejectWithoutPublication(const Operation& operation)
  {
    const auto before = Saved();
    const auto targetBefore = MpChangeForm::ToJson(Target().GetChangeForm());
    const auto emitters = Actor().GetEmitters();
    const auto listeners = Target().GetListeners();
    const auto submitted = storage->submissions;
    REQUIRE_THROWS(operation());
    REQUIRE(Saved() == before);
    REQUIRE(MpChangeForm::ToJson(Target().GetChangeForm()) == targetBefore);
    REQUIRE(Actor().GetEmitters() == emitters);
    REQUIRE(Target().GetListeners() == listeners);
    Tick();
    REQUIRE(storage->submissions == submitted);
    REQUIRE(Saved() == before);
  }
};
}

TEST_CASE("Admission builder validates every pristine root before preparation",
          "[QuestAdmissionBuilder][QuestIngress][Rollback]")
{
  BuilderFixture f;
  f.Sleep();
  auto roots = f.Roots(3);
  roots[1].id += 100;
  bool prepared = false;
  size_t captures = 0;
  f.RejectWithoutPublication([&] {
    QuestJournal::AtomicJobAdmissionRoots(
      f.Actor(), roots, [&] { prepared = true; },
      [&](QuestJobState::Job& job) {
        ++captures;
        f.Capture(job);
      });
  });
  REQUIRE_FALSE(prepared);
  REQUIRE(captures == 0);
}

TEST_CASE(
  "A middle runtime capture failure discards the entire admission batch",
  "[QuestAdmissionBuilder][QuestIngress][Rollback]")
{
  BuilderFixture f;
  f.Sleep();
  auto roots = f.Roots(3);
  // The envelope remains well formed, but the registered private target has
  // a different physical identity. This fails actual runtime validation.
  roots[1].interaction->targetReference =
    f.Actor().GetChangeForm().formDesc.ToString();
  size_t captures = 0;
  f.RejectWithoutPublication([&] {
    QuestJournal::AtomicJobAdmissionRoots(
      f.Actor(), roots, [] {},
      [&](QuestJobState::Job& job) {
        ++captures;
        f.Capture(job);
      });
  });
  REQUIRE(captures == 2);
}

TEST_CASE("An admission callback cannot rewrite earlier roots or retirement",
          "[QuestAdmissionBuilder][QuestIngress][Rollback]")
{
  BuilderFixture f;
  const auto completed = f.Submit("OnTriggerLeave");
  REQUIRE(f.Drain(completed) == Status::Completed);
  const auto sleeping = f.Sleep();
  const auto roots = f.Roots(2);
  enum class Change
  {
    Sleeping,
    EarlierAdmission,
    Retirement,
    EarlierOutput,
    EarlierOutputIdentity
  };
  const auto change =
    GENERATE(Change::Sleeping, Change::EarlierAdmission, Change::Retirement,
             Change::EarlierOutput, Change::EarlierOutputIdentity);
  size_t captures = 0;
  QuestJobState::Job* earlierOutput = nullptr;
  f.RejectWithoutPublication([&] {
    QuestJournal::AtomicJobAdmissionRoots(
      f.Actor(), roots, [] {},
      [&](QuestJobState::Job& job) {
        f.Capture(job);
        ++captures;
        if (captures != 2) {
          earlierOutput = &job;
          return;
        }
        if (change == Change::EarlierOutput ||
            change == Change::EarlierOutputIdentity) {
          // Builder output references remain valid during this call. Their
          // mutation still cannot change an already accepted snapshot.
          REQUIRE(earlierOutput);
          if (change == Change::EarlierOutputIdentity)
            *earlierOutput = job;
          else
            earlierOutput->automatic = true;
          return;
        }
        auto state = QuestJournal::JobState(f.Actor());
        if (change == Change::Sleeping)
          state.live.at(sleeping.sequence).automatic = true;
        else if (change == Change::EarlierAdmission)
          state.live.at(roots.front().id).automatic = true;
        else {
          REQUIRE(state.completed.size() == 1);
          state.completed.clear();
          state.retiredThrough = completed.sequence;
          state.ingress.retiredThrough = 1;
        }
        REQUIRE_NOTHROW(state.Save());
        QuestJournal::SetJobState(f.Actor(), state);
      });
  });
  REQUIRE(captures == 2);
}

TEST_CASE("Admission cannot enter runtime twice for the same root",
          "[QuestAdmissionBuilder][QuestIngress][Rollback]")
{
  BuilderFixture f;
  f.Sleep();
  const auto roots = f.Roots(1);
  bool firstCaptured = false;
  f.RejectWithoutPublication([&] {
    QuestJournal::AtomicJobAdmissionRoots(
      f.Actor(), roots, [] {},
      [&](QuestJobState::Job& job) {
        f.Capture(job);
        firstCaptured = true;
        f.Capture(job);
      });
  });
  REQUIRE(firstCaptured);
}

TEST_CASE("Admission getters return copies while old continuations survive",
          "[QuestAdmissionBuilder][QuestIngress]")
{
  BuilderFixture f;
  const auto sleeping = f.Sleep();
  const auto prior = QuestJournal::JobState(f.Actor());
  const auto heap = QuestJournal::ScriptState(f.Actor());
  const auto roots = f.Roots(3);
  const auto committed = QuestJournal::AtomicJobAdmissionRoots(
    f.Actor(), roots, [] {},
    [&](QuestJobState::Job& job) {
      auto stateCopy = QuestJournal::JobState(f.Actor());
      stateCopy.live.at(sleeping.sequence).automatic = true;
      stateCopy.nextJob += 100;
      auto heapCopy = QuestJournal::ScriptState(f.Actor());
      heapCopy["continuations"] = Json::object();
      heapCopy["scripts"] = Json::object();
      f.Capture(job);
    });
  REQUIRE(committed.receipt);
  REQUIRE(committed.receipt->GetState() == SaveReceipt::State::Pending);
  REQUIRE(committed.retiredIds.empty());
  const auto saved = QuestJournal::JobState(f.Actor());
  REQUIRE(saved.nextJob == prior.nextJob + roots.size());
  REQUIRE(saved.live.at(sleeping.sequence) ==
          prior.live.at(sleeping.sequence));
  REQUIRE(QuestJournal::ScriptState(f.Actor()).at("continuations") ==
          heap.at("continuations"));
  for (const auto& root : roots) {
    const auto& captured = saved.live.at(root.id);
    REQUIRE(captured.dispatch);
    REQUIRE_FALSE(captured.dispatch->captured);
    REQUIRE_FALSE(captured.handler);
    REQUIRE(captured.segment == 1);
  }
  for (size_t step = 0; step < 4 &&
       committed.receipt->GetState() == SaveReceipt::State::Pending;
       ++step)
    f.Tick();
  REQUIRE(committed.receipt->GetState() == SaveReceipt::State::Durable);
  f.time->unixMs += 1000;
  f.time->steadyMs += 1000;
  REQUIRE(f.Drain(sleeping) == Status::Completed);
  REQUIRE(QuestJournal::ScriptState(f.Actor())
            .at("scripts")
            .at("reference:6debb:Skyrim.esm/defaultsetstagetrigscript")
            .at("values")
            .at("defaultsetstagetrigscript/suffixes")
            .at("value") == 1);
}

TEST_CASE("Admission accepts only the recipient snapshot returned by runtime",
          "[QuestAdmissionBuilder][QuestIngress][Rollback]")
{
  enum class Fabrication
  {
    NoRuntime,
    FailedRuntime,
    ChangedRoster
  };
  const auto fabrication =
    GENERATE(Fabrication::NoRuntime, Fabrication::FailedRuntime,
             Fabrication::ChangedRoster);
  BuilderFixture f;
  f.Sleep();
  auto roots = f.Roots(1);
  if (fabrication == Fabrication::FailedRuntime)
    roots.front().interaction->targetReference =
      f.Actor().GetChangeForm().formDesc.ToString();
  bool reachedValidFabrication = false;
  f.RejectWithoutPublication([&] {
    QuestJournal::AtomicJobAdmissionRoots(
      f.Actor(), roots, [] {},
      [&](QuestJobState::Job& job) {
        if (fabrication == Fabrication::ChangedRoster) {
          f.Capture(job);
          REQUIRE(job.dispatch);
          REQUIRE(job.dispatch->recipients.size() == 1);
          job.dispatch->recipients.push_back({ { "kind", "alias" },
                                               { "quest", "4e50d:Skyrim.esm" },
                                               { "id", 3 } });
        } else {
          if (fabrication == Fabrication::FailedRuntime) {
            bool caught = false;
            try {
              f.Capture(job);
            } catch (const std::exception&) {
              caught = true;
            }
            REQUIRE(caught);
          }
          job = roots.front();
          job.dispatch = QuestJobState::Dispatch{};
          job.dispatch->recipients = { job.interaction->target };
          job.segment = job.steps = job.instructions = 1;
        }
        // These snapshots are codec-valid. Admission must prove successful
        // runtime capture and preserve its exact roster, not merely its shape.
        REQUIRE(QuestJobState::ValidateJob(job) == job);
        reachedValidFabrication = true;
      });
  });
  REQUIRE(reachedValidFabrication);
}

TEST_CASE("Movement admission cannot omit its physical trigger occurrences",
          "[QuestAdmissionBuilder][QuestIngress][CampaignMovement][Rollback]")
{
  const bool legacy = GENERATE(false, true);
  BuilderFixture f;
  f.Sleep();
  const auto plan =
    f.Actor().PrepareCampaignMovement(f.Actor(), kFinish, { 0, 0, 30 });
  REQUIRE(plan->GetEvents().size() == 1);
  REQUIRE(plan->GetEvents().front().name == "OnTriggerLeave");
  bool captured = false;
  f.RejectWithoutPublication([&] {
    const auto prepare = [&] { QuestJournal::StageMovement(f.Actor(), plan); };
    if (legacy)
      QuestJournal::AtomicJobAdmissions(f.Actor(), {}, prepare,
                                        [&](uint64_t) { captured = true; });
    else
      QuestJournal::AtomicJobAdmissionRoots(
        f.Actor(), {}, prepare, [&](QuestJobState::Job&) { captured = true; });
  });
  REQUIRE_FALSE(captured);
  REQUIRE(f.Actor().GetPos() == kStart);
  REQUIRE(f.Actor().GetDynamicFields().GetValueDump(
            CampaignMovementPlan::kProperty) == "null");
}

TEST_CASE("A corrupt admission heap cannot publish staged physical movement",
          "[QuestAdmissionBuilder][QuestIngress][CampaignMovement][Rollback]")
{
  BuilderFixture f;
  f.Sleep();
  const auto roots = f.Roots(1);
  const auto plan =
    f.Actor().PrepareCampaignMovement(f.Actor(), kFinish, { 0, 0, 30 });
  REQUIRE(plan->GetEvents().size() == 1);
  REQUIRE(plan->GetEvents().front().name == "OnTriggerLeave");
  REQUIRE(plan->GetEvents().front().target.get() == &f.Target());
  REQUIRE(roots.front().interaction->name == plan->GetEvents().front().name);
  bool captured = false;
  f.RejectWithoutPublication([&] {
    QuestJournal::AtomicJobAdmissionRoots(
      f.Actor(), roots, [&] { QuestJournal::StageMovement(f.Actor(), plan); },
      [&](QuestJobState::Job& job) {
        f.Capture(job);
        captured = true;
        auto heap = QuestJournal::ScriptState(f.Actor());
        heap["continuations"] = Json::object();
        QuestJournal::SetScriptState(f.Actor(), heap);
      });
  });
  REQUIRE(captured);
  REQUIRE(f.Actor().GetPos() == kStart);
  REQUIRE(f.Actor().GetDynamicFields().GetValueDump(
            CampaignMovementPlan::kProperty) == "null");
}
