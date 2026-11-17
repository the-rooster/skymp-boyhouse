#include "CampaignFactionApi.h"
#include "TestUtils.hpp"
#include "UpdatePropertyMessage.h"
#include <catch2/catch_all.hpp>

extern espm::Loader& GetEspmLoader();

namespace {
struct FactionReplicationWorld
{
  PartOne server;
  std::vector<std::pair<uint32_t, nlohmann::json>> sent;
  FactionReplicationWorld()
  {
    server.worldState.espmFiles = { "Skyrim.esm", "Update.esm" };
  }
  MpActor& Add(uint32_t id, const std::string& instance, bool online = true,
               uint32_t baseId = 0)
  {
    auto callbacks = FormCallbacks::DoNothing();
    callbacks.getUserId = [online](auto*) {
      return online ? Networking::UserId(0) : Networking::InvalidUserId;
    };
    callbacks.sendToUser = [this](MpActor* recipient,
                                  const IMessageBase& message, bool reliable) {
      REQUIRE(reliable);
      nlohmann::json json;
      message.WriteJson(json);
      sent.emplace_back(recipient->GetFormId(), std::move(json));
    };
    server.worldState.AddForm(
      std::make_unique<MpActor>(
        LocationalData{ { 1, 2, 3 }, {}, FormDesc::Tamriel() }, callbacks,
        baseId),
      id);
    auto& actor = server.worldState.GetFormAt<MpActor>(id);
    actor.SetWorldInstance(instance);
    return actor;
  }
  void Set(MpActor& actor, std::optional<std::vector<Faction>> factions)
  {
    auto change = actor.GetChangeForm();
    change.factions = std::move(factions);
    actor.ApplyChangeForm(change);
  }
};
std::optional<nlohmann::json> Baseline(MpActor& actor)
{
  CreateActorMessage message;
  actor.VisitProperties(message, VisitPropertiesMode::All);
  for (const auto& property : message.customPropsJsonDumps)
    if (property.propName == "campaignFactions")
      return nlohmann::json::parse(property.propValueJsonDump);
  return {};
}
}

TEST_CASE("Faction baselines preserve authoritative empty versus missing and "
          "signed ranks",
          "[campaign-factions-replication]")
{
  FactionReplicationWorld world;
  auto& actor = world.Add(0xff007000, "campaign:factions");
  REQUIRE_FALSE(Baseline(actor));
  world.Set(actor, std::vector<Faction>{});
  REQUIRE(Baseline(actor) == nlohmann::json::array());
  REQUIRE(actor.GetFactions(-128, 127)
            .empty()); // saved[] must not attempt base loading
  world.Set(
    actor,
    std::vector<Faction>{ { FormDesc::FromString("abc:Update.esm"), -128 },
                          { FormDesc::FromString("123:Skyrim.esm"), -1 },
                          { FormDesc::FromString("456:Skyrim.esm"), 127 } });
  REQUIRE(
    Baseline(actor) ==
    nlohmann::json::array(
      { { { "factionId", 0x123 }, { "rank", -1 }, { "isMember", false } },
        { { "factionId", 0x456 }, { "rank", 127 }, { "isMember", true } },
        { { "factionId", 0x1000abc },
          { "rank", -128 },
          { "isMember", false } } }));
}

TEST_CASE("Faction publication uses complete reliable instance-filtered actor "
          "properties",
          "[campaign-factions-replication]")
{
  FactionReplicationWorld world;
  auto& owner = world.Add(0xff007000, "campaign:factions");
  auto& neighbour = world.Add(0xff007001, "campaign:factions");
  auto& outsider = world.Add(0xff007002, "campaign:other");
  world.Set(
    owner,
    std::vector<Faction>{ { FormDesc::FromString("123:Skyrim.esm"), 4 } });
  MpObjectReference::Subscribe(&owner, &neighbour);
  MpObjectReference::Subscribe(&owner, &outsider);
  world.sent.clear();
  owner.SendCampaignFactionsUpdate();
  REQUIRE(world.sent.size() == 2);
  for (const auto& [recipient, message] : world.sent) {
    REQUIRE(recipient != outsider.GetFormId());
    REQUIRE(message.at("idx") == owner.GetIdx());
    REQUIRE(message.at("refrId") == 0);
    REQUIRE(message.at("propName") == "campaignFactions");
    REQUIRE(nlohmann::json::parse(message.at("dataDump").get<std::string>()) ==
            Baseline(owner));
  }
  world.Set(owner, std::vector<Faction>{});
  world.sent.clear();
  owner.SendCampaignFactionsUpdate();
  REQUIRE_FALSE(world.sent.empty());
  REQUIRE(nlohmann::json::parse(
            world.sent.front().second.at("dataDump").get<std::string>())
            .empty());
}

TEST_CASE("Faction publication validates the whole snapshot before sending "
          "any recipient",
          "[campaign-factions-replication]")
{
  FactionReplicationWorld world;
  auto& owner = world.Add(0xff007000, "campaign:factions");
  world.Set(
    owner,
    std::vector<Faction>{ { FormDesc::FromString("123:Skyrim.esm"), 0 },
                          { FormDesc::FromString("123:Skyrim.esm"), 1 } });
  world.sent.clear();
  REQUIRE_THROWS(owner.SendCampaignFactionsUpdate());
  REQUIRE(world.sent.empty());
  world.Set(
    owner,
    std::vector<Faction>{ { FormDesc::FromString("123:Missing.esm"), 0 } });
  REQUIRE_THROWS(owner.SendCampaignFactionsUpdate());
  REQUIRE(world.sent.empty());
}

TEST_CASE(
  "Faction updates only reach observers whose baseline contains the actor",
  "[campaign-factions-replication]")
{
  FactionReplicationWorld world;
  auto& owner = world.Add(0xff007000, "");
  auto& direct = world.Add(0xff007001, "");
  auto& mappedOnly = world.Add(0xff007002, "");
  auto& offline = world.Add(0xff007003, "", false);
  world.Set(
    owner,
    std::vector<Faction>{ { FormDesc::FromString("123:Skyrim.esm"), 4 } });
  MpObjectReference::Subscribe(&owner, &direct);
  MpObjectReference::Subscribe(&owner, &offline);
  // The native baseline builder enumerates each online actor's emitters.
  REQUIRE(direct.GetEmitters().count(&owner) == 1);
  REQUIRE(mappedOnly.GetEmitters().count(&owner) == 0);
  const bool directHosts = GENERATE(false, true);
  world.server.worldState.hosters[offline.GetFormId()] =
    directHosts ? direct.GetFormId() : mappedOnly.GetFormId();
  world.sent.clear();
  owner.SendCampaignFactionsUpdate();
  std::vector<uint32_t> recipients;
  for (const auto& [recipient, message] : world.sent) {
    recipients.push_back(recipient);
    REQUIRE(message.at("idx") == owner.GetIdx());
  }
  std::sort(recipients.begin(), recipients.end());
  REQUIRE(recipients ==
          std::vector<uint32_t>{ owner.GetFormId(), direct.GetFormId() });
}

TEST_CASE("Faction baselines read immutable defaults without initializing "
          "live actor state",
          "[campaign-factions-replication]")
{
  FactionReplicationWorld world;
  world.server.AttachEspm(&GetEspmLoader());
  auto& actor = world.Add(0xff007000, "campaign:factions", true, 0x13bbd);
  world.Set(actor, std::nullopt);
  const auto before = MpChangeForm::ToJson(actor.GetChangeForm());
  const auto expected = CampaignFactionApi::ReadBaseFactions(actor);
  REQUIRE_FALSE(expected.empty());
  const auto initial = Baseline(actor);
  REQUIRE(initial);
  REQUIRE(initial->size() == expected.size());
  REQUIRE(MpChangeForm::ToJson(actor.GetChangeForm()) == before);
  REQUIRE_FALSE(actor.GetChangeForm().factions);
  for (const auto& entry : expected) {
    const auto id = entry.formDesc.ToFormId(world.server.worldState.espmFiles);
    const auto row =
      std::find_if(initial->begin(), initial->end(), [&](const auto& value) {
        return value.at("factionId") == id;
      });
    REQUIRE(row != initial->end());
    REQUIRE(row->at("rank") == entry.rank);
    REQUIRE(row->at("isMember") == (entry.rank >= 0));
  }
  const auto active =
    std::find_if(expected.begin(), expected.end(),
                 [](const auto& entry) { return entry.rank >= 0; });
  REQUIRE(active != expected.end());
  for (const int rank : { -128, -2, -1, 0, 127 }) {
    world.Set(
      actor,
      std::vector<Faction>{ { active->formDesc, static_cast<int8_t>(rank) } });
    const auto data = Baseline(actor);
    REQUIRE(data->size() == 1);
    REQUIRE(data->at(0).at("rank") == rank);
    REQUIRE(data->at(0).at("isMember") == (rank != -1));
  }
  world.Set(actor, std::vector<Faction>{});
  REQUIRE(Baseline(actor) == nlohmann::json::array());
  REQUIRE(actor.GetChangeForm().factions->empty());
}
