#include "CampaignMovement.h"
#include "CampaignReferenceBinding.h"
#include "MessageSerializerFactory.h"
#include "PacketParser.h"
#include "QuestJobRunner.h"
#include "QuestJournal.h"
#include "QuestScriptRuntime.h"
#include "TestUtils.hpp"
#include "UpdateMovementMessage.h"
#include "libespm/REFR.h"
#include "save_storages/ISaveStorage.h"
#include "script_storages/IScriptStorage.h"
#include <algorithm>
#include <exception>
#include <map>
#include <set>
#include <slikenet/BitStream.h>
#include <tuple>

extern espm::Loader& GetEspmLoader();

namespace {
using Json = nlohmann::json;
using Forms = std::vector<std::optional<MpChangeForm>>;
constexpr uint32_t kOwner = 0xff009500, kGuest = kOwner + 1,
                   kObserver = kOwner + 2;
constexpr uint32_t kLeft = 0x6debb, kRight = 0x2b363;
constexpr uint32_t kLeftCopy = kOwner + 3, kRightCopy = kOwner + 4;
const NiPoint3 kStart{ 1000, 1000, 1000 }, kFinish{ 1600, 1000, 1000 };
const NiPoint3 kOutside{ 2500, 1000, 1000 };

// Minimal generated Skyrim PEX, read by the production Reader/storage path.
// Only the owned references' existing attachment and native type bases are
// supplied. No script bytes are extracted from the user's installation.
struct PexBytes
{
  std::vector<uint8_t> bytes;
  void U8(uint8_t value) { bytes.push_back(value); }
  void U16(uint16_t value)
  {
    U8(value >> 8);
    U8(value);
  }
  void U32(uint32_t value)
  {
    U16(value >> 16);
    U16(value);
  }
  void Text(const std::string& value)
  {
    U16(static_cast<uint16_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
  }
};

std::vector<uint8_t> ScriptBytes(const std::string& name,
                                 const std::string& parent, bool handlers)
{
  const std::vector<std::string> strings = { "",
                                             name,
                                             parent,
                                             "None",
                                             "Int",
                                             "ObjectReference",
                                             "Form",
                                             "::stage_var",
                                             "enters",
                                             "leaves",
                                             "levels",
                                             "OnInit",
                                             "OnTriggerEnter",
                                             "OnTriggerLeave",
                                             "OnTrigger",
                                             "source",
                                             "player",
                                             "gold",
                                             "temp",
                                             "Game",
                                             "GetForm",
                                             "AddItem" };
  const auto index = [&](const std::string& value) {
    const auto found = std::find(strings.begin(), strings.end(), value);
    REQUIRE(found != strings.end());
    return static_cast<uint16_t>(found - strings.begin());
  };
  PexBytes object;
  const auto word = [&](const std::string& value) {
    object.U16(index(value));
  };
  const auto id = [&](const char* value) {
    object.U8(1);
    word(value);
  };
  const auto text = [&](const char* value) {
    object.U8(2);
    word(value);
  };
  const auto integer = [&](int value) {
    object.U8(3);
    object.U32(value);
  };
  word(parent);
  word("");
  object.U32(0);
  word("");
  object.U16(handlers ? 4 : 0);
  if (handlers) {
    for (const auto* variable :
         { "::stage_var", "enters", "leaves", "levels" }) {
      word(variable);
      word("Int");
      object.U32(0);
      integer(0);
    }
  }
  object.U16(0); // Properties are supplied by the owned reference VMAD.
  object.U16(handlers ? 1 : 0);
  if (handlers) {
    word("");
    object.U16(4);
    for (const auto& [event, counter, reward] :
         std::vector<std::tuple<const char*, const char*, int>>{
           { "OnInit", "", 0 },
           { "OnTriggerEnter", "enters", 1 },
           { "OnTriggerLeave", "leaves", 2 },
           { "OnTrigger", "levels", 4 } }) {
      const bool init = reward == 0;
      word(event);
      word("None");
      word("");
      object.U32(0);
      object.U8(0);
      object.U16(init ? 0 : 1);
      if (!init) {
        word("source");
        word("ObjectReference");
      }
      object.U16(init ? 0 : 3);
      if (!init) {
        word("player");
        word("ObjectReference");
        word("gold");
        word("Form");
        word("temp");
        word("None");
      }
      object.U16(init ? 4 : 5);
      if (init) {
        for (const auto* variable : { "enters", "leaves", "levels" }) {
          object.U8(FunctionCode::kOp_Assign);
          id(variable);
          integer(0);
        }
      } else {
        object.U8(FunctionCode::kOp_IAdd);
        id(counter);
        id(counter);
        integer(1);
        for (const auto& [local, form] :
             std::vector<std::pair<const char*, int>>{ { "player", 0x14 },
                                                       { "gold", 0xf } }) {
          object.U8(FunctionCode::kOp_CallStatic);
          text("Game");
          text("GetForm");
          id(local);
          integer(1);
          integer(form);
        }
        object.U8(FunctionCode::kOp_CallMethod);
        text("AddItem");
        id("player");
        id("temp");
        integer(3);
        id("gold");
        integer(reward);
        object.U8(5);
        object.U8(1);
      }
      object.U8(FunctionCode::kOp_Return);
      object.U8(0);
    }
  }
  PexBytes output;
  output.U32(0xfa57c0de);
  output.U8(3);
  output.U8(2);
  output.U16(1);
  output.U32(0);
  output.U32(0);
  output.Text(name + ".psc");
  output.Text("");
  output.Text("");
  output.U16(static_cast<uint16_t>(strings.size()));
  for (const auto& value : strings)
    output.Text(value);
  output.U8(0);
  output.U16(0);
  output.U16(1);
  output.U16(index(name));
  output.U32(static_cast<uint32_t>(object.bytes.size() + 4));
  output.bytes.insert(output.bytes.end(), object.bytes.begin(),
                      object.bytes.end());
  return output.bytes;
}

class MovementScripts : public IScriptStorage
{
public:
  MovementScripts()
  {
    for (const auto& [name, parent] :
         std::vector<std::pair<std::string, std::string>>{
           { "Form", "" },
           { "ObjectReference", "Form" },
           { "Actor", "ObjectReference" },
           { "Quest", "Form" },
           { "MiscObject", "Form" },
           { "defaultSetStageTRIGScript", "ObjectReference" } }) {
      names.insert(CIString(name.c_str()));
      scripts.emplace(
        CIString(name.c_str()),
        ScriptBytes(name, parent, name == "defaultSetStageTRIGScript"));
    }
  }
  std::vector<uint8_t> GetScriptPex(const char* name) override
  {
    return scripts.at(CIString(name));
  }
  const std::set<CIString>& ListScripts(bool) override { return names; }

private:
  std::set<CIString> names;
  CIMap<std::vector<uint8_t>> scripts;
};

struct MovementStorage
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
  std::map<FormDesc, MpChangeForm> rows;
  bool SupportsAtomicBatches() const noexcept override { return true; }
  bool SupportsUpsertResults() const noexcept override { return true; }
  void IterateSync(const IterateSyncCallback& callback) override
  {
    for (const auto& [id, form] : rows)
      callback(form);
  }
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
    if (!error) {
      auto next = rows;
      for (const auto& form : submissions.at(index).forms)
        if (form)
          next[form->formDesc] = *form;
      rows.swap(next);
      ++finished;
    }
    const auto callback = submissions.at(index).complete;
    callback(error);
  }
  uint32_t GetNumFinishedUpserts() const override { return finished; }
  uint32_t GetNumFinishedIterates() const override { return 0; }
  void Tick() override {}
  bool GetRecycledChangeFormsBuffer(Forms&) override { return false; }
  const std::string& GetName() const override
  {
    static const std::string name = "campaign-movement-controlled";
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

struct PhysicalFixture
{
  PartOne server;
  PacketParser parser;
  std::shared_ptr<MovementScripts> scripts =
    std::make_shared<MovementScripts>();
  std::shared_ptr<MovementStorage> storage =
    std::make_shared<MovementStorage>();
  bool guest;
  explicit PhysicalFixture(bool guestSource = false,
                           bool initializeRight = true, bool outside = false)
    : guest(guestSource)
  {
    Attach();
    const auto phase = "campaign:" + std::to_string(kOwner) + ":movement";
    const auto start = outside ? kOutside : kStart;
    for (const auto& [id, user] :
         std::vector<std::pair<uint32_t, Networking::UserId>>{
           { kOwner, 0 }, { kGuest, 1 }, { kObserver, 2 } }) {
      DoConnect(server, user);
      server.CreateActor(id, start, 0, 0x3c);
      auto& actor = server.worldState.GetFormAt<MpActor>(id);
      actor.SetWorldInstance(phase);
      actor.SetInventory(Inventory().AddItem(0xf, id == kOwner ? 10 : 77));
      server.SetUserActor(user, id);
    }
    Guest().SetPropertyValueDump("private.movement.personal", "123", false,
                                 false);
    {
      WorldState::RestorationScope restoring(server.worldState);
      Bind(kLeft, kLeftCopy, kStart);
      Bind(kRight, kRightCopy, kFinish);
    }
    const auto content = QuestScriptRuntime::LoadScripts(server.worldState);
    QuestScriptRuntime::ExecuteReferenceLoad(Owner(), kLeft, content);
    if (initializeRight)
      QuestScriptRuntime::ExecuteReferenceLoad(Owner(), kRight, content);
    Source().ForceSubscriptionsUpdate();
    Owner().ForceSubscriptionsUpdate();
    Guest().ForceSubscriptionsUpdate();
    server.worldState.GetFormAt<MpActor>(kObserver).ForceSubscriptionsUpdate();
    Tick();
    storage->submissions.clear();
    storage->automatic = false;
    server.Messages().clear();
    REQUIRE(Left().IsPointInsidePrimitive(kStart));
    REQUIRE_FALSE(Right().IsPointInsidePrimitive(kStart));
    REQUIRE(Right().IsPointInsidePrimitive(kFinish));
    REQUIRE_FALSE(Left().IsPointInsidePrimitive(kFinish));
  }
  void Attach()
  {
    server.AttachEspm(&GetEspmLoader());
    server.worldState.AttachScriptStorage(scripts);
    server.AttachSaveStorage(storage);
  }
  MpActor& Owner() { return server.worldState.GetFormAt<MpActor>(kOwner); }
  MpActor& Guest() { return server.worldState.GetFormAt<MpActor>(kGuest); }
  MpActor& Source() { return guest ? Guest() : Owner(); }
  MpObjectReference& Left()
  {
    return server.worldState.GetFormAt<MpObjectReference>(kLeftCopy);
  }
  MpObjectReference& Right()
  {
    return server.worldState.GetFormAt<MpObjectReference>(kRightCopy);
  }
  QuestJobRunner& Jobs() { return server.worldState.GetQuestJobs(); }
  void Tick()
  {
    // Production ticks release durable movement packets after storage and
    // before timer/job execution; a bare WorldState tick omits that handoff.
    server.Tick();
  }
  void Bind(uint32_t original, uint32_t id, const NiPoint3& position)
  {
    auto& world = server.worldState;
    const auto record = world.GetEspm().GetBrowser().LookupById(original);
    REQUIRE(record.rec);
    REQUIRE(record.rec->GetType() == "REFR");
    const auto data = reinterpret_cast<const espm::REFR*>(record.rec)
                        ->GetData(world.GetEspmCache());
    REQUIRE(data.boundsDiv2);
    const auto base = record.ToGlobalId(data.baseId);
    const auto type =
      world.GetEspm().GetBrowser().LookupById(base).rec->GetType().ToString();
    world.AddForm(std::make_unique<MpObjectReference>(
                    LocationalData{ position, {}, FormDesc::Tamriel() },
                    server.CreateFormCallbacks(), base, type.c_str(),
                    NiPoint3{ data.boundsDiv2[0], data.boundsDiv2[1],
                              data.boundsDiv2[2] }),
                  id);
    auto& copy = world.GetFormAt<MpObjectReference>(id);
    copy.SetWorldInstance(Owner().GetWorldInstance());
    copy.SetInventory(Inventory());
    CampaignReferenceBinding::Bind(Owner(), original, copy);
  }
  UpdateMovementMessage Packet(const NiPoint3& position = kFinish)
  {
    UpdateMovementMessage message;
    message.idx = Source().GetIdx();
    message.data.worldOrCell = 0x3c;
    message.data.pos = { position.x, position.y, position.z };
    message.data.rot = { 0, 0, 30 };
    message.data.direction = 0.25f;
    message.data.healthPercentage = 1;
    message.data.speed = 100;
    message.data.runMode = "Running";
    return message;
  }
  std::vector<uint8_t> Encode(const UpdateMovementMessage& message, bool json)
  {
    if (json) {
      Json value;
      message.WriteJson(value);
      const auto text = MakeMessage(value);
      return { text.begin(), text.end() };
    }
    SLNet::BitStream stream;
    MessageSerializerFactory::CreateMessageSerializer()->Serialize(message,
                                                                   stream);
    return { stream.GetData(),
             stream.GetData() + stream.GetNumberOfBytesUsed() };
  }
  void SendBytes(const std::vector<uint8_t>& bytes)
  {
    parser.TransformPacketIntoAction(guest ? 1 : 0, bytes.data(), bytes.size(),
                                     server.GetActionListener());
  }
  void Send(const NiPoint3& position = kFinish, bool json = false)
  {
    SendBytes(Encode(Packet(position), json));
  }
  size_t MovementCount(Networking::UserId user = Networking::InvalidUserId)
  {
    return std::count_if(server.Messages().begin(), server.Messages().end(),
                         [&](const PartOne::Message& message) {
                           return message.j.at("t") ==
                             static_cast<int>(MsgType::UpdateMovement) &&
                             (user == Networking::InvalidUserId ||
                              message.userId == user);
                         });
  }
  int Counter(uint32_t original, const char* name)
  {
    const auto key = "reference:" +
      FormDesc::FromFormId(original, server.worldState.espmFiles).ToString() +
      "/defaultsetstagetrigscript";
    return QuestJournal::ScriptState(Owner())
      .at("scripts")
      .at(key)
      .at("values")
      .at(std::string("defaultsetstagetrigscript/") + name)
      .at("value")
      .get<int>();
  }
  void Ack()
  {
    Tick();
    REQUIRE_FALSE(storage->submissions.empty());
    storage->Complete(storage->submissions.size() - 1);
    Tick();
  }
  void Drain()
  {
    storage->automatic = true;
    for (size_t step = 0; step < 512; ++step) {
      Tick();
      const auto jobs = QuestJournal::JobState(Owner());
      if (jobs.HasLive())
        continue;
      bool pending = false;
      for (const auto& job : jobs.completed)
        pending |= Jobs()
                     .Inspect(Source(),
                              QuestJobRunner::RequestId{
                                job.owner, job.generation, job.id })
                     .status == QuestJobRunner::Status::Pending;
      if (!pending) {
        storage->automatic = false;
        return;
      }
    }
    FAIL("Physical movement handlers did not complete");
  }
};
}

TEST_CASE(
  "Campaign packet movement saves source and all trigger occurrences together",
  "[CampaignMovement]")
{
  const bool guest = GENERATE(false, true);
  const bool json = GENERATE(false, true);
  PhysicalFixture f(guest);
  const auto guestBefore = f.Guest().GetChangeForm();
  const auto next = f.Jobs().NextOccurrence(f.Source());
  const auto packet = f.Packet();
  auto bytes = f.Encode(packet, json);
  f.SendBytes(bytes);
  std::fill(bytes.begin(), bytes.end(),
            0xcc); // Deferred publication must own its packet.
  REQUIRE(f.Source().GetPos() == kFinish);
  REQUIRE(f.Source().GetAngle() == NiPoint3{ 0, 0, 30 });
  REQUIRE(f.MovementCount() == 0);
  REQUIRE(f.Owner().GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(f.Counter(kLeft, "leaves") == 0);
  REQUIRE(f.Counter(kRight, "enters") == 0);
  REQUIRE(f.Counter(kRight, "levels") == 0);
  const auto jobs = QuestJournal::JobState(f.Owner());
  REQUIRE(jobs.live.size() == 3);
  REQUIRE(jobs.ingress.next == next.sequence + 3);
  std::set<std::pair<std::string, std::string>> actual;
  for (const auto& [id, job] : jobs.live) {
    REQUIRE(job.interaction);
    REQUIRE_FALSE(job.handler);
    actual.emplace(job.interaction->target, job.interaction->name);
    REQUIRE(job.actor == f.Source().GetChangeForm().formDesc.ToString());
    REQUIRE(job.interaction->sourceReference ==
            f.Source().GetChangeForm().formDesc.ToString());
  }
  REQUIRE(actual ==
          std::set<std::pair<std::string, std::string>>{
            { "6debb:Skyrim.esm", "OnTriggerLeave" },
            { "2b363:Skyrim.esm", "OnTriggerEnter" },
            { "2b363:Skyrim.esm", "OnTrigger" } });
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  const auto& batch = f.storage->submissions.front().forms;
  REQUIRE(batch.at(f.Source().GetIdx()));
  REQUIRE(batch.at(f.Owner().GetIdx()));
  REQUIRE(batch.at(f.Source().GetIdx())->position == kFinish);
  const auto baseline = f.Source().GetDynamicFields().GetValueDump(
    CampaignMovementPlan::kProperty);
  REQUIRE(baseline != "null");
  REQUIRE(batch.at(f.Source().GetIdx())
            ->dynamicFields.GetValueDump(CampaignMovementPlan::kProperty) ==
          baseline);
  REQUIRE(QuestJobState::Read(Json::parse(batch.at(f.Owner().GetIdx())
                                            ->dynamicFields.GetValueDump(
                                              QuestJournal::kProperty))
                                .at("jobs"))
            .Save() == jobs.Save());
  REQUIRE(f.MovementCount() == 0);
  f.storage->Complete(0);
  f.Tick();
  REQUIRE(f.MovementCount(2) == 1);
  Json expected;
  packet.WriteJson(expected);
  for (const auto& sent : f.server.Messages())
    if (sent.j.at("t") == static_cast<int>(MsgType::UpdateMovement))
      REQUIRE(sent.j == expected);
  f.Drain();
  REQUIRE(f.Counter(kLeft, "leaves") == 1);
  REQUIRE(f.Counter(kRight, "enters") == 1);
  REQUIRE(f.Counter(kRight, "levels") == 1);
  REQUIRE(f.Owner().GetInventory().GetItemCount(0xf) == 17);
  REQUIRE(f.Guest().GetInventory() == guestBefore.inv);
  REQUIRE(f.Guest().GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          guestBefore.dynamicFields.GetValueDump(QuestJournal::kProperty));
  REQUIRE(f.Guest().GetDynamicFields().GetValueDump(
            "private.movement.personal") == "123");
}

TEST_CASE(
  "Failed movement receipts retry the same physical sample before publishing",
  "[CampaignMovement][SaveReceipt]")
{
  PhysicalFixture f;
  f.Send();
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  const auto immutable = Serialized(f.storage->submissions.front().forms);
  const auto pending = MpChangeForm::ToJson(f.Source().GetChangeForm());
  f.storage->Complete(
    0, std::make_exception_ptr(std::runtime_error("disk unavailable")));
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 2);
  REQUIRE(Serialized(f.storage->submissions.back().forms) == immutable);
  f.Source().SetTeleportFlag(true);
  f.Send(kStart); // Pending input cannot consume another baseline or teleport
                  // correction.
  REQUIRE(f.Source().GetTeleportFlag());
  REQUIRE(MpChangeForm::ToJson(f.Source().GetChangeForm()) == pending);
  REQUIRE(f.MovementCount() == 0);
  REQUIRE(f.Owner().GetInventory().GetItemCount(0xf) == 10);
  f.storage->Complete(0);
  f.Tick();
  REQUIRE(f.MovementCount() == 0);
  f.storage->Complete(1);
  f.Tick();
  REQUIRE(f.MovementCount(2) == 1);
  f.Drain();
  REQUIRE(f.Owner().GetInventory().GetItemCount(0xf) == 17);
  REQUIRE(f.Counter(kRight, "enters") == 1);
}

TEST_CASE(
  "A preinitialization failure in a later crossing rejects the whole movement",
  "[CampaignMovement][Rollback]")
{
  PhysicalFixture f(false, false);
  const auto owner = MpChangeForm::ToJson(f.Owner().GetChangeForm());
  const auto next = f.Jobs().NextOccurrence(f.Source());
  const auto sourceIdx = f.Source().GetIdx();
  const auto& times = f.server.worldState.lastMovUpdateByIdx;
  const auto previousTime =
    sourceIdx < times.size() ? times[sourceIdx] : std::nullopt;
  REQUIRE_THROWS(f.Send());
  REQUIRE(MpChangeForm::ToJson(f.Owner().GetChangeForm()) == owner);
  REQUIRE(f.Jobs().NextOccurrence(f.Source()) == next);
  const auto& afterTimes = f.server.worldState.lastMovUpdateByIdx;
  REQUIRE((sourceIdx < afterTimes.size() ? afterTimes[sourceIdx]
                                         : std::nullopt) == previousTime);
  REQUIRE(f.MovementCount() == 0);
  f.Tick();
  REQUIRE(f.storage->submissions.empty());
  QuestScriptRuntime::ExecuteReferenceLoad(
    f.Owner(), kRight, QuestScriptRuntime::LoadScripts(f.server.worldState));
  f.storage->automatic = true;
  f.Tick();
  f.storage->automatic = false;
  f.storage->submissions.clear();
  f.Send();
  REQUIRE(QuestJournal::JobState(f.Owner()).live.size() == 3);
  f.Ack();
  f.Drain();
  REQUIRE(f.Counter(kLeft, "leaves") == 1);
  REQUIRE(f.Counter(kRight, "enters") == 1);
}

TEST_CASE(
  "A movement with no trigger events still saves its physical baseline",
  "[CampaignMovement][SaveReceipt]")
{
  PhysicalFixture f(true, true, true);
  const auto next = f.Jobs().NextOccurrence(f.Source());
  const auto nextJob = f.Jobs().NextRequest(f.Source());
  const NiPoint3 moved{ 2501, 1000, 1000 };
  f.Send(moved);
  REQUIRE(f.Source().GetPos() == moved);
  REQUIRE(f.Source().GetDynamicFields().GetValueDump(
            CampaignMovementPlan::kProperty) != "null");
  REQUIRE_FALSE(QuestJournal::JobState(f.Owner()).HasLive());
  REQUIRE(f.Jobs().NextOccurrence(f.Source()) == next);
  REQUIRE(f.Jobs().NextRequest(f.Source()) == nextJob);
  REQUIRE(f.MovementCount() == 0);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  const auto& batch = f.storage->submissions.front().forms;
  REQUIRE(batch.at(f.Source().GetIdx())->position == moved);
  REQUIRE(batch.at(f.Owner().GetIdx()));
  f.storage->Complete(0);
  f.Tick();
  REQUIRE(f.MovementCount(2) == 1);
  REQUIRE(f.Owner().GetInventory().GetItemCount(0xf) == 10);
}

TEST_CASE(
  "Campaign teleport paths refuse before effects from the first movement",
  "[CampaignMovement][Rollback]")
{
  PhysicalFixture f;
  REQUIRE(f.Source().UsesCampaignMovement());
  REQUIRE(f.Source().GetDynamicFields().GetValueDump(
            CampaignMovementPlan::kProperty) == "null");

  const auto requireUnchanged = [&](const auto& operation) {
    const auto before = MpChangeForm::ToJson(f.Source().GetChangeForm());
    const auto teleportFlag = f.Source().GetTeleportFlag();
    const auto emitters = f.Source().GetEmitters();
    const auto listeners = f.Source().GetListeners();
    const auto actorListeners = f.Source().GetActorListeners();
    const auto leftListeners = f.Left().GetListeners();
    const auto leftActors = f.Left().GetActorListeners();
    const auto rightListeners = f.Right().GetListeners();
    const auto rightActors = f.Right().GetActorListeners();
    REQUIRE(f.server.Messages().empty());
    REQUIRE_THROWS(operation());
    REQUIRE(f.server.Messages().empty());
    // The complete snapshot covers pose, cell, health/death, inventory,
    // journal and the private movement baseline, including partial writes.
    REQUIRE(MpChangeForm::ToJson(f.Source().GetChangeForm()) == before);
    REQUIRE(f.Source().GetTeleportFlag() == teleportFlag);
    REQUIRE(f.Source().GetEmitters() == emitters);
    REQUIRE(f.Source().GetListeners() == listeners);
    REQUIRE(f.Source().GetActorListeners() == actorListeners);
    REQUIRE(f.Left().GetListeners() == leftListeners);
    REQUIRE(f.Left().GetActorListeners() == leftActors);
    REQUIRE(f.Right().GetListeners() == rightListeners);
    REQUIRE(f.Right().GetActorListeners() == rightActors);
  };
  const auto rejectTeleports = [&] {
    requireUnchanged([&] {
      f.Source().Teleport(
        LocationalData{ kOutside, { 0, 0, 90 }, FormDesc::Tamriel() });
    });
    requireUnchanged([&] { f.Source().Kill(nullptr, true); });
  };

  // Bound primitives require the durable path even before enrollment. A
  // late SetPos rejection must not follow a teleport packet or grid clear.
  rejectTeleports();
  REQUIRE(f.storage->submissions.empty());
  f.Send();
  REQUIRE(f.Source().GetPos() == kFinish);
  REQUIRE(QuestJournal::JobState(f.Owner()).live.size() == 3);
  REQUIRE(f.Source().GetDynamicFields().GetValueDump(
            CampaignMovementPlan::kProperty) != "null");
  rejectTeleports(); // The same guarantee holds while the receipt is pending.
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  f.storage->Complete(0);
  f.Tick();
  REQUIRE(f.MovementCount(2) == 1);
  f.Drain();
  REQUIRE(f.Counter(kLeft, "leaves") == 1);
  REQUIRE(f.Counter(kRight, "enters") == 1);
  REQUIRE(f.Counter(kRight, "levels") == 1);
  REQUIRE(f.Owner().GetInventory().GetItemCount(0xf) == 17);
}

TEST_CASE(
  "Late movement publication cannot cross a source connection or lifetime",
  "[CampaignMovement][SaveReceipt]")
{
  const bool replace = GENERATE(false, true);
  PhysicalFixture f(true);
  f.Send();
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  const auto original = f.server.worldState.LookupFormByIdNoLoad(kGuest);
  DoDisconnect(f.server, 1);
  if (replace) {
    f.server.DestroyActor(kGuest);
    f.server.CreateActor(kGuest, kOutside, 0, 0x3c);
  }
  DoConnect(f.server, 1);
  // A reused connection is not the accepted movement's connection. Rejoining
  // an enrolled spatial roster is a separate, currently unsupported lane.
  f.server.Messages().clear();
  f.storage->Complete(0);
  f.Tick();
  REQUIRE(f.MovementCount() == 0);
  if (replace) {
    REQUIRE(f.server.worldState.LookupFormByIdNoLoad(kGuest) != original);
    REQUIRE(f.Guest().GetPos() == kOutside);
  }
}
