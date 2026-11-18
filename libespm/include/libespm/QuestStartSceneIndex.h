#pragma once
#include <cstdint>
#include <unordered_map>

namespace espm {
class CombineBrowser;

class QuestStartSceneIndex
{
public:
  explicit QuestStartSceneIndex(const CombineBrowser& browser);
  // Lowest global ID of a winning child scene marked Begin on Quest Start.
  // This picks a stable example for diagnostics, not engine playback order.
  uint32_t Find(uint32_t questId) const noexcept;

private:
  std::unordered_map<uint32_t, uint32_t> firstByQuest;
};
}
