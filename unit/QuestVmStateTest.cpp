#include "QuestVmState.h"
#include "papyrus-vm/IGameObject.h"
#include <catch2/catch_all.hpp>
#include <limits>

namespace {
using Json = nlohmann::json;
QuestVmState Heap(Json saved = Json::object())
{
  return QuestVmState(
    std::move(saved),
    [](IGameObject*) -> Json {
      throw std::runtime_error("Unexpected object");
    },
    [](const Json&) -> VarValue {
      throw std::runtime_error("Unexpected object");
    });
}
VarValue Integers(int32_t first)
{
  VarValue array(static_cast<uint8_t>(VarValue::kType_IntArray));
  array.pArray = std::make_shared<std::vector<VarValue>>();
  array.pArray->push_back(VarValue(first));
  return array;
}
Json Script(Json values)
{
  return { { "schemaVersion", 1 }, { "values", std::move(values) } };
}
class ContinuationObject : public IGameObject
{
protected:
  const std::vector<std::shared_ptr<ActivePexInstance>>&
  ListActivePexInstances() const override
  {
    return scripts;
  }
  void AddScript(std::shared_ptr<ActivePexInstance> script) noexcept override
  {
    scripts.push_back(std::move(script));
  }

private:
  std::vector<std::shared_ptr<ActivePexInstance>> scripts;
};
QuestVmState ContinuationHeap(std::shared_ptr<IGameObject> object,
                              Json saved = Json::object())
{
  return QuestVmState(
    std::move(saved),
    [object](IGameObject* value) -> Json {
      if (value != object.get())
        throw std::runtime_error("Unexpected continuation object");
      return "a:Skyrim.esm";
    },
    [object](const Json& value) -> VarValue {
      if (value != "a:Skyrim.esm")
        throw std::runtime_error("Unknown continuation object");
      return VarValue(object);
    });
}
PapyrusContinuation Continuation(std::shared_ptr<IGameObject> object)
{
  PapyrusContinuation result;
  result.scope = "a:Skyrim.esm";
  result.receiver = VarValue(std::move(object));
  result.rootScript = "ChildQuest";
  result.script = "ParentQuest";
  result.function = "Fragment_0";
  result.contentHashes = { { "ChildQuest", std::string(64, 'a') },
                           { "ParentQuest", std::string(64, 'B') } };
  result.token = "quest-await-1";
  result.resultType = "Bool";
  result.instruction = 12;
  result.executedInstructions = 37;
  VarValue count(42);
  count.objectType = "Int";
  result.locals.emplace_back("count", count);
  return result;
}
PapyrusFrame Caller(std::shared_ptr<IGameObject> object,
                    const std::string& kind = "method")
{
  PapyrusFrame result = Continuation(std::move(object));
  result.kind = kind;
  result.function = kind + "Caller";
  if (kind == "static") {
    result.receiver = VarValue::None();
    result.rootScript = result.script = "StaticHelper";
  }
  return result;
}
}

TEST_CASE("Campaign heap retains shared arrays across scripts and restarts",
          "[Quest][QuestVM][QuestHeap]")
{
  auto heap = Heap();
  auto array = Integers(7);
  heap.SetScript("quest/a", Script({ { "a", heap.Encode(array) } }));
  heap.SetScript("quest/b", Script({ { "b", heap.Encode(array) } }));
  auto saved = heap.Save();
  REQUIRE(saved["arrays"].size() == 1);

  auto restart = Heap(saved);
  auto a = restart.Decode(restart.GetScript("quest/a")["values"]["a"]);
  auto b = restart.Decode(restart.GetScript("quest/b")["values"]["b"]);
  REQUIRE(a.pArray == b.pArray);
  REQUIRE(a == b);
  (*a.pArray)[0] = VarValue(8);
  REQUIRE(static_cast<int>((*b.pArray)[0]) == 8);
  auto savedAgain = restart.Save();
  auto next = Heap(savedAgain);
  auto c = next.Decode(next.GetScript("quest/b")["values"]["b"]);
  REQUIRE(static_cast<int>((*c.pArray)[0]) == 8);
  // Loading a different campaign creates an independent heap.
  auto otherCampaign = Heap(savedAgain);
  auto d =
    otherCampaign.Decode(otherCampaign.GetScript("quest/b")["values"]["b"]);
  REQUIRE(d.pArray != c.pArray);
}

TEST_CASE("Unloaded quest scripts retain shared array mutations",
          "[Quest][QuestVM][QuestHeap]")
{
  auto heap = Heap();
  auto array = Integers(1);
  heap.SetScript("quest/a", Script({ { "a", heap.Encode(array) } }));
  heap.SetScript("quest/b", Script({ { "b", heap.Encode(array) } }));
  auto restart = Heap(heap.Save());
  auto a = restart.Decode(restart.GetScript("quest/a")["values"]["a"]);
  (*a.pArray)[0] = VarValue(42);
  // No decode or SetScript call for quest/b in this transaction.
  auto third = Heap(restart.Save());
  auto b = third.Decode(third.GetScript("quest/b")["values"]["b"]);
  REQUIRE(static_cast<int>((*b.pArray)[0]) == 42);
}

TEST_CASE("Quest heap keeps null empty and equal allocations distinct",
          "[Quest][QuestVM][QuestHeap]")
{
  auto heap = Heap();
  auto a = Integers(7), b = Integers(7), empty = Integers(0);
  empty.pArray->clear();
  VarValue null(static_cast<uint8_t>(VarValue::kType_IntArray));
  heap.SetScript("quest/a",
                 Script({ { "a", heap.Encode(a) },
                          { "b", heap.Encode(b) },
                          { "empty", heap.Encode(empty) },
                          { "null", heap.Encode(null) } }));
  auto restart = Heap(heap.Save());
  const auto values = restart.GetScript("quest/a")["values"];
  REQUIRE(restart.Decode(values["a"]).pArray !=
          restart.Decode(values["b"]).pArray);
  REQUIRE_FALSE(restart.Decode(values["a"]) == restart.Decode(values["b"]));
  REQUIRE(restart.Decode(values["empty"]).pArray);
  REQUIRE(restart.Decode(values["empty"]).pArray->empty());
  REQUIRE_FALSE(restart.Decode(values["null"]).pArray);
}

TEST_CASE("Legacy inline quest arrays migrate without merging equal values",
          "[Quest][QuestVM][QuestHeap]")
{
  Json oldArray = { { "type", 13 },
                    { "value",
                      Json::array({ { { "type", 3 }, { "value", 7 } } }) } };
  Json old = { { "quest/a",
                 Script(
                   { { "a", oldArray },
                     { "b", oldArray },
                     { "count", { { "type", 3 }, { "value", 4 } } } }) } };
  auto migrated = Heap(old);
  const auto values = migrated.GetScript("quest/a")["values"];
  auto a = migrated.Decode(values["a"]), b = migrated.Decode(values["b"]);
  REQUIRE(a.pArray != b.pArray);
  REQUIRE(static_cast<int>((*a.pArray)[0]) == 7);
  REQUIRE(static_cast<int>(migrated.Decode(values["count"])) == 4);
  REQUIRE(migrated.Save()["schemaVersion"] == 6);
  REQUIRE(old["quest/a"]["values"]["a"].contains("value"));
}

TEST_CASE("Reset and replacement collect only unreachable quest arrays",
          "[Quest][QuestVM][QuestHeap]")
{
  auto heap = Heap();
  auto shared = Integers(7), privateArray = Integers(9);
  heap.SetScript("a:Skyrim.esm/one",
                 Script({ { "shared", heap.Encode(shared) },
                          { "private", heap.Encode(privateArray) } }));
  heap.SetScript("b:Skyrim.esm/two",
                 Script({ { "shared", heap.Encode(shared) } }));
  auto saved = heap.Save();
  QuestVmState::ResetQuest(saved, "a:Skyrim.esm/");
  REQUIRE(saved["arrays"].size() == 1);
  auto restart = Heap(saved);
  auto ref =
    restart.Decode(restart.GetScript("b:Skyrim.esm/two")["values"]["shared"]);
  REQUIRE(static_cast<int>((*ref.pArray)[0]) == 7);
  restart.SetScript(
    "b:Skyrim.esm/two",
    Script({ { "shared", { { "type", 13 }, { "array", nullptr } } } }));
  REQUIRE(restart.Save()["arrays"].empty());
  QuestVmState::ResetQuest(saved, "b:Skyrim.esm/");
  REQUIRE(saved.empty());
}

TEST_CASE("Invalid quest heap input cannot become valid-looking progress",
          "[Quest][QuestVM][QuestHeap]")
{
  auto heap = Heap();
  heap.SetScript("quest/a", Script({ { "a", heap.Encode(Integers(7)) } }));
  auto good = heap.Save();
  SECTION("unknown version")
  {
    good["schemaVersion"] = 99;
  }
  SECTION("dangling pointer")
  {
    good["arrays"] = Json::object();
  }
  SECTION("wrong array reference type")
  {
    good["scripts"]["quest/a"]["values"]["a"]["type"] = 12;
  }
  SECTION("wrong element type")
  {
    good["arrays"]["1"]["values"][0] = { { "type", 2 }, { "value", "no" } };
  }
  SECTION("integer overflow")
  {
    good["arrays"]["1"]["values"][0]["value"] = UINT64_MAX;
  }
  SECTION("oversized type tag")
  {
    good["scripts"]["quest/a"]["values"]["a"]["type"] = uint64_t(0x10000000d);
  }
  SECTION("noncanonical identity")
  {
    good["arrays"]["01"] = good["arrays"]["1"];
  }
  SECTION("no compound arrays in Skyrim Papyrus")
  {
    good["arrays"]["1"]["values"][0] = { { "type", 13 }, { "array", "1" } };
  }
  REQUIRE_THROWS(Heap(good));
}

TEST_CASE("Quest heap rejects invalid runtime values before persistence",
          "[Quest][QuestVM][QuestHeap]")
{
  auto heap = Heap();
  REQUIRE_THROWS(
    heap.Encode(VarValue(std::numeric_limits<double>::infinity())));
  auto array = Integers(1);
  heap.SetScript("quest/a", Script({ { "a", heap.Encode(array) } }));
  array.pArray->push_back(VarValue("wrong type"));
  REQUIRE_THROWS(heap.Save());
}

TEST_CASE("Papyrus array equality recognizes identities and typed None",
          "[Quest][QuestHeap][VarValue]")
{
  for (uint8_t type = 11; type <= 15; ++type) {
    VarValue null(type), sameNull(type), a(type), b(type);
    a.pArray = std::make_shared<std::vector<VarValue>>();
    b.pArray = std::make_shared<std::vector<VarValue>>();
    const auto alias = a;
    REQUIRE(a == alias);
    REQUIRE(alias == a);
    REQUIRE_FALSE(a == b);
    REQUIRE(null == sameNull);
    REQUIRE(null == VarValue::None());
    REQUIRE(VarValue::None() == null);
    REQUIRE_FALSE(a == VarValue::None());
    REQUIRE_FALSE(VarValue::None() == a);
    REQUIRE_FALSE(a == null);
  }
}

TEST_CASE("Campaign globals survive heap collection and reject corrupt saves",
          "[Quest][QuestVM][QuestHeap]")
{
  auto heap = Heap();
  heap.SetGlobal("17424:Skyrim.esm", 0.1);
  auto saved = heap.Save();
  REQUIRE(saved["globals"]["17424:Skyrim.esm"] == double(float(0.1)));
  QuestVmState::ResetQuest(saved, "17424:Skyrim.esm/");
  auto restored = Heap(saved);
  REQUIRE(restored.GetGlobal("17424:Skyrim.esm") == float(0.1));
  REQUIRE_FALSE(restored.GetGlobal("unknown"));
  SECTION("wrong container type")
  {
    saved["globals"] = Json::array();
  }
  SECTION("non-number")
  {
    saved["globals"]["17424:Skyrim.esm"] = "1";
  }
  SECTION("float32 overflow")
  {
    saved["globals"]["17424:Skyrim.esm"] = 1e100;
  }
  REQUIRE_THROWS(Heap(saved));
}

TEST_CASE("Heap schema two migrates without changing shared allocations",
          "[Quest][QuestVM][QuestHeap]")
{
  auto heap = Heap();
  auto array = Integers(7);
  heap.SetScript(
    "quest/a",
    Script({ { "a", heap.Encode(array) }, { "b", heap.Encode(array) } }));
  auto old = heap.Save();
  old["schemaVersion"] = 2;
  old.erase("globals");
  auto migrated = Heap(old);
  const auto values = migrated.GetScript("quest/a")["values"];
  REQUIRE(migrated.Decode(values["a"]).pArray ==
          migrated.Decode(values["b"]).pArray);
  REQUIRE(migrated.Save()["schemaVersion"] == 6);
  REQUIRE(migrated.Save()["globals"].empty());
}

TEST_CASE("Alias bindings distinguish unresolved from empty and reset only "
          "their quest",
          "[Quest][QuestVM][QuestHeap]")
{
  auto heap = Heap();
  REQUIRE_FALSE(heap.GetAliasBinding("a:Skyrim.esm/alias:1"));
  heap.SetAliasBinding("a:Skyrim.esm/alias:1", "14:Skyrim.esm");
  heap.SetAliasBinding("b:Skyrim.esm/alias:2", nullptr);
  auto saved = heap.Save();
  auto restarted = Heap(saved);
  REQUIRE(*restarted.GetAliasBinding("a:Skyrim.esm/alias:1") ==
          "14:Skyrim.esm");
  REQUIRE(restarted.GetAliasBinding("b:Skyrim.esm/alias:2")->is_null());
  QuestVmState::ResetQuest(saved, "a:Skyrim.esm/");
  REQUIRE(saved["aliases"].size() == 1);
  REQUIRE(saved["aliases"].contains("b:Skyrim.esm/alias:2"));
  QuestVmState::ResetQuest(saved, "b:Skyrim.esm/");
  REQUIRE(saved.empty());
  REQUIRE_THROWS(heap.SetAliasBinding("", nullptr));
  REQUIRE_THROWS(heap.SetAliasBinding("a", 17));
}

TEST_CASE("Schema three migrates globals without inventing alias bindings",
          "[Quest][QuestVM][QuestHeap]")
{
  auto heap = Heap();
  heap.SetGlobal("17424:Skyrim.esm", 3);
  auto old = heap.Save();
  old["schemaVersion"] = 3;
  old.erase("aliases");
  const auto saved = Heap(old).Save();
  REQUIRE(saved["schemaVersion"] == 6);
  REQUIRE(saved["globals"]["17424:Skyrim.esm"] == 3);
  REQUIRE(saved["aliases"].empty());
  auto bad = saved;
  bad["aliases"]["quest/alias:1"] = Json::array();
  REQUIRE_THROWS(Heap(bad));
}

TEST_CASE("Continuation locals share field arrays across reload and consumption",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto heap = ContinuationHeap(object);
  auto frame = Continuation(object);
  auto shared = Integers(7), privateArray = Integers(9);
  shared.objectType = privateArray.objectType = "Int[]";
  heap.SetScript("a:Skyrim.esm/ChildQuest",
                 Script({ { "field", heap.Encode(shared) } }));
  frame.locals.emplace_back("shared", shared);
  frame.locals.emplace_back("sameShared", shared);
  frame.locals.emplace_back("private", privateArray);
  auto nullObject = VarValue::None();
  nullObject.objectType = "ObjectReference";
  frame.locals.emplace_back("typedNone", nullObject);
  VarValue nullArray(static_cast<uint8_t>(VarValue::kType_ObjectArray));
  nullArray.objectType = "Actor[]";
  frame.locals.emplace_back("nullArray", nullArray);
  auto localObject = VarValue(object);
  localObject.objectType = "ParentQuest";
  frame.locals.emplace_back("object", localObject);
  heap.PutContinuation(frame);
  auto saved = heap.Save();
  REQUIRE(saved["schemaVersion"] == 6);
  REQUIRE(saved["arrays"].size() == 2);
  REQUIRE(saved["continuations"].size() == 1);
  REQUIRE(saved["continuations"][frame.token]["receiver"]["value"] ==
          "a:Skyrim.esm");

  auto restarted = ContinuationHeap(object, saved);
  auto field = restarted.Decode(
    restarted.GetScript("a:Skyrim.esm/ChildQuest")["values"]["field"]);
  auto resumed = restarted.TakeContinuation(frame.scope, frame.token);
  REQUIRE(resumed.scope == frame.scope);
  REQUIRE(resumed.token == frame.token);
  REQUIRE(static_cast<IGameObject*>(resumed.receiver) == object.get());
  REQUIRE(resumed.rootScript == frame.rootScript);
  REQUIRE(resumed.script == frame.script);
  REQUIRE(resumed.state.empty());
  REQUIRE(resumed.function == frame.function);
  REQUIRE(resumed.resultType == frame.resultType);
  REQUIRE(resumed.contentHashes == frame.contentHashes);
  REQUIRE(resumed.instruction == frame.instruction);
  REQUIRE(resumed.executedInstructions == frame.executedInstructions);
  REQUIRE(resumed.locals.size() == frame.locals.size());
  for (size_t i = 0; i < frame.locals.size(); ++i) {
    REQUIRE(resumed.locals[i].first == frame.locals[i].first);
    REQUIRE(resumed.locals[i].second.objectType ==
            frame.locals[i].second.objectType);
  }
  REQUIRE(static_cast<int>(resumed.locals[0].second) == 42);
  REQUIRE(resumed.locals[1].second.pArray == field.pArray);
  REQUIRE(resumed.locals[2].second.pArray == field.pArray);
  REQUIRE(resumed.locals[3].second.pArray != field.pArray);
  REQUIRE(static_cast<int>((*resumed.locals[3].second.pArray)[0]) == 9);
  REQUIRE(static_cast<IGameObject*>(resumed.locals[4].second) == nullptr);
  REQUIRE_FALSE(resumed.locals[5].second.pArray);
  REQUIRE(static_cast<IGameObject*>(resumed.locals[6].second) == object.get());
  (*resumed.locals[1].second.pArray)[0] = VarValue(11);
  auto consumed = restarted.Save();
  REQUIRE(consumed["continuations"].empty());
  REQUIRE(consumed["arrays"].size() == 1);
  REQUIRE_THROWS(restarted.TakeContinuation(frame.scope, frame.token));
  REQUIRE(restarted.Save() == consumed);
  auto third = ContinuationHeap(object, consumed);
  auto retained = third.Decode(
    third.GetScript("a:Skyrim.esm/ChildQuest")["values"]["field"]);
  REQUIRE(static_cast<int>((*retained.pArray)[0]) == 11);
  // Re-suspension after consumption can preserve a live local-only allocation.
  resumed.token = "quest-await-2";
  restarted.PutContinuation(resumed);
  REQUIRE(restarted.Save()["arrays"].size() == 2);
}

TEST_CASE("Continuation-only arrays remain rooted until their last frame ends",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto heap = ContinuationHeap(object);
  auto frame = Continuation(object);
  auto shared = Integers(7);
  shared.objectType = "Int[]";
  frame.locals.emplace_back("shared", shared);
  heap.PutContinuation(frame);
  auto secondFrame = frame;
  secondFrame.token = "quest-await-2";
  secondFrame.scope = "b:Skyrim.esm";
  heap.PutContinuation(secondFrame);
  auto restarted = ContinuationHeap(object, heap.Save());
  auto first = restarted.TakeContinuation(frame.scope, frame.token);
  (*first.locals[1].second.pArray)[0] = VarValue(12);
  auto halfway = restarted.Save();
  REQUIRE(halfway["arrays"].size() == 1);
  REQUIRE(halfway["scripts"].empty());
  auto next = ContinuationHeap(object, halfway);
  auto second = next.TakeContinuation(secondFrame.scope, secondFrame.token);
  REQUIRE(static_cast<int>((*second.locals[1].second.pArray)[0]) == 12);
  REQUIRE(next.Save()["arrays"].empty());
}

TEST_CASE("Continuation mismatch and decode failures leave pending work intact",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto heap = ContinuationHeap(object);
  auto frame = Continuation(object);
  heap.PutContinuation(frame);
  const auto saved = heap.Save();
  REQUIRE_THROWS(heap.PutContinuation(frame));
  REQUIRE_THROWS(heap.TakeContinuation("b:Skyrim.esm", frame.token));
  REQUIRE_THROWS(heap.TakeContinuation(frame.scope, "unknown-token"));
  REQUIRE_THROWS(heap.TakeContinuation("", frame.token));
  REQUIRE_THROWS(heap.TakeContinuation(frame.scope, std::string(257, 't')));
  REQUIRE(heap.Save() == saved);

  SECTION("resolver throws")
  {
    auto missing = Heap(saved);
    REQUIRE_THROWS(missing.TakeContinuation(frame.scope, frame.token));
    REQUIRE(missing.Save() == saved);
  }
  SECTION("receiver resolves to none")
  {
    QuestVmState missing(saved, {}, [](const Json&) { return VarValue::None(); });
    REQUIRE_THROWS(missing.TakeContinuation(frame.scope, frame.token));
    REQUIRE(missing.Save() == saved);
  }
  SECTION("receiver resolves to a scalar")
  {
    QuestVmState missing(saved, {}, [](const Json&) { return VarValue(7); });
    REQUIRE_THROWS(missing.TakeContinuation(frame.scope, frame.token));
    REQUIRE(missing.Save() == saved);
  }
}

TEST_CASE("Invalid continuation input does not allocate or replace saved work",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto heap = ContinuationHeap(object);
  auto frame = Continuation(object);
  auto array = Integers(7);
  array.objectType = "Int[]";
  frame.locals.emplace_back("array", array);
  heap.SetGlobal("a:Skyrim.esm", 3);
  const auto saved = heap.Save();
  SECTION("empty scope") { frame.scope.clear(); }
  SECTION("long scope") { frame.scope = std::string(301, 's'); }
  SECTION("empty token") { frame.token.clear(); }
  SECTION("long token") { frame.token = std::string(257, 't'); }
  SECTION("embedded NUL") { frame.function = std::string("bad\0name", 8); }
  SECTION("long name") { frame.rootScript = std::string(1025, 'n'); }
  SECTION("empty script") { frame.script.clear(); }
  SECTION("empty result type") { frame.resultType.clear(); }
  SECTION("long state") { frame.state = std::string(1025, 's'); }
  SECTION("null receiver") { frame.receiver = VarValue::None(); }
  SECTION("scalar receiver") { frame.receiver = VarValue(7); }
  SECTION("instruction limit") { frame.instruction = 100000; }
  SECTION("zero executed instructions") { frame.executedInstructions = 0; }
  SECTION("execution limit") { frame.executedInstructions = 100001; }
  SECTION("empty declared type") { frame.locals[1].second.objectType.clear(); }
  SECTION("long declared type")
  {
    frame.locals[1].second.objectType = std::string(1025, 't');
  }
  SECTION("duplicate local") { frame.locals.push_back(frame.locals.front()); }
  SECTION("empty local name") { frame.locals[0].first.clear(); }
  SECTION("invalid hash") { frame.contentHashes[0].second[0] = 'g'; }
  SECTION("short hash") { frame.contentHashes[0].second.pop_back(); }
  SECTION("duplicate content name")
  {
    frame.contentHashes.push_back(frame.contentHashes.front());
  }
  SECTION("too many content hashes")
  {
    frame.contentHashes.resize(129, frame.contentHashes.front());
  }
  SECTION("too many locals")
  {
    frame.locals.resize(65536, frame.locals.front());
  }
  SECTION("invalid live array element")
  {
    array.pArray->push_back(VarValue("wrong type"));
  }
  SECTION("late object encoding failure")
  {
    VarValue unknown(std::make_shared<ContinuationObject>());
    unknown.objectType = "Quest";
    frame.locals.emplace_back("unknown", unknown);
  }
  REQUIRE_THROWS(heap.PutContinuation(frame));
  REQUIRE(heap.Save() == saved);
  auto fresh = Integers(4);
  heap.SetScript("a:Skyrim.esm/ChildQuest",
                 Script({ { "array", heap.Encode(fresh) } }));
  REQUIRE(heap.Save()["arrays"].contains("1"));
}

TEST_CASE("Malformed persisted continuations cannot be consumed or reset",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto heap = ContinuationHeap(object);
  const auto frame = Continuation(object);
  heap.PutContinuation(frame);
  auto saved = heap.Save();
  auto& node = saved["continuations"][frame.token];
  SECTION("unknown VM version")
  {
    saved["schemaVersion"] = 7;
  }
  SECTION("fractional VM version")
  {
    saved["schemaVersion"] = 6.0;
  }
  SECTION("wrong container") { saved["continuations"] = Json::array(); }
  SECTION("missing container") { saved.erase("continuations"); }
  SECTION("unknown frame version")
  {
    node["schemaVersion"] = 3;
  }
  SECTION("fractional frame version")
  {
    node["schemaVersion"] = 2.0;
  }
  SECTION("missing frame version") { node.erase("schemaVersion"); }
  SECTION("unknown frame field") { node["futureJob"] = true; }
  SECTION("missing field") { node.erase("rootScript"); }
  SECTION("null receiver") { node["receiver"]["value"] = nullptr; }
  SECTION("array receiver")
  {
    node["receiver"] = { { "type", 13 }, { "array", nullptr } };
  }
  SECTION("invalid scope type") { node["scope"] = 1; }
  SECTION("negative instruction") { node["instruction"] = -1; }
  SECTION("oversized instruction") { node["instruction"] = UINT64_MAX; }
  SECTION("fractional instruction") { node["instruction"] = 1.0; }
  SECTION("zero execution count") { node["executedInstructions"] = 0; }
  SECTION("invalid digest") { node["contentHashes"][0]["hash"] = "not-a-hash"; }
  SECTION("wrong hashes container") { node["contentHashes"] = Json::object(); }
  SECTION("wrong locals container") { node["locals"] = Json::object(); }
  SECTION("unknown local field") { node["locals"][0]["future"] = true; }
  SECTION("missing local declaration") { node["locals"][0].erase("declaredType"); }
  SECTION("dangling local array")
  {
    node["locals"][0]["value"] = { { "type", 13 }, { "array", "999" } };
  }
  SECTION("integer overflow local")
  {
    node["locals"][0]["value"]["value"] = UINT64_MAX;
  }
  SECTION("too many frames")
  {
    const auto original = node;
    for (size_t i = 0; i < 256; ++i)
      saved["continuations"]["extra-" + std::to_string(i)] = original;
  }
  SECTION("empty token")
  {
    saved["continuations"][""] = node;
  }
  const auto original = saved;
  REQUIRE_THROWS(ContinuationHeap(object, saved));
  REQUIRE(saved == original);
  REQUIRE_THROWS(QuestVmState::ResetQuest(saved, "a:Skyrim.esm/"));
  REQUIRE(saved == original);
}

TEST_CASE("Pending frames block every quest reset until explicitly consumed",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto heap = ContinuationHeap(object);
  auto frame = Continuation(object);
  heap.SetScript("b:Skyrim.esm/OtherQuest", Script(Json::object()));
  heap.PutContinuation(frame);
  auto saved = heap.Save();
  const auto original = saved;
  REQUIRE_THROWS(QuestVmState::ResetQuest(saved, "a:Skyrim.esm/"));
  REQUIRE_THROWS(QuestVmState::ResetQuest(saved, "b:Skyrim.esm/"));
  REQUIRE_THROWS(QuestVmState::ResetQuest(saved, "absent:Skyrim.esm/"));
  REQUIRE(saved == original);
  auto restarted = ContinuationHeap(object, saved);
  restarted.TakeContinuation(frame.scope, frame.token);
  saved = restarted.Save();
  QuestVmState::ResetQuest(saved, "b:Skyrim.esm/");
  REQUIRE(saved.empty());
}

TEST_CASE("Heap versions two through four gain an empty continuation table",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  const auto version = GENERATE(2, 3, 4);
  auto heap = Heap();
  auto array = Integers(7);
  heap.SetScript("a:Skyrim.esm/Quest",
                 Script({ { "a", heap.Encode(array) },
                          { "b", heap.Encode(array) } }));
  heap.SetGlobal("a:Skyrim.esm", 4);
  heap.SetAliasBinding("a:Skyrim.esm/alias:1", "14:Skyrim.esm");
  auto old = heap.Save();
  old["schemaVersion"] = version;
  old.erase("continuations");
  if (version < 3)
    old.erase("globals");
  if (version < 4)
    old.erase("aliases");
  const auto original = old;
  auto migrated = Heap(old);
  const auto saved = migrated.Save();
  REQUIRE(saved["schemaVersion"] == 6);
  REQUIRE(saved["continuations"].empty());
  REQUIRE(saved["arrays"].size() == 1);
  const auto values = migrated.GetScript("a:Skyrim.esm/Quest")["values"];
  REQUIRE(migrated.Decode(values["a"]).pArray ==
          migrated.Decode(values["b"]).pArray);
  REQUIRE(old == original);
  if (version >= 3)
    REQUIRE(saved["globals"] == original["globals"]);
  if (version >= 4)
    REQUIRE(saved["aliases"] == original["aliases"]);

  auto object = std::make_shared<ContinuationObject>();
  auto pending = ContinuationHeap(object);
  pending.PutContinuation(Continuation(object));
  auto wrongVersion = pending.Save();
  wrongVersion["schemaVersion"] = version;
  const auto untouched = wrongVersion;
  REQUIRE_THROWS(ContinuationHeap(object, wrongVersion));
  REQUIRE_THROWS(QuestVmState::ResetQuest(wrongVersion, "a:Skyrim.esm/"));
  REQUIRE(wrongVersion == untouched);
}

TEST_CASE("Continuation bounds admit their exact limits",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto heap = ContinuationHeap(object);
  auto frame = Continuation(object);
  frame.scope = std::string(300, 's');
  frame.token = std::string(256, 't');
  frame.rootScript = frame.script = frame.function = std::string(1024, 'n');
  frame.state = std::string(1024, 's');
  frame.resultType = "None";
  frame.instruction = 99999;
  frame.executedInstructions = 100000;
  frame.contentHashes.clear();
  for (size_t i = 0; i < 128; ++i)
    frame.contentHashes.emplace_back("script-" + std::to_string(i),
                                      std::string(64, 'f'));
  frame.locals.clear();
  VarValue local(1);
  local.objectType = "Int";
  for (size_t i = 0; i < 65535; ++i)
    frame.locals.emplace_back("local-" + std::to_string(i), local);
  frame.locals[0].first = std::string(1024, 'l');
  frame.locals[0].second.objectType = std::string(1024, 't');
  heap.PutContinuation(frame);
  auto saved = heap.Save();
  auto restarted = ContinuationHeap(object, saved);
  auto consumed = restarted.TakeContinuation(frame.scope, frame.token);
  REQUIRE(consumed.locals.size() == 65535);
  REQUIRE(consumed.contentHashes.size() == 128);
  REQUIRE(consumed.instruction == 99999);
  REQUIRE(consumed.executedInstructions == 100000);
  REQUIRE(consumed.locals[0].second.objectType == std::string(1024, 't'));
}

TEST_CASE("Continuation capacity is freed by consumption without replacing work",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto heap = ContinuationHeap(object);
  const auto frame = Continuation(object);
  heap.PutContinuation(frame);
  auto saved = heap.Save();
  const auto node = saved["continuations"][frame.token];
  for (size_t i = 0; i < 255; ++i)
    saved["continuations"]["pending-" + std::to_string(i)] = node;
  auto full = ContinuationHeap(object, saved);
  REQUIRE(full.Save()["continuations"].size() == 256);
  auto overflow = frame;
  overflow.token = "overflow";
  REQUIRE_THROWS(full.PutContinuation(overflow));
  REQUIRE(full.Save() == saved);
  full.TakeContinuation(frame.scope, frame.token);
  full.PutContinuation(overflow);
  REQUIRE(full.Save()["continuations"].size() == 256);
  REQUIRE_FALSE(full.Save()["continuations"].contains(frame.token));
}

TEST_CASE("Late continuation resolution failures preserve prior heap identities",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto heap = ContinuationHeap(object);
  auto frame = Continuation(object);
  auto array = Integers(8);
  array.objectType = "Int[]";
  frame.locals.emplace_back("array", array);
  VarValue localObject(object);
  localObject.objectType = "Quest";
  frame.locals.emplace_back("object", localObject);
  heap.PutContinuation(frame);
  auto saved = heap.Save();
  saved["continuations"][frame.token]["locals"][2]["value"]["value"] =
    "b:Skyrim.esm";
  bool missing = true;
  QuestVmState restarted(
    saved, [](IGameObject*) -> Json { return "a:Skyrim.esm"; },
    [&missing, object](const Json& value) -> VarValue {
      if (value == "b:Skyrim.esm" && missing)
        throw std::runtime_error("Local object unavailable");
      return VarValue(object);
    });
  REQUIRE_THROWS(restarted.TakeContinuation(frame.scope, frame.token));
  REQUIRE(restarted.Save() == saved);
  missing = false;
  auto resumed = restarted.TakeContinuation(frame.scope, frame.token);
  REQUIRE(static_cast<int>((*resumed.locals[1].second.pArray)[0]) == 8);
  REQUIRE(static_cast<IGameObject*>(resumed.locals[2].second) == object.get());
  REQUIRE(restarted.Save()["continuations"].empty());
}

TEST_CASE("Nested continuation frames share heap roots and retain their order",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto heap = ContinuationHeap(object);
  auto frame = Continuation(object);
  auto shared = Integers(7), callerOnly = Integers(9);
  auto outerOnly = Integers(13);
  shared.objectType = callerOnly.objectType = outerOnly.objectType = "Int[]";
  heap.SetScript("a:Skyrim.esm/ChildQuest",
                 Script({ { "field", heap.Encode(shared) } }));
  frame.locals.emplace_back("shared", shared);
  frame.callers = { Caller(object, "propertyGet"), Caller(object, "static"),
                    Caller(object, "propertySet"), Caller(object) };
  frame.callers[0].resultType = "Int";
  frame.callers[1].resultType = "String";
  frame.callers[2].resultType = "None";
  frame.callers[3].resultType = "Quest[]";
  frame.callers[0].locals.emplace_back("shared", shared);
  frame.callers[0].locals.emplace_back("private", callerOnly);
  frame.callers[1].locals.emplace_back("private", callerOnly);
  frame.callers[2].locals.emplace_back("outer", outerOnly);
  VarValue nullObject = VarValue::None();
  nullObject.objectType = "ObjectReference";
  frame.callers[3].locals.emplace_back("null", nullObject);
  heap.PutContinuation(frame);
  auto saved = heap.Save();
  REQUIRE(saved["schemaVersion"] == 6);
  REQUIRE(saved["continuations"][frame.token]["schemaVersion"] == 2);
  REQUIRE(saved["arrays"].size() == 3);
  REQUIRE(saved["continuations"][frame.token]["callers"][0].size() == 10);
  auto restarted = ContinuationHeap(object, saved);
  auto field = restarted.Decode(
    restarted.GetScript("a:Skyrim.esm/ChildQuest")["values"]["field"]);
  auto resumed = restarted.TakeContinuation(frame.scope, frame.token);
  REQUIRE(resumed.callers.size() == 4);
  for (size_t i = 0; i < frame.callers.size(); ++i) {
    const auto& before = frame.callers[i];
    const auto& after = resumed.callers[i];
    REQUIRE(after.kind == before.kind);
    REQUIRE(after.rootScript == before.rootScript);
    REQUIRE(after.script == before.script);
    REQUIRE(after.state == before.state);
    REQUIRE(after.function == before.function);
    REQUIRE(after.resultType == before.resultType);
    REQUIRE(after.instruction == before.instruction);
    REQUIRE(after.contentHashes == before.contentHashes);
    REQUIRE(static_cast<IGameObject*>(after.receiver) ==
            static_cast<IGameObject*>(before.receiver));
    REQUIRE(after.locals.size() == before.locals.size());
    for (size_t j = 0; j < before.locals.size(); ++j) {
      REQUIRE(after.locals[j].first == before.locals[j].first);
      REQUIRE(after.locals[j].second.objectType ==
              before.locals[j].second.objectType);
    }
  }
  REQUIRE(resumed.locals[1].second.pArray == field.pArray);
  REQUIRE(resumed.callers[0].locals[1].second.pArray == field.pArray);
  REQUIRE(resumed.callers[0].locals[2].second.pArray ==
          resumed.callers[1].locals[1].second.pArray);
  REQUIRE(static_cast<int>((*resumed.callers[2].locals[1].second.pArray)[0]) ==
          13);
  (*resumed.callers[0].locals[1].second.pArray)[0] = VarValue(17);
  auto consumed = restarted.Save();
  REQUIRE(consumed["arrays"].size() == 1);
  REQUIRE(consumed["continuations"].empty());
  REQUIRE(static_cast<int>((*field.pArray)[0]) == 17);
  resumed.token = "nested-await-2";
  restarted.PutContinuation(resumed);
  REQUIRE(restarted.Save()["arrays"].size() == 3);
}

TEST_CASE("Static continuation leaves need no object resolver",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto frame = Continuation(object);
  static_cast<PapyrusFrame&>(frame) = Caller(object, "static");
  auto heap = Heap();
  heap.PutContinuation(frame);
  auto restarted = Heap(heap.Save());
  const auto resumed = restarted.TakeContinuation(frame.scope, frame.token);
  REQUIRE(resumed.kind == "static");
  REQUIRE(static_cast<IGameObject*>(resumed.receiver) == nullptr);
  REQUIRE(resumed.state.empty());
  REQUIRE(resumed.rootScript == resumed.script);
  REQUIRE(static_cast<int>(resumed.locals[0].second) == 42);
}

TEST_CASE("Malformed caller metadata never replaces pending work",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto heap = ContinuationHeap(object);
  auto frame = Continuation(object);
  frame.callers.push_back(Caller(object));
  heap.PutContinuation(frame);
  const auto original = heap.Save();
  frame.token = "invalid-nested-await";
  auto array = Integers(7);
  array.objectType = "Int[]";
  frame.locals.emplace_back("array", array);
  auto& caller = frame.callers[0];
  SECTION("unknown kind")
  {
    caller.kind = "native";
  }
  SECTION("empty kind")
  {
    caller.kind.clear();
  }
  SECTION("method receiver missing")
  {
    caller.receiver = VarValue::None();
  }
  SECTION("property receiver missing")
  {
    caller.kind = "propertyGet";
    caller.receiver = VarValue::None();
  }
  SECTION("static receiver present")
  {
    caller.kind = "static";
  }
  SECTION("static with active state")
  {
    caller = Caller(object, "static");
    caller.state = "Busy";
  }
  SECTION("static with differing root")
  {
    caller = Caller(object, "static");
    caller.rootScript = "Other";
  }
  SECTION("invalid result declaration")
  {
    caller.resultType.clear();
  }
  SECTION("invalid local declaration")
  {
    caller.locals[0].second.objectType.clear();
  }
  SECTION("caller object encoding fails after leaf array")
  {
    VarValue unknown(std::make_shared<ContinuationObject>());
    unknown.objectType = "Quest";
    caller.locals.emplace_back("unknown", unknown);
  }
  SECTION("too many hashes")
  {
    caller.contentHashes.resize(129, caller.contentHashes[0]);
  }
  SECTION("too many callers")
  {
    frame.callers.resize(128, caller);
  }
  REQUIRE_THROWS(heap.PutContinuation(frame));
  REQUIRE(heap.Save() == original);
  heap.SetScript("a:Skyrim.esm/fresh",
                 Script({ { "array", heap.Encode(array) } }));
  REQUIRE(heap.Save()["arrays"].contains("1"));
}

TEST_CASE("Persisted caller stacks enforce frame shape and shared "
          "local bounds",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto heap = ContinuationHeap(object);
  auto frame = Continuation(object);
  frame.callers.push_back(Caller(object));
  heap.PutContinuation(frame);
  auto saved = heap.Save();
  auto& node = saved["continuations"][frame.token];
  auto& caller = node["callers"][0];
  SECTION("missing callers")
  {
    node.erase("callers");
  }
  SECTION("wrong callers container")
  {
    node["callers"] = Json::object();
  }
  SECTION("too many callers")
  {
    node["callers"] = std::vector<Json>(128, caller);
  }
  SECTION("unknown kind")
  {
    caller["kind"] = "unknown";
  }
  SECTION("wrong kind type")
  {
    caller["kind"] = 1;
  }
  SECTION("missing kind")
  {
    caller.erase("kind");
  }
  SECTION("caller operation token")
  {
    caller["token"] = "second-token";
  }
  SECTION("caller private budget")
  {
    caller["executedInstructions"] = 1;
  }
  SECTION("caller nested list")
  {
    caller["callers"] = Json::array();
  }
  SECTION("caller schema override")
  {
    caller["schemaVersion"] = 1;
  }
  SECTION("caller null receiver")
  {
    caller["receiver"]["value"] = nullptr;
  }
  SECTION("caller scalar receiver")
  {
    caller["receiver"] = { { "type", 3 }, { "value", 1 } };
  }
  SECTION("caller invalid instruction")
  {
    caller["instruction"] = 100000;
  }
  SECTION("caller missing type")
  {
    caller["locals"][0].erase("declaredType");
  }
  SECTION("caller dangling array")
  {
    caller["locals"][0]["value"] = { { "type", 13 }, { "array", "999" } };
  }
  SECTION("static receiver not empty")
  {
    caller["kind"] = "static";
    caller["rootScript"] = caller["script"];
  }
  SECTION("static state not empty")
  {
    caller["kind"] = "static";
    caller["receiver"]["value"] = nullptr;
    caller["rootScript"] = caller["script"];
    caller["state"] = "Busy";
  }
  SECTION("static root mismatch")
  {
    caller["kind"] = "static";
    caller["receiver"]["value"] = nullptr;
  }
  SECTION("combined locals exceed limit")
  {
    auto local = caller["locals"][0];
    for (size_t i = 1; i < 65535; ++i) {
      local["name"] = "local-" + std::to_string(i);
      caller["locals"].push_back(local);
    }
  }
  const auto original = saved;
  REQUIRE_THROWS(ContinuationHeap(object, saved));
  REQUIRE_THROWS(QuestVmState::ResetQuest(saved, "a:Skyrim.esm/"));
  REQUIRE(saved == original);
}

TEST_CASE("Caller count and total local limits include the leaf frame",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto frame = Continuation(object);
  frame.callers.resize(127, Caller(object));
  auto& locals = frame.callers[0].locals;
  const auto local = locals[0].second;
  for (size_t i = 1; i < 65408; ++i)
    locals.emplace_back("local-" + std::to_string(i), local);
  // 65408 in the nearest caller, 126 in others, one in the leaf = 65535.
  auto heap = ContinuationHeap(object);
  heap.PutContinuation(frame);
  const auto saved = heap.Save();
  auto restarted = ContinuationHeap(object, saved);
  const auto restored = restarted.TakeContinuation(frame.scope, frame.token);
  REQUIRE(restored.callers.size() == 127);
  REQUIRE(restored.callers[0].locals.size() == 65408);
  frame.token = "overflow-locals";
  frame.callers.back().locals.emplace_back("extra", local);
  REQUIRE_THROWS(heap.PutContinuation(frame));
  REQUIRE(heap.Save() == saved);
}

TEST_CASE("Schema five leaf checkpoints migrate without losing array roots",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto heap = ContinuationHeap(object);
  auto frame = Continuation(object);
  auto array = Integers(8);
  array.objectType = "Int[]";
  frame.locals.emplace_back("array", array);
  heap.SetScript("a:Skyrim.esm/ChildQuest",
                 Script({ { "field", heap.Encode(array) } }));
  heap.PutContinuation(frame);
  auto old = heap.Save();
  old["schemaVersion"] = 5;
  auto& node = old["continuations"][frame.token];
  node["schemaVersion"] = 1;
  node.erase("kind");
  node.erase("callers");
  const auto original = old;
  auto migrated = ContinuationHeap(object, old);
  const auto saved = migrated.Save();
  REQUIRE(old == original);
  REQUIRE(saved["schemaVersion"] == 6);
  REQUIRE(saved["continuations"][frame.token]["schemaVersion"] == 2);
  REQUIRE(saved["continuations"][frame.token]["kind"] == "method");
  REQUIRE(saved["continuations"][frame.token]["callers"].empty());
  auto field = migrated.Decode(
    migrated.GetScript("a:Skyrim.esm/ChildQuest")["values"]["field"]);
  const auto resumed = migrated.TakeContinuation(frame.scope, frame.token);
  REQUIRE(resumed.locals[1].second.pArray == field.pArray);
  REQUIRE(resumed.locals[1].second.objectType == "Int[]");
  REQUIRE(resumed.kind == "method");
  REQUIRE(resumed.callers.empty());
}

TEST_CASE("Schema five migration refuses unrecognized or malformed "
          "pending work",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto heap = ContinuationHeap(object);
  auto frame = Continuation(object);
  heap.PutContinuation(frame);
  auto old = heap.Save();
  old["schemaVersion"] = 5;
  auto& node = old["continuations"][frame.token];
  node["schemaVersion"] = 1;
  node.erase("kind");
  node.erase("callers");
  SECTION("invented empty callers")
  {
    node["callers"] = Json::array();
  }
  SECTION("invented caller work")
  {
    node["callers"] = Json::array({ node });
  }
  SECTION("invented kind")
  {
    node["kind"] = "static";
  }
  SECTION("unknown node version")
  {
    node["schemaVersion"] = 2;
  }
  SECTION("fractional node version")
  {
    node["schemaVersion"] = 1.0;
  }
  SECTION("missing type")
  {
    node["locals"][0].erase("declaredType");
  }
  SECTION("null receiver")
  {
    node["receiver"]["value"] = nullptr;
  }
  SECTION("unknown field")
  {
    node["futureWork"] = true;
  }
  SECTION("missing locals")
  {
    node.erase("locals");
  }
  const auto original = old;
  REQUIRE_THROWS(ContinuationHeap(object, old));
  REQUIRE_THROWS(QuestVmState::ResetQuest(old, "a:Skyrim.esm/"));
  REQUIRE(old == original);
}

TEST_CASE("Caller decoding failures preserve tokens after the leaf "
          "was decoded",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto heap = ContinuationHeap(object);
  auto frame = Continuation(object);
  auto shared = Integers(8);
  shared.objectType = "Int[]";
  frame.locals.emplace_back("array", shared);
  frame.callers.push_back(Caller(object));
  frame.callers[0].locals.emplace_back("array", shared);
  VarValue reference(object);
  reference.objectType = "Quest";
  frame.callers[0].locals.emplace_back("reference", reference);
  heap.PutContinuation(frame);
  auto saved = heap.Save();
  auto& caller = saved["continuations"][frame.token]["callers"][0];
  SECTION("caller receiver")
  {
    caller["receiver"]["value"] = "missing";
  }
  SECTION("caller local")
  {
    caller["locals"][2]["value"]["value"] = "missing";
  }
  bool missing = true;
  size_t resolutions = 0;
  QuestVmState restarted(
    saved, [](IGameObject*) -> Json { return "a:Skyrim.esm"; },
    [&missing, &resolutions, object](const Json& value) -> VarValue {
      ++resolutions;
      if (value == "missing" && missing)
        throw std::runtime_error("Caller object unavailable");
      return VarValue(object);
    });
  REQUIRE_THROWS(restarted.TakeContinuation(frame.scope, frame.token));
  REQUIRE(resolutions >= 2);
  REQUIRE(restarted.Save() == saved);
  missing = false;
  const auto resumed = restarted.TakeContinuation(frame.scope, frame.token);
  REQUIRE(resumed.locals[1].second.pArray ==
          resumed.callers[0].locals[1].second.pArray);
  REQUIRE(static_cast<IGameObject*>(resumed.callers[0].receiver) ==
          object.get());
  REQUIRE(restarted.Save()["continuations"].empty());
}

TEST_CASE("Invalid caller object array resolution preserves the token and "
          "permits a clean retry",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto heap = ContinuationHeap(object);
  auto frame = Continuation(object);
  auto shared = Integers(8);
  shared.objectType = "Int[]";
  heap.SetScript("a:Skyrim.esm/ChildQuest",
                 Script({ { "field", heap.Encode(shared) } }));
  frame.locals.emplace_back("shared", shared);
  VarValue references(static_cast<uint8_t>(VarValue::kType_ObjectArray));
  references.objectType = "Quest[]";
  references.pArray =
    std::make_shared<std::vector<VarValue>>(std::initializer_list<VarValue>{
      VarValue(object), VarValue(object), VarValue::None() });
  frame.callers = { Caller(object), Caller(object) };
  for (auto& caller : frame.callers)
    caller.locals.emplace_back("references", references);
  heap.PutContinuation(frame);
  auto saved = heap.Save();
  const auto id = saved["continuations"][frame.token]["callers"][0]["locals"]
                       [1]["value"]["array"]
                         .get<std::string>();
  saved["arrays"][id]["values"][1]["value"] = "unavailable";
  VarValue invalid = VarValue::None();
  SECTION("scalar resolver result")
  {
    invalid = VarValue(7);
  }
  SECTION("latent resolver result")
  {
    invalid = VarValue(object);
    invalid.promise = std::make_shared<Viet::Promise<VarValue>>();
  }
  SECTION("nonnull identity resolves to None")
  {
  }
  bool fail = true;
  size_t unavailableResolutions = 0;
  QuestVmState restarted(
    saved, [](IGameObject*) -> Json { return "a:Skyrim.esm"; },
    [object, &invalid, &fail, &unavailableResolutions](const Json& value) {
      if (value == "unavailable") {
        ++unavailableResolutions;
        if (fail)
          return invalid;
      }
      return VarValue(object);
    });
  auto field = restarted.Decode(
    restarted.GetScript("a:Skyrim.esm/ChildQuest")["values"]["field"]);
  REQUIRE_THROWS(restarted.TakeContinuation(frame.scope, frame.token));
  REQUIRE(unavailableResolutions == 1);
  REQUIRE(restarted.Save() == saved);
  fail = false;
  const auto resumed = restarted.TakeContinuation(frame.scope, frame.token);
  REQUIRE(unavailableResolutions == 2);
  REQUIRE(resumed.locals[1].second.pArray == field.pArray);
  const auto& restored = resumed.callers[0].locals[1].second;
  REQUIRE(restored.pArray == resumed.callers[1].locals[1].second.pArray);
  REQUIRE(restored.pArray->size() == 3);
  REQUIRE(static_cast<IGameObject*>((*restored.pArray)[0]) == object.get());
  REQUIRE(static_cast<IGameObject*>((*restored.pArray)[1]) == object.get());
  REQUIRE(static_cast<IGameObject*>((*restored.pArray)[2]) == nullptr);
  REQUIRE_FALSE((*restored.pArray)[1].promise);
  REQUIRE(restarted.Save()["continuations"].empty());
}

TEST_CASE("Nonnull caller object locals cannot silently resolve to None",
          "[Quest][QuestVM][QuestHeap][Continuation]")
{
  auto object = std::make_shared<ContinuationObject>();
  auto heap = ContinuationHeap(object);
  auto frame = Continuation(object);
  frame.callers.push_back(Caller(object));
  VarValue reference(object);
  reference.objectType = "Quest";
  frame.callers[0].locals.emplace_back("reference", reference);
  heap.PutContinuation(frame);
  auto saved = heap.Save();
  saved["continuations"][frame.token]["callers"][0]["locals"][1]["value"]
       ["value"] = "unavailable";
  bool fail = true;
  QuestVmState restarted(
    saved, [](IGameObject*) -> Json { return "a:Skyrim.esm"; },
    [object, &fail](const Json& value) {
      return value == "unavailable" && fail ? VarValue::None()
                                            : VarValue(object);
    });
  REQUIRE_THROWS(restarted.TakeContinuation(frame.scope, frame.token));
  REQUIRE(restarted.Save() == saved);
  fail = false;
  const auto resumed = restarted.TakeContinuation(frame.scope, frame.token);
  REQUIRE(static_cast<IGameObject*>(resumed.callers[0].locals[1].second) ==
          object.get());
  REQUIRE(restarted.Save()["continuations"].empty());
}
