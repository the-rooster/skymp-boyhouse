#include "CampaignReferenceQueryApi.h"
#include "CampaignReferenceBinding.h"
#include "LocationalDataUtils.h"
#include "QuestJournal.h"
#include "TestUtils.hpp"
#include "libespm/GroupUtils.h"
#include "libespm/RecordHeaderAccess.h"
#include "script_objects/CampaignFormObject.h"
#include <catch2/catch_all.hpp>
#include <cstring>
#include <limits>

extern espm::Loader& GetEspmLoader();

namespace {
constexpr uint32_t kNpc = 0x1a677;
constexpr uint32_t kItem = 0x8a986;
constexpr uint32_t kQuest = 0x4e50d;

std::vector<std::shared_ptr<PexScript>> QueryBaseScripts()
{
  std::vector<std::shared_ptr<PexScript>> result;
  for (const auto& [name, parent] :
       std::vector<std::pair<std::string, std::string>>{
         { "Form", "" }, { "ObjectReference", "Form" },
         { "Actor", "ObjectReference" }, { "Cell", "Form" },
         { "WorldSpace", "Form" } }) {
    auto script = std::make_shared<PexScript>();
    script->source = name;
    Object object;
    object.NameIndex = name;
    object.parentClassName = parent;
    script->objectTable.push_back(object);
    result.push_back(script);
  }
  return result;
}

struct QueryFixture
{
  PartOne server;
  VirtualMachine vm{ QueryBaseScripts() };
  MpActor *owner, *guest, *stranger, *npc;
  std::map<std::pair<uint32_t, uint32_t>,
           std::shared_ptr<CampaignFormObject>> objects;

  QueryFixture()
  {
    server.AttachEspm(&GetEspmLoader());
    owner = &AddPlayer(0xff004001);
    guest = &AddPlayer(0xff004002);
    stranger = &AddPlayer(0xff004003);
    owner->SetWorldInstance("campaign:" + std::to_string(owner->GetFormId()) +
                            ":queries");
    guest->SetWorldInstance(owner->GetWorldInstance());
    stranger->SetWorldInstance("campaign:" +
                               std::to_string(stranger->GetFormId()) +
                               ":queries");
    guest->SetPosAndAngleSilent({ 1000, 2000, 3000 }, {});
    npc = &AddNpc(*owner, 0xff004010, { 4, 6, 15 });
    vm.SetStrictExecution(true);
    CampaignReferenceQueryApi::Register(
      vm, *guest,
      [&](const VarValue& value) {
        const auto* object = value.GetType() == VarValue::kType_Object
          ? dynamic_cast<CampaignFormObject*>(static_cast<IGameObject*>(value))
          : nullptr;
        if (!object || object->OwnerId() != QuestJournal::Owner(*guest).GetFormId())
          throw std::runtime_error("Expected a form from the acting campaign");
        return object->FormId();
      },
      [&](uint32_t id) { return Resolve(id); });
  }

  MpActor& AddPlayer(uint32_t id)
  {
    server.CreateActor(id, { 1, 2, 3 }, 0, 0x3c);
    auto& actor = server.worldState.GetFormAt<MpActor>(id);
    actor.SetInventory(Inventory().AddItem(0xf, 10));
    actor.SetPropertyValueDump(QuestJournal::kEnabled,
                               "[\"4e50d:Skyrim.esm\"]", false, false);
    return actor;
  }

  MpActor& AddNpc(MpActor& campaign, uint32_t id, const NiPoint3& position)
  {
    server.worldState.AddForm(
      std::make_unique<MpActor>(
        LocationalData{ position, { 15, 25, 35 }, FormDesc::Tamriel() },
        FormCallbacks::DoNothing(), 0x13bbd),
      id);
    auto& actor = server.worldState.GetFormAt<MpActor>(id);
    actor.SetWorldInstance(campaign.GetWorldInstance());
    actor.SetInventory(Inventory());
    CampaignReferenceBinding::Bind(campaign, kNpc, actor);
    return actor;
  }

  std::shared_ptr<CampaignFormObject> Resolve(uint32_t id)
  {
    const auto campaign = QuestJournal::Owner(*guest).GetFormId();
    const auto key = std::make_pair(campaign, id);
    if (!objects.count(key))
      objects[key] = std::make_shared<CampaignFormObject>(
        server.worldState.GetEspm().GetBrowser().LookupById(id), campaign, id);
    return objects.at(key);
  }

  VarValue Inside(uint32_t id, const char* name,
                  std::vector<VarValue> arguments = {})
  {
    return vm.CallMethod(Resolve(id).get(), name, arguments);
  }

  VarValue Query(uint32_t id, const char* name,
                 std::vector<VarValue> arguments = {})
  {
    VarValue result;
    QuestJournal::Atomic(*guest, [&] { result = Inside(id, name, arguments); });
    return result;
  }

  FormDesc Interior()
  {
    const auto& browser = server.worldState.GetEspm().GetBrowser();
    const auto id = LocationalDataUtils::GetWorldOrCell(
      browser, browser.LookupById(kNpc));
    REQUIRE(browser.LookupById(id).rec);
    REQUIRE(browser.LookupById(id).rec->GetType() == "CELL");
    return FormDesc::FromFormId(id, server.worldState.espmFiles);
  }

  FormDesc ExteriorCell()
  {
    const auto& browser = server.worldState.GetEspm().GetBrowser();
    for (const auto& cell : browser.GetDistinctRecordsByType("CELL")) {
      if (cell.rec->IsDeleted())
        continue;
      const auto* group = espm::GetExteriorWorldGroup(browser, cell.rec);
      uint32_t world = 0;
      if (!group || !group->GetParentWRLD(world) ||
          cell.ToGlobalId(world) != 0x3c)
        continue;
      bool coordinates = false, exterior = false;
      espm::RecordHeaderAccess::IterateFields(
        cell.rec,
        [&](const char* tag, uint32_t size, const char* bytes) {
          if (!std::memcmp(tag, "XCLC", 4) && (size == 8 || size == 12))
            coordinates = true;
          if (!std::memcmp(tag, "DATA", 4) && (size == 1 || size == 2))
            exterior = !(static_cast<uint8_t>(bytes[0]) & 1);
        },
        server.worldState.GetEspmCache());
      if (coordinates && exterior)
        return FormDesc::FromFormId(cell.ToGlobalId(cell.rec->GetId()),
                                    server.worldState.espmFiles);
    }
    throw std::runtime_error("Owned test data has no Tamriel exterior cell");
  }
};
}

TEST_CASE("Campaign spatial queries read the owner and its private NPC",
          "[Quest][ReferenceQueries]")
{
  QueryFixture f;
  auto& otherNpc = f.AddNpc(*f.stranger, 0xff004011, { 9000, 8000, 7000 });
  REQUIRE(otherNpc.GetWorldInstance() != f.npc->GetWorldInstance());
  REQUIRE(static_cast<double>(f.Query(0x14, "GetPositionX")) == 1.0);
  REQUIRE(static_cast<double>(f.Query(0x14, "GetPositionY")) == 2.0);
  REQUIRE(static_cast<double>(f.Query(0x14, "GetPositionZ")) == 3.0);
  REQUIRE(static_cast<double>(f.Query(kNpc, "GetPositionX")) == 4.0);
  REQUIRE(static_cast<double>(f.Query(kNpc, "GetPositionY")) == 6.0);
  REQUIRE(static_cast<double>(f.Query(kNpc, "GetPositionZ")) == 15.0);
  REQUIRE(static_cast<double>(f.Query(kNpc, "GetAngleX")) == 15.0);
  REQUIRE(static_cast<double>(f.Query(kNpc, "GetAngleY")) == 25.0);
  REQUIRE(static_cast<double>(f.Query(kNpc, "GetAngleZ")) == 35.0);
  REQUIRE(static_cast<double>(f.Query(
            0x14, "GetDistance", { VarValue(f.Resolve(kNpc)) })) == 13.0);
  REQUIRE(static_cast<double>(f.Query(
            kNpc, "GetDistance", { VarValue(f.Resolve(0x14)) })) == 13.0);
  REQUIRE(static_cast<double>(f.Query(
            kNpc, "GetDistance", { VarValue(f.Resolve(kNpc)) })) == 0.0);
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(kNpc));
}

TEST_CASE("Campaign death queries distinguish copies of the same NPC",
          "[Quest][ReferenceQueries]")
{
  QueryFixture f;
  auto& other = f.AddNpc(*f.stranger, 0xff004011, { 9, 8, 7 });
  other.SetIsDead(true);
  REQUIRE_FALSE(static_cast<bool>(f.Query(kNpc, "IsDead")));
  f.npc->SetIsDead(true);
  REQUIRE(static_cast<bool>(f.Query(kNpc, "IsDead")));
  REQUIRE_FALSE(static_cast<bool>(f.Query(0x14, "IsDead")));
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(kNpc));
}

TEST_CASE("Campaign cell queries return logical cells and worldspaces",
          "[Quest][ReferenceQueries]")
{
  QueryFixture f;
  const auto outside = f.Query(0x14, "GetWorldSpace");
  const auto* world =
    dynamic_cast<CampaignFormObject*>(static_cast<IGameObject*>(outside));
  REQUIRE(world);
  REQUIRE(world->FormId() == 0x3c);
  REQUIRE(world->OwnerId() == f.owner->GetFormId());
  REQUIRE_THROWS_WITH(f.Query(0x14, "GetParentCell"),
                      Catch::Matchers::ContainsSubstring("cell index"));
  const auto interior = f.Interior();
  f.owner->SetCellOrWorldObsolete(interior);
  f.npc->SetCellOrWorldObsolete(interior);
  const auto cellValue = f.Query(0x14, "GetParentCell");
  const auto* cell =
    dynamic_cast<CampaignFormObject*>(static_cast<IGameObject*>(cellValue));
  REQUIRE(cell);
  REQUIRE(cell->OwnerId() == f.owner->GetFormId());
  REQUIRE(cell->FormId() == interior.ToFormId(f.server.worldState.espmFiles));
  REQUIRE(cell->record.rec->GetType() == "CELL");
  REQUIRE_FALSE(static_cast<IGameObject*>(f.Query(0x14, "GetWorldSpace")));
  REQUIRE(static_cast<double>(f.Query(
            0x14, "GetDistance", { VarValue(f.Resolve(kNpc)) })) == 13.0);
}

TEST_CASE("An authoritative exterior CELL shares its worldspace coordinates",
          "[Quest][ReferenceQueries]")
{
  QueryFixture f;
  const auto exterior = f.ExteriorCell();
  f.owner->SetCellOrWorldObsolete(exterior);
  const auto cellValue = f.Query(0x14, "GetParentCell");
  const auto* cell =
    dynamic_cast<CampaignFormObject*>(static_cast<IGameObject*>(cellValue));
  REQUIRE(cell);
  REQUIRE(cell->FormId() == exterior.ToFormId(f.server.worldState.espmFiles));
  const auto worldValue = f.Query(0x14, "GetWorldSpace");
  const auto* world =
    dynamic_cast<CampaignFormObject*>(static_cast<IGameObject*>(worldValue));
  REQUIRE(world);
  REQUIRE(world->FormId() == 0x3c);
  REQUIRE(static_cast<double>(f.Query(
            0x14, "GetDistance", { VarValue(f.Resolve(kNpc)) })) == 13.0);
}

TEST_CASE("Campaign query ingress refuses other histories and unbound targets",
          "[Quest][ReferenceQueries]")
{
  QueryFixture f;
  REQUIRE_THROWS(f.Inside(0x14, "GetPositionX")); // No transaction.
  REQUIRE_THROWS(f.Query(kItem, "GetPositionX"));
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(kItem));
  REQUIRE_THROWS(f.Query(0x14, "GetPositionX", { VarValue(1) }));
  REQUIRE_THROWS(f.Query(0x14, "IsDead", { VarValue(false) }));
  REQUIRE_THROWS(f.Query(0x14, "GetDistance"));
  auto foreign = std::make_shared<CampaignFormObject>(
    f.server.worldState.GetEspm().GetBrowser().LookupById(kNpc),
    f.stranger->GetFormId(), kNpc);
  for (auto argument : { VarValue::None(), VarValue(0x14),
                         VarValue(foreign), f.guest->ToVarValue() }) {
    REQUIRE_THROWS(f.Query(0x14, "GetDistance", { argument }));
  }
  f.npc->SetWorldInstance(f.stranger->GetWorldInstance());
  REQUIRE_THROWS(f.Query(kNpc, "GetPositionX"));
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(kNpc));
}

TEST_CASE("Campaign distance rejects incomparable spaces and invalid numbers",
          "[Quest][ReferenceQueries]")
{
  QueryFixture f;
  SECTION("Different interiors or worldspaces are not one coordinate frame")
  {
    f.npc->SetCellOrWorldObsolete(f.Interior());
    REQUIRE_THROWS_WITH(
      f.Query(0x14, "GetDistance", { VarValue(f.Resolve(kNpc)) }),
      Catch::Matchers::ContainsSubstring("same interior or worldspace"));
  }
  SECTION("An unrelated asset cannot masquerade as a cell")
  {
    f.owner->SetCellOrWorldObsolete(
      FormDesc::FromFormId(0xf, f.server.worldState.espmFiles));
    REQUIRE_THROWS(f.Query(0x14, "GetParentCell"));
    REQUIRE_THROWS(f.Query(0x14, "GetWorldSpace"));
    REQUIRE_THROWS(
      f.Query(0x14, "GetDistance", { VarValue(f.Resolve(kNpc)) }));
  }
  SECTION("Positions and angles must be finite")
  {
    const auto bad = GENERATE(std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN());
    f.npc->SetPosAndAngleSilent({ bad, 6, 15 }, { 15, bad, 35 });
    REQUIRE_THROWS(f.Query(kNpc, "GetPositionX"));
    REQUIRE_THROWS(f.Query(kNpc, "GetAngleY"));
    REQUIRE_THROWS(
      f.Query(0x14, "GetDistance", { VarValue(f.Resolve(kNpc)) }));
  }
  SECTION("Finite endpoints must not overflow a Papyrus float distance")
  {
    const auto largest = std::numeric_limits<float>::max();
    f.owner->SetPosAndAngleSilent({ largest, 0, 0 }, {});
    f.npc->SetPosAndAngleSilent({ -largest, 0, 0 }, {});
    REQUIRE_THROWS_WITH(
      f.Query(0x14, "GetDistance", { VarValue(f.Resolve(kNpc)) }),
      Catch::Matchers::ContainsSubstring("float range"));
  }
}

TEST_CASE("A campaign query read conflict rolls back journal and rewards",
          "[Quest][ReferenceQueries]")
{
  QueryFixture f;
  const bool death = GENERATE(false, true);
  bool externalChanged = false;
  const auto before =
    f.owner->GetDynamicFields().GetValueDump(QuestJournal::kProperty);
  REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
    if (death)
      REQUIRE_FALSE(static_cast<bool>(f.Inside(kNpc, "IsDead")));
    else
      REQUIRE(static_cast<double>(f.Inside(kNpc, "GetPositionX")) == 4.0);
    QuestJournal::Apply(*f.guest,
                         { { "quest", kQuest }, { "op", "setStage" },
                           { "index", 10 } });
    QuestJournal::EditInventory(*f.guest, *f.owner,
                                 [](Inventory& inventory) {
      inventory.AddItem(0xf, 100);
    });
    // Simulate an external/reentrant native change after the script's read.
    // Its side effect is not ours to undo, but the dependent quest write and
    // reward must not commit from the now-stale observation.
    if (death)
      f.npc->SetIsDead(true);
    else
      f.npc->SetPosAndAngleSilent({ 8, 6, 15 }, { 15, 25, 35 });
    externalChanged = true;
  }));
  REQUIRE(externalChanged);
  REQUIRE(f.owner->GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          before);
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
}
