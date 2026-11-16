#include "CampaignReferenceScripts.h"
#include "WorldState.h"
#include "libespm/RecordHeaderAccess.h"
#include <algorithm>
#include <cstring>
#include <optional>
#include <set>
#include <stdexcept>

namespace {
std::string Lower(std::string value)
{
  for (auto& c : value)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return value;
}

void MapIds(espm::QuestScriptValue& value, const espm::LookupResult& source)
{
  if (value.type == 1 && value.formId)
    value.formId = source.ToGlobalId(value.formId);
  for (auto& element : value.elements)
    MapIds(element, source);
}

void Merge(std::vector<espm::QuestScript>& result,
           const espm::LookupResult& source, WorldState& world)
{
  const auto data =
    espm::QuestScriptData::ReadSimple(source.rec, world.GetEspmCache());
  std::set<std::string> seen;
  for (const auto& script : data.scripts) {
    const auto name = Lower(script.name);
    if (name.empty() || !seen.insert(name).second)
      throw std::runtime_error(
        "Invalid or duplicate reference script binding");
    auto existing =
      std::find_if(result.begin(), result.end(), [&](const auto& entry) {
        return Lower(entry.name) == name;
      });
    if (script.status & 2) {
      if (existing != result.end())
        result.erase(existing);
      continue;
    }
    if (existing == result.end()) {
      result.push_back({ script.name, 0, {} });
      existing = result.end() - 1;
    }
    std::set<std::string> properties;
    for (auto property : script.properties) {
      const auto propertyName = Lower(property.name);
      if (propertyName.empty() || !properties.insert(propertyName).second)
        throw std::runtime_error("Invalid or duplicate reference property");
      auto previous = std::find_if(
        existing->properties.begin(), existing->properties.end(),
        [&](const auto& entry) { return Lower(entry.name) == propertyName; });
      if (property.status == 3) {
        if (previous != existing->properties.end())
          existing->properties.erase(previous);
        continue;
      }
      MapIds(property.value, source);
      if (previous == existing->properties.end())
        existing->properties.push_back(std::move(property));
      else
        *previous = std::move(property);
    }
  }
}
}

std::vector<espm::QuestScript> CampaignReferenceScripts::Read(
  WorldState& world, const espm::LookupResult& reference)
{
  if (!reference.rec || reference.rec->IsDeleted() ||
      (reference.rec->GetType() != "REFR" &&
       reference.rec->GetType() != "ACHR"))
    throw std::invalid_argument("Expected an owned placed reference");
  std::optional<uint32_t> baseId;
  espm::RecordHeaderAccess::IterateFields(
    reference.rec,
    [&](const char* tag, uint32_t size, const char* bytes) {
      if (std::memcmp(tag, "NAME", 4))
        return;
      if (baseId || size != 4)
        throw std::runtime_error("Malformed reference script base");
      uint32_t raw;
      std::memcpy(&raw, bytes, 4);
      baseId = reference.ToGlobalId(raw);
    },
    world.GetEspmCache());
  if (!baseId || !*baseId)
    throw std::runtime_error("Reference scripts have no base record");
  const auto& browser = world.GetEspm().GetBrowser();
  auto sourceId = *baseId;
  std::set<uint32_t> templates;
  while (true) {
    const auto source = browser.LookupById(sourceId);
    if (!source.rec || source.rec->IsDeleted())
      throw std::runtime_error("Missing reference script base/template");
    if (source.rec->GetType() == "LVLN")
      throw std::runtime_error(
        "Reference scripts require campaign leveled actor template selection");
    if (source.rec->GetType() != "NPC_")
      break;
    const auto id = source.ToGlobalId(source.rec->GetId());
    if (!templates.insert(id).second || templates.size() > 128)
      throw std::runtime_error(
        "Reference script template cycle/depth exceeded");
    std::optional<uint32_t> parent;
    std::optional<uint16_t> flags;
    espm::RecordHeaderAccess::IterateFields(
      source.rec,
      [&](const char* tag, uint32_t size, const char* bytes) {
        if (!std::memcmp(tag, "TPLT", 4)) {
          if (parent || size != 4)
            throw std::runtime_error("Malformed NPC script template");
          uint32_t raw;
          std::memcpy(&raw, bytes, 4);
          parent = raw ? source.ToGlobalId(raw) : 0;
        } else if (!std::memcmp(tag, "ACBS", 4)) {
          if (flags || size != 24)
            throw std::runtime_error("Malformed NPC script template flags");
          uint16_t raw;
          std::memcpy(&raw, bytes + 18, 2);
          flags = raw;
        }
      },
      world.GetEspmCache());
    if (!flags)
      throw std::runtime_error("NPC script template has no flags");
    if (!parent.value_or(0) || !(*flags & 0x200))
      break;
    const auto next = browser.LookupById(*parent);
    if (next.rec && next.rec->GetType() != "NPC_" &&
        next.rec->GetType() != "LVLN")
      throw std::runtime_error("NPC script template target has wrong type");
    sourceId = *parent;
  }
  std::vector<espm::QuestScript> result;
  Merge(result, browser.LookupById(sourceId), world);
  Merge(result, reference, world);
  return result;
}
