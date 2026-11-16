#include "libespm/QuestAliasData.h"
#include "TestUtils.hpp"
#include <cstring>

extern espm::Loader& GetEspmLoader();

namespace {
using Field = espm::QuestAliasData::Field;
template <class T>
Field Number(const char* tag, T value)
{
  Field field{ tag, std::vector<uint8_t>(sizeof(T)) };
  std::memcpy(field.bytes.data(), &value, sizeof(T));
  return field;
}
Field Text(const char* tag, const std::string& value)
{
  Field field{ tag, std::vector<uint8_t>(value.begin(), value.end()) };
  field.bytes.push_back(0);
  return field;
}
std::vector<Field> Alias()
{
  return { Number<uint32_t>("ALST", 7),
           Text("ALID", "Target"),
           Number<uint32_t>("FNAM", 0x802),
           Number<uint32_t>("ALCO", 0x42),
           Number<uint32_t>("ALCA", 0x8000ffff),
           Number<uint32_t>("ALCL", 4),
           { "ALED", {} } };
}
}

TEST_CASE("Owned alias metadata preserves signed IDs, creation flags and kind",
          "[Quest][QuestAliases]")
{
  auto fields = Alias();
  fields.insert(fields.end(),
                { Number<uint32_t>("ALLS", 8),
                  Text("ALID", "Location"),
                  Number<uint32_t>("FNAM", 2),
                  Number<int32_t>("ALFA", -1),
                  Number<uint32_t>("KNAM", 0x123),
                  { "ALED", {} } });
  auto parsed = espm::QuestAliasData::Parse(fields);
  fields.clear();
  REQUIRE(parsed.aliases.size() == 2);
  const auto* reference = parsed.Find(7);
  REQUIRE(reference);
  REQUIRE(reference->kind == espm::QuestAliasDefinition::Kind::Reference);
  REQUIRE(reference->name == "Target");
  REQUIRE(reference->flags == 0x802); // bit 11 is Forced By Aliases.
  REQUIRE(reference->creation->alias == -1);
  REQUIRE(reference->creation->flags == 0x8000);
  REQUIRE(reference->Unsigned("ALCO") == 0x42);
  REQUIRE(parsed.Find(8)->kind == espm::QuestAliasDefinition::Kind::Location);
  REQUIRE(parsed.Find(8)->Signed("ALFA") == -1);
  REQUIRE_FALSE(parsed.Find(99));
  REQUIRE_FALSE(reference->Unsigned("ALFR"));
}

TEST_CASE(
  "Alias parser bounds checks every singleton and retains unknown semantics",
  "[Quest][QuestAliases]")
{
  const auto original = Alias();
  for (size_t field = 0; field < original.size() - 1; ++field) {
    for (size_t size = 0; size < original[field].bytes.size(); ++size) {
      auto malformed = original;
      malformed[field].bytes.resize(size);
      INFO("field=" << field << " size=" << size);
      REQUIRE_THROWS(espm::QuestAliasData::Parse(malformed));
    }
  }
  auto fields = original;
  fields.insert(fields.end() - 1, Field{ "FUTR", { 1, 2, 3 } });
  const auto parsed = espm::QuestAliasData::Parse(fields);
  REQUIRE(parsed.Find(7)->unknownFields == std::vector<std::string>{ "FUTR" });
  REQUIRE(parsed.Find(7)->fields.at("FUTR")[0].size() == 3);
}

TEST_CASE(
  "Alias parser rejects ambiguous structure and inconsistent counted fields",
  "[Quest][QuestAliases]")
{
  auto fields = Alias();
  SECTION("unterminated")
  {
    fields.pop_back();
  }
  SECTION("nested")
  {
    fields.insert(fields.begin() + 1, fields.front());
  }
  SECTION("duplicate ID")
  {
    const auto other = fields;
    fields.insert(fields.end(), other.begin(), other.end());
  }
  SECTION("duplicate scalar")
  {
    fields.insert(fields.end() - 1, Number<uint32_t>("FNAM", 1));
  }
  SECTION("missing name")
  {
    fields.erase(fields.begin() + 1);
  }
  SECTION("missing flags")
  {
    fields.erase(fields.begin() + 2);
  }
  SECTION("orphan end")
  {
    fields.insert(fields.begin(), Field{ "ALED", {} });
  }
  SECTION("nonempty end")
  {
    fields.back().bytes.push_back(0);
  }
  SECTION("bad keyword count")
  {
    fields.insert(fields.end() - 1, Number<uint32_t>("KSIZ", 2));
  }
  SECTION("keyword array without count")
  {
    fields.insert(fields.end() - 1, Number<uint32_t>("KWDA", 42));
  }
  SECTION("bad inventory count")
  {
    fields.insert(fields.end() - 1, Number<uint32_t>("COCT", 1));
  }
  REQUIRE_THROWS(espm::QuestAliasData::Parse(fields));
}

TEST_CASE("Alias conditions and repeated effects retain owned data",
          "[Quest][QuestAliases]")
{
  auto fields = Alias();
  espm::CTDA condition{};
  condition.functionIndex = 58;
  condition.runOnType = espm::CTDA::RunOnTypeFlags::QuestAlias;
  fields.insert(fields.end() - 1,
                { Number("CTDA", condition),
                  Number<uint32_t>("KSIZ", 1),
                  Number<uint32_t>("KWDA", 0x123),
                  Number<uint32_t>("ALSP", 1),
                  Number<uint32_t>("ALSP", 2),
                  Number<uint32_t>("COCT", 1),
                  { "CNTO", { 0x42, 0, 0, 0, 2, 0, 0, 0 } } });
  const auto parsed = espm::QuestAliasData::Parse(fields);
  REQUIRE(parsed.Find(7)->conditions.at(0).functionIndex == 58);
  REQUIRE(parsed.Find(7)->fields.at("ALSP").size() == 2);
  REQUIRE(parsed.Find(7)->fields.at("CNTO")[0][4] == 2);
}

TEST_CASE("Every owned quest alias decodes with distinct IDs and owned fields",
          "[Quest][QuestAliases]")
{
  auto& browser = GetEspmLoader().GetBrowser();
  const auto records = browser.GetDistinctRecordsByType("QUST");
  size_t references = 0, locations = 0, conditions = 0;
  for (const auto& record : records) {
    INFO("quest=" << record.ToGlobalId(record.rec->GetId()));
    const auto data =
      espm::QuestAliasData::Read(record.rec, browser.GetCache());
    for (const auto& alias : data.aliases) {
      REQUIRE(data.Find(alias.id) == &alias);
      REQUIRE(alias.Has("ALID"));
      REQUIRE(alias.Has("FNAM"));
      conditions += alias.conditions.size();
      if (alias.kind == espm::QuestAliasDefinition::Kind::Reference)
        ++references;
      else
        ++locations;
    }
  }
  REQUIRE(records.size() >= 1800);
  REQUIRE(references > 1000);
  REQUIRE(locations > 100);
  REQUIRE(conditions > 1000);
}

TEST_CASE("Legacy alias inventory can omit its optional count field",
          "[Quest][QuestAliases]")
{
  auto fields = Alias();
  fields.insert(fields.end() - 1,
                Field{ "CNTO", { 0x42, 0, 0, 0, 1, 0, 0, 0 } });
  const auto parsed = espm::QuestAliasData::Parse(fields);
  REQUIRE(parsed.Find(7)->fields.at("CNTO").size() == 1);
  REQUIRE_FALSE(parsed.Find(7)->Has("COCT"));
}
