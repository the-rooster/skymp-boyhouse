#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace espm {
class RecordHeader;
class CompressedFieldsCache;

// Owned, bounds-checked VMAD data. Object values retain alias IDs; discarding
// those IDs turns quest-alias properties into the wrong Papyrus objects.
struct QuestScriptValue
{
  uint8_t type = 0;
  uint32_t formId = 0;
  uint16_t alias = 0xffff;
  int32_t integer = 0;
  float real = 0;
  bool boolean = false;
  std::string text;
  std::vector<QuestScriptValue> elements;
};

struct QuestScriptProperty
{
  std::string name;
  uint8_t status = 0;
  QuestScriptValue value;
};

struct QuestScript
{
  std::string name;
  uint8_t status = 0;
  std::vector<QuestScriptProperty> properties;
};

struct QuestScriptData
{
  struct Fragment
  {
    uint32_t stage = 0;
    uint32_t logEntry = 0;
    uint8_t flags = 0;
    std::string script, function;
  };
  struct Alias
  {
    QuestScriptValue object;
    uint16_t version = 0, objectFormat = 0;
    std::vector<QuestScript> scripts;
  };

  uint16_t version = 0, objectFormat = 0;
  std::vector<QuestScript> scripts;
  bool hasFragments = false;
  uint8_t bindVersion = 0;
  std::string fileName;
  std::vector<Fragment> fragments;
  std::vector<Alias> aliases;

  // Layout: TES5Edit Core/wbDefinitionsTES5.pas, wbVMADFragmentedQUST.
  static QuestScriptData Parse(const uint8_t* bytes, size_t size);
  static QuestScriptData Read(const RecordHeader* quest,
                              CompressedFieldsCache& cache);
};
}
