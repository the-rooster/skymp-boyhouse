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

namespace {
using Verdict = Index::Verdict;
using Check = Index::Check;

Bytes EventTree(const Bytes& descendants)
{
  return Record("TES4", 0) + Record("SMBN", 1, NodeFields(0)) +
    Record("SMEN", 2, NodeFields(1) + Number("ENAM", kDialogue)) + descendants;
}

Check PassNode(const Index::Node&)
{
  return { Verdict::Pass, {} };
}
Check ReadyQuest(const Index::Node&, uint32_t)
{
  return { Verdict::Pass, {} };
}
}

TEST_CASE(
  "Stacked Story Manager walks authored siblings and quests until admission",
  "[StoryEventSelection]")
{
  // Physical order and numeric IDs disagree with SNAM: 20 precedes 3.
  const auto index = Read(
    EventTree(Record("SMQN", 3,
                     NodeFields(2, 20) + Number("QNAM", 2) +
                       Number("NNAM", 14) + Number("NNAM", 13)) +
              Record("SMBN", 20, NodeFields(2)) +
              Record("SMQN", 30, QuestFields(20, 16)) + Record("QUST", 14) +
              Record("QUST", 13) + Record("QUST", 16)));
  std::vector<uint32_t> seenNodes, seenQuests;
  auto select = [&](uint32_t id, Verdict verdict) {
    seenNodes.clear();
    seenQuests.clear();
    return index.Select(
      kDialogue,
      [&](const Index::Node& node) {
        seenNodes.push_back(node.id);
        return PassNode(node);
      },
      [&](const Index::Node& node, uint32_t i) {
        const auto quest = node.quests.at(i).questId;
        seenQuests.push_back(quest);
        return Check{ quest == id ? verdict : Verdict::Reject,
                      "checked quest" };
      });
  };

  auto admitted = select(13, Verdict::Pass);
  REQUIRE(seenNodes == std::vector<uint32_t>{ 1, 2, 20, 30, 3 });
  REQUIRE(seenQuests == std::vector<uint32_t>{ 16, 14, 13 });
  REQUIRE(admitted.verdict == Verdict::Pass);
  REQUIRE(admitted.nodeIds == std::vector<uint32_t>{ 1, 2, 3 });
  REQUIRE(admitted.nodeId == 3);
  REQUIRE(admitted.questId == 13);
  REQUIRE(admitted.questIndex == 1);

  auto earlier = select(16, Verdict::Unsupported);
  REQUIRE(earlier.verdict == Verdict::Unsupported);
  REQUIRE(earlier.questId == 16);
  REQUIRE(earlier.detail == "checked quest");
  REQUIRE(seenQuests == std::vector<uint32_t>{ 16 });

  auto none = select(100, Verdict::Pass);
  REQUIRE(none.verdict == Verdict::Reject);
  REQUIRE(seenQuests == std::vector<uint32_t>{ 16, 14, 13 });
  REQUIRE(index.Select(123, PassNode, ReadyQuest).verdict == Verdict::Reject);
}

TEST_CASE("Failed Story Manager conditions skip descendants and unknown ones "
          "stop traversal",
          "[StoryEventSelection]")
{
  const auto index = Read(EventTree(
    Record("SMBN", 7, NodeFields(2, 0, 1)) +
    Record("SMQN", 8, QuestFields(7, 13)) +
    Record("SMQN", 9,
           NodeFields(2, 7) + Number("QNAM", 1) + Number("NNAM", 14)) +
    Record("QUST", 13) + Record("QUST", 14)));
  auto select = [&](uint32_t gate, Verdict verdict) {
    return index.Select(
      kDialogue,
      [&](const Index::Node& node) {
        return Check{ node.id == gate ? verdict : Verdict::Pass,
                      "node condition" };
      },
      ReadyQuest);
  };
  REQUIRE(select(7, Verdict::Reject).questId == 14);
  auto blocked = select(7, Verdict::Unsupported);
  REQUIRE(blocked.verdict == Verdict::Unsupported);
  REQUIRE(blocked.nodeId == 7);
  REQUIRE(blocked.detail == "node condition");
  for (const auto ancestor : { 1u, 2u }) {
    auto rejected = select(ancestor, Verdict::Reject);
    REQUIRE(rejected.verdict == Verdict::Reject);
    REQUIRE(rejected.nodeIds.back() == ancestor);
  }
  REQUIRE(select(7, Verdict::Pass).verdict == Verdict::Unsupported);
}

TEST_CASE(
  "Story Manager skips detached siblings only on a proved condition rejection",
  "[StoryEventSelection]")
{
  for (const auto prev : { 9u, 88u, 1u }) {
    auto index = Read(EventTree(
      Record("SMQN", 7, QuestFields(2, 13)) +
      Record("SMQN", 9,
             NodeFields(2, prev) + Number("QNAM", 1) + Number("NNAM", 14)) +
      Record("QUST", 13) + Record("QUST", 14)));
    for (const auto unresolved :
         { Verdict::Pass, Verdict::Unsupported, Verdict::Reject }) {
      std::vector<uint32_t> checked;
      auto result = index.Select(
        kDialogue,
        [&](const Index::Node& node) {
          return Check{ node.id == 9 ? unresolved : Verdict::Pass, {} };
        },
        [&](const Index::Node& node, uint32_t) {
          checked.push_back(node.id);
          return Check{ Verdict::Pass, {} };
        });
      if (unresolved == Verdict::Reject) {
        REQUIRE(result.verdict == Verdict::Pass);
        REQUIRE(result.questId == 13);
        REQUIRE(checked == std::vector<uint32_t>{ 7 });
      } else {
        REQUIRE(result.verdict == Verdict::Unsupported);
        REQUIRE(result.nodeIds == std::vector<uint32_t>{ 1, 2, 9 });
        if (unresolved == Verdict::Pass)
          REQUIRE(checked == std::vector<uint32_t>{ 9 });
        else
          REQUIRE(checked.empty());
      }
    }
  }
}

TEST_CASE("Story Manager requires complete ancestry, valid winning quests and "
          "supported scheduling",
          "[StoryEventSelection]")
{
  for (const auto& patch :
       { Record("SMEN", 2, NodeFields(99) + Number("ENAM", kDialogue)),
         Record("SMBN", 1, NodeFields(1)),
         Record("SMEN", 0x01000017,
                NodeFields(1, 2) + Number("ENAM", kDialogue)),
         Record("SMQN", 4, QuestFields(3, 5) + Field("RNAM", Binary(1.0f))),
         Record("SMQN", 4, QuestFields(3, 5) + Number("FNAM", 1)),
         Record("SMQN", 4,
                NodeFields(3, 0, 0x20000) + Number("QNAM", 1) +
                  Number("NNAM", 5)),
         Record("SMBN", 3, NodeFields(2) + Number("FUTR", 1)),
         Record("SMQN", 0x01000012, QuestFields(4, 5)),
         Record("QUST", 5, {}, 0x20) }) {
    auto index = Read(Tree(), Patch(patch));
    bool attempted = false;
    auto result =
      index.Select(kDialogue, PassNode, [&](const Index::Node&, uint32_t) {
        attempted = true;
        return Check{ Verdict::Pass, {} };
      });
    REQUIRE(result.verdict == Verdict::Unsupported);
    REQUIRE_FALSE(result.detail.empty());
    REQUIRE_FALSE(attempted);
  }
}

TEST_CASE("An unordered branch can only be excluded after every descendant is "
          "ruled out",
          "[StoryEventSelection]")
{
  // The detached branch (9) has no conditions of its own. Its first child
  // (10) rejects at the node. Its second child (11) has two quest candidates.
  const auto index =
    Read(EventTree(Record("SMQN", 7, QuestFields(2, 13)) +
                   Record("SMBN", 9, NodeFields(2, 9)) +
                   Record("SMQN", 10, QuestFields(9, 14)) +
                   Record("SMQN", 11,
                          NodeFields(9, 10) + Number("QNAM", 2) +
                            Number("NNAM", 15) + Number("NNAM", 16)) +
                   Record("QUST", 13) + Record("QUST", 14) +
                   Record("QUST", 15) + Record("QUST", 16)));
  auto check = [&](Verdict last) {
    return index.Select(
      kDialogue,
      [&](const Index::Node& node) {
        return Check{ node.id == 10 ? Verdict::Reject : Verdict::Pass, {} };
      },
      [&](const Index::Node& node, uint32_t i) {
        const auto id = node.quests.at(i).questId;
        REQUIRE(id != 14);
        if (id == 13)
          return Check{ Verdict::Pass, {} };
        return Check{ id == 15 ? Verdict::Reject : last,
                      "quest prerequisite" };
      });
  };
  auto eligible = check(Verdict::Reject);
  REQUIRE(eligible.verdict == Verdict::Pass);
  REQUIRE(eligible.questId == 13);
  for (auto unresolved : { Verdict::Pass, Verdict::Unsupported }) {
    auto blocked = check(unresolved);
    REQUIRE(blocked.verdict == Verdict::Unsupported);
    REQUIRE(blocked.nodeIds == std::vector<uint32_t>{ 1, 2, 9, 11 });
    REQUIRE(blocked.questId == 16);
    REQUIRE(blocked.questIndex == 1);
    if (unresolved == Verdict::Unsupported)
      REQUIRE(blocked.detail == "quest prerequisite");
  }
}

TEST_CASE(
  "A random node in an unordered subtree can be ruled out without a draw",
  "[StoryEventSelection]")
{
  const auto records = [](uint32_t flags, uint32_t previous) {
    return EventTree(Record("SMQN", 7, QuestFields(2, 13)) +
                     Record("SMBN", 9, NodeFields(2, previous, flags)) +
                     Record("SMQN", 10,
                            NodeFields(9) + Number("QNAM", 2) +
                              Number("NNAM", 14) + Number("NNAM", 15)) +
                     Record("QUST", 13) + Record("QUST", 14) +
                     Record("QUST", 15));
  };
  const auto index = Read(records(1, 9)); // detached random branch
  auto tryChoice = [&](const Index& graph, Verdict last) {
    return graph.Select(
      kDialogue, PassNode, [&](const Index::Node& node, uint32_t i) {
        auto id = node.quests.at(i).questId;
        if (id == 14)
          return Check{ Verdict::Reject, {} };
        return Check{ id == 15 ? last : Verdict::Pass, "quest prerequisite" };
      });
  };
  auto ineligible = tryChoice(index, Verdict::Reject);
  REQUIRE(ineligible.verdict == Verdict::Pass);
  REQUIRE(ineligible.questId == 13);
  for (auto last : { Verdict::Pass, Verdict::Unsupported }) {
    auto unresolved = tryChoice(index, last);
    REQUIRE(unresolved.verdict == Verdict::Unsupported);
    REQUIRE(unresolved.questId == 15);
    REQUIRE(unresolved.nodeIds == std::vector<uint32_t>{ 1, 2, 9, 10 });
    REQUIRE(unresolved.detail ==
            (last == Verdict::Pass
               ? "Story Manager random selection requires a draw"
               : "quest prerequisite"));
  }
  auto mixedFlags = tryChoice(Read(records(0x20001, 9)), Verdict::Reject);
  REQUIRE(mixedFlags.verdict == Verdict::Unsupported);
  REQUIRE(mixedFlags.nodeId == 9);
}
