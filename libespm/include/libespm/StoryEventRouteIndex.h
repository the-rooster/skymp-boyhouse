#pragma once
#include "QuestLifecycleData.h"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace espm {
class CombineBrowser;

// A winning NNAM reference establishes candidacy, not event selection.
class StoryEventRouteIndex
{
public:
  enum class Kind
  {
    Event,
    Branch,
    Quest
  };
  struct Entry
  {
    uint32_t questId = 0;
    std::optional<uint32_t> flags;
    std::optional<float> resetHours;
  };
  struct Node
  {
    Kind kind = Kind::Branch;
    uint32_t id = 0, parent = 0, previousSibling = 0;
    uint8_t fileIndex = 0; // CTDA operands are local to this plugin.
    uint32_t flags = 0, maxConcurrent = 0;
    std::optional<uint32_t> eventType, questsToRun;
    std::vector<QuestLifecycleData::Condition> conditions;
    std::vector<Entry> quests; // NNAM order, including repeated entries
    bool unknownFields = false;
  };
  struct Route
  {
    uint32_t eventNodeId = 0, questNodeId = 0, questIndex = 0;
    std::vector<uint32_t> nodeIds; // Structural root through the quest node.
    // First structural selection decision requiring a scheduler. Independently
    // evaluate conditions on every node in nodeIds, even when gate is empty.
    std::string gate;
  };

  explicit StoryEventRouteIndex(const CombineBrowser& browser);
  const std::vector<Route>& Find(uint32_t questId,
                                 uint32_t eventType) const noexcept;
  const Node* GetNode(uint32_t id) const noexcept;

private:
  std::unordered_map<uint32_t, Node> nodes;
  std::map<std::pair<uint32_t, uint32_t>, std::vector<Route>> routes;
};
}
