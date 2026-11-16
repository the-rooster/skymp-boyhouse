#include "LeveledListUtils.h"
#include "libespm/Loader.h"
#include "libespm/espm.h"
#include <algorithm>
#include <atomic>
#include <catch2/catch_all.hpp>
#include <cstring>
#include <limits>
#include <numeric>
#include <spdlog/spdlog.h>
#include <thread>

namespace {
using Bytes = std::vector<uint8_t>;
template <class T>
void Append(Bytes& bytes, T value)
{
  const auto begin = reinterpret_cast<const uint8_t*>(&value);
  bytes.insert(bytes.end(), begin, begin + sizeof(T));
}
Bytes Field(const char* tag, const Bytes& value)
{
  Bytes out(tag, tag + 4);
  Append(out, static_cast<uint16_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
  return out;
}
template <class T>
Bytes Number(const char* tag, T value)
{
  Bytes out;
  Append(out, value);
  return Field(tag, out);
}
void Join(Bytes& to, const Bytes& from)
{
  to.insert(to.end(), from.begin(), from.end());
}
Bytes Record(const char* kind, uint32_t id, const Bytes& fields)
{
  Bytes out(kind, kind + 4);
  Append(out, static_cast<uint32_t>(fields.size()));
  Append(out, uint32_t(0));
  Append(out, id);
  Append(out, uint32_t(0));
  Append(out, uint16_t(44));
  Append(out, uint16_t(0));
  Join(out, fields);
  return out;
}
struct ListEntry
{
  uint16_t level;
  uint32_t form;
  uint16_t count;
};
Bytes List(std::initializer_list<ListEntry> entries, uint8_t flags = 0,
           uint8_t chance = 0, uint32_t global = 0)
{
  auto out = Number("LVLF", flags);
  Join(out, Number("LVLD", chance));
  Join(out, Number("LLCT", static_cast<uint8_t>(entries.size())));
  if (global)
    Join(out, Number("LVLG", global));
  for (const auto& entry : entries) {
    Bytes data;
    Append(data, entry.level);
    Append(data, uint16_t(0x1234));
    Append(data, entry.form);
    Append(data, entry.count);
    Append(data, uint16_t(0x5678));
    Join(out, Field("LVLO", data));
  }
  return out;
}
struct SyntheticLists
{
  Bytes data = Record("TES4", 0, {});
  std::unique_ptr<espm::Browser> source;
  std::unique_ptr<espm::CombineBrowser> browser;
  void Add(const char* type, uint32_t id, const Bytes& fields = {})
  {
    Join(data, Record(type, id, fields));
  }
  void Finish()
  {
    source = std::make_unique<espm::Browser>(data.data(), data.size());
    espm::Combiner combine;
    combine.AddSource(source.get(), "Fixture.esm");
    browser = combine.Combine();
  }
  auto Expand(uint32_t id, uint32_t count = 1, uint32_t level = 1,
              uint8_t* chance = nullptr,
              const LeveledListUtils::Options* options = nullptr)
  {
    return LeveledListUtils::EvaluateListRecurse(
      *browser, browser->LookupById(id), count, level, chance, options);
  }
};
}

TEST_CASE("Leveled entries retain nested counts and ignore unused padding",
          "[LeveledAllocation]")
{
  SyntheticLists f;
  f.Add("MISC", 1);
  f.Add("MISC", 2);
  f.Add("LVLI", 11, List({ { 1, 1, 4 }, { 1, 2, 2 } }, 4));
  f.Add("LVLI", 10, List({ { 1, 11, 3 } }));
  f.Finish();
  const auto result = f.Expand(10, 5);
  REQUIRE(result == std::map<uint32_t, uint32_t>{ { 1, 60 }, { 2, 30 } });
  REQUIRE(f.Expand(10, 0).empty());
  REQUIRE_THROWS(f.Expand(10, std::numeric_limits<uint32_t>::max()));
}

TEST_CASE(
  "Level selection distinguishes the nearest tier from all lower tiers",
  "[LeveledAllocation]")
{
  SyntheticLists f;
  f.Add("MISC", 1);
  f.Add("MISC", 2);
  f.Add("MISC", 3);
  f.Add("LVLI", 10, List({ { 1, 1, 1 }, { 5, 2, 1 }, { 20, 3, 1 } }));
  f.Add("LVLI", 11, List({ { 1, 1, 1 }, { 5, 2, 1 }, { 20, 3, 1 } }, 3));
  f.Finish();
  REQUIRE(f.Expand(10, 50, 10) == std::map<uint32_t, uint32_t>{ { 2, 50 } });
  std::mt19937 rng(42);
  LeveledListUtils::Options options{ {}, &rng };
  const auto mixed = f.Expand(11, 1000, 10, nullptr, &options);
  REQUIRE(mixed.size() == 2);
  REQUIRE(mixed.at(1) + mixed.at(2) == 1000);
}

TEST_CASE(
  "Chance overrides survive every Each roll and do not override child lists",
  "[LeveledAllocation]")
{
  SyntheticLists f;
  f.Add("MISC", 1);
  f.Add("LVLI", 10, List({ { 1, 1, 1 } }, 2, 100));
  f.Add("LVLI", 11, List({ { 1, 10, 2 } }, 2, 100));
  f.Finish();
  uint8_t force = 0;
  REQUIRE(f.Expand(10, 100, 1, &force) ==
          std::map<uint32_t, uint32_t>{ { 1, 100 } });
  REQUIRE(f.Expand(11, 100, 1, &force).empty());
  REQUIRE_THROWS(f.Expand(10, 100001, 1, &force));
}

TEST_CASE(
  "Leveled chance globals use stored defaults or the supplied campaign value",
  "[LeveledAllocation]")
{
  SyntheticLists f;
  f.Add("MISC", 1);
  f.Add("GLOB", 30, Number("FLTV", 0.f));
  f.Add("LVLI", 10, List({ { 1, 1, 1 } }, 0, 100, 30));
  f.Finish();
  REQUIRE(f.Expand(10) == std::map<uint32_t, uint32_t>{ { 1, 1 } });
  LeveledListUtils::Options options;
  options.readGlobal = [](uint32_t id) {
    REQUIRE(id == 30);
    return 100.f;
  };
  REQUIRE(f.Expand(10, 1, 1, nullptr, &options).empty());
  options.readGlobal = [](uint32_t) {
    return std::numeric_limits<float>::infinity();
  };
  REQUIRE_THROWS(f.Expand(10, 1, 1, nullptr, &options));
}

TEST_CASE("Leveled evaluation rejects cycles and missing child records",
          "[LeveledAllocation]")
{
  SyntheticLists f;
  f.Add("LVLI", 10, List({ { 1, 11, 1 } }));
  SECTION("cycle")
  {
    f.Add("LVLI", 11, List({ { 1, 10, 1 } }));
  }
  SECTION("missing")
  {
  }
  f.Finish();
  REQUIRE_THROWS(f.Expand(10));
}

TEST_CASE("NPC template chains reject cycles and an empty leveled selection",
          "[LeveledAllocation]")
{
  SyntheticLists f;
  f.Add("NPC_", 10, Number("TPLT", uint32_t(11)));
  SECTION("cycle")
  {
    f.Add("NPC_", 11, Number("TPLT", uint32_t(10)));
  }
  SECTION("empty selection")
  {
    f.Add("NPC_", 12);
    f.Add("LVLN", 11, List({ { 1, 12, 1 } }, 0, 100));
  }
  f.Finish();
  REQUIRE_THROWS(LeveledListUtils::EvaluateTemplateChain(
    *f.browser, f.browser->LookupById(10), 1));
}

TEST_CASE("Leveled list reader handles interleaved ownership fields",
          "[LeveledAllocation]")
{
  auto fields = List({ { 1, 1, 2 }, { 2, 2, 3 } }, 4);
  // Insert COED between the LVLO fields, not after the list's declared count.
  fields.insert(fields.end() - 18, 18, 0);
  const auto extra = Field("COED", Bytes(12, 0));
  std::copy(extra.begin(), extra.end(), fields.end() - 36);
  SyntheticLists f;
  f.Add("MISC", 1);
  f.Add("MISC", 2);
  f.Add("LVLI", 10, fields);
  f.Finish();
  REQUIRE(f.Expand(10, 1, 2) ==
          std::map<uint32_t, uint32_t>{ { 1, 2 }, { 2, 3 } });
}

TEST_CASE("Leveled list reader rejects malformed and inconsistent fields",
          "[LeveledAllocation]")
{
  auto fields = List({ { 1, 1, 1 } });
  SECTION("count mismatch")
  {
    fields[20] = 2;
  }
  SECTION("duplicate count")
  {
    Join(fields, Number("LLCT", uint8_t(1)));
  }
  SECTION("truncated entry")
  {
    fields.pop_back();
    fields[25] = 11;
    fields[26] = 0;
  }
  SyntheticLists f;
  f.Add("MISC", 1);
  f.Add("LVLI", 10, fields);
  f.Finish();
  REQUIRE_THROWS(f.Expand(10));
}

extern espm::Loader& GetEspmLoader();

TEST_CASE(
  "Every owned winning leveled list parses independently of field adjacency",
  "[LeveledAllocation][espm]")
{
  const auto& browser = GetEspmLoader().GetBrowser();
  espm::CompressedFieldsCache cache;
  size_t count = 0;
  for (const auto* kind : { "LVLI", "LVLN", "LVSP" }) {
    for (const auto& source : browser.GetDistinctRecordsByType(kind)) {
      if (source.rec->IsDeleted())
        continue;
      INFO("list=" << source.ToGlobalId(source.rec->GetId()));
      const auto parsed =
        reinterpret_cast<const espm::LeveledListBase*>(source.rec)
          ->GetData(cache);
      REQUIRE(parsed.entries.size() == parsed.numEntries);
      ++count;
    }
  }
  REQUIRE(count > 4000);
}

using namespace std::chrono_literals;

TEST_CASE("Bug", "[espm]")
{
  espm::CompressedFieldsCache dummyCache;

  auto chestId = 0x774bf;

  std::atomic<bool> finished = false;

  std::jthread watchdog([&](std::stop_token stop) {
    auto was = std::chrono::system_clock::now();

    while (!finished.load() && !stop.stop_requested()) {
      std::this_thread::sleep_for(1ms);

      auto now = std::chrono::system_clock::now();
      if (now - was > 10s) {
        printf(
          "EvaluateListRecurse didn't finish for a single container after 10s "
          "of waiting. Consider it's an infinite loop\n");
        std::abort();
      }
    }
  });

  auto chest = espm::Convert<espm::CONT>(
    GetEspmLoader().GetBrowser().LookupById(chestId).rec);
  auto objects = chest->GetData(dummyCache).objects;
  std::vector<espm::LookupResult> leveled;
  for (auto obj : objects) {
    leveled.push_back(GetEspmLoader().GetBrowser().LookupById(obj.formId));
  }

  for (auto lvli : leveled) {
    for (int i = 0; i < 100; ++i) {
      const int pcLevel = 1, count = 1;
      auto result = LeveledListUtils::EvaluateListRecurse(
        GetEspmLoader().GetBrowser(), lvli, count, pcLevel, nullptr);
    }
  }
  finished = true;
}

TEST_CASE("Evaluate LItemFoodCabbage75", "[espm]")
{
  auto LItemFoodCabbage75 = 0x10bf6a;
  auto FoodCabbage = 0x64b3f;
  auto& br = GetEspmLoader().GetBrowser();
  auto leveledList = br.LookupById(LItemFoodCabbage75);

  int totalItems = 0;

  for (int i = 0; i < 100000; ++i) {
    auto res = LeveledListUtils::EvaluateList(br, leveledList);
    if (res.size() > 0) {
      REQUIRE(res.size() == 1);
      REQUIRE(res[0].formId == FoodCabbage);
      REQUIRE(res[0].count == 1);
      totalItems += res[0].count;
    }
  }

  REQUIRE(totalItems > 74000);
  REQUIRE(totalItems < 76000);
}

TEST_CASE("Evaluate recurse LItemFoodCabbage", "[espm]")
{
  auto LItemFoodCabbage = 0x10bf6e;
  auto FoodCabbage = 0x64b3f;
  auto& br = GetEspmLoader().GetBrowser();
  auto leveledList = br.LookupById(LItemFoodCabbage);

  for (int i = 0; i < 100; i++) {
    auto res = LeveledListUtils::EvaluateListRecurse(br, leveledList);
    REQUIRE(res.size() == 1);
    REQUIRE(res[FoodCabbage] >= 1);
    REQUIRE(res[FoodCabbage] <= 5);
  }

  // "Calculate for each" flag is not set
  auto r = LeveledListUtils::EvaluateListRecurse(br, leveledList, 10);
  REQUIRE(r.size() == 1);
  REQUIRE(r[FoodCabbage] % 10 == 0);
}

TEST_CASE("Evaluate LootGoldChange25", "[espm]")
{
  auto LootGoldChange25 = 0x4f78d;
  auto Gold001 = 0xf;
  auto& br = GetEspmLoader().GetBrowser();
  auto leveledList = br.LookupById(LootGoldChange25);

  int n[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

  for (int i = 0; i < 100000; ++i) {
    auto res = LeveledListUtils::EvaluateList(br, leveledList);
    if (res.size() == 0) {
      n[0]++;
    }
    if (res.size() > 0) {
      REQUIRE(res.size() == 1);
      REQUIRE(res[0].formId == Gold001);
      n[res[0].count]++;
    }
  }

  int initial_sum = 0;

  REQUIRE(std::accumulate(n, n + 10, initial_sum) == 100000);
  REQUIRE(abs(std::accumulate(n + 1, n + 10, initial_sum) / 9 - n[1]) <
          300); // difference between n[1] and avg(1..9) should be < 300
}

TEST_CASE("Evaluate DeathItemDraugr", "[espm]")
{
  auto DeathItemDraugr = 0x3ad7f;
  auto& br = GetEspmLoader().GetBrowser();
  auto leveledList = br.LookupById(DeathItemDraugr);

  auto res = LeveledListUtils::EvaluateList(br, leveledList);
  REQUIRE(res.size() == 5);
}

TEST_CASE("Evaluate LItemWeaponDaggerTown", "[espm]")
{
  auto LItemWeaponDaggerTown = 0x17177;
  auto& br = GetEspmLoader().GetBrowser();
  auto leveledList = br.LookupById(LItemWeaponDaggerTown);

  // Normally this leveled list returns one of daggers (iron, steel, etc)
  auto res = LeveledListUtils::EvaluateListRecurse(br, leveledList, 1);
  REQUIRE(res.size() == 1);

  // But if we increase count, we will get a lot of different daggers
  // The reason is "Each" flag is set
  res = LeveledListUtils::EvaluateListRecurse(br, leveledList, 1000);

  REQUIRE(res.size() == 5);
  REQUIRE(res[0x1397e] > 100);
  REQUIRE(res[0x13986] > 100);
  REQUIRE(res[0x1398e] > 30);
  REQUIRE(res[0x13996] > 30);
  REQUIRE(res[0x1399e] > 30);

  // PlayerCharacter's level is 1. Only IronDagger should be generated
  res = LeveledListUtils::EvaluateListRecurse(br, leveledList, 1000, 1);
  REQUIRE(res.size() == 1);
  REQUIRE(res[0x1397e] == 1000);
}

TEST_CASE("Evaluate LCharHorse", "[espm]")
{
  auto LCharHorse = 0x68d6f;
  auto& br = GetEspmLoader().GetBrowser();
  auto leveledList = br.LookupById(LCharHorse);

  std::map<uint32_t, uint32_t> results;

  for (int i = 0; i < 1000000; ++i) {
    auto res = LeveledListUtils::EvaluateListRecurse(br, leveledList, 1);
    REQUIRE(res.size() == 1);

    auto pair = *res.begin();
    results[pair.first] += pair.second;
    if (results.size() == 5) {
      break;
    }
  }

  REQUIRE(results.size() == 5);
  REQUIRE(results[0x68d6e] > 0);
  REQUIRE(results[0x68d6d] > 0);
  REQUIRE(results[0x68d6b] > 0);
  REQUIRE(results[0x68d5b] > 0);
  REQUIRE(results[0x68d07] > 0);
}

TEST_CASE("Evaluate LvlHorse template", "[espm]")
{
  auto LvlHorse = 0x68d71;
  auto& br = GetEspmLoader().GetBrowser();
  auto horse = br.LookupById(LvlHorse);

  std::map<uint32_t, uint32_t> results;

  for (int i = 0; i < 1000000; ++i) {
    auto res = LeveledListUtils::EvaluateTemplateChain(br, horse, 1);
    REQUIRE(res.size() == 2);
    REQUIRE(res[0] == LvlHorse);

    results[res[1]] += 1;
    if (results.size() == 5) {
      break;
    }
  }

  REQUIRE(results.size() == 5);
  REQUIRE(results[0x68d6e] > 0);
  REQUIRE(results[0x68d6d] > 0);
  REQUIRE(results[0x68d6b] > 0);
  REQUIRE(results[0x68d5b] > 0);
  REQUIRE(results[0x68d07] > 0);
}

TEST_CASE("Evaluate LvlMudcrab template", "[espm]")
{
  auto LvlMudcrab = 0x8cacc;
  auto& br = GetEspmLoader().GetBrowser();
  auto mudcrab = br.LookupById(LvlMudcrab);
  espm::CompressedFieldsCache cache;
  const auto list = espm::Convert<espm::LVLN>(br.LookupById(0x2183e).rec);
  REQUIRE(list);
  REQUIRE_FALSE(list->GetData(cache).leveledItemFlags &
                espm::LeveledListBase::AllLevels);

  // LCharMudcrab has two e4010 entries and one e4011 at level 1,
  // only e4011 at level 5, then 21875 at levels 10 and 15. Its all-levels
  // flag is OFF: the old 50/50 expectation at level 5 encoded a bug.
  std::mt19937 random(42);
  LeveledListUtils::Options options{ {}, &random };
  for (const auto level : { 1u, 5u, 10u, 15u }) {
    INFO("player level=" << level);
    std::map<uint32_t, uint32_t> counts;
    for (int i = 0; i < 100; ++i) {
      const auto chain =
        LeveledListUtils::EvaluateTemplateChain(br, mudcrab, level, &options);
      REQUIRE(chain.size() == 2);
      REQUIRE(chain[0] == 0x8cacc);
      ++counts[chain[1]];
    }
    if (level == 1) {
      REQUIRE(counts.size() == 2);
      REQUIRE(counts.at(0xe4010) > 0);
      REQUIRE(counts.at(0xe4011) > 0);
    } else {
      const auto expected = level == 5 ? 0xe4011u : 0x21875u;
      REQUIRE(counts == std::map<uint32_t, uint32_t>{ { expected, 100 } });
    }
  }
}
