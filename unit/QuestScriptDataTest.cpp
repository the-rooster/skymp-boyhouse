#include "libespm/QuestScriptData.h"
#include "CampaignReferenceScripts.h"
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

TEST_CASE(
  "Simple VMAD owns its data and refuses fragmented or truncated bindings",
  "[Quest][VMAD][ReferenceScripts]")
{
  for (uint16_t format : { 1, 2 }) {
    size_t baseSize;
    auto fixture = Fixture(format, baseSize);
    const auto parsed =
      espm::QuestScriptData::Parse(fixture.bytes.data(), baseSize, false);
    REQUIRE_FALSE(parsed.hasFragments);
    REQUIRE(parsed.scripts[0].properties[0].value.alias == 7);
    REQUIRE_THROWS(espm::QuestScriptData::Parse(fixture.bytes.data(),
                                                fixture.bytes.size(), false));
    for (size_t length = 0; length < baseSize; ++length)
      REQUIRE_THROWS(
        espm::QuestScriptData::Parse(fixture.bytes.data(), length, false));
    fixture.bytes.clear();
    REQUIRE(parsed.scripts[0].name == "FixtureQuest");
    REQUIRE(parsed.scripts[0].properties[1].value.elements[1].integer == 42);
  }
}

TEST_CASE(
  "VMAD retains both removed-script flags and payload-free removed properties",
  "[Quest][VMAD][ReferenceScripts]")
{
  for (uint8_t status : { 2, 3 }) {
    Bytes b;
    b.Put<uint16_t>(5);
    b.Put<uint16_t>(2);
    b.Put<uint16_t>(1);
    b.String("Removed");
    const auto scriptFlag = b.bytes.size();
    b.Put<uint8_t>(status);
    b.Put<uint16_t>(1);
    b.String("OldProperty");
    b.Put<uint8_t>(0);
    b.Put<uint8_t>(3);
    auto parsed =
      espm::QuestScriptData::Parse(b.bytes.data(), b.bytes.size(), false);
    REQUIRE(parsed.scripts[0].status == status);
    REQUIRE(parsed.scripts[0].properties[0].value.type == 0);
    b.bytes.back() = 1;
    REQUIRE_THROWS(
      espm::QuestScriptData::Parse(b.bytes.data(), b.bytes.size(), false));
    b.bytes.back() = 3;
    b.bytes[scriptFlag] = 4;
    REQUIRE_THROWS(
      espm::QuestScriptData::Parse(b.bytes.data(), b.bytes.size(), false));
  }
}

TEST_CASE("Every owned placed-reference VMAD has a complete simple binding",
          "[Quest][VMAD][ReferenceScripts]")
{
  auto& browser = GetEspmLoader().GetBrowser();
  const auto records = browser.GetDistinctRecordsByType("REFR");
  size_t scriptCount = 0;
  for (const auto& record : records) {
    INFO("reference=" << record.ToGlobalId(record.rec->GetId()));
    const auto data =
      espm::QuestScriptData::ReadSimple(record.rec, browser.GetCache());
    scriptCount += data.scripts.size();
  }
  REQUIRE(records.size() > 10000);
  REQUIRE(scriptCount > 1000);
}

namespace {
Bytes ReferenceBytes(uint32_t base, const Bytes& scriptData)
{
  Bytes fields;
  const auto field = [&](const char* tag, const Bytes& payload) {
    fields.bytes.insert(fields.bytes.end(), tag, tag + 4);
    fields.Put<uint16_t>(payload.bytes.size());
    fields.bytes.insert(fields.bytes.end(), payload.bytes.begin(),
                        payload.bytes.end());
  };
  Bytes name;
  name.Put(base);
  field("NAME", name);
  field("VMAD", scriptData);
  Bytes record;
  record.bytes.insert(record.bytes.end(), { 'R', 'E', 'F', 'R' });
  record.Put<uint32_t>(fields.bytes.size());
  record.Put<uint32_t>(0);
  record.Put<uint32_t>(0x02001234);
  record.Put<uint32_t>(0);
  record.Put<uint16_t>(44);
  record.Put<uint16_t>(0);
  record.bytes.insert(record.bytes.end(), fields.bytes.begin(),
                      fields.bytes.end());
  return record;
}
Bytes SimpleScript(const std::string& name, uint8_t status,
                   uint16_t properties)
{
  Bytes vmad;
  vmad.Put<uint16_t>(5);
  vmad.Put<uint16_t>(2);
  vmad.Put<uint16_t>(1);
  vmad.String(name);
  vmad.Put(status);
  vmad.Put(properties);
  return vmad;
}
std::vector<espm::QuestScript> ReferenceBindings(const Bytes& bytes)
{
  espm::Browser source(bytes.bytes.data(), bytes.bytes.size());
  PartOne server;
  server.AttachEspm(&GetEspmLoader());
  // Dragonborn's local index 2 maps to global index 4 in the five-master
  // order.
  espm::LookupResult reference(&GetEspmLoader().GetBrowser(),
                               source.LookupById(0x02001234), 4);
  return CampaignReferenceScripts::Read(server.worldState, reference);
}
}

TEST_CASE(
  "Reference layers remove inherited scripts and properties explicitly",
  "[Quest][ReferenceScripts]")
{
  SECTION("local and inherited removal flags detach the base script")
  {
    for (uint8_t status : { 2, 3 }) {
      const auto vmad = SimpleScript("defaultSetStageTRIGScript", status, 0);
      REQUIRE(ReferenceBindings(ReferenceBytes(0x33f50, vmad)).empty());
    }
  }
  SECTION("removed property clears the inherited binding")
  {
    auto vmad = SimpleScript("defaultSetStageTRIGSpecificActor", 1, 1);
    vmad.String("TriggerActor");
    vmad.Put<uint8_t>(0);
    vmad.Put<uint8_t>(3);
    const auto result = ReferenceBindings(ReferenceBytes(0x46eff, vmad));
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].properties.empty());
  }
}

TEST_CASE("Reference layers map object arrays through their source plugin",
          "[Quest][ReferenceScripts]")
{
  auto vmad = SimpleScript("defaultSetStageTRIGScript", 1, 1);
  vmad.String("Objects");
  vmad.Put<uint8_t>(11);
  vmad.Put<uint8_t>(1);
  vmad.Put<uint32_t>(3);
  vmad.Object(2, 0x02000042, 7);
  vmad.Object(2, 0x00000014, 0xffff);
  vmad.Object(2, 0, 0xffff);
  const auto result = ReferenceBindings(ReferenceBytes(0x33f50, vmad));
  REQUIRE(result.size() == 1);
  const auto& values = result[0].properties[0].value.elements;
  REQUIRE(values[0].formId == 0x04000042);
  REQUIRE(values[0].alias == 7);
  REQUIRE(values[1].formId == 0x14);
  REQUIRE(values[2].formId == 0);
}
