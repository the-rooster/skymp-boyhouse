#include "libespm/QuestLifecycleData.h"
#include "libespm/RecordHeaderAccess.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace espm {
namespace {
constexpr size_t kMaxBytes = 64 * 1024 * 1024;
constexpr size_t kMaxFields = 1024 * 1024;

template <class T>
T Number(const std::vector<uint8_t>& bytes, size_t offset = 0)
{
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
    throw std::runtime_error("Truncated quest lifecycle numeric field");
  T result;
  std::memcpy(&result, bytes.data() + offset, sizeof(result));
  return result;
}
void Size(const std::vector<uint8_t>& bytes, size_t expected)
{
  if (bytes.size() != expected)
    throw std::runtime_error("Invalid quest lifecycle field size");
}
std::string String(const std::vector<uint8_t>& bytes)
{
  if (bytes.empty() || bytes.size() > 65536 || bytes.back() != 0 ||
      std::find(bytes.begin(), bytes.end() - 1, 0) != bytes.end() - 1)
    throw std::runtime_error("Invalid quest lifecycle condition string");
  return std::string(bytes.begin(), bytes.end() - 1);
}
}

QuestLifecycleData QuestLifecycleData::Parse(const std::vector<Field>& fields)
{
  if (fields.size() > kMaxFields)
    throw std::runtime_error("Too many quest lifecycle fields");
  enum class Scope
  {
    Dialogue,
    Story,
    Stages,
    Objectives,
    Aliases
  } scope = Scope::Dialogue;
  QuestLifecycleData result;
  bool foundData = false, foundNext = false, foundAliases = false;
  Condition* lastCondition = nullptr;
  size_t total = 0;
  for (const auto& field : fields) {
    const auto& tag = field.tag;
    const auto& bytes = field.bytes;
    if (tag.size() != 4 || bytes.size() > kMaxBytes - total)
      throw std::runtime_error("Invalid quest lifecycle field buffer");
    total += bytes.size();
    const bool top = scope == Scope::Dialogue || scope == Scope::Story;
    if (top && (tag == "CIS1" || tag == "CIS2")) {
      if (!lastCondition)
        throw std::runtime_error("Orphan quest lifecycle condition string");
      auto& value = tag == "CIS1" ? lastCondition->parameter1String
                                  : lastCondition->parameter2String;
      if (value)
        throw std::runtime_error("Duplicate quest lifecycle condition string");
      value = String(bytes);
      continue;
    }
    lastCondition = nullptr;
    if (tag == "DNAM") {
      if (foundData || scope != Scope::Dialogue)
        throw std::runtime_error("Duplicate or misplaced quest DNAM");
      Size(bytes, 12);
      foundData = true;
      result.flags = Number<uint16_t>(bytes);
      result.priority = bytes[2];
      result.formVersion = bytes[3];
      result.unknownData = Number<uint32_t>(bytes, 4);
      result.questType = Number<uint32_t>(bytes, 8);
    } else if (tag == "ENAM") {
      if (result.event || scope != Scope::Dialogue)
        throw std::runtime_error("Duplicate or misplaced quest ENAM");
      Size(bytes, 4);
      result.event = Number<uint32_t>(bytes);
    } else if (tag == "QTGL" && top) {
      Size(bytes, 4);
      result.textDisplayGlobals.push_back(Number<uint32_t>(bytes));
    } else if (tag == "NEXT") {
      if (foundNext || !foundData || scope != Scope::Dialogue)
        throw std::runtime_error("Duplicate or misplaced quest NEXT");
      Size(bytes, 0);
      foundNext = true;
      scope = Scope::Story;
    } else if (tag == "INDX") {
      if (!foundNext ||
          (scope != Scope::Story && scope != Scope::Stages))
        throw std::runtime_error("Misplaced quest stage index");
      Size(bytes, 4);
      if (result.stages.size() >= 65536)
        throw std::runtime_error("Too many quest lifecycle stages");
      scope = Scope::Stages;
      result.stages.push_back(
        { Number<uint16_t>(bytes), bytes[2], bytes[3] });
    } else if (tag == "QOBJ") {
      if (!foundNext || scope == Scope::Aliases)
        throw std::runtime_error("Misplaced quest objective index");
      Size(bytes, 2);
      scope = Scope::Objectives;
    } else if (tag == "ANAM") {
      if (!foundNext || foundAliases)
        throw std::runtime_error("Duplicate or misplaced quest ANAM");
      Size(bytes, 4);
      foundAliases = true;
      result.nextAliasId = Number<uint32_t>(bytes);
      scope = Scope::Aliases;
    } else if (tag == "ALST" || tag == "ALLS" || tag == "ALED") {
      if (!foundAliases)
        throw std::runtime_error("Quest alias precedes ANAM");
      // Alias shape and nested alias conditions belong to QuestAliasData.
    } else if (tag == "CTDA" && top) {
      if (!foundData)
        throw std::runtime_error("Quest condition precedes DNAM");
      Size(bytes, sizeof(CTDA));
      auto& conditions = scope == Scope::Dialogue
        ? result.dialogueConditions
        : result.storyConditions;
      conditions.push_back({ Number<CTDA>(bytes), {}, {} });
      lastCondition = &conditions.back();
    } else if (top && tag != "EDID" && tag != "VMAD" && tag != "FULL" &&
               tag != "FLTR") {
      result.unknownFields.push_back(field);
    }
  }
  if (!foundData || !foundNext || !foundAliases)
    throw std::runtime_error("Missing required quest lifecycle field");
  return result;
}

QuestLifecycleData QuestLifecycleData::Read(const RecordHeader* quest,
                                            CompressedFieldsCache& cache)
{
  if (!quest || quest->GetType() != "QUST")
    throw std::invalid_argument("Expected quest record for lifecycle metadata");
  std::vector<Field> fields;
  size_t total = 0;
  RecordHeaderAccess::IterateFields(
    quest,
    [&](const char* tag, uint32_t size, const char* data) {
      if (fields.size() >= kMaxFields || size > kMaxBytes - total)
        throw std::runtime_error("Quest lifecycle metadata exceeds limits");
      total += size;
      fields.push_back(
        { std::string(tag, 4), std::vector<uint8_t>(data, data + size) });
    },
    cache);
  return Parse(fields);
}
}
