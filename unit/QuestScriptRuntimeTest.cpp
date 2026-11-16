#include "QuestScriptRuntime.h"
#include "QuestJournal.h"
#include "QuestVmState.h"
#include "TestUtils.hpp"
#include "papyrus-vm/Reader.h"
#include "script_objects/CampaignAliasObject.h"
#include "script_objects/CampaignFormObject.h"
#include "script_objects/CampaignQuestObject.h"
#include "script_storages/BsaArchiveScriptStorage.h"
#include <fstream>
#include <limits>

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
    std::vector<PexScript::Lazy> scripts = {
      { fixture->source, [pex = fixture] { return pex; } },
      { quest->source, [pex = quest] { return pex; } }
    };
    for (const auto& script : extraScripts)
      scripts.push_back({ script->source, [script] { return script; } });
    return QuestScriptRuntime::ExecuteStage(actor, questId, stage, metadata,
                                            scripts);
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
  REQUIRE_THROWS_WITH(QuestJournal::Atomic(
                        actor,
                        [&] {
                          QuestJournal::Apply(
                            actor, { { "quest", kQuest }, { "op", "start" } });
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
  REQUIRE(QuestJournal::ScriptState(restored)["schemaVersion"] == 4);
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
