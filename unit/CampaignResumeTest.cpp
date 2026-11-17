#include "CampaignFrameMessage.h"
#include "CampaignMovement.h"
#include "CampaignReferenceBinding.h"
#include "CampaignReferenceFactory.h"
#include "CreateActorMessage.h"
#include "DestroyActorMessage.h"
#include "MessageSerializerFactory.h"
#include "PacketParser.h"
#include "QuestJobRunner.h"
#include "QuestJournal.h"
#include "QuestScriptRuntime.h"
#include "TestUtils.hpp"
#include "UpdateMovementMessage.h"
#include "database_drivers/SqliteDatabase.h"
#include "libespm/GroupUtils.h"
#include "libespm/REFR.h"
#include "save_storages/ISaveStorage.h"
#include "script_storages/IScriptStorage.h"
#include <algorithm>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <random>
#include <set>
#include <slikenet/BitStream.h>

extern espm::Loader& GetEspmLoader();

namespace {
using Json = nlohmann::json;
using Forms = std::vector<std::optional<MpChangeForm>>;
constexpr uint32_t kOwner = 0xff009b00, kTrigger = 0x6debb;
constexpr uint32_t kObserver = kOwner + 1;
constexpr ProfileId kProfile = 42;
constexpr Networking::UserId kUser = 0;

struct ResumeFiles
{
  std::filesystem::path directory;
  ResumeFiles()
  {
    std::random_device random;
    for (size_t attempt = 0; attempt < 20; ++attempt) {
      const auto candidate = std::filesystem::temp_directory_path() /
        ("skymp-resume-" + std::to_string(random()) + "-" +
         std::to_string(random()));
      if (std::filesystem::create_directory(candidate)) {
        directory = candidate;
        return;
      }
    }
    throw std::runtime_error("Cannot create an isolated resume database");
  }
  ~ResumeFiles()
  {
    // Only the directory exclusively created by this fixture is removed.
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
  std::string File() const { return (directory / "resume.sqlite3").string(); }
};

struct Bytes
{
  std::vector<uint8_t> value;
  void U8(uint8_t n) { value.push_back(n); }
  void U16(uint16_t n)
  {
    U8(n >> 8);
    U8(n);
  }
  void U32(uint32_t n)
  {
    U16(n >> 16);
    U16(n);
  }
  void Text(const std::string& text)
  {
    U16(static_cast<uint16_t>(text.size()));
    value.insert(value.end(), text.begin(), text.end());
  }
};

std::vector<uint8_t> ResumePex(const std::string& name,
                               const std::string& parent, bool handlers)
{
  const std::vector<std::string> strings = { "",
                                             name,
                                             parent,
                                             "None",
                                             "Int",
                                             "ObjectReference",
                                             "::stage_var",
                                             "inits",
                                             "cells",
                                             "loads",
                                             "prefixes",
                                             "suffixes",
                                             "leaves",
                                             "levels",
                                             "OnInit",
                                             "OnCellLoad",
                                             "OnLoad",
                                             "OnTriggerEnter",
                                             "OnTriggerLeave",
                                             "OnTrigger",
                                             "source",
                                             "temp",
                                             "Utility",
                                             "Wait" };
  const auto index = [&](const std::string& text) {
    const auto found = std::find(strings.begin(), strings.end(), text);
    REQUIRE(found != strings.end());
    return static_cast<uint16_t>(found - strings.begin());
  };
  Bytes body;
  const auto word = [&](const std::string& text) { body.U16(index(text)); };
  const auto id = [&](const char* text) {
    body.U8(1);
    word(text);
  };
  const auto text = [&](const char* value) {
    body.U8(2);
    word(value);
  };
  const auto integer = [&](int value) {
    body.U8(3);
    body.U32(value);
  };
  const auto increment = [&](const char* field) {
    body.U8(FunctionCode::kOp_IAdd);
    id(field);
    id(field);
    integer(1);
  };
  word(parent);
  word("");
  body.U32(0);
  word("");
  body.U16(handlers ? 8 : 0);
  if (handlers) {
    for (const auto* field : { "::stage_var", "inits", "cells", "loads",
                               "prefixes", "suffixes", "leaves", "levels" }) {
      word(field);
      word("Int");
      body.U32(0);
      integer(0);
    }
  }
  body.U16(0);
  body.U16(handlers ? 1 : 0);
  if (handlers) {
    word("");
    body.U16(6);
    for (const auto& [event, field] :
         std::vector<std::pair<const char*, const char*>>{
           { "OnInit", "inits" },
           { "OnCellLoad", "cells" },
           { "OnLoad", "loads" },
           { "OnTriggerEnter", "prefixes" },
           { "OnTriggerLeave", "leaves" },
           { "OnTrigger", "levels" } }) {
      const bool enter = std::string(event) == "OnTriggerEnter";
      const bool trigger = std::string(event).starts_with("OnTrigger");
      word(event);
      word("None");
      word("");
      body.U32(0);
      body.U8(0);
      body.U16(trigger ? 1 : 0);
      if (trigger) {
        word("source");
        word("ObjectReference");
      }
      body.U16(enter ? 1 : 0);
      if (enter) {
        word("temp");
        word("None");
      }
      body.U16(enter ? 4 : 2);
      increment(field);
      if (enter) {
        body.U8(FunctionCode::kOp_CallStatic);
        text("Utility");
        text("Wait");
        id("temp");
        integer(1);
        body.U8(4);
        const float seconds = 60;
        uint32_t bits;
        std::memcpy(&bits, &seconds, sizeof(bits));
        body.U32(bits);
        increment("suffixes");
      }
      body.U8(FunctionCode::kOp_Return);
      body.U8(0);
    }
  }
  Bytes output;
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
  output.U32(static_cast<uint32_t>(body.value.size() + 4));
  output.value.insert(output.value.end(), body.value.begin(),
                      body.value.end());
  return output.value;
}

class ResumeScripts : public IScriptStorage
{
public:
  ResumeScripts()
  {
    for (const auto& [name, parent] :
         std::vector<std::pair<std::string, std::string>>{
           { "Form", "" },
           { "ObjectReference", "Form" },
           { "Actor", "ObjectReference" },
           { "Quest", "Form" },
           { "defaultSetStageTRIGScript", "ObjectReference" } }) {
      names.insert(CIString(name.c_str()));
      scripts.emplace(
        CIString(name.c_str()),
        ResumePex(name, parent, name == "defaultSetStageTRIGScript"));
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

// Real SQLite rows/transactions, with only callback delivery and iteration
// order controlled. PartOne itself restores registered players as disabled.
struct ResumeStorage
  : Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>
{
  struct Submission
  {
    Forms forms;
    UpsertResultCallback callback;
  };
  std::shared_ptr<SqliteDatabase> database;
  bool ownerFirst, automatic = true;
  std::vector<Submission> submissions;
  explicit ResumeStorage(const ResumeFiles& files, bool first)
    : database(std::make_shared<SqliteDatabase>(files.File()))
    , ownerFirst(first)
  {
  }
  bool SupportsAtomicBatches() const noexcept override { return true; }
  bool SupportsUpsertResults() const noexcept override { return true; }
  void IterateSync(const IterateSyncCallback& callback) override
  {
    std::vector<MpChangeForm> forms;
    database->Iterate([&](const MpChangeForm& form) { forms.push_back(form); },
                      {});
    std::stable_sort(forms.begin(), forms.end(),
                     [&](const auto& a, const auto& b) {
                       return ownerFirst ? a.profileId > b.profileId
                                         : a.profileId < b.profileId;
                     });
    for (const auto& form : forms)
      callback(form);
  }
  void Iterate(const IterateCallback& callback,
               const std::optional<std::vector<FormDesc>>& filter) override
  {
    std::vector<MpChangeForm> forms;
    database->Iterate([&](const MpChangeForm& form) { forms.push_back(form); },
                      filter);
    callback(forms);
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
      auto forms = submissions.at(index).forms;
      database->Upsert(std::move(forms));
    }
    const auto callback = submissions.at(index).callback;
    callback(error);
  }
  uint32_t GetNumFinishedUpserts() const override { return 0; }
  uint32_t GetNumFinishedIterates() const override { return 0; }
  void Tick() override {}
  bool GetRecycledChangeFormsBuffer(Forms&) override { return false; }
  const std::string& GetName() const override
  {
    static const std::string name = "campaign-resume-sqlite";
    return name;
  }
};

Json Serialized(const Forms& forms)
{
  auto output = Json::array();
  for (const auto& form : forms)
    output.push_back(form ? MpChangeForm::ToJson(*form) : Json(nullptr));
  return output;
}

struct ResumeWorld
{
  PartOne server;
  PacketParser parser;
  std::shared_ptr<ResumeScripts> scripts = std::make_shared<ResumeScripts>();
  std::shared_ptr<ResumeStorage> storage;
  std::shared_ptr<QuestJobTime> time =
    std::make_shared<QuestJobTime>(QuestJobTime{ 1000000, 5000 });
  NiPoint3 center, outside;
  uint32_t cell;
  uint32_t tombstoneId = 0;
  Json lastMovement;
  const std::string phase = "campaign:" + std::to_string(kOwner) + ":resume";
  explicit ResumeWorld(const ResumeFiles& files, bool ownerFirst = true)
    : storage(std::make_shared<ResumeStorage>(files, ownerFirst))
  {
    server.AttachEspm(&GetEspmLoader());
    server.worldState.AttachScriptStorage(scripts);
    server.worldState.GetQuestJobs().SetClock([time = time] { return *time; });
    const auto reference = server.GetEspm().GetBrowser().LookupById(kTrigger);
    const auto data = reinterpret_cast<const espm::REFR*>(reference.rec)
                        ->GetData(server.worldState.GetEspmCache());
    REQUIRE(data.loc);
    center = { data.loc->pos[0], data.loc->pos[1], data.loc->pos[2] };
    outside = center + NiPoint3{ 600, 0, 0 };
    cell = reference.ToGlobalId(
      espm::GetWorldOrCell(server.GetEspm().GetBrowser(), reference.rec));
    server.AttachSaveStorage(storage);
  }
  MpActor& Owner() { return server.worldState.GetFormAt<MpActor>(kOwner); }
  MpObjectReference& Target()
  {
    return CampaignReferenceBinding::Resolve(Owner(), kTrigger);
  }
  Json Area() const
  {
    return { { "id", "resume" },
             { "worldOrCell", cell },
             { "center", { center.x, center.y, center.z } },
             { "radius", 1 } };
  }
  void Tick() { server.Tick(); }
  int Counter(const char* field)
  {
    const auto heap = QuestJournal::ScriptState(Owner());
    const auto& values =
      heap.at("scripts")
        .at("reference:6debb:Skyrim.esm/defaultsetstagetrigscript")
        .at("values");
    const auto key = std::string("defaultsetstagetrigscript/") + field;
    return values.contains(key) ? values.at(key).at("value").get<int>() : 0;
  }
  Json History()
  {
    return { { "jobs", QuestJournal::JobState(Owner()).Save() },
             { "heap", QuestJournal::ScriptState(Owner()) },
             { "baseline",
               Owner().GetDynamicFields().GetValueDump(
                 CampaignMovementPlan::kProperty) },
             { "init", Counter("inits") },
             { "cells", Counter("cells") },
             { "loads", Counter("loads") } };
  }
  void Move(const NiPoint3& position, uint32_t actorId = kOwner,
            Networking::UserId user = kUser)
  {
    auto& actor = server.worldState.GetFormAt<MpActor>(actorId);
    const auto firstMessage = server.Messages().size();
    lastMovement = { { "teleportBefore", actor.GetTeleportFlag() },
                     { "requested", { position.x, position.y, position.z } } };
    UpdateMovementMessage movement;
    movement.idx = actor.GetIdx();
    movement.data.worldOrCell = cell;
    movement.data.pos = { position.x, position.y, position.z };
    movement.data.rot = { 0, 0, 0 };
    movement.data.healthPercentage = 1;
    movement.data.speed = 100;
    movement.data.runMode = "Running";
    SLNet::BitStream inner;
    MessageSerializerFactory::CreateMessageSerializer()->Serialize(movement,
                                                                   inner);
    CampaignFrameMessage frame;
    frame.epoch = server.GetPacketEpoch(user);
    frame.payload.assign(inner.GetData(),
                         inner.GetData() + inner.GetNumberOfBytesUsed());
    SLNet::BitStream stream;
    MessageSerializerFactory::CreateMessageSerializer()->Serialize(frame,
                                                                   stream);
    parser.TransformPacketIntoAction(user, stream.GetData(),
                                     stream.GetNumberOfBytesUsed(),
                                     server.GetActionListener());
    const auto pose = actor.GetPos();
    lastMovement["teleportAfter"] = actor.GetTeleportFlag();
    lastMovement["positionAfter"] = { pose.x, pose.y, pose.z };
    lastMovement["messages"] = Json::array();
    for (size_t i = firstMessage; i < server.Messages().size(); ++i) {
      const auto& sent = server.Messages()[i];
      if (const auto* frame =
            dynamic_cast<CampaignFrameMessage*>(sent.message.get())) {
        auto decoded =
          MessageSerializerFactory::CreateMessageSerializer()->Deserialize(
            frame->payload.data(), frame->payload.size());
        Json message;
        REQUIRE(decoded);
        decoded->message->WriteJson(message);
        lastMovement["messages"].push_back(std::move(message));
      } else {
        lastMovement["messages"].push_back(sent.j);
      }
    }
  }
  template <class Predicate>
  void Until(const Predicate& predicate, const char* boundary = "unspecified")
  {
    for (size_t step = 0; step < 512; ++step) {
      Tick();
      if (predicate())
        return;
    }
    INFO("Boundary: " << boundary);
    INFO("Resume: " << server.GetCampaignResume(kUser).dump());
    INFO("Counters: init=" << Counter("inits")
                           << ", cells=" << Counter("cells")
                           << ", loads=" << Counter("loads")
                           << ", prefixes=" << Counter("prefixes")
                           << ", suffixes=" << Counter("suffixes")
                           << ", leaves=" << Counter("leaves")
                           << ", levels=" << Counter("levels"));
    INFO("Roots: " << QuestJournal::JobState(Owner()).Save().dump());
    INFO("Storage submissions: " << storage->submissions.size());
    INFO("Last movement: " << lastMovement.dump());
    FAIL("Campaign resume did not reach its expected durable boundary");
  }
  void Seed(bool tombstone = false, bool observer = false)
  {
    DoConnect(server, kUser);
    server.CreateActor(kOwner, outside, 0, cell, kProfile);
    Owner().SetWorldInstance(phase);
    server.SetUserActor(kUser, kOwner);
    Owner().SetRaceMenuOpen(true);
    auto appearance = jAppearance;
    appearance["idx"] = Owner().GetIdx();
    appearance["data"]["raceId"] = 0x13746; // Owned Nord race.
    DoMessage(server, kUser, appearance);
    REQUIRE(Owner().GetAppearance());
    REQUIRE_FALSE(Owner().IsRaceMenuOpen());
    server.serverState.userInfo[kUser]->gameplayBlocked = true;
    server.BeginPacketEpoch(kUser);
    server.serverState.userInfo[kUser]->gameplayBlocked = false;
    if (observer) {
      DoConnect(server, 1);
      server.CreateActor(kObserver, outside, 0, cell, kProfile + 1);
      server.worldState.GetFormAt<MpActor>(kObserver).SetWorldInstance(phase);
      server.SetUserActor(1, kObserver);
    }
    const auto originals = tombstone
      ? std::vector<uint32_t>{ kTrigger, 0x8a986 }
      : std::vector<uint32_t>{ kTrigger };
    const auto copies =
      CampaignReferenceFactory::Ensure(Owner(), originals, 1);
    REQUIRE(copies.size() == originals.size());
    if (tombstone) {
      tombstoneId = copies.back();
      server.worldState.GetFormAt<MpObjectReference>(tombstoneId).Delete();
    }
    QuestScriptRuntime::ExecuteReferenceLoad(Owner(), kTrigger);
    Owner().ForceSubscriptionsUpdate();
    if (observer)
      server.worldState.GetFormAt<MpActor>(kObserver)
        .ForceSubscriptionsUpdate();
    Tick();
    Move(center);
    Until(
      [&] {
        const auto jobs = QuestJournal::JobState(Owner());
        if (jobs.live.size() != 1 || Counter("prefixes") != 1 ||
            Counter("levels") != 1)
          return false;
        const auto& job = jobs.live.begin()->second;
        return job.handler && job.handler->cursor == "waiting" &&
          server.worldState.GetQuestJobs()
            .Inspect(
              Owner(),
              QuestJobRunner::RequestId{ job.owner, job.generation, job.id })
            .status == QuestJobRunner::Status::Sleeping;
      },
      "seed one sleeping Enter and completed level event");
    REQUIRE(Counter("suffixes") == 0);
    REQUIRE(Counter("leaves") == 0);
    REQUIRE(Counter("inits") == 1);
    REQUIRE(Owner().GetPos() == center);
    REQUIRE(Owner().GetChangeForm().profileId == kProfile);
    // The other root may have completed in the same Tick that exposed the
    // sleeper. Flush its final receipt before taking the restart snapshot.
    for (size_t step = 0; step < 3; ++step)
      Tick();
  }
  void EnrollVisitor()
  {
    auto& guest = server.worldState.GetFormAt<MpActor>(kObserver);
    guest.SetRaceMenuOpen(true);
    auto appearance = jAppearance;
    appearance["idx"] = guest.GetIdx();
    appearance["data"]["raceId"] = 0x13746;
    DoMessage(server, 1, appearance);
    REQUIRE_FALSE(guest.IsRaceMenuOpen());
    server.serverState.userInfo[1]->gameplayBlocked = true;
    server.BeginPacketEpoch(1);
    server.serverState.userInfo[1]->gameplayBlocked = false;
    Move(center, kObserver, 1);
    Until(
      [&] {
        const auto jobs = QuestJournal::PersonalJobState(Owner());
        return jobs.live.size() == 2 && Counter("prefixes") == 2 &&
          Counter("levels") == 2 &&
          std::all_of(jobs.live.begin(), jobs.live.end(),
                      [](const auto& entry) {
                        return entry.second.handler &&
                          entry.second.handler->cursor == "waiting";
                      });
      },
      "visitor movement durably creates its own suspended host handler");
    for (size_t step = 0; step < 3; ++step)
      Tick();
  }
  Json Begin()
  {
    DoConnect(server, kUser);
    server.serverState.userInfo[kUser]->gameplayBlocked = true;
    server.BeginPacketEpoch(kUser);
    return server.ResumeCampaignActor(kUser, kOwner, phase, Area());
  }
  Json ProjectionReady()
  {
    Until(
      [&] {
        return server.GetCampaignResume(kUser).at("status") ==
          "ProjectionReady";
      },
      "durable resume ready for projection");
    return server.GetCampaignResume(kUser);
  }
  Json Capture()
  {
    const auto epoch = server.BeginPacketEpoch(kUser);
    return server.CaptureCampaignResumeProjection(kUser, epoch);
  }
  bool Complete(
    const Json& projection, const std::function<void()>& publish = [] {})
  {
    return server.CompleteCampaignResume(
      kUser, projection.at("attempt").get<std::string>(),
      server.GetPacketEpoch(kUser), projection.at("proof").get<std::string>(),
      publish);
  }
};
}

TEST_CASE(
  "Registered campaign owners resume SQLite history before live movement",
  "[CampaignResume][SQLite][Projection]")
{
  const bool ownerFirst = GENERATE(false, true);
  ResumeFiles files;
  Json history;
  {
    ResumeWorld initial(files);
    initial.Seed();
    history = initial.History();
  }
  ResumeWorld restored(files, ownerFirst);
  REQUIRE(restored.Owner().IsDisabled());
  REQUIRE_FALSE(restored.Owner().IsRaceMenuOpen());
  REQUIRE(restored.server.GetActorsByProfileId(kProfile).contains(kOwner));
  REQUIRE(restored.History() == history);
  REQUIRE(restored.Target().GetActorListeners().empty());
  restored.time->unixMs += 60000;
  restored.time->steadyMs += 60000;
  for (size_t step = 0; step < 4; ++step)
    restored.Tick();
  REQUIRE(restored.History() == history);

  restored.storage->automatic = false;
  const auto pending = restored.Begin();
  REQUIRE(pending.at("status") == "Pending");
  const auto epoch = restored.server.GetPacketEpoch(kUser);
  const auto repeated = restored.server.ResumeCampaignActor(
    kUser, kOwner, restored.phase, restored.Area());
  REQUIRE(repeated.at("attempt") == pending.at("attempt"));
  REQUIRE(restored.server.GetPacketEpoch(kUser) == epoch);
  REQUIRE(restored.server.serverState.userInfo[kUser]->gameplayBlocked);
  restored.Move(restored.outside);
  REQUIRE(restored.Owner().GetPos() == restored.center);
  REQUIRE(restored.History() == history);
  // These are the metadata writes performed by the real handled spawn flow.
  restored.Owner().SetPropertyValueDump("private.discordRoles", "[\"member\"]",
                                        false, false);
  restored.Owner().RegisterPrivateIndexedProperty("private.indexed.discordId",
                                                  "\"resume-user\"");
  restored.Until([&] { return restored.storage->submissions.size() == 1; },
                 "resume snapshot submitted");
  REQUIRE(restored.storage->submissions.size() == 1);
  REQUIRE(restored.server.GetCampaignResume(kUser).at("status") == "Pending");
  REQUIRE_THROWS(
    restored.server.CaptureCampaignResumeProjection(kUser, epoch));
  REQUIRE(restored.server.GetPacketEpoch(kUser) == epoch);
  restored.storage->Complete(0);
  restored.storage->automatic = true;
  restored.ProjectionReady();
  REQUIRE(restored.History() == history);

  size_t presentations = 0;
  restored.server.SetOnActorStreamIn([&](const auto&, const auto&, auto&) {
    ++presentations;
    restored.Owner().SetPropertyValueDump("private.resume.presentation",
                                          std::to_string(presentations), false,
                                          false);
  });
  const auto projection = restored.Capture();
  REQUIRE(presentations > 0);
  const auto capturedPresentations = presentations;
  REQUIRE(projection.at("attempt") == pending.at("attempt"));
  REQUIRE(projection.at("manifest").at("actor") == kOwner);
  REQUIRE(projection.at("baseline").at("actor") == kOwner);
  REQUIRE(projection.at("baseline").at("packetEpoch") ==
          restored.server.GetPacketEpoch(kUser));
  auto values = restored.Owner().GetActorValues();
  values.healthPercentage = 0.75f;
  restored.Owner().SetActorValues(values);
  restored.Owner().NetSendChangeValues(values, std::nullopt);
  bool published = false;
  REQUIRE(restored.Complete(projection, [&] {
    REQUIRE(restored.server.serverState.userInfo[kUser]->gameplayBlocked);
    restored.Tick();
    REQUIRE(restored.Counter("suffixes") == 0);
    published = true;
  }));
  REQUIRE(published);
  REQUIRE(presentations == capturedPresentations);
  REQUIRE_FALSE(restored.server.serverState.userInfo[kUser]->gameplayBlocked);
  REQUIRE_FALSE(restored.Owner().IsDisabled());
  REQUIRE(restored.History() == history);
  const auto next = QuestJournal::JobState(restored.Owner()).ingress.next;
  restored.Move(restored.outside);
  restored.Until(
    [&] {
      return !QuestJournal::JobState(restored.Owner()).HasLive() &&
        restored.Counter("leaves") == 1 && restored.Counter("suffixes") == 1;
    },
    "restored sleeper suffix and later real Leave complete");
  REQUIRE(QuestJournal::JobState(restored.Owner()).ingress.next == next + 1);
  REQUIRE(restored.Counter("prefixes") == 1);
  REQUIRE(restored.Counter("levels") == 1);
  REQUIRE(restored.Counter("inits") == history.at("init"));
  REQUIRE(restored.Counter("cells") == history.at("cells"));
  REQUIRE(restored.Counter("loads") == history.at("loads"));
  for (size_t step = 0; step < 3; ++step)
    restored.Tick();
  SqliteDatabase independent(files.File());
  bool readOwner = false;
  independent.Iterate(
    [&](const MpChangeForm& form) {
      if (form.profileId != kProfile)
        return;
      readOwner = true;
      REQUIRE_FALSE(form.isDisabled);
      REQUIRE(form.position == restored.outside);
      REQUIRE(
        form.dynamicFields.GetValueDump(CampaignMovementPlan::kProperty) ==
        restored.Owner().GetDynamicFields().GetValueDump(
          CampaignMovementPlan::kProperty));
    },
    {});
  REQUIRE(readOwner);
}

TEST_CASE("A failed pre-detach movement save fences the next resume",
          "[CampaignResume][SQLite][SaveReceipt]")
{
  ResumeFiles files;
  ResumeWorld f(files);
  f.Seed();
  const auto continuation =
    QuestJournal::ScriptState(f.Owner()).at("continuations");
  f.storage->automatic = false;
  const auto first = f.storage->submissions.size();
  f.Move(f.center + NiPoint3{ 1, 0, 0 });
  f.Tick();
  REQUIRE(f.storage->submissions.size() == first + 1);
  const auto immutable = Serialized(f.storage->submissions[first].forms);
  REQUIRE(f.server.DetachCampaignActor(kUser));
  DoDisconnect(f.server, kUser);
  const auto pending = f.Begin();
  REQUIRE(pending.at("status") == "Pending");
  f.time->unixMs += 60000;
  f.time->steadyMs += 60000;
  f.storage->Complete(
    first, std::make_exception_ptr(std::runtime_error("disk unavailable")));
  f.Tick();
  REQUIRE(f.storage->submissions.size() == first + 2);
  REQUIRE(Serialized(f.storage->submissions.back().forms) == immutable);
  REQUIRE(f.server.GetCampaignResume(kUser).at("status") == "Pending");
  REQUIRE(QuestJournal::ScriptState(f.Owner()).at("continuations") ==
          continuation);
  REQUIRE(f.Counter("suffixes") == 0);
  REQUIRE(f.Counter("levels") == 1);
  f.storage->Complete(first + 1);
  f.storage->automatic = true;
  f.ProjectionReady();
  REQUIRE(f.Counter("suffixes") == 0);
  const auto projection = f.Capture();
  REQUIRE(f.Complete(projection));
  f.Until(
    [&] {
      return !QuestJournal::JobState(f.Owner()).HasLive() &&
        f.Counter("suffixes") == 1;
    },
    "failed old save retried, original sleeper completes once");
  REQUIRE(f.Counter("prefixes") == 1);
  REQUIRE(f.Counter("levels") == 2);
  REQUIRE(f.Counter("leaves") == 0);
}

TEST_CASE("A replacement connection cannot release an older resume attempt",
          "[CampaignResume][SQLite][Projection][Lifetime]")
{
  const bool duringAck = GENERATE(false, true);
  ResumeFiles files;
  {
    ResumeWorld initial(files);
    initial.Seed();
  }
  ResumeWorld f(files);
  f.storage->automatic = duringAck;
  const auto first = f.Begin();
  const auto replace = [&] {
    DoDisconnect(f.server, kUser);
    DoConnect(f.server, kUser);
    f.server.serverState.userInfo[kUser]->gameplayBlocked = true;
    f.server.BeginPacketEpoch(kUser);
  };
  if (duringAck) {
    f.ProjectionReady();
    const auto projection = f.Capture();
    bool invoked = false;
    REQUIRE_FALSE(f.Complete(projection, [&] {
      invoked = true;
      replace();
    }));
    REQUIRE(invoked);
  } else {
    f.Until([&] { return f.storage->submissions.size() == 1; },
            "resume snapshot pending before connection replacement");
    REQUIRE(f.storage->submissions.size() == 1);
    replace();
    f.storage->Complete(0);
    f.storage->automatic = true;
    for (size_t step = 0; step < 4; ++step)
      f.Tick();
  }
  REQUIRE(f.server.serverState.userInfo[kUser]->gameplayBlocked);
  REQUIRE(f.Counter("suffixes") == 0);
  const auto next =
    f.server.ResumeCampaignActor(kUser, kOwner, f.phase, f.Area());
  REQUIRE(next.at("attempt") != first.at("attempt"));
  f.ProjectionReady();
  const auto projection = f.Capture();
  REQUIRE(f.Complete(projection));
  REQUIRE_FALSE(f.server.serverState.userInfo[kUser]->gameplayBlocked);
}

TEST_CASE(
  "Resume restores bound deleted allocations instead of replacing them",
  "[CampaignResume][SQLite][CampaignAllocation]")
{
  const bool ownerFirst = GENERATE(false, true);
  ResumeFiles files;
  uint32_t tombstone;
  Json history;
  {
    ResumeWorld initial(files);
    initial.Seed(true);
    tombstone = initial.tombstoneId;
    history = initial.History();
  }
  ResumeWorld f(files, ownerFirst);
  const auto saved = f.server.worldState.LookupFormByIdNoLoad(tombstone);
  REQUIRE(saved);
  REQUIRE(saved->AsObjectReference());
  REQUIRE(saved->AsObjectReference()->IsDeleted());
  f.Begin();
  f.ProjectionReady();
  const auto projection = f.Capture();
  bool found = false;
  for (const auto& reference : projection.at("manifest").at("references")) {
    if (reference.at("copy") == tombstone) {
      found = true;
      REQUIRE(reference.at("deleted") == true);
      REQUIRE(reference.at("original") == 0x8a986);
    }
  }
  REQUIRE(found);
  REQUIRE(f.Complete(projection));
  REQUIRE(f.History() == history);
  REQUIRE(f.server.worldState.LookupFormByIdNoLoad(tombstone) == saved);
}

TEST_CASE("Resume refuses incomplete allocations and changed saved geometry",
          "[CampaignResume][SQLite][CampaignAllocation]")
{
  enum class Damage
  {
    MissingCopy,
    IndexOnlyCopy,
    Geometry
  };
  const auto damage =
    GENERATE(Damage::MissingCopy, Damage::IndexOnlyCopy, Damage::Geometry);
  ResumeFiles original, damaged;
  FormDesc target;
  {
    ResumeWorld initial(original);
    initial.Seed();
    target = initial.Target().GetChangeForm().formDesc;
  }
  // Reconstruct a real database from saved rows, preserving the normal
  // PartOne restoration path and its registered-player disable behavior.
  Forms rows;
  {
    SqliteDatabase source(original.File());
    source.Iterate(
      [&](const MpChangeForm& saved) {
        auto row = saved;
        if (row.formDesc == target) {
          if (damage == Damage::MissingCopy)
            return;
          if (damage == Damage::Geometry)
            row.position.x += 1;
        }
        if (row.profileId == kProfile && damage == Damage::IndexOnlyCopy) {
          auto ledger = Json::parse(row.dynamicFields.GetValueDump(
            CampaignReferenceFactory::kAllocations));
          REQUIRE(ledger.at("copies").erase("6debb:Skyrim.esm") == 1);
          row.dynamicFields.SetValueDump(
            CampaignReferenceFactory::kAllocations, ledger.dump());
        }
        rows.push_back(std::move(row));
      },
      {});
    SqliteDatabase destination(damaged.File());
    destination.Upsert(std::move(rows));
  }
  ResumeWorld f(damaged);
  REQUIRE(f.Owner().IsDisabled());
  const auto history = f.History();
  const auto owner = MpChangeForm::ToJson(f.Owner().GetChangeForm());
  size_t presentations = 0;
  f.server.SetOnActorStreamIn(
    [&](const auto&, const auto&, auto&) { ++presentations; });
  if (damage == Damage::Geometry) {
    REQUIRE(f.Begin().at("status") == "Pending");
    f.Until(
      [&] {
        return f.server.GetCampaignResume(kUser).at("status") == "Failed";
      },
      "changed geometry rejected before resume commit");
  } else {
    REQUIRE_THROWS(f.Begin());
  }
  REQUIRE(f.server.serverState.userInfo[kUser]->gameplayBlocked);
  REQUIRE(f.Owner().IsDisabled());
  REQUIRE(f.History() == history);
  REQUIRE(MpChangeForm::ToJson(f.Owner().GetChangeForm()) == owner);
  REQUIRE(presentations == 0);
  REQUIRE(f.storage->submissions.empty());
  REQUIRE_THROWS(f.server.CaptureCampaignResumeProjection(
    kUser, f.server.GetPacketEpoch(kUser)));
}

TEST_CASE(
  "Superseded recovery receipts preserve each observer's published graph",
  "[CampaignResume][SQLite][Projection][SaveReceipt]")
{
  ResumeFiles files;
  ResumeWorld f(files);
  f.Seed(false, true);
  const auto history = f.History();
  const auto idx = f.Owner().GetIdx();
  const auto creates = [&] {
    return std::count_if(
      f.server.Messages().begin(), f.server.Messages().end(),
      [&](const auto& sent) {
        const auto* message =
          dynamic_cast<CreateActorMessage*>(sent.message.get());
        return sent.userId == 1 && message && message->refrId == kOwner;
      });
  };
  const auto destroys = [&] {
    return std::count_if(
      f.server.Messages().begin(), f.server.Messages().end(),
      [&](const auto& sent) {
        const auto* message =
          dynamic_cast<DestroyActorMessage*>(sent.message.get());
        return sent.userId == 1 && message && message->idx == idx;
      });
  };
  REQUIRE(creates() == 1);
  f.server.Messages().clear();
  f.storage->automatic = false;
  const auto first = f.storage->submissions.size();
  REQUIRE(f.server.DetachCampaignActor(kUser));
  f.Until([&] { return f.storage->submissions.size() == first + 1; },
          "detach snapshot pending before superseding resume");
  REQUIRE(f.Owner().IsDisabled());
  REQUIRE(creates() == 0);
  REQUIRE(destroys() == 0);
  DoDisconnect(f.server, kUser);
  REQUIRE(f.Begin().at("status") == "Pending");
  f.storage->Complete(first);
  f.storage->automatic = true;
  f.ProjectionReady();
  const auto projection = f.Capture();
  REQUIRE(f.Complete(projection));
  // The observer never saw the pending detach. Its still-current connection
  // already has the actor, so a compensating duplicate create is incorrect.
  REQUIRE(creates() == 0);
  REQUIRE(destroys() == 0);
  REQUIRE(f.History() == history);

  REQUIRE(f.server.DetachCampaignActor(kUser));
  f.Until([&] { return destroys() == 1; },
          "durable detach publishes one observer destroy");
  REQUIRE(f.Owner().IsDisabled());
  REQUIRE(creates() == 0);
  for (size_t step = 0; step < 3; ++step)
    f.Tick();
  REQUIRE(destroys() == 1);
  DoDisconnect(f.server, kUser);

  // Inverse ordering: an undelivered resume followed by detach must neither
  // expose the actor nor destroy an actor absent from the observer's baseline.
  f.server.Messages().clear();
  f.storage->automatic = false;
  const auto resumeSave = f.storage->submissions.size();
  f.Begin();
  f.Until([&] { return f.storage->submissions.size() == resumeSave + 1; },
          "resume snapshot pending before superseding detach");
  REQUIRE_FALSE(f.Owner().IsDisabled());
  REQUIRE(creates() == 0);
  REQUIRE(f.server.DetachCampaignActor(kUser));
  DoDisconnect(f.server, kUser);
  f.storage->Complete(resumeSave);
  f.storage->automatic = true;
  for (size_t step = 0; step < 8; ++step)
    f.Tick();
  REQUIRE(f.Owner().IsDisabled());
  REQUIRE(creates() == 0);
  REQUIRE(destroys() == 0);
  f.Begin();
  f.ProjectionReady();
  const auto latest = f.Capture();
  REQUIRE(f.Complete(latest));
  REQUIRE(creates() == 1);
  REQUIRE(destroys() == 0);
  REQUIRE(f.History() == history);
}

TEST_CASE(
  "Stale saved visitors do not prevent unrelated owners from recovering",
  "[CampaignResume][SQLite][GuestRestore]")
{
  const bool missingHost = GENERATE(false, true);
  const bool ownerFirst = GENERATE(false, true);
  constexpr uint32_t otherId = kOwner + 2;
  const std::string otherPhase =
    "campaign:" + std::to_string(otherId) + ":resume";
  ResumeFiles original, changed;
  Json otherArea;
  {
    ResumeWorld initial(original);
    initial.Seed(false, true);
    initial.EnrollVisitor();
    DoConnect(initial.server, 2);
    initial.server.CreateActor(otherId, initial.outside, 0, initial.cell,
                               kProfile + 2);
    auto& other = initial.server.worldState.GetFormAt<MpActor>(otherId);
    other.SetWorldInstance(otherPhase);
    initial.server.SetUserActor(2, otherId);
    other.SetRaceMenuOpen(true);
    auto appearance = jAppearance;
    appearance["idx"] = other.GetIdx();
    appearance["data"]["raceId"] = 0x13746;
    DoMessage(initial.server, 2, appearance);
    REQUIRE_FALSE(other.IsRaceMenuOpen());
    initial.server.serverState.userInfo[2]->gameplayBlocked = true;
    initial.server.BeginPacketEpoch(2);
    initial.server.serverState.userInfo[2]->gameplayBlocked = false;
    CampaignReferenceFactory::Ensure(other, { kTrigger }, 1);
    QuestScriptRuntime::ExecuteReferenceLoad(other, kTrigger);
    other.ForceSubscriptionsUpdate();
    initial.Tick();
    const auto position = initial.outside + NiPoint3{ 1, 0, 0 };
    initial.Move(position, otherId, 2); // A real zero-event durable baseline.
    REQUIRE(other.GetPos() == position);
    for (size_t step = 0; step < 3; ++step)
      initial.Tick();
    REQUIRE_FALSE(other.HasPendingCampaignMovement());
    otherArea = { { "id", "resume" },
                  { "worldOrCell", initial.cell },
                  { "center", { position.x, position.y, position.z } },
                  { "radius", 1 } };
  }
  Forms rows;
  {
    SqliteDatabase source(original.File());
    source.Iterate(
      [&](const MpChangeForm& saved) {
        auto row = saved;
        if (row.profileId == kProfile) {
          if (missingHost)
            return;
          // Its visitor still refers to the old phase. Recovery must classify
          // the visitor without requiring the host to remain in that phase.
          row.worldInstance =
            "campaign:" + std::to_string(kOwner) + ":elsewhere";
        }
        rows.push_back(std::move(row));
      },
      {});
    SqliteDatabase destination(changed.File());
    destination.Upsert(std::move(rows));
  }
  ResumeWorld f(changed, ownerFirst);
  REQUIRE(f.server.worldState.IsStorageRestoreReady());
  auto& guest = f.server.worldState.GetFormAt<MpActor>(kObserver);
  auto& other = f.server.worldState.GetFormAt<MpActor>(otherId);
  auto& jobs = f.server.worldState.GetQuestJobs();
  REQUIRE(guest.IsDisabled());
  REQUIRE_FALSE(jobs.HasResumeHold(guest));
  REQUIRE(other.IsDisabled());
  REQUIRE(jobs.HasResumeHold(other));
  const auto guestState = MpChangeForm::ToJson(guest.GetChangeForm());
  const auto otherJournal = QuestJournal::PersonalJobState(other).Save();
  for (size_t step = 0; step < 3; ++step)
    f.Tick();
  REQUIRE(QuestJournal::PersonalJobState(other).Save() == otherJournal);

  DoConnect(f.server, 1);
  f.server.serverState.userInfo[1]->gameplayBlocked = true;
  f.server.BeginPacketEpoch(1);
  REQUIRE_THROWS(f.server.ResumeCampaignActor(
    1, kObserver, guest.GetWorldInstance(), f.Area()));
  REQUIRE(f.server.GetUserActor(1) == 0);
  REQUIRE(f.server.serverState.userInfo[1]->gameplayBlocked);
  REQUIRE(MpChangeForm::ToJson(guest.GetChangeForm()) == guestState);

  DoConnect(f.server, 2);
  f.server.serverState.userInfo[2]->gameplayBlocked = true;
  f.server.BeginPacketEpoch(2);
  REQUIRE(f.server.ResumeCampaignActor(2, otherId, otherPhase, otherArea)
            .at("status") == "Pending");
  for (size_t step = 0;
       step < 32 && f.server.GetCampaignResume(2).at("status") == "Pending";
       ++step)
    f.Tick();
  REQUIRE(f.server.GetCampaignResume(2).at("status") == "ProjectionReady");
  const auto epoch = f.server.BeginPacketEpoch(2);
  const auto projection = f.server.CaptureCampaignResumeProjection(2, epoch);
  REQUIRE(f.server.CompleteCampaignResume(
    2, projection.at("attempt").get<std::string>(), epoch,
    projection.at("proof").get<std::string>(), [] {}));
  REQUIRE_FALSE(jobs.HasResumeHold(other));
  REQUIRE_FALSE(other.IsDisabled());
  const auto position = other.GetPos() + NiPoint3{ 1, 0, 0 };
  f.Move(position, otherId, 2);
  REQUIRE(other.GetPos() == position);
  for (size_t step = 0; step < 3; ++step)
    f.Tick();
  REQUIRE_FALSE(other.HasPendingCampaignMovement());
  REQUIRE(guest.IsDisabled());
  REQUIRE(MpChangeForm::ToJson(guest.GetChangeForm()) == guestState);
}

TEST_CASE("Malformed enrolled visitor phases still fail storage restoration",
          "[CampaignResume][SQLite][GuestRestore]")
{
  const auto phase =
    GENERATE(std::string("campaign:0:resume"),
             std::string("campaign:4294967296:resume"),
             std::string("campaign:01:resume"), std::string("campaign:1:"));
  ResumeFiles files;
  {
    ResumeWorld initial(files);
    initial.Seed(false, true);
    initial.EnrollVisitor();
  }
  Forms rows;
  {
    SqliteDatabase database(files.File());
    database.Iterate(
      [&](const MpChangeForm& saved) {
        if (saved.profileId == kProfile + 1) {
          auto row = saved;
          row.worldInstance = phase;
          rows.push_back(std::move(row));
        }
      },
      {});
    REQUIRE(rows.size() == 1);
    database.Upsert(std::move(rows));
  }
  PartOne server;
  server.AttachEspm(&GetEspmLoader());
  server.worldState.AttachScriptStorage(std::make_shared<ResumeScripts>());
  auto storage = std::make_shared<ResumeStorage>(files, true);
  REQUIRE_THROWS(server.AttachSaveStorage(storage));
  REQUIRE_FALSE(server.worldState.IsStorageRestoreReady());
  const auto form = server.worldState.LookupFormByIdNoLoad(kObserver);
  REQUIRE(form);
  const auto* guest = form->AsActor();
  REQUIRE(guest);
  REQUIRE(guest->IsDisabled());
  for (size_t step = 0; step < 3; ++step)
    server.Tick();
  REQUIRE_FALSE(server.worldState.IsStorageRestoreReady());
  REQUIRE(storage->submissions.empty());
}
