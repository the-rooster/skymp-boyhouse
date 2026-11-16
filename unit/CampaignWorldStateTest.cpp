#include "CampaignReferenceBinding.h"
#include "CampaignVisibilityBarrier.h"
#include "CampaignWorldStateApi.h"
#include "QuestJournal.h"
#include "TestUtils.hpp"
#include "save_storages/ISaveStorage.h"
#include "script_objects/CampaignFormObject.h"
#include <catch2/catch_all.hpp>

extern espm::Loader& GetEspmLoader();

namespace {
constexpr uint32_t kNpc = 0x1a677;
constexpr uint32_t kQuest = 0x4e50d;

struct VisibilityStorage
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
    static const std::string name = "campaign-visibility-test";
    return name;
  }
};

struct VisibilityWorld
{
  PartOne server;
  std::shared_ptr<VisibilityStorage> storage =
    std::make_shared<VisibilityStorage>();
  MpActor *owner, *guest, *copy;
  bool gated = false, failNotification = false;
  int before = 0, after = 0;
  bool synchronized = false;

  VisibilityWorld()
  {
    server.AttachEspm(&GetEspmLoader());
    server.AttachSaveStorage(storage);
    server.CreateActor(0xff003000, { 1, 2, 3 }, 0, 0x3c);
    server.CreateActor(0xff003001, { 1, 2, 3 }, 0, 0x3c);
    owner = &server.worldState.GetFormAt<MpActor>(0xff003000);
    guest = &server.worldState.GetFormAt<MpActor>(0xff003001);
    const auto instance =
      "campaign:" + std::to_string(owner->GetFormId()) + ":fixture";
    for (auto* player : { owner, guest }) {
      player->SetWorldInstance(instance);
      player->SetInventory(Inventory().AddItem(0xf, 10));
    }
    owner->SetPropertyValueDump(QuestJournal::kEnabled,
                                "[\"4e50d:Skyrim.esm\"]", false, false);
    auto callbacks = FormCallbacks::DoNothing();
    callbacks.unsubscribe = [&](auto*, auto*) {
      if (failNotification)
        throw std::runtime_error("Fixture subscription notification failed");
    };
    server.worldState.AddForm(
      std::make_unique<MpActor>(
        LocationalData{ { 1, 2, 3 }, {}, FormDesc::Tamriel() }, callbacks,
        0x13bbd),
      0xff003002);
    copy = &server.worldState.GetFormAt<MpActor>(0xff003002);
    copy->SetWorldInstance(instance);
    copy->SetInventory(Inventory().AddItem(0xf, 10));
    CampaignReferenceBinding::Bind(*owner, kNpc, *copy);
    server.worldState.Tick();
    storage->batches.clear();
  }
  void Barrier()
  {
    server.worldState.SetCampaignVisibilityCallbacks(
      [&](MpActor& actor) {
        REQUIRE(&actor == owner);
        ++before;
        gated = true;
      },
      [&](MpActor& actor, bool success) {
        REQUIRE(&actor == owner);
        REQUIRE(gated);
        ++after;
        synchronized = success;
      });
  }
  void StageAndReward()
  {
    REQUIRE(QuestJournal::Apply(
      *guest, { { "quest", kQuest }, { "op", "setStage" }, { "index", 10 } }));
    QuestJournal::EditInventory(*guest, *owner,
                                [](auto& inv) { inv.AddItem(0xf, 5); });
    QuestJournal::SetScriptState(*guest, { { "fixture", true } });
  }
};
}

TEST_CASE(
  "Campaign visibility publishes with journal inventory and atomic save",
  "[Quest][CampaignVisibility]")
{
  VisibilityWorld f;
  f.Barrier();
  REQUIRE(f.copy->GetListeners().count(f.owner) == 1);
  f.server.worldState.hosters[f.copy->GetFormId()] = f.owner->GetFormId();
  QuestJournal::Atomic(*f.guest, [&] {
    f.StageAndReward();
    QuestJournal::SetReferenceDisabled(*f.guest, *f.copy, true);
    REQUIRE(QuestJournal::ReadReference(*f.guest, *f.copy).isDisabled);
    REQUIRE_FALSE(f.copy->IsDisabled());
    REQUIRE(f.before == 0);
  });
  REQUIRE(f.copy->IsDisabled());
  REQUIRE(f.copy->GetListeners().empty());
  REQUIRE(f.copy->GetEmitters().empty());
  REQUIRE_FALSE(f.server.worldState.hosters.contains(f.copy->GetFormId()));
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 15);
  REQUIRE(f.guest->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(QuestJournal::Get(*f.owner, kQuest).at("stage") == 10);
  REQUIRE(f.before == 1);
  REQUIRE(f.after == 1);
  REQUIRE(f.synchronized);
  f.server.worldState.Tick();
  REQUIRE(f.storage->batches.size() == 1);
  const auto& batch = f.storage->batches.front();
  REQUIRE(batch.at(f.copy->GetIdx())->isDisabled);
  REQUIRE(batch.at(f.owner->GetIdx())->inv.GetItemCount(0xf) == 15);
  const auto journal = nlohmann::json::parse(
    batch.at(f.owner->GetIdx())
      ->dynamicFields.GetValueDump(QuestJournal::kProperty));
  REQUIRE(journal.at("quests").at("4e50d:Skyrim.esm").at("stage") == 10);
  QuestJournal::Atomic(*f.owner, [&] {
    QuestJournal::SetReferenceDisabled(*f.owner, *f.copy, false);
    REQUIRE_FALSE(QuestJournal::ReadReference(*f.owner, *f.copy).isDisabled);
    REQUIRE(f.copy->IsDisabled());
  });
  REQUIRE_FALSE(f.copy->IsDisabled());
  REQUIRE(f.copy->GetListeners().count(f.owner) == 1);
  REQUIRE(f.owner->GetEmitters().count(f.copy) == 1);
  REQUIRE(f.before == 2);
  REQUIRE(f.after == 2);
}

TEST_CASE("Campaign visibility refreshes native baselines behind paired gates",
          "[Quest][Projection][CampaignVisibility]")
{
  VisibilityWorld f;
  for (Networking::UserId user = 0; user < 2; ++user) {
    DoConnect(f.server, user);
    f.server.SetUserActor(
      user, user == 0 ? f.owner->GetFormId() : f.guest->GetFormId());
    f.server.serverState.userInfo[user]->gameplayBlocked = true;
    f.server.BeginPacketEpoch(user);
    f.server.serverState.userInfo[user]->gameplayBlocked = false;
  }
  bool expectDisabled = true;
  int captures = 0;
  CampaignVisibilityBarrier barrier(
    f.server,
    [&](auto, const auto& users) {
      REQUIRE(f.copy->IsDisabled() != expectDisabled);
      REQUIRE(users == CampaignVisibilityBarrier::UserIds{ 0, 1 });
      for (auto user : users) {
        REQUIRE(f.server.serverState.userInfo[user]->gameplayBlocked);
        f.server.BeginPacketEpoch(user);
      }
      REQUIRE_THROWS(
        f.server.worldState.SetCampaignVisibilityCallbacks({}, {}));
      return true;
    },
    [&](auto, const auto& users, bool synchronized) {
      REQUIRE(synchronized);
      REQUIRE(f.copy->IsDisabled() == expectDisabled);
      REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 15);
      REQUIRE(QuestJournal::Get(*f.owner, kQuest).at("stage") == 10);
      for (auto user : users) {
        // ProjectionFeed pauses with one token, then publish rotates again
        // for the fresh manifest/baseline pair. Both rotations remain gated.
        f.server.BeginPacketEpoch(user);
        const auto baseline =
          f.server.GetWorldBaseline(user, f.server.GetPacketEpoch(user));
        bool includesCopy = false;
        for (const auto& message : baseline.at("messages"))
          includesCopy |=
            message.value("refrId", uint64_t(0)) == f.copy->GetFormId();
        REQUIRE(includesCopy != expectDisabled);
        ++captures;
      }
      return true;
    },
    [](auto) { FAIL("A synchronized commit must not disconnect observers"); });
  f.server.worldState.SetCampaignVisibilityCallbacks(
    [&](auto& owner) { barrier.BeforeCommit(owner); },
    [&](auto& owner, bool synchronized) {
      barrier.AfterCommit(owner, synchronized);
    });
  QuestJournal::Atomic(*f.owner, [&] {
    f.StageAndReward();
    QuestJournal::SetReferenceDisabled(*f.owner, *f.copy, true);
  });
  REQUIRE(captures == 2);
  REQUIRE_FALSE(barrier.IsActive());
  expectDisabled = false;
  QuestJournal::Atomic(*f.guest, [&] {
    QuestJournal::SetReferenceDisabled(*f.guest, *f.copy, false);
  });
  REQUIRE(captures == 4);
  REQUIRE_FALSE(barrier.IsActive());
  REQUIRE(f.server.serverState.userInfo[0]->gameplayBlocked);
  REQUIRE(f.server.serverState.userInfo[1]->gameplayBlocked);
}

TEST_CASE("Campaign visibility failures before publication leave no effects",
          "[Quest][CampaignVisibility]")
{
  VisibilityWorld f;
  const std::string failure =
    GENERATE("handler", "missing barrier", "barrier", "storage");
  if (failure != "missing barrier")
    f.Barrier();
  if (failure == "barrier")
    f.server.worldState.SetCampaignVisibilityCallbacks(
      [&](MpActor&) {
        f.gated = true;
        throw std::runtime_error("Fixture gate failed");
      },
      [&](MpActor&, bool) { ++f.after; });
  if (failure == "storage")
    f.storage->atomic = false;
  const auto before = QuestJournal::Snapshot(*f.owner);
  REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
    f.StageAndReward();
    QuestJournal::SetReferenceDisabled(*f.guest, *f.copy, true);
    if (failure == "handler")
      throw std::runtime_error("Fixture handler failed");
  }));
  REQUIRE_FALSE(f.copy->IsDisabled());
  REQUIRE(f.copy->GetListeners().count(f.owner) == 1);
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
  REQUIRE(f.after == (failure == "barrier" ? 1 : 0));
  f.server.worldState.Tick();
  REQUIRE(f.storage->batches.empty());
}

TEST_CASE("Visibility barrier reentrancy cannot commit stale physical reads",
          "[Quest][CampaignVisibility]")
{
  VisibilityWorld f;
  f.server.worldState.SetCampaignVisibilityCallbacks(
    [&](MpActor&) {
      f.gated = true;
      f.copy->SetPos({ 4, 5, 6 });
    },
    [&](MpActor&, bool) { ++f.after; });
  const auto before = QuestJournal::Snapshot(*f.owner);
  REQUIRE_THROWS_WITH(
    QuestJournal::Atomic(*f.guest,
                         [&] {
                           f.StageAndReward();
                           QuestJournal::SetReferenceDisabled(*f.guest,
                                                              *f.copy, true);
                         }),
    Catch::Matchers::ContainsSubstring("changed during execution"));
  REQUIRE(f.gated);
  REQUIRE(f.after == 1);
  REQUIRE_FALSE(f.copy->IsDisabled());
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
}

TEST_CASE("Visibility publication remains committed if replication fails",
          "[Quest][CampaignVisibility]")
{
  VisibilityWorld f;
  f.Barrier();
  f.failNotification = true;
  REQUIRE_NOTHROW(QuestJournal::Atomic(*f.guest, [&] {
    f.StageAndReward();
    QuestJournal::SetReferenceDisabled(*f.guest, *f.copy, true);
  }));
  REQUIRE(f.copy->IsDisabled());
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 15);
  REQUIRE(QuestJournal::Get(*f.owner, kQuest).at("stage") == 10);
  REQUIRE(f.gated);
  REQUIRE(f.after == 1);
  REQUIRE_FALSE(f.synchronized);
  // Avoid the intentionally failing callback during fixture teardown.
  f.failNotification = false;
}

TEST_CASE("Reference snapshots reject stale state and keep staged inventory",
          "[Quest][CampaignVisibility]")
{
  VisibilityWorld f;
  REQUIRE_THROWS(QuestJournal::ReadReference(*f.owner, *f.copy));
  REQUIRE_THROWS(QuestJournal::Atomic(
    *f.guest, [&] { QuestJournal::ReadReference(*f.guest, *f.guest); }));
  QuestJournal::Atomic(*f.guest, [&] {
    const auto& snapshot = QuestJournal::ReadReference(*f.guest, *f.copy);
    QuestJournal::EditInventory(*f.guest, *f.copy,
                                [](auto& inv) { inv.AddItem(0xf, 3); });
    REQUIRE(snapshot.inv.GetItemCount(0xf) == 13);
    REQUIRE(f.copy->GetInventory().GetItemCount(0xf) == 10);
  });
  const std::string change =
    GENERATE("position", "phase", "disabled", "faction");
  const auto before = QuestJournal::Snapshot(*f.owner);
  REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
    f.StageAndReward();
    QuestJournal::ReadReference(*f.guest, *f.copy);
    if (change == "position")
      f.copy->SetPos({ 4, 5, 6 });
    if (change == "phase") {
      const auto phase = f.copy->GetWorldInstance();
      f.copy->SetWorldInstance("");
      f.copy->SetWorldInstance(phase);
    }
    if (change == "disabled")
      f.copy->Disable();
    if (change == "faction")
      f.copy->AddToFaction(
        Faction{ FormDesc::FromFormId(0x5c84d, f.server.worldState.espmFiles),
                 1 },
        false);
  }));
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
}

TEST_CASE("Journal-only reads reject invalidated campaign bindings",
          "[Quest][CampaignVisibility]")
{
  VisibilityWorld f;
  const auto before = QuestJournal::Snapshot(*f.owner);
  REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
    QuestJournal::ReadReference(*f.guest, *f.copy);
    REQUIRE(QuestJournal::Apply(
      *f.guest,
      { { "quest", kQuest }, { "op", "setStage" }, { "index", 10 } }));
    f.owner->SetPropertyValueDump("private.boyhouse.campaignIdentity",
                                  "\"11111111111111111111111111111111\"",
                                  false, false);
  }));
  REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE_FALSE(f.copy->IsDisabled());
}

TEST_CASE(
  "No-op visibility needs no barrier but unsupported targets are refused",
  "[Quest][CampaignVisibility]")
{
  VisibilityWorld f;
  QuestJournal::Atomic(*f.guest, [&] {
    QuestJournal::SetReferenceDisabled(*f.guest, *f.copy, true);
    QuestJournal::SetReferenceDisabled(*f.guest, *f.copy, false);
  });
  REQUIRE_FALSE(f.copy->IsDisabled());
  REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
    QuestJournal::SetReferenceDisabled(*f.guest, *f.owner, true);
  }));
  struct Occupied : MpActor::DisableEventSink
  {
    void BeforeDisable(MpActor&) override
    {
      FAIL("Disable callback must not run");
    }
  };
  const auto occupied = std::make_shared<Occupied>();
  f.copy->AddEventSink(occupied);
  REQUIRE_THROWS_WITH(
    QuestJournal::Atomic(
      *f.owner,
      [&] { QuestJournal::SetReferenceDisabled(*f.owner, *f.copy, true); }),
    Catch::Matchers::ContainsSubstring("transactional occupation"));
  REQUIRE_FALSE(f.copy->IsDisabled());
  f.copy->RemoveEventSink(occupied);
  REQUIRE(f.server.worldState.HasCampaignEnableLinks(0xf3f36));
  REQUIRE(f.server.worldState.HasCampaignEnableLinks(0xf3f1a));
  // Browser's REFR aggregate also contains ACHR; actors and their parents
  // must not disappear from the index when querying that aggregate once.
  REQUIRE(f.server.worldState.GetEspm()
            .GetBrowser()
            .LookupById(0x3e4c0)
            .rec->GetType() == "ACHR");
  REQUIRE(f.server.worldState.HasCampaignEnableLinks(0x3e4c0));
  REQUIRE(f.server.worldState.HasCampaignEnableLinks(0x10f8e0));
}

TEST_CASE(
  "Visibility native API stages state and refuses fades or wrong signatures",
  "[Quest][CampaignVisibility]")
{
  VisibilityWorld f;
  f.Barrier();
  VirtualMachine vm(std::vector<PexScript::Lazy>{});
  CampaignWorldStateApi::Register(vm, *f.guest, [&](const VarValue& value) {
    if (value.GetType() != VarValue::kType_Object)
      throw std::runtime_error("Fixture expected form");
    const auto* object =
      dynamic_cast<CampaignFormObject*>(static_cast<IGameObject*>(value));
    if (!object || object->OwnerId() != f.owner->GetFormId())
      throw std::runtime_error("Fixture expected campaign form");
    return object->FormId();
  });
  const auto logical = std::make_shared<CampaignFormObject>(
    f.server.worldState.GetEspm().GetBrowser().LookupById(kNpc),
    f.owner->GetFormId(), kNpc);
  const auto get =
    vm.GetFunctionImplementation("ObjectReference", "IsDisabled", false);
  const auto disable =
    vm.GetFunctionImplementation("ObjectReference", "Disable", false);
  const auto enable =
    vm.GetFunctionImplementation("ObjectReference", "EnableNoWait", false);
  REQUIRE(get);
  REQUIRE(disable);
  REQUIRE(enable);
  QuestJournal::Atomic(*f.guest, [&] {
    REQUIRE_FALSE(static_cast<bool>(get(VarValue(logical.get()), {})));
    disable(VarValue(logical.get()), { VarValue(false) });
    REQUIRE(static_cast<bool>(get(VarValue(logical.get()), {})));
    REQUIRE_FALSE(f.copy->IsDisabled());
  });
  REQUIRE(f.copy->IsDisabled());
  QuestJournal::Atomic(
    *f.guest, [&] { enable(VarValue(logical.get()), { VarValue(false) }); });
  REQUIRE_FALSE(f.copy->IsDisabled());
  for (const auto& args :
       { std::vector<VarValue>{}, std::vector{ VarValue(true) },
         std::vector{ VarValue(1) } })
    REQUIRE_THROWS(QuestJournal::Atomic(
      *f.guest, [&] { disable(VarValue(logical.get()), args); }));
  REQUIRE_FALSE(f.copy->IsDisabled());
}
