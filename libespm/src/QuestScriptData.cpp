#include "libespm/QuestScriptData.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>
#include <stdexcept>

namespace espm {
namespace {
class Cursor
{
public:
  Cursor(const uint8_t* bytes_, size_t size_)
    : bytes(bytes_)
    , size(size_)
  {
  }
  size_t Remaining() const { return size - offset; }
  [[noreturn]] void Fail(const char* reason) const
  {
    throw std::runtime_error("Invalid quest VMAD at byte " +
                             std::to_string(offset) + ": " + reason);
  }
  template <class T>
  T Read()
  {
    if (Remaining() < sizeof(T))
      Fail("truncated value");
    T value;
    std::memcpy(&value, bytes + offset, sizeof(T));
    offset += sizeof(T);
    return value;
  }
  std::string String()
  {
    const auto length = Read<uint16_t>();
    if (Remaining() < length)
      Fail("truncated string");
    std::string value(reinterpret_cast<const char*>(bytes + offset), length);
    offset += length;
    return value;
  }
  void Header(uint16_t version, uint16_t format)
  {
    if (version < 4 || version > 5)
      Fail("unsupported VMAD version");
    if (format != 1 && format != 2)
      Fail("unsupported object format");
  }
  QuestScriptValue Object(uint16_t format)
  {
    QuestScriptValue value;
    value.type = 1;
    if (format == 1) {
      value.formId = Read<uint32_t>();
      value.alias = Read<uint16_t>();
      Read<uint16_t>();
    } else {
      Read<uint16_t>();
      value.alias = Read<uint16_t>();
      value.formId = Read<uint32_t>();
    }
    return value;
  }
  QuestScriptValue Value(uint8_t type, uint16_t format)
  {
    QuestScriptValue value;
    value.type = type;
    switch (type) {
      case 1:
        return Object(format);
      case 2:
        value.text = String();
        break;
      case 3:
        value.integer = Read<int32_t>();
        break;
      case 4:
        value.real = Read<float>();
        break;
      case 5: {
        const auto boolean = Read<uint8_t>();
        if (boolean > 1)
          Fail("invalid boolean");
        value.boolean = boolean != 0;
        break;
      }
      case 11:
      case 12:
      case 13:
      case 14:
      case 15: {
        const auto count = Read<uint32_t>();
        if (count > Remaining() || count > 1000000)
          Fail("invalid array length");
        for (uint32_t i = 0; i < count; ++i)
          value.elements.push_back(Value(type - 10, format));
        break;
      }
      default:
        Fail("unsupported property type");
    }
    return value;
  }
  std::vector<QuestScript> Scripts(uint16_t format)
  {
    std::vector<QuestScript> scripts;
    const auto count = Read<uint16_t>();
    for (uint32_t i = 0; i < count; ++i) {
      QuestScript script;
      script.name = String();
      script.status = Read<uint8_t>();
      const auto properties = Read<uint16_t>();
      for (uint32_t p = 0; p < properties; ++p) {
        QuestScriptProperty prop;
        prop.name = String();
        const auto type = Read<uint8_t>();
        prop.status = Read<uint8_t>();
        prop.value = Value(type, format);
        script.properties.push_back(std::move(prop));
      }
      scripts.push_back(std::move(script));
    }
    return scripts;
  }

private:
  const uint8_t* bytes;
  size_t size, offset = 0;
};
}

QuestScriptData QuestScriptData::Parse(const uint8_t* bytes, size_t size)
{
  if (!bytes || size > 64 * 1024 * 1024)
    throw std::runtime_error("Invalid VMAD buffer");
  Cursor cursor(bytes, size);
  QuestScriptData result;
  result.version = cursor.Read<uint16_t>();
  result.objectFormat = cursor.Read<uint16_t>();
  cursor.Header(result.version, result.objectFormat);
  result.scripts = cursor.Scripts(result.objectFormat);
  if (!cursor.Remaining())
    return result;
  result.hasFragments = true;
  result.bindVersion = cursor.Read<uint8_t>();
  if (result.bindVersion != 2)
    cursor.Fail("unsupported fragment binding version");
  const auto count = cursor.Read<uint16_t>();
  result.fileName = cursor.String();
  for (uint32_t i = 0; i < count; ++i) {
    Fragment fragment;
    fragment.stage = cursor.Read<uint32_t>();
    fragment.logEntry = cursor.Read<uint32_t>();
    fragment.flags = cursor.Read<uint8_t>();
    fragment.script = cursor.String();
    fragment.function = cursor.String();
    result.fragments.push_back(std::move(fragment));
  }
  const auto aliasCount = cursor.Read<uint16_t>();
  for (uint32_t i = 0; i < aliasCount; ++i) {
    Alias alias;
    alias.object = cursor.Object(result.objectFormat);
    alias.version = cursor.Read<uint16_t>();
    alias.objectFormat = cursor.Read<uint16_t>();
    cursor.Header(alias.version, alias.objectFormat);
    alias.scripts = cursor.Scripts(alias.objectFormat);
    result.aliases.push_back(std::move(alias));
  }
  if (cursor.Remaining())
    cursor.Fail("unexpected trailing data");
  return result;
}

QuestScriptData QuestScriptData::Read(const RecordHeader* quest,
                                      CompressedFieldsCache& cache)
{
  if (!quest || quest->GetType() != "QUST")
    throw std::invalid_argument("Expected a quest record");
  QuestScriptData result;
  bool found = false;
  RecordHeaderAccess::IterateFields(
    quest,
    [&](const char* tag, uint32_t size, const char* data) {
      if (std::memcmp(tag, "VMAD", 4))
        return;
      if (found)
        throw std::runtime_error("Duplicate quest VMAD field");
      found = true;
      result = Parse(reinterpret_cast<const uint8_t*>(data), size);
    },
    cache);
  return result;
}
}
