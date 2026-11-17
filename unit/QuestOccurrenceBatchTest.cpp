#include "CampaignReferenceBinding.h"
#include "QuestJobRunner.h"
#include "QuestJournal.h"
#include "QuestScriptRuntime.h"
#include "QuestVmState.h"
#include "TestUtils.hpp"
#include "libespm/REFR.h"
#include "save_storages/ISaveStorage.h"
#include <algorithm>
#include <exception>
#include <set>

extern espm::Loader& GetEspmLoader();

namespace {
using Json = nlohmann::json;
using Forms = std::vector<std::optional<MpChangeForm>>;
using Status = QuestJobRunner::Status;
using Admission = QuestJobRunner::AdmissionStatus;
using Request = QuestJobRunner::RequestId;
using Occurrence = QuestJobRunner::CapturedOccurrence;
constexpr uint32_t kOwner = 0xff009100, kGuest = kOwner + 1;
constexpr uint32_t kTrigger = 0x6debb, kItem = 0x8a986, kQuest = 0x4e50d;
constexpr auto kScriptState =
  "reference:6debb:Skyrim.esm/defaultsetstagetrigscript";

VarValue Id(const char* name)
{
  return VarValue(VarValue::kType_Identifier, name);
}

struct BatchProgram
{
  QuestJobRunner::MetadataOverrides metadata;
  std::vector<std::shared_ptr<PexScript>> scripts;
  explicit BatchProgram(bool waits)
  {
    auto trigger = std::make_shared<PexScript>();
    trigger->source = "defaultSetStageTRIGScript";
    Object object;
    object.NameIndex = trigger->source;
    object.parentClassName = "ObjectReference";
    object.variables = { { "::stage_var", "Int", 0, VarValue(0) },
                         { "prefixes", "Int", 0, VarValue(0) },
                         { "suffixes", "Int", 0, VarValue(0) },
                         { "leaves", "Int", 0, VarValue(0) },
                         { "released", "Bool", 0, VarValue(false) } };
    FunctionInfo init;
    init.valid = true;
    init.returnType = "None";
    init.code.instructions = {
      { FunctionCode::kOp_Assign, { Id("prefixes"), VarValue(0) } },
      { FunctionCode::kOp_Assign, { Id("suffixes"), VarValue(0) } },
      { FunctionCode::kOp_Assign, { Id("leaves"), VarValue(0) } },
      { FunctionCode::kOp_Assign, { Id("released"), VarValue(false) } },
      { FunctionCode::kOp_Return, { VarValue::None() } }
    };
    FunctionInfo enter;
    enter.valid = true;
    enter.returnType = "None";
    enter.params = { { "source", "ObjectReference" } };
    enter.locals = { { "player", "ObjectReference" },
                     { "gold", "Form" },
                     { "temp", "None" },
                     { "ordinal", "Int" } };
    enter.code.instructions = {
      { FunctionCode::kOp_CallStatic,
        { VarValue("Game"), VarValue("GetForm"), Id("player"), VarValue(1),
          VarValue(0x14) } },
      { FunctionCode::kOp_CallStatic,
        { VarValue("Game"), VarValue("GetForm"), Id("gold"), VarValue(1),
          VarValue(0xf) } },
      { FunctionCode::kOp_CallMethod,
        { VarValue("AddItem"), Id("player"), Id("temp"), VarValue(3),
          Id("gold"), VarValue(1), VarValue(true) } },
      { FunctionCode::kOp_IAdd,
        { Id("prefixes"), Id("prefixes"), VarValue(1) } },
      { FunctionCode::kOp_Assign, { Id("ordinal"), Id("prefixes") } }
    };
    if (waits) {
      enter.code.instructions.push_back(
        { FunctionCode::kOp_CallStatic,
          { VarValue("Utility"), VarValue("Wait"), Id("temp"), VarValue(1),
            VarValue(1.0) } });
      enter.code.instructions.push_back(
        { FunctionCode::kOp_JumpF, { Id("released"), VarValue(-1) } });
    }
    enter.code.instructions.insert(
      enter.code.instructions.end(),
      { { FunctionCode::kOp_IAdd,
          { Id("suffixes"), Id("suffixes"), VarValue(1) } },
        { FunctionCode::kOp_CallMethod,
          { VarValue("AddItem"), Id("player"), Id("temp"), VarValue(3),
            Id("gold"), VarValue(2), VarValue(true) } },
        { FunctionCode::kOp_Return, { VarValue::None() } } });
    FunctionInfo leave;
    leave.valid = true;
    leave.returnType = "None";
    leave.params = enter.params;
    leave.code.instructions = {
      { FunctionCode::kOp_IAdd, { Id("leaves"), Id("leaves"), VarValue(1) } },
      { FunctionCode::kOp_Assign, { Id("released"), VarValue(true) } },
      { FunctionCode::kOp_Return, { VarValue::None() } }
    };
    object.states = { { "",
                        { { "OnInit", init },
                          { "OnTriggerEnter", enter },
                          { "OnTrigger", enter },
                          { "OnTriggerLeave", leave } } } };
    trigger->objectTable.push_back(std::move(object));
    scripts.push_back(std::move(trigger));

    auto alias = std::make_shared<PexScript>();
    alias->source = "BatchAlias";
    Object observer;
    observer.NameIndex = alias->source;
    observer.parentClassName = "ReferenceAlias";
    observer.variables = { { "calls", "Int", 0, VarValue(0) } };
    auto observed = leave;
    observed.code.instructions = {
      { FunctionCode::kOp_IAdd, { Id("calls"), Id("calls"), VarValue(1) } },
      { FunctionCode::kOp_Return, { VarValue::None() } }
    };
    observer.states = { { "", { { "OnTriggerEnter", observed } } } };
    alias->objectTable.push_back(std::move(observer));
    scripts.push_back(std::move(alias));

    espm::QuestScriptData data;
    data.version = 5;
    data.objectFormat = 2;
    espm::QuestScriptData::Alias binding;
    binding.object.type = 1;
    binding.object.formId = kQuest;
    binding.object.alias = 3;
    binding.version = 5;
    binding.objectFormat = 2;
    binding.scripts = { { "BatchAlias", 0, {} } };
    data.aliases = { binding };
    metadata.emplace(kQuest, std::move(data));
    for (const auto& [name, parent] :
         std::vector<std::pair<std::string, std::string>>{
           { "Form", "" },
           { "Quest", "Form" },
           { "ObjectReference", "Form" },
           { "Actor", "ObjectReference" },
           { "MiscObject", "Form" },
           { "Alias", "" },
           { "ReferenceAlias", "Alias" } }) {
      auto base = std::make_shared<PexScript>();
      base->source = name;
      Object baseObject;
      baseObject.NameIndex = name;
      baseObject.parentClassName = parent;
      base->objectTable.push_back(std::move(baseObject));
      scripts.push_back(std::move(base));
    }
  }
  std::vector<PexScript::Lazy> Scripts() const
  {
    std::vector<PexScript::Lazy> result;
    for (const auto& script : scripts)
      result.push_back({ script->source, [script] { return script; } });
    return result;
  }
};

struct BatchStorage
  : Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>
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
    if (!error)
      ++finished;
  }
  uint32_t GetNumFinishedUpserts() const override { return finished; }
  uint32_t GetNumFinishedIterates() const override { return 0; }
  void Tick() override {}
  bool GetRecycledChangeFormsBuffer(Forms&) override { return false; }
  const std::string& GetName() const override
  {
    static const std::string name = "occurrence-batches";
    return name;
  }
};

Json Serialized(const Forms& forms)
{
  auto result = Json::array();
  for (const auto& form : forms)
    result.push_back(form ? MpChangeForm::ToJson(*form) : Json(nullptr));
  return result;
}

struct BatchFixture
{
  BatchProgram program;
  PartOne server;
  std::shared_ptr<BatchStorage> storage = std::make_shared<BatchStorage>();
  std::shared_ptr<QuestJobTime> time =
    std::make_shared<QuestJobTime>(QuestJobTime{ 1000000, 5000 });
  explicit BatchFixture(bool waits = false)
    : program(waits)
  {
    server.AttachEspm(&GetEspmLoader());
    server.AttachSaveStorage(storage);
    server.CreateActor(kOwner, { 1, 2, 3 }, 0, 0x3c);
    server.CreateActor(kGuest, { 1, 2, 3 }, 0, 0x3c);
    Owner().SetPropertyValueDump(QuestJournal::kEnabled,
                                 "[\"4e50d:Skyrim.esm\"]", false, false);
    const auto phase = "campaign:" + std::to_string(kOwner) + ":batch";
    Owner().SetWorldInstance(phase);
    Guest().SetWorldInstance(phase);
    Owner().SetInventory(Inventory().AddItem(0xf, 10));
    Guest().SetInventory(Inventory().AddItem(0xf, 77));
    Guest().SetPropertyValueDump("private.batch.personal", "123", false,
                                 false);
    Bind(kTrigger, kOwner + 2);
    Bind(kItem, kOwner + 3);
    for (const auto original : { kTrigger, kItem })
      QuestScriptRuntime::ExecuteReferenceLoad(Owner(), original,
                                               program.Scripts());
    // Start the real MQ102 alias only after setup loads. Event admission uses
    // the fixture metadata, preserving its owned reference-alias definition.
    QuestJournal::Apply(Owner(), { { "quest", kQuest }, { "op", "start" } });
    QuestJournal::Atomic(Owner(), [&] {
      QuestVmState heap(QuestJournal::ScriptState(Owner()), {}, {});
      heap.SetAliasBinding("4e50d:Skyrim.esm/alias:3", "6debb:Skyrim.esm");
      QuestJournal::SetScriptState(Owner(), heap.Save());
    });
    Tick();
    storage->submissions.clear();
    storage->automatic = false;
    Jobs().SetClock([time = time] { return *time; });
  }
  MpActor& Owner() { return server.worldState.GetFormAt<MpActor>(kOwner); }
  MpActor& Guest() { return server.worldState.GetFormAt<MpActor>(kGuest); }
  QuestJobRunner& Jobs() { return server.worldState.GetQuestJobs(); }
  void Tick() { server.worldState.Tick(); }
  void Bind(uint32_t original, uint32_t copyId)
  {
    auto& world = server.worldState;
    const auto record = world.GetEspm().GetBrowser().LookupById(original);
    REQUIRE(record.rec);
    REQUIRE(record.rec->GetType() == "REFR");
    const auto data = reinterpret_cast<const espm::REFR*>(record.rec)
                        ->GetData(world.GetEspmCache());
    const auto base = record.ToGlobalId(data.baseId);
    const auto type =
      world.GetEspm().GetBrowser().LookupById(base).rec->GetType().ToString();
    world.AddForm(std::make_unique<MpObjectReference>(
                    LocationalData{ { 1, 2, 3 }, {}, FormDesc::Tamriel() },
                    FormCallbacks::DoNothing(), base, type.c_str()),
                  copyId);
    auto& copy = world.GetFormAt<MpObjectReference>(copyId);
    copy.SetWorldInstance(Owner().GetWorldInstance());
    copy.SetInventory(Inventory());
    CampaignReferenceBinding::Bind(Owner(), original, copy);
  }
  std::vector<Occurrence> Capture(
    MpActor& actor,
    const std::vector<std::pair<uint32_t, std::string>>& events)
  {
    auto next = Jobs().NextOccurrence(actor);
    std::vector<Occurrence> result;
    for (const auto& [original, event] : events) {
      result.push_back(Jobs().CaptureOccurrence(
        actor, next, CampaignReferenceBinding::Resolve(actor, original), event,
        actor));
      ++next.sequence;
    }
    return result;
  }
  std::vector<Occurrence> Capture(size_t count, uint32_t target = kItem,
                                  const std::string& event = "OnTriggerLeave")
  {
    return Capture(
      Owner(),
      std::vector<std::pair<uint32_t, std::string>>(count, { target, event }));
  }
  QuestJobRunner::AdmissionBatchResult Admit(
    MpActor& actor, const std::vector<Occurrence>& events)
  {
    return Jobs().AdmitOccurrences(actor, events, program.metadata,
                                   program.Scripts());
  }
  QuestJobRunner::AdmissionBatchResult Admit(
    const std::vector<Occurrence>& events)
  {
    return Admit(Owner(), events);
  }
  void Ack()
  {
    Tick();
    REQUIRE_FALSE(storage->submissions.empty());
    storage->Complete(storage->submissions.size() - 1);
    Tick();
  }
  Status Drain(MpActor& actor, const Request& request)
  {
    for (size_t step = 0; step < 96; ++step) {
      const auto result = Jobs().Inspect(actor, request);
      if (result.status == Status::Pending)
        Ack();
      else if (result.status == Status::Ready) {
        const auto advanced =
          Jobs().Advance(actor, request, program.metadata, program.Scripts());
        REQUIRE(advanced.status == Status::Pending);
      } else
        return result.status;
    }
    FAIL("Batch occurrence failed to reach a wait or completion");
    return Status::Failed;
  }
  Status Drain(const Request& request) { return Drain(Owner(), request); }
  Json Field(const char* name)
  {
    return QuestJournal::ScriptState(Owner())
      .at("scripts")
      .at(kScriptState)
      .at("values")
      .at(std::string("defaultsetstagetrigscript/") + name)
      .at("value");
  }
  uint32_t Gold() { return Owner().GetInventory().GetItemCount(0xf); }
};

std::vector<Request> PendingRequests(
  const QuestJobRunner::AdmissionBatchResult& result, size_t count)
{
  REQUIRE(result.status == Admission::Pending);
  REQUIRE(result.receipt);
  REQUIRE(result.receipt->GetState() == SaveReceipt::State::Pending);
  REQUIRE(result.results.size() == count);
  std::vector<Request> requests;
  for (const auto& entry : result.results) {
    REQUIRE(entry.status == Admission::Pending);
    REQUIRE(entry.error.empty());
    REQUIRE(entry.request);
    requests.push_back(*entry.request);
  }
  return requests;
}
}

TEST_CASE("A multi-target occurrence batch has one exact admission receipt",
          "[QuestOccurrenceBatch][QuestIngress][SaveReceipt]")
{
  const bool guest = GENERATE(false, true);
  BatchFixture f;
  auto& actor = guest ? f.Guest() : f.Owner();
  const auto guestBefore = MpChangeForm::ToJson(f.Guest().GetChangeForm());
  const auto before = QuestJournal::JobState(f.Owner());
  const auto heap = QuestJournal::ScriptState(f.Owner());
  const auto events = f.Capture(
    actor, { { kTrigger, "OnTriggerEnter" }, { kItem, "OnTriggerLeave" } });
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == before.Save());
  const auto admitted = f.Admit(actor, events);
  const auto requests = PendingRequests(admitted, 2);
  REQUIRE(requests[0].sequence == before.nextJob);
  REQUIRE(requests[1].sequence == before.nextJob + 1);
  // Capturing aliases may materialize their lazy variable holders, but it
  // cannot run a handler or alter any existing continuation.
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations") ==
          heap.at("continuations"));
  REQUIRE(f.Field("prefixes") == 0);
  REQUIRE(f.Field("suffixes") == 0);
  const auto admittedHeap = QuestJournal::ScriptState(f.Owner());
  REQUIRE(f.Gold() == 10);
  const auto jobs = QuestJournal::JobState(f.Owner());
  REQUIRE(jobs.ingress.next == events.back().id.sequence + 1);
  REQUIRE(jobs.nextJob == before.nextJob + 2);
  for (size_t i = 0; i < events.size(); ++i) {
    const auto* job = jobs.FindLive(requests[i].sequence);
    REQUIRE(job);
    REQUIRE(job->interaction == events[i].interaction);
    REQUIRE(job->actor == actor.GetChangeForm().formDesc.ToString());
    REQUIRE(job->dispatch);
    REQUIRE_FALSE(job->dispatch->captured);
    REQUIRE_FALSE(job->handler);
    REQUIRE(job->children.empty());
    REQUIRE(job->events.empty());
    REQUIRE(job->completions.empty());
    REQUIRE(f.Jobs().InspectOccurrence(actor, events[i].id).status ==
            Admission::Pending);
    REQUIRE(f.Jobs().Inspect(actor, requests[i]).status == Status::Pending);
  }
  const auto& recipients =
    jobs.FindLive(requests.front().sequence)->dispatch->recipients;
  REQUIRE(std::find(recipients.begin(), recipients.end(),
                    Json{ { "kind", "alias" },
                          { "quest", "4e50d:Skyrim.esm" },
                          { "id", 3 } }) != recipients.end());
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  const auto immutable = Serialized(f.storage->submissions.front().forms);
  const auto& savedOwner =
    f.storage->submissions.front().forms.at(f.Owner().GetIdx());
  REQUIRE(savedOwner);
  const auto savedJournal = Json::parse(
    savedOwner->dynamicFields.GetValueDump(QuestJournal::kProperty));
  REQUIRE(QuestJobState::Read(savedJournal.at("jobs")).Save() == jobs.Save());
  const auto duplicate = f.Jobs().AdmitOccurrences(actor, events);
  REQUIRE(duplicate.status == Admission::Pending);
  REQUIRE(duplicate.results.size() == requests.size());
  for (size_t i = 0; i < requests.size(); ++i)
    REQUIRE(duplicate.results[i].request == requests[i]);
  f.storage->Complete(
    0, std::make_exception_ptr(std::runtime_error("disk unavailable")));
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 2);
  REQUIRE(Serialized(f.storage->submissions[1].forms) == immutable);
  for (const auto& event : events)
    REQUIRE(f.Jobs().InspectOccurrence(actor, event.id).status ==
            Admission::Pending);
  f.storage->Complete(0); // A stale success cannot acknowledge the retry.
  f.Tick();
  REQUIRE(admitted.receipt->GetState() == SaveReceipt::State::Pending);
  f.storage->Complete(1);
  f.Tick();
  REQUIRE(admitted.receipt->GetState() == SaveReceipt::State::Durable);
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == admittedHeap);
  for (const auto& event : events)
    REQUIRE(f.Jobs().InspectOccurrence(actor, event.id).status ==
            Admission::Durable);

  const auto later = f.Capture(actor, { { kItem, "OnTriggerLeave" } });
  PendingRequests(f.Admit(actor, later), 1);
  for (const auto& event : events)
    REQUIRE(f.Jobs().InspectOccurrence(actor, event.id).status ==
            Admission::Durable);
  REQUIRE(f.Jobs().InspectOccurrence(actor, later.front().id).status ==
          Admission::Pending);
  REQUIRE_THROWS(f.Jobs().Advance(actor, requests.front(), f.program.metadata,
                                  f.program.Scripts()));
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == admittedHeap);
  f.Ack();
  REQUIRE(f.Drain(actor, requests.front()) == Status::Completed);
  REQUIRE(f.Field("prefixes") == 1);
  REQUIRE(f.Field("suffixes") == 1);
  REQUIRE(f.Gold() == 13);
  REQUIRE(MpChangeForm::ToJson(f.Guest().GetChangeForm()) == guestBefore);
}

TEST_CASE("A bad final occurrence cannot partially consume a batch",
          "[QuestOccurrenceBatch][QuestIngress][SaveReceipt][Rollback]")
{
  const std::string corruption =
    GENERATE("physical", "identity", "gap", "duplicate");
  BatchFixture f;
  const auto events =
    f.Capture(f.Owner(),
              { { kTrigger, "OnTriggerEnter" }, { kItem, "OnTriggerLeave" } });
  auto bad = events;
  if (corruption == "physical")
    bad.back().interaction.targetReference = "ff009199";
  else if (corruption == "identity")
    bad.back().id.campaignIdentity = std::string(32, 'f');
  else if (corruption == "gap")
    ++bad.back().id.sequence;
  else
    bad.back().id.sequence = bad.front().id.sequence;
  const auto owner = MpChangeForm::ToJson(f.Owner().GetChangeForm());
  const auto guest = MpChangeForm::ToJson(f.Guest().GetChangeForm());
  const auto heap = QuestJournal::ScriptState(f.Owner());
  const auto next = f.Jobs().NextOccurrence(f.Owner());
  REQUIRE_THROWS(f.Admit(bad));
  REQUIRE(MpChangeForm::ToJson(f.Owner().GetChangeForm()) == owner);
  REQUIRE(MpChangeForm::ToJson(f.Guest().GetChangeForm()) == guest);
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == heap);
  REQUIRE(f.Jobs().NextOccurrence(f.Owner()) == next);
  f.Tick();
  REQUIRE(f.storage->submissions.empty());
  PendingRequests(f.Admit(events), 2);
  f.Ack();
  const auto mixed =
    std::vector<Occurrence>{ events.back(), f.Capture(1).front() };
  const auto acceptedState = QuestJournal::JobState(f.Owner()).Save();
  REQUIRE_THROWS(f.Admit(mixed));
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == acceptedState);
  auto conflicting = events;
  conflicting.back().interaction.name = "OnTriggerEnter";
  REQUIRE_THROWS(f.Jobs().AdmitOccurrences(f.Owner(), conflicting));
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == acceptedState);
}

TEST_CASE(
  "An empty occurrence batch seals durability without consuming identities",
  "[QuestOccurrenceBatch][QuestIngress][SaveReceipt]")
{
  BatchFixture f;
  const auto nextOccurrence = f.Jobs().NextOccurrence(f.Owner());
  const auto nextRequest = f.Jobs().NextRequest(f.Owner());
  const auto heap = QuestJournal::ScriptState(f.Owner());
  const auto admitted = f.Admit({});
  PendingRequests(admitted, 0);
  REQUIRE(f.Jobs().NextOccurrence(f.Owner()) == nextOccurrence);
  REQUIRE(f.Jobs().NextRequest(f.Owner()) == nextRequest);
  REQUIRE_FALSE(QuestJournal::JobState(f.Owner()).HasLive());
  REQUIRE(QuestJournal::ScriptState(f.Owner()) == heap);
  f.Ack();
  REQUIRE(admitted.receipt->GetState() == SaveReceipt::State::Durable);
  REQUIRE(f.storage->submissions.size() == 1);
}

TEST_CASE("Batch retirement keeps original admission durable and gates "
          "retired results",
          "[QuestOccurrenceBatch][QuestIngress][SaveReceipt][Retention]")
{
  BatchFixture f;
  const auto originals = f.Capture(32);
  const auto requests = PendingRequests(f.Admit(originals), originals.size());
  f.Ack();
  for (const auto& request : requests)
    REQUIRE(f.Drain(request) == Status::Completed);
  REQUIRE(QuestJournal::JobState(f.Owner()).completed.size() == 32);
  const auto newer = f.Capture(2);
  const auto admitted = f.Admit(newer);
  PendingRequests(admitted, 2);
  const auto state = QuestJournal::JobState(f.Owner());
  std::vector<size_t> retired;
  for (size_t i = 0; i < requests.size(); ++i)
    if (state.IsRetired(requests[i].sequence))
      retired.push_back(i);
  REQUIRE_FALSE(retired.empty());
  for (const auto index : retired) {
    REQUIRE(f.Jobs().Inspect(f.Owner(), requests[index]).status ==
            Status::Pending);
    const auto accepted =
      f.Jobs().InspectOccurrence(f.Owner(), originals[index].id);
    REQUIRE(accepted.status == Admission::Durable);
    REQUIRE_FALSE(accepted.request);
  }
  f.Tick();
  const auto attempt = f.storage->submissions.size() - 1;
  const auto immutable = Serialized(f.storage->submissions[attempt].forms);
  f.storage->Complete(
    attempt, std::make_exception_ptr(std::runtime_error("retry retirement")));
  f.Tick();
  REQUIRE(Serialized(f.storage->submissions.back().forms) == immutable);
  for (const auto index : retired)
    REQUIRE(f.Jobs().Inspect(f.Owner(), requests[index]).status ==
            Status::Pending);
  f.Ack();
  REQUIRE(admitted.receipt->GetState() == SaveReceipt::State::Durable);
  const auto opaque = originals.at(retired.front());
  f.server.worldState.DestroyForm(
    CampaignReferenceBinding::Resolve(f.Owner(), kItem).GetFormId());
  auto replay = opaque;
  replay.interaction.name = "invented-after-retirement";
  const auto consumed = f.Jobs().AdmitOccurrences(f.Owner(), { replay });
  REQUIRE(consumed.status == Admission::Durable);
  REQUIRE(consumed.results.size() == 1);
  REQUIRE_FALSE(consumed.results.front().request);
  REQUIRE(f.Gold() == 10);
  // A later receipt cannot revoke an older retired result.
  PendingRequests(f.Admit(f.Capture(1, kTrigger, "OnTriggerLeave")), 1);
  for (const auto index : retired)
    REQUIRE(f.Jobs().Inspect(f.Owner(), requests[index]).status ==
            Status::Retired);
}

TEST_CASE(
  "Twenty-one sleeping occurrence roots allow an independent later wake",
  "[QuestOccurrenceBatch][QuestIngress][QuestJobs][Timers]")
{
  BatchFixture f(true);
  const auto events = f.Capture(21, kTrigger, "OnTriggerEnter");
  const auto requests = PendingRequests(f.Admit(events), events.size());
  f.Ack();
  for (const auto& request : requests)
    REQUIRE(f.Drain(request) == Status::Sleeping);
  REQUIRE(f.Field("prefixes") == 21);
  REQUIRE(f.Field("suffixes") == 0);
  REQUIRE(f.Gold() == 31);
  const auto beforeWake =
    QuestJournal::ScriptState(f.Owner()).at("continuations");
  REQUIRE(beforeWake.size() == 21);
  std::set<int> ordinals;
  for (const auto& frame : beforeWake) {
    for (const auto& local : frame.at("locals"))
      if (local.at("name") == "ordinal")
        ordinals.insert(local.at("value").at("value").get<int>());
  }
  REQUIRE(ordinals.size() == 21);
  REQUIRE(*ordinals.begin() == 1);
  REQUIRE(*ordinals.rbegin() == 21);
  const auto beforeEmpty = QuestJournal::JobState(f.Owner()).Save();
  PendingRequests(f.Admit({}), 0);
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == beforeEmpty);
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations") ==
          beforeWake);
  f.Ack();
  const auto wake = f.Capture(1, kTrigger, "OnTriggerLeave");
  const auto wakeRequests = PendingRequests(f.Admit(wake), 1);
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations") ==
          beforeWake);
  REQUIRE(QuestJournal::JobState(f.Owner()).live.size() == 22);
  f.Ack();
  REQUIRE(f.Drain(wakeRequests.front()) == Status::Completed);
  REQUIRE(f.Field("released") == true);
  REQUIRE(f.Field("leaves") == 1);
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations") ==
          beforeWake);
  for (const auto& request : requests)
    REQUIRE(f.Jobs().Inspect(f.Owner(), request).status == Status::Sleeping);
  f.time->unixMs += 1000;
  f.time->steadyMs += 1000;
  for (const auto& request : requests)
    REQUIRE(f.Drain(request) == Status::Completed);
  REQUIRE(f.Field("prefixes") == 21);
  REQUIRE(f.Field("suffixes") == 21);
  REQUIRE(f.Gold() == 73);
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations").empty());
  REQUIRE(QuestJournal::ScriptState(f.Owner())
            .at("scripts")
            .at("4e50d:Skyrim.esm/alias:3/batchalias")
            .at("values")
            .at("batchalias/calls")
            .at("value") == 21);
  REQUIRE_FALSE(QuestJournal::JobState(f.Owner()).HasLive());
  const auto duplicate = f.Jobs().AdmitOccurrences(f.Owner(), events);
  REQUIRE(duplicate.status == Admission::Durable);
  REQUIRE(f.Gold() == 73);
}

TEST_CASE("Completion retires older results under its own exact receipt",
          "[QuestOccurrenceBatch][QuestIngress][SaveReceipt][Retention]")
{
  BatchFixture f;
  const auto events = f.Capture(35);
  const auto requests = PendingRequests(f.Admit(events), events.size());
  f.Ack();
  // Leave the oldest and newest roots live while thirty-three later roots
  // finish. Completion, rather than another admission, must bound history.
  for (size_t i = 1; i <= 33; ++i)
    REQUIRE(f.Drain(requests[i]) == Status::Completed);
  REQUIRE(QuestJournal::JobState(f.Owner()).completed.size() == 32);
  REQUIRE(f.Jobs().Inspect(f.Owner(), requests[1]).status == Status::Retired);
  bool completed = false;
  for (size_t step = 0; step < 32; ++step) {
    REQUIRE(f.Jobs().Inspect(f.Owner(), requests[0]).status == Status::Ready);
    REQUIRE(f.Jobs()
              .Advance(f.Owner(), requests[0], f.program.metadata,
                       f.program.Scripts())
              .status == Status::Pending);
    if (QuestJournal::JobState(f.Owner()).FindCompleted(
          requests[0].sequence)) {
      completed = true;
      break;
    }
    f.Ack();
  }
  REQUIRE(completed);
  const auto pending = QuestJournal::JobState(f.Owner());
  REQUIRE(pending.FindCompleted(requests[0].sequence));
  REQUIRE(pending.IsRetired(requests[2].sequence));
  REQUIRE(pending.completed.size() == 32);
  REQUIRE(f.Jobs().Inspect(f.Owner(), requests[0]).status == Status::Pending);
  REQUIRE(f.Jobs().Inspect(f.Owner(), requests[2]).status == Status::Pending);
  REQUIRE(f.Jobs().Inspect(f.Owner(), requests[1]).status == Status::Retired);
  REQUIRE(f.Jobs().InspectOccurrence(f.Owner(), events[2].id).status ==
          Admission::Durable);
  f.Tick();
  const auto attempt = f.storage->submissions.size() - 1;
  const auto immutable = Serialized(f.storage->submissions[attempt].forms);
  f.storage->Complete(
    attempt,
    std::make_exception_ptr(std::runtime_error("completion save failed")));
  f.Tick();
  REQUIRE(Serialized(f.storage->submissions.back().forms) == immutable);
  REQUIRE(f.Jobs().Inspect(f.Owner(), requests[0]).status == Status::Pending);
  REQUIRE(f.Jobs().Inspect(f.Owner(), requests[2]).status == Status::Pending);
  REQUIRE(f.Jobs().Inspect(f.Owner(), requests[1]).status == Status::Retired);
  f.Ack();
  REQUIRE(f.Jobs().Inspect(f.Owner(), requests[0]).status ==
          Status::Completed);
  REQUIRE(f.Jobs().Inspect(f.Owner(), requests[2]).status == Status::Retired);
  REQUIRE(f.Jobs().Inspect(f.Owner(), requests[1]).status == Status::Retired);
  REQUIRE(f.Gold() == 10);
}

TEST_CASE("A batch cannot partially fill the last available occurrence slots",
          "[QuestOccurrenceBatch][QuestIngress][SaveReceipt][Capacity]")
{
  BatchFixture f;
  PendingRequests(f.Admit(f.Capture(64)), 64);
  f.Ack();
  PendingRequests(f.Admit(f.Capture(63)), 63);
  f.Ack();
  REQUIRE(QuestJournal::JobState(f.Owner()).live.size() == 127);
  const auto lastTwo = f.Capture(2);
  const auto before = MpChangeForm::ToJson(f.Owner().GetChangeForm());
  const auto submitted = f.storage->submissions.size();
  REQUIRE_THROWS(f.Admit(lastTwo));
  REQUIRE(MpChangeForm::ToJson(f.Owner().GetChangeForm()) == before);
  REQUIRE(f.Jobs().NextOccurrence(f.Owner()) == lastTwo.front().id);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == submitted);
  PendingRequests(f.Admit({ lastTwo.front() }), 1);
  f.Ack();
  REQUIRE(QuestJournal::JobState(f.Owner()).live.size() == 128);
  const auto full = QuestJournal::JobState(f.Owner()).Save();
  REQUIRE_THROWS(f.Admit({ lastTwo.back() }));
  REQUIRE(QuestJournal::JobState(f.Owner()).Save() == full);
  REQUIRE(f.Gold() == 10);
}

TEST_CASE(
  "Distinct level occurrences execute independently at an unchanged transform",
  "[QuestOccurrenceBatch][QuestIngress][LevelEvents]")
{
  BatchFixture f;
  auto& target = CampaignReferenceBinding::Resolve(f.Owner(), kTrigger);
  const auto sourcePosition = f.Owner().GetPos();
  const auto sourceAngle = f.Owner().GetAngle();
  const auto targetPosition = target.GetPos();
  const auto targetAngle = target.GetAngle();
  // Trusted callers supply two distinct occurrences. This verifies accepted
  // level work, not the native cadence or physical production of OnTrigger.
  const auto events = f.Capture(2, kTrigger, "OnTrigger");
  REQUIRE(events[0].id.sequence != events[1].id.sequence);
  REQUIRE(events[0].interaction == events[1].interaction);
  const auto requests = PendingRequests(f.Admit(events), 2);
  REQUIRE(f.Gold() == 10);
  f.Ack();
  for (const auto& request : requests)
    REQUIRE(f.Drain(request) == Status::Completed);
  REQUIRE(f.Field("prefixes") == 2);
  REQUIRE(f.Field("suffixes") == 2);
  REQUIRE(f.Gold() == 16);
  REQUIRE(f.Owner().GetPos() == sourcePosition);
  REQUIRE(f.Owner().GetAngle() == sourceAngle);
  REQUIRE(target.GetPos() == targetPosition);
  REQUIRE(target.GetAngle() == targetAngle);

  const auto journal =
    f.Owner().GetDynamicFields().GetValueDump(QuestJournal::kProperty);
  const auto submitted = f.storage->submissions.size();
  const auto replay = f.Jobs().AdmitOccurrences(f.Owner(), events);
  REQUIRE(replay.status == Admission::Durable);
  REQUIRE(replay.results.size() == requests.size());
  for (size_t i = 0; i < requests.size(); ++i) {
    REQUIRE(replay.results[i].status == Admission::Durable);
    REQUIRE(replay.results[i].request == requests[i]);
    REQUIRE(f.Jobs().Inspect(f.Owner(), requests[i]).status ==
            Status::Completed);
  }
  f.Tick();
  REQUIRE(f.storage->submissions.size() == submitted);
  REQUIRE(f.Owner().GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          journal);
  REQUIRE(f.Gold() == 16);
}
