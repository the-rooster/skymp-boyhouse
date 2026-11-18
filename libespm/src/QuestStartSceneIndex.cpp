#include "libespm/QuestStartSceneIndex.h"
#include "libespm/CombineBrowser.h"
#include "libespm/RecordHeaderAccess.h"
#include <algorithm>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace espm {
QuestStartSceneIndex::QuestStartSceneIndex(const CombineBrowser& browser)
{
  for (const auto& scene : browser.GetDistinctRecordsByType("SCEN")) {
    if (scene.rec->IsDeleted())
      continue;

    bool inAction = false;
    std::optional<uint32_t> flags, parent;
    RecordHeaderAccess::IterateFields(
      scene.rec,
      [&](const char* tag, uint32_t size, const char* bytes) {
        if (!std::memcmp(tag, "ANAM", 4)) {
          if ((size != 0 && size != 2) || inAction == (size != 0))
            throw std::runtime_error("Invalid SCEN action boundary");
          inAction = size != 0;
          return;
        }
        // Actions have their own FNAM flags and PNAM package IDs.
        if (inAction)
          return;
        const bool isFlags = !std::memcmp(tag, "FNAM", 4);
        if (!isFlags && std::memcmp(tag, "PNAM", 4))
          return;
        auto& field = isFlags ? flags : parent;
        if (field || size != sizeof(uint32_t))
          throw std::runtime_error("Invalid SCEN flags or parent field");
        uint32_t value;
        std::memcpy(&value, bytes, sizeof(value));
        field = value;
      },
      browser.GetCache());
    if (inAction)
      throw std::runtime_error("Unterminated SCEN action");
    if (!(flags.value_or(0) & 1))
      continue;
    if (!parent.value_or(0))
      throw std::runtime_error("Auto-start scene has no parent quest");

    const auto owner = scene.ToGlobalId(*parent);
    const auto record = browser.LookupById(owner);
    if (!record.rec || record.rec->IsDeleted() ||
        record.rec->GetType() != "QUST")
      throw std::runtime_error("Auto-start scene has invalid parent quest");
    const auto sceneId = scene.ToGlobalId(scene.rec->GetId());
    const auto entry = firstByQuest.emplace(owner, sceneId);
    if (!entry.second)
      entry.first->second = std::min(entry.first->second, sceneId);
  }
}

uint32_t QuestStartSceneIndex::Find(uint32_t questId) const noexcept
{
  const auto it = firstByQuest.find(questId);
  return it == firstByQuest.end() ? 0 : it->second;
}
}
