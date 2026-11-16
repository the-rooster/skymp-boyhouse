#include "libespm/QuestScriptData.h"
#include "TestUtils.hpp"
#include <cstring>

extern espm::Loader& GetEspmLoader();

namespace {
struct Bytes
{
  std::vector<uint8_t> bytes;
  template <class T>
  void Put(T value)
  {
    const auto start = bytes.size();
    bytes.resize(start + sizeof(T));
    std::memcpy(bytes.data() + start, &value, sizeof(T));
  }
  void String(const std::string& value)
  {
    Put<uint16_t>(value.size());
    bytes.insert(bytes.end(), value.begin(), value.end());
  }
  void Object(uint16_t format, uint32_t form, uint16_t alias)
  {
    if (format == 1) {
      Put(form);
      Put(alias);
      Put<uint16_t>(0);
    } else {
      Put<uint16_t>(0);
      Put(alias);
      Put(form);
    }
  }
};
Bytes Fixture(uint16_t format, size_t& baseSize)
{
  Bytes b;
  b.Put<uint16_t>(5);
  b.Put(format);
  b.Put<uint16_t>(1);
  b.String("FixtureQuest");
  b.Put<uint8_t>(0);
  b.Put<uint16_t>(2);
  b.String("AliasProperty");
  b.Put<uint8_t>(1);
  b.Put<uint8_t>(1);
  b.Object(format, 0x12345, 7);
  b.String("Counts");
  b.Put<uint8_t>(13);
  b.Put<uint8_t>(1);
  b.Put<uint32_t>(2);
  b.Put<int32_t>(-8);
  b.Put<int32_t>(42);
  baseSize = b.bytes.size();
  b.Put<uint8_t>(2);
  b.Put<uint16_t>(1);
  b.String("FixtureQuest");
  b.Put<uint32_t>(65535);
  b.Put<uint32_t>(3);
  b.Put<uint8_t>(1);
  b.String("FixtureQuest");
  b.String("Fragment_3");
  b.Put<uint16_t>(1);
  b.Object(format, 0x12345, 7);
  b.Put<uint16_t>(5);
  b.Put<uint16_t>(2);
  b.Put<uint16_t>(1);
  b.String("AliasScript");
  b.Put<uint8_t>(0);
  b.Put<uint16_t>(0);
  return b;
}
}

TEST_CASE(
  "Quest VMAD retains fragment and alias bindings in both object formats",
  "[Quest][VMAD]")
{
  for (uint16_t format : { 1, 2 }) {
    size_t baseSize;
    const auto b = Fixture(format, baseSize);
    const auto parsed =
      espm::QuestScriptData::Parse(b.bytes.data(), b.bytes.size());
    REQUIRE(parsed.scripts.at(0).properties.at(0).value.formId == 0x12345);
    REQUIRE(parsed.scripts.at(0).properties.at(0).value.alias == 7);
    REQUIRE(
      parsed.scripts.at(0).properties.at(1).value.elements.at(0).integer ==
      -8);
    REQUIRE(parsed.fragments.at(0).stage == 65535);
    REQUIRE(parsed.fragments.at(0).logEntry == 3);
    REQUIRE(parsed.fragments.at(0).function == "Fragment_3");
    REQUIRE(parsed.aliases.at(0).object.alias == 7);
    REQUIRE(parsed.aliases.at(0).scripts.at(0).name == "AliasScript");
    REQUIRE_FALSE(
      espm::QuestScriptData::Parse(b.bytes.data(), baseSize).hasFragments);
  }
}

TEST_CASE("Quest VMAD rejects every truncated field and unsupported layout",
          "[Quest][VMAD]")
{
  size_t baseSize;
  auto b = Fixture(2, baseSize);
  for (size_t size = 0; size < b.bytes.size(); ++size) {
    if (size == baseSize)
      continue; // optional fragment tail may be absent
    INFO("prefix size=" << size);
    REQUIRE_THROWS(espm::QuestScriptData::Parse(b.bytes.data(), size));
  }
  auto bad = b.bytes;
  bad.push_back(0);
  REQUIRE_THROWS(espm::QuestScriptData::Parse(bad.data(), bad.size()));
  bad = b.bytes;
  bad[0] = 99;
  REQUIRE_THROWS(espm::QuestScriptData::Parse(bad.data(), bad.size()));
  bad = b.bytes;
  bad[2] = 3;
  REQUIRE_THROWS(espm::QuestScriptData::Parse(bad.data(), bad.size()));
  bad = b.bytes;
  bad[baseSize] = 9;
  REQUIRE_THROWS(espm::QuestScriptData::Parse(bad.data(), bad.size()));
}

TEST_CASE("Every owned QUST VMAD parses without losing its tail",
          "[Quest][VMAD]")
{
  auto& loader = GetEspmLoader();
  const auto records = loader.GetBrowser().GetDistinctRecordsByType("QUST");
  size_t fragmentCount = 0, aliasCount = 0;
  for (const auto& record : records) {
    INFO("quest=" << record.ToGlobalId(record.rec->GetId()));
    const auto data =
      espm::QuestScriptData::Read(record.rec, loader.GetBrowser().GetCache());
    fragmentCount += data.fragments.size();
    aliasCount += data.aliases.size();
    for (const auto& fragment : data.fragments) {
      REQUIRE_FALSE(fragment.script.empty());
      REQUIRE_FALSE(fragment.function.empty());
    }
  }
  REQUIRE(records.size() >= 1800);
  REQUIRE(fragmentCount > 1000);
  REQUIRE(aliasCount > 1000);
}
