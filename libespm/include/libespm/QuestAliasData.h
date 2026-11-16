#pragma once
#include "CTDA.h"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace espm {
class RecordHeader;
class CompressedFieldsCache;

// Owned alias metadata. Alias IDs are quest-local, never FormIDs. Keep fields
// whose runtime semantics are not implemented so callers can reject them.
struct QuestAliasDefinition
{
  enum class Kind
  {
    Reference,
    Location
  };
  struct Creation
  {
    int16_t alias;
    uint16_t flags;
  };
  uint32_t id = 0;
  Kind kind = Kind::Reference;
  std::string name;
  uint32_t flags = 0;
  std::optional<Creation> creation;
  std::vector<CTDA> conditions;
  std::map<std::string, std::vector<std::vector<uint8_t>>> fields;
  std::vector<std::string> unknownFields;

  bool Has(const std::string& tag) const;
  std::optional<uint32_t> Unsigned(const std::string& tag) const;
  std::optional<int32_t> Signed(const std::string& tag) const;
};

class QuestAliasData
{
public:
  struct Field
  {
    std::string tag;
    std::vector<uint8_t> bytes;
  };
  std::vector<QuestAliasDefinition> aliases;
  const QuestAliasDefinition* Find(uint32_t id) const;
  static QuestAliasData Parse(const std::vector<Field>& fields);
  static QuestAliasData Read(const RecordHeader* quest,
                             CompressedFieldsCache& cache);
};
}
