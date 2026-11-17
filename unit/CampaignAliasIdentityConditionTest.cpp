#include "CampaignCondition.h"
#include "CampaignReferenceBinding.h"
#include "CampaignReferenceScripts.h"
#include "QuestJournal.h"
#include "QuestScriptRuntime.h"
#include "QuestVmState.h"
#include "TestUtils.hpp"
#include "papyrus-vm/Reader.h"
#include "script_storages/BsaArchiveScriptStorage.h"
#include <climits>
#include <cstring>
#include <filesystem>
#include <set>

extern espm::Loader& GetEspmLoader();

namespace {
constexpr uint32_t kAlchemy = 0x10557c;
constexpr uint32_t kSmithing = 0x105385;
constexpr uint32_t kQuest = 0x4e50d;
constexpr uint32_t kNpc = 0x1a677;
using RunOn = espm::CTDA::RunOnTypeFlags;

espm::CTDA Clause(RunOn runOn, uint32_t reference, uint32_t operand,
                  int32_t alias = -1, float value = 1, uint8_t flags = 0)
{
  uint8_t bytes[32]{};
  const uint16_t function = 566;
  bytes[0] = flags;
  std::memcpy(bytes + 4, &value, 4);
  std::memcpy(bytes + 8, &function, 2);
  std::memcpy(bytes + 12, &operand, 4);
  std::memcpy(bytes + 20, &runOn, 4);
  std::memcpy(bytes + 24, &reference, 4);
  std::memcpy(bytes + 28, &alias, 4);
  espm::CTDA result;
  std::memcpy(&result, bytes, sizeof(result));
  return result;
}

CampaignCondition Compile(const espm::CTDA& condition)
{
  return CampaignCondition::Compile(condition, [](uint32_t id) { return id; });
}

struct Fixture
{
  PartOne server;
  MpActor *owner, *guest, *stranger, *npc;

  Fixture()
  {
    server.AttachEspm(&GetEspmLoader());
    owner = &AddPlayer(0xff006001);
    guest = &AddPlayer(0xff006002);
    stranger = &AddPlayer(0xff006003);
    owner->SetWorldInstance("campaign:" + std::to_string(owner->GetFormId()) +
                            ":alias-identity");
    guest->SetWorldInstance(owner->GetWorldInstance());
    stranger->SetWorldInstance(
      "campaign:" + std::to_string(stranger->GetFormId()) + ":alias-identity");
    npc = &AddNpc(*owner, kNpc, 0x13bbd, 0xff006010);
    server.worldState.SetCampaignVisibilityCallbacks([](MpActor&) {},
                                                     [](MpActor&, bool) {});
  }

  MpActor& AddPlayer(uint32_t id)
  {
    server.CreateActor(id, { 1, 2, 3 }, 0, 0x3c);
    auto& actor = server.worldState.GetFormAt<MpActor>(id);
    actor.SetInventory(Inventory().AddItem(0xf, 10));
    actor.SetPropertyValueDump(
      QuestJournal::kEnabled,
      "[\"4e50d:Skyrim.esm\",\"10557c:Skyrim.esm\",\"105385:Skyrim.esm\"]",
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
    actor.SetInventory(Inventory());
    CampaignReferenceBinding::Bind(campaign, original, actor);
    return actor;
  }

  MpObjectReference& AddMarker(uint32_t original, uint32_t id)
  {
    server.worldState.AddForm(
      std::make_unique<MpObjectReference>(
        LocationalData{ { 1, 2, 3 }, {}, FormDesc::Tamriel() },
        FormCallbacks::DoNothing(), 0x3b, "STAT"),
      id);
    auto& reference = server.worldState.GetFormAt<MpObjectReference>(id);
    reference.SetWorldInstance(owner->GetWorldInstance());
    reference.SetInventory(Inventory());
    CampaignReferenceBinding::Bind(*owner, original, reference);
    return reference;
  }

  CampaignCondition::Context Context()
  {
    return { *guest, kQuest, {}, {}, {} };
  }

  void Reward()
  {
    QuestJournal::Apply(
      *guest, { { "quest", kQuest }, { "op", "setStage" }, { "index", 10 } });
    QuestJournal::EditInventory(*guest, *owner, [](Inventory& inventory) {
      inventory.AddItem(0xf, 50);
    });
  }
};

VarValue Identifier(const char* name)
{
  return VarValue(VarValue::kType_Identifier, name);
}

std::string Lower(std::string name)
{
  for (auto& c : name)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return name;
}

struct AuthoredFixture : Fixture
{
  espm::QuestScriptData metadata;
  std::vector<PexScript::Lazy> scripts;
  std::shared_ptr<PexScript> fragment;
  std::set<std::string> referenceScriptNames;
  std::unique_ptr<BsaArchiveScriptStorage> ownedScripts;

  AuthoredFixture()
  {
    for (const auto& [name, parent] :
         std::vector<std::pair<std::string, std::string>>{
           { "Form", "" },
           { "ObjectReference", "Form" },
           { "Actor", "ObjectReference" },
           { "Quest", "Form" },
           { "Alias", "" },
           { "ReferenceAlias", "Alias" },
           { "AliasIdentityFixture", "Quest" } }) {
      auto script = std::make_shared<PexScript>();
      script->source = name;
      Object object;
      object.NameIndex = name;
      object.parentClassName = parent;
      script->objectTable.push_back(object);
      scripts.push_back({ name, [script] { return script; } });
      referenceScriptNames.insert(Lower(name));
      fragment = script;
    }
    fragment->objectTable[0].variables.push_back(
      { "counter", "Int", 0, VarValue(0) });
    fragment->objectTable[0].states.push_back({ "", {} });
    metadata.version = 5;
    metadata.objectFormat = 2;
    metadata.scripts.push_back({ fragment->source, 0, {} });
  }

  void AddOwnedScript(const std::string& name)
  {
    if (name.empty() || !referenceScriptNames.insert(Lower(name)).second)
      return;
    if (!ownedScripts) {
      ownedScripts = std::make_unique<BsaArchiveScriptStorage>(
        (std::filesystem::path(GetDataDir()) / "Skyrim - Misc.bsa")
          .string()
          .c_str());
      ownedScripts->ListScripts(false);
    }
    auto loaded = Reader(std::vector<std::vector<uint8_t>>{
                           ownedScripts->GetScriptPex(name.c_str()) })
                    .GetSourceStructures();
    REQUIRE(loaded.size() == 1);
    auto script = loaded.front();
    REQUIRE(script->objectTable.size() == 1);
    script->source = name;
    AddOwnedScript(script->objectTable.front().parentClassName);
    scripts.push_back({ name, [script] { return script; } });
  }

  void AddAuthoredNpc(uint32_t original, uint32_t base, uint32_t id)
  {
    auto& actor = AddNpc(*owner, original, base, id);
    for (const auto& binding : CampaignReferenceScripts::Read(
           server.worldState,
           server.worldState.GetEspm().GetBrowser().LookupById(original),
           [&] { return actor.GetChangeForm().templateChain; }))
      // Alias selection resolves the real NPC and therefore attaches its
      // original PEX, including properties and custom inheritance. Loading
      // these attachments does not execute their lifecycle handlers.
      AddOwnedScript(binding.name);
  }

  void Fragment(uint16_t log, int amount, bool fail = false)
  {
    const auto name = "Fragment" + std::to_string(log);
    FunctionInfo body;
    body.valid = true;
    body.returnType = "None";
    body.locals = { { "temp", "None" } };
    body.code.instructions.push_back(
      { FunctionCode::kOp_IAdd,
        { Identifier("counter"), Identifier("counter"), VarValue(amount) } });
    if (fail)
      body.code.instructions.push_back(
        { FunctionCode::kOp_CallStatic,
          { VarValue("Unknown"), VarValue("Unsupported"), Identifier("temp"),
            VarValue(0) } });
    body.code.instructions.push_back(
      { FunctionCode::kOp_Return, { VarValue::None() } });
    fragment->objectTable[0].states[0].functions.push_back({ name, body });
    metadata.fragments.push_back({ 10, log, 1, fragment->source, name });
  }

  void BindGiver(uint32_t quest, uint32_t alias,
                 std::optional<uint32_t> reference)
  {
    QuestJournal::Atomic(*guest, [&] {
      QuestVmState state(QuestJournal::ScriptState(*guest), {}, {});
      state.SetAliasBinding(
        FormDesc::FromFormId(quest, server.worldState.espmFiles).ToString() +
          "/alias:" + std::to_string(alias),
        reference ? nlohmann::json(FormDesc::FromFormId(
                                     *reference, server.worldState.espmFiles)
                                     .ToString())
                  : nlohmann::json(nullptr));
      QuestJournal::SetScriptState(*guest, state.Save());
    });
  }

  int Counter(uint32_t quest)
  {
    const auto saved = QuestJournal::ScriptState(*owner);
    const auto key =
      FormDesc::FromFormId(quest, server.worldState.espmFiles).ToString() +
      "/aliasidentityfixture";
    if (!saved.at("scripts").contains(key))
      return 0;
    const auto& values = saved.at("scripts").at(key).at("values");
    return values.contains("aliasidentityfixture/counter")
      ? values.at("aliasidentityfixture/counter").at("value").get<int>()
      : 0;
  }
};
}

TEST_CASE("Alias identity conditions preserve both quest-local alias IDs",
          "[Quest][CampaignConditions][CampaignAliasIdentity]")
{
  Fixture f;
  for (const uint32_t operand : { 0u, 17u }) {
    const auto condition = CampaignCondition::Compile(
      Clause(RunOn::QuestAlias, 0xfeabcdef, operand, 2),
      [](uint32_t) -> uint32_t {
        throw std::runtime_error("Alias IDs must not be remapped");
      });
    auto context = f.Context();
    std::vector<uint32_t> resolved;
    context.resolveReferenceAlias = [&](uint32_t quest, uint32_t alias) {
      REQUIRE(quest == kQuest);
      resolved.push_back(alias);
      return kNpc;
    };
    QuestJournal::Atomic(*f.guest, [&] {
      REQUIRE(CampaignCondition::Evaluate({ condition }, context));
    });
    REQUIRE(resolved == std::vector<uint32_t>{ 2, operand });
  }
  std::vector<uint32_t> mapped;
  const auto explicitRef = CampaignCondition::Compile(
    Clause(RunOn::Reference, 0x01000014, 0), [&](uint32_t raw) {
      mapped.push_back(raw);
      return raw & 0xffffff;
    });
  REQUIRE(mapped == std::vector<uint32_t>{ 0x01000014 });
  auto context = f.Context();
  context.resolveReferenceAlias = [](auto, auto) { return 0x14; };
  QuestJournal::Atomic(*f.guest, [&] {
    REQUIRE(CampaignCondition::Evaluate({ explicitRef }, context));
  });
  for (const auto invalid :
       { Clause(RunOn::Subject, 0, 0), Clause(RunOn::Target, 0, 0),
         Clause(RunOn::QuestAlias, 0, 0, -1),
         Clause(RunOn::QuestAlias, 0, uint32_t{ INT_MAX } + 1, 0),
         Clause(RunOn::Reference, 0x14, 0xffffffff),
         Clause(RunOn::Reference, 0x14, 0, -1, 1, 2),
         Clause(RunOn::Reference, 0x14, 0, -1, 1, 8),
         Clause(RunOn::Reference, 0x14, 0, -1, 1, 16) })
    REQUIRE_THROWS(Compile(invalid));
}

TEST_CASE("GetIsAliasRef compares campaign references rather than base forms",
          "[Quest][CampaignConditions][CampaignAliasIdentity]")
{
  Fixture f;
  auto& first = f.AddMarker(0x105580, 0xff006020);
  auto& second = f.AddMarker(0x105581, 0xff006021);
  REQUIRE(first.GetBaseId() == second.GetBaseId());
  const bool same = GENERATE(false, true);
  auto context = f.Context();
  context.resolveReferenceAlias = [&](auto, auto) {
    return same ? 0x105580 : 0x105581;
  };
  QuestJournal::Atomic(*f.guest, [&] {
    REQUIRE(CampaignCondition::Evaluate(
      { Compile(Clause(RunOn::Reference, 0x105580, 0, -1, same ? 1 : 0)) },
      context));
  });
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(0x105580));
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(0x105581));
}

TEST_CASE("Empty alias identity is false while unresolved aliases fail closed",
          "[Quest][CampaignConditions][CampaignAliasIdentity]")
{
  Fixture f;
  const auto condition = Compile(Clause(RunOn::QuestAlias, 0, 0, 2, 0));
  auto context = f.Context();
  for (const auto empty : { 0, 1, 2 }) {
    context.resolveReferenceAlias = [=](auto, uint32_t alias) {
      return (empty == 2 || (alias == 0) == (empty == 0)) ? 0u : kNpc;
    };
    QuestJournal::Atomic(*f.guest, [&] {
      REQUIRE(CampaignCondition::Evaluate({ condition }, context));
    });
  }
  const auto before = QuestJournal::Snapshot(*f.owner);
  for (const uint32_t invalid : { f.npc->GetFormId(), kQuest, 0xfeffffffu }) {
    context.resolveReferenceAlias = [=](auto, auto) { return invalid; };
    REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
      f.Reward();
      CampaignCondition::Evaluate({ condition }, context);
    }));
    REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
    REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
  }
  context.resolveReferenceAlias = {};
  REQUIRE_THROWS(QuestJournal::Atomic(
    *f.guest, [&] { CampaignCondition::Evaluate({ condition }, context); }));
}

TEST_CASE("Alias identity preserves OR short circuit and physical read guards",
          "[Quest][CampaignConditions][CampaignAliasIdentity]")
{
  Fixture f;
  auto context = f.Context();
  context.resolveReferenceAlias = [](auto, uint32_t alias) {
    if (alias == 99)
      throw std::runtime_error("Skipped alias resolution");
    return 0x14;
  };
  QuestJournal::Atomic(*f.guest, [&] {
    REQUIRE(CampaignCondition::Evaluate(
      { Compile(Clause(RunOn::Reference, 0x14, 0, -1, 1, 1)),
        Compile(Clause(RunOn::QuestAlias, 0, 99, 99)) },
      context));
  });
  const auto before = QuestJournal::Snapshot(*f.owner);
  // The operand differs from the owner, but must still be a guarded read.
  context.resolveReferenceAlias = [](auto, auto) { return kNpc; };
  REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
    REQUIRE(CampaignCondition::Evaluate(
      { Compile(Clause(RunOn::Reference, 0x14, 0, -1, 0)) }, context));
    f.Reward();
    f.npc->SetIsDead(true);
  }));
  REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(f.npc->IsDead());
}

TEST_CASE("Alias identity refuses missing or cross-phase private copies",
          "[Quest][CampaignConditions][CampaignAliasIdentity]")
{
  Fixture f;
  auto context = f.Context();
  const auto condition = Compile(Clause(RunOn::Reference, 0x14, 0, -1, 0));
  const bool wrongPhase = GENERATE(false, true);
  const auto original = wrongPhase ? kNpc : 0x13487u;
  if (wrongPhase)
    f.npc->SetWorldInstance(f.stranger->GetWorldInstance());
  context.resolveReferenceAlias = [=](auto, auto) { return original; };
  const auto before = QuestJournal::Snapshot(*f.owner);
  REQUIRE_THROWS(QuestJournal::Atomic(*f.guest, [&] {
    f.Reward();
    CampaignCondition::Evaluate({ condition }, context);
  }));
  REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
  REQUIRE_FALSE(f.server.worldState.LookupFormByIdNoLoad(original));
}

TEST_CASE("Owned tutorial stage logs select the current quest-giver alias",
          "[Quest][QuestVM][CampaignConditions][CampaignAliasIdentity]")
{
  AuthoredFixture f;
  const bool alchemy = GENERATE(false, true);
  const int selected = GENERATE(-1, 0, 1, 2);
  const uint32_t quest = alchemy ? kAlchemy : kSmithing;
  const std::vector<std::pair<uint32_t, uint32_t>> targets = alchemy
    ? std::vector<std::pair<uint32_t, uint32_t>>{ { 0x13486, 0x13479 },
                                                  { 0x3a19b, 0x3a19a },
                                                  { 0x1a66d, 0x13ba4 } }
    : std::vector<std::pair<uint32_t, uint32_t>>{ { 0x13482, 0x13475 },
                                                  { 0x1a67c, 0x13bb9 },
                                                  { 0x3a19f, 0x13650 } };
  for (size_t i = 0; i < targets.size(); ++i)
    f.AddAuthoredNpc(targets[i].first, targets[i].second, 0xff006030 + i);
  f.BindGiver(quest, alchemy ? 0 : 5,
              selected < 0 ? std::nullopt
                           : std::optional<uint32_t>(targets[selected].first));
  f.Fragment(0, 1);
  f.Fragment(1, 10);
  f.Fragment(2, 100);
  // Conditions and ALUA definitions come from the original winning QUST.
  // Only fragment bodies are fixtures: this is not whole-quest certification.
  REQUIRE(QuestScriptRuntime::ExecuteStage(*f.guest, quest, 10, f.metadata,
                                           f.scripts));
  const int expected = alchemy ? 1 +
      (selected == 1     ? 10
         : selected == 2 ? 100
                         : 0)
                               : (selected == 0     ? 1
                                    : selected == 1 ? 10
                                    : selected == 2 ? 100
                                                    : 0);
  REQUIRE(f.Counter(quest) == expected);
  REQUIRE(QuestJournal::Get(*f.owner, quest).at("stage") == 10);
  REQUIRE(QuestJournal::Get(*f.guest, quest) ==
          QuestJournal::Get(*f.owner, quest));
  REQUIRE(QuestJournal::ScriptState(*f.guest) ==
          QuestJournal::ScriptState(*f.owner));
}

TEST_CASE("Failure in an owned alias-selected log rolls back prior fragments",
          "[Quest][QuestVM][CampaignConditions][CampaignAliasIdentity]")
{
  AuthoredFixture f;
  f.AddAuthoredNpc(0x3a19b, 0x3a19a, 0xff006030);
  f.BindGiver(kAlchemy, 0, 0x3a19b);
  f.Fragment(0, 1);
  f.Fragment(1, 10, true);
  f.Fragment(2, 100);
  const auto before = QuestJournal::Snapshot(*f.owner);
  REQUIRE_THROWS_WITH(
    QuestScriptRuntime::ExecuteStage(*f.guest, kAlchemy, 10, f.metadata,
                                     f.scripts),
    Catch::Matchers::ContainsSubstring("script is missing - 'Unknown'"));
  REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
}
