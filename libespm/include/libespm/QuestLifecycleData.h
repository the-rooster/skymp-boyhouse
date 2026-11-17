#pragma once
#include "CTDA.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace espm {
class RecordHeader;
class CompressedFieldsCache;

// Owned QUST lifecycle metadata, separate from VMAD bindings. Values are
// retained, not interpreted as permission to run a quest. The runtime must
// account for flags, story conditions, startup/shutdown stages and alias
// effects before enabling lifecycle execution.
struct QuestLifecycleData
{
  enum Flag : uint16_t
  {
    StartGameEnabled = 1 << 0,
    Completed = 1 << 1,
    AddIdleTopicToHello = 1 << 2,
    AllowRepeatedStages = 1 << 3,
    StartsEnabled = 1 << 4,
    DisplayedInHud = 1 << 5,
    Failed = 1 << 6,
    StageWait = 1 << 7,
    RunOnce = 1 << 8,
    ExcludeFromDialogueExport = 1 << 9,
    WarnOnAliasFillFailure = 1 << 10,
    Active = 1 << 11,
    RepeatsConditions = 1 << 12,
    KeepInstance = 1 << 13,
    WantDormant = 1 << 14,
    HasDialogueData = 1 << 15
  };
  struct Field
  {
    std::string tag;
    std::vector<uint8_t> bytes;
  };
  struct Condition
  {
    CTDA data{};
    std::optional<std::string> parameter1String, parameter2String;
  };
  struct Stage
  {
    enum Flag : uint8_t
    {
      StartUp = 1 << 1,
      ShutDown = 1 << 2,
      KeepInstanceData = 1 << 3
    };
    uint16_t index = 0;
    uint8_t flags = 0, unknown = 0;
  };

  uint16_t flags = 0;
  uint8_t priority = 0, formVersion = 0;
  // Disk DNAM bytes 4..7 are documented as unknown; the in-memory engine
  // QUEST_DATA layout is different. Do not reinterpret this as a timer.
  uint32_t unknownData = 0, questType = 0;
  std::optional<uint32_t> event;
  uint32_t nextAliasId = 0;
  std::vector<uint32_t> textDisplayGlobals;
  std::vector<Condition> dialogueConditions, storyConditions;
  // File order and duplicate indices are preserved. Duplicate stage IDs occur
  // in owned content; execution must resolve or reject that ambiguity.
  std::vector<Stage> stages;
  // Unrecognized fields in the quest-level section, before stages, objectives
  // and aliases. Those later sections have separate parsers.
  std::vector<Field> unknownFields;

  // Layout: TES5Edit Core/wbDefinitionsTES5.pas, QUST DNAM/NEXT/INDX.
  // Requires the record's singleton DNAM, NEXT and ANAM structural fields.
  static QuestLifecycleData Parse(const std::vector<Field>& fields);
  static QuestLifecycleData Read(const RecordHeader* quest,
                                 CompressedFieldsCache& cache);
};
}
