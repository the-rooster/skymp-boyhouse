#include "QuestScriptRuntime.h"
#include "CampaignReferenceBinding.h"
#include "CampaignReferenceScripts.h"
#include "CampaignVisibilityBarrier.h"
#include "QuestJournal.h"
#include "QuestVmState.h"
#include "TestUtils.hpp"
#include "papyrus-vm/Reader.h"
#include "libespm/QuestLifecycleData.h"
#include "save_storages/ISaveStorage.h"
#include "script_objects/CampaignAliasObject.h"
#include "script_objects/CampaignFormObject.h"
#include "script_objects/CampaignQuestObject.h"
#include "script_objects/MpFormGameObject.h"
#include "script_storages/BsaArchiveScriptStorage.h"
#include <fstream>
#include <limits>
#include <tuple>

extern espm::Loader& GetEspmLoader();

namespace {
constexpr uint32_t kQuest = 0x4e50d;
const char* kStateKey = "4e50d:Skyrim.esm/boyhousefixture";
VarValue Id(const char* name)
{
  return VarValue(VarValue::kType_Identifier, name);
}
struct Fixture
{
  PartOne server;
  espm::QuestScriptData metadata;
  std::shared_ptr<PexScript> fixture = std::make_shared<PexScript>();
  std::shared_ptr<PexScript> quest = std::make_shared<PexScript>();
  std::vector<std::shared_ptr<PexScript>> extraScripts;
  Fixture()
  {
    server.AttachEspm(&GetEspmLoader());
    metadata.version = 5;
    metadata.objectFormat = 2;
    metadata.scripts.push_back({ "BoyhouseFixture", 0, {} });
    metadata.fragments.push_back({ 10, 0, 1, "BoyhouseFixture", "Fragment" });
    metadata.fragments.push_back({ 20, 0, 1, "BoyhouseFixture", "Fragment" });
    fixture->source = "BoyhouseFixture";
    Object object;
    object.NameIndex = fixture->source;
    object.parentClassName = "Quest";
    object.variables.push_back({ "counter", "Int", 0, VarValue(0) });
    FunctionInfo fragment;
    fragment.valid = true;
    fragment.returnType = "None";
    fragment.locals.push_back({ "temp", "None" });
    fragment.code.instructions = {
      { FunctionCode::kOp_IAdd,
        { Id("counter"), Id("counter"), VarValue(1) } },
      { FunctionCode::kOp_CallMethod,
        { VarValue("SetObjectiveDisplayed"), Id("self"), Id("temp"),
          VarValue(2), VarValue(10), VarValue(true) } },
      { FunctionCode::kOp_Return, { VarValue::None() } }
    };
    object.states.push_back({ "", { { "Fragment", fragment } } });
    fixture->objectTable.push_back(object);
    quest->source = "Quest";
    Object base;
    base.NameIndex = "Quest";
    quest->objectTable.push_back(base);
  }
  MpActor& Add(uint32_t id)
  {
    server.CreateActor(id, { 1, 2, 3 }, 0, 0x3c);
    auto& actor = server.worldState.GetFormAt<MpActor>(id);
    actor.SetPropertyValueDump(QuestJournal::kEnabled,
                               "[\"4e50d:Skyrim.esm\"]", false, false);
    return actor;
  }
  bool Run(MpActor& actor, uint16_t stage = 10, uint32_t questId = kQuest)
  {
    return QuestScriptRuntime::ExecuteStage(actor, questId, stage, metadata,
                                            Scripts());
  }
  std::vector<PexScript::Lazy> Scripts()
  {
    std::vector<PexScript::Lazy> scripts = {
      { fixture->source, [pex = fixture] { return pex; } },
      { quest->source, [pex = quest] { return pex; } }
    };
    for (const auto& script : extraScripts)
      scripts.push_back({ script->source, [script] { return script; } });
    return scripts;
  }
  FunctionInfo& Fragment()
  {
    return fixture->objectTable[0].states[0].functions[0].function;
  }
};
}

TEST_CASE("Quest objects have symmetric campaign-specific Papyrus identities",
          "[Quest][QuestVM]")
{
  const auto record = GetEspmLoader().GetBrowser().LookupById(kQuest);
  CampaignQuestObject a(record, 1), same(record, 1), b(record, 2);
  EspmGameObject global(record);
  REQUIRE(a.EqualsByValue(same));
  REQUIRE_FALSE(a.EqualsByValue(b));
  REQUIRE_FALSE(b.EqualsByValue(a));
  REQUIRE_FALSE(global.EqualsByValue(a));
  REQUIRE_FALSE(a.EqualsByValue(global));
  REQUIRE(std::string(a.GetStringID()) != b.GetStringID());
}

TEST_CASE(
  "Fragments persist isolated variables and duplicate stages do not rerun",
  "[Quest][QuestVM]")
{
  Fixture f;
  auto& a = f.Add(0xff000001);
  auto& b = f.Add(0xff000002);
  REQUIRE(f.Run(a));
  REQUIRE(QuestJournal::Get(a, kQuest)["objectives"]["10"]["displayed"] ==
          true);
  REQUIRE(
    QuestJournal::ScriptState(
      a)["scripts"][kStateKey]["values"]["boyhousefixture/counter"]["value"] ==
    1);
  REQUIRE_FALSE(QuestJournal::Snapshot(a).contains("vm"));
  const auto snapshot = QuestJournal::Snapshot(a);
  REQUIRE_FALSE(f.Run(a));
  REQUIRE(QuestJournal::Snapshot(a) == snapshot);
  REQUIRE(f.Run(a, 20));
  REQUIRE(
    QuestJournal::ScriptState(
      a)["scripts"][kStateKey]["values"]["boyhousefixture/counter"]["value"] ==
    2);
  REQUIRE(f.Run(b));
  REQUIRE(
    QuestJournal::ScriptState(
      b)["scripts"][kStateKey]["values"]["boyhousefixture/counter"]["value"] ==
    1);
  Fixture restarted;
  auto& restored = restarted.Add(a.GetFormId());
  auto json = MpChangeForm::ToJson(a.GetChangeForm()).dump();
  simdjson::dom::parser parser;
  auto parsed = parser.parse(json).value();
  restored.ApplyChangeForm(MpChangeForm::JsonToChangeForm(parsed));
  REQUIRE(QuestJournal::ScriptState(restored) == QuestJournal::ScriptState(a));
}

TEST_CASE("Guest fragments update only the owner's script variables",
          "[Quest][QuestVM]")
{
  Fixture f;
  auto& a = f.Add(0xff000001);
  auto& b = f.Add(0xff000002);
  const auto instance =
    "campaign:" + std::to_string(a.GetFormId()) + ":fixture";
  a.SetWorldInstance(instance);
  b.SetWorldInstance(instance);
  REQUIRE(f.Run(b));
  REQUIRE(
    QuestJournal::ScriptState(
      a)["scripts"][kStateKey]["values"]["boyhousefixture/counter"]["value"] ==
    1);
  b.SetWorldInstance("");
  REQUIRE(QuestJournal::ScriptState(b).empty());
  REQUIRE(QuestJournal::Get(b, kQuest)["stage"] == 0);
}

TEST_CASE("Quest reset discards script variables before the next run",
          "[Quest][QuestVM]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  REQUIRE(f.Run(actor));
  REQUIRE(
    QuestJournal::Apply(actor, { { "quest", kQuest }, { "op", "reset" } }));
  REQUIRE(QuestJournal::ScriptState(actor).empty());
  REQUIRE(f.Run(actor));
  REQUIRE(
    QuestJournal::ScriptState(actor)["scripts"][kStateKey]["values"]
                                    ["boyhousefixture/counter"]["value"] == 1);
}

TEST_CASE(
  "Conditional log fragments read the host history while a guest visits",
  "[Quest][QuestVM]")
{
  Fixture f;
  auto& owner = f.Add(0xff000001);
  auto& guest = f.Add(0xff000002);
  // MQ102 stage 10 has one unconditional entry and two GetStageDone branches.
  f.metadata.fragments.push_back({ 10, 1, 1, "BoyhouseFixture", "Fragment" });
  f.metadata.fragments.push_back({ 10, 2, 1, "BoyhouseFixture", "Fragment" });
  owner.SetPropertyValueDump(QuestJournal::kEnabled,
                             "[\"4e50d:Skyrim.esm\",\"2bf9c:Skyrim.esm\"]",
                             false, false);
  REQUIRE(QuestJournal::Apply(
    owner, { { "quest", 0x2bf9c }, { "op", "setStage" }, { "index", 10 } }));
  const auto instance =
    "campaign:" + std::to_string(owner.GetFormId()) + ":fixture";
  owner.SetWorldInstance(instance);
  guest.SetWorldInstance(instance);
  REQUIRE(f.Run(guest));
  REQUIRE(
    QuestJournal::ScriptState(owner)["scripts"][kStateKey]["values"]
                                    ["boyhousefixture/counter"]["value"] == 2);
  guest.SetWorldInstance("");
  REQUIRE(f.Run(guest));
  REQUIRE(
    QuestJournal::ScriptState(guest)["scripts"][kStateKey]["values"]
                                    ["boyhousefixture/counter"]["value"] == 1);
}

TEST_CASE("Runaway fragments roll back and do not poison later executions",
          "[Quest][QuestVM]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  const auto before = QuestJournal::Snapshot(actor);
  const auto original = f.Fragment().code.instructions.back();
  f.Fragment().code.instructions.back() = { FunctionCode::kOp_Jump,
                                            { VarValue(0) } };
  REQUIRE_THROWS_WITH(f.Run(actor),
                      Catch::Matchers::ContainsSubstring("quota exceeded"));
  REQUIRE(QuestJournal::Snapshot(actor) == before);
  f.Fragment().code.instructions.back() = original;
  REQUIRE(f.Run(actor));
}

TEST_CASE(
  "Property writes persist on the intended script and None never aliases self",
  "[Quest][QuestVM]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  Object::PropInfo property;
  property.name = "Count";
  property.type = "Int";
  property.flags = 7;
  property.autoVarName = "counter";
  f.fixture->objectTable[0].properties.push_back(property);
  auto& code = f.Fragment().code.instructions;
  code.insert(code.end() - 1,
              { FunctionCode::kOp_PropSet,
                { VarValue("Count"), Id("self"), VarValue(42) } });
  REQUIRE(f.Run(actor));
  REQUIRE(QuestJournal::ScriptState(
            actor)["scripts"][kStateKey]["values"]["boyhousefixture/counter"]
                  ["value"] == 42);
  const auto before = QuestJournal::Snapshot(actor);
  f.Fragment().locals.push_back({ "target", "Quest" });
  code[2].args[1] = Id("target");
  REQUIRE_THROWS_WITH(f.Run(actor, 20),
                      Catch::Matchers::ContainsSubstring("property target"));
  REQUIRE(QuestJournal::Snapshot(actor) == before);
}

TEST_CASE("Fragments keep shared arrays across saved campaign reloads",
          "[Quest][QuestVM]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  auto& object = f.fixture->objectTable[0];
  object.variables.push_back({ "a", "Int[]", 0, VarValue::None() });
  object.variables.push_back({ "b", "Int[]", 0, VarValue::None() });
  auto& code = f.Fragment().code.instructions;
  code = { { FunctionCode::kOp_ArrayCreate, { Id("a"), VarValue(2) } },
           { FunctionCode::kOp_ArraySetElement,
             { Id("a"), VarValue(0), VarValue(7) } },
           { FunctionCode::kOp_Assign, { Id("b"), Id("a") } },
           { FunctionCode::kOp_Return, { VarValue::None() } } };
  REQUIRE(f.Run(actor));

  Fixture restarted;
  auto& restored = restarted.Add(actor.GetFormId());
  const auto dump = MpChangeForm::ToJson(actor.GetChangeForm()).dump();
  simdjson::dom::parser parser;
  auto parsed = parser.parse(dump).value();
  restored.ApplyChangeForm(MpChangeForm::JsonToChangeForm(parsed));
  restarted.fixture = f.fixture;
  restarted.Fragment().code.instructions = {
    { FunctionCode::kOp_ArraySetElement,
      { Id("a"), VarValue(0), VarValue(8) } },
    { FunctionCode::kOp_ArrayGetElement,
      { Id("counter"), Id("b"), VarValue(0) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  REQUIRE(restarted.Run(restored, 20));
  const auto saved = QuestJournal::ScriptState(restored);
  REQUIRE(saved["scripts"][kStateKey]["values"]["boyhousefixture/counter"]
               ["value"] == 8);
  REQUIRE(saved["arrays"].size() == 1);
  // A failed later fragment must not leak an in-memory heap mutation.
  const auto before =
    restored.GetDynamicFields().GetValueDump(QuestJournal::kProperty);
  restarted.metadata.fragments.push_back(
    { 100, 0, 1, "BoyhouseFixture", "Fragment" });
  restarted.Fragment().code.instructions.insert(
    restarted.Fragment().code.instructions.end() - 1,
    { FunctionCode::kOp_CallMethod,
      { VarValue("UnsupportedNative"), Id("self"), Id("temp"),
        VarValue(0) } });
  restarted.Fragment().code.instructions[0].args[2] = VarValue(99);
  REQUIRE_THROWS(restarted.Run(restored, 100));
  REQUIRE(restored.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          before);
}

TEST_CASE(
  "Native failure after a fragment mutation rolls back journal and variables",
  "[Quest][QuestVM]")
{
  Fixture f;
  auto& a = f.Add(0xff000001);
  const auto before = QuestJournal::Snapshot(a);
  auto& code = f.Fragment().code.instructions;
  code.insert(code.end() - 1,
              { FunctionCode::kOp_CallMethod,
                { VarValue("UnsupportedNative"), Id("self"), Id("temp"),
                  VarValue(0) } });
  REQUIRE_THROWS(f.Run(a));
  REQUIRE(QuestJournal::Snapshot(a) == before);
  REQUIRE(QuestJournal::ScriptState(a).empty());
  code.erase(code.end() - 2);
  REQUIRE(f.Run(a));
}

TEST_CASE(
  "Missing bindings and unsupported lifecycle calls cannot silently complete",
  "[Quest][QuestVM]")
{
  Fixture f;
  auto& a = f.Add(0xff000001);
  const auto before = QuestJournal::Snapshot(a);
  f.metadata.fragments[0].function = "MissingFragment";
  REQUIRE_THROWS(f.Run(a));
  REQUIRE(QuestJournal::Snapshot(a) == before);
  f.metadata.fragments[0].function = "Fragment";
  f.Fragment().code.instructions[1] = { FunctionCode::kOp_CallMethod,
                                        { VarValue("Stop"), Id("self"),
                                          Id("temp"), VarValue(0) } };
  REQUIRE_THROWS(f.Run(a));
  REQUIRE(QuestJournal::Snapshot(a) == before);
  f.metadata.scripts[0].name = "MissingPex";
  REQUIRE_THROWS(f.Run(a));
  REQUIRE(QuestJournal::Snapshot(a) == before);
}

TEST_CASE(
  "Quest transactions reject context changes and cross-campaign writes",
  "[Quest][QuestVM]")
{
  Fixture f;
  auto& a = f.Add(0xff000001);
  auto& b = f.Add(0xff000002);
  const auto before = QuestJournal::Snapshot(a);
  REQUIRE_THROWS(QuestJournal::Atomic(a, [&] {
    QuestJournal::Apply(a, { { "quest", kQuest }, { "op", "start" } });
    QuestJournal::Apply(b, { { "quest", kQuest }, { "op", "start" } });
  }));
  REQUIRE(QuestJournal::Snapshot(a) == before);
  REQUIRE_THROWS(QuestJournal::Atomic(a, [&] {
    QuestJournal::Apply(a, { { "quest", kQuest }, { "op", "start" } });
    a.SetWorldInstance("campaign:4278190081:fixture");
  }));
  REQUIRE(QuestJournal::Get(a, kQuest)["running"] == false);
  REQUIRE_THROWS(QuestJournal::SetScriptState(a, nlohmann::json::object()));
}

TEST_CASE(
  "Owned original quest fragment runs through VMAD and compressed PEX storage",
  "[Quest][QuestVM][OwnedFragment]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  auto storage = std::make_shared<BsaArchiveScriptStorage>(
    (std::filesystem::path(GetDataDir()) / "Skyrim - Misc.bsa")
      .string()
      .c_str());
  f.server.worldState.AttachScriptStorage(storage);
  const auto before = QuestJournal::Snapshot(actor);
  REQUIRE_THROWS_WITH(
    QuestScriptRuntime::ExecuteStage(actor, kQuest, 10),
    Catch::Matchers::ContainsSubstring("adapter is not enabled"));
  REQUIRE(QuestJournal::Snapshot(actor) == before);
  const auto record =
    f.server.worldState.GetEspm().GetBrowser().LookupById(kQuest);
  const auto data = espm::QuestScriptData::Read(
    record.rec, f.server.worldState.GetEspmCache());
  REQUIRE(std::count_if(data.fragments.begin(), data.fragments.end(),
                        [](const auto& f) { return f.stage == 100; }) > 0);
  REQUIRE(QuestScriptRuntime::ExecuteStage(actor, kQuest, 100));
  REQUIRE(QuestJournal::Get(actor, kQuest)["stage"] == 100);
  REQUIRE_FALSE(QuestJournal::ScriptState(actor).empty());

  // An original fragment with a visible journal effect, executed by a guest.
  const uint32_t objectiveQuest = 0x2d512;
  actor.SetPropertyValueDump(QuestJournal::kEnabled,
                             "[\"4e50d:Skyrim.esm\",\"2d512:Skyrim.esm\"]",
                             false, false);
  auto& guest = f.Add(0xff000002);
  const auto instance =
    "campaign:" + std::to_string(actor.GetFormId()) + ":fixture";
  actor.SetWorldInstance(instance);
  guest.SetWorldInstance(instance);
  REQUIRE(QuestScriptRuntime::ExecuteStage(guest, objectiveQuest, 5));
  REQUIRE(QuestJournal::Get(
            actor, objectiveQuest)["objectives"]["5"]["displayed"] == true);
  const auto progress = QuestJournal::Snapshot(actor);
  REQUIRE_FALSE(QuestScriptRuntime::ExecuteStage(guest, objectiveQuest, 5));
  REQUIRE(QuestJournal::Snapshot(actor) == progress);
  guest.SetWorldInstance("");
  REQUIRE(QuestJournal::Get(guest, objectiveQuest)["objectives"].empty());
}

TEST_CASE("Reusing a character ID cannot receive an old quest transaction",
          "[Quest][QuestVM]")
{
  Fixture f;
  const uint32_t id = 0xff000001;
  auto& actor = f.Add(id);
  REQUIRE_THROWS_WITH(
    QuestJournal::Atomic(
      actor,
      [&] {
        QuestJournal::Apply(actor, { { "quest", kQuest }, { "op", "start" } });
        f.server.worldState.DestroyForm(id);
        f.Add(id);
      }),
    Catch::Matchers::ContainsSubstring("context changed"));
  REQUIRE(QuestJournal::Snapshot(
            f.server.worldState.GetFormAt<MpActor>(id))["quests"]
            .empty());
}

TEST_CASE("PEX reader rejects truncation and malformed headers instead of "
          "reading past input",
          "[Quest][QuestVM]")
{
  std::ifstream stream(std::filesystem::path(TEST_PEX_DIR) / "quest.pex",
                       std::ios::binary);
  REQUIRE(stream.good());
  const std::vector<uint8_t> bytes{ std::istreambuf_iterator<char>(stream),
                                    std::istreambuf_iterator<char>() };
  REQUIRE(Reader(std::vector<std::vector<uint8_t>>{ bytes })
            .GetSourceStructures()
            .size() == 1);
  for (const size_t size : { size_t(0), size_t(8), size_t(16),
                             bytes.size() / 2, bytes.size() - 1 }) {
    std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + size);
    REQUIRE_THROWS(Reader(std::vector<std::vector<uint8_t>>{ truncated }));
  }
  auto bad = bytes;
  bad[0] = 0;
  REQUIRE_THROWS(Reader(std::vector<std::vector<uint8_t>>{ bad }));
}

TEST_CASE("PEX without debug info keeps parameters separate from locals",
          "[Quest][QuestVM]")
{
  std::vector<uint8_t> bytes;
  auto number = [&](uint64_t value, unsigned width) {
    for (unsigned i = width; i > 0; --i)
      bytes.push_back(uint8_t(value >> ((i - 1) * 8)));
  };
  auto string = [&](const std::string& value) {
    number(value.size(), 2);
    bytes.insert(bytes.end(), value.begin(), value.end());
  };
  number(0xfa57c0de, 4);
  number(3, 1);
  number(2, 1);
  number(1, 2);
  number(0, 8);
  string("Fixture.psc");
  string("");
  string("");
  const std::vector<std::string> strings = { "Fixture", "",        "None",
                                             "Int",     "Example", "argument",
                                             "local" };
  number(strings.size(), 2);
  for (const auto& value : strings)
    string(value);
  number(0, 1); // no debug info, so no timestamp/function table follows
  number(0, 2); // user flags
  number(1, 2); // objects
  number(0, 2); // object name
  const auto objectSize = bytes.size();
  number(0, 4);
  number(1, 2);
  number(1, 2);
  number(0, 4);
  number(1, 2); // parent, docs, flags, state
  number(0, 2);
  number(0, 2);
  number(1, 2); // variables, properties, states
  number(1, 2);
  number(1, 2);
  number(4, 2); // state name, functions, function name
  number(2, 2);
  number(1, 2);
  number(0, 4);
  number(0, 1); // return, docs, flags
  number(1, 2);
  number(5, 2);
  number(3, 2); // one Int parameter
  number(1, 2);
  number(6, 2);
  number(3, 2); // one Int local
  number(1, 2);
  number(FunctionCode::kOp_Return, 1);
  number(0, 1);
  const auto size = bytes.size() - objectSize;
  for (unsigned i = 0; i < 4; ++i)
    bytes[objectSize + i] = uint8_t(size >> ((3 - i) * 8));
  const auto parsed = Reader(std::vector<std::vector<uint8_t>>{ bytes })
                        .GetSourceStructures()
                        .at(0);
  const auto& function =
    parsed->objectTable.at(0).states.at(0).functions.at(0).function;
  REQUIRE(function.params.size() == 1);
  REQUIRE(function.params.at(0).name == "argument");
  REQUIRE(function.locals.size() == 1);
  REQUIRE(function.locals.at(0).name == "local");
}

TEST_CASE("Owned vanilla PEX files parse after archive decompression",
          "[Quest][QuestVM][OwnedPex]")
{
  const auto path = std::filesystem::path(GetDataDir()) / "Skyrim - Misc.bsa";
  BsaArchiveScriptStorage storage(path.string().c_str());
  const auto& names = storage.ListScripts(false);
  REQUIRE(names.size() > 1000);
  for (const auto& name : names) {
    INFO("script=" << name.c_str());
    const auto bytes = storage.GetScriptPex(name.c_str());
    const auto parsed =
      Reader(std::vector<std::vector<uint8_t>>{ bytes }).GetSourceStructures();
    REQUIRE(parsed.size() == 1);
    REQUIRE(parsed.front()->objectTable.size() == 1);
    const auto hash = parsed.front()->ExecutionHash();
    REQUIRE(hash.size() == 64);
    REQUIRE(hash.find_first_not_of("0123456789abcdef") == std::string::npos);
  }
}

TEST_CASE(
  "Nested original stages execute once in the same campaign transaction",
  "[Quest][QuestVM]")
{
  Fixture f;
  auto& host = f.Add(0xff000001);
  auto& guest = f.Add(0xff000002);
  const auto instance =
    "campaign:" + std::to_string(host.GetFormId()) + ":fixture";
  host.SetWorldInstance(instance);
  guest.SetWorldInstance(instance);
  // Both 10 and 20 call 20. The second call observes the in-transaction
  // visited stage and returns false; it must neither recurse forever nor run
  // twice.
  auto& code = f.Fragment().code.instructions;
  code.insert(code.end() - 1,
              { FunctionCode::kOp_CallMethod,
                { VarValue("SetStage"), Id("self"), Id("temp"), VarValue(1),
                  VarValue(20) } });
  REQUIRE(f.Run(guest));
  const auto journal = QuestJournal::Get(host, kQuest);
  REQUIRE(journal["stages"] == nlohmann::json::array({ 10, 20 }));
  REQUIRE(
    QuestJournal::ScriptState(host)["scripts"][kStateKey]["values"]
                                   ["boyhousefixture/counter"]["value"] == 2);
  guest.SetWorldInstance("");
  REQUIRE(QuestJournal::Get(guest, kQuest)["stages"].empty());
  REQUIRE(QuestJournal::ScriptState(guest).empty());
}

TEST_CASE("Failure after a nested stage rolls back every stage and variable",
          "[Quest][QuestVM]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  auto& object = f.fixture->objectTable[0];
  auto parent = f.Fragment();
  parent.code.instructions = {
    { FunctionCode::kOp_CallMethod,
      { VarValue("SetCurrentStageID"), Id("self"), Id("temp"), VarValue(1),
        VarValue(20) } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("UnsupportedNative"), Id("self"), Id("temp"), VarValue(0) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  object.states[0].functions.push_back({ "Parent", parent });
  f.metadata.fragments[0].function = "Parent";
  const auto before =
    actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty);
  REQUIRE_THROWS(f.Run(actor));
  REQUIRE(actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          before);
  REQUIRE(QuestJournal::Get(actor, kQuest)["stages"].empty());
  REQUIRE(QuestJournal::ScriptState(actor).empty());
  // The failed transaction cannot poison the next VM.
  object.states[0].functions.back().function.code.instructions.erase(
    object.states[0].functions.back().function.code.instructions.begin() + 1);
  REQUIRE(f.Run(actor));
  REQUIRE(QuestJournal::Get(actor, kQuest)["stages"] ==
          nlohmann::json::array({ 10, 20 }));
}

TEST_CASE("Nested stages validate indices without recording partial progress",
          "[Quest][QuestVM]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  auto& code = f.Fragment().code.instructions;
  const auto index = GENERATE(-1, 65536, 65535);
  code.insert(code.end() - 1,
              { FunctionCode::kOp_CallMethod,
                { VarValue("SetStage"), Id("self"), Id("temp"), VarValue(1),
                  VarValue(index) } });
  REQUIRE_THROWS(f.Run(actor));
  REQUIRE(QuestJournal::Get(actor, kQuest)["stages"].empty());
  REQUIRE(QuestJournal::ScriptState(actor).empty());
}

TEST_CASE(
  "Nested stages can advance a different quest without losing atomicity",
  "[Quest][QuestVM]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  // An owned helper quest with actual stages and no fragments/aliases.
  constexpr uint32_t other = 0xc07c1;
  espm::QuestScriptValue value;
  value.type = 1;
  value.formId = other;
  f.metadata.scripts[0].properties.push_back({ "Target", 1, value });
  f.fixture->objectTable[0].variables.push_back(
    { "::Target_var", "Quest", 0, VarValue::None() });
  f.Fragment().code.instructions = {
    { FunctionCode::kOp_CallMethod,
      { VarValue("SetStage"), Id("::Target_var"), Id("temp"), VarValue(1),
        VarValue(100) } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("UnsupportedNative"), Id("self"), Id("temp"), VarValue(0) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  const auto before =
    actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty);
  // Another quest is not automatically opted in by a script reference.
  REQUIRE_THROWS_WITH(
    f.Run(actor),
    Catch::Matchers::ContainsSubstring("adapter is not enabled"));
  REQUIRE(actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          before);
  actor.SetPropertyValueDump(QuestJournal::kEnabled,
                             "[\"4e50d:Skyrim.esm\",\"c07c1:Skyrim.esm\"]",
                             false, false);
  REQUIRE_THROWS_WITH(f.Run(actor),
                      Catch::Matchers::ContainsSubstring("UnsupportedNative"));
  REQUIRE(actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          before);
  REQUIRE(QuestJournal::Get(actor, other)["stages"].empty());
  f.Fragment().code.instructions.erase(f.Fragment().code.instructions.begin() +
                                       1);
  REQUIRE(f.Run(actor));
  REQUIRE(QuestJournal::Get(actor, other)["stages"] ==
          nlohmann::json::array({ 100 }));
  REQUIRE(QuestJournal::Get(actor, kQuest)["stages"] ==
          nlohmann::json::array({ 10 }));
}

namespace {
constexpr uint32_t kGlobal = 0x17424; // DA16ErandurCheckpoint, default 0.
const char* kGlobalKey = "17424:Skyrim.esm";
void GlobalFixture(Fixture& f)
{
  espm::QuestScriptValue value;
  value.type = 1;
  value.formId = kGlobal;
  f.metadata.scripts[0].properties.push_back({ "Checkpoint", 1, value });
  f.fixture->objectTable[0].variables.push_back(
    { "::Checkpoint_var", "GlobalVariable", 0, VarValue::None() });
  f.Fragment().code.instructions = {
    { FunctionCode::kOp_CallMethod,
      { VarValue("GetValueInt"), Id("::Checkpoint_var"), Id("counter"),
        VarValue(0) } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("Mod"), Id("::Checkpoint_var"), Id("temp"), VarValue(1),
        VarValue(5.0) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
}
}

TEST_CASE(
  "Global variables persist per campaign and guest writes use the owner",
  "[Quest][QuestVM][QuestGlobals]")
{
  Fixture f;
  GlobalFixture(f);
  auto& host = f.Add(0xff000001);
  auto& guest = f.Add(0xff000002);
  const auto instance =
    "campaign:" + std::to_string(host.GetFormId()) + ":fixture";
  host.SetWorldInstance(instance);
  guest.SetWorldInstance(instance);
  REQUIRE(f.Run(guest));
  REQUIRE(QuestJournal::ScriptState(host)["globals"][kGlobalKey] == 5.0);
  REQUIRE(
    QuestJournal::ScriptState(host)["scripts"][kStateKey]["values"]
                                   ["boyhousefixture/counter"]["value"] == 0);
  REQUIRE(f.Run(guest, 20));
  REQUIRE(QuestJournal::ScriptState(host)["globals"][kGlobalKey] == 10.0);
  guest.SetWorldInstance("");
  REQUIRE(QuestJournal::ScriptState(guest).empty());
  REQUIRE(f.Run(guest));
  REQUIRE(QuestJournal::ScriptState(guest)["globals"][kGlobalKey] == 5.0);
  REQUIRE(QuestJournal::ScriptState(host)["globals"][kGlobalKey] == 10.0);
  REQUIRE_FALSE(QuestJournal::Snapshot(host).contains("globals"));
}

TEST_CASE(
  "Character reload keeps campaign globals and global object properties",
  "[Quest][QuestVM][QuestGlobals]")
{
  Fixture f;
  GlobalFixture(f);
  auto& actor = f.Add(0xff000001);
  REQUIRE(f.Run(actor));
  Fixture restarted;
  restarted.fixture = f.fixture;
  restarted.metadata = f.metadata;
  auto& restored = restarted.Add(actor.GetFormId());
  const auto dump = MpChangeForm::ToJson(actor.GetChangeForm()).dump();
  simdjson::dom::parser parser;
  auto parsed = parser.parse(dump).value();
  restored.ApplyChangeForm(MpChangeForm::JsonToChangeForm(parsed));
  REQUIRE(restarted.Run(restored, 20));
  REQUIRE(QuestJournal::ScriptState(restored)["globals"][kGlobalKey] == 10.0);
  REQUIRE(QuestJournal::ScriptState(
            restored)["scripts"][kStateKey]["values"]
                     ["boyhousefixture/counter"]["value"] == 5);
}

TEST_CASE("Quest reset leaves campaign global effects intact",
          "[Quest][QuestVM][QuestGlobals]")
{
  Fixture f;
  GlobalFixture(f);
  auto& actor = f.Add(0xff000001);
  REQUIRE(f.Run(actor));
  REQUIRE(
    QuestJournal::Apply(actor, { { "quest", kQuest }, { "op", "reset" } }));
  REQUIRE(QuestJournal::ScriptState(actor)["scripts"].empty());
  REQUIRE(QuestJournal::ScriptState(actor)["globals"][kGlobalKey] == 5.0);
  REQUIRE(f.Run(actor));
  REQUIRE(QuestJournal::ScriptState(actor)["globals"][kGlobalKey] == 10.0);
}

TEST_CASE("Failed fragments cannot change saved campaign globals",
          "[Quest][QuestVM][QuestGlobals]")
{
  Fixture f;
  GlobalFixture(f);
  auto& actor = f.Add(0xff000001);
  REQUIRE(f.Run(actor));
  const auto before =
    actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty);
  auto& code = f.Fragment().code.instructions;
  code.insert(code.end() - 1,
              { FunctionCode::kOp_CallMethod,
                { VarValue("UnsupportedNative"), Id("self"), Id("temp"),
                  VarValue(0) } });
  REQUIRE_THROWS(f.Run(actor, 20));
  REQUIRE(actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          before);
}

TEST_CASE("Global value natives preserve float and integer calling semantics",
          "[Quest][QuestVM][QuestGlobals]")
{
  Fixture f;
  GlobalFixture(f);
  auto& actor = f.Add(0xff000001);
  auto& code = f.Fragment().code.instructions;
  code = { { FunctionCode::kOp_CallMethod,
             { VarValue("SetValue"), Id("::Checkpoint_var"), Id("temp"),
               VarValue(1), VarValue(12.75) } },
           { FunctionCode::kOp_CallMethod,
             { VarValue("GetValueInt"), Id("::Checkpoint_var"), Id("counter"),
               VarValue(0) } },
           { FunctionCode::kOp_Return, { VarValue::None() } } };
  REQUIRE(f.Run(actor));
  REQUIRE(QuestJournal::ScriptState(actor)["globals"][kGlobalKey] == 12.75);
  REQUIRE(QuestJournal::ScriptState(
            actor)["scripts"][kStateKey]["values"]["boyhousefixture/counter"]
                  ["value"] == 12);
  code[0].args[0] = VarValue("SetValueInt");
  code[0].args[4] = VarValue(-4);
  code[1].args[0] = VarValue("GetValue");
  REQUIRE(f.Run(actor, 20));
  REQUIRE(QuestJournal::ScriptState(actor)["globals"][kGlobalKey] == -4.0);
}

TEST_CASE("Invalid global values roll back the entire stage",
          "[Quest][QuestVM][QuestGlobals]")
{
  Fixture f;
  GlobalFixture(f);
  auto& actor = f.Add(0xff000001);
  auto& code = f.Fragment().code.instructions;
  SECTION("wrong argument type")
  {
    code[1].args[4] = VarValue(true);
  }
  SECTION("nonfinite value")
  {
    code[1].args[4] = VarValue(std::numeric_limits<double>::infinity());
  }
  SECTION("float32 overflow")
  {
    code[1].args[4] = VarValue(1e100);
  }
  REQUIRE_THROWS(f.Run(actor));
  REQUIRE(QuestJournal::Get(actor, kQuest)["stages"].empty());
  REQUIRE(QuestJournal::ScriptState(actor).empty());
}

TEST_CASE(
  "Original global conditions see writes from earlier nested fragments",
  "[Quest][QuestVM][QuestGlobals]")
{
  Fixture f;
  GlobalFixture(f);
  constexpr uint32_t quest = 0x4a37b; // DA08: stage 60 branches on 216a9.
  f.metadata.scripts[0].properties[0].value.formId = 0x216a9;
  f.metadata.fragments = { { 10, 0, 1, "BoyhouseFixture", "Parent" },
                           { 60, 0, 1, "BoyhouseFixture", "One" },
                           { 60, 1, 1, "BoyhouseFixture", "Zero" } };
  auto& functions = f.fixture->objectTable[0].states[0].functions;
  const auto branch = GENERATE(0, 1);
  auto parent = f.Fragment();
  parent.code.instructions = {
    { FunctionCode::kOp_CallMethod,
      { VarValue("SetValue"), Id("::Checkpoint_var"), Id("temp"), VarValue(1),
        VarValue(double(branch)) } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("SetStage"), Id("self"), Id("temp"), VarValue(1),
        VarValue(60) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  auto one = f.Fragment(), zero = f.Fragment();
  one.code.instructions = {
    { FunctionCode::kOp_Assign, { Id("counter"), VarValue(11) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  zero.code.instructions = {
    { FunctionCode::kOp_Assign, { Id("counter"), VarValue(22) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  functions.push_back({ "Parent", parent });
  functions.push_back({ "One", one });
  functions.push_back({ "Zero", zero });
  auto& owner = f.Add(0xff000001);
  auto& guest = f.Add(0xff000002);
  owner.SetPropertyValueDump(QuestJournal::kEnabled, "[\"4a37b:Skyrim.esm\"]",
                             false, false);
  const auto instance =
    "campaign:" + std::to_string(owner.GetFormId()) + ":fixture";
  owner.SetWorldInstance(instance);
  guest.SetWorldInstance(instance);
  REQUIRE(f.Run(guest, 10, quest));
  const auto state = QuestJournal::ScriptState(owner);
  REQUIRE(state["globals"]["216a9:Skyrim.esm"] == branch);
  REQUIRE(state["scripts"]["4a37b:Skyrim.esm/boyhousefixture"]["values"]
               ["boyhousefixture/counter"]["value"] == (branch ? 11 : 22));
  REQUIRE(QuestJournal::Get(owner, quest)["completed"] == true);
  guest.SetWorldInstance("");
  REQUIRE(QuestJournal::ScriptState(guest).empty());
}

namespace {
void BaseScripts(Fixture& f)
{
  f.quest->objectTable[0].parentClassName = "Form";
  for (const auto& [name, parent] :
       std::vector<std::pair<std::string, std::string>>{
         { "Form", "" },
         { "ObjectReference", "Form" },
         { "Actor", "ObjectReference" },
         { "ActorBase", "Form" },
         { "Alias", "" },
         { "ReferenceAlias", "Alias" },
         { "LocationAlias", "Alias" },
         { "Location", "Form" },
         { "Scene", "Form" },
         { "TopicInfo", "Form" } }) {
    auto script = std::make_shared<PexScript>();
    script->source = name;
    Object object;
    object.NameIndex = name;
    object.parentClassName = parent;
    script->objectTable.push_back(object);
    f.extraScripts.push_back(script);
  }
}
void AddOwnedScript(Fixture& f, const std::string& name)
{
  BsaArchiveScriptStorage storage(
    (std::filesystem::path(GetDataDir()) / "Skyrim - Misc.bsa")
      .string()
      .c_str());
  storage.ListScripts(false);
  auto scripts = Reader(std::vector<std::vector<uint8_t>>{
                          storage.GetScriptPex(name.c_str()) })
                   .GetSourceStructures();
  REQUIRE(scripts.size() == 1);
  scripts.front()->source = name;
  f.extraScripts.push_back(scripts.front());
}
void AliasProperty(Fixture& f, uint32_t quest = kQuest, uint16_t alias = 4)
{
  BaseScripts(f);
  espm::QuestScriptValue value;
  value.type = 1;
  value.formId = quest;
  value.alias = alias;
  f.metadata.scripts[0].properties.push_back({ "Target", 1, value });
  f.fixture->objectTable[0].variables.push_back(
    { "::Target_var", "ReferenceAlias", 0, VarValue::None() });
  f.fixture->objectTable[0].variables.push_back(
    { "bound", "ObjectReference", 0, VarValue::None() });
  f.Fragment().code.instructions = {
    { FunctionCode::kOp_CallMethod,
      { VarValue("GetReference"), Id("::Target_var"), Id("bound"),
        VarValue(0) } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("GetFormID"), Id("bound"), Id("counter"), VarValue(0) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
}
int32_t Counter(MpActor& actor)
{
  return QuestJournal::ScriptState(actor)["scripts"][kStateKey]["values"]
                                         ["boyhousefixture/counter"]["value"]
                                           .get<int32_t>();
}
void RestoreCharacter(MpActor& from, MpActor& to)
{
  const auto dump = MpChangeForm::ToJson(from.GetChangeForm()).dump();
  simdjson::dom::parser parser;
  auto parsed = parser.parse(dump).value();
  to.ApplyChangeForm(MpChangeForm::JsonToChangeForm(parsed));
}
}

TEST_CASE(
  "Campaign forms and aliases have symmetric owner-specific identities",
  "[Quest][QuestAliases]")
{
  const auto record = GetEspmLoader().GetBrowser().LookupById(0x1a677);
  CampaignFormObject a(record, 1, 0x1a677), same(record, 1, 0x1a677),
    other(record, 2, 0x1a677), player({}, 1, 0x14), guestPlayer({}, 2, 0x14);
  EspmGameObject shared(record);
  REQUIRE(a.EqualsByValue(same));
  REQUIRE_FALSE(a.EqualsByValue(other));
  REQUIRE_FALSE(other.EqualsByValue(a));
  REQUIRE_FALSE(a.EqualsByValue(shared));
  REQUIRE_FALSE(shared.EqualsByValue(a));
  REQUIRE_FALSE(player.EqualsByValue(guestPlayer));
  REQUIRE(std::string(player.GetParentNativeScript()) == "actor");
  espm::QuestAliasDefinition def;
  def.id = 4;
  CampaignAliasObject first(1, kQuest, def), identical(1, kQuest, def),
    guest(2, kQuest, def), differentQuest(1, 0x2d512, def);
  REQUIRE(first.EqualsByValue(identical));
  REQUIRE_FALSE(first.EqualsByValue(guest));
  REQUIRE_FALSE(guest.EqualsByValue(first));
  REQUIRE_FALSE(first.EqualsByValue(differentQuest));
  REQUIRE(std::string(first.GetStringID()) != guest.GetStringID());
}

TEST_CASE(
  "Alias reflection and VMAD properties resolve the same campaign object",
  "[Quest][QuestAliases]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  AliasProperty(f);
  f.fixture->objectTable[0].variables.push_back(
    { "equal", "Bool", 0, VarValue(false) });
  f.Fragment().locals.push_back({ "lookup", "Alias" });
  f.Fragment().code.instructions = {
    { FunctionCode::kOp_CallMethod,
      { VarValue("GetAlias"), Id("self"), Id("lookup"), VarValue(1),
        VarValue(4) } },
    { FunctionCode::kOp_CompareEQ,
      { Id("equal"), Id("lookup"), Id("::Target_var") } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("GetID"), Id("lookup"), Id("counter"), VarValue(0) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  REQUIRE(f.Run(actor));
  REQUIRE(Counter(actor) == 4);
  const auto saved = QuestJournal::ScriptState(actor);
  REQUIRE(
    saved["scripts"][kStateKey]["values"]["boyhousefixture/equal"]["value"] ==
    true);
  REQUIRE(saved["scripts"][kStateKey]["values"]["boyhousefixture/::target_var"]
               ["value"]["kind"] == "alias");
  REQUIRE(saved["aliases"].empty()); // Reflection does not fill an alias.
  REQUIRE_FALSE(QuestJournal::Snapshot(actor).contains("vm"));
}

TEST_CASE(
  "Unique actor aliases persist host bindings and stable form references",
  "[Quest][QuestAliases]")
{
  Fixture f;
  auto& host = f.Add(0xff000001);
  auto& guest = f.Add(0xff000002);
  AliasProperty(f);
  const auto instance =
    "campaign:" + std::to_string(host.GetFormId()) + ":fixture";
  host.SetWorldInstance(instance);
  guest.SetWorldInstance(instance);
  REQUIRE(f.Run(guest));
  REQUIRE(Counter(host) == 0x1a677);
  REQUIRE(
    QuestJournal::ScriptState(host)["aliases"]["4e50d:Skyrim.esm/alias:4"] ==
    "1a677:Skyrim.esm");
  guest.SetWorldInstance("");
  REQUIRE(QuestJournal::ScriptState(guest).empty());
  REQUIRE(f.Run(guest));
  REQUIRE(Counter(guest) == 0x1a677);

  Fixture restart;
  auto& restored = restart.Add(host.GetFormId());
  restored.SetWorldInstance(instance);
  RestoreCharacter(host, restored);
  AliasProperty(restart);
  REQUIRE(restart.Run(restored, 20));
  REQUIRE(Counter(restored) == 0x1a677);
  REQUIRE(QuestJournal::ScriptState(restored)["schemaVersion"] == 6);
}

TEST_CASE("Alias binding failures roll back and unsupported optional fills "
          "stay explicit",
          "[Quest][QuestAliases]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  AliasProperty(f);
  auto& code = f.Fragment().code.instructions;
  code.insert(code.end() - 1,
              { FunctionCode::kOp_CallMethod,
                { VarValue("UnsupportedNative"), Id("self"), Id("temp"),
                  VarValue(0) } });
  const auto before =
    actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty);
  REQUIRE_THROWS(f.Run(actor));
  REQUIRE(actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          before);
  REQUIRE(QuestJournal::ScriptState(actor).empty());
  code.erase(code.end() - 2);
  REQUIRE(f.Run(actor));
  REQUIRE(
    QuestJournal::Apply(actor, { { "quest", kQuest }, { "op", "reset" } }));
  REQUIRE(QuestJournal::ScriptState(actor).empty());
  // Optional FactionFriend is a runtime-filled alias. Unknown is not None.
  f.metadata.scripts[0].properties[0].value.alias = 1;
  REQUIRE_THROWS_WITH(
    f.Run(actor),
    Catch::Matchers::ContainsSubstring("Alias fill is unresolved"));
  REQUIRE(QuestJournal::ScriptState(actor).empty());
}

TEST_CASE("Forced reference aliases bind from owned data without using shared "
          "live objects",
          "[Quest][QuestAliases]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  AliasProperty(f, 0x4a37b, 1);
  AddOwnedScript(f, "DA08WhisperingDoorScript");
  actor.SetPropertyValueDump(QuestJournal::kEnabled,
                             "[\"4e50d:Skyrim.esm\",\"4a37b:Skyrim.esm\"]",
                             false, false);
  REQUIRE(
    QuestJournal::Apply(actor, { { "quest", 0x4a37b }, { "op", "start" } }));
  REQUIRE(f.Run(actor));
  REQUIRE(Counter(actor) == 0x4a38c);
  REQUIRE(
    QuestJournal::ScriptState(actor)["aliases"]["4a37b:Skyrim.esm/alias:1"] ==
    "4a38c:Skyrim.esm");
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(0x4a38c));
}

TEST_CASE(
  "Alias reads respect stopped quests and cross-quest adapter boundaries",
  "[Quest][QuestAliases]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  AliasProperty(f, 0x4a37b, 1);
  AddOwnedScript(f, "DA08WhisperingDoorScript");
  f.Fragment().code.instructions.erase(f.Fragment().code.instructions.begin() +
                                       1);
  REQUIRE(f.Run(actor)); // A not-yet-started target quest has no filled alias.
  REQUIRE(QuestJournal::ScriptState(actor)["aliases"].empty());
  REQUIRE(
    QuestJournal::ScriptState(
      actor)["scripts"][kStateKey]["values"]["boyhousefixture/bound"]["value"]
      .is_null());
  actor.SetPropertyValueDump(QuestJournal::kEnabled,
                             "[\"4e50d:Skyrim.esm\",\"4a37b:Skyrim.esm\"]",
                             false, false);
  REQUIRE(
    QuestJournal::Apply(actor, { { "quest", 0x4a37b }, { "op", "start" } }));
  actor.SetPropertyValueDump(QuestJournal::kEnabled, "[\"4e50d:Skyrim.esm\"]",
                             false, false);
  const auto before =
    actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty);
  REQUIRE_THROWS_WITH(
    f.Run(actor, 20),
    Catch::Matchers::ContainsSubstring("adapter is not enabled"));
  REQUIRE(actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          before);
}

TEST_CASE("Campaign player and base forms survive reload without exposing a "
          "guest identity",
          "[Quest][QuestAliases]")
{
  Fixture f;
  auto& host = f.Add(0xff000001);
  auto& guest = f.Add(0xff000002);
  BaseScripts(f);
  f.fixture->objectTable[0].variables.push_back(
    { "player", "Actor", 0, VarValue::None() });
  f.Fragment().code.instructions = {
    { FunctionCode::kOp_CallStatic,
      { VarValue("Game"), VarValue("GetPlayer"), Id("player"), VarValue(0) } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("GetFormID"), Id("player"), Id("counter"), VarValue(0) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  const auto instance =
    "campaign:" + std::to_string(host.GetFormId()) + ":fixture";
  host.SetWorldInstance(instance);
  guest.SetWorldInstance(instance);
  REQUIRE(f.Run(guest));
  REQUIRE(Counter(host) == 0x14);
  REQUIRE(QuestJournal::ScriptState(host)["scripts"][kStateKey]["values"]
                                         ["boyhousefixture/player"]["value"] ==
          "14:Skyrim.esm");
  guest.SetWorldInstance("");
  REQUIRE(QuestJournal::ScriptState(guest).empty());
  Fixture restart;
  auto& restored = restart.Add(host.GetFormId());
  RestoreCharacter(host, restored);
  restart.fixture = f.fixture;
  BaseScripts(restart);
  restart.extraScripts = f.extraScripts;
  restart.Fragment().code.instructions.erase(
    restart.Fragment().code.instructions.begin());
  REQUIRE(restart.Run(restored, 20));
  REQUIRE(Counter(restored) == 0x14);
}

TEST_CASE(
  "Forced location aliases retain location identity in the host campaign",
  "[Quest][QuestAliases]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  AliasProperty(f, 0xe3163, 4);
  f.fixture->objectTable[0].variables[1].typeName = "LocationAlias";
  f.fixture->objectTable[0].variables[2].typeName = "Location";
  f.Fragment().code.instructions[0].args[0] = VarValue("GetLocation");
  actor.SetPropertyValueDump(QuestJournal::kEnabled,
                             "[\"4e50d:Skyrim.esm\",\"e3163:Skyrim.esm\"]",
                             false, false);
  REQUIRE(
    QuestJournal::Apply(actor, { { "quest", 0xe3163 }, { "op", "start" } }));
  REQUIRE(f.Run(actor));
  REQUIRE(Counter(actor) == 0x19189);
  REQUIRE(
    QuestJournal::ScriptState(actor)["aliases"]["e3163:Skyrim.esm/alias:4"] ==
    "19189:Skyrim.esm");
}

TEST_CASE("Campaign references resolve their owned base records",
          "[Quest][QuestAliases]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  AliasProperty(f);
  f.Fragment().locals.push_back({ "base", "Form" });
  f.Fragment().code.instructions.insert(
    f.Fragment().code.instructions.begin() + 1,
    { FunctionCode::kOp_CallMethod,
      { VarValue("GetBaseObject"), Id("bound"), Id("base"), VarValue(0) } });
  f.Fragment().code.instructions[2].args[1] = Id("base");
  REQUIRE(f.Run(actor));
  REQUIRE(Counter(actor) == 0x13bbd);
}

TEST_CASE("Alias scripts persist separate variables and owning quest identity",
          "[Quest][QuestAliases]")
{
  Fixture f;
  auto& host = f.Add(0xff000001);
  auto& guest = f.Add(0xff000002);
  AliasProperty(f);
  espm::QuestScriptData::Alias binding;
  binding.object.type = 1;
  binding.object.formId = kQuest;
  binding.object.alias = 4;
  binding.version = 5;
  binding.objectFormat = 2;
  binding.scripts.push_back({ "AliasFixture", 0, {} });
  f.metadata.aliases.push_back(binding);
  auto script = std::make_shared<PexScript>();
  script->source = "AliasFixture";
  Object object;
  object.NameIndex = script->source;
  object.parentClassName = "ReferenceAlias";
  object.variables.push_back({ "visits", "Int", 0, VarValue(0) });
  FunctionInfo method;
  method.valid = true;
  method.returnType = "Int";
  method.locals.push_back({ "owner", "Quest" });
  method.code.instructions = {
    { FunctionCode::kOp_IAdd, { Id("visits"), Id("visits"), VarValue(1) } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("GetOwningQuest"), Id("self"), Id("owner"), VarValue(0) } },
    { FunctionCode::kOp_Return, { Id("visits") } }
  };
  object.states.push_back({ "", { { "Visit", method } } });
  script->objectTable.push_back(object);
  f.extraScripts.push_back(script);
  f.Fragment().code.instructions = {
    { FunctionCode::kOp_CallMethod,
      { VarValue("Visit"), Id("::Target_var"), Id("counter"), VarValue(0) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  const auto instance =
    "campaign:" + std::to_string(host.GetFormId()) + ":fixture";
  host.SetWorldInstance(instance);
  guest.SetWorldInstance(instance);
  REQUIRE(f.Run(guest));
  REQUIRE(f.Run(guest, 20));
  REQUIRE(Counter(host) == 2);
  const auto saved = QuestJournal::ScriptState(host);
  REQUIRE(saved["scripts"]["4e50d:Skyrim.esm/alias:4/aliasfixture"]["values"]
               ["aliasfixture/visits"]["value"] == 2);
  REQUIRE(saved["aliases"].empty()); // The script did not request a target.
  guest.SetWorldInstance("");
  REQUIRE(QuestJournal::ScriptState(guest).empty());
  REQUIRE(f.Run(guest));
  REQUIRE(Counter(guest) == 1);
}

TEST_CASE("Invalid saved aliases and dynamic form lookups cannot cross the "
          "campaign boundary",
          "[Quest][QuestAliases]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  AliasProperty(f);
  REQUIRE(f.Run(actor));
  auto ledger = nlohmann::json::parse(
    actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty));
  ledger["vm"]["scripts"][kStateKey]["values"]["boyhousefixture/::target_var"]
        ["value"]["id"] = -1;
  actor.SetPropertyValueDump(QuestJournal::kProperty, ledger.dump(), false,
                             false);
  const auto before =
    actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty);
  REQUIRE_THROWS_WITH(f.Run(actor, 20),
                      Catch::Matchers::ContainsSubstring(
                        "Invalid saved campaign alias identity"));
  REQUIRE(actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          before);
  REQUIRE(
    QuestJournal::Apply(actor, { { "quest", kQuest }, { "op", "reset" } }));
  f.Fragment().code.instructions = {
    { FunctionCode::kOp_CallStatic,
      { VarValue("Game"), VarValue("GetForm"), Id("bound"), VarValue(1),
        VarValue(static_cast<int32_t>(0xff000002)) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  REQUIRE_THROWS_WITH(
    f.Run(actor),
    Catch::Matchers::ContainsSubstring("campaign reference allocation"));
  REQUIRE(QuestJournal::ScriptState(actor).empty());
}

TEST_CASE("Invalid saved targets cannot empty or retype required aliases",
          "[Quest][QuestAliases]")
{
  for (const auto& target :
       { nlohmann::json(nullptr), nlohmann::json("13bbd:Skyrim.esm") }) {
    Fixture f;
    auto& actor = f.Add(0xff000001);
    AliasProperty(f);
    REQUIRE(f.Run(actor));
    auto ledger = nlohmann::json::parse(
      actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty));
    ledger["vm"]["aliases"]["4e50d:Skyrim.esm/alias:4"] = target;
    actor.SetPropertyValueDump(QuestJournal::kProperty, ledger.dump(), false,
                               false);
    const auto before =
      actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty);
    REQUIRE_THROWS_WITH(f.Run(actor, 20),
                        Catch::Matchers::ContainsSubstring(
                          target.is_null()
                            ? "Required alias has an empty saved binding"
                            : "Alias target type mismatch"));
    REQUIRE(actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
            before);
  }
}

TEST_CASE("Scene and dialogue ownership resolves the host campaign quest",
          "[Quest][QuestRecordOwners]")
{
  for (const auto& [id, type] : std::vector<std::pair<uint32_t, std::string>>{
         { 0xd50f3, "Scene" }, { 0x98d4a, "TopicInfo" } }) {
    Fixture f;
    auto& host = f.Add(0xff000001);
    auto& guest = f.Add(0xff000002);
    BaseScripts(f);
    espm::QuestScriptValue value;
    value.type = 1;
    value.formId = id;
    value.alias = 0xffff;
    f.metadata.scripts[0].properties.push_back({ "Record", 1, value });
    f.fixture->objectTable[0].variables.push_back(
      { "::Record_var", type, 0, VarValue::None() });
    f.fixture->objectTable[0].variables.push_back(
      { "owner", "Quest", 0, VarValue::None() });
    f.Fragment().code.instructions = {
      { FunctionCode::kOp_CallMethod,
        { VarValue("GetOwningQuest"), Id("::Record_var"), Id("owner"),
          VarValue(0) } },
      { FunctionCode::kOp_CallMethod,
        { VarValue("GetFormID"), Id("owner"), Id("counter"), VarValue(0) } },
      { FunctionCode::kOp_CallMethod,
        { VarValue("SetObjectiveDisplayed"), Id("owner"), Id("temp"),
          VarValue(2), VarValue(10), VarValue(true) } },
      { FunctionCode::kOp_Return, { VarValue::None() } }
    };
    const auto instance =
      "campaign:" + std::to_string(host.GetFormId()) + ":fixture";
    host.SetWorldInstance(instance);
    guest.SetWorldInstance(instance);
    REQUIRE(f.Run(guest));
    REQUIRE(Counter(host) == kQuest);
    REQUIRE(QuestJournal::Get(host, kQuest)["objectives"]["10"]["displayed"] ==
            true);
    REQUIRE(QuestJournal::ScriptState(
              host)["scripts"][kStateKey]["values"]["boyhousefixture/owner"]
                   ["value"] == "4e50d:Skyrim.esm");
    // The scene has several package-action PNAMs before its parent quest.
    // The INFO's owning quest comes from its winning DIAL, not its own fields.
    guest.SetWorldInstance("");
    REQUIRE(QuestJournal::Get(guest, kQuest)["objectives"].empty());
    REQUIRE(QuestJournal::ScriptState(guest).empty());
    REQUIRE(f.Run(guest));
    REQUIRE(Counter(guest) == kQuest);
    auto& code = f.Fragment().code.instructions;
    code.insert(code.end() - 1,
                { FunctionCode::kOp_CallMethod,
                  { VarValue("UnsupportedNative"), Id("owner"), Id("temp"),
                    VarValue(0) } });
    const auto before =
      guest.GetDynamicFields().GetValueDump(QuestJournal::kProperty);
    REQUIRE_THROWS(f.Run(guest, 20));
    REQUIRE(guest.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
            before);
  }
}

namespace {
constexpr uint32_t kConditionalQuest =
  0x3372b; // MQ101's original log branches.
const char* kConditionalState = "3372b:Skyrim.esm/boyhousefixture";
void ConditionalFixture(Fixture& f, int path)
{
  // Use a nonstandard bit index: names in the PEX table define user flags.
  f.fixture->userFlagTable.push_back({ "Conditional", 4 });
  auto& object = f.fixture->objectTable[0];
  object.userFlags = 16;
  object.variables.push_back({ "::FactionPath_var", "Int", 16, VarValue(0) });
  auto setter = f.Fragment(), one = setter, two = setter;
  setter.code.instructions = {
    { FunctionCode::kOp_Assign, { Id("::FactionPath_var"), VarValue(path) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  one.code.instructions = {
    { FunctionCode::kOp_Assign, { Id("counter"), VarValue(11) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  two.code.instructions = {
    { FunctionCode::kOp_Assign, { Id("counter"), VarValue(22) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  object.states[0].functions = { { "Setter", setter },
                                 { "One", one },
                                 { "Two", two } };
  f.metadata.fragments = { { 250, 0, 1, "BoyhouseFixture", "Setter" },
                           { 250, 1, 1, "BoyhouseFixture", "One" },
                           { 250, 2, 1, "BoyhouseFixture", "Two" },
                           { 255, 1, 1, "BoyhouseFixture", "One" },
                           { 255, 2, 1, "BoyhouseFixture", "Two" } };
}
void EnableConditional(MpActor& actor)
{
  actor.SetPropertyValueDump(QuestJournal::kEnabled, "[\"3372b:Skyrim.esm\"]",
                             false, false);
}
int ConditionalCounter(MpActor& actor)
{
  return QuestJournal::ScriptState(
           actor)["scripts"][kConditionalState]["values"]
                 ["boyhousefixture/counter"]["value"]
                   .get<int>();
}
}

TEST_CASE("Owned conditional branches see earlier fragment writes in the host",
          "[Quest][QuestVM][QuestVariableConditions]")
{
  const auto path = GENERATE(1, 2);
  Fixture f;
  ConditionalFixture(f, path);
  auto& host = f.Add(0xff000001);
  auto& guest = f.Add(0xff000002);
  EnableConditional(host);
  EnableConditional(guest);
  // Give the guest a different personal branch before joining the host.
  auto& setter = f.fixture->objectTable[0].states[0].functions[0].function;
  setter.code.instructions[0].args[1] = VarValue(3 - path);
  REQUIRE(f.Run(guest, 250, kConditionalQuest));
  const auto personal = QuestJournal::ScriptState(guest);
  setter.code.instructions[0].args[1] = VarValue(path);
  const auto instance =
    "campaign:" + std::to_string(host.GetFormId()) + ":fixture";
  host.SetWorldInstance(instance);
  guest.SetWorldInstance(instance);
  REQUIRE(f.Run(guest, 250, kConditionalQuest));
  REQUIRE(ConditionalCounter(host) == path * 11);
  REQUIRE(ConditionalCounter(guest) == path * 11);
  REQUIRE_FALSE(QuestJournal::Snapshot(guest).contains("vm"));
  guest.SetWorldInstance("");
  REQUIRE(QuestJournal::ScriptState(guest) == personal);
  REQUIRE(f.Run(guest, 255, kConditionalQuest));
  REQUIRE(ConditionalCounter(guest) == (3 - path) * 11);
  REQUIRE(ConditionalCounter(host) == path * 11);
}

TEST_CASE("Quest variable conditions survive character reload and inheritance",
          "[Quest][QuestVM][QuestVariableConditions]")
{
  Fixture f;
  ConditionalFixture(f, 2);
  SECTION("declared on the attached script")
  {
  }
  SECTION("declared on a conditional parent")
  {
    auto parent = std::make_shared<PexScript>();
    parent->source = "ConditionalParent";
    parent->userFlagTable = f.fixture->userFlagTable;
    Object object;
    object.NameIndex = parent->source;
    object.parentClassName = "Quest";
    object.userFlags = 16;
    object.variables.push_back(f.fixture->objectTable[0].variables.back());
    object.states.push_back({ "", { { "SetPath", f.Fragment() } } });
    parent->objectTable.push_back(object);
    f.fixture->objectTable[0].variables.pop_back();
    f.fixture->objectTable[0].parentClassName = parent->source;
    // A compiled child calls a parent method/property; it cannot directly
    // address a parent's private backing variable in its own frame.
    f.Fragment().code.instructions = {
      { FunctionCode::kOp_CallParent,
        { VarValue("SetPath"), Id("temp"), VarValue(0) } },
      { FunctionCode::kOp_Return, { VarValue::None() } }
    };
    f.extraScripts.push_back(parent);
  }
  auto& actor = f.Add(0xff000001);
  EnableConditional(actor);
  REQUIRE(f.Run(actor, 250, kConditionalQuest));
  Fixture restarted;
  restarted.fixture = f.fixture;
  restarted.metadata = f.metadata;
  restarted.extraScripts = f.extraScripts;
  auto& restored = restarted.Add(actor.GetFormId());
  const auto dump = MpChangeForm::ToJson(actor.GetChangeForm()).dump();
  simdjson::dom::parser parser;
  auto parsed = parser.parse(dump).value();
  restored.ApplyChangeForm(MpChangeForm::JsonToChangeForm(parsed));
  REQUIRE(restarted.Run(restored, 255, kConditionalQuest));
  REQUIRE(ConditionalCounter(restored) == 22);
}

TEST_CASE("Conditional reads use numeric types and case insensitive names",
          "[Quest][QuestVM][QuestVariableConditions]")
{
  Fixture f;
  ConditionalFixture(f, 1);
  // No setter at stage 255: read the PEX default directly.
  auto& variable = f.fixture->objectTable[0].variables.back();
  variable.name = "::factionpath_VAR";
  SECTION("float")
  {
    variable.typeName = "Float";
    variable.value = VarValue(1.0);
  }
  SECTION("boolean")
  {
    variable.typeName = "Bool";
    variable.value = VarValue(true);
  }
  SECTION("integer")
  {
    variable.value = VarValue(1);
  }
  auto& actor = f.Add(0xff000001);
  EnableConditional(actor);
  REQUIRE(f.Run(actor, 255, kConditionalQuest));
  REQUIRE(ConditionalCounter(actor) == 11);
}

TEST_CASE("Invalid conditional variables roll back preceding fragments",
          "[Quest][QuestVM][QuestVariableConditions]")
{
  Fixture f;
  ConditionalFixture(f, 1);
  auto& object = f.fixture->objectTable[0];
  SECTION("script is not conditional")
  {
    object.userFlags = 0;
  }
  SECTION("variable is not conditional")
  {
    object.variables.back().userFlags = 0;
  }
  SECTION("variable declaration is nonnumeric")
  {
    object.variables.back().typeName = "String";
  }
  SECTION("variable does not exist")
  {
    object.variables.back().name = "DifferentVariable";
    f.Fragment().code.instructions[0].args[0] = Id("counter");
  }
  SECTION("ambiguous conditional scripts")
  {
    auto other = std::make_shared<PexScript>();
    other->source = "OtherConditional";
    other->userFlagTable = f.fixture->userFlagTable;
    other->objectTable.push_back(object);
    other->objectTable[0].NameIndex = other->source;
    f.extraScripts.push_back(other);
    f.metadata.scripts.push_back({ other->source, 0, {} });
  }
  auto& actor = f.Add(0xff000001);
  EnableConditional(actor);
  const auto before =
    actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty);
  REQUIRE_THROWS(f.Run(actor, 250, kConditionalQuest));
  REQUIRE(actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          before);
  REQUIRE(QuestJournal::ScriptState(actor).empty());
}

TEST_CASE("Owned civil war conditions compare private variables and globals",
          "[Quest][QuestVM][QuestVariableConditions]")
{
  Fixture f;
  ConditionalFixture(f, 1);
  constexpr uint32_t objective = 0x29940, civilWar = 0x19e53;
  for (const auto* script :
       { "CWHoldManagerScript", "QF_CW_00019E53", "CWScript" })
    AddOwnedScript(f, script);
  // CWObj stage 1 has two original GetVMQuestVariable/UseGlobal branches.
  f.metadata.fragments = { { 1, 0, 1, "BoyhouseFixture", "One" },
                           { 1, 1, 1, "BoyhouseFixture", "Two" } };
  auto& actor = f.Add(0xff000001);
  actor.SetPropertyValueDump(QuestJournal::kEnabled,
                             "[\"29940:Skyrim.esm\",\"19e53:Skyrim.esm\"]",
                             false, false);
  REQUIRE(
    QuestJournal::Apply(actor, { { "quest", civilWar }, { "op", "start" } }));
  QuestJournal::Atomic(actor, [&] {
    QuestVmState vm(nlohmann::json::object(), {}, {});
    vm.SetScript("19e53:Skyrim.esm/cwscript",
                 { { "schemaVersion", 1 },
                   { "values",
                     { { "cwscript/::playerallegiance_var",
                         { { "type", 3 }, { "value", 9 } } } } } });
    // Distinct from the masters' defaults. Only the first comparison matches.
    vm.SetGlobal("73f10:Skyrim.esm", 9);
    vm.SetGlobal("73f11:Skyrim.esm", 10);
    QuestJournal::SetScriptState(actor, vm.Save());
  });
  const auto before =
    actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty);
  SECTION("reads the current campaign")
  {
    REQUIRE(f.Run(actor, 1, objective));
    REQUIRE(QuestJournal::ScriptState(
              actor)["scripts"]["29940:Skyrim.esm/boyhousefixture"]["values"]
                    ["boyhousefixture/counter"]["value"] == 11);
  }
  SECTION("later logs see earlier uncommitted comparison-global writes")
  {
    espm::QuestScriptValue global;
    global.type = 1;
    global.alias = 0xffff;
    global.formId = 0x73f11;
    f.metadata.scripts[0].properties.push_back({ "Comparison", 1, global });
    f.fixture->objectTable[0].variables.push_back(
      { "::Comparison_var", "GlobalVariable", 0, VarValue::None() });
    auto& one = f.fixture->objectTable[0].states[0].functions[1].function;
    one.code.instructions.insert(
      one.code.instructions.begin(),
      { FunctionCode::kOp_CallMethod,
        { VarValue("SetValue"), Id("::Comparison_var"), Id("temp"),
          VarValue(1), VarValue(9.0) } });
    SECTION("both matching logs commit")
    {
      REQUIRE(f.Run(actor, 1, objective));
      const auto saved = QuestJournal::ScriptState(actor);
      REQUIRE(saved["globals"]["73f11:Skyrim.esm"] == 9.0);
      REQUIRE(saved["scripts"]["29940:Skyrim.esm/boyhousefixture"]["values"]
                   ["boyhousefixture/counter"]["value"] == 22);
    }
    SECTION("later failure rolls back globals and earlier fragment effects")
    {
      auto& two = f.fixture->objectTable[0].states[0].functions[2].function;
      two.code.instructions.insert(
        two.code.instructions.begin(),
        { FunctionCode::kOp_CallStatic,
          { VarValue("Utility"), VarValue("Wait"), Id("temp"), VarValue(1),
            VarValue(1.0) } });
      REQUIRE_THROWS(f.Run(actor, 1, objective));
      REQUIRE(actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
              before);
    }
  }
  SECTION("cross quest adapter is required")
  {
    actor.SetPropertyValueDump(QuestJournal::kEnabled,
                               "[\"29940:Skyrim.esm\"]", false, false);
    REQUIRE_THROWS(f.Run(actor, 1, objective));
    REQUIRE(actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
            before);
  }
  SECTION(
    "stopped script state is not treated as an initialized running quest")
  {
    REQUIRE(
      QuestJournal::Apply(actor, { { "quest", civilWar }, { "op", "stop" } }));
    const auto stopped =
      actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty);
    REQUIRE_THROWS_WITH(
      f.Run(actor, 1, objective),
      Catch::Matchers::ContainsSubstring("requires a running quest"));
    REQUIRE(actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
            stopped);
  }
}

namespace {
constexpr uint32_t kScriptedReference = 0x6debb;
const char* kReferenceState =
  "reference:6debb:Skyrim.esm/defaultsetstagetrigscript";
void ReferenceFixture(Fixture& f)
{
  BaseScripts(f);
  espm::QuestScriptValue target;
  target.type = 1;
  target.formId = kScriptedReference;
  target.alias = 0xffff;
  f.metadata.scripts[0].properties.push_back({ "Target", 1, target });
  f.fixture->objectTable[0].variables.push_back(
    { "::Target_var", "ObjectReference", 0, VarValue::None() });
  f.fixture->objectTable[0].variables.push_back(
    { "scripted", "defaultSetStageTRIGScript", 0, VarValue::None() });
  auto script = std::make_shared<PexScript>();
  script->source = "defaultSetStageTRIGScript";
  Object object;
  object.NameIndex = script->source;
  object.parentClassName = "ObjectReference";
  object.variables = { { "counter", "Int", 0, VarValue(0) },
                       { "::stage_var", "Int", 0, VarValue::None() } };
  FunctionInfo step;
  step.valid = true;
  step.returnType = "Int";
  step.code.instructions = { { FunctionCode::kOp_IAdd,
                               { Id("counter"), Id("counter"),
                                 Id("::stage_var") } },
                             { FunctionCode::kOp_Return, { Id("counter") } } };
  object.states.push_back({ "", { { "Step", step } } });
  script->objectTable.push_back(object);
  f.extraScripts.push_back(script);
  f.Fragment().code.instructions = {
    { FunctionCode::kOp_Cast, { Id("scripted"), Id("::Target_var") } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("Step"), Id("scripted"), Id("counter"), VarValue(0) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
}
int SavedReferenceCounter(MpActor& actor)
{
  return QuestJournal::ScriptState(
           actor)["scripts"][kReferenceState]["values"]
                 ["defaultsetstagetrigscript/counter"]["value"]
                   .get<int>();
}
}

TEST_CASE("Reference bindings merge owned base and placed properties once",
          "[Quest][ReferenceScripts]")
{
  Fixture f;
  const auto bindings = CampaignReferenceScripts::Read(
    f.server.worldState,
    GetEspmLoader().GetBrowser().LookupById(kScriptedReference));
  REQUIRE(bindings.size() == 1);
  const auto& props = bindings[0].properties;
  const auto quest =
    std::find_if(props.begin(), props.end(),
                 [](const auto& prop) { return prop.name == "myQuest"; });
  REQUIRE(quest != props.end());
  REQUIRE(quest->value.formId == 0x36191);
  const auto stage =
    std::find_if(props.begin(), props.end(),
                 [](const auto& prop) { return prop.name == "stage"; });
  REQUIRE(stage != props.end());
  REQUIRE(stage->value.integer == 170);
}

TEST_CASE("NPC reference scripts follow deterministic template inheritance",
          "[Quest][ReferenceScripts]")
{
  Fixture f;
  const auto& browser = GetEspmLoader().GetBrowser();
  // The placed NPC inherits UseScript through 3763a -> 8a1c9.
  const auto bindings = CampaignReferenceScripts::Read(
    f.server.worldState, browser.LookupById(0x8767a));
  REQUIRE(bindings.size() == 1);
  auto name = bindings[0].name;
  for (auto& c : name)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  REQUIRE(name == "masterambushscript");
  // This chain reaches LVLN. Selecting a random NPC would break replay.
  REQUIRE_THROWS_WITH(CampaignReferenceScripts::Read(
                        f.server.worldState, browser.LookupById(0x29d87)),
                      Catch::Matchers::ContainsSubstring(
                        "campaign leveled actor template selection"));
}

TEST_CASE(
  "Reference script variables follow the campaign and survive quest reset",
  "[Quest][QuestVM][ReferenceScripts]")
{
  Fixture f;
  ReferenceFixture(f);
  auto& host = f.Add(0xff000001);
  auto& guest = f.Add(0xff000002);
  REQUIRE(f.Run(guest));
  REQUIRE(SavedReferenceCounter(guest) == 170);
  const auto personal = QuestJournal::ScriptState(guest);
  const auto instance =
    "campaign:" + std::to_string(host.GetFormId()) + ":fixture";
  host.SetWorldInstance(instance);
  guest.SetWorldInstance(instance);
  REQUIRE(f.Run(guest));
  REQUIRE(f.Run(guest, 20));
  REQUIRE(SavedReferenceCounter(host) == 340);
  REQUIRE(
    QuestJournal::Apply(guest, { { "quest", kQuest }, { "op", "reset" } }));
  REQUIRE(SavedReferenceCounter(host) == 340);
  REQUIRE(f.Run(guest));
  REQUIRE(SavedReferenceCounter(host) == 510);
  guest.SetWorldInstance("");
  REQUIRE(QuestJournal::ScriptState(guest) == personal);
  REQUIRE(f.Run(guest, 20));
  REQUIRE(SavedReferenceCounter(guest) == 340);
  REQUIRE(SavedReferenceCounter(host) == 510);
}

TEST_CASE(
  "Reference script roots reload and later failure rolls back their writes",
  "[Quest][QuestVM][ReferenceScripts]")
{
  Fixture f;
  ReferenceFixture(f);
  auto& actor = f.Add(0xff000001);
  REQUIRE(f.Run(actor));
  Fixture restarted;
  ReferenceFixture(restarted);
  auto& restored = restarted.Add(actor.GetFormId());
  const auto dump = MpChangeForm::ToJson(actor.GetChangeForm()).dump();
  simdjson::dom::parser parser;
  auto parsed = parser.parse(dump).value();
  restored.ApplyChangeForm(MpChangeForm::JsonToChangeForm(parsed));
  SECTION("reload preserves state")
  {
    REQUIRE(restarted.Run(restored, 20));
    REQUIRE(SavedReferenceCounter(restored) == 340);
  }
  SECTION("later failure rolls back")
  {
    const auto before =
      restored.GetDynamicFields().GetValueDump(QuestJournal::kProperty);
    auto& code = restarted.Fragment().code.instructions;
    code.insert(code.end() - 1,
                { FunctionCode::kOp_CallStatic,
                  { VarValue("Utility"), VarValue("Wait"), Id("temp"),
                    VarValue(1), VarValue(1.0) } });
    REQUIRE_THROWS(restarted.Run(restored, 20));
    REQUIRE(restored.GetDynamicFields().GetValueDump(
              QuestJournal::kProperty) == before);
  }
}

TEST_CASE(
  "Different quests share one campaign reference script instance state",
  "[Quest][QuestVM][ReferenceScripts]")
{
  Fixture f;
  ReferenceFixture(f);
  f.metadata.fragments.push_back({ 5, 0, 1, "BoyhouseFixture", "Fragment" });
  auto& actor = f.Add(0xff000001);
  actor.SetPropertyValueDump(QuestJournal::kEnabled,
                             "[\"4e50d:Skyrim.esm\",\"2d512:Skyrim.esm\"]",
                             false, false);
  REQUIRE(f.Run(actor));
  REQUIRE(SavedReferenceCounter(actor) == 170);
  REQUIRE(f.Run(actor, 5, 0x2d512));
  REQUIRE(SavedReferenceCounter(actor) == 340);
  const auto state = QuestJournal::ScriptState(actor);
  REQUIRE(state["scripts"][kStateKey]["values"]["boyhousefixture/counter"]
               ["value"] == 170);
  REQUIRE(state["scripts"]["2d512:Skyrim.esm/boyhousefixture"]["values"]
               ["boyhousefixture/counter"]["value"] == 340);
}

TEST_CASE(
  "Unavailable reference code and removed quest attachments cannot execute",
  "[Quest][QuestVM][ReferenceScripts]")
{
  Fixture f;
  auto& actor = f.Add(0xff000001);
  SECTION("missing reference script fails explicitly")
  {
    ReferenceFixture(f);
    f.extraScripts.pop_back();
    REQUIRE_THROWS_WITH(
      f.Run(actor),
      Catch::Matchers::ContainsSubstring("Missing campaign reference script"));
  }
  SECTION("removed local quest script")
  {
    f.metadata.scripts[0].status = 2;
    REQUIRE_THROWS(f.Run(actor));
  }
  SECTION("removed inherited quest script")
  {
    f.metadata.scripts[0].status = 3;
    REQUIRE_THROWS(f.Run(actor));
  }
  REQUIRE(QuestJournal::Get(actor, kQuest)["stages"].empty());
  REQUIRE(QuestJournal::ScriptState(actor).empty());
}

namespace {
FunctionInfo Returns(int value)
{
  FunctionInfo fn;
  fn.valid = true;
  fn.returnType = "Int";
  fn.code.instructions = { { FunctionCode::kOp_Return, { VarValue(value) } } };
  return fn;
}
std::shared_ptr<PexScript> MethodScript(Fixture& f, const char* name,
                                        const char* parent, bool attach)
{
  auto pex = std::make_shared<PexScript>();
  pex->source = name;
  Object object;
  object.NameIndex = name;
  object.parentClassName = parent;
  object.states.push_back({ "", {} });
  pex->objectTable.push_back(object);
  f.extraScripts.push_back(pex);
  if (attach)
    f.metadata.scripts.insert(f.metadata.scripts.begin(), { name, 0, {} });
  return pex;
}
int MethodResult(MpActor& actor)
{
  return QuestJournal::ScriptState(actor)["scripts"][kStateKey]["values"]
                                         ["boyhousefixture/counter"]["value"]
                                           .get<int>();
}
void CallFixture(Fixture& f, const char* name, const char* receiver = "self")
{
  f.Fragment().code.instructions = {
    { FunctionCode::kOp_CallMethod,
      { VarValue(name), Id(receiver), Id("counter"), VarValue(0) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
}
}

TEST_CASE("Campaign method calls select the receiver script instead of the "
          "first attachment",
          "[Quest][QuestVM][MethodDispatch]")
{
  Fixture f;
  auto sibling = MethodScript(f, "OtherBinding", "Quest", true);
  sibling->objectTable[0].states[0].functions.push_back(
    { "Read", Returns(99) });
  f.fixture->objectTable[0].states[0].functions.push_back(
    { "Read", Returns(7) });
  SECTION("self remains in its own binding")
  {
    CallFixture(f, "Read");
  }
  SECTION("typed reference selects its binding case insensitively")
  {
    espm::QuestScriptValue target;
    target.type = 1;
    target.formId = kQuest;
    target.alias = 0xffff;
    f.metadata.scripts.back().properties.push_back({ "Target", 1, target });
    f.fixture->objectTable[0].variables.push_back(
      { "::Target_var", "BOYHOUSEFIXTURE", 0, VarValue::None() });
    CallFixture(f, "rEAd", "::Target_var");
  }
  auto& actor = f.Add(0xff000001);
  REQUIRE(f.Run(actor));
  REQUIRE(MethodResult(actor) == 7);
}

TEST_CASE("Campaign inherited methods preserve virtual self and explicit "
          "parent dispatch",
          "[Quest][QuestVM][MethodDispatch]")
{
  Fixture f;
  auto sibling = MethodScript(f, "OtherBinding", "Quest", true);
  sibling->objectTable[0].states[0].functions.push_back(
    { "Read", Returns(99) });
  auto parent = MethodScript(f, "MethodParent", "Quest", false);
  parent->objectTable[0].states[0].functions.push_back(
    { "Read", Returns(13) });
  f.fixture->objectTable[0].parentClassName = "MethodParent";
  f.fixture->objectTable[0].states[0].functions.push_back(
    { "Read", Returns(7) });
  int expected = 0;
  SECTION("inherited body calls the child's override through self")
  {
    FunctionInfo indirect = Returns(0);
    indirect.locals.push_back({ "result", "Int" });
    indirect.code.instructions = {
      { FunctionCode::kOp_CallMethod,
        { VarValue("Read"), Id("self"), Id("result"), VarValue(0) } },
      { FunctionCode::kOp_Return, { Id("result") } }
    };
    parent->objectTable[0].states[0].functions.push_back(
      { "Indirect", indirect });
    CallFixture(f, "Indirect");
    expected = 7;
  }
  SECTION("parent bypasses the child and unrelated attachments")
  {
    f.Fragment().code.instructions = {
      { FunctionCode::kOp_CallParent,
        { VarValue("Read"), Id("counter"), VarValue(0) } },
      { FunctionCode::kOp_Return, { VarValue::None() } }
    };
    expected = 13;
  }
  SECTION("a cast to an unattached ancestor still dispatches virtually")
  {
    f.fixture->objectTable[0].variables.push_back(
      { "parentRef", "MethodParent", 0, VarValue::None() });
    CallFixture(f, "Read", "parentRef");
    f.Fragment().code.instructions.insert(
      f.Fragment().code.instructions.begin(),
      { FunctionCode::kOp_Cast, { Id("parentRef"), Id("self") } });
    expected = 7;
  }
  auto& actor = f.Add(0xff000001);
  REQUIRE(f.Run(actor));
  REQUIRE(MethodResult(actor) == expected);
}

TEST_CASE("Campaign native declarations execute registered implementations or "
          "roll back",
          "[Quest][QuestVM][MethodDispatch]")
{
  Fixture f;
  FunctionInfo native = Returns(99);
  native.flags = 2;
  f.quest->objectTable[0].states.push_back({ "", { { "GetStage", native } } });
  auto& actor = f.Add(0xff000001);
  SECTION("registered inherited native returns real journal state")
  {
    CallFixture(f, "GetStage");
    REQUIRE(f.Run(actor));
    REQUIRE(MethodResult(actor) == 10);
  }
  SECTION("shared engine helpers use the registered implementation once")
  {
    MethodScript(f, "OtherBinding", "Quest", true);
    f.quest->objectTable[0].states[0].functions[0].function.flags = 0;
    espm::QuestScriptValue target;
    target.type = 1;
    target.formId = kQuest;
    target.alias = 0xffff;
    f.metadata.scripts.back().properties.push_back({ "Target", 1, target });
    f.fixture->objectTable[0].variables.push_back(
      { "::Target_var", "Quest", 0, VarValue::None() });
    CallFixture(f, "GetStage", "::Target_var");
    REQUIRE(f.Run(actor));
    REQUIRE(MethodResult(actor) == 10);
  }
  SECTION("missing native cannot execute an empty or forged body")
  {
    f.fixture->objectTable[0].states[0].functions.push_back(
      { "Missing", native });
    auto& code = f.Fragment().code.instructions;
    code.insert(
      code.end() - 1,
      { FunctionCode::kOp_CallMethod,
        { VarValue("Missing"), Id("self"), Id("temp"), VarValue(0) } });
    const auto before = QuestJournal::Snapshot(actor);
    REQUIRE_THROWS_WITH(
      f.Run(actor),
      Catch::Matchers::ContainsSubstring("Unimplemented native"));
    REQUIRE(QuestJournal::Snapshot(actor) == before);
    REQUIRE(QuestJournal::ScriptState(actor).empty());
  }
  SECTION("custom native declaration cannot fall through to the engine base")
  {
    f.fixture->objectTable[0].states[0].functions.push_back(
      { "GetStage", native });
    CallFixture(f, "GetStage");
    REQUIRE_THROWS_WITH(f.Run(actor),
                        Catch::Matchers::ContainsSubstring(
                          "Unimplemented native: BoyhouseFixture.GetStage"));
    REQUIRE(QuestJournal::Get(actor, kQuest)["stages"].empty());
  }
  SECTION("native method arguments must match its declaration")
  {
    native.params.push_back({ "unexpected", "Int" });
    f.quest->objectTable[0].states[0].functions[0].function = native;
    CallFixture(f, "GetStage");
    REQUIRE_THROWS_WITH(
      f.Run(actor),
      Catch::Matchers::ContainsSubstring("argument count mismatch"));
    REQUIRE(QuestJournal::ScriptState(actor).empty());
  }
}

TEST_CASE("Ambiguous or invalid campaign method targets never choose an "
          "unrelated script",
          "[Quest][QuestVM][MethodDispatch]")
{
  Fixture f;
  auto sibling = MethodScript(f, "OtherBinding", "Quest", true);
  sibling->objectTable[0].states[0].functions.push_back(
    { "Read", Returns(99) });
  f.fixture->objectTable[0].states[0].functions.push_back(
    { "Read", Returns(7) });
  espm::QuestScriptValue target;
  target.type = 1;
  target.formId = kQuest;
  target.alias = 0xffff;
  f.metadata.scripts.back().properties.push_back({ "Target", 1, target });
  f.fixture->objectTable[0].variables.push_back(
    { "::Target_var", "Quest", 0, VarValue::None() });
  CallFixture(f, "Read", "::Target_var");
  std::string error = "Ambiguous Papyrus method binding";
  SECTION("two unqualified implementations")
  {
  }
  SECTION("unattached custom receiver")
  {
    f.fixture->objectTable[0].variables.back().typeName = "MissingBinding";
    error = "receiver has no matching script binding";
  }
  SECTION("None remains None")
  {
    f.metadata.scripts.back().properties[0].value.formId = 0;
    error = "Method called on None";
  }
  SECTION("a malformed nonobject target cannot become a pointer")
  {
    f.metadata.scripts.back().properties[0].value.type = 3;
    f.metadata.scripts.back().properties[0].value.integer = 1;
    error = "method target is not an object";
  }
  SECTION("malformed state cannot become a string pointer")
  {
    CallFixture(f, "Read");
    f.Fragment().code.instructions.insert(
      f.Fragment().code.instructions.begin(),
      { FunctionCode::kOp_Assign, { Id("::State"), VarValue(1) } });
    error = "Invalid Papyrus instance state";
  }
  SECTION("global function cannot be invoked as a method")
  {
    f.fixture->objectTable[0].variables.back().typeName = "BoyhouseFixture";
    f.fixture->objectTable[0].states[0].functions.back().function.flags = 1;
    error = "Global Papyrus function called as a method";
  }
  auto& actor = f.Add(0xff000001);
  REQUIRE_THROWS_WITH(f.Run(actor), Catch::Matchers::ContainsSubstring(error));
  REQUIRE(QuestJournal::ScriptState(actor).empty());
}

TEST_CASE("Campaign method state and state events are scoped to the receiver",
          "[Quest][QuestVM][MethodDispatch]")
{
  Fixture f;
  f.fixture->objectTable[0].autoStateName = "BUSY";
  f.fixture->objectTable[0].states[0].functions.push_back(
    { "Read", Returns(1) });
  auto stateEvent = Returns(0);
  stateEvent.returnType = "None";
  stateEvent.code.instructions = {
    { FunctionCode::kOp_Assign, { Id("counter"), VarValue(9) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  f.fixture->objectTable[0].states.push_back(
    { "Busy", { { "Read", Returns(8) }, { "OnBeginState", stateEvent } } });
  auto sibling = MethodScript(f, "OtherBinding", "Quest", true);
  sibling->objectTable[0].variables.push_back(
    { "counter", "Int", 0, VarValue(0) });
  stateEvent.code.instructions[0].args[1] = VarValue(99);
  sibling->objectTable[0].states[0].functions.push_back(
    { "OnBeginState", stateEvent });
  int expected = 0;
  bool changedSibling = false;
  SECTION("state names and methods ignore case")
  {
    CallFixture(f, "rEAd");
    expected = 8;
  }
  SECTION("explicit state event targets self only")
  {
    CallFixture(f, "ONBEGINSTATE");
    f.Fragment().code.instructions[0].args[2] = Id("temp");
    expected = 9;
  }
  SECTION("state event targets another receiver")
  {
    espm::QuestScriptValue target;
    target.type = 1;
    target.formId = kQuest;
    target.alias = 0xffff;
    f.metadata.scripts.back().properties.push_back({ "Target", 1, target });
    f.fixture->objectTable[0].variables.push_back(
      { "::Target_var", "OtherBinding", 0, VarValue::None() });
    CallFixture(f, "OnBeginState", "::Target_var");
    f.Fragment().code.instructions[0].args[2] = Id("temp");
    changedSibling = true;
  }
  SECTION("inherited implementations use the derived instance state")
  {
    auto parent = MethodScript(f, "MethodParent", "Quest", false);
    f.fixture->objectTable[0].parentClassName = "MethodParent";
    parent->objectTable[0].states[0].functions.push_back(
      { "ReadParent", Returns(20) });
    parent->objectTable[0].states.push_back(
      { "Busy", { { "ReadParent", Returns(21) } } });
    CallFixture(f, "ReadParent");
    expected = 21;
  }
  SECTION("parent state writes update the same derived instance")
  {
    auto parent = MethodScript(f, "MethodParent", "Quest", false);
    f.fixture->objectTable[0].parentClassName = "MethodParent";
    f.fixture->objectTable[0].states.push_back(
      { "Idle", { { "Read", Returns(16) } } });
    FunctionInfo change = Returns(0);
    change.code.instructions.insert(
      change.code.instructions.begin(),
      { FunctionCode::kOp_Assign, { Id("::State"), VarValue("idle") } });
    parent->objectTable[0].states[0].functions.push_back({ "Change", change });
    CallFixture(f, "Read");
    f.Fragment().code.instructions.insert(
      f.Fragment().code.instructions.begin(),
      { FunctionCode::kOp_CallParent,
        { VarValue("Change"), Id("temp"), VarValue(0) } });
    expected = 16;
  }
  f.Fragment().code.instructions.insert(
    f.Fragment().code.instructions.end() - 1,
    { FunctionCode::kOp_IAdd, { Id("counter"), Id("counter"), VarValue(0) } });
  auto& actor = f.Add(0xff000001);
  REQUIRE(f.Run(actor));
  REQUIRE(MethodResult(actor) == expected);
  const auto state = QuestJournal::ScriptState(actor);
  const auto& values =
    state["scripts"]["4e50d:Skyrim.esm/otherbinding"]["values"];
  if (changedSibling)
    REQUIRE(values["otherbinding/counter"]["value"] == 99);
  else
    REQUIRE_FALSE(values.contains("otherbinding/counter"));
}

TEST_CASE(
  "Invalid campaign inheritance and ambiguous casts fail before commit",
  "[Quest][QuestVM][MethodDispatch]")
{
  Fixture f;
  auto parent = MethodScript(f, "MethodParent", "Quest", false);
  f.fixture->objectTable[0].parentClassName = "MethodParent";
  std::string error;
  SECTION("cyclic inheritance cannot overflow the server stack")
  {
    parent->objectTable[0].parentClassName = "BoyhouseFixture";
    error = "inheritance cycle/depth exceeded";
  }
  SECTION("missing parent cannot be silently dropped")
  {
    parent->objectTable[0].parentClassName = "MissingParent";
    error = "Unable to find parent script";
  }
  SECTION("an ambiguous custom ancestor requires an explicit binding")
  {
    MethodScript(f, "OtherBinding", "MethodParent", true);
    f.fixture->objectTable[0].variables.push_back(
      { "parentRef", "MethodParent", 0, VarValue::None() });
    f.Fragment().code.instructions.insert(
      f.Fragment().code.instructions.begin(),
      { FunctionCode::kOp_Cast, { Id("parentRef"), Id("self") } });
    error = "Ambiguous Papyrus script cast";
  }
  auto& actor = f.Add(0xff000001);
  REQUIRE_THROWS_WITH(f.Run(actor), Catch::Matchers::ContainsSubstring(error));
  REQUIRE(QuestJournal::ScriptState(actor).empty());
}

TEST_CASE(
  "Resolved self property operands preserve the executing script binding",
  "[Quest][QuestVM][MethodDispatch]")
{
  Fixture f;
  auto sibling = MethodScript(f, "OtherBinding", "Quest", true);
  Object::PropInfo property;
  property.name = "Count";
  property.type = "Int";
  property.flags = 7;
  property.autoVarName = "counter";
  f.fixture->objectTable[0].properties.push_back(property);
  sibling->objectTable[0].properties.push_back(property);
  sibling->objectTable[0].variables.push_back(
    { "counter", "Int", 0, VarValue(99) });
  f.Fragment().code.instructions = {
    { FunctionCode::kOp_PropSet,
      { VarValue("Count"), Id("self"), VarValue(17) } },
    { FunctionCode::kOp_PropGet,
      { VarValue("Count"), Id("self"), Id("counter") } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  auto& actor = f.Add(0xff000001);
  REQUIRE(f.Run(actor));
  REQUIRE(MethodResult(actor) == 17);
  const auto state = QuestJournal::ScriptState(actor);
  REQUIRE_FALSE(
    state["scripts"]["4e50d:Skyrim.esm/otherbinding"]["values"].contains(
      "otherbinding/counter"));
}

namespace {
void InventoryFragment(Fixture& f, uint32_t target = 0x14)
{
  BaseScripts(f);
  f.Fragment().locals.push_back({ "target", "ObjectReference" });
  f.Fragment().locals.push_back({ "gold", "Form" });
  f.Fragment().code.instructions = {
    { FunctionCode::kOp_CallStatic,
      { VarValue("Game"), VarValue("GetForm"), Id("target"), VarValue(1),
        VarValue(static_cast<int32_t>(target)) } },
    { FunctionCode::kOp_CallStatic,
      { VarValue("Game"), VarValue("GetForm"), Id("gold"), VarValue(1),
        VarValue(0xf) } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("AddItem"), Id("target"), Id("temp"), VarValue(3), Id("gold"),
        VarValue(5), VarValue(true) } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("GetItemCount"), Id("target"), Id("counter"), VarValue(1),
        Id("gold") } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
}

MpActor& BoundBalgruuf(Fixture& f, MpActor& owner, uint32_t id = 0xff002010)
{
  f.server.worldState.AddForm(
    std::make_unique<MpActor>(
      LocationalData{ { 1, 2, 3 }, { 0, 0, 0 }, FormDesc::Tamriel() },
      FormCallbacks::DoNothing(), 0x13bbd),
    id);
  auto& copy = f.server.worldState.GetFormAt<MpActor>(id);
  copy.SetWorldInstance(owner.GetWorldInstance());
  copy.SetInventory(Inventory().AddItem(0xf, 3));
  CampaignReferenceBinding::Bind(owner, 0x1a677, copy);
  return copy;
}

void InventoryAlias(Fixture& f, bool fail = false)
{
  espm::QuestScriptData::Alias binding;
  binding.object.type = 1;
  binding.object.formId = kQuest;
  binding.object.alias = 4;
  binding.version = 5;
  binding.objectFormat = 2;
  binding.scripts.push_back({ "InventoryAliasFixture", 0, {} });
  f.metadata.aliases.push_back(binding);
  auto script = std::make_shared<PexScript>();
  script->source = "InventoryAliasFixture";
  Object object;
  object.NameIndex = script->source;
  object.parentClassName = "ReferenceAlias";
  object.variables.push_back({ "received", "Int", 0, VarValue(0) });
  FunctionInfo event;
  event.valid = true;
  event.returnType = "None";
  event.params = { { "item", "Form" },
                   { "count", "Int" },
                   { "reference", "ObjectReference" },
                   { "source", "ObjectReference" } };
  event.locals.push_back({ "temp", "None" });
  event.code.instructions = {
    { FunctionCode::kOp_IAdd,
      { Id("received"), Id("received"), Id("count") } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  if (fail)
    event.code.instructions.insert(
      event.code.instructions.end() - 1,
      { FunctionCode::kOp_CallMethod,
        { VarValue("UnsupportedInventoryEvent"), Id("self"), Id("temp"),
          VarValue(0) } });
  object.states.push_back({ "", { { "OnItemAdded", event } } });
  script->objectTable.push_back(object);
  f.extraScripts.push_back(script);
}
}

TEST_CASE("Campaign inventory natives stage rewards with the host journal",
          "[Quest][CampaignInventory]")
{
  Fixture f;
  InventoryFragment(f);
  auto& owner = f.Add(0xff002001);
  auto& guest = f.Add(0xff002002);
  owner.SetInventory(Inventory().AddItem(0xf, 10));
  guest.SetInventory(Inventory().AddItem(0xf, 30));
  const auto instance =
    "campaign:" + std::to_string(owner.GetFormId()) + ":fixture";
  owner.SetWorldInstance(instance);
  guest.SetWorldInstance(instance);
  REQUIRE(f.Run(guest));
  REQUIRE(owner.GetInventory().GetItemCount(0xf) == 15);
  REQUIRE(guest.GetInventory().GetItemCount(0xf) == 30);
  REQUIRE(Counter(owner) == 15);
  REQUIRE_FALSE(f.Run(guest));
  REQUIRE(owner.GetInventory().GetItemCount(0xf) == 15);
  f.Fragment().code.instructions.insert(
    f.Fragment().code.instructions.end() - 1,
    { FunctionCode::kOp_CallMethod,
      { VarValue("MissingAfterReward"), Id("self"), Id("temp"),
        VarValue(0) } });
  const auto before = QuestJournal::ScriptState(owner);
  REQUIRE_THROWS(f.Run(guest, 20));
  REQUIRE(owner.GetInventory().GetItemCount(0xf) == 15);
  REQUIRE(QuestJournal::Get(owner, kQuest).at("stage") == 10);
  REQUIRE(QuestJournal::ScriptState(owner) == before);
}

TEST_CASE("NPC inventory events update the bound campaign alias atomically",
          "[Quest][CampaignInventory]")
{
  Fixture f;
  InventoryFragment(f, 0x1a677);
  const bool fail = GENERATE(false, true);
  InventoryAlias(f, fail);
  auto& owner = f.Add(0xff002001);
  owner.SetWorldInstance("campaign:" + std::to_string(owner.GetFormId()) +
                         ":fixture");
  auto& npc = BoundBalgruuf(f, owner);
  if (fail) {
    REQUIRE_THROWS_WITH(
      f.Run(owner),
      Catch::Matchers::ContainsSubstring("UnsupportedInventoryEvent"));
    REQUIRE(npc.GetInventory().GetItemCount(0xf) == 3);
    REQUIRE(QuestJournal::Get(owner, kQuest).at("stage") == 0);
    REQUIRE(QuestJournal::ScriptState(owner).empty());
  } else {
    REQUIRE(f.Run(owner));
    REQUIRE(npc.GetInventory().GetItemCount(0xf) == 8);
    REQUIRE(Counter(owner) == 8);
    const auto saved = QuestJournal::ScriptState(owner);
    REQUIRE(saved["scripts"]["4e50d:Skyrim.esm/alias:4/inventoryaliasfixture"]
                 ["values"]["inventoryaliasfixture/received"]["value"] == 5);
  }
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(0x1a677));
}

TEST_CASE(
  "Inventory transfers retain extra data and never cross campaign copies",
  "[Quest][CampaignInventory]")
{
  Fixture f;
  InventoryFragment(f);
  auto& owner = f.Add(0xff002001);
  owner.SetWorldInstance("campaign:" + std::to_string(owner.GetFormId()) +
                         ":fixture");
  auto& npc = BoundBalgruuf(f, owner);
  Inventory::Entry named(0xf, 7);
  named.name = "Marked gold";
  named.health = 0.5f;
  Inventory inventory;
  inventory.AddItems({ { 0xf, 2 }, named });
  owner.SetInventory(inventory);
  f.Fragment().locals.push_back({ "destination", "ObjectReference" });
  auto& code = f.Fragment().code.instructions;
  code.erase(code.begin() + 2, code.end());
  code.push_back({ FunctionCode::kOp_CallStatic,
                   { VarValue("Game"), VarValue("GetForm"), Id("destination"),
                     VarValue(1), VarValue(0x1a677) } });
  code.push_back(
    { FunctionCode::kOp_CallMethod,
      { VarValue("RemoveItem"), Id("target"), Id("temp"), VarValue(4),
        Id("gold"), VarValue(20), VarValue(true), Id("destination") } });
  code.push_back({ FunctionCode::kOp_Return, { VarValue::None() } });
  REQUIRE(f.Run(owner));
  REQUIRE(owner.GetInventory().IsEmpty());
  REQUIRE(npc.GetInventory().GetItemCount(0xf) == 12);
  REQUIRE(npc.GetInventory().entries.size() == 2);
  REQUIRE(npc.GetInventory().entries[1] == named);
  npc.SetWorldInstance("");
  const auto before = QuestJournal::Snapshot(owner);
  REQUIRE_THROWS_WITH(f.Run(owner, 20),
                      Catch::Matchers::ContainsSubstring("not materialized"));
  REQUIRE(QuestJournal::Snapshot(owner) == before);
  REQUIRE(npc.GetInventory().GetItemCount(0xf) == 12);
}

TEST_CASE("Campaign reference bindings reject collisions and survive reload",
          "[Quest][CampaignInventory]")
{
  Fixture f;
  auto& owner = f.Add(0xff002001);
  auto& stranger = f.Add(0xff002002);
  owner.SetWorldInstance("campaign:" + std::to_string(owner.GetFormId()) +
                         ":fixture");
  auto& copy = BoundBalgruuf(f, owner);
  REQUIRE(&CampaignReferenceBinding::Resolve(owner, 0x1a677) == &copy);
  REQUIRE_THROWS_WITH(
    copy.SendPapyrusEvent("OnInit"),
    Catch::Matchers::ContainsSubstring("campaign event dispatcher"));
  REQUIRE_THROWS_WITH(
    f.server.worldState.SendPapyrusEvent(&copy, "OnInit", nullptr, 0),
    Catch::Matchers::ContainsSubstring("campaign event dispatcher"));
  REQUIRE_THROWS(CampaignReferenceBinding::Resolve(stranger, 0x1a677));
  REQUIRE_THROWS(BoundBalgruuf(f, owner, 0xff002011));
  REQUIRE_THROWS(CampaignReferenceBinding::Bind(owner, 0x14, copy));
  REQUIRE_THROWS(CampaignReferenceBinding::Bind(owner, 0x1a677, owner));
  Fixture restarted;
  auto& restored = restarted.Add(owner.GetFormId());
  RestoreCharacter(owner, restored);
  restarted.server.worldState.AddForm(
    std::make_unique<MpActor>(
      LocationalData{ { 1, 2, 3 }, { 0, 0, 0 }, FormDesc::Tamriel() },
      FormCallbacks::DoNothing(), 0x13bbd),
    copy.GetFormId());
  auto& reloaded =
    restarted.server.worldState.GetFormAt<MpActor>(copy.GetFormId());
  RestoreCharacter(copy, reloaded);
  REQUIRE(&CampaignReferenceBinding::Resolve(restored, 0x1a677) == &reloaded);
}

TEST_CASE("Reusing a character form ID cannot inherit its old campaign copies",
          "[Quest][CampaignInventory]")
{
  Fixture f;
  auto& owner = f.Add(0xff002001);
  const auto id = owner.GetFormId();
  const auto instance = "campaign:" + std::to_string(id) + ":fixture";
  owner.SetWorldInstance(instance);
  BoundBalgruuf(f, owner);
  f.server.worldState.DestroyForm(id);
  auto& replacement = f.Add(id);
  replacement.SetWorldInstance(instance);
  REQUIRE_THROWS_WITH(CampaignReferenceBinding::Resolve(replacement, 0x1a677),
                      Catch::Matchers::ContainsSubstring("not materialized"));
}

TEST_CASE("Inventory events inherit active-state handlers independently per "
          "attachment",
          "[Quest][CampaignInventory]")
{
  Fixture f;
  InventoryFragment(f, 0x1a677);
  InventoryAlias(f);
  auto parent = f.extraScripts.back();
  parent->source = "InventoryEventBase";
  auto& object = parent->objectTable[0];
  object.NameIndex = parent->source;
  auto active = object.states[0].functions[0].function;
  active.code.instructions[0].args[2] = VarValue(50);
  object.states.push_back({ "busy", { { "OnItemAdded", active } } });
  for (const std::string name :
       { "InventoryAliasFixture", "OtherInventoryAlias" }) {
    auto child = std::make_shared<PexScript>();
    child->source = name;
    Object definition;
    definition.NameIndex = name;
    definition.parentClassName = parent->source;
    definition.autoStateName = "BUSY";
    child->objectTable.push_back(definition);
    f.extraScripts.push_back(child);
  }
  f.metadata.aliases[0].scripts.push_back({ "OtherInventoryAlias", 0, {} });
  auto& owner = f.Add(0xff002001);
  owner.SetWorldInstance("campaign:" + std::to_string(owner.GetFormId()) +
                         ":fixture");
  auto& npc = BoundBalgruuf(f, owner);
  REQUIRE(f.Run(owner));
  REQUIRE(npc.GetInventory().GetItemCount(0xf) == 8);
  const auto state = QuestJournal::ScriptState(owner);
  for (const std::string name :
       { "inventoryaliasfixture", "otherinventoryalias" })
    REQUIRE(state["scripts"]["4e50d:Skyrim.esm/alias:4/" + name]["values"]
                 ["inventoryeventbase/received"]["value"] == 50);
}

TEST_CASE(
  "Self-generating inventory events cannot publish an unbounded reward",
  "[Quest][CampaignInventory]")
{
  Fixture f;
  InventoryFragment(f, 0x1a677);
  InventoryAlias(f);
  auto& event =
    f.extraScripts.back()->objectTable[0].states[0].functions[0].function;
  event.locals.push_back({ "target", "ObjectReference" });
  event.code.instructions.insert(
    event.code.instructions.end() - 1,
    { { FunctionCode::kOp_CallMethod,
        { VarValue("GetReference"), Id("self"), Id("target"), VarValue(0) } },
      { FunctionCode::kOp_CallMethod,
        { VarValue("AddItem"), Id("target"), Id("temp"), VarValue(3),
          Id("item"), VarValue(1), VarValue(true) } } });
  auto& owner = f.Add(0xff002001);
  owner.SetWorldInstance("campaign:" + std::to_string(owner.GetFormId()) +
                         ":fixture");
  auto& npc = BoundBalgruuf(f, owner);
  REQUIRE_THROWS(f.Run(owner));
  REQUIRE(npc.GetInventory().GetItemCount(0xf) == 3);
  REQUIRE(QuestJournal::Get(owner, kQuest).at("stage") == 0);
  REQUIRE(QuestJournal::ScriptState(owner).empty());
}

namespace {
MpObjectReference& BoundTrigger(Fixture& f, MpActor& owner)
{
  auto& world = f.server.worldState;
  const auto source =
    world.GetEspm().GetBrowser().LookupById(kScriptedReference);
  const auto data = reinterpret_cast<const espm::REFR*>(source.rec)
                      ->GetData(world.GetEspmCache());
  const auto base = source.ToGlobalId(data.baseId);
  const auto type =
    world.GetEspm().GetBrowser().LookupById(base).rec->GetType().ToString();
  constexpr uint32_t id = 0xff002020;
  world.AddForm(
    std::make_unique<MpObjectReference>(
      LocationalData{ { 1, 2, 3 }, { 0, 0, 0 }, FormDesc::Tamriel() },
      FormCallbacks::DoNothing(), base, type.c_str()),
    id);
  auto& copy = world.GetFormAt<MpObjectReference>(id);
  copy.SetWorldInstance(owner.GetWorldInstance());
  copy.SetInventory(Inventory());
  CampaignReferenceBinding::Bind(owner, kScriptedReference, copy);
  return copy;
}

void ReferenceLoadFixture(Fixture& f)
{
  ReferenceFixture(f);
  auto& state = f.extraScripts.back()->objectTable[0].states[0];
  for (const auto& [name, increment] :
       std::vector<std::pair<std::string, int>>{
         { "OnInit", 100 }, { "OnCellLoad", 10 }, { "OnLoad", 1 } }) {
    FunctionInfo handler;
    handler.valid = true;
    handler.returnType = "None";
    handler.locals = { { "player", "Actor" },
                       { "gold", "Form" },
                       { "temp", "None" } };
    handler.code.instructions = {
      { FunctionCode::kOp_IAdd,
        { Id("counter"), Id("counter"), VarValue(increment) } },
      { FunctionCode::kOp_Return, { VarValue::None() } }
    };
    if (name == "OnInit") {
      handler.code.instructions.insert(
        handler.code.instructions.end() - 1,
        { { FunctionCode::kOp_CallStatic,
            { VarValue("Game"), VarValue("GetPlayer"), Id("player"),
              VarValue(0) } },
          { FunctionCode::kOp_CallStatic,
            { VarValue("Game"), VarValue("GetForm"), Id("gold"), VarValue(1),
              VarValue(0xf) } },
          { FunctionCode::kOp_CallMethod,
            { VarValue("AddItem"), Id("player"), Id("temp"), VarValue(3),
              Id("gold"), VarValue(5), VarValue(true) } } });
    }
    state.functions.push_back({ name, handler });
  }
}
}

TEST_CASE("Campaign reference initialization rewards run once across reloads",
          "[Quest][ReferenceLifecycle]")
{
  Fixture f;
  ReferenceLoadFixture(f);
  auto& owner = f.Add(0xff002001);
  auto& guest = f.Add(0xff002002);
  const auto phase =
    "campaign:" + std::to_string(owner.GetFormId()) + ":fixture";
  owner.SetWorldInstance(phase);
  guest.SetWorldInstance(phase);
  owner.SetInventory(Inventory());
  guest.SetInventory(Inventory());
  auto& copy = BoundTrigger(f, owner);
  REQUIRE_FALSE(QuestJournal::ReferenceInitialized(owner, kScriptedReference));
  REQUIRE_THROWS(
    QuestJournal::MarkReferenceInitialized(owner, kScriptedReference));
  QuestScriptRuntime::ExecuteReferenceLoad(guest, kScriptedReference,
                                           f.Scripts());
  REQUIRE(SavedReferenceCounter(owner) == 111);
  REQUIRE(owner.GetInventory().GetItemCount(0xf) == 5);
  REQUIRE(guest.GetInventory().GetItemCount(0xf) == 0);
  REQUIRE(QuestJournal::ReferenceInitialized(owner, kScriptedReference));
  REQUIRE_FALSE(QuestJournal::Snapshot(owner).contains("referenceInit"));
  QuestScriptRuntime::ExecuteReferenceLoad(owner, kScriptedReference,
                                           f.Scripts());
  REQUIRE(SavedReferenceCounter(owner) == 122);
  REQUIRE(owner.GetInventory().GetItemCount(0xf) == 5);

  Fixture restarted;
  ReferenceLoadFixture(restarted);
  // The copy must restore before its owner without executing scripts or
  // initializing the owner's history as a side effect of database iteration.
  restarted.server.worldState.LoadChangeForm(copy.GetChangeForm(),
                                             FormCallbacks::DoNothing());
  // Match PartOne's startup path: saved players are disabled until login.
  // Enabling inside a trigger also requires the separate trigger dispatcher.
  auto ownerSave = owner.GetChangeForm();
  ownerSave.profileId = 42;
  ownerSave.isDisabled = true;
  restarted.server.worldState.LoadChangeForm(ownerSave,
                                             FormCallbacks::DoNothing());
  auto& restored =
    restarted.server.worldState.GetFormAt<MpActor>(owner.GetFormId());
  auto& reloaded =
    restarted.server.worldState.GetFormAt<MpObjectReference>(copy.GetFormId());
  REQUIRE(CampaignReferenceBinding::Context(reloaded).first == &restored);
  REQUIRE(CampaignReferenceBinding::Context(reloaded).second ==
          kScriptedReference);
  REQUIRE(SavedReferenceCounter(restored) == 122);
  QuestScriptRuntime::ExecuteReferenceLoad(restored, kScriptedReference,
                                           restarted.Scripts());
  REQUIRE(SavedReferenceCounter(restored) == 133);
  REQUIRE(restored.GetInventory().GetItemCount(0xf) == 5);
  REQUIRE(QuestJournal::ReferenceInitialized(restored, kScriptedReference));
  guest.SetWorldInstance("");
  REQUIRE(QuestJournal::ScriptState(guest).empty());
  REQUIRE_FALSE(QuestJournal::ReferenceInitialized(guest, kScriptedReference));
}

TEST_CASE("Failed campaign load rolls back initialization and permits retry",
          "[Quest][ReferenceLifecycle]")
{
  Fixture f;
  ReferenceLoadFixture(f);
  auto& owner = f.Add(0xff002001);
  owner.SetWorldInstance("campaign:" + std::to_string(owner.GetFormId()) +
                         ":fixture");
  owner.SetInventory(Inventory());
  BoundTrigger(f, owner);
  auto& load =
    f.extraScripts.back()->objectTable[0].states[0].functions.back().function;
  load.code.instructions.insert(load.code.instructions.end() - 1,
                                { FunctionCode::kOp_CallMethod,
                                  { VarValue("UnsupportedLoadEffect"),
                                    Id("self"), Id("temp"), VarValue(0) } });
  const auto before =
    owner.GetDynamicFields().GetValueDump(QuestJournal::kProperty);
  REQUIRE_THROWS(QuestScriptRuntime::ExecuteReferenceLoad(
    owner, kScriptedReference, f.Scripts()));
  REQUIRE(owner.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          before);
  REQUIRE_FALSE(QuestJournal::ReferenceInitialized(owner, kScriptedReference));
  REQUIRE(QuestJournal::ScriptState(owner).empty());
  REQUIRE(owner.GetInventory().GetItemCount(0xf) == 0);
  load.code.instructions.erase(load.code.instructions.end() - 2);
  QuestScriptRuntime::ExecuteReferenceLoad(owner, kScriptedReference,
                                           f.Scripts());
  REQUIRE(SavedReferenceCounter(owner) == 111);
  REQUIRE(owner.GetInventory().GetItemCount(0xf) == 5);
}

namespace {
FunctionInfo InteractionHandler(bool reward = true)
{
  FunctionInfo handler;
  handler.valid = true;
  handler.returnType = "None";
  handler.params = { { "source", "ObjectReference" } };
  handler.locals = { { "gold", "Form" }, { "temp", "None" } };
  handler.code.instructions = {
    { FunctionCode::kOp_IAdd, { Id("counter"), Id("counter"), VarValue(1) } },
    { FunctionCode::kOp_Assign, { Id("lastSource"), Id("source") } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("GetFormID"), Id("source"), Id("sourceId"), VarValue(0) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  if (reward)
    handler.code.instructions.insert(
      handler.code.instructions.end() - 1,
      { { FunctionCode::kOp_CallStatic,
          { VarValue("Game"), VarValue("GetForm"), Id("gold"), VarValue(1),
            VarValue(0xf) } },
        { FunctionCode::kOp_CallMethod,
          { VarValue("AddItem"), Id("source"), Id("temp"), VarValue(3),
            Id("gold"), VarValue(2), VarValue(true) } } });
  return handler;
}

void InteractionFixture(Fixture& f)
{
  ReferenceLoadFixture(f);
  auto& object = f.extraScripts.back()->objectTable[0];
  object.variables.push_back({ "sourceId", "Int", 0, VarValue(0) });
  object.variables.push_back(
    { "lastSource", "ObjectReference", 0, VarValue::None() });
  for (const auto* name : { "OnActivate", "OnTriggerEnter", "OnTriggerLeave",
                            "OnTrigger", "SkympOnActivateClose" })
    object.states[0].functions.push_back({ name, InteractionHandler() });
}
}

TEST_CASE("Private interactions use the host PlayerRef for party guests",
          "[Quest][ReferenceEvents]")
{
  const auto* name = GENERATE("OnActivate", "OnTriggerEnter", "OnTriggerLeave",
                              "OnTrigger", "SkympOnActivateClose");
  Fixture f;
  InteractionFixture(f);
  auto& owner = f.Add(0xff002001);
  auto& guest = f.Add(0xff002002);
  owner.SetWorldInstance("campaign:" + std::to_string(owner.GetFormId()) +
                         ":fixture");
  guest.SetWorldInstance(owner.GetWorldInstance());
  owner.SetInventory(Inventory());
  guest.SetInventory(Inventory());
  auto& copy = BoundTrigger(f, owner);
  auto source = guest.ToVarValue();
  REQUIRE_THROWS_WITH(QuestScriptRuntime::ExecuteReferenceEvent(
                        copy, name, &source, 1, f.Scripts()),
                      Catch::Matchers::ContainsSubstring("initialization"));
  QuestScriptRuntime::ExecuteReferenceLoad(owner, kScriptedReference,
                                           f.Scripts());
  QuestScriptRuntime::ExecuteReferenceEvent(copy, name, &source, 1,
                                            f.Scripts());
  REQUIRE(SavedReferenceCounter(owner) == 112);
  REQUIRE(owner.GetInventory().GetItemCount(0xf) == 7);
  REQUIRE(guest.GetInventory().GetItemCount(0xf) == 0);
  const auto values =
    QuestJournal::ScriptState(owner)["scripts"][kReferenceState]["values"];
  REQUIRE(values["defaultsetstagetrigscript/sourceid"]["value"] == 0x14);
  REQUIRE(values["defaultsetstagetrigscript/lastsource"]["value"] ==
          "14:Skyrim.esm");
  REQUIRE(MpFormGameObject(&copy).ListActivePexInstances().empty());
  guest.SetWorldInstance("");
  REQUIRE(QuestJournal::ScriptState(guest).empty());
  REQUIRE_THROWS(QuestScriptRuntime::ExecuteReferenceEvent(copy, name, &source,
                                                           1, f.Scripts()));
  REQUIRE(SavedReferenceCounter(owner) == 112);
}

TEST_CASE("Private interaction sources resolve to their original NPC identity",
          "[Quest][ReferenceEvents]")
{
  Fixture f;
  InteractionFixture(f);
  auto& owner = f.Add(0xff002001);
  owner.SetWorldInstance("campaign:" + std::to_string(owner.GetFormId()) +
                         ":fixture");
  owner.SetInventory(Inventory());
  auto& copy = BoundTrigger(f, owner);
  auto& npc = BoundBalgruuf(f, owner);
  QuestScriptRuntime::ExecuteReferenceLoad(owner, kScriptedReference,
                                           f.Scripts());
  auto source = npc.ToVarValue();
  QuestScriptRuntime::ExecuteReferenceEvent(copy, "OnTriggerEnter", &source, 1,
                                            f.Scripts());
  REQUIRE(SavedReferenceCounter(owner) == 112);
  REQUIRE(npc.GetInventory().GetItemCount(0xf) == 5);
  REQUIRE(owner.GetInventory().GetItemCount(0xf) == 5);
  const auto values =
    QuestJournal::ScriptState(owner)["scripts"][kReferenceState]["values"];
  REQUIRE(values["defaultsetstagetrigscript/sourceid"]["value"] == 0x1a677);
  REQUIRE(values["defaultsetstagetrigscript/lastsource"]["value"] ==
          "1a677:Skyrim.esm");
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(0x1a677));
}

TEST_CASE("Failed interaction handlers roll back state and inventory rewards",
          "[Quest][ReferenceEvents]")
{
  Fixture f;
  InteractionFixture(f);
  auto& owner = f.Add(0xff002001);
  owner.SetWorldInstance("campaign:" + std::to_string(owner.GetFormId()) +
                         ":fixture");
  owner.SetInventory(Inventory());
  auto& copy = BoundTrigger(f, owner);
  QuestScriptRuntime::ExecuteReferenceLoad(owner, kScriptedReference,
                                           f.Scripts());
  auto& handler =
    f.extraScripts.back()->objectTable[0].states[0].functions.back().function;
  handler.code.instructions.insert(
    handler.code.instructions.end() - 1,
    { FunctionCode::kOp_CallMethod,
      { VarValue("UnsupportedInteractionEffect"), Id("self"), Id("temp"),
        VarValue(0) } });
  const auto before =
    owner.GetDynamicFields().GetValueDump(QuestJournal::kProperty);
  auto source = owner.ToVarValue();
  REQUIRE_THROWS_WITH(
    QuestScriptRuntime::ExecuteReferenceEvent(copy, "SkympOnActivateClose",
                                              &source, 1, f.Scripts()),
    Catch::Matchers::ContainsSubstring("UnsupportedInteractionEffect"));
  REQUIRE(owner.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          before);
  REQUIRE(owner.GetInventory().GetItemCount(0xf) == 5);
  handler.code.instructions.erase(handler.code.instructions.end() - 2);
  QuestScriptRuntime::ExecuteReferenceEvent(copy, "SkympOnActivateClose",
                                            &source, 1, f.Scripts());
  REQUIRE(SavedReferenceCounter(owner) == 112);
  REQUIRE(owner.GetInventory().GetItemCount(0xf) == 7);
}

TEST_CASE("Campaign event ingress rejects unsupported and foreign arguments",
          "[Quest][ReferenceEvents]")
{
  Fixture f;
  InteractionFixture(f);
  auto& owner = f.Add(0xff002001);
  auto& stranger = f.Add(0xff002002);
  owner.SetWorldInstance("campaign:" + std::to_string(owner.GetFormId()) +
                         ":fixture");
  stranger.SetWorldInstance(
    "campaign:" + std::to_string(stranger.GetFormId()) + ":fixture");
  auto& copy = BoundTrigger(f, owner);
  auto source = owner.ToVarValue();
  REQUIRE_THROWS(QuestScriptRuntime::ExecuteReferenceEvent(
    copy, "OnDeath", &source, 1, f.Scripts()));
  REQUIRE_THROWS(QuestScriptRuntime::ExecuteReferenceEvent(
    copy, nullptr, &source, 1, f.Scripts()));
  REQUIRE_THROWS(QuestScriptRuntime::ExecuteReferenceEvent(
    copy, "OnActivate", nullptr, 1, f.Scripts()));
  REQUIRE_THROWS(QuestScriptRuntime::ExecuteReferenceEvent(
    copy, "OnActivate", &source, 2, f.Scripts()));
  const auto logical = std::make_shared<CampaignFormObject>(
    espm::LookupResult{}, owner.GetFormId(), 0x14);
  for (auto argument : { VarValue::None(), VarValue(0x14),
                         stranger.ToVarValue(), VarValue(logical.get()) }) {
    REQUIRE_THROWS(QuestScriptRuntime::ExecuteReferenceEvent(
      copy, "OnActivate", &argument, 1, f.Scripts()));
  }
  // An unbound NPC in the same instance is not a campaign reference.
  f.server.worldState.AddForm(
    std::make_unique<MpActor>(
      LocationalData{ { 1, 2, 3 }, { 0, 0, 0 }, FormDesc::Tamriel() },
      FormCallbacks::DoNothing(), 0x13bbd),
    0xff002030);
  auto& unbound = f.server.worldState.GetFormAt<MpActor>(0xff002030);
  unbound.SetWorldInstance(owner.GetWorldInstance());
  auto argument = unbound.ToVarValue();
  REQUIRE_THROWS(QuestScriptRuntime::ExecuteReferenceEvent(
    copy, "OnActivate", &argument, 1, f.Scripts()));
  // An ordinarily loadable door, unlike protected/unique vanilla NPCs.
  constexpr uint32_t staleSource = 87048;
  const auto shared = f.server.worldState.LookupFormById(staleSource);
  REQUIRE(shared);
  auto expired = shared->ToVarValue();
  f.server.worldState.DestroyForm(staleSource);
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(staleSource));
  REQUIRE_THROWS(QuestScriptRuntime::ExecuteReferenceEvent(
    copy, "OnActivate", &expired, 1, f.Scripts()));
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(staleSource));
  REQUIRE(QuestJournal::ScriptState(owner).empty());
}

TEST_CASE("Private interactions dispatch to matching running quest aliases",
          "[Quest][ReferenceEvents]")
{
  Fixture f;
  InteractionFixture(f);
  auto alias = std::make_shared<PexScript>();
  alias->source = "defaultsetstagealiasscript";
  Object object;
  object.NameIndex = alias->source;
  object.parentClassName = "ReferenceAlias";
  object.variables = { { "counter", "Int", 0, VarValue(0) },
                       { "sourceId", "Int", 0, VarValue(0) },
                       { "lastSource", "ObjectReference", 0,
                         VarValue::None() } };
  auto handler = InteractionHandler(false);
  const bool fail = GENERATE(false, true);
  if (fail)
    handler.code.instructions.insert(
      handler.code.instructions.end() - 1,
      { FunctionCode::kOp_CallMethod,
        { VarValue("UnsupportedAliasInteraction"), Id("self"), Id("temp"),
          VarValue(0) } });
  object.states.push_back({ "", { { "OnActivate", handler } } });
  alias->objectTable.push_back(object);
  f.extraScripts.push_back(alias);
  auto& owner = f.Add(0xff002001);
  owner.SetWorldInstance("campaign:" + std::to_string(owner.GetFormId()) +
                         ":fixture");
  owner.SetInventory(Inventory());
  auto& copy = BoundTrigger(f, owner);
  QuestScriptRuntime::ExecuteReferenceLoad(owner, kScriptedReference,
                                           f.Scripts());
  REQUIRE(
    QuestJournal::Apply(owner, { { "quest", kQuest }, { "op", "start" } }));
  // Controlled saved fills for the two scripted aliases in the owned QUST.
  // Filling these world-query aliases automatically remains unsupported.
  QuestJournal::Atomic(owner, [&] {
    QuestVmState state(QuestJournal::ScriptState(owner), {}, {});
    state.SetAliasBinding("4e50d:Skyrim.esm/alias:1", "6debb:Skyrim.esm");
    state.SetAliasBinding("4e50d:Skyrim.esm/alias:3", "14:Skyrim.esm");
    QuestJournal::SetScriptState(owner, state.Save());
  });
  const auto before =
    owner.GetDynamicFields().GetValueDump(QuestJournal::kProperty);
  auto source = owner.ToVarValue();
  if (fail) {
    REQUIRE_THROWS_WITH(
      QuestScriptRuntime::ExecuteReferenceEvent(copy, "OnActivate", &source, 1,
                                                f.Scripts()),
      Catch::Matchers::ContainsSubstring("UnsupportedAliasInteraction"));
    REQUIRE(owner.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
            before);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 5);
  } else {
    QuestScriptRuntime::ExecuteReferenceEvent(copy, "OnActivate", &source, 1,
                                              f.Scripts());
    const auto saved = QuestJournal::ScriptState(owner);
    for (const auto id : { 1, 3 }) {
      const auto key = "4e50d:Skyrim.esm/alias:" + std::to_string(id) +
        "/defaultsetstagealiasscript";
      const auto& values = saved.at("scripts").at(key).at("values");
      if (id == 1)
        REQUIRE(values.at("defaultsetstagealiasscript/counter").at("value") ==
                1);
      else
        REQUIRE_FALSE(values.contains("defaultsetstagealiasscript/counter"));
    }
    REQUIRE(SavedReferenceCounter(owner) == 112);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 7);
    REQUIRE(
      QuestJournal::Apply(owner, { { "quest", kQuest }, { "op", "stop" } }));
    QuestScriptRuntime::ExecuteReferenceEvent(copy, "OnActivate", &source, 1,
                                              f.Scripts());
    REQUIRE(
      QuestJournal::ScriptState(
        owner)["scripts"]
              ["4e50d:Skyrim.esm/alias:1/defaultsetstagealiasscript"] ==
      saved["scripts"]["4e50d:Skyrim.esm/alias:1/defaultsetstagealiasscript"]);
    REQUIRE(SavedReferenceCounter(owner) == 113);
  }
}

TEST_CASE("Private native event entry never initializes the shared VM",
          "[Quest][ReferenceEvents]")
{
  Fixture f;
  auto& owner = f.Add(0xff002001);
  owner.SetWorldInstance("campaign:" + std::to_string(owner.GetFormId()) +
                         ":fixture");
  auto& npc = BoundBalgruuf(f, owner);
  f.server.worldState.AttachScriptStorage(
    std::make_shared<BsaArchiveScriptStorage>(
      (std::filesystem::path(GetDataDir()) / "Skyrim - Misc.bsa")
        .string()
        .c_str()));
  QuestScriptRuntime::ExecuteReferenceLoad(owner, 0x1a677);
  auto source = owner.ToVarValue();
  REQUIRE_NOTHROW(npc.SendPapyrusEvent("OnActivate", &source, 1));
  REQUIRE(MpFormGameObject(&npc).ListActivePexInstances().empty());
  // Direct global-VM entry remains forbidden, even for supported events.
  REQUIRE_THROWS_WITH(
    f.server.worldState.SendPapyrusEvent(&npc, "OnActivate", &source, 1),
    Catch::Matchers::ContainsSubstring("campaign event dispatcher"));
}

TEST_CASE(
  "Private reference subscription retries failed load before replication",
  "[Quest][ReferenceLifecycle]")
{
  Fixture f;
  auto& owner = f.Add(0xff002001);
  owner.SetWorldInstance("campaign:" + std::to_string(owner.GetFormId()) +
                         ":fixture");
  auto& npc = BoundBalgruuf(f, owner);
  owner.RegisterProfileId(42);
  // Missing storage fails before marking the first load complete.
  REQUIRE_THROWS_WITH(MpObjectReference::Subscribe(&npc, &owner),
                      Catch::Matchers::ContainsSubstring("owned PEX storage"));
  REQUIRE_FALSE(QuestJournal::ReferenceInitialized(owner, 0x1a677));
  f.server.worldState.AttachScriptStorage(
    std::make_shared<BsaArchiveScriptStorage>(
      (std::filesystem::path(GetDataDir()) / "Skyrim - Misc.bsa")
        .string()
        .c_str()));
  REQUIRE_NOTHROW(MpObjectReference::Subscribe(&npc, &owner));
  REQUIRE(QuestJournal::ReferenceInitialized(owner, 0x1a677));
  const auto saved =
    owner.GetDynamicFields().GetValueDump(QuestJournal::kProperty);
  REQUIRE_NOTHROW(MpObjectReference::Subscribe(&npc, &owner));
  REQUIRE(owner.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          saved);
}

namespace {
struct RuntimeIntegrationStorage
  : Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>
{
  std::vector<std::vector<std::optional<MpChangeForm>>> batches;
  bool SupportsAtomicBatches() const noexcept override { return true; }
  void IterateSync(const IterateSyncCallback&) override {}
  void Upsert(std::vector<std::optional<MpChangeForm>>&& forms,
              const UpsertCallback& callback) override
  {
    batches.push_back(std::move(forms));
    callback();
  }
  void Iterate(const IterateCallback& callback,
               const std::optional<std::vector<FormDesc>>&) override
  {
    callback({});
  }
  uint32_t GetNumFinishedUpserts() const override { return batches.size(); }
  uint32_t GetNumFinishedIterates() const override { return 0; }
  void Tick() override {}
  bool GetRecycledChangeFormsBuffer(
    std::vector<std::optional<MpChangeForm>>&) override
  {
    return false;
  }
  const std::string& GetName() const override
  {
    static const std::string name = "quest-runtime-integration";
    return name;
  }
};

FunctionInfo RuntimeWorldHandler(bool disabled)
{
  auto handler = InteractionHandler();
  handler.locals.insert(
    handler.locals.end(),
    { { "player", "Actor" }, { "npc", "Actor" }, { "quest", "Quest" } });
  auto& code = handler.code.instructions;
  code.insert(code.end() - 1,
              { { FunctionCode::kOp_CallStatic,
                  { VarValue("Game"), VarValue("GetPlayer"), Id("player"),
                    VarValue(0) } },
                { FunctionCode::kOp_CallStatic,
                  { VarValue("Game"), VarValue("GetForm"), Id("npc"),
                    VarValue(1), VarValue(0x1a677) } } });
  for (const auto& [method, target, destination] :
       std::vector<std::tuple<const char*, const char*, const char*>>{
         { "GetPositionX", "player", "playerX" },
         { "GetPositionY", "player", "playerY" },
         { "GetPositionZ", "player", "playerZ" },
         { "IsDead", "player", "playerDead" },
         { "GetPositionX", "npc", "npcX" },
         { "GetPositionY", "npc", "npcY" },
         { "GetPositionZ", "npc", "npcZ" },
         { "IsDead", "npc", "npcDead" } }) {
    code.insert(
      code.end() - 1,
      { FunctionCode::kOp_CallMethod,
        { VarValue(method), Id(target), Id(destination), VarValue(0) } });
  }
  code.insert(code.end() - 1,
              { { FunctionCode::kOp_CallMethod,
                  { VarValue("IsDisabled"), Id("npc"), Id("wasDisabled"),
                    VarValue(0) } },
                { FunctionCode::kOp_CallMethod,
                  { VarValue(disabled ? "Disable" : "Enable"), Id("npc"),
                    Id("temp"), VarValue(1), VarValue(false) } },
                { FunctionCode::kOp_CallMethod,
                  { VarValue("IsDisabled"), Id("npc"), Id("nowDisabled"),
                    VarValue(0) } },
                { FunctionCode::kOp_CallStatic,
                  { VarValue("Game"), VarValue("GetForm"), Id("quest"),
                    VarValue(1), VarValue(static_cast<int32_t>(kQuest)) } },
                { FunctionCode::kOp_CallMethod,
                  { VarValue("SetObjectiveDisplayed"), Id("quest"), Id("temp"),
                    VarValue(2), VarValue(10), VarValue(disabled) } } });
  return handler;
}

// These controlled PEX programs deliberately use the production runtime's
// native registrations, logical PlayerRef, bound private NPC and event path.
// They establish transaction behavior, not support for any original quest.
struct RuntimeWorldFixture : Fixture
{
  std::shared_ptr<RuntimeIntegrationStorage> storage =
    std::make_shared<RuntimeIntegrationStorage>();
  MpActor *owner, *guest, *npc;
  MpObjectReference* trigger;

  RuntimeWorldFixture()
  {
    InteractionFixture(*this);
    auto& object = extraScripts.back()->objectTable[0];
    for (const auto* target : { "player", "npc" }) {
      for (const auto* axis : { "X", "Y", "Z" })
        object.variables.push_back(
          { std::string(target) + axis, "Float", 0, VarValue(0.f) });
      object.variables.push_back(
        { std::string(target) + "Dead", "Bool", 0, VarValue(false) });
    }
    for (const auto* name : { "wasDisabled", "nowDisabled" })
      object.variables.push_back({ name, "Bool", 0, VarValue(false) });
    for (auto& function : object.states[0].functions) {
      if (function.name == "OnActivate")
        function.function = RuntimeWorldHandler(true);
      if (function.name == "OnTriggerEnter")
        function.function = RuntimeWorldHandler(false);
    }
    // Game.GetForm resolves the owned QUST's VMAD bindings. Supply controlled
    // empty subclasses for those names; only the registered Quest natives
    // are exercised here, never the original quest's fragments.
    const auto record =
      server.worldState.GetEspm().GetBrowser().LookupById(kQuest);
    const auto questData = espm::QuestScriptData::Read(
      record.rec, server.worldState.GetEspmCache());
    for (const auto& binding : questData.scripts) {
      if (binding.status & 2)
        continue;
      auto script = std::make_shared<PexScript>();
      script->source = binding.name;
      Object questObject;
      questObject.NameIndex = script->source;
      questObject.parentClassName = "Quest";
      script->objectTable.push_back(questObject);
      extraScripts.push_back(script);
    }
    server.AttachSaveStorage(storage);
    owner = &Add(0xff002001);
    guest = &Add(0xff002002);
    owner->SetWorldInstance("campaign:" + std::to_string(owner->GetFormId()) +
                            ":fixture");
    guest->SetWorldInstance(owner->GetWorldInstance());
    guest->SetPosAndAngleSilent({ 1000, 2000, 3000 }, {});
    owner->SetInventory(Inventory());
    guest->SetInventory(Inventory());
    trigger = &BoundTrigger(*this, *owner);
    npc = &BoundBalgruuf(*this, *owner);
    npc->SetPosAndAngleSilent({ 4, 6, 15 }, {});
    // Model a saved campaign corpse. SetIsDead invokes the ordinary death
    // event and schedules a respawn, which visibility correctly refuses to
    // mutate until transactional respawn handling exists. Restore through the
    // same private AddForm path used by WorldState::LoadChangeForm instead.
    auto savedNpc = npc->GetChangeForm();
    savedNpc.isDead = true;
    savedNpc.actorValues.healthPercentage = 0;
    const auto npcId = npc->GetFormId();
    server.DestroyActor(npcId);
    server.worldState.AddForm(
      std::make_unique<MpActor>(LocationalData(), FormCallbacks::DoNothing(),
                                0x13bbd),
      npcId, false, &savedNpc);
    npc = &server.worldState.GetFormAt<MpActor>(npcId);
    REQUIRE(npc->IsDead());
    REQUIRE_FALSE(npc->IsRespawning());
    REQUIRE_NOTHROW(npc->ValidateCampaignVisibilityChange());
    npc->SetInventory(Inventory().AddItem(0xf, 3));
    QuestScriptRuntime::ExecuteReferenceLoad(*guest, kScriptedReference,
                                             Scripts());
    server.worldState.Tick();
    storage->batches.clear();
  }

  void RunEvent(const char* name = "OnActivate")
  {
    auto source = guest->ToVarValue();
    QuestScriptRuntime::ExecuteReferenceEvent(*trigger, name, &source, 1,
                                              Scripts());
  }

  FunctionInfo& Activate()
  {
    for (const auto& script : extraScripts)
      if (script->source == "defaultSetStageTRIGScript")
        for (auto& function : script->objectTable[0].states[0].functions)
          if (function.name == "OnActivate")
            return function.function;
    throw std::runtime_error("Missing runtime integration handler");
  }

  void AddAliases(bool fail)
  {
    auto alias = std::make_shared<PexScript>();
    alias->source = "defaultsetstagealiasscript";
    Object object;
    object.NameIndex = alias->source;
    object.parentClassName = "ReferenceAlias";
    object.variables = { { "counter", "Int", 0, VarValue(0) },
                         { "sourceId", "Int", 0, VarValue(0) },
                         { "lastSource", "ObjectReference", 0,
                           VarValue::None() } };
    auto handler = InteractionHandler(false);
    if (fail)
      handler.code.instructions.insert(
        handler.code.instructions.end() - 1,
        { FunctionCode::kOp_CallMethod,
          { VarValue("UnsupportedWorldAliasEffect"), Id("self"), Id("temp"),
            VarValue(0) } });
    object.states.push_back(
      { "", { { "OnActivate", handler }, { "OnTriggerEnter", handler } } });
    alias->objectTable.push_back(object);
    extraScripts.push_back(alias);
    REQUIRE(
      QuestJournal::Apply(*owner, { { "quest", kQuest }, { "op", "start" } }));
    QuestJournal::Atomic(*owner, [&] {
      QuestVmState state(QuestJournal::ScriptState(*owner), {}, {});
      state.SetAliasBinding("4e50d:Skyrim.esm/alias:1", "6debb:Skyrim.esm");
      state.SetAliasBinding("4e50d:Skyrim.esm/alias:3", "14:Skyrim.esm");
      QuestJournal::SetScriptState(*owner, state.Save());
    });
    server.worldState.Tick();
    storage->batches.clear();
  }
};
}

TEST_CASE("Production quest VM publishes reference queries visibility rewards "
          "and matching running aliases together",
          "[Quest][QuestVM][CampaignRuntimeIntegration]")
{
  RuntimeWorldFixture f;
  f.AddAliases(false);
  const auto before = QuestJournal::ScriptState(*f.owner);
  int prepared = 0, published = 0;
  bool disabling = true;
  f.server.worldState.SetCampaignVisibilityCallbacks(
    [&](MpActor& owner) {
      REQUIRE(&owner == f.owner);
      REQUIRE(f.npc->IsDisabled() == !disabling);
      REQUIRE(owner.GetInventory().GetItemCount(0xf) == (disabling ? 5 : 7));
      REQUIRE(SavedReferenceCounter(owner) == (disabling ? 111 : 112));
      if (disabling) {
        REQUIRE(QuestJournal::ScriptState(owner) == before);
        REQUIRE(QuestJournal::Get(owner, kQuest).at("objectives").empty());
      } else {
        REQUIRE(QuestJournal::Get(
                  owner, kQuest)["objectives"]["10"]["displayed"] == true);
      }
      ++prepared;
    },
    [&](MpActor& owner, bool success) {
      REQUIRE(&owner == f.owner);
      REQUIRE(success);
      REQUIRE(prepared == published + 1);
      REQUIRE(f.npc->IsDisabled() == disabling);
      REQUIRE(owner.GetInventory().GetItemCount(0xf) == (disabling ? 7 : 9));
      REQUIRE(SavedReferenceCounter(owner) == (disabling ? 112 : 113));
      REQUIRE(QuestJournal::Get(
                owner, kQuest)["objectives"]["10"]["displayed"] == disabling);
      REQUIRE(
        QuestJournal::ScriptState(
          owner)["scripts"]
                ["4e50d:Skyrim.esm/alias:1/defaultsetstagealiasscript"]
                ["values"]["defaultsetstagealiasscript/counter"]["value"] ==
        1);
      ++published;
    });
  f.RunEvent();
  REQUIRE(prepared == 1);
  REQUIRE(published == 1);
  REQUIRE(f.guest->GetInventory().GetItemCount(0xf) == 0);
  REQUIRE(f.npc->GetInventory().GetItemCount(0xf) == 3);
  const auto saved = QuestJournal::ScriptState(*f.owner);
  const auto& values = saved.at("scripts").at(kReferenceState).at("values");
  for (const auto& [name, value] :
       std::vector<std::pair<std::string, int>>{ { "playerx", 1 },
                                                 { "playery", 2 },
                                                 { "playerz", 3 },
                                                 { "npcx", 4 },
                                                 { "npcy", 6 },
                                                 { "npcz", 15 } })
    REQUIRE(values.at("defaultsetstagetrigscript/" + name).at("value") ==
            value);
  REQUIRE(values.at("defaultsetstagetrigscript/playerdead").at("value") ==
          false);
  REQUIRE(values.at("defaultsetstagetrigscript/npcdead").at("value") == true);
  REQUIRE(values.at("defaultsetstagetrigscript/wasdisabled").at("value") ==
          false);
  REQUIRE(values.at("defaultsetstagetrigscript/nowdisabled").at("value") ==
          true);
  const auto aliasKey = "4e50d:Skyrim.esm/alias:1/defaultsetstagealiasscript";
  REQUIRE(saved["scripts"][aliasKey]["values"]
               ["defaultsetstagealiasscript/counter"]["value"] == 1);
  REQUIRE_FALSE(
    saved["scripts"]["4e50d:Skyrim.esm/alias:3/defaultsetstagealiasscript"]
         ["values"]
           .contains("defaultsetstagealiasscript/counter"));
  f.server.worldState.Tick();
  REQUIRE(f.storage->batches.size() == 1);
  const auto& batch = f.storage->batches.front();
  REQUIRE(batch.at(f.npc->GetIdx())->isDisabled);
  REQUIRE(batch.at(f.owner->GetIdx())->inv.GetItemCount(0xf) == 7);
  REQUIRE(batch.at(f.owner->GetIdx())
            ->dynamicFields.GetValueDump(QuestJournal::kProperty) ==
          f.owner->GetDynamicFields().GetValueDump(QuestJournal::kProperty));

  // A stopped quest's alias does not run, while the private reference still
  // enables its copy and restores an objective in the same production path.
  REQUIRE(
    QuestJournal::Apply(*f.owner, { { "quest", kQuest }, { "op", "stop" } }));
  disabling = false;
  f.RunEvent("OnTriggerEnter");
  REQUIRE(prepared == 2);
  REQUIRE(published == 2);
  const auto enabled = QuestJournal::ScriptState(*f.owner);
  REQUIRE(enabled["scripts"][aliasKey] == saved["scripts"][aliasKey]);
  REQUIRE(enabled["scripts"][kReferenceState]["values"]
                 ["defaultsetstagetrigscript/wasdisabled"]["value"] == true);
  REQUIRE(enabled["scripts"][kReferenceState]["values"]
                 ["defaultsetstagetrigscript/nowdisabled"]["value"] == false);
  f.server.worldState.Tick();
  REQUIRE(f.storage->batches.size() == 2);
  REQUIRE_FALSE(f.storage->batches.back().at(f.npc->GetIdx())->isDisabled);
  REQUIRE(
    f.storage->batches.back().at(f.owner->GetIdx())->inv.GetItemCount(0xf) ==
    9);
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(0x1a677));
}

TEST_CASE("Production quest VM rolls back staged visibility journal rewards "
          "and alias state when a handler or publication barrier fails",
          "[Quest][QuestVM][CampaignRuntimeIntegration]")
{
  const std::string failure =
    GENERATE("reference", "alias", "barrier", "missing barrier");
  RuntimeWorldFixture f;
  if (failure == "reference")
    f.Activate().code.instructions.insert(
      f.Activate().code.instructions.end() - 1,
      { FunctionCode::kOp_CallMethod,
        { VarValue("UnsupportedWorldReferenceEffect"), Id("self"), Id("temp"),
          VarValue(0) } });
  f.AddAliases(failure == "alias");
  const auto before =
    f.owner->GetDynamicFields().GetValueDump(QuestJournal::kProperty);
  const auto initialNpc = MpChangeForm::ToJson(f.npc->GetChangeForm());
  int prepared = 0, finished = 0;
  if (failure != "missing barrier")
    f.server.worldState.SetCampaignVisibilityCallbacks(
      [&](MpActor& owner) {
        REQUIRE(&owner == f.owner);
        REQUIRE_FALSE(f.npc->IsDisabled());
        REQUIRE(owner.GetInventory().GetItemCount(0xf) == 5);
        REQUIRE(owner.GetDynamicFields().GetValueDump(
                  QuestJournal::kProperty) == before);
        ++prepared;
        if (failure == "barrier")
          throw std::runtime_error("Fixture rejected visibility publication");
      },
      [&](MpActor& owner, bool success) {
        REQUIRE(&owner == f.owner);
        REQUIRE_FALSE(success);
        REQUIRE_FALSE(f.npc->IsDisabled());
        REQUIRE(owner.GetInventory().GetItemCount(0xf) == 5);
        REQUIRE(owner.GetDynamicFields().GetValueDump(
                  QuestJournal::kProperty) == before);
        ++finished;
      });
  const auto expected = failure == "reference"
    ? "UnsupportedWorldReferenceEffect"
    : failure == "alias"   ? "UnsupportedWorldAliasEffect"
    : failure == "barrier" ? "Fixture rejected visibility publication"
                           : "available projection barrier";
  REQUIRE_THROWS_WITH(f.RunEvent(),
                      Catch::Matchers::ContainsSubstring(expected));
  REQUIRE(f.owner->GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          before);
  REQUIRE(MpChangeForm::ToJson(f.npc->GetChangeForm()) == initialNpc);
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 5);
  REQUIRE(f.guest->GetInventory().GetItemCount(0xf) == 0);
  REQUIRE(prepared == (failure == "barrier" ? 1 : 0));
  REQUIRE(finished == (failure == "barrier" ? 1 : 0));
  f.server.worldState.Tick();
  REQUIRE(f.storage->batches.empty());
}

namespace {
// One authored PEX override, generated with test_quest_dependencies.make_pex.
// It replaces only this test's reference script, not any owned game file:
//   OnInit: counter = 100; Game.GetPlayer().AddItem(Game.GetForm(15), 5, true)
//   OnLoad: counter = 101; self.Disable(false)
// The unused ::stage_var accepts the placed reference's owned stage property.
// Production Subscribe loads these bytes through the real Reader/storage path.
class NestedVisibilityLoadStorage : public BsaArchiveScriptStorage
{
public:
  NestedVisibilityLoadStorage()
    : BsaArchiveScriptStorage(
        (std::filesystem::path(GetDataDir()) / "Skyrim - Misc.bsa")
          .string()
          .c_str())
  {
  }
  int reads = 0;
  std::vector<uint8_t> GetScriptPex(const char* name) override
  {
    if (CIString(name) != CIString("defaultSetStageTRIGScript"))
      return BsaArchiveScriptStorage::GetScriptPex(name);
    ++reads;
    return {
      0xfa, 0x57, 0xc0, 0xde, 0x03, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x66, 0x69, 0x78, 0x74, 0x75, 0x72,
      0x65, 0x2e, 0x70, 0x73, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x15, 0x00,
      0x0f, 0x4f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x52, 0x65, 0x66, 0x65, 0x72,
      0x65, 0x6e, 0x63, 0x65, 0x00, 0x00, 0x00, 0x07, 0x63, 0x6f, 0x75, 0x6e,
      0x74, 0x65, 0x72, 0x00, 0x03, 0x49, 0x6e, 0x74, 0x00, 0x0b, 0x3a, 0x3a,
      0x73, 0x74, 0x61, 0x67, 0x65, 0x5f, 0x76, 0x61, 0x72, 0x00, 0x06, 0x4f,
      0x6e, 0x49, 0x6e, 0x69, 0x74, 0x00, 0x04, 0x47, 0x61, 0x6d, 0x65, 0x00,
      0x09, 0x47, 0x65, 0x74, 0x50, 0x6c, 0x61, 0x79, 0x65, 0x72, 0x00, 0x06,
      0x70, 0x6c, 0x61, 0x79, 0x65, 0x72, 0x00, 0x07, 0x47, 0x65, 0x74, 0x46,
      0x6f, 0x72, 0x6d, 0x00, 0x04, 0x67, 0x6f, 0x6c, 0x64, 0x00, 0x07, 0x41,
      0x64, 0x64, 0x49, 0x74, 0x65, 0x6d, 0x00, 0x04, 0x74, 0x65, 0x6d, 0x70,
      0x00, 0x04, 0x6e, 0x6f, 0x6e, 0x65, 0x00, 0x05, 0x41, 0x63, 0x74, 0x6f,
      0x72, 0x00, 0x04, 0x46, 0x6f, 0x72, 0x6d, 0x00, 0x04, 0x4e, 0x6f, 0x6e,
      0x65, 0x00, 0x06, 0x4f, 0x6e, 0x4c, 0x6f, 0x61, 0x64, 0x00, 0x07, 0x44,
      0x69, 0x73, 0x61, 0x62, 0x6c, 0x65, 0x00, 0x04, 0x73, 0x65, 0x6c, 0x66,
      0x00, 0x19, 0x64, 0x65, 0x66, 0x61, 0x75, 0x6c, 0x74, 0x53, 0x65, 0x74,
      0x53, 0x74, 0x61, 0x67, 0x65, 0x54, 0x52, 0x49, 0x47, 0x53, 0x63, 0x72,
      0x69, 0x70, 0x74, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x14, 0x00, 0x00,
      0x00, 0xc7, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
      0x00, 0x02, 0x00, 0x02, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x04, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
      0x00, 0x01, 0x00, 0x02, 0x00, 0x05, 0x00, 0x0d, 0x00, 0x01, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x08, 0x00, 0x0e, 0x00,
      0x0a, 0x00, 0x0f, 0x00, 0x0c, 0x00, 0x10, 0x00, 0x05, 0x0d, 0x01, 0x00,
      0x02, 0x03, 0x00, 0x00, 0x00, 0x64, 0x19, 0x02, 0x00, 0x06, 0x02, 0x00,
      0x07, 0x01, 0x00, 0x08, 0x03, 0x00, 0x00, 0x00, 0x00, 0x19, 0x02, 0x00,
      0x06, 0x02, 0x00, 0x09, 0x01, 0x00, 0x0a, 0x03, 0x00, 0x00, 0x00, 0x01,
      0x03, 0x00, 0x00, 0x00, 0x0f, 0x17, 0x02, 0x00, 0x0b, 0x01, 0x00, 0x08,
      0x01, 0x00, 0x0c, 0x03, 0x00, 0x00, 0x00, 0x03, 0x01, 0x00, 0x0a, 0x03,
      0x00, 0x00, 0x00, 0x05, 0x05, 0x01, 0x1a, 0x00, 0x00, 0x11, 0x00, 0x0d,
      0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00,
      0x08, 0x00, 0x0e, 0x00, 0x0a, 0x00, 0x0f, 0x00, 0x0c, 0x00, 0x10, 0x00,
      0x03, 0x0d, 0x01, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, 0x65, 0x17, 0x02,
      0x00, 0x12, 0x01, 0x00, 0x13, 0x01, 0x00, 0x0c, 0x03, 0x00, 0x00, 0x00,
      0x01, 0x05, 0x00, 0x1a, 0x00,
    };
  }
};
}

TEST_CASE("Fresh player subscription rejects nested OnLoad visibility after "
          "outer commit",
          "[Quest][ReferenceLifecycle][CampaignVisibility]["
          "CampaignRuntimeIntegration]")
{
  Fixture f;
  auto scripts = std::make_shared<NestedVisibilityLoadStorage>();
  f.server.worldState.AttachScriptStorage(scripts);
  auto storage = std::make_shared<RuntimeIntegrationStorage>();
  f.server.AttachSaveStorage(storage);
  auto& owner = f.Add(0xff002001);
  auto& guest = f.Add(0xff002002);
  owner.SetWorldInstance("campaign:" + std::to_string(owner.GetFormId()) +
                         ":fixture");
  guest.SetWorldInstance(owner.GetWorldInstance());
  owner.SetInventory(Inventory());
  guest.SetInventory(Inventory());
  auto& copy = BoundTrigger(f, owner);
  copy.Disable();
  // Bots with profileId=-1 never enter the first-subscription load path.
  owner.RegisterProfileId(42);
  guest.RegisterProfileId(43);
  for (Networking::UserId user = 0; user < 2; ++user) {
    DoConnect(f.server, user);
    f.server.SetUserActor(user,
                          user == 0 ? owner.GetFormId() : guest.GetFormId());
    f.server.serverState.userInfo[user]->gameplayBlocked = true;
    f.server.BeginPacketEpoch(user);
    f.server.serverState.userInfo[user]->gameplayBlocked = false;
  }
  REQUIRE_FALSE(QuestJournal::ReferenceInitialized(owner, kScriptedReference));
  REQUIRE(copy.IsDisabled());
  f.server.worldState.Tick();
  storage->batches.clear();

  SECTION(
    "The authored load program completes without a nested visibility change")
  {
    // Disable(false) is a no-op on the already-disabled copy. This proves the
    // real PEX, owned base scripts, property binding and OnInit reward work;
    // the failure section must not pass because storage or bytecode is
    // invalid.
    REQUIRE_NOTHROW(
      QuestScriptRuntime::ExecuteReferenceLoad(owner, kScriptedReference));
    REQUIRE(scripts->reads == 1);
    REQUIRE(SavedReferenceCounter(owner) == 101);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 5);
    REQUIRE(QuestJournal::ReferenceInitialized(owner, kScriptedReference));
    REQUIRE(copy.IsDisabled());
  }
  SECTION(
    "An outer enable commits once and quarantines the failed nested load")
  {
    CampaignVisibilityBarrier::UserIds closed;
    int prepared = 0, finished = 0;
    bool synchronized = true;
    CampaignVisibilityBarrier barrier(
      f.server,
      [&](auto id, const auto& users) {
        REQUIRE(id == owner.GetFormId());
        REQUIRE(copy.IsDisabled());
        REQUIRE(owner.GetInventory().GetItemCount(0xf) == 0);
        REQUIRE(users == CampaignVisibilityBarrier::UserIds{ 0, 1 });
        for (auto user : users)
          f.server.BeginPacketEpoch(user);
        ++prepared;
        return true;
      },
      [&](auto, const auto& users, bool success) {
        REQUIRE(users == CampaignVisibilityBarrier::UserIds{ 0, 1 });
        synchronized = success;
        ++finished;
        return true;
      },
      [&](auto user) { closed.push_back(user); });
    f.server.worldState.SetCampaignVisibilityCallbacks(
      [&](auto& actor) { barrier.BeforeCommit(actor); },
      [&](auto& actor, bool success) { barrier.AfterCommit(actor, success); });
    REQUIRE_NOTHROW(QuestJournal::Atomic(guest, [&] {
      REQUIRE(QuestJournal::Apply(
        guest,
        { { "quest", kQuest }, { "op", "setStage" }, { "index", 10 } }));
      QuestJournal::EditInventory(
        guest, owner, [](auto& inventory) { inventory.AddItem(0xf, 7); });
      QuestJournal::SetReferenceDisabled(guest, copy, false);
    }));
    REQUIRE(scripts->reads >= 1);
    REQUIRE(prepared == 1);
    REQUIRE(finished == 1);
    REQUIRE_FALSE(synchronized);
    REQUIRE_FALSE(barrier.IsActive());
    REQUIRE(closed == CampaignVisibilityBarrier::UserIds{ 0, 1 });
    for (auto user : closed) {
      REQUIRE(f.server.serverState.userInfo[user]->gameplayBlocked);
      REQUIRE(f.server.serverState.userInfo[user]->campaignVisibilityFailed);
    }
    // The outer enable/stage/reward published; nested OnInit and OnLoad did
    // not persist their five gold, initialization marker, variables or
    // Disable.
    REQUIRE_FALSE(copy.IsDisabled());
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 7);
    REQUIRE(guest.GetInventory().GetItemCount(0xf) == 0);
    REQUIRE(QuestJournal::Get(owner, kQuest).at("stage") == 10);
    REQUIRE_FALSE(
      QuestJournal::ReferenceInitialized(owner, kScriptedReference));
    REQUIRE(QuestJournal::ScriptState(owner).empty());
    const auto committed = QuestJournal::Snapshot(owner);
    REQUIRE_FALSE(
      f.Run(guest)); // A committed stage is not replayed as rollback.
    REQUIRE(QuestJournal::Snapshot(owner) == committed);
    REQUIRE(owner.GetInventory().GetItemCount(0xf) == 7);
    REQUIRE(prepared == 1);
    f.server.worldState.Tick();
    REQUIRE(storage->batches.size() == 1);
    REQUIRE_FALSE(storage->batches[0].at(copy.GetIdx())->isDisabled);
    REQUIRE(storage->batches[0].at(owner.GetIdx())->inv.GetItemCount(0xf) ==
            7);
    f.server.worldState.SetCampaignVisibilityCallbacks({}, {});
  }
}

namespace {
constexpr uint32_t kRepeatQuest = 0x2aa57;
constexpr auto kRepeatScript = "2aa57:Skyrim.esm/boyhousefixture";
void EnableRuntimeQuest(MpActor& actor, uint32_t questId)
{
  const auto key =
    FormDesc::FromFormId(questId, actor.GetParent()->espmFiles).ToString();
  actor.SetPropertyValueDump(QuestJournal::kEnabled,
                            nlohmann::json::array({ key }).dump(), false,
                            false);
}
int RepeatCounter(MpActor& actor)
{
  return QuestJournal::ScriptState(actor)
    .at("scripts")
    .at(kRepeatScript)
    .at("values")
    .at("boyhousefixture/counter")
    .at("value")
    .get<int>();
}
}

TEST_CASE("Owned repeatable stages rerun in the host campaign without "
          "duplicating stage history",
          "[Quest][QuestVM][RepeatableStage]")
{
  Fixture f;
  auto& owner = f.Add(0xff008001);
  auto& guest = f.Add(0xff008002);
  EnableRuntimeQuest(owner, kRepeatQuest);
  const auto record = GetEspmLoader().GetBrowser().LookupById(kRepeatQuest);
  REQUIRE(espm::QuestLifecycleData::Read(record.rec,
                                        f.server.worldState.GetEspmCache())
            .flags & espm::QuestLifecycleData::AllowRepeatedStages);
  const auto phase =
    "campaign:" + std::to_string(owner.GetFormId()) + ":repeated";
  owner.SetWorldInstance(phase);
  guest.SetWorldInstance(phase);
  REQUIRE(f.Run(guest, 10, kRepeatQuest));
  REQUIRE(f.Run(guest, 20, kRepeatQuest));
  const auto revision = QuestJournal::Snapshot(owner).at("revision");
  REQUIRE(f.Run(guest, 10, kRepeatQuest));
  REQUIRE(RepeatCounter(owner) == 3);
  REQUIRE(QuestJournal::Snapshot(owner).at("revision") > revision);
  const auto progress = QuestJournal::Get(owner, kRepeatQuest);
  REQUIRE(progress.at("stages") == nlohmann::json::array({ 10, 20 }));
  REQUIRE(progress.at("stage") == 20);
  REQUIRE(progress.at("objectives").at("10").at("displayed") == true);
  guest.SetWorldInstance("");
  REQUIRE(QuestJournal::Get(guest, kRepeatQuest).at("stages").empty());
  REQUIRE(QuestJournal::ScriptState(guest).empty());
}

TEST_CASE("Repeated stages re-evaluate owned log conditions against current "
          "campaign variables",
          "[Quest][QuestVM][RepeatableStage][QuestVariableConditions]")
{
  Fixture f;
  constexpr uint32_t questId = 0x15d24;
  constexpr auto stateKey = "15d24:Skyrim.esm/boyhousefixture";
  auto& actor = f.Add(0xff008001);
  EnableRuntimeQuest(actor, questId);
  f.fixture->userFlagTable.push_back({ "Conditional", 4 });
  auto& object = f.fixture->objectTable[0];
  object.userFlags = 16;
  object.variables.push_back(
    { "::pTGRDUPlanted_var", "Bool", 16, VarValue(false) });
  auto first = f.Fragment(), second = first;
  first.code.instructions = {
    { FunctionCode::kOp_IAdd,
      { Id("counter"), Id("counter"), VarValue(1) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  second.code.instructions = first.code.instructions;
  second.code.instructions[0].args[2] = VarValue(10);
  object.states[0].functions = { { "First", first }, { "Second", second } };
  f.metadata.fragments = { { 20, 0, 1, "BoyhouseFixture", "First" },
                           { 20, 1, 1, "BoyhouseFixture", "Second" } };
  auto counter = [&] {
    return QuestJournal::ScriptState(actor)
      .at("scripts").at(stateKey).at("values")
      .at("boyhousefixture/counter").at("value").get<int>();
  };
  REQUIRE(f.Run(actor, 20, questId));
  REQUIRE(counter() == 1);
  // Simulate a later event changing the conditional quest variable. The next
  // visit must select the other original CTDA log, not reuse its first choice.
  auto state = QuestJournal::ScriptState(actor);
  state["scripts"][stateKey]["values"]
       ["boyhousefixture/::ptgrduplanted_var"]["value"] = true;
  QuestJournal::Atomic(actor, [&] {
    QuestJournal::SetScriptState(actor, state);
  });
  REQUIRE(f.Run(actor, 20, questId));
  REQUIRE(counter() == 11);
  REQUIRE(QuestJournal::Get(actor, questId).at("stages") ==
          nlohmann::json::array({ 20 }));
  const auto before = actor.GetDynamicFields().GetValueDump(
    QuestJournal::kProperty);
  object.states[0].functions[1].function.code.instructions.insert(
    object.states[0].functions[1].function.code.instructions.end() - 1,
    { FunctionCode::kOp_CallMethod,
      { VarValue("MissingInRepeatedBranch"), Id("self"), Id("temp"),
        VarValue(0) } });
  REQUIRE_THROWS_WITH(f.Run(actor, 20, questId),
                      Catch::Matchers::ContainsSubstring(
                        "MissingInRepeatedBranch"));
  REQUIRE(actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          before);
  REQUIRE(counter() == 11);
}

TEST_CASE("Sequential nested repeat visits execute twice but active repeat "
          "cycles roll back",
          "[Quest][QuestVM][RepeatableStage]")
{
  Fixture f;
  auto& actor = f.Add(0xff008001);
  EnableRuntimeQuest(actor, kRepeatQuest);
  SECTION("sequential calls are separate visits")
  {
    auto caller = f.Fragment();
    const FunctionCode::Instruction call{
      FunctionCode::kOp_CallMethod,
      { VarValue("SetStage"), Id("self"), Id("temp"), VarValue(1),
        VarValue(20) }
    };
    caller.code.instructions = {
      call, call, { FunctionCode::kOp_Return, { VarValue::None() } }
    };
    f.fixture->objectTable[0].states[0].functions.push_back(
      { "CallTwice", caller });
    f.metadata.fragments[0].function = "CallTwice";
    REQUIRE(f.Run(actor, 10, kRepeatQuest));
    REQUIRE(RepeatCounter(actor) == 2);
    REQUIRE(f.Run(actor, 10, kRepeatQuest));
    REQUIRE(RepeatCounter(actor) == 4);
    REQUIRE(QuestJournal::Get(actor, kRepeatQuest).at("stages") ==
            nlohmann::json::array({ 10, 20 }));
  }
  SECTION("an active repeated stage needs asynchronous scheduling")
  {
    f.Fragment().code.instructions.insert(
      f.Fragment().code.instructions.end() - 1,
      { FunctionCode::kOp_CallMethod,
        { VarValue("SetStage"), Id("self"), Id("temp"), VarValue(1),
          VarValue(10) } });
    const auto before = QuestJournal::Snapshot(actor);
    REQUIRE_THROWS_WITH(
      f.Run(actor, 10, kRepeatQuest),
      Catch::Matchers::ContainsSubstring("requires scheduling"));
    REQUIRE(QuestJournal::Snapshot(actor) == before);
    REQUIRE(QuestJournal::ScriptState(actor).empty());
  }
}

TEST_CASE("Pure reward repeats advance revision and persist without changing "
          "VM variables while failed repeats roll back",
          "[Quest][QuestVM][RepeatableStage]")
{
  Fixture f;
  InventoryFragment(f);
  // Keep every script variable unchanged: the revision cannot rely on a
  // counter or on a changed serialized VM to record the repeated reward.
  f.Fragment().code.instructions.erase(
    f.Fragment().code.instructions.begin() + 3);
  auto storage = std::make_shared<RuntimeIntegrationStorage>();
  f.server.AttachSaveStorage(storage);
  auto& actor = f.Add(0xff008001);
  EnableRuntimeQuest(actor, kRepeatQuest);
  actor.SetInventory(Inventory().AddItem(0xf, 10));
  REQUIRE(f.Run(actor, 10, kRepeatQuest));
  const auto variables = QuestJournal::ScriptState(actor);
  const auto revision = QuestJournal::Snapshot(actor).at("revision");
  REQUIRE(f.Run(actor, 10, kRepeatQuest));
  REQUIRE(actor.GetInventory().GetItemCount(0xf) == 20);
  REQUIRE(QuestJournal::ScriptState(actor) == variables);
  REQUIRE(QuestJournal::Snapshot(actor).at("revision") > revision);
  f.server.worldState.Tick();
  REQUIRE(storage->batches.size() == 1);
  const auto& saved = storage->batches.back().at(actor.GetIdx());
  REQUIRE(saved.has_value());
  REQUIRE(saved->inv.GetItemCount(0xf) == 20);
  const auto before = actor.GetDynamicFields().GetValueDump(
    QuestJournal::kProperty);
  f.Fragment().code.instructions.insert(
    f.Fragment().code.instructions.end() - 1,
    { FunctionCode::kOp_CallMethod,
      { VarValue("MissingAfterRepeatedReward"), Id("self"), Id("temp"),
        VarValue(0) } });
  REQUIRE_THROWS_WITH(
    f.Run(actor, 10, kRepeatQuest),
    Catch::Matchers::ContainsSubstring("MissingAfterRepeatedReward"));
  REQUIRE(actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          before);
  REQUIRE(actor.GetInventory().GetItemCount(0xf) == 20);
  f.server.worldState.Tick();
  REQUIRE(storage->batches.size() == 1);

  Fixture restarted;
  InventoryFragment(restarted);
  restarted.Fragment().code.instructions.erase(
    restarted.Fragment().code.instructions.begin() + 3);
  auto& restored = restarted.Add(actor.GetFormId());
  restored.ApplyChangeForm(*saved);
  REQUIRE(restarted.Run(restored, 10, kRepeatQuest));
  REQUIRE(restored.GetInventory().GetItemCount(0xf) == 25);
  REQUIRE(QuestJournal::ScriptState(restored) == variables);
  REQUIRE(QuestJournal::Get(restored, kRepeatQuest).at("stages") ==
          nlohmann::json::array({ 10 }));
}

TEST_CASE("Repeated stage execution cannot silently restart a stopped quest",
          "[Quest][QuestVM][RepeatableStage]")
{
  Fixture f;
  auto& actor = f.Add(0xff008001);
  EnableRuntimeQuest(actor, kRepeatQuest);
  REQUIRE(f.Run(actor, 10, kRepeatQuest));
  QuestJournal::Apply(actor, { { "quest", kRepeatQuest }, { "op", "stop" } });
  const auto before = actor.GetDynamicFields().GetValueDump(
    QuestJournal::kProperty);
  REQUIRE_THROWS(f.Run(actor, 10, kRepeatQuest));
  REQUIRE(actor.GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          before);
  REQUIRE(RepeatCounter(actor) == 1);
}

TEST_CASE("A stage without log items records visitation but returns false",
          "[Quest][QuestVM][QuestLifecycle]")
{
  Fixture f;
  constexpr uint32_t questId = 0x1075b1;
  auto& actor = f.Add(0xff008001);
  EnableRuntimeQuest(actor, questId);
  f.metadata.fragments.clear();
  const auto before = QuestJournal::Snapshot(actor).at("revision");
  REQUIRE_FALSE(f.Run(actor, 0, questId));
  REQUIRE(QuestJournal::Get(actor, questId).at("stages") ==
          nlohmann::json::array({ 0 }));
  REQUIRE(QuestJournal::Snapshot(actor).at("revision") > before);
  REQUIRE(QuestJournal::ScriptState(actor).empty());
  const auto saved = QuestJournal::Snapshot(actor);
  REQUIRE_FALSE(f.Run(actor, 0, questId));
  REQUIRE(QuestJournal::Snapshot(actor) == saved);
}

TEST_CASE("Duplicate authored stage indices cannot merge unrelated log lists",
          "[Quest][QuestVM][QuestLifecycle]")
{
  Fixture f;
  constexpr uint32_t questId = 0x786b9;
  auto& actor = f.Add(0xff008001);
  EnableRuntimeQuest(actor, questId);
  f.metadata.fragments = { { 0, 0, 1, "BoyhouseFixture", "Fragment" } };
  const auto before = QuestJournal::Snapshot(actor);
  REQUIRE_THROWS_WITH(
    f.Run(actor, 0, questId),
    Catch::Matchers::ContainsSubstring("Ambiguous duplicate quest stage"));
  REQUIRE(QuestJournal::Snapshot(actor) == before);
  REQUIRE(QuestJournal::ScriptState(actor).empty());
}

TEST_CASE("Completion log flags apply before fragment queries and roll back "
          "with a failing fragment",
          "[Quest][QuestVM][QuestLifecycle]")
{
  Fixture f;
  constexpr uint32_t questId = 0x10d9f4;
  const bool fail = GENERATE(false, true);
  auto& actor = f.Add(0xff008001);
  EnableRuntimeQuest(actor, questId);
  QuestJournal::Apply(
    actor, { { "quest", questId }, { "op", "setActive" }, { "value", true } });
  f.metadata.fragments = { { 100, 0, 1, "BoyhouseFixture", "Fragment" } };
  for (const auto* name : { "completedAtEntry", "activeAtEntry",
                            "runningAtEntry" })
    f.fixture->objectTable[0].variables.push_back(
      { name, "Bool", 0, VarValue(false) });
  f.Fragment().code.instructions = {
    { FunctionCode::kOp_CallMethod,
      { VarValue("IsCompleted"), Id("self"), Id("completedAtEntry"),
        VarValue(0) } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("IsActive"), Id("self"), Id("activeAtEntry"), VarValue(0) } },
    { FunctionCode::kOp_CallMethod,
      { VarValue("IsRunning"), Id("self"), Id("runningAtEntry"), VarValue(0) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  if (fail)
    f.Fragment().code.instructions.insert(
      f.Fragment().code.instructions.end() - 1,
      { FunctionCode::kOp_CallMethod,
        { VarValue("MissingAfterCompletion"), Id("self"), Id("temp"),
          VarValue(0) } });
  const auto before = QuestJournal::Snapshot(actor);
  if (fail) {
    REQUIRE_THROWS(f.Run(actor, 100, questId));
    REQUIRE(QuestJournal::Snapshot(actor) == before);
    REQUIRE(QuestJournal::ScriptState(actor).empty());
  } else {
    REQUIRE(f.Run(actor, 100, questId));
    const auto variables = QuestJournal::ScriptState(actor)
                             .at("scripts")
                             .at("10d9f4:Skyrim.esm/boyhousefixture")
                             .at("values");
    for (const auto* name : { "completedatentry", "activeatentry",
                              "runningatentry" })
      REQUIRE(variables.at(std::string("boyhousefixture/") + name)
                .at("value") == true);
  }
}
