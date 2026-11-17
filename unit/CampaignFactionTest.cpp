#include "CampaignFactionApi.h"
#include "CampaignReferenceBinding.h"
#include "CampaignReferenceFactory.h"
#include "QuestJournal.h"
#include "TestUtils.hpp"
#include "save_storages/ISaveStorage.h"
#include "script_objects/CampaignFormObject.h"
#include <catch2/catch_all.hpp>
#include <filesystem>
#include <fstream>
#include <random>

extern espm::Loader& GetEspmLoader();

namespace {
constexpr uint32_t kNpc = 0x1a677, kQuest = 0x4e50d;
constexpr uint32_t kFaction = 0x5c84d, kOtherFaction = 0x19809;
struct FactionStorage
  : Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>
{
  bool atomic = true;
  std::vector<std::vector<std::optional<MpChangeForm>>> batches;
  std::map<FormDesc, MpChangeForm> saved;
  bool SupportsAtomicBatches() const noexcept override { return atomic; }
  void IterateSync(const IterateSyncCallback& callback) override
  {
    for (const auto& [id, form] : saved)
      callback(form);
  }
  void Upsert(std::vector<std::optional<MpChangeForm>>&& forms,
              const UpsertCallback& callback) override
  {
    for (const auto& form : forms)
      if (form)
        saved.insert_or_assign(form->formDesc, *form);
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
    static const std::string name = "campaign-factions-test";
    return name;
  }
};
struct FactionWorld
{
  PartOne server;
  std::shared_ptr<FactionStorage> storage = std::make_shared<FactionStorage>();
  MpActor *owner, *guest, *copy;
  VirtualMachine vm{ std::vector<PexScript::Lazy>{} };
  std::map<uint32_t, std::shared_ptr<CampaignFormObject>> forms;
  unsigned before = 0, after = 0;
  bool synchronized = false;
  FactionWorld()
  {
    server.AttachEspm(&GetEspmLoader());
    server.AttachSaveStorage(storage);
    server.CreateActor(0xff004000, { 1, 2, 3 }, 0, 0x3c);
    server.CreateActor(0xff004001, { 1, 2, 3 }, 0, 0x3c);
    owner = &server.worldState.GetFormAt<MpActor>(0xff004000);
    guest = &server.worldState.GetFormAt<MpActor>(0xff004001);
    const auto instance =
      "campaign:" + std::to_string(owner->GetFormId()) + ":factions";
    for (auto* actor : { owner, guest }) {
      actor->SetWorldInstance(instance);
      actor->SetInventory(Inventory().AddItem(0xf, 10));
    }
    owner->SetPropertyValueDump(QuestJournal::kEnabled,
                                "[\"4e50d:Skyrim.esm\"]", false, false);
    copy = &server.worldState.GetFormAt<MpActor>(
      CampaignReferenceFactory::Ensure(*owner, { kNpc }, 5).front());
    CampaignFactionApi::Register(vm, *guest, [&](const VarValue& value) {
      if (value.GetType() != VarValue::kType_Object)
        throw std::runtime_error("Expected campaign form");
      const auto* form =
        dynamic_cast<CampaignFormObject*>(static_cast<IGameObject*>(value));
      if (!form || form->OwnerId() != owner->GetFormId())
        throw std::runtime_error("Expected owning campaign form");
      return form->FormId();
    });
    server.worldState.Tick();
    storage->batches.clear();
  }
  VarValue Form(uint32_t id)
  {
    auto& value = forms[id];
    if (!value)
      value = std::make_shared<CampaignFormObject>(
        server.worldState.GetEspm().GetBrowser().LookupById(id),
        owner->GetFormId(), id);
    return VarValue(value.get());
  }
  VarValue Call(const char* name, uint32_t target, std::vector<VarValue> args)
  {
    const auto call = vm.GetFunctionImplementation("Actor", name, false);
    REQUIRE(call);
    return call(Form(target), std::move(args));
  }
  void Barrier()
  {
    server.worldState.SetCampaignVisibilityCallbacks(
      [&](MpActor& actor) {
        REQUIRE(&actor == owner);
        ++before;
      },
      [&](MpActor& actor, bool success) {
        REQUIRE(&actor == owner);
        ++after;
        synchronized = success;
      });
  }
  void Empty(MpActor& actor)
  {
    auto saved = actor.GetChangeForm();
    saved.factions = std::vector<Faction>{};
    actor.ApplyChangeForm(saved);
  }
  void StageReward()
  {
    REQUIRE(QuestJournal::Apply(
      *guest, { { "quest", kQuest }, { "op", "setStage" }, { "index", 10 } }));
    QuestJournal::EditInventory(*guest, *owner,
                                [](auto& inv) { inv.AddItem(0xf, 5); });
    QuestJournal::SetScriptState(*guest, { { "factions", true } });
  }
};

struct FactionMetadataFile
{
  std::filesystem::path directory;
  FactionMetadataFile()
  {
    std::random_device random;
    for (int attempt = 0; attempt < 20; ++attempt) {
      auto candidate = std::filesystem::temp_directory_path() /
        ("skymp-factions-unit-" + std::to_string(random()) + "-" +
         std::to_string(random()));
      if (std::filesystem::create_directory(candidate)) {
        directory = candidate;
        break;
      }
    }
    if (directory.empty())
      throw std::runtime_error(
        "Cannot create isolated faction metadata fixture");
    const auto bytes = [](uint32_t value, size_t count) {
      std::string result(count, '\0');
      for (size_t i = 0; i < count; ++i)
        result[i] = static_cast<char>((value >> (i * 8)) & 0xff);
      return result;
    };
    const auto field = [&](const char* tag, const std::string& data) {
      return std::string(tag, 4) +
        bytes(static_cast<uint32_t>(data.size()), 2) + data;
    };
    const auto record = [&](const char* type, uint32_t id,
                            const std::string& fields) {
      return std::string(type, 4) +
        bytes(static_cast<uint32_t>(fields.size()), 4) + bytes(0, 4) +
        bytes(id, 4) + std::string(8, '\0') + fields;
    };
    std::string plugin = record("TES4", 0, "");
    for (uint32_t id : { 0x100u, 0x101u, 0x102u, 0x103u })
      plugin +=
        record("FACT", id,
               field("DATA", bytes((id == 0x100 || id == 0x102) ? 4 : 0, 4)));
    auto actorData = bytes(0x20, 4) + std::string(20, '\0');
    const auto factions =
      field("SNAM", bytes(0x100, 4) + bytes(3, 1) + std::string(3, '\0')) +
      field("SNAM", bytes(0x101, 4) + bytes(1, 1) + std::string(3, '\0')) +
      field("SNAM", bytes(0x102, 4) + bytes(0x80, 1) + std::string(3, '\0'));
    plugin += record("NPC_", 7, field("ACBS", actorData) + factions);
    std::ofstream out(File(), std::ios::binary);
    out.write(plugin.data(), static_cast<std::streamsize>(plugin.size()));
    if (!out)
      throw std::runtime_error("Cannot write faction metadata fixture");
  }
  ~FactionMetadataFile()
  {
    // Only our one fixture and exclusively created directory are removed.
    std::error_code error;
    std::filesystem::remove(File(), error);
    std::filesystem::remove(directory, error);
  }
  std::filesystem::path File() const
  {
    return directory / "FactionFixture.esm";
  }
};
}

TEST_CASE(
  "Campaign faction reads derive templates without live lazy initialization",
  "[Quest][CampaignFactions]")
{
  FactionWorld f;
  const auto seeded = f.copy->GetChangeForm().factions;
  REQUIRE(seeded);
  REQUIRE_FALSE(seeded->empty());
  auto saved = f.copy->GetChangeForm();
  saved.factions.reset();
  f.copy->ApplyChangeForm(saved);
  f.server.worldState.Tick();
  f.storage->batches.clear();
  QuestJournal::Atomic(*f.guest, [&] {
    const auto& read = QuestJournal::ReadFactions(*f.guest, *f.copy);
    REQUIRE(read.size() == seeded->size());
    for (size_t i = 0; i < read.size(); ++i) {
      REQUIRE(read[i].formDesc == seeded->at(i).formDesc);
      REQUIRE(read[i].rank == seeded->at(i).rank);
    }
    REQUIRE_FALSE(f.copy->GetChangeForm().factions);
  });
  REQUIRE_FALSE(f.copy->GetChangeForm().factions);
  f.server.worldState.Tick();
  REQUIRE(f.storage->batches.empty());
  f.Empty(*f.copy);
  QuestJournal::Atomic(*f.guest, [&] {
    REQUIRE(QuestJournal::ReadFactions(*f.guest, *f.copy).empty());
  });
  REQUIRE(f.copy->GetFactions(-128, 127)
            .empty()); // Saved empty cannot reload base entries.
}

TEST_CASE(
  "Campaign faction natives stage signed ranks and preserve AddToFaction rank",
  "[Quest][CampaignFactions]")
{
  FactionWorld f;
  f.Empty(*f.copy);
  f.Empty(*f.owner);
  f.Barrier();
  QuestJournal::Atomic(*f.guest, [&] {
    REQUIRE(int32_t(f.Call("GetFactionRank", kNpc, { f.Form(kFaction) })) ==
            -2);
    REQUIRE_FALSE(bool(f.Call("IsInFaction", kNpc, { f.Form(kFaction) })));
    f.Call("AddToFaction", kNpc, { f.Form(kFaction) });
    REQUIRE(int32_t(f.Call("GetFactionRank", kNpc, { f.Form(kFaction) })) ==
            0);
    f.Call("SetFactionRank", kNpc,
           { f.Form(kFaction), VarValue(int32_t(127)) });
    f.Call("AddToFaction", kNpc, { f.Form(kFaction) });
    REQUIRE(int32_t(f.Call("GetFactionRank", kNpc, { f.Form(kFaction) })) ==
            127);
    f.Call("SetFactionRank", 0x14,
           { f.Form(kOtherFaction), VarValue(int32_t(-1)) });
    REQUIRE_FALSE(
      bool(f.Call("IsInFaction", 0x14, { f.Form(kOtherFaction) })));
    REQUIRE(f.copy->GetChangeForm().factions->empty());
    REQUIRE(f.owner->GetChangeForm().factions->empty());
    REQUIRE(f.before == 0);
  });
  REQUIRE(f.copy->GetChangeForm().factions->at(0).rank == 127);
  REQUIRE(f.owner->GetChangeForm().factions->at(0).rank == -1);
  REQUIRE(f.before == 1);
  REQUIRE(f.after == 1);
  REQUIRE(f.synchronized);
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(kNpc));
  REQUIRE_FALSE(f.vm.GetFunctionImplementation("Actor", "GetFactions", false));
}

TEST_CASE("Faction journal inventory visibility and VM state publish and "
          "restart together",
          "[Quest][CampaignFactions]")
{
  FactionWorld f;
  f.Empty(*f.copy);
  f.Barrier();
  f.server.worldState.Tick();
  f.storage->batches.clear();
  QuestJournal::Atomic(*f.guest, [&] {
    f.StageReward();
    f.Call("SetFactionRank", kNpc,
           { f.Form(kFaction), VarValue(int32_t(-1)) });
    QuestJournal::SetReferenceDisabled(*f.guest, *f.copy, true);
    REQUIRE(
      QuestJournal::ReadReference(*f.guest, *f.copy).factions->at(0).rank ==
      -1);
    REQUIRE_FALSE(f.copy->IsDisabled());
  });
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 15);
  REQUIRE(f.copy->IsDisabled());
  REQUIRE(QuestJournal::Get(*f.owner, kQuest).at("stage") == 10);
  f.server.worldState.Tick();
  REQUIRE(f.storage->batches.size() == 1);
  const auto& batch = f.storage->batches.front();
  REQUIRE(batch.at(f.copy->GetIdx())->factions->at(0).rank == -1);
  REQUIRE(batch.at(f.owner->GetIdx())->inv.GetItemCount(0xf) == 15);
  PartOne restarted;
  restarted.AttachEspm(&GetEspmLoader());
  restarted.AttachSaveStorage(f.storage);
  auto& restoredOwner =
    restarted.worldState.GetFormAt<MpActor>(f.owner->GetFormId());
  auto& restoredCopy =
    restarted.worldState.GetFormAt<MpActor>(f.copy->GetFormId());
  QuestJournal::Atomic(restoredOwner, [&] {
    REQUIRE(CampaignFactionApi::GetRank(restoredOwner, restoredCopy,
                                        kFaction) == -1);
  });
  REQUIRE(restoredCopy.IsDisabled());
  REQUIRE(QuestJournal::Get(restoredOwner, kQuest).at("stage") == 10);
  REQUIRE(QuestJournal::ScriptState(restoredOwner).at("factions") == true);
  REQUIRE(restoredOwner.GetInventory().GetItemCount(0xf) == 15);
}

TEST_CASE("Failed campaign factions roll back journal inventory visibility "
          "and VM state",
          "[Quest][CampaignFactions]")
{
  FactionWorld f;
  f.Empty(*f.copy);
  f.Barrier();
  f.server.worldState.Tick();
  f.storage->batches.clear();
  const auto journal = QuestJournal::Snapshot(*f.owner);
  REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
    f.StageReward();
    f.Call("AddToFaction", kNpc, { f.Form(kFaction) });
    QuestJournal::SetReferenceDisabled(*f.guest, *f.copy, true);
    throw std::runtime_error("Later fragment failed");
  }));
  REQUIRE(f.copy->GetChangeForm().factions->empty());
  REQUIRE_FALSE(f.copy->IsDisabled());
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(QuestJournal::Snapshot(*f.owner) == journal);
  REQUIRE(QuestJournal::ScriptState(*f.owner).empty());
  REQUIRE(f.before == 0);
  f.server.worldState.Tick();
  REQUIRE(f.storage->batches.empty());
}

TEST_CASE(
  "Campaign faction conflicts include ordinary writes and campaign identity",
  "[Quest][CampaignFactions]")
{
  FactionWorld f;
  f.Empty(*f.copy);
  f.Barrier();
  const auto journal = QuestJournal::Snapshot(*f.owner);
  REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
    f.StageReward();
    QuestJournal::ReadFactions(*f.guest, *f.copy);
    SECTION("ordinary faction write")
    {
      f.copy->AddToFaction(
        { FormDesc::FromFormId(kOtherFaction, f.server.worldState.espmFiles),
          3 });
    }
    SECTION("phase epoch changed")
    {
      f.copy->SetWorldInstance("");
    }
    SECTION("campaign binding identity changed")
    {
      f.owner->SetPropertyValueDump("private.boyhouse.campaignIdentity",
                                    "\"11111111111111111111111111111111\"",
                                    false, false);
    }
  }));
  REQUIRE(QuestJournal::Snapshot(*f.owner) == journal);
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
}

TEST_CASE("Campaign faction barriers abort before publication and never "
          "replay committed state",
          "[Quest][CampaignFactions]")
{
  FactionWorld f;
  f.Empty(*f.copy);
  bool abort = true;
  f.server.worldState.SetCampaignVisibilityCallbacks(
    [&](MpActor&) {
      ++f.before;
      if (abort)
        throw std::runtime_error("Barrier refused");
    },
    [&](MpActor&, bool success) {
      ++f.after;
      f.synchronized = success;
      if (!abort)
        throw std::runtime_error("Receipt failed");
    });
  const auto execute = [&] {
    f.StageReward();
    f.Call("AddToFaction", kNpc, { f.Form(kFaction) });
  };
  REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, execute));
  REQUIRE(f.after == 1);
  REQUIRE_FALSE(f.synchronized);
  REQUIRE(f.copy->GetChangeForm().factions->empty());
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
  abort = false;
  REQUIRE_NOTHROW(QuestJournal::Atomic(*f.guest, execute));
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 15);
  REQUIRE(QuestJournal::Get(*f.owner, kQuest).at("stage") == 10);
  REQUIRE(f.copy->GetChangeForm().factions->at(0).rank == 0);
  REQUIRE(f.after == 2);
}

TEST_CASE("Faction native validation rejects shared actors other players and "
          "malformed values",
          "[Quest][CampaignFactions]")
{
  FactionWorld f;
  f.Empty(*f.copy);
  for (const auto rank : { -129, -128, -2, 128 })
    REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
      f.Call("SetFactionRank", kNpc,
             { f.Form(kFaction), VarValue(int32_t(rank)) });
    }));
  REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
    f.Call("SetFactionRank", kNpc, { f.Form(kFaction), VarValue(1.5) });
  }));
  REQUIRE_THROWS(QuestJournal::Atomic(
    *f.guest, [&] { f.Call("AddToFaction", kNpc, { f.Form(0xf) }); }));
  REQUIRE_THROWS(QuestJournal::Atomic(
    *f.guest, [&] { f.Call("AddToFaction", 0x1a676, { f.Form(kFaction) }); }));
  REQUIRE_THROWS(QuestJournal::Atomic(
    *f.guest, [&] { QuestJournal::ReadFactions(*f.guest, *f.guest); }));
  REQUIRE_THROWS(QuestJournal::ReadFactions(*f.guest, *f.copy));
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(kNpc));
  REQUIRE(f.copy->GetChangeForm().factions->empty());
}

TEST_CASE(
  "Faction writes need atomic storage and a barrier while no-ops do not",
  "[Quest][CampaignFactions]")
{
  FactionWorld f;
  f.Empty(*f.copy);
  REQUIRE_NOTHROW(QuestJournal::Atomic(*f.guest, [&] {
    REQUIRE(CampaignFactionApi::GetRank(*f.guest, *f.copy, kFaction) == -2);
    QuestJournal::EditFactions(*f.guest, *f.copy,
                               [](auto& factions) { factions.clear(); });
  }));
  REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
    f.Call("RemoveFromFaction", kNpc, { f.Form(kFaction) });
  }));
  REQUIRE_THROWS(QuestJournal::Atomic(
    *f.guest, [&] { f.Call("AddToFaction", kNpc, { f.Form(kFaction) }); }));
  f.Barrier();
  f.storage->atomic = false;
  REQUIRE_THROWS(QuestJournal::Atomic(
    *f.guest, [&] { f.Call("AddToFaction", kNpc, { f.Form(kFaction) }); }));
  REQUIRE(f.before == 0);
  REQUIRE(f.copy->GetChangeForm().factions->empty());
}

TEST_CASE("Shared-world faction changes remain local to the campaign owner",
          "[Quest][CampaignFactions]")
{
  FactionWorld f;
  f.Empty(*f.owner);
  f.Empty(*f.guest);
  f.owner->SetWorldInstance("");
  f.guest->SetWorldInstance("");
  f.Barrier();
  QuestJournal::Atomic(*f.owner, [&] {
    QuestJournal::EditFactions(*f.owner, *f.owner, [&](auto& factions) {
      factions.push_back(
        { FormDesc::FromFormId(kFaction, f.server.worldState.espmFiles), 4 });
    });
    REQUIRE(CampaignFactionApi::GetRank(*f.owner, *f.owner, kFaction) == 4);
    REQUIRE(f.owner->GetChangeForm().factions->empty());
  });
  REQUIRE(f.owner->GetChangeForm().factions->at(0).rank == 4);
  REQUIRE(f.guest->GetChangeForm().factions->empty());
  REQUIRE(f.before == 1);
  REQUIRE(f.synchronized);
  // NPC auto-loading is disabled in this fixture. Register the shared original
  // explicitly so this checks mutation isolation, not a missing-form error.
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(kNpc));
  f.server.worldState.AddForm(
    std::make_unique<MpActor>(
      LocationalData{ { 1, 2, 3 }, {}, FormDesc::Tamriel() },
      FormCallbacks::DoNothing(), 0x13bbd),
    kNpc);
  auto& shared = f.server.worldState.GetFormAt<MpActor>(kNpc);
  REQUIRE(shared.GetWorldInstance().empty());
  const auto sharedBefore = MpChangeForm::ToJson(shared.GetChangeForm());
  REQUIRE_THROWS(QuestJournal::Atomic(*f.owner, [&] {
    QuestJournal::EditInventory(*f.owner, *f.owner,
                                [](auto& inv) { inv.AddItem(0xf, 5); });
    QuestJournal::EditFactions(*f.owner, shared,
                               [](auto& factions) { factions.clear(); });
  }));
  REQUIRE(MpChangeForm::ToJson(shared.GetChangeForm()) == sharedBefore);
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(f.before == 1);
}

TEST_CASE("Malformed faction edits cannot partially publish a campaign",
          "[Quest][CampaignFactions]")
{
  FactionWorld f;
  f.Empty(*f.copy);
  f.Barrier();
  const auto journal = QuestJournal::Snapshot(*f.owner);
  REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
    f.StageReward();
    QuestJournal::EditFactions(*f.guest, *f.copy, [&](auto& factions) {
      const auto descriptor =
        FormDesc::FromFormId(kFaction, f.server.worldState.espmFiles);
      SECTION("duplicate identities")
      {
        factions = { { descriptor, 1 }, { descriptor, 2 } };
      }
      SECTION("invalid record kind")
      {
        factions = {
          { FormDesc::FromFormId(0xf, f.server.worldState.espmFiles), 0 }
        };
      }
      SECTION("missing plugin")
      {
        factions = { { FormDesc(kFaction, "Absent.esm"), 0 } };
      }
      SECTION("noncanonical identity")
      {
        factions = { { FormDesc(0x01000000u + kFaction, "Skyrim.esm"), 0 } };
      }
      SECTION("resource bound")
      {
        factions.assign(Faction::kMaxEntries + 1, { descriptor, 0 });
      }
    });
  }));
  REQUIRE(f.copy->GetChangeForm().factions->empty());
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(QuestJournal::Snapshot(*f.owner) == journal);
  REQUIRE(f.before == 0);
}

TEST_CASE("Faction reads preserve signed overrides and native base membership",
          "[Quest][CampaignFactions]")
{
  FactionWorld f;
  const auto base = CampaignFactionApi::ReadBaseFactions(*f.copy);
  const auto active =
    std::find_if(base.begin(), base.end(),
                 [](const auto& entry) { return entry.rank >= 0; });
  REQUIRE(active != base.end());
  const auto id = active->formDesc.ToFormId(f.server.worldState.espmFiles);
  for (const int rank : { -128, -2, -1, 0, 127 }) {
    auto saved = f.copy->GetChangeForm();
    saved.factions =
      std::vector<Faction>{ { active->formDesc, static_cast<int8_t>(rank) } };
    f.copy->ApplyChangeForm(saved);
    QuestJournal::Atomic(*f.guest, [&] {
      REQUIRE(int32_t(f.Call("GetFactionRank", kNpc, { f.Form(id) })) == rank);
      REQUIRE(bool(f.Call("IsInFaction", kNpc, { f.Form(id) })) ==
              (rank != -1));
      if (rank != -1) {
        f.Call("AddToFaction", kNpc, { f.Form(id) });
        REQUIRE(CampaignFactionApi::GetRank(*f.guest, *f.copy, id) == rank);
      }
    });
  }
  f.Empty(*f.copy);
  QuestJournal::Atomic(*f.guest, [&] {
    REQUIRE(CampaignFactionApi::GetRank(*f.guest, *f.copy, id) == -2);
    REQUIRE_FALSE(CampaignFactionApi::IsMember(*f.guest, *f.copy, id));
  });
  REQUIRE(f.before == 0);
}

TEST_CASE("Unique actor removal refuses an unrepresentable base erase before "
          "rewards commit",
          "[Quest][CampaignFactions]")
{
  FactionWorld f;
  f.Barrier();
  const uint32_t target = GENERATE(kNpc, 0x14u);
  auto& actor = target == 0x14 ? *f.owner : *f.copy;
  REQUIRE(espm::Convert<espm::NPC_>(f.server.worldState.GetEspm()
                                      .GetBrowser()
                                      .LookupById(actor.GetBaseId())
                                      .rec)
            ->GetData(f.server.worldState.GetEspmCache())
            .isUnique);
  auto state = actor.GetChangeForm();
  state.factions = std::vector<Faction>{
    { FormDesc::FromFormId(kFaction, f.server.worldState.espmFiles), 3 }
  };
  actor.ApplyChangeForm(state);
  const auto journal = QuestJournal::Snapshot(*f.owner);
  for (const bool setter : { false, true }) {
    REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
      f.StageReward();
      if (setter)
        f.Call("SetFactionRank", target,
               { f.Form(kFaction), VarValue(int32_t(-1)) });
      else
        f.Call("RemoveFromFaction", target, { f.Form(kFaction) });
    }));
    REQUIRE(QuestJournal::Snapshot(*f.owner) == journal);
    REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
    REQUIRE(actor.GetChangeForm().factions == state.factions);
    REQUIRE(f.before == 0);
  }
}

TEST_CASE("Removing an absent faction preserves the native minus-one entry",
          "[Quest][CampaignFactions]")
{
  FactionWorld f;
  f.Empty(*f.copy);
  f.Barrier();
  QuestJournal::Atomic(*f.guest, [&] {
    f.Call("RemoveFromFaction", kNpc, { f.Form(kFaction) });
    REQUIRE(CampaignFactionApi::GetRank(*f.guest, *f.copy, kFaction) == -1);
    REQUIRE_FALSE(CampaignFactionApi::IsMember(*f.guest, *f.copy, kFaction));
  });
  REQUIRE(f.copy->GetChangeForm().factions->size() == 1);
  REQUIRE(f.copy->GetChangeForm().factions->front().rank == -1);
  REQUIRE(f.before == 1);
  REQUIRE(f.synchronized);
}

TEST_CASE("Expelled player baselines preserve rank and membership through "
          "unrelated writes",
          "[Quest][CampaignFactions]")
{
  FactionMetadataFile file;
  espm::Loader loader(std::vector<std::filesystem::path>{ file.File() });
  WorldState world;
  world.espmFiles = { "FactionFixture.esm" };
  world.AddForm(std::make_unique<MpActor>(
                  LocationalData{ { 1, 2, 3 }, {}, FormDesc::Tamriel() },
                  FormCallbacks::DoNothing()),
                0xff008000);
  world.AttachEspm(&loader, [] { return FormCallbacks::DoNothing(); });
  auto& actor = world.GetFormAt<MpActor>(0xff008000);
  REQUIRE_FALSE(actor.GetChangeForm().factions);
  const auto player = CampaignFactionApi::InitialFactions(actor, true);
  const auto npc = CampaignFactionApi::InitialFactions(actor, false);
  REQUIRE(player.size() == 3);
  REQUIRE(player.at(0).rank == -2);
  REQUIRE(player.at(1).rank == 1);
  REQUIRE(player.at(2).rank == -2);
  REQUIRE(npc.at(0).rank == 3);
  REQUIRE(npc.at(2).rank == -128);
  REQUIRE_FALSE(actor.GetChangeForm().factions);
  QuestJournal::Atomic(actor, [&] {
    REQUIRE(CampaignFactionApi::GetRank(actor, actor, 0x100) == -2);
    REQUIRE(CampaignFactionApi::IsMember(actor, actor, 0x100));
    REQUIRE(CampaignFactionApi::GetRank(actor, actor, 0x102) == -2);
    REQUIRE_FALSE(CampaignFactionApi::IsMember(actor, actor, 0x102));
  });
  REQUIRE_FALSE(actor.GetChangeForm().factions);
  world.SetCampaignVisibilityCallbacks([](MpActor&) {}, [](MpActor&, bool) {});
  QuestJournal::Atomic(actor, [&] {
    QuestJournal::EditFactions(actor, actor, [&](auto& factions) {
      factions.push_back({ FormDesc(0x103, "FactionFixture.esm"), 5 });
    });
  });
  REQUIRE(actor.GetChangeForm().factions);
  QuestJournal::Atomic(actor, [&] {
    REQUIRE(CampaignFactionApi::GetRank(actor, actor, 0x100) == -2);
    REQUIRE(CampaignFactionApi::IsMember(actor, actor, 0x100));
    REQUIRE_FALSE(CampaignFactionApi::IsMember(actor, actor, 0x102));
    REQUIRE(CampaignFactionApi::GetRank(actor, actor, 0x103) == 5);
  });
  auto saved = actor.GetChangeForm();
  saved.factions = std::vector<Faction>{};
  REQUIRE(CampaignFactionApi::InitialFactions(actor, saved, true).empty());
  saved.factions =
    std::vector<Faction>{ { FormDesc(0x100, "FactionFixture.esm"), 7 } };
  REQUIRE(
    CampaignFactionApi::InitialFactions(actor, saved, true).front().rank == 7);
}
