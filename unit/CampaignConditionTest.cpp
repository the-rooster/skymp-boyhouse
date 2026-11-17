#include "CampaignCondition.h"
#include "CampaignReferenceBinding.h"
#include "QuestJournal.h"
#include "QuestScriptRuntime.h"
#include "TestUtils.hpp"
#include "libespm/NPC_.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>

extern espm::Loader& GetEspmLoader();

namespace {
constexpr uint32_t kQuest = 0x4e50d;
constexpr uint32_t kNpc = 0x1a677;
constexpr uint32_t kMq103 = 0xd0800;
constexpr uint32_t kClaw = 0xdf202;
constexpr uint32_t kMs13 = 0x39645;
constexpr uint32_t kLucan = 0x13487;
constexpr uint32_t kMq302 = 0x45923;
constexpr uint32_t kSonsFaction = 0x2bf9b;
using RunOn = espm::CTDA::RunOnTypeFlags;

espm::CTDA Clause(uint16_t function, RunOn runOn, uint32_t reference,
                  float value = 0, uint8_t flags = 0, uint32_t first = 0,
                  uint32_t second = 0, int32_t alias = -1)
{
  // Use the on-disk layout to catch confusion between run-on Reference and
  // QuestAlias's distinct fields, independent of GetDefaultData's casts.
  uint8_t bytes[32]{};
  bytes[0] = flags;
  std::memcpy(bytes + 4, &value, 4);
  std::memcpy(bytes + 8, &function, 2);
  std::memcpy(bytes + 12, &first, 4);
  std::memcpy(bytes + 16, &second, 4);
  std::memcpy(bytes + 20, &runOn, 4);
  std::memcpy(bytes + 24, &reference, 4);
  std::memcpy(bytes + 28, &alias, 4);
  espm::CTDA result;
  std::memcpy(&result, bytes, sizeof(result));
  return result;
}

CampaignCondition Compile(const espm::CTDA& data)
{
  return CampaignCondition::Compile(data, [](uint32_t id) { return id; });
}

struct ConditionFixture
{
  PartOne server;
  MpActor *owner, *guest, *stranger, *npc;

  ConditionFixture()
  {
    server.AttachEspm(&GetEspmLoader());
    owner = &AddPlayer(0xff005001);
    guest = &AddPlayer(0xff005002);
    stranger = &AddPlayer(0xff005003);
    owner->SetWorldInstance("campaign:" + std::to_string(owner->GetFormId()) +
                            ":conditions");
    guest->SetWorldInstance(owner->GetWorldInstance());
    stranger->SetWorldInstance(
      "campaign:" + std::to_string(stranger->GetFormId()) + ":conditions");
    npc = &AddNpc(*owner, kNpc, 0x13bbd, 0xff005010);
    // This fixture exercises condition/VM semantics; transport acknowledgement
    // and observer quarantine are covered by CampaignVisibilityBarrierTest.
    server.worldState.SetCampaignVisibilityCallbacks([](MpActor&) {},
                                                     [](MpActor&, bool) {});
  }

  MpActor& AddPlayer(uint32_t id)
  {
    server.CreateActor(id, { 1, 2, 3 }, 0, 0x3c);
    auto& actor = server.worldState.GetFormAt<MpActor>(id);
    actor.SetInventory(Inventory().AddItem(0xf, 10));
    // Only fixture execution is authorized; this is not quest certification.
    actor.SetPropertyValueDump(QuestJournal::kEnabled,
                               "[\"4e50d:Skyrim.esm\",\"d0800:Skyrim.esm\","
                               "\"39645:Skyrim.esm\",\"45923:Skyrim.esm\"]",
                               false, false);
    return actor;
  }

  MpActor& AddNpc(MpActor& campaign, uint32_t original, uint32_t base,
                  uint32_t id)
  {
    server.worldState.AddForm(
      std::make_unique<MpActor>(
        LocationalData{ { 1, 2, 3 }, {}, FormDesc::Tamriel() },
        FormCallbacks::DoNothing(), base),
      id);
    auto& actor = server.worldState.GetFormAt<MpActor>(id);
    actor.SetWorldInstance(campaign.GetWorldInstance());
    actor.SetInventory(Inventory().AddItem(0xf, 3));
    CampaignReferenceBinding::Bind(campaign, original, actor);
    return actor;
  }

  CampaignCondition::Context Context()
  {
    return { *guest, kQuest, {}, {}, {} };
  }

  bool Evaluate(const std::vector<CampaignCondition>& conditions)
  {
    bool result = false;
    QuestJournal::Atomic(*guest, [&] {
      result = CampaignCondition::Evaluate(conditions, Context());
    });
    return result;
  }

  void Stage()
  {
    QuestJournal::Apply(
      *guest, { { "quest", kQuest }, { "op", "setStage" }, { "index", 10 } });
  }

  std::vector<Faction> Factions(int rank)
  {
    return { { FormDesc::FromFormId(kSonsFaction, server.worldState.espmFiles),
               static_cast<int8_t>(rank) } };
  }

  void SetFactions(MpActor& target, int rank)
  {
    auto saved = target.GetChangeForm();
    saved.factions = Factions(rank);
    target.ApplyChangeForm(saved);
  }
};
}

TEST_CASE("Campaign faction conditions preserve signed ranks and owner scope",
          "[Quest][CampaignConditions][CampaignFactions]")
{
  ConditionFixture f;
  const auto rank = GENERATE(-128, -2, -1, 0, 1, 127);
  f.SetFactions(*f.owner, rank);
  f.SetFactions(*f.guest, rank >= 0 ? -1 : 1);
  f.SetFactions(*f.npc, 3);
  auto& other = f.AddNpc(*f.stranger, kNpc, 0x13bbd, 0xff005011);
  f.SetFactions(other, 9);
  REQUIRE(f.Evaluate(
    { Compile(Clause(73, RunOn::Reference, 0x14, static_cast<float>(rank), 0,
                     kSonsFaction)) }));
  REQUIRE(f.Evaluate({ Compile(Clause(
    71, RunOn::Reference, 0x14, rank >= 0 ? 1.f : 0.f, 0, kSonsFaction)) }));
  REQUIRE(f.Evaluate(
    { Compile(Clause(73, RunOn::Reference, kNpc, 3, 0, kSonsFaction)) }));
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(kNpc));
}

TEST_CASE(
  "Faction alias selectors map faction forms but never byte28 alias IDs",
  "[Quest][CampaignConditions][CampaignFactions]")
{
  ConditionFixture f;
  f.SetFactions(*f.npc, 3);
  const int32_t aliasId = GENERATE(0, 17);
  const uint16_t function = GENERATE(71, 73);
  std::vector<uint32_t> mapped;
  const auto condition = CampaignCondition::Compile(
    Clause(function, RunOn::QuestAlias, 0xfeabcdef, function == 71 ? 1.f : 3.f,
           0, 0x0102bf9b, 0, aliasId),
    [&](uint32_t raw) {
      mapped.push_back(raw);
      REQUIRE(raw == 0x0102bf9b);
      return raw & 0xffffff;
    });
  REQUIRE(mapped == std::vector<uint32_t>{ 0x0102bf9b });
  auto context = f.Context();
  context.owningQuest = kMq302;
  context.resolveReferenceAlias = [&](uint32_t quest, uint32_t alias) {
    REQUIRE(quest == kMq302);
    REQUIRE(alias == static_cast<uint32_t>(aliasId));
    return kNpc;
  };
  QuestJournal::Atomic(*f.guest, [&] {
    REQUIRE(CampaignCondition::Evaluate({ condition }, context));
  });
  context.resolveReferenceAlias = [](auto, auto) { return 0; };
  REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
    f.Stage();
    CampaignCondition::Evaluate({ condition }, context);
  }));
  REQUIRE(QuestJournal::Get(*f.owner, kQuest).at("stage") == 0);
}

TEST_CASE(
  "Faction predicates observe staged changes and rollback with rewards",
  "[Quest][CampaignConditions][CampaignFactions]")
{
  ConditionFixture f;
  f.SetFactions(*f.npc, -1);
  const auto before = QuestJournal::Snapshot(*f.owner);
  REQUIRE_THROWS_WITH(
    QuestJournal::Atomic(
      *f.guest,
      [&] {
        REQUIRE(CampaignCondition::Evaluate(
          { Compile(Clause(71, RunOn::Reference, kNpc, 0, 0, kSonsFaction)) },
          f.Context()));
        QuestJournal::EditFactions(
          *f.guest, *f.npc, [&](auto& factions) { factions = f.Factions(4); });
        REQUIRE(CampaignCondition::Evaluate(
          { Compile(Clause(71, RunOn::Reference, kNpc, 1, 0, kSonsFaction)),
            Compile(Clause(73, RunOn::Reference, kNpc, 4, 0, kSonsFaction)) },
          f.Context()));
        REQUIRE(f.npc->GetChangeForm().factions->at(0).rank == -1);
        f.Stage();
        QuestJournal::EditInventory(
          *f.guest, *f.owner, [](Inventory& inv) { inv.AddItem(0xf, 50); });
        throw std::runtime_error("Fixture faction abort");
      }),
    Catch::Matchers::ContainsSubstring("Fixture faction abort"));
  REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
  REQUIRE(f.npc->GetChangeForm().factions->at(0).rank == -1);
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
}

TEST_CASE(
  "Faction ranks distinguish an absent entry from stored negative ranks",
  "[Quest][CampaignConditions][CampaignFactions]")
{
  ConditionFixture f;
  auto empty = f.owner->GetChangeForm();
  empty.factions = std::vector<Faction>{};
  f.owner->ApplyChangeForm(empty);
  REQUIRE(f.Evaluate(
    { Compile(Clause(73, RunOn::Reference, 0x14, -2, 0, kSonsFaction)),
      Compile(Clause(71, RunOn::Reference, 0x14, 0, 0, kSonsFaction)) }));
  f.SetFactions(*f.owner, -1);
  REQUIRE(f.Evaluate(
    { Compile(Clause(73, RunOn::Reference, 0x14, -1, 0, kSonsFaction)),
      Compile(Clause(71, RunOn::Reference, 0x14, 0, 0, kSonsFaction)) }));
}

TEST_CASE(
  "Authored active faction membership survives negative overrides except "
  "the removal rank",
  "[Quest][CampaignConditions][CampaignFactions]")
{
  ConditionFixture f;
  const auto record =
    f.server.worldState.GetEspm().GetBrowser().LookupById(f.npc->GetBaseId());
  const auto base = espm::Convert<espm::NPC_>(record.rec)
                      ->GetData(f.server.worldState.GetEspmCache());
  REQUIRE((base.templateDataFlags & espm::NPC_::UseFactions) == 0);
  const auto authored =
    std::find_if(base.factions.begin(), base.factions.end(),
                 [](const auto& faction) { return faction.rank >= 0; });
  REQUIRE(authored != base.factions.end());
  const auto faction = record.ToGlobalId(authored->formId);
  const auto descriptor =
    FormDesc::FromFormId(faction, f.server.worldState.espmFiles);
  const int rank = GENERATE(-128, -2, -1, 0, 127);
  // This actor's real base has an active membership. The actual Skyrim
  // IsInFaction handler treats only an override of -1 as its removal marker;
  // other negative overrides still return true while GetFactionRank is
  // negative.
  auto saved = f.npc->GetChangeForm();
  saved.factions =
    std::vector<Faction>{ { descriptor, static_cast<int8_t>(rank) } };
  f.npc->ApplyChangeForm(saved);
  REQUIRE(f.Evaluate({ Compile(Clause(73, RunOn::Reference, kNpc,
                                      static_cast<float>(rank), 0, faction)),
                       Compile(Clause(71, RunOn::Reference, kNpc,
                                      rank == -1 ? 0.f : 1.f, 0, faction)) }));
  QuestJournal::Atomic(*f.guest, [&] {
    QuestJournal::EditFactions(*f.guest, *f.npc, [&](auto& factions) {
      factions = { { descriptor, -1 } };
    });
    REQUIRE(CampaignCondition::Evaluate(
      { Compile(Clause(71, RunOn::Reference, kNpc, 0, 0, faction)) },
      f.Context()));
  });
}

TEST_CASE("Faction conditions preserve mapped global operands and OR groups",
          "[Quest][CampaignConditions][CampaignFactions]")
{
  ConditionFixture f;
  f.SetFactions(*f.owner, 3);
  auto context = f.Context();
  context.readGlobal = [](uint32_t id) {
    REQUIRE(id == 0x42);
    return 3.0;
  };
  auto clause = Clause(73, RunOn::Reference, 0x01000014, 0, 4, 0x0102bf9b);
  const uint32_t rawGlobal = 0x01000042;
  std::memcpy(&clause.comparisonValue, &rawGlobal, 4);
  std::vector<uint32_t> mapped;
  const auto global = CampaignCondition::Compile(clause, [&](uint32_t raw) {
    mapped.push_back(raw);
    return raw & 0xffffff;
  });
  REQUIRE(mapped ==
          std::vector<uint32_t>({ 0x01000014, 0x0102bf9b, rawGlobal }));
  const auto no =
    Compile(Clause(73, RunOn::Reference, 0x14, 9, 1, kSonsFaction));
  const auto yes =
    Compile(Clause(71, RunOn::Reference, 0x14, 1, 0, kSonsFaction));
  QuestJournal::Atomic(*f.guest, [&] {
    REQUIRE(CampaignCondition::Evaluate({ global, no, yes, yes }, context));
    REQUIRE_FALSE(CampaignCondition::Evaluate(
      { global, no, yes,
        Compile(Clause(73, RunOn::Reference, 0x14, 9, 0, kSonsFaction)) },
      context));
  });
}

TEST_CASE("Stale faction condition reads cannot publish quest progress",
          "[Quest][CampaignConditions][CampaignFactions]")
{
  ConditionFixture f;
  f.SetFactions(*f.npc, 3);
  const auto before = QuestJournal::Snapshot(*f.owner);
  REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
    REQUIRE(CampaignCondition::Evaluate(
      { Compile(Clause(73, RunOn::Reference, kNpc, 3, 0, kSonsFaction)) },
      f.Context()));
    f.Stage();
    QuestJournal::EditInventory(*f.guest, *f.owner,
                                [](Inventory& inv) { inv.AddItem(0xf, 50); });
    f.SetFactions(*f.npc, 4);
  }));
  REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(f.npc->GetChangeForm().factions->at(0).rank == 4);
}

TEST_CASE("Faction conditions validate skipped static targets and parameters",
          "[Quest][CampaignConditions][CampaignFactions]")
{
  ConditionFixture f;
  const uint16_t function = GENERATE(71, 73);
  const auto good = Compile(Clause(47, RunOn::Reference, 0x14, 10, 1, 0xf));
  for (const auto faction : { 0u, 0xfu, kQuest, 0x13bbdu, 0xfeffffffu }) {
    REQUIRE_THROWS(f.Evaluate(
      { good,
        Compile(Clause(function, RunOn::Reference, kNpc, 0, 0, faction)) }));
  }
  for (const auto reference :
       { 0xfu, kQuest, 0x13bbdu, 0x8a986u, 0xfeffffffu }) {
    REQUIRE_THROWS(
      f.Evaluate({ good,
                   Compile(Clause(function, RunOn::Reference, reference, 0, 0,
                                  kSonsFaction)) }));
    REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(reference));
  }
  REQUIRE(f.Evaluate({ good,
                       Compile(Clause(function, RunOn::Reference, kLucan, 0, 0,
                                      kSonsFaction)) }));
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(kLucan));
  REQUIRE_THROWS(CampaignReferenceBinding::Resolve(*f.guest, kLucan));
  auto context = f.Context();
  context.resolveReferenceAlias = [](auto, auto) -> uint32_t {
    throw std::runtime_error("Skipped faction alias must not resolve");
  };
  QuestJournal::Atomic(*f.guest, [&] {
    REQUIRE(CampaignCondition::Evaluate(
      { good,
        Compile(Clause(function, RunOn::QuestAlias, 0xfeabcdef, 0, 0,
                       kSonsFaction, 0, 0)) },
      context));
  });
  for (const auto runOn :
       { RunOn::Subject, RunOn::Target, RunOn::CombatTarget,
         RunOn::LinkedReference, RunOn::PackageData, RunOn::EventData })
    REQUIRE_THROWS(Compile(Clause(function, runOn, 0x14, 0, 0, kSonsFaction)));
  for (const auto flag : { 2, 8, 16 })
    REQUIRE_THROWS(Compile(
      Clause(function, RunOn::Reference, 0x14, 0, flag, kSonsFaction)));
  REQUIRE_THROWS(Compile(
    Clause(function, RunOn::QuestAlias, 0, 0, 0, kSonsFaction, 0, -1)));
}

TEST_CASE("Campaign conditions distinguish owner inventory and private copies",
          "[Quest][CampaignConditions]")
{
  ConditionFixture f;
  f.guest->SetInventory(Inventory().AddItem(0xf, 99));
  auto& other = f.AddNpc(*f.stranger, kNpc, 0x13bbd, 0xff005011);
  other.SetIsDead(true);
  REQUIRE(
    f.Evaluate({ Compile(Clause(47, RunOn::Reference, 0x14, 10, 0, 0xf)) }));
  REQUIRE(
    f.Evaluate({ Compile(Clause(47, RunOn::Reference, kNpc, 3, 0, 0xf)) }));
  REQUIRE(f.Evaluate({ Compile(Clause(46, RunOn::Reference, kNpc)) }));
  f.npc->SetIsDead(true);
  REQUIRE(f.Evaluate({ Compile(Clause(46, RunOn::Reference, kNpc, 1)) }));
  REQUIRE(f.Evaluate({ Compile(Clause(46, RunOn::Reference, 0x14)) }));
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(kNpc));
}

TEST_CASE(
  "Campaign condition counts retain integer precision across comparisons",
  "[Quest][CampaignConditions]")
{
  ConditionFixture f;
  f.owner->SetInventory(Inventory().AddItem(0xf, 16777217));
  REQUIRE(f.Evaluate(
    { Compile(Clause(47, RunOn::Reference, 0x14, 16777216.f, 2 << 5, 0xf)) }));
  REQUIRE_FALSE(f.Evaluate(
    { Compile(Clause(47, RunOn::Reference, 0x14, 16777216.f, 0, 0xf)) }));
  f.owner->SetInventory(Inventory().AddItem(0xf, uint32_t(INT_MAX) + 1));
  REQUIRE_THROWS_WITH(
    f.Evaluate({ Compile(Clause(47, RunOn::Reference, 0x14, 0, 0, 0xf)) }),
    Catch::Matchers::ContainsSubstring("count overflow"));
}

TEST_CASE("Quest alias conditions use byte28 and owning quest without mapping "
          "alias zero",
          "[Quest][CampaignConditions]")
{
  ConditionFixture f;
  const int32_t aliasId = GENERATE(0, 17);
  const auto condition = CampaignCondition::Compile(
    Clause(46, RunOn::QuestAlias, 0xfeabcdef, 0, 0, 0, 0, aliasId),
    [](uint32_t) -> uint32_t {
      throw std::runtime_error("Unexpected FormID mapping");
    });
  auto context = f.Context();
  context.owningQuest = kMs13;
  int calls = 0;
  context.resolveReferenceAlias = [&](uint32_t quest, uint32_t alias) {
    REQUIRE(quest == kMs13);
    REQUIRE(alias == static_cast<uint32_t>(aliasId));
    ++calls;
    return kNpc;
  };
  QuestJournal::Atomic(*f.guest, [&] {
    REQUIRE(CampaignCondition::Evaluate({ condition }, context));
  });
  REQUIRE(calls == 1);
  context.resolveReferenceAlias = [](auto, auto) { return 0; };
  REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
    f.Stage();
    CampaignCondition::Evaluate({ condition }, context);
  }));
  REQUIRE(QuestJournal::Get(*f.owner, kQuest).at("stage") == 0);
}

TEST_CASE(
  "Explicit reference and item parameters use the originating master mapping",
  "[Quest][CampaignConditions]")
{
  ConditionFixture f;
  std::vector<uint32_t> mapped;
  const auto condition = CampaignCondition::Compile(
    Clause(47, RunOn::Reference, 0x01000014, 10, 0, 0x0100000f),
    [&](uint32_t raw) {
      mapped.push_back(raw);
      REQUIRE((raw >> 24) == 1);
      return raw & 0xffffff;
    });
  REQUIRE(mapped == std::vector<uint32_t>({ 0x01000014, 0x0100000f }));
  REQUIRE(f.Evaluate({ condition }));
}

TEST_CASE("Unsupported campaign condition branches cannot hide behind OR",
          "[Quest][CampaignConditions]")
{
  ConditionFixture f;
  const auto before = QuestJournal::Snapshot(*f.owner);
  const auto good = Clause(47, RunOn::Reference, 0x14, 10, 1, 0xf);
  for (const auto bad : { Clause(1, RunOn::Reference, 0x14),
                          Clause(47, RunOn::Subject, 0, 0, 0, 0xf),
                          Clause(46, RunOn::Target, 0),
                          Clause(35, RunOn::QuestAlias, 0, 0, 0, 0, 0, 0),
                          Clause(46, RunOn::QuestAlias, 0, 0, 0, 0, 0, -1),
                          Clause(46, RunOn::Reference, 0x14, 0, 2),
                          Clause(46, RunOn::Reference, 0x14, 0, 8),
                          Clause(46, RunOn::Reference, 0x14, 0, 16),
                          Clause(46, RunOn::Reference, 0x14, 0, 6 << 5) }) {
    REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
      f.Stage();
      const std::vector<CampaignCondition> conditions{ Compile(good),
                                                       Compile(bad) };
      CampaignCondition::Evaluate(conditions, f.Context());
    }));
    REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
  }
  // Supported function, unsupported parameter type must also fail before
  // short-circuiting a true OR arm. A QUST is not a concrete inventory item.
  REQUIRE_THROWS(
    f.Evaluate({ Compile(good),
                 Compile(Clause(47, RunOn::Reference, 0x14, 0, 0, kQuest)) }));
  auto missingString = Compile(Clause(629, RunOn::Subject, 0, 0, 0, kQuest));
  REQUIRE_THROWS(f.Evaluate({ Compile(good), missingString }));
}

TEST_CASE("Invalid explicit condition targets cannot hide behind OR",
          "[Quest][CampaignConditions]")
{
  ConditionFixture f;
  const auto before = QuestJournal::Snapshot(*f.owner);
  const auto good = Compile(Clause(47, RunOn::Reference, 0x14, 10, 1, 0xf));
  const auto function = GENERATE(35, 46, 47);
  const auto& browser = f.server.worldState.GetEspm().GetBrowser();
  uint32_t deleted = 0;
  for (const auto& record : browser.GetDistinctRecordsByType("REFR")) {
    if (record.rec->IsDeleted()) {
      deleted = record.ToGlobalId(record.rec->GetId());
      break;
    }
  }
  REQUIRE(deleted != 0);
  constexpr uint32_t missing = 0xfeffffff;
  REQUIRE_FALSE(browser.LookupById(missing).rec);
  // Quest, item base, NPC base, missing record, and deleted placed reference.
  for (const uint32_t target : { kQuest, 0xfu, 0x13bbdu, missing, deleted }) {
    CAPTURE(function, target);
    REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
      f.Stage();
      CampaignCondition::Evaluate(
        { good,
          Compile(Clause(function, RunOn::Reference, target, 0, 0, 0xf)) },
        f.Context());
    }));
    REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
    REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(target));
  }
  // An ordinary placed REFR is valid for other predicates, never GetDead.
  REQUIRE(browser.LookupById(0x8a986).rec);
  REQUIRE(browser.LookupById(0x8a986).rec->GetType() == "REFR");
  REQUIRE_THROWS(
    f.Evaluate({ good, Compile(Clause(46, RunOn::Reference, 0x8a986)) }));
}

TEST_CASE(
  "Skipped physical conditions validate records without resolving copies",
  "[Quest][CampaignConditions]")
{
  ConditionFixture f;
  const auto good = Compile(Clause(47, RunOn::Reference, 0x14, 10, 1, 0xf));
  for (const uint16_t function : { 35, 46, 47 }) {
    REQUIRE(f.Evaluate(
      { good,
        Compile(Clause(function, RunOn::Reference, kLucan, 0, 0, 0xf)) }));
    REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(kLucan));
    REQUIRE_THROWS(CampaignReferenceBinding::Resolve(*f.guest, kLucan));
  }
  for (const uint16_t function : { 35, 47 }) {
    REQUIRE(f.Evaluate(
      { good,
        Compile(Clause(function, RunOn::Reference, 0x8a986, 0, 0, 0xf)) }));
    REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(0x8a986));
    REQUIRE_THROWS(CampaignReferenceBinding::Resolve(*f.guest, 0x8a986));
  }
  auto context = f.Context();
  context.resolveReferenceAlias = [](uint32_t, uint32_t) -> uint32_t {
    throw std::runtime_error("Skipped alias must not be filled or resolved");
  };
  QuestJournal::Atomic(*f.guest, [&] {
    REQUIRE(CampaignCondition::Evaluate(
      { good, Compile(Clause(46, RunOn::QuestAlias, 0, 0, 0, 0, 0, 0)) },
      context));
  });
}

TEST_CASE("Campaign conditions preserve quest predicates global operands and "
          "OR groups",
          "[Quest][CampaignConditions]")
{
  ConditionFixture f;
  auto context = f.Context();
  std::vector<uint32_t> globals;
  context.readGlobal = [&](uint32_t id) {
    globals.push_back(id);
    return 10.0;
  };
  auto count = Clause(47, RunOn::Reference, 0x14, 0, 4, 0xf);
  const uint32_t rawGlobal = 0x01000042;
  std::memcpy(&count.comparisonValue, &rawGlobal, 4);
  const auto withGlobal = CampaignCondition::Compile(
    count, [](uint32_t raw) { return raw == 0x01000042 ? 0x42 : raw; });
  auto variable = Compile(Clause(629, RunOn::Subject, 0, 7, 0, kQuest));
  variable.SetVariableName("MyCounter");
  context.readQuestVariable = [&](uint32_t quest, const std::string& name) {
    REQUIRE(quest == kQuest);
    REQUIRE(name == "MyCounter");
    return 7.0;
  };
  QuestJournal::Atomic(*f.guest, [&] {
    f.Stage();
    REQUIRE(CampaignCondition::Evaluate(
      { Compile(Clause(56, RunOn::Subject, 0, 1, 0, kQuest)),
        Compile(Clause(58, RunOn::Reference, 0x14, 10, 0, kQuest)),
        Compile(Clause(59, RunOn::Subject, 0, 1, 0, kQuest, 10)),
        Compile(Clause(543, RunOn::Subject, 0, 0, 0, kQuest)),
        Compile(Clause(74, RunOn::Subject, 0, 10, 0, 0x43)), withGlobal,
        variable },
      context));
    // A AND (B OR C) AND D: B is false; C rescues its group, not D.
    const auto a = Compile(Clause(47, RunOn::Reference, 0x14, 10, 0, 0xf));
    const auto b = Compile(Clause(47, RunOn::Reference, 0x14, 99, 1, 0xf));
    REQUIRE(CampaignCondition::Evaluate({ a, b, a, a }, context));
    REQUIRE_FALSE(CampaignCondition::Evaluate(
      { a, b, a, Compile(Clause(47, RunOn::Reference, 0x14, 99, 0, 0xf)) },
      context));
    auto skipped = Compile(Clause(629, RunOn::Subject, 0, 0, 0, kQuest));
    skipped.SetVariableName("MustNotExecute");
    context.readQuestVariable = [](auto, const auto&) -> double {
      throw std::runtime_error("Unexpected OR branch evaluation");
    };
    REQUIRE(CampaignCondition::Evaluate(
      { Compile(Clause(47, RunOn::Reference, 0x14, 10, 1, 0xf)), skipped },
      context));
  });
  REQUIRE(globals == std::vector<uint32_t>({ 0x42, 0x43 }));
  REQUIRE_THROWS(variable.SetVariableName("duplicate"));
  REQUIRE_THROWS(Compile(Clause(46, RunOn::Reference, kNpc,
                                std::numeric_limits<float>::infinity())));
}

TEST_CASE("Campaign condition reads observe staged inventory and visibility "
          "then roll back",
          "[Quest][CampaignConditions]")
{
  ConditionFixture f;
  REQUIRE_THROWS(CampaignCondition::Evaluate({}, f.Context()));
  const auto before = QuestJournal::Snapshot(*f.owner);
  REQUIRE_THROWS_WITH(
    QuestJournal::Atomic(
      *f.guest,
      [&] {
        const auto count =
          Compile(Clause(47, RunOn::Reference, kNpc, 3, 0, 0xf));
        REQUIRE(CampaignCondition::Evaluate({ count }, f.Context()));
        QuestJournal::EditInventory(
          *f.guest, *f.npc, [](Inventory& inv) { inv.AddItem(0xf, 2); });
        REQUIRE(CampaignCondition::Evaluate(
          { Compile(Clause(47, RunOn::Reference, kNpc, 5, 0, 0xf)) },
          f.Context()));
        QuestJournal::SetReferenceDisabled(*f.guest, *f.npc, true);
        REQUIRE(CampaignCondition::Evaluate(
          { Compile(Clause(35, RunOn::Reference, kNpc, 1)) }, f.Context()));
        REQUIRE_FALSE(f.npc->IsDisabled());
        f.Stage();
        throw std::runtime_error("Fixture abort");
      }),
    Catch::Matchers::ContainsSubstring("Fixture abort"));
  REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
  REQUIRE(f.npc->GetInventory().GetItemCount(0xf) == 3);
  REQUIRE_FALSE(f.npc->IsDisabled());
}

TEST_CASE(
  "Campaign conditions reject uninitialized or missing private targets",
  "[Quest][CampaignConditions]")
{
  ConditionFixture f;
  SECTION("bound private inventory has not been initialized")
  {
    auto saved = f.npc->GetChangeForm();
    saved.baseContainerAdded = false;
    f.npc->ApplyChangeForm(saved);
    REQUIRE_THROWS_WITH(
      QuestJournal::Atomic(
        *f.guest,
        [&] {
          f.Stage();
          CampaignCondition::Evaluate(
            { Compile(Clause(47, RunOn::Reference, kNpc, 0, 0, 0xf)) },
            f.Context());
        }),
      Catch::Matchers::ContainsSubstring("inventory is not initialized"));
    REQUIRE(QuestJournal::Get(*f.owner, kQuest).at("stage") == 0);
  }
  SECTION("a missing private copy never loads the shared original")
  {
    REQUIRE_THROWS(
      f.Evaluate({ Compile(Clause(46, RunOn::Reference, kLucan)) }));
    REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(kLucan));
  }
  SECTION("a bound copy in another area is unavailable")
  {
    f.npc->SetWorldInstance(f.owner->GetWorldInstance() + "-other");
    REQUIRE_THROWS(
      f.Evaluate({ Compile(Clause(35, RunOn::Reference, kNpc)) }));
  }
}

TEST_CASE(
  "Stale campaign condition observations cannot commit stages or rewards",
  "[Quest][CampaignConditions]")
{
  ConditionFixture f;
  const bool reward = GENERATE(false, true);
  const auto before = QuestJournal::Snapshot(*f.owner);
  REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
    REQUIRE(CampaignCondition::Evaluate(
      { Compile(Clause(46, RunOn::Reference, kNpc)) }, f.Context()));
    f.Stage();
    if (reward)
      QuestJournal::EditInventory(
        *f.guest, *f.owner, [](Inventory& inv) { inv.AddItem(0xf, 50); });
    f.npc->SetIsDead(true);
  }));
  REQUIRE(f.npc->IsDead());
  REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
}

namespace {
VarValue Identifier(const char* name)
{
  return VarValue(VarValue::kType_Identifier, name);
}

struct LogFixture : ConditionFixture
{
  espm::QuestScriptData metadata;
  std::vector<std::shared_ptr<PexScript>> scripts;
  std::shared_ptr<PexScript> fixture;

  LogFixture()
  {
    for (const auto& [name, parent] :
         std::vector<std::pair<std::string, std::string>>{
           { "Form", "" },
           { "ObjectReference", "Form" },
           { "Actor", "ObjectReference" },
           { "Faction", "Form" },
           { "Quest", "Form" },
           { "Alias", "" },
           { "ReferenceAlias", "Alias" },
           { "Game", "" },
           { "ConditionFixture", "Quest" } }) {
      auto script = std::make_shared<PexScript>();
      script->source = name;
      Object object;
      object.NameIndex = name;
      object.parentClassName = parent;
      script->objectTable.push_back(object);
      scripts.push_back(script);
    }
    fixture = scripts.back();
    fixture->objectTable[0].variables.push_back(
      { "counter", "Int", 0, VarValue(0) });
    fixture->objectTable[0].states.push_back({ "", {} });
    metadata.version = 5;
    metadata.objectFormat = 2;
    metadata.scripts.push_back({ "ConditionFixture", 0, {} });
  }

  void Fragment(uint16_t stage, uint16_t log, int increment,
                bool addClaw = false, bool fail = false)
  {
    const auto name = "Fragment" + std::to_string(log);
    FunctionInfo fragment;
    fragment.valid = true;
    fragment.returnType = "None";
    fragment.locals = { { "player", "ObjectReference" },
                        { "item", "Form" },
                        { "temp", "None" } };
    fragment.code.instructions = {
      { FunctionCode::kOp_IAdd,
        { Identifier("counter"), Identifier("counter"), VarValue(increment) } }
    };
    if (addClaw) {
      fragment.code.instructions.push_back(
        { FunctionCode::kOp_CallStatic,
          { VarValue("Game"), VarValue("GetForm"), Identifier("player"),
            VarValue(1), VarValue(0x14) } });
      fragment.code.instructions.push_back(
        { FunctionCode::kOp_CallStatic,
          { VarValue("Game"), VarValue("GetForm"), Identifier("item"),
            VarValue(1), VarValue(static_cast<int32_t>(kClaw)) } });
      fragment.code.instructions.push_back(
        { FunctionCode::kOp_CallMethod,
          { VarValue("AddItem"), Identifier("player"), Identifier("temp"),
            VarValue(3), Identifier("item"), VarValue(1), VarValue(true) } });
    }
    if (fail)
      fragment.code.instructions.push_back(
        { FunctionCode::kOp_CallStatic,
          { VarValue("Unknown"), VarValue("Unsupported"), Identifier("temp"),
            VarValue(0) } });
    fragment.code.instructions.push_back(
      { FunctionCode::kOp_Return, { VarValue::None() } });
    fixture->objectTable[0].states[0].functions.push_back({ name, fragment });
    metadata.fragments.push_back({ stage, log, 1, fixture->source, name });
  }

  bool Run(uint32_t quest, uint16_t stage)
  {
    std::vector<PexScript::Lazy> lazy;
    for (const auto& script : scripts)
      lazy.push_back({ script->source, [script] { return script; } });
    return QuestScriptRuntime::ExecuteStage(*guest, quest, stage, metadata,
                                            lazy);
  }

  int Counter(uint32_t quest)
  {
    const auto key =
      FormDesc::FromFormId(quest, server.worldState.espmFiles).ToString() +
      "/conditionfixture";
    return QuestJournal::ScriptState(*owner)
      .at("scripts")
      .at(key)
      .at("values")
      .at("conditionfixture/counter")
      .at("value")
      .get<int>();
  }
};
}

TEST_CASE(
  "Owned MQ103 log reads the owner's staged reward from its earlier fragment",
  "[Quest][QuestVM][CampaignConditions]")
{
  LogFixture f;
  const bool reward = GENERATE(false, true);
  f.guest->SetInventory(Inventory().AddItem(kClaw, 1));
  // Owned MQ103 stage50: log0 unconditional, log1 PlayerRef.GetItemCount
  // GoldenClaw >0. A visiting player's existing claw must not select log1.
  f.Fragment(50, 0, 1, reward);
  f.Fragment(50, 1, 10);
  REQUIRE(f.Run(kMq103, 50));
  REQUIRE(f.Counter(kMq103) == (reward ? 11 : 1));
  REQUIRE(f.owner->GetInventory().GetItemCount(kClaw) == (reward ? 1 : 0));
  REQUIRE(f.guest->GetInventory().GetItemCount(kClaw) == 1);
}

TEST_CASE(
  "Failure in a selected owned log rolls back its earlier inventory reward",
  "[Quest][QuestVM][CampaignConditions]")
{
  LogFixture f;
  f.Fragment(50, 0, 1, true);
  f.Fragment(50, 1, 10, false, true);
  const auto before = QuestJournal::Snapshot(*f.owner);
  REQUIRE_THROWS(f.Run(kMq103, 50));
  REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
  REQUIRE(QuestJournal::ScriptState(*f.owner).empty());
  REQUIRE(f.owner->GetInventory().GetItemCount(kClaw) == 0);
}

TEST_CASE("Owned MS13 death conditions resolve Lucan alias zero in the active "
          "campaign",
          "[Quest][QuestVM][CampaignConditions]")
{
  LogFixture f;
  const bool dead = GENERATE(false, true);
  auto& lucan = f.AddNpc(*f.owner, kLucan, 0x1347a, 0xff005020);
  auto& other = f.AddNpc(*f.stranger, kLucan, 0x1347a, 0xff005021);
  lucan.SetIsDead(dead);
  other.SetIsDead(!dead);
  f.Fragment(10, 0, 1);
  f.Fragment(10, 1, 10);
  REQUIRE(f.Run(kMs13, 10));
  REQUIRE(f.Counter(kMs13) == (dead ? 10 : 1));
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(kLucan));
}

TEST_CASE(
  "Owned MQ302 faction logs select the campaign owner through explicit "
  "PlayerRef and forced alias43",
  "[Quest][QuestVM][CampaignConditions][CampaignFactions]")
{
  LogFixture f;
  const uint16_t stage = GENERATE(100, 110);
  const bool member = GENERATE(false, true);
  f.SetFactions(*f.owner, member ? 0 : -1);
  f.SetFactions(*f.guest, member ? -1 : 0);
  // Owned stage100 uses alias43 (forced PlayerRef); stage110 uses explicit
  // PlayerRef. Both have complementary CWSonsFaction membership conditions.
  f.Fragment(stage, 0, 1);
  f.Fragment(stage, 1, 10);
  REQUIRE(f.Run(kMq302, stage));
  REQUIRE(f.Counter(kMq302) == (member ? 10 : 1));
  REQUIRE(f.owner->GetChangeForm().factions->at(0).rank == (member ? 0 : -1));
  REQUIRE(f.guest->GetChangeForm().factions->at(0).rank == (member ? -1 : 0));
}

TEST_CASE(
  "A later owned faction log observes an earlier fragment's staged rank",
  "[Quest][QuestVM][CampaignConditions][CampaignFactions]")
{
  LogFixture f;
  const bool fail = GENERATE(false, true);
  f.SetFactions(*f.owner, -1);
  f.Fragment(110, 0, 1);
  f.Fragment(110, 1, 10, true, fail);
  auto& fragment = f.fixture->objectTable[0].states[0].functions[0].function;
  fragment.locals.push_back({ "faction", "Faction" });
  // The first real log requires nonmembership. Its fixture fragment joins
  // the faction; the second real log must then observe the staged membership.
  fragment.code.instructions.pop_back();
  fragment.code.instructions.push_back(
    { FunctionCode::kOp_CallStatic,
      { VarValue("Game"), VarValue("GetForm"), Identifier("player"),
        VarValue(1), VarValue(0x14) } });
  fragment.code.instructions.push_back(
    { FunctionCode::kOp_CallStatic,
      { VarValue("Game"), VarValue("GetForm"), Identifier("faction"),
        VarValue(1), VarValue(static_cast<int32_t>(kSonsFaction)) } });
  fragment.code.instructions.push_back(
    { FunctionCode::kOp_CallMethod,
      { VarValue("SetFactionRank"), Identifier("player"), Identifier("temp"),
        VarValue(2), Identifier("faction"), VarValue(4) } });
  fragment.code.instructions.push_back(
    { FunctionCode::kOp_Return, { VarValue::None() } });
  const auto before = QuestJournal::Snapshot(*f.owner);
  if (fail) {
    REQUIRE_THROWS(f.Run(kMq302, 110));
    REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
    REQUIRE(QuestJournal::ScriptState(*f.owner).empty());
    REQUIRE(f.owner->GetChangeForm().factions->at(0).rank == -1);
    REQUIRE(f.owner->GetInventory().GetItemCount(kClaw) == 0);
  } else {
    REQUIRE(f.Run(kMq302, 110));
    REQUIRE(f.Counter(kMq302) == 11);
    REQUIRE(f.owner->GetChangeForm().factions->at(0).rank == 4);
    REQUIRE(f.owner->GetInventory().GetItemCount(kClaw) == 1);
  }
}
