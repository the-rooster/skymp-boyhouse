#include "CampaignReferenceFactory.h"
#include "CampaignReferenceBinding.h"
#include "CampaignReferenceScripts.h"
#include "QuestScriptRuntime.h"
#include "LocationalDataUtils.h"
#include "QuestJournal.h"
#include "QuestVmState.h"
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

TEST_CASE("Campaign NPC scripts reuse the persisted leveled template choice",
          "[Quest][CampaignAllocation][ReferenceScripts]")
{
  AllocationWorld f;
  auto& world = f.server.worldState;
  // This rabbit is next to the native client transition fixture. Its script
  // inheritance reaches a leveled list even though the selected NPC has no
  // attached scripts. Looking only at plugin records used to abort admission.
  constexpr uint32_t original = 1080569;
  const auto ids = CampaignReferenceFactory::Ensure(*f.owner, { original }, 5);
  const auto selected = world.GetFormAt<MpActor>(ids.front()).GetChangeForm().templateChain;
  const auto reference = world.GetEspm().GetBrowser().LookupById(original);
  REQUIRE(selected.size() >= 2);
  REQUIRE_THROWS(CampaignReferenceScripts::Read(world, reference));
  int reads = 0;
  const auto scripts = CampaignReferenceScripts::Read(world, reference, [&] {
    ++reads;
    return selected;
  });
  REQUIRE(reads == 1);
  REQUIRE(scripts.empty());
  QuestScriptRuntime::ExecuteReferenceLoad(*f.owner, original, {});
  REQUIRE(QuestJournal::ReferenceInitialized(*f.owner, original));
  REQUIRE(world.GetFormAt<MpActor>(ids.front()).GetChangeForm().templateChain == selected);
  REQUIRE_THROWS(CampaignReferenceScripts::Read(world, reference, [] {
    return std::vector<FormDesc>{};
  }));
  REQUIRE_THROWS(CampaignReferenceScripts::Read(world, reference, [&] {
    return std::vector<FormDesc>{ selected.front() };
  }));
  REQUIRE_THROWS(CampaignReferenceScripts::Read(world, reference, [&] {
    return std::vector<FormDesc>{ selected.front(), FormDesc::FromFormId(0xf, world.espmFiles) };
  }));
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

TEST_CASE("Destination preparation does not admit or change the player",
          "[Quest][CampaignAllocation]")
{
  AllocationWorld f;
  auto& world = f.server.worldState;
  f.owner->SetWorldInstance("");
  const auto epoch = f.owner->GetWorldInstanceEpoch();
  const auto inventory = f.owner->GetInventory().ToJson();
  const auto ids = CampaignReferenceFactory::Prepare(*f.owner, "fixture",
                                                     { 0x1a677, 0x8a986 }, 5);
  REQUIRE(f.owner->GetWorldInstance().empty());
  REQUIRE(f.owner->GetWorldInstanceEpoch() == epoch);
  REQUIRE(f.owner->GetInventory().ToJson() == inventory);
  for (const auto id : ids) {
    const auto& copy = world.GetFormAt<MpObjectReference>(id);
    REQUIRE(copy.GetWorldInstance() == "campaign:4278198273:fixture");
    REQUIRE_FALSE(copy.SharesWorldInstance(*f.owner));
  }
  REQUIRE_THROWS(CampaignReferenceBinding::Resolve(*f.owner, 0x1a677));
  world.GetFormAt<MpActor>(ids[0]).SetInventory(Inventory().AddItem(0xf, 19));
  REQUIRE(CampaignReferenceFactory::Prepare(*f.owner, "fixture",
                                            { 0x1a677, 0x8a986 }, 99) == ids);
  f.owner->SetWorldInstance("campaign:4278198273:fixture");
  REQUIRE(CampaignReferenceBinding::Resolve(*f.owner, 0x1a677).GetFormId() ==
          ids[0]);
  REQUIRE(world.GetFormAt<MpActor>(ids[0]).GetInventory().GetItemCount(0xf) ==
          19);
}

TEST_CASE(
  "Guest recovery preparation reads personal VM state without the host",
  "[Quest][CampaignAllocation]")
{
  AllocationWorld f;
  QuestVmState personal(nlohmann::json::object(), {}, {});
  personal.SetGlobal("73f10:Skyrim.esm", 7);
  QuestJournal::Atomic(*f.owner, [&] {
    QuestJournal::SetScriptState(*f.owner, personal.Save());
  });
  auto badHost = nlohmann::json{ { "schemaVersion", 1 },
                                 { "revision", uint64_t(0) },
                                 { "quests", nlohmann::json::object() },
                                 { "vm", "invalid host VM" } };
  f.other->SetPropertyValueDump(QuestJournal::kProperty, badHost.dump(), false,
                                false);
  SECTION("present host with unusable VM")
  {
    f.owner->SetWorldInstance(f.other->GetWorldInstance());
  }
  SECTION("absent host after restart")
  {
    f.owner->SetWorldInstance("campaign:4294967000:fixture");
  }
  const auto phase = f.owner->GetWorldInstance();
  const auto epoch = f.owner->GetWorldInstanceEpoch();
  REQUIRE_THROWS(QuestJournal::ScriptState(*f.owner));
  const auto ids =
    CampaignReferenceFactory::Prepare(*f.owner, "fixture", { 0x1a677 }, 5);
  REQUIRE(ids.size() == 1);
  REQUIRE(f.owner->GetWorldInstance() == phase);
  REQUIRE(f.owner->GetWorldInstanceEpoch() == epoch);
  REQUIRE(QuestJournal::PersonalScriptState(*f.owner) == personal.Save());
  REQUIRE(f.other->GetDynamicFields().GetValueDump(QuestJournal::kProperty) ==
          badHost.dump());
  // A malformed personal VM must not silently fall back to the host's state.
  f.owner->SetPropertyValueDump(QuestJournal::kProperty, badHost.dump(), false,
                                false);
  const auto forms = *f.server.worldState.GetAllForms(0xff);
  REQUIRE_THROWS(
    CampaignReferenceFactory::Prepare(*f.owner, "fixture", { 0x8a986 }, 5));
  REQUIRE(*f.server.worldState.GetAllForms(0xff) == forms);
}

TEST_CASE("Invalid destination preparation keeps the old phase and ledger",
          "[Quest][CampaignAllocation]")
{
  AllocationWorld f;
  f.owner->SetWorldInstance(f.other->GetWorldInstance());
  const auto before = MpChangeForm::ToJson(f.owner->GetChangeForm());
  const auto forms = *f.server.worldState.GetAllForms(0xff);
  for (const std::string area :
       { "", "bad:area", "bad area", "campaign:4278198274:fixture" })
    REQUIRE_THROWS(
      CampaignReferenceFactory::Prepare(*f.owner, area, { 0x1a677 }, 5));
  REQUIRE_THROWS(CampaignReferenceFactory::Prepare(
    *f.owner, std::string(49, 'a'), { 0x1a677 }, 5));
  REQUIRE_THROWS(CampaignReferenceFactory::Prepare(*f.owner, "fixture",
                                                   { 0x1a677, 0xf }, 5));
  REQUIRE(MpChangeForm::ToJson(f.owner->GetChangeForm()) == before);
  REQUIRE(*f.server.worldState.GetAllForms(0xff) == forms);
}

TEST_CASE("An authored area can be prepared and restored before admission",
          "[Quest][CampaignAllocation]")
{
  AllocationWorld f;
  f.owner->SetWorldInstance("");
  auto& world = f.server.worldState;
  const auto source = world.GetEspm().GetBrowser().LookupById(0x8a986);
  const auto data = reinterpret_cast<const espm::REFR*>(source.rec)
                      ->GetData(world.GetEspmCache());
  const auto cell = source.ToGlobalId(
    espm::GetWorldOrCell(world.GetEspm().GetBrowser(), source.rec));
  const auto center = LocationalDataUtils::GetPos(data.loc);
  const auto ids = CampaignReferenceFactory::PrepareArea(*f.owner, "fixture",
                                                         cell, center, 1.f, 5);
  REQUIRE_FALSE(ids.empty());
  REQUIRE(f.owner->GetWorldInstance().empty());
  auto savedOwner = f.owner->GetChangeForm();
  savedOwner.profileId = 42;
  savedOwner.isDisabled = true;
  PartOne restarted;
  restarted.AttachEspm(&GetEspmLoader());
  for (const auto id : ids)
    restarted.worldState.LoadChangeForm(
      world.GetFormAt<MpObjectReference>(id).GetChangeForm(),
      FormCallbacks::DoNothing());
  restarted.worldState.LoadChangeForm(savedOwner, FormCallbacks::DoNothing());
  auto& owner = restarted.worldState.GetFormAt<MpActor>(f.owner->GetFormId());
  REQUIRE(CampaignReferenceFactory::PrepareArea(owner, "fixture", cell, center,
                                                1.f, 50) == ids);
  REQUIRE(owner.GetWorldInstance().empty());
  REQUIRE(owner.IsDisabled());
}

TEST_CASE("Projection manifests include hidden originals only from the "
          "current campaign area",
          "[Quest][CampaignAllocation][Projection]")
{
  AllocationWorld f;
  auto& world = f.server.worldState;
  const auto ids =
    CampaignReferenceFactory::Ensure(*f.owner, { 0x1a677, 0x8a986 }, 5);
  CampaignReferenceFactory::Prepare(*f.owner, "elsewhere", { 0xe4cf6 }, 5);
  const auto otherIds =
    CampaignReferenceFactory::Ensure(*f.other, { 0x1a677 }, 5);
  world.GetFormAt<MpActor>(ids[0]).Disable();
  world.GetFormAt<MpObjectReference>(ids[1]).Delete();
  const auto manifest = CampaignReferenceFactory::Snapshot(*f.owner);
  REQUIRE(manifest.at("schemaVersion") == 1);
  REQUIRE(manifest.at("actor") == f.owner->GetFormId());
  REQUIRE(manifest.at("campaignOwner") == f.owner->GetFormId());
  REQUIRE(manifest.at("instance") == f.owner->GetWorldInstance());
  REQUIRE(manifest.at("identity").get<std::string>().size() == 32);
  const auto& refs = manifest.at("references");
  REQUIRE(refs.size() == 2);
  REQUIRE(refs[0].at("original") == 0x1a677);
  REQUIRE(refs[0].at("copy") == ids[0]);
  REQUIRE(refs[0].at("baseId") == 0x13bbd);
  REQUIRE(refs[0].at("kind") == "actor");
  REQUIRE(refs[0].at("disabled") == true);
  REQUIRE(refs[1].at("deleted") == true);
  REQUIRE(refs[1].at("kind") == "object");
  REQUIRE_FALSE(manifest.contains("vm"));
  f.other->SetWorldInstance(f.owner->GetWorldInstance());
  const auto guest = CampaignReferenceFactory::Snapshot(*f.other);
  REQUIRE(guest.at("actor") == f.other->GetFormId());
  REQUIRE(guest.at("campaignOwner") == f.owner->GetFormId());
  REQUIRE(guest.at("identity") == manifest.at("identity"));
  REQUIRE(guest.at("references") == refs);
  REQUIRE(otherIds.front() != guest.at("references")[0].at("copy"));
  f.other->SetWorldInstance("");
  const auto shared = CampaignReferenceFactory::Snapshot(*f.other);
  REQUIRE(shared.at("references").empty());
  REQUIRE(shared.at("identity") == "");
  REQUIRE(shared.at("campaignOwner") == f.other->GetFormId());
  REQUIRE(shared.at("contextEpoch") != guest.at("contextEpoch"));
}

TEST_CASE("Projection reads neither create identities nor silently repair "
          "corrupt bindings",
          "[Quest][CampaignAllocation][Projection]")
{
  AllocationWorld f;
  const auto before = MpChangeForm::ToJson(f.owner->GetChangeForm());
  REQUIRE(
    CampaignReferenceFactory::Snapshot(*f.owner).at("references").empty());
  REQUIRE(CampaignReferenceFactory::Snapshot(*f.owner).at("identity") == "");
  REQUIRE(MpChangeForm::ToJson(f.owner->GetChangeForm()) == before);
  CampaignReferenceFactory::Ensure(*f.owner, { 0x1a677 }, 5);
  SECTION("missing identity")
  {
    f.owner->SetPropertyValueDump("private.boyhouse.campaignIdentity", "null",
                                  false, false);
  }
  SECTION("missing allocation")
  {
    f.owner->SetPropertyValueDump(
      CampaignReferenceFactory::kAllocations,
      R"({"schemaVersion":1,"copies":{"1a677:Skyrim.esm":"fefefe"}})", false,
      false);
  }
  const auto corrupt = MpChangeForm::ToJson(f.owner->GetChangeForm());
  REQUIRE_THROWS(CampaignReferenceFactory::Snapshot(*f.owner));
  REQUIRE(MpChangeForm::ToJson(f.owner->GetChangeForm()) == corrupt);
}
