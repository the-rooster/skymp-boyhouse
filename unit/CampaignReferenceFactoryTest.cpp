#include "CampaignReferenceFactory.h"
#include "CampaignReferenceBinding.h"
#include "LocationalDataUtils.h"
#include "QuestJournal.h"
#include "TestUtils.hpp"
#include "libespm/GroupUtils.h"
#include "save_storages/ISaveStorage.h"
#include <catch2/catch_all.hpp>

extern espm::Loader& GetEspmLoader();
namespace {
struct AllocationStorage
  : Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>
{
  bool atomic = true;
  std::vector<std::vector<std::optional<MpChangeForm>>> batches;
  bool SupportsAtomicBatches() const noexcept override { return atomic; }
  void IterateSync(const IterateSyncCallback&) override {}
  void Upsert(std::vector<std::optional<MpChangeForm>>&& forms,
              const UpsertCallback& callback) override
  {
    batches.push_back(std::move(forms));
    callback();
  }
  void Iterate(const IterateCallback& callback,
               const std::optional<std::vector<FormDesc>>&) override
  {
    callback({});
  }
  uint32_t GetNumFinishedUpserts() const override { return batches.size(); }
  uint32_t GetNumFinishedIterates() const override { return 0; }
  void Tick() override {}
  bool GetRecycledChangeFormsBuffer(
    std::vector<std::optional<MpChangeForm>>&) override
  {
    return false;
  }
  const std::string& GetName() const override
  {
    static const std::string name = "allocation-test";
    return name;
  }
};
struct AllocationWorld
{
  PartOne server;
  std::shared_ptr<AllocationStorage> storage =
    std::make_shared<AllocationStorage>();
  MpActor *owner, *other;
  AllocationWorld()
  {
    server.AttachEspm(&GetEspmLoader());
    server.AttachSaveStorage(storage);
    server.CreateActor(0xff002001, { 1, 2, 3 }, 0, 0x3c);
    server.CreateActor(0xff002002, { 1, 2, 3 }, 0, 0x3c);
    owner = &server.worldState.GetFormAt<MpActor>(0xff002001);
    other = &server.worldState.GetFormAt<MpActor>(0xff002002);
    owner->SetWorldInstance("campaign:4278198273:fixture");
    other->SetWorldInstance("campaign:4278198274:fixture");
    server.worldState.Tick();
    storage->batches.clear();
  }
};
}

TEST_CASE(
  "Campaign reference allocation publishes unique NPCs and placed stacks once",
  "[Quest][CampaignAllocation]")
{
  AllocationWorld f;
  auto& world = f.server.worldState;
  REQUIRE_FALSE(world.LookupFormByIdNoLoad(0x1a677));
  const auto ids = CampaignReferenceFactory::Ensure(
    *f.owner, { 0x1a677, 0x8a986, 0x1a677 }, 5);
  REQUIRE(ids.size() == 3);
  REQUIRE(ids[0] == ids[2]);
  REQUIRE(ids[0] != ids[1]);
  auto& npc = world.GetFormAt<MpActor>(ids[0]);
  auto& item = world.GetFormAt<MpObjectReference>(ids[1]);
  REQUIRE(npc.GetBaseId() == 0x13bbd);
  REQUIRE(item.GetChangeForm().count == 15);
  REQUIRE(npc.GetChangeForm().baseContainerAdded);
  REQUIRE(npc.GetChangeForm().factions.has_value());
  REQUIRE_FALSE(npc.GetChangeForm().isRaceMenuOpen);
  REQUIRE_FALSE(npc.IsRespawning());
  REQUIRE(std::dynamic_pointer_cast<MpFormGameObject>(npc.ToGameObject())
            ->ListActivePexInstances()
            .empty());
  REQUIRE(&CampaignReferenceBinding::Resolve(*f.owner, 0x1a677) == &npc);
  REQUIRE_FALSE(world.LookupFormByIdNoLoad(0x1a677));
  for (const auto& equipped : npc.GetEquipment().inv.entries)
    REQUIRE(npc.GetInventory().GetItemCount(equipped.baseId) >=
            equipped.count);
  npc.SetInventory(Inventory().AddItem(0xf, 123));
  const auto saved = MpChangeForm::ToJson(npc.GetChangeForm());
  REQUIRE(
    CampaignReferenceFactory::Ensure(*f.owner, { 0x1a677, 0x8a986 }, 50) ==
    std::vector<uint32_t>{ ids[0], ids[1] });
  REQUIRE(MpChangeForm::ToJson(npc.GetChangeForm()) == saved);
  const auto other =
    CampaignReferenceFactory::Ensure(*f.other, { 0x1a677 }, 5);
  REQUIRE(other[0] != ids[0]);
  REQUIRE_FALSE(world.GetFormAt<MpActor>(other[0]).SharesWorldInstance(npc));
  f.server.worldState.Tick();
  REQUIRE(f.storage->batches.size() == 1);
  const auto& batch = f.storage->batches.front();
  REQUIRE(batch[f.owner->GetIdx()]->dynamicFields.GetValueDump(
            CampaignReferenceFactory::kAllocations) != "null");
  REQUIRE(batch[npc.GetIdx()]->inv.GetItemCount(0xf) == 123);
  REQUIRE(batch[item.GetIdx()]->count == 15);
}

TEST_CASE("Invalid allocation batches publish no references or owner metadata",
          "[Quest][CampaignAllocation]")
{
  AllocationWorld f;
  const auto initial = MpChangeForm::ToJson(f.owner->GetChangeForm());
  const auto index = f.server.worldState.actorIdByPrivateIndexedProperty;
  SECTION("invalid trailing reference")
  {
    REQUIRE_THROWS(
      CampaignReferenceFactory::Ensure(*f.owner, { 0x1a677, 0xf }, 5));
  }
  SECTION("non-atomic storage")
  {
    f.storage->atomic = false;
    REQUIRE_THROWS(CampaignReferenceFactory::Ensure(*f.owner, { 0x1a677 }, 5));
  }
  SECTION("inside a quest transaction")
  {
    QuestJournal::Atomic(*f.owner, [&] {
      REQUIRE_THROWS(
        CampaignReferenceFactory::Ensure(*f.owner, { 0x1a677 }, 5));
    });
  }
  REQUIRE(MpChangeForm::ToJson(f.owner->GetChangeForm()) == initial);
  REQUIRE(f.server.worldState.actorIdByPrivateIndexedProperty == index);
  f.server.worldState.Tick();
  REQUIRE(f.storage->batches.empty());
}

TEST_CASE("Allocation ledger survives recovery and does not replace missing "
          "or deleted copies",
          "[Quest][CampaignAllocation]")
{
  AllocationWorld f;
  const auto ids =
    CampaignReferenceFactory::Ensure(*f.owner, { 0x1a677, 0x8a986 }, 5);
  auto& npc = f.server.worldState.GetFormAt<MpActor>(ids[0]);
  npc.SetInventory(Inventory().AddItem(0xf, 37));
  auto ownerState = f.owner->GetChangeForm();
  ownerState.isDisabled = true;
  ownerState.profileId = 42;
  PartOne restarted;
  restarted.AttachEspm(&GetEspmLoader());
  auto& world = restarted.worldState;
  // Database iteration may deliver copies before their owner.
  world.LoadChangeForm(npc.GetChangeForm(), FormCallbacks::DoNothing());
  world.LoadChangeForm(
    f.server.worldState.GetFormAt<MpObjectReference>(ids[1]).GetChangeForm(),
    FormCallbacks::DoNothing());
  world.LoadChangeForm(ownerState, FormCallbacks::DoNothing());
  auto& owner = world.GetFormAt<MpActor>(f.owner->GetFormId());
  REQUIRE(CampaignReferenceFactory::Ensure(owner, { 0x1a677, 0x8a986 }, 99) ==
          ids);
  REQUIRE(world.GetFormAt<MpActor>(ids[0]).GetInventory().GetItemCount(0xf) ==
          37);
  SECTION("deleted allocation")
  {
    auto& copy = world.GetFormAt<MpActor>(ids[0]);
    copy.Delete();
    REQUIRE(CampaignReferenceFactory::Ensure(owner, { 0x1a677 }, 99).front() ==
            ids[0]);
    REQUIRE(copy.IsDeleted());
  }
  SECTION("missing allocation")
  {
    auto ledger = nlohmann::json::parse(owner.GetDynamicFields().GetValueDump(
      CampaignReferenceFactory::kAllocations));
    ledger["copies"]["1a677:Skyrim.esm"] = "fefefe";
    owner.SetPropertyValueDump(CampaignReferenceFactory::kAllocations,
                               ledger.dump(), false, false);
    REQUIRE_THROWS(CampaignReferenceFactory::Ensure(owner, { 0x1a677 }, 5));
  }
}

TEST_CASE(
  "Guests and shared-world characters cannot allocate another campaign",
  "[Quest][CampaignAllocation]")
{
  AllocationWorld f;
  f.other->SetWorldInstance(f.owner->GetWorldInstance());
  REQUIRE_THROWS(CampaignReferenceFactory::Ensure(*f.other, { 0x1a677 }, 1));
  f.other->SetWorldInstance("");
  REQUIRE_THROWS(CampaignReferenceFactory::Ensure(*f.other, { 0x1a677 }, 1));
}

TEST_CASE("Campaign areas discover original references through the plugin "
          "spatial index",
          "[Quest][CampaignAllocation]")
{
  AllocationWorld f;
  auto& world = f.server.worldState;
  const auto& browser = world.GetEspm().GetBrowser();
  const auto source = browser.LookupById(0x8a986);
  const auto data = reinterpret_cast<const espm::REFR*>(source.rec)
                      ->GetData(world.GetEspmCache());
  const auto cell =
    source.ToGlobalId(espm::GetWorldOrCell(browser, source.rec));
  const auto center = LocationalDataUtils::GetPos(data.loc);
  const auto copies =
    CampaignReferenceFactory::EnsureArea(*f.owner, cell, center, 1.f, 5);
  REQUIRE_FALSE(copies.empty());
  const auto& item = CampaignReferenceBinding::Resolve(*f.owner, 0x8a986);
  REQUIRE(std::find(copies.begin(), copies.end(), item.GetFormId()) !=
          copies.end());
  REQUIRE(item.GetChangeForm().count == 15);
  REQUIRE(CampaignReferenceFactory::EnsureArea(*f.owner, cell, center, 1.f,
                                               50) == copies);
  const auto saved = MpChangeForm::ToJson(f.owner->GetChangeForm());
  REQUIRE_THROWS(
    CampaignReferenceFactory::EnsureArea(*f.owner, cell, center, 100000.f, 5));
  REQUIRE_THROWS(
    CampaignReferenceFactory::EnsureArea(*f.owner, 0xf, center, 1.f, 5));
  REQUIRE(MpChangeForm::ToJson(f.owner->GetChangeForm()) == saved);
}

TEST_CASE("Allocation preparation failure leaves indexes and spatial state "
          "unpublished",
          "[Quest][CampaignAllocation]")
{
  AllocationWorld f;
  auto& world = f.server.worldState;
  const auto ownerState = MpChangeForm::ToJson(f.owner->GetChangeForm());
  const auto index = world.actorIdByPrivateIndexedProperty;
  const auto before = *world.GetAllForms(0xff);
  int callbacks = 0;
  world.AttachEspm(&GetEspmLoader(), [&] {
    if (++callbacks == 2)
      throw std::runtime_error("Allocation fixture failure");
    return FormCallbacks::DoNothing();
  });
  REQUIRE_THROWS_WITH(
    CampaignReferenceFactory::Ensure(*f.owner, { 0x1a677, 0x8a986 }, 5),
    "Allocation fixture failure");
  REQUIRE(MpChangeForm::ToJson(f.owner->GetChangeForm()) == ownerState);
  REQUIRE(world.actorIdByPrivateIndexedProperty == index);
  REQUIRE(*world.GetAllForms(0xff) == before);
  world.Tick();
  REQUIRE(f.storage->batches.empty());
  const auto ids =
    CampaignReferenceFactory::Ensure(*f.owner, { 0x1a677, 0x8a986 }, 5);
  const auto after = world.GetAllForms(0xff);
  REQUIRE(after->size() == before.size() + 2);
  for (const auto id : ids) {
    auto& copy = world.GetFormAt<MpObjectReference>(id);
    REQUIRE(world.LookupFormByIdx(copy.GetIdx()) == &copy);
    REQUIRE(std::find(after->begin(), after->end(), id) != after->end());
  }
}

TEST_CASE("Campaign copies keep initial death and enable-parent state",
          "[Quest][CampaignAllocation]")
{
  AllocationWorld f;
  auto& world = f.server.worldState;
  const auto ids = CampaignReferenceFactory::Ensure(
    *f.owner, { 0x2fb40, 0xe4cf6, 0xe4d27, 0xf3f36 }, 5);
  auto& corpse = world.GetFormAt<MpActor>(ids[0]);
  REQUIRE(corpse.IsDead());
  REQUIRE(corpse.GetChangeForm().actorValues.healthPercentage == 0);
  REQUIRE_FALSE(corpse.IsRespawning());
  REQUIRE(world.GetFormAt<MpObjectReference>(ids[1]).IsDisabled());
  REQUIRE_FALSE(world.GetFormAt<MpObjectReference>(ids[2]).IsDisabled());
  REQUIRE_FALSE(world.GetFormAt<MpObjectReference>(ids[3]).IsDisabled());
  // Another child must use this campaign's changed parent, not its ESM
  // default.
  world.GetFormAt<MpObjectReference>(ids[1]).Enable();
  const auto changed =
    CampaignReferenceFactory::Ensure(*f.owner, { 0xe4d26 }, 5);
  REQUIRE(world.GetFormAt<MpObjectReference>(changed.front()).IsDisabled());
}
