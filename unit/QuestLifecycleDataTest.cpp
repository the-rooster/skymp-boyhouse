#include "libespm/QuestLifecycleData.h"
#include "libespm/RecordHeaderAccess.h"
#include "TestUtils.hpp"
#include <cstring>
#include <set>

extern espm::Loader& GetEspmLoader();

namespace {
using Data = espm::QuestLifecycleData;
using Field = Data::Field;
template <class T>
Field Number(const char* tag, T value)
{
  Field field{ tag, std::vector<uint8_t>(sizeof(T)) };
  std::memcpy(field.bytes.data(), &value, sizeof(T));
  return field;
}
Field Text(const char* tag, const std::string& text)
{
  Field field{ tag, std::vector<uint8_t>(text.begin(), text.end()) };
  field.bytes.push_back(0);
  return field;
}
std::vector<Field> Minimal()
{
  return {
    { "DNAM",
      { 0xff, 0xff, 250, 204, 0x78, 0x56, 0x34, 0x12, 0x98, 0xba, 0xdc,
        0xfe } },
    { "NEXT", {} },
    Number<uint32_t>("ANAM", 42)
  };
}
Field LifecycleConditionField(uint16_t function)
{
  espm::CTDA condition{};
  condition.functionIndex = function;
  condition.runOnType = espm::CTDA::RunOnTypeFlags::Subject;
  return Number("CTDA", condition);
}
}

TEST_CASE("Quest lifecycle metadata owns exact flags and ordered stage bytes",
          "[Quest][QuestLifecycleData]")
{
  auto fields = Minimal();
  fields.insert(fields.begin() + 1,
                { Number<uint32_t>("ENAM", 0),
                  Number<uint32_t>("QTGL", 0x02001234),
                  Number<uint32_t>("QTGL", 0x02005678) });
  fields.insert(fields.end() - 1,
                { Field{ "INDX", { 0xff, 0xff, 0xff, 0xab } },
                  Field{ "INDX", { 2, 0, Data::Stage::StartUp, 0 } },
                  Field{ "INDX", { 2, 0, Data::Stage::ShutDown, 0x7f } } });
  const auto parsed = Data::Parse(fields);
  fields.clear();
  REQUIRE(parsed.flags == 0xffff);
  REQUIRE(parsed.priority == 250);
  REQUIRE(parsed.formVersion == 204);
  REQUIRE(parsed.unknownData == 0x12345678);
  REQUIRE(parsed.questType == 0xfedcba98);
  REQUIRE(parsed.event == 0);
  REQUIRE(parsed.nextAliasId == 42);
  REQUIRE(parsed.textDisplayGlobals ==
          std::vector<uint32_t>{ 0x02001234, 0x02005678 });
  REQUIRE(parsed.stages.size() == 3);
  REQUIRE(parsed.stages[0].index == 65535);
  REQUIRE(parsed.stages[0].flags == 0xff);
  REQUIRE(parsed.stages[0].unknown == 0xab);
  REQUIRE(parsed.stages[1].index == 2);
  REQUIRE(parsed.stages[1].flags == Data::Stage::StartUp);
  REQUIRE(parsed.stages[2].index == 2);
  REQUIRE(parsed.stages[2].flags == Data::Stage::ShutDown);
  REQUIRE(parsed.stages[2].unknown == 0x7f);
  REQUIRE_FALSE(Data::Parse(Minimal()).event);
}

TEST_CASE("Quest lifecycle conditions do not absorb stage objective or alias conditions",
          "[Quest][QuestLifecycleData]")
{
  auto fields = Minimal();
  fields.insert(fields.begin() + 1,
                { LifecycleConditionField(629), Text("CIS1", "First"),
                  Text("CIS2", "Second"), Field{ "FUTR", { 8, 7, 6 } } });
  fields.insert(fields.end() - 1,
                { LifecycleConditionField(58), Text("CIS2", "Story"),
                  Field{ "INDX", { 10, 0, 0, 0 } },
                  Number<uint8_t>("QSDT", 0), LifecycleConditionField(71),
                  Text("CIS2", "Stage"), Number<uint16_t>("QOBJ", 4),
                  Number<uint32_t>("FNAM", 0), Text("NNAM", "Objective"),
                  LifecycleConditionField(73), Text("CIS1", "Objective") });
  fields.insert(fields.end(),
                { Number<uint32_t>("ALST", 7), Text("ALID", "Target"),
                  Number<uint32_t>("FNAM", 2), LifecycleConditionField(46),
                  Text("CIS2", "Alias"), Field{ "ALED", {} },
                  Text("NNAM", "Description"),
                  Field{ "QSTA", std::vector<uint8_t>(8) }, LifecycleConditionField(47),
                  Text("CIS1", "Target") });
  const auto parsed = Data::Parse(fields);
  REQUIRE(parsed.dialogueConditions.size() == 1);
  REQUIRE(parsed.dialogueConditions[0].data.functionIndex == 629);
  REQUIRE(parsed.dialogueConditions[0].parameter1String == "First");
  REQUIRE(parsed.dialogueConditions[0].parameter2String == "Second");
  REQUIRE(parsed.storyConditions.size() == 1);
  REQUIRE(parsed.storyConditions[0].data.functionIndex == 58);
  REQUIRE_FALSE(parsed.storyConditions[0].parameter1String);
  REQUIRE(parsed.storyConditions[0].parameter2String == "Story");
  REQUIRE(parsed.unknownFields.size() == 1);
  REQUIRE(parsed.unknownFields[0].tag == "FUTR");
  REQUIRE(parsed.unknownFields[0].bytes == std::vector<uint8_t>{ 8, 7, 6 });
}

TEST_CASE("Quest lifecycle parser bounds checks typed fields and condition strings",
          "[Quest][QuestLifecycleData]")
{
  auto original = Minimal();
  original.insert(original.begin() + 1,
                  { Number<uint32_t>("ENAM", 1),
                    Number<uint32_t>("QTGL", 42), LifecycleConditionField(629),
                    Text("CIS1", "First"), Text("CIS2", "Second") });
  original.insert(original.end() - 1,
                  { Field{ "INDX", { 0xff, 0xff, 2, 0 } },
                    Number<uint16_t>("QOBJ", 3) });
  for (size_t field = 0; field < original.size(); ++field) {
    for (size_t size = 0; size < original[field].bytes.size(); ++size) {
      auto malformed = original;
      malformed[field].bytes.resize(size);
      INFO("field=" << original[field].tag << " size=" << size);
      REQUIRE_THROWS(Data::Parse(malformed));
    }
  }
  auto fields = original;
  fields[0].bytes.push_back(0);
  REQUIRE_THROWS(Data::Parse(fields));
  fields = original;
  fields[4].bytes[2] = 0;
  REQUIRE_THROWS(Data::Parse(fields));
  fields = Minimal();
  fields[1].bytes.push_back(0);
  REQUIRE_THROWS(Data::Parse(fields));
  fields = Minimal();
  fields[0].tag = "DNAMX";
  REQUIRE_THROWS(Data::Parse(fields));
}

TEST_CASE("Quest lifecycle parser rejects missing duplicate and misplaced singleton structure",
          "[Quest][QuestLifecycleData]")
{
  const auto original = Minimal();
  for (size_t index = 0; index < original.size(); ++index) {
    INFO("singleton=" << original[index].tag);
    auto fields = original;
    fields.erase(fields.begin() + index);
    REQUIRE_THROWS(Data::Parse(fields));
    fields = original;
    fields.insert(fields.begin() + index, original[index]);
    REQUIRE_THROWS(Data::Parse(fields));
  }
  auto fields = original;
  SECTION("present zero event is still a singleton")
  {
    fields.insert(fields.begin() + 1,
                  { Number<uint32_t>("ENAM", 0),
                    Number<uint32_t>("ENAM", 1) });
  }
  SECTION("data after story marker")
  {
    std::swap(fields[0], fields[1]);
  }
  SECTION("stage before story marker")
  {
    fields.insert(fields.begin() + 1, Field{ "INDX", { 1, 0, 2, 0 } });
  }
  SECTION("stage after objective section")
  {
    fields.insert(fields.end() - 1,
                  { Number<uint16_t>("QOBJ", 1),
                    Field{ "INDX", { 1, 0, 2, 0 } } });
  }
  SECTION("stage after alias section")
  {
    fields.push_back({ "INDX", { 1, 0, 2, 0 } });
  }
  SECTION("objective before story marker")
  {
    fields.insert(fields.begin() + 1, Number<uint16_t>("QOBJ", 1));
  }
  SECTION("alias before its section marker")
  {
    fields.insert(fields.end() - 1, Number<uint32_t>("ALST", 1));
  }
  SECTION("condition before data")
  {
    fields.insert(fields.begin(), LifecycleConditionField(56));
  }
  SECTION("condition string without condition")
  {
    fields.insert(fields.begin() + 1, Text("CIS1", "Orphan"));
  }
  SECTION("duplicate condition string")
  {
    fields.insert(fields.begin() + 1,
                  { LifecycleConditionField(629), Text("CIS2", ""), Text("CIS2", "New") });
  }
  SECTION("condition string separated from its condition")
  {
    fields.insert(fields.begin() + 1,
                  { LifecycleConditionField(629), Text("FLTR", ""), Text("CIS2", "Old") });
  }
  SECTION("condition string crosses its section")
  {
    fields.insert(fields.begin() + 1, LifecycleConditionField(629));
    fields.insert(fields.end() - 1, Text("CIS2", "Old"));
  }
  REQUIRE_THROWS(Data::Parse(fields));
}

TEST_CASE("Every owned quest lifecycle keeps exact DNAM and ordered stage metadata",
          "[Quest][QuestLifecycleData]")
{
  auto& browser = GetEspmLoader().GetBrowser();
  const auto records = browser.GetDistinctRecordsByType("QUST");
  size_t stageCount = 0, startupCount = 0, shutdownCount = 0;
  size_t dialogueCount = 0, storyCount = 0, eventCount = 0;
  size_t duplicateStageQuests = 0;
  for (const auto& record : records) {
    INFO("quest=" << record.ToGlobalId(record.rec->GetId()));
    const auto data = Data::Read(record.rec, browser.GetCache());
    std::set<uint16_t> ids;
    size_t rawStages = 0;
    bool duplicated = false;
    espm::RecordHeaderAccess::IterateFields(
      record.rec,
      [&](const char* tag, uint32_t size, const char* bytes) {
        if (!std::memcmp(tag, "DNAM", 4)) {
          REQUIRE(size == 12);
          uint16_t flags;
          std::memcpy(&flags, bytes, 2);
          REQUIRE(data.flags == flags);
          REQUIRE(data.priority == static_cast<uint8_t>(bytes[2]));
          REQUIRE(data.formVersion == static_cast<uint8_t>(bytes[3]));
        } else if (!std::memcmp(tag, "INDX", 4)) {
          REQUIRE(size == 4);
          const auto& stage = data.stages.at(rawStages++);
          uint16_t index;
          std::memcpy(&index, bytes, 2);
          REQUIRE(stage.index == index);
          REQUIRE(stage.flags == static_cast<uint8_t>(bytes[2]));
          REQUIRE(stage.unknown == static_cast<uint8_t>(bytes[3]));
          if (!ids.insert(index).second)
            duplicated = true;
          startupCount += !!(stage.flags & Data::Stage::StartUp);
          shutdownCount += !!(stage.flags & Data::Stage::ShutDown);
        }
      },
      browser.GetCache());
    REQUIRE(data.stages.size() == rawStages);
    stageCount += rawStages;
    dialogueCount += data.dialogueConditions.size();
    storyCount += data.storyConditions.size();
    eventCount += data.event.has_value();
    duplicateStageQuests += duplicated;
  }
  REQUIRE(records.size() >= 1800);
  REQUIRE(stageCount > 5000);
  REQUIRE(startupCount > 500);
  REQUIRE(shutdownCount > 250);
  REQUIRE(dialogueCount > 300);
  REQUIRE(storyCount > 400);
  REQUIRE(eventCount > 1000);
  REQUIRE(duplicateStageQuests > 0);
  REQUIRE_THROWS(Data::Read(nullptr, browser.GetCache()));
  REQUIRE_THROWS(
    Data::Read(browser.LookupById(0xf).rec, browser.GetCache()));
}
