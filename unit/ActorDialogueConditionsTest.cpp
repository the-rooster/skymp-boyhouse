#include "ActorDialogueConditions.h"
#include "TestUtils.hpp"
#include <cstring>

extern espm::Loader& GetEspmLoader();

TEST_CASE("Actor dialogue predicates use the named participant and winning "
          "location hierarchy",
          "[ActorDialogueConditions]")
{
  constexpr uint32_t location = 0x1f86d; // The Bannered Mare
  const uint32_t cell = 0x1605e;
  const uint32_t hulda = 0x13ba3, mikael = 0x1a670;
  PartOne server;
  server.AttachEspm(&GetEspmLoader());
  auto& world = server.worldState;
  auto add = [&](uint32_t base, uint32_t id) -> MpActor& {
    world.AddForm(
      std::make_unique<MpActor>(
        LocationalData{ {}, {}, FormDesc::FromFormId(cell, world.espmFiles) },
        FormCallbacks::DoNothing(), base),
      id);
    return world.GetFormAt<MpActor>(id);
  };
  auto& actor1 = add(hulda, 0xff006001);
  auto& actor2 = add(mikael, 0xff006002);
  const auto record = world.GetEspm().GetBrowser().LookupById(cell);
  auto clause = [](uint16_t index, uint32_t raw, int32_t member = 0x3152,
                   uint8_t flags = 0) {
    espm::QuestLifecycleData::Condition c;
    c.data.functionIndex = index;
    c.data.comparisonValue = 1;
    c.data.operatorFlag = flags;
    c.data.unknown2 = member;
    c.data.runOnType = espm::CTDA::RunOnTypeFlags::EventData;
    std::memcpy(c.data.functionData, &raw, sizeof(raw));
    return c;
  };
  using State = ActorDialogueConditionResult::State;
  auto check = [&](std::vector<espm::QuestLifecycleData::Condition> clauses) {
    return EvaluateActorDialogueConditions(actor1, actor2, location, record,
                                           clauses)
      .state;
  };
  REQUIRE(check({ clause(359, location), clause(359, 0x18a56),
                  clause(359, 0x16772) }) == State::Passed);
  REQUIRE(check({ clause(359, 0x18a57) }) == State::Failed);
  REQUIRE(check({ clause(72, hulda), clause(72, mikael, 0x3252),
                  clause(300, 0) }) == State::Passed);
  REQUIRE(check({ clause(72, hulda, 0x3252) }) == State::Failed);
  // The two NPCs have distinct authored factions even in the same cell.
  REQUIRE(check({ clause(71, 0xbf210), clause(71, 0xcf8f9, 0x3252) }) ==
          State::Passed);
  REQUIRE(check({ clause(71, 0xbf210, 0x3252) }) == State::Failed);
  REQUIRE(check({ clause(72, mikael, 0x3152, 1), clause(359, location),
                  clause(359, 0x18a57) }) == State::Failed);
  REQUIRE(check({ clause(72, hulda, 0x3152, 1), clause(565, location) }) ==
          State::Unsupported);
  REQUIRE(check({ clause(359, hulda) }) == State::Unsupported);
  REQUIRE(check({ clause(72, hulda, -1) }) == State::Unsupported);
  REQUIRE(check({ clause(72, hulda, 0x3152, 4) }) == State::Unsupported);
}
