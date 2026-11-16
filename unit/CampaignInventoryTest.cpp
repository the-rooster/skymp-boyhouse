#include "QuestJournal.h"
#include "TestUtils.hpp"
#include "save_storages/ISaveStorage.h"
#include <catch2/catch_all.hpp>
#include <limits>

extern espm::Loader& GetEspmLoader();

namespace {
struct RecordingStorage
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
    static const std::string name = "campaign-test";
    return name;
  }
};

struct InventoryWorld
{
  PartOne server;
  std::shared_ptr<RecordingStorage> storage =
    std::make_shared<RecordingStorage>();
  MpActor *owner, *guest;
  MpObjectReference* chest;
  InventoryWorld()
  {
    server.AttachEspm(&GetEspmLoader());
    server.AttachSaveStorage(storage);
    server.CreateActor(0xff001000, { 1, 2, 3 }, 0, 0x3c);
    server.CreateActor(0xff001001, { 1, 2, 3 }, 0, 0x3c);
    owner = &server.worldState.GetFormAt<MpActor>(0xff001000);
    guest = &server.worldState.GetFormAt<MpActor>(0xff001001);
    owner->SetPropertyValueDump(QuestJournal::kEnabled,
                                "[\"4e50d:Skyrim.esm\"]", false, false);
    guest->SetPropertyValueDump(QuestJournal::kEnabled,
                                "[\"4e50d:Skyrim.esm\"]", false, false);
    server.worldState.AddForm(
      std::make_unique<MpObjectReference>(
        LocationalData{ { 1, 2, 3 }, { 0, 0, 0 }, FormDesc::Tamriel() },
        FormCallbacks::DoNothing(), 0x10cd5b, "CONT"),
      0xff001002);
    chest = &server.worldState.GetFormAt<MpObjectReference>(0xff001002);
    const auto instance =
      "campaign:" + std::to_string(owner->GetFormId()) + ":fixture";
    for (auto* reference :
         std::vector<MpObjectReference*>{ owner, guest, chest }) {
      reference->SetWorldInstance(instance);
      reference->SetInventory(Inventory().AddItem(0xf, 10));
    }
    server.worldState.Tick();
    storage->batches.clear();
  }
  void Stage()
  {
    QuestJournal::Apply(
      *guest, { { "quest", 0x4e50d }, { "op", "setStage" }, { "index", 10 } });
  }
  void Reward(MpObjectReference& target, uint32_t count)
  {
    QuestJournal::EditInventory(
      *guest, target, [&](Inventory& inv) { inv.AddItem(0xf, count); });
  }
};
}

TEST_CASE(
  "Campaign journals and inventory transfers publish and save together",
  "[Quest][CampaignInventory]")
{
  InventoryWorld f;
  QuestJournal::Atomic(*f.guest, [&] {
    f.Stage();
    QuestJournal::SetScriptState(*f.guest, { { "fixture", true } });
    f.Reward(*f.owner, 6);
    QuestJournal::EditInventory(*f.guest, *f.chest, [](Inventory& inventory) {
      inventory.RemoveItems({ { 0xf, 6 } });
    });
    REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
    REQUIRE(f.chest->GetInventory().GetItemCount(0xf) == 10);
    REQUIRE(
      QuestJournal::ReadInventory(*f.guest, *f.owner).GetItemCount(0xf) == 16);
    REQUIRE(
      QuestJournal::ReadInventory(*f.guest, *f.chest).GetItemCount(0xf) == 4);
  });
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 16);
  REQUIRE(f.chest->GetInventory().GetItemCount(0xf) == 4);
  REQUIRE(f.guest->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(QuestJournal::Get(*f.owner, 0x4e50d).at("stage") == 10);
  f.server.worldState.Tick();
  REQUIRE(f.storage->batches.size() == 1);
  const auto& batch = f.storage->batches[0];
  REQUIRE(batch[f.owner->GetIdx()]->inv.GetItemCount(0xf) == 16);
  REQUIRE(batch[f.chest->GetIdx()]->inv.GetItemCount(0xf) == 4);
  const auto journal =
    nlohmann::json::parse(batch[f.owner->GetIdx()]->dynamicFields.GetValueDump(
      QuestJournal::kProperty));
  REQUIRE(journal.at("quests").at("4e50d:Skyrim.esm").at("stage") == 10);
  REQUIRE(journal.at("vm").at("fixture") == true);
  f.guest->SetWorldInstance("");
  REQUIRE(QuestJournal::Get(*f.guest, 0x4e50d).at("stage") == 0);
}

TEST_CASE(
  "Failed campaign inventory operations leave no reward or journal change",
  "[Quest][CampaignInventory]")
{
  InventoryWorld f;
  const auto before = QuestJournal::Snapshot(*f.owner);
  REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
    f.Stage();
    QuestJournal::SetScriptState(*f.guest, { { "fixture", true } });
    f.Reward(*f.owner, 6);
    f.Reward(*f.chest, std::numeric_limits<uint32_t>::max());
  }));
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(f.chest->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
  REQUIRE(QuestJournal::ScriptState(*f.owner).empty());
  f.server.worldState.Tick();
  REQUIRE(f.storage->batches.empty());
  REQUIRE_THROWS(QuestJournal::ReadInventory(*f.owner, *f.owner));
}

TEST_CASE(
  "Campaign inventory refuses guests, shared objects and unsafe storage",
  "[Quest][CampaignInventory]")
{
  InventoryWorld f;
  SECTION("another player")
  {
    REQUIRE_THROWS_WITH(
      QuestJournal::Atomic(*f.guest, [&] { f.Reward(*f.guest, 1); }),
      Catch::Matchers::ContainsSubstring("another player"));
  }
  SECTION("shared object")
  {
    f.chest->SetWorldInstance("");
    REQUIRE_THROWS_WITH(
      QuestJournal::Atomic(*f.guest, [&] { f.Reward(*f.chest, 1); }),
      Catch::Matchers::ContainsSubstring("private instance"));
  }
  SECTION("another campaign")
  {
    f.chest->SetWorldInstance("campaign:4278195000:fixture");
    REQUIRE_THROWS(
      QuestJournal::Atomic(*f.guest, [&] { f.Reward(*f.chest, 1); }));
  }
  SECTION("non-atomic persistence")
  {
    f.storage->atomic = false;
    REQUIRE_THROWS_WITH(
      QuestJournal::Atomic(*f.guest,
                           [&] {
                             f.Stage();
                             f.Reward(*f.owner, 1);
                           }),
      Catch::Matchers::ContainsSubstring("atomic save storage"));
    REQUIRE(QuestJournal::Get(*f.owner, 0x4e50d).at("stage") == 0);
  }
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(f.guest->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(f.chest->GetInventory().GetItemCount(0xf) == 10);
}

TEST_CASE("Campaign inventory detects stale references and conflicting edits",
          "[Quest][CampaignInventory]")
{
  InventoryWorld f;
  const auto before = QuestJournal::Snapshot(*f.owner);
  SECTION("scope changed away and back")
  {
    REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
      f.Stage();
      f.Reward(*f.chest, 1);
      const auto instance = f.chest->GetWorldInstance();
      f.chest->SetWorldInstance("");
      f.chest->SetWorldInstance(instance);
    }));
  }
  SECTION("inventory changed outside the transaction")
  {
    REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
      f.Stage();
      f.Reward(*f.owner, 1);
      f.owner->SetInventory(Inventory().AddItem(0xf, 20));
    }));
    REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 20);
  }
  SECTION("journal changed outside the transaction")
  {
    REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
      f.Stage();
      f.Reward(*f.owner, 1);
      f.owner->SetPropertyValueDump(QuestJournal::kProperty, "{}", false,
                                    false);
    }));
    REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
    f.owner->SetPropertyValueDump(QuestJournal::kProperty, "null", false,
                                  false);
  }
  SECTION("deleted target")
  {
    REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
      f.Stage();
      f.Reward(*f.chest, 1);
      f.server.worldState.DestroyForm(f.chest->GetFormId());
    }));
  }
  REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
}
