#include "libespm/SceneActionData.h"
#include "libespm/Browser.h"
#include "libespm/Combiner.h"
#include "libespm/Loader.h"
#include <catch2/catch_all.hpp>

extern espm::Loader& GetEspmLoader();

namespace {
using Bytes = std::string;
template <class T>
Bytes Binary(T value)
{
  return { reinterpret_cast<const char*>(&value), sizeof(value) };
}
Bytes Field(const char* name, const Bytes& bytes = {})
{
  return Bytes(name, 4) + Binary(static_cast<uint16_t>(bytes.size())) + bytes;
}
Bytes Num(const char* name, uint32_t value)
{
  return Field(name, Binary(value));
}
Bytes Record(const char* type, uint32_t id, const Bytes& fields = {},
             uint32_t flags = 0)
{
  return Bytes(type, 4) + Binary(static_cast<uint32_t>(fields.size())) +
    Binary(flags) + Binary(id) + Bytes(8, '\0') + fields;
}
Bytes Phase()
{
  return Field("HNAM") + Field("NEXT") + Field("NEXT") + Field("HNAM");
}
Bytes Action(uint16_t type, uint32_t index, const Bytes& extras = {})
{
  return Field("ANAM", Binary(type)) + Num("ALID", 2) + Num("INAM", index) +
    Num("SNAM", 0) + Num("ENAM", 0) + extras + Field("ANAM");
}
espm::SceneActionData Read(const Bytes& body)
{
  const auto file = Record("TES4", 0) + Record("SCEN", 1, body);
  espm::Browser browser(file.data(), file.size());
  espm::Combiner combine;
  combine.AddSource(&browser, "Base.esm");
  return espm::SceneActionData::Read(*combine.Combine(), 1);
}
}

TEST_CASE("Scene action reader retains authored phases, raw controls and "
          "other action types",
          "[SceneActionData]")
{
  const auto condition = Field("CTDA", Bytes(32, '\0'));
  const auto phases = Field("HNAM") + condition + Field("NEXT") + condition +
    condition + Field("NEXT") + Field("HNAM");
  const auto scene = Read(
    Num("FNAM", 7) + phases + Num("ALID", 2) + Num("LNAM", 1) +
    Num("DNAM", 26) +
    Action(0, 5, Num("DATA", 5) + Num("FNAM", 4) + Num("HTID", 9)) +
    Action(1, 5, Num("PNAM", 4)) + Action(2, 12, Field("SNAM", Binary(3.5f))) +
    Action(91, 13, Num("ZZZZ", 3)) + Num("PNAM", 3) + Num("INAM", 13) +
    Field("VNAM", Bytes(16, '\x12')) + condition + Field("VMAD"));
  REQUIRE(scene.flags == 7);
  REQUIRE(scene.questId == 3);
  REQUIRE(scene.conditionCount == 1);
  REQUIRE(scene.hasScripts);
  REQUIRE(scene.duplicateActionIndices);
  REQUIRE_FALSE(scene.unknownFields);
  REQUIRE(scene.vnam == std::vector<uint8_t>(16, 0x12));
  REQUIRE(scene.phases.size() == 1);
  REQUIRE(scene.phases[0].startConditionCount == 1);
  REQUIRE(scene.phases[0].completionConditionCount == 2);
  REQUIRE(scene.actors.size() == 1);
  REQUIRE(scene.actors[0].flags == 1);
  REQUIRE(scene.actors[0].behaviorFlags == 26);
  REQUIRE(scene.actions.size() == 4);
  REQUIRE(scene.actions[0].index == 5);
  REQUIRE(scene.actions[0].actorAlias == 2);
  REQUIRE(scene.actions[0].topicId == 5);
  REQUIRE(scene.actions[0].flags == 4);
  REQUIRE(scene.actions[0].headtrackActorAlias == 9);
  REQUIRE(scene.actions[1].packageIds == std::vector<uint32_t>{ 4 });
  REQUIRE(scene.actions[2].timerSeconds == 3.5f);
  REQUIRE(scene.actions[3].type == 91);
  REQUIRE(scene.actions[3].unknownFields);
}

TEST_CASE("Scene action topic and quest IDs use the winning plugin mapping",
          "[SceneActionData]")
{
  const auto base =
    Record("TES4", 0) + Record("SCEN", 1, Phase() + Num("PNAM", 9));
  const auto extra = Record("TES4", 0); // Shifts the patch to file index 2.
  const auto patch = Record("TES4", 0, Field("MAST", Bytes("Base.esm\0", 9))) +
    Record("SCEN", 1,
           Phase() + Action(0, 1, Num("DATA", 0x01000003)) +
             Num("PNAM", 0x01000004)) +
    Record("SCEN", 7, {}, 0x20);
  espm::Browser br(base.data(), base.size()),
    other(extra.data(), extra.size()), override(patch.data(), patch.size());
  espm::Combiner combine;
  combine.AddSource(&br, "Base.esm");
  combine.AddSource(&other, "Other.esm");
  combine.AddSource(&override, "Patch.esp");
  const auto combined = combine.Combine();
  const auto& browser = *combined;
  const auto scene = espm::SceneActionData::Read(browser, 1);
  REQUIRE(scene.questId == 0x02000004);
  REQUIRE(scene.actions.at(0).topicId == 0x02000003);
  REQUIRE_THROWS(espm::SceneActionData::Read(browser, 7));
  REQUIRE_THROWS(espm::SceneActionData::Read(browser, 0xf00003));
}

TEST_CASE(
  "Malformed scene boundaries, repeated fields and bad ranges fail explicitly",
  "[SceneActionData]")
{
  const auto start = Field("ANAM", Binary(uint16_t(0)));
  for (const auto& body :
       { Field("HNAM") + Field("NEXT") + Field("HNAM"),
         Phase() + Field("ANAM"), Phase() + start,
         Phase() + start + Field("HNAM"), Phase() + start + Field("ANAM"),
         Phase() + Action(0, 1, Num("ENAM", 0)),
         Phase() + Action(0, 1, Field("DATA", "x")),
         Phase() + start + Num("ALID", 1) + Num("INAM", 1) + Num("SNAM", 0) +
           Num("ENAM", 1) + Field("ANAM") }) {
    REQUIRE_THROWS(Read(body + Num("PNAM", 3)));
  }
}

TEST_CASE("Original Bannered Mare scene has six single-phase dialogue actions",
          "[SceneActionData][espm]")
{
  const auto& browser = GetEspmLoader().GetBrowser();
  const auto scene = espm::SceneActionData::Read(browser, 0x17f33);
  REQUIRE(scene.questId == 0x17f32);
  REQUIRE(scene.flags == 7);
  REQUIRE_FALSE(scene.hasScripts);
  REQUIRE_FALSE(scene.unknownFields);
  REQUIRE_FALSE(scene.duplicateActionIndices);
  REQUIRE(scene.phases.size() == 6);
  REQUIRE(scene.actors.size() == 2);
  REQUIRE(scene.actors[0].aliasId == 1);
  REQUIRE(scene.actors[1].aliasId == 2);
  REQUIRE(scene.actors[0].behaviorFlags == 26);
  REQUIRE(scene.actors[1].behaviorFlags == 26);
  REQUIRE(scene.actions.size() == 6);
  const std::vector<uint32_t> topics{ 0x17f2b, 0x17f2a, 0x17f29,
                                      0x17f28, 0x17f27, 0x17f1f };
  for (size_t i = 0; i < 6; ++i) {
    REQUIRE(scene.phases[i].startConditionCount == 0);
    REQUIRE(scene.phases[i].completionConditionCount == 0);
    REQUIRE_FALSE(scene.phases[i].unknownFields);
    const auto& action = scene.actions[i];
    REQUIRE(action.type == 0);
    REQUIRE(action.index == i + 1);
    REQUIRE(action.actorAlias == int(i % 2 + 1));
    REQUIRE(action.startPhase == i);
    REQUIRE(action.endPhase == i);
    REQUIRE(action.topicId == topics[i]);
    REQUIRE(browser.LookupById(action.topicId).rec->GetType() == "DIAL");
    REQUIRE(action.headtrackActorAlias == -1);
    REQUIRE_FALSE(action.unknownFields);
  }
}
