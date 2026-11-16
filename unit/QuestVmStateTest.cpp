#include "QuestVmState.h"
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
  REQUIRE(migrated.Save()["schemaVersion"] == 4);
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
  REQUIRE(migrated.Save()["schemaVersion"] == 4);
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
  REQUIRE(saved["schemaVersion"] == 4);
  REQUIRE(saved["globals"]["17424:Skyrim.esm"] == 3);
  REQUIRE(saved["aliases"].empty());
  auto bad = saved;
  bad["aliases"]["quest/alias:1"] = Json::array();
  REQUIRE_THROWS(Heap(bad));
}
