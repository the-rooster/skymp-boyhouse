#include "libespm/LeveledListBase.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>
#include <optional>
#include <set>
#include <stdexcept>

namespace espm {

LeveledListBase::Data LeveledListBase::GetData(
  CompressedFieldsCache& compressedFieldsCache) const
{
  Data result;
  std::optional<uint8_t> count;
  std::set<std::string> seen;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t dataSize, const char* data) {
      const std::string tag(type, 4);
      if (tag == "LVLD" || tag == "LVLF" || tag == "LLCT" || tag == "LVLG") {
        const auto expected = tag == "LVLG" ? 4u : 1u;
        if (dataSize != expected || !seen.insert(tag).second)
          throw std::runtime_error("Malformed leveled list singleton");
      }
      if (!std::memcmp(type, "EDID", 4)) {
        result.editorId = data;
      } else if (!std::memcmp(type, "LVLF", 4)) {
        result.leveledItemFlags = *reinterpret_cast<const uint8_t*>(data);
      } else if (!std::memcmp(type, "LVLG", 4)) {
        result.chanceNoneGlobalId = *reinterpret_cast<const uint32_t*>(data);
      } else if (!std::memcmp(type, "LVLD", 4)) {
        result.chanceNone = *reinterpret_cast<const uint8_t*>(data);
      } else if (!std::memcmp(type, "LLCT", 4)) {
        count = static_cast<uint8_t>(*data);
      } else if (tag == "LVLO") {
        if (dataSize != 12 || result.entries.size() >= 255)
          throw std::runtime_error("Malformed leveled list entry");
        Entry entry;
        entry.dataSize = 12;
        uint16_t level, quantity;
        std::memcpy(&level, data, 2);
        std::memcpy(&entry.formId, data + 4, 4);
        std::memcpy(&quantity, data + 8, 2);
        entry.level = level;
        entry.count = quantity;
        result.entries.push_back(entry);
      } else if (tag == "COED") {
        if (dataSize != 12 || result.entries.empty())
          throw std::runtime_error("Malformed leveled list ownership data");
      }
    },
    compressedFieldsCache);
  // Empty owned lists can omit LLCT entirely.
  if ((count && *count != result.entries.size()) ||
      (!count && !result.entries.empty()))
    throw std::runtime_error("Leveled list entry count mismatch");
  result.numEntries = static_cast<uint8_t>(result.entries.size());
  return result;
}

}
