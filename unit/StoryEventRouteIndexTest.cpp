#include "libespm/StoryEventRouteIndex.h"
#include "libespm/Browser.h"
#include "libespm/Combiner.h"
#include <catch2/catch_all.hpp>

namespace {
using Index = espm::StoryEventRouteIndex;
constexpr uint32_t kDialogue = 0x41494441;
using Bytes = std::string;

template <class T>
Bytes Binary(T value)
{
  return { reinterpret_cast<const char*>(&value), sizeof(T) };
}
Bytes Field(const char* tag, const Bytes& data)
{
  return Bytes(tag, 4) + Binary(static_cast<uint16_t>(data.size())) + data;
}
Bytes Number(const char* tag, uint32_t value)
{
  return Field(tag, Binary(value));
}
Bytes Record(const char* type, uint32_t id, const Bytes& fields = {},
             uint32_t flags = 0)
{
  return Bytes(type, 4) + Binary(static_cast<uint32_t>(fields.size())) +
    Binary(flags) + Binary(id) + Bytes(8, '\0') + fields;
}
Bytes NodeFields(uint32_t parent, uint32_t previous = 0, uint32_t flags = 0,
                 uint32_t conditions = 0)
{
  return Number("PNAM", parent) + Number("SNAM", previous) +
    Number("DNAM", flags) + Number("XNAM", 0) + Number("CITC", conditions);
}
Bytes QuestFields(uint32_t parent, uint32_t quest)
{
  return NodeFields(parent) + Number("QNAM", 1) + Number("NNAM", quest);
}
Bytes Tree()
{
  return Record("TES4", 0) + Record("SMBN", 1, NodeFields(0)) +
    Record("SMEN", 2, NodeFields(1) + Number("ENAM", kDialogue)) +
    Record("SMBN", 3, NodeFields(2)) + Record("SMQN", 4, QuestFields(3, 5)) +
    Record("QUST", 5);
}
Index Read(const Bytes& master, const Bytes& patch = {})
{
  espm::Browser base(master.data(), master.size());
  espm::Combiner combiner;
  combiner.AddSource(&base, "Base.esm");
  std::unique_ptr<espm::Browser> override;
  if (!patch.empty()) {
    override = std::make_unique<espm::Browser>(patch.data(), patch.size());
    combiner.AddSource(override.get(), "Patch.esp");
  }
  return Index(*combiner.Combine());
}
Bytes Patch(const Bytes& records)
{
  return Record("TES4", 0, Field("MAST", Bytes("Base.esm\0", 9))) + records;
}
}

TEST_CASE("Story route index follows winning nested nodes across plugin IDs",
          "[StoryEventRouteIndex]")
{
  const auto simple = Read(Tree());
  const auto& only = simple.Find(5, kDialogue);
  REQUIRE(only.size() == 1);
  REQUIRE(only[0].gate.empty());
  REQUIRE(only[0].nodeIds == std::vector<uint32_t>{ 1, 2, 3, 4 });
  REQUIRE(simple.Find(5, 42).empty());

  espm::CTDA condition{};
  condition.functionIndex = 576;
  constexpr uint32_t addedQuest = 0x0100000a;
  const auto conditions =
    Field("CTDA", Binary(condition)) + Field("CIS2", Bytes("Key\0", 4));
  const auto index = Read(
    Tree(),
    Patch(Record("SMBN", 3, NodeFields(2, 0, 0, 1) + conditions) +
          Record("SMQN", 4,
                 NodeFields(3) + Number("QNAM", 2) +
                   Number("NNAM", addedQuest) + Number("FNAM", 2) +
                   Field("RNAM", Binary(24.0f)) + Number("NNAM", addedQuest)) +
          Record("QUST", addedQuest)));
  REQUIRE(index.Find(5, kDialogue).empty());
  const auto& matches = index.Find(addedQuest, kDialogue);
  REQUIRE(matches.size() == 2);
  REQUIRE(matches[0].questIndex == 0);
  REQUIRE(matches[1].questIndex == 1);
  REQUIRE_FALSE(matches[0].gate.empty());
  REQUIRE(matches[0].eventNodeId == 2);
  REQUIRE(matches[0].questNodeId == 4);
  auto branch = index.GetNode(3);
  REQUIRE(branch);
  REQUIRE(branch->fileIndex == 1);
  REQUIRE(branch->conditions.size() == 1);
  REQUIRE(branch->conditions[0].data.functionIndex == 576);
  REQUIRE(branch->conditions[0].parameter2String == "Key");
  auto leaf = index.GetNode(4);
  REQUIRE(leaf);
  REQUIRE(leaf->quests[0].flags == 2);
  REQUIRE(leaf->quests[0].resetHours == 24.0f);
  REQUIRE_FALSE(leaf->quests[1].resetHours);
}

TEST_CASE("Deleted and disconnected Story Manager nodes do not route quests",
          "[StoryEventRouteIndex]")
{
  for (const auto& patch :
       { Record("SMEN", 2, {}, 0x20), Record("SMBN", 3, {}, 0x20),
         Record("SMQN", 4, {}, 0x20), Record("QUST", 5, {}, 0x20),
         Record("SMBN", 3, NodeFields(3)),  // cyclic PNAM
         Record("SMBN", 3, NodeFields(99)), // broken PNAM
         Record("SMBN", 3, NodeFields(1)),  // no event ancestor
         Record("SMEN", 2, NodeFields(1) + Number("ENAM", 123)) }) {
    REQUIRE(Read(Tree(), Patch(patch)).Find(5, kDialogue).empty());
  }
}

TEST_CASE("Story route structural decisions never silently select a candidate",
          "[StoryEventRouteIndex]")
{
  for (const auto& patch :
       { Record("SMBN", 3, NodeFields(2, 3)),    // cyclic SNAM
         Record("SMBN", 3, NodeFields(2, 1)),    // wrong parent for sibling
         Record("SMBN", 3, NodeFields(2, 0, 1)), // random
         Record("SMBN", 3, NodeFields(2) + Number("FUTR", 9)),
         Record("SMQN", 4, QuestFields(3, 5) + Field("RNAM", Binary(3.f))),
         Record("SMQN", 4, QuestFields(3, 5) + Number("FNAM", 2)),
         Record("SMBN", 0x01000011, NodeFields(2, 3)), // competing child
         Record("SMEN", 0x01000012,
                NodeFields(1) + Number("ENAM", kDialogue)) }) {
    auto index = Read(Tree(), Patch(patch));
    auto& matches = index.Find(5, kDialogue);
    REQUIRE(matches.size() == 1);
    REQUIRE_FALSE(matches[0].gate.empty());
  }
}

TEST_CASE("Story Manager count and entry metadata must match record structure",
          "[StoryEventRouteIndex]")
{
  REQUIRE_THROWS(Read(
    Tree(), Patch(Record("SMQN", 4, QuestFields(3, 5) + Number("NNAM", 6)))));
  REQUIRE_THROWS(
    Read(Tree(), Patch(Record("SMBN", 3, NodeFields(2, 0, 0, 1)))));
  REQUIRE_THROWS(Read(
    Tree(),
    Patch(Record("SMQN", 4,
                 NodeFields(3) + Number("QNAM", 0) + Number("FNAM", 2)))));
}
