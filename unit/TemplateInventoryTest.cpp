#include "InventoryDefaults.h"
#include "MsgType.h"
#include "ServerState.h"
#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <nlohmann/json.hpp>

PartOne& GetPartOne();
extern espm::Loader& GetEspmLoader();

TEST_CASE("DLC container and NPC inventory IDs follow the supplying plugin",
          "[TemplateInventory][LeveledAllocation]")
{
  PartOne server;
  server.AttachEspm(&GetEspmLoader());
  auto& world = server.worldState;
  // Dragonborn is raw file index 2 in these records, global file index 4.
  for (const auto& [base, expected, count] :
       std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>{
         { 0x0403cd8d, 0x0403cd8e, 3 }, { 0x0403d2da, 0x0403d241, 4 } }) {
    const auto objects = GetInventoryObjects(
      &world, {}, world.GetEspm().GetBrowser().LookupById(base));
    const auto found =
      std::find_if(objects.begin(), objects.end(), [&](const auto& entry) {
        return entry.formId == expected;
      });
    REQUIRE(found != objects.end());
    REQUIRE(found->count == count);
  }
}

TEST_CASE("MS13BanditCampfire01 in BleakFalls should have inventory/equipment "
          "derived from NPC template",
          "[TemplateInventory]")
{
  PartOne& p = GetPartOne();
  p.worldState.npcSettings["Skyrim.esm"].spawnInInterior = true;
  p.worldState.npcEnabled = true;
  uint32_t actorId = 0x39fe4;
  auto& bandit = p.worldState.GetFormAt<MpActor>(actorId);

  REQUIRE(bandit.GetEquipment().inv.ToJson().size() > 0);

  int numWeaps = 0;
  for (auto entry : bandit.GetEquipment().inv.entries) {
    auto lookupRes =
      p.worldState.GetEspm().GetBrowser().LookupById(entry.baseId);
    if (lookupRes.rec && lookupRes.rec->GetType() == "WEAP") {
      numWeaps++;
    }
  }

  REQUIRE(numWeaps == 1);
}
