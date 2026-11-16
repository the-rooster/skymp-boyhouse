#include "LeveledListUtils.h"
#include "libespm/RecordHeaderAccess.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>

namespace {
bool IsLeveledType(const espm::LookupResult& record)
{
  return record.rec &&
    (record.rec->GetType() == "LVLI" || record.rec->GetType() == "LVLN" ||
     record.rec->GetType() == "LVSP");
}

std::mt19937& Random(const LeveledListUtils::Options* options)
{
  if (options && options->random)
    return *options->random;
  thread_local std::mt19937 random(std::random_device{}());
  return random;
}

float GlobalDefault(const espm::CombineBrowser& browser, uint32_t id,
                    espm::CompressedFieldsCache& cache)
{
  const auto record = browser.LookupById(id);
  if (!record.rec || record.rec->IsDeleted() ||
      record.rec->GetType() != "GLOB")
    throw std::runtime_error("Invalid leveled list chance global");
  std::optional<float> result;
  espm::RecordHeaderAccess::IterateFields(
    record.rec,
    [&](const char* tag, uint32_t size, const char* data) {
      if (std::memcmp(tag, "FLTV", 4))
        return;
      if (result || size != sizeof(float))
        throw std::runtime_error("Malformed leveled list chance global");
      float value;
      std::memcpy(&value, data, sizeof(value));
      result = value;
    },
    cache);
  if (!result)
    throw std::runtime_error("Missing leveled list chance global");
  return *result;
}

uint32_t Product(uint32_t left, uint32_t right)
{
  const auto product = uint64_t(left) * right;
  if (product > std::numeric_limits<uint32_t>::max())
    throw std::overflow_error("Leveled list count overflow");
  return static_cast<uint32_t>(product);
}

void Add(std::map<uint32_t, uint32_t>& output, uint32_t id, uint32_t count)
{
  if (!count)
    return;
  auto& previous = output[id];
  if (count > std::numeric_limits<uint32_t>::max() - previous)
    throw std::overflow_error("Leveled list count overflow");
  previous += count;
}

class Evaluation
{
public:
  Evaluation(const espm::CombineBrowser& browser_, uint32_t level_,
             const LeveledListUtils::Options* options_)
    : browser(browser_)
    , level(level_)
    , options(options_)
  {
  }

  std::vector<LeveledListUtils::Entry> Select(const espm::LookupResult& record,
                                              uint8_t* chanceOverride)
  {
    if (!IsLeveledType(record))
      return {};
    const auto& data = Data(record);
    float chance = data.chanceNone;
    if (chanceOverride) {
      chance = *chanceOverride;
    } else if (data.chanceNoneGlobalId) {
      const auto id = record.ToGlobalId(data.chanceNoneGlobalId);
      const auto global = browser.LookupById(id);
      if (!global.rec || global.rec->IsDeleted() ||
          global.rec->GetType() != "GLOB")
        throw std::runtime_error("Invalid leveled list chance global");
      chance = options && options->readGlobal
        ? options->readGlobal(id)
        : GlobalDefault(browser, id, cache);
    }
    if (!std::isfinite(chance))
      throw std::runtime_error("Non-finite leveled list chance");
    chance = std::clamp(chance, 0.f, 100.f);
    if (chance >= 100.f ||
        (chance > 0.f &&
         std::uniform_real_distribution<float>(0.f, 100.f)(Random(options)) <
           chance))
      return {};

    std::vector<const espm::LeveledListBase::Entry*> eligible;
    uint32_t highest = 0;
    for (const auto& entry : data.entries) {
      if (!level || entry.level <= level) {
        highest = std::max(highest, entry.level);
        eligible.push_back(&entry);
      }
    }
    const bool useAll = data.leveledItemFlags & espm::LeveledListBase::UseAll;
    if (level && !useAll &&
        !(data.leveledItemFlags & espm::LeveledListBase::AllLevels)) {
      std::erase_if(
        eligible, [&](const auto* entry) { return entry->level != highest; });
    }
    if (!useAll && !eligible.empty()) {
      const auto chosen = eligible[std::uniform_int_distribution<size_t>(
        0, eligible.size() - 1)(Random(options))];
      eligible.assign(1, chosen);
    }
    std::vector<LeveledListUtils::Entry> selected;
    for (const auto* entry : eligible) {
      if (!entry->formId)
        throw std::runtime_error("Null leveled list entry");
      if (entry->count)
        selected.push_back({ record.ToGlobalId(entry->formId), entry->count });
    }
    return selected;
  }

  std::map<uint32_t, uint32_t> Expand(const espm::LookupResult& record,
                                      uint32_t count, uint8_t* chanceOverride,
                                      size_t depth = 0)
  {
    if (!count)
      return {};
    if (!record.rec || record.rec->IsDeleted())
      throw std::runtime_error("Missing leveled list entry record");
    if (depth >= 128 || ++work > 100000)
      throw std::runtime_error("Leveled list expansion budget exceeded");
    const auto id = record.ToGlobalId(record.rec->GetId());
    if (!IsLeveledType(record))
      return { { id, count } };
    if (!active.insert(id).second)
      throw std::runtime_error("Cyclic leveled list");
    const auto& data = Data(record);
    const bool each = data.leveledItemFlags & espm::LeveledListBase::Each;
    const uint32_t rolls = each ? count : 1;
    if (rolls > 100000 - work)
      throw std::runtime_error("Leveled list expansion budget exceeded");
    std::map<uint32_t, uint32_t> result;
    for (uint32_t roll = 0; roll < rolls; ++roll) {
      if (++work > 100000)
        throw std::runtime_error("Leveled list expansion budget exceeded");
      for (const auto& selected : Select(record, chanceOverride)) {
        // The parent's LVLO count belongs to the child list too. Overrides
        // apply only to this root, including every Each roll, not descendants.
        for (const auto& [child, amount] :
             Expand(browser.LookupById(selected.formId), selected.count,
                    nullptr, depth + 1))
          Add(result, child, Product(amount, each ? 1 : count));
      }
    }
    active.erase(id);
    return result;
  }

private:
  const espm::LeveledListBase::Data& Data(const espm::LookupResult& record)
  {
    if (record.rec->IsDeleted())
      throw std::runtime_error("Deleted leveled list");
    const auto found = parsed.find(record.rec);
    if (found != parsed.end())
      return found->second;
    return parsed
      .emplace(record.rec,
               reinterpret_cast<const espm::LeveledListBase*>(record.rec)
                 ->GetData(cache))
      .first->second;
  }
  const espm::CombineBrowser& browser;
  uint32_t level;
  const LeveledListUtils::Options* options;
  espm::CompressedFieldsCache cache;
  std::map<const espm::RecordHeader*, espm::LeveledListBase::Data> parsed;
  std::set<uint32_t> active;
  size_t work = 0;
};
}

std::vector<LeveledListUtils::Entry> LeveledListUtils::EvaluateList(
  const espm::CombineBrowser& browser, const espm::LookupResult& record,
  uint32_t level, uint8_t* chanceOverride, const Options* options)
{
  return Evaluation(browser, level, options).Select(record, chanceOverride);
}

std::map<uint32_t, uint32_t> LeveledListUtils::EvaluateListRecurse(
  const espm::CombineBrowser& browser, const espm::LookupResult& record,
  uint32_t count, uint32_t level, uint8_t* chanceOverride,
  const Options* options)
{
  return Evaluation(browser, level, options)
    .Expand(record, count, chanceOverride);
}

std::vector<uint32_t> LeveledListUtils::EvaluateTemplateChain(
  const espm::CombineBrowser& browser, const espm::LookupResult& head,
  uint32_t level, const Options* options)
{
  uint32_t cursorId = head.rec ? head.ToGlobalId(head.rec->GetId()) : 0;
  std::set<uint32_t> seen;
  std::vector<uint32_t> result;
  espm::CompressedFieldsCache cache;
  while (true) {
    const auto cursor = browser.LookupById(cursorId);
    const auto* npc = espm::Convert<espm::NPC_>(cursor.rec);
    if (!npc || cursor.rec->IsDeleted())
      throw std::runtime_error("Invalid NPC template record");
    const auto id = cursor.ToGlobalId(cursor.rec->GetId());
    if (!seen.insert(id).second || result.size() >= 128)
      throw std::runtime_error("Cyclic or excessive NPC template chain");
    result.push_back(id);
    const auto data = npc->GetData(cache);
    if (!data.baseTemplate)
      return result;
    cursorId = cursor.ToGlobalId(data.baseTemplate);
    const auto next = browser.LookupById(cursorId);
    if (next.rec && next.rec->GetType() == "LVLN") {
      const auto selected =
        EvaluateListRecurse(browser, next, 1, level, nullptr, options);
      if (selected.size() != 1 || selected.begin()->second != 1)
        throw std::runtime_error(
          "NPC template must resolve to exactly one actor");
      cursorId = selected.begin()->first;
    }
  }
}
