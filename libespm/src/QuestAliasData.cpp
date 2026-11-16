#include "libespm/QuestAliasData.h"
#include "libespm/RecordHeaderAccess.h"
#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>

namespace espm {
namespace {
template <class T>
T Number(const std::vector<uint8_t>& bytes)
{
  if (bytes.size() != sizeof(T))
    throw std::runtime_error("Invalid quest alias numeric field size");
  T result;
  std::memcpy(&result, bytes.data(), sizeof(T));
  return result;
}
std::string String(const std::vector<uint8_t>& bytes)
{
  if (bytes.empty() || bytes.size() > 65536 || bytes.back() != 0 ||
      std::find(bytes.begin(), bytes.end() - 1, 0) != bytes.end() - 1)
    throw std::runtime_error("Invalid quest alias string");
  return std::string(bytes.begin(), bytes.end() - 1);
}
void Finish(const QuestAliasDefinition& alias)
{
  if (!alias.Has("ALID") || !alias.Has("FNAM"))
    throw std::runtime_error("Quest alias has no name or flags");
  if (const auto count = alias.Unsigned("KSIZ")) {
    const auto it = alias.fields.find("KWDA");
    const auto actual =
      it == alias.fields.end() ? 0 : it->second[0].size() / 4;
    if (*count != actual)
      throw std::runtime_error("Quest alias keyword count mismatch");
  } else if (alias.Has("KWDA")) {
    throw std::runtime_error("Quest alias keywords have no count");
  }
  if (const auto count = alias.Unsigned("COCT")) {
    const auto it = alias.fields.find("CNTO");
    const auto actual = it == alias.fields.end() ? 0 : it->second.size();
    if (*count != actual)
      throw std::runtime_error("Quest alias inventory count mismatch");
  }
  // COCT is optional in older owned QUST records; CNTO entries remain owned
  // and authoritative when the count field is absent.
}
}

bool QuestAliasDefinition::Has(const std::string& tag) const
{
  return fields.count(tag) != 0;
}
std::optional<uint32_t> QuestAliasDefinition::Unsigned(
  const std::string& tag) const
{
  const auto it = fields.find(tag);
  if (it == fields.end())
    return std::nullopt;
  if (it->second.size() != 1)
    throw std::runtime_error("Expected singleton quest alias field");
  return Number<uint32_t>(it->second[0]);
}
std::optional<int32_t> QuestAliasDefinition::Signed(
  const std::string& tag) const
{
  const auto it = fields.find(tag);
  if (it == fields.end())
    return std::nullopt;
  if (it->second.size() != 1)
    throw std::runtime_error("Expected singleton quest alias field");
  return Number<int32_t>(it->second[0]);
}
const QuestAliasDefinition* QuestAliasData::Find(uint32_t id) const
{
  const auto it =
    std::find_if(aliases.begin(), aliases.end(),
                 [id](const auto& alias) { return alias.id == id; });
  return it == aliases.end() ? nullptr : &*it;
}

QuestAliasData QuestAliasData::Parse(const std::vector<Field>& fields)
{
  static const std::set<std::string> numeric = {
    "FNAM", "ALFI", "ALFR", "ALUA", "ALFA", "ALRT", "ALEQ", "ALEA",
    "ALCO", "ALCL", "ALNA", "ALNT", "ALFE", "ALFD", "ALFL", "KNAM",
    "KSIZ", "COCT", "SPOR", "OCOR", "GWOR", "ECOR", "ALDN", "VTCK"
  };
  static const std::set<std::string> repeated = { "CTDA", "CIS1", "CIS2",
                                                  "CNTO", "COED", "ALSP",
                                                  "ALFC", "ALPC" };
  QuestAliasData result;
  std::set<uint32_t> ids;
  QuestAliasDefinition* current = nullptr;
  size_t totalBytes = 0;
  for (const auto& field : fields) {
    const auto& tag = field.tag;
    const auto& bytes = field.bytes;
    if (tag.size() != 4 || bytes.size() > 1024 * 1024 ||
        (totalBytes += bytes.size()) > 64 * 1024 * 1024)
      throw std::runtime_error("Invalid quest alias field buffer");
    if (tag == "ALST" || tag == "ALLS") {
      if (current)
        throw std::runtime_error("Nested quest alias");
      const auto id = Number<uint32_t>(bytes);
      if (id > INT32_MAX || !ids.insert(id).second || ids.size() > 65536)
        throw std::runtime_error("Invalid or duplicate quest alias ID");
      result.aliases.emplace_back();
      current = &result.aliases.back();
      current->id = id;
      current->kind = tag == "ALST" ? QuestAliasDefinition::Kind::Reference
                                    : QuestAliasDefinition::Kind::Location;
      continue;
    }
    if (tag == "ALED") {
      if (!current || !bytes.empty())
        throw std::runtime_error("Invalid quest alias end marker");
      Finish(*current);
      current = nullptr;
      continue;
    }
    if (!current)
      continue; // FNAM/CTDA/etc also occur in quest stages and objectives.
    if (current->Has(tag) && !repeated.count(tag))
      throw std::runtime_error("Duplicate quest alias field: " + tag);
    if (numeric.count(tag)) {
      const auto value = Number<uint32_t>(bytes);
      if (tag == "FNAM")
        current->flags = value;
    } else if (tag == "ALID" || tag == "CIS1" || tag == "CIS2") {
      const auto value = String(bytes);
      if (tag == "ALID")
        current->name = value;
    } else if (tag == "ALCA") {
      if (bytes.size() != 4)
        throw std::runtime_error("Invalid alias creation descriptor");
      QuestAliasDefinition::Creation value;
      std::memcpy(&value.alias, bytes.data(), 2);
      std::memcpy(&value.flags, bytes.data() + 2, 2);
      current->creation = value;
    } else if (tag == "CTDA") {
      current->conditions.push_back(Number<CTDA>(bytes));
    } else if (tag == "ALSP" || tag == "ALFC" || tag == "ALPC") {
      Number<uint32_t>(bytes);
    } else if (tag == "KWDA") {
      if (bytes.size() % 4)
        throw std::runtime_error("Invalid alias keyword array");
    } else if (tag == "CNTO" || tag == "COED") {
      if (bytes.size() != (tag == "CNTO" ? 8 : 12))
        throw std::runtime_error("Invalid alias inventory entry");
    } else {
      current->unknownFields.push_back(tag);
    }
    current->fields[tag].push_back(bytes);
  }
  if (current)
    throw std::runtime_error("Unterminated quest alias");
  return result;
}

QuestAliasData QuestAliasData::Read(const RecordHeader* quest,
                                    CompressedFieldsCache& cache)
{
  if (!quest || quest->GetType() != "QUST")
    throw std::invalid_argument("Expected quest record for aliases");
  std::vector<Field> fields;
  bool inside = false;
  RecordHeaderAccess::IterateFields(
    quest,
    [&](const char* tag, uint32_t size, const char* data) {
      const std::string name(tag, 4);
      if (name == "ALST" || name == "ALLS" || name == "ALED" || inside) {
        if (size > 1024 * 1024)
          throw std::runtime_error("Quest alias field too large");
        fields.push_back({ name, std::vector<uint8_t>(data, data + size) });
      }
      if (name == "ALST" || name == "ALLS")
        inside = true;
      if (name == "ALED")
        inside = false;
    },
    cache);
  return Parse(fields);
}
}
