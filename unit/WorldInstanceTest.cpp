#include "ActionListener.h"
#include "DestroyActorMessage.h"
#include "MessageSerializerFactory.h"
#include "PacketParser.h"
#include "TestUtils.hpp"
#include <algorithm>
#include <slikenet/BitStream.h>

namespace {
class EventCountingTrigger : public MpObjectReference
{
public:
  EventCountingTrigger()
    : MpObjectReference(
        LocationalData{ { 100, 100, 100 }, {}, FormDesc::Tamriel() },
        FormCallbacks::DoNothing(), 0, "ACTI", NiPoint3{ 10, 10, 10 })
  {
  }
  void SendPapyrusEvent(const char* name, const VarValue*, size_t) override
  {
    if (std::string(name) == "OnTriggerEnter")
      ++enters;
    if (std::string(name) == "OnTrigger")
      ++inside;
  }
  int enters = 0;
  int inside = 0;
};

MpActor& AddPlayer(PartOne& server, uint32_t id, Networking::UserId user)
{
  DoConnect(server, user);
  server.CreateActor(id, { 1, 2, 3 }, 0, 0x3c);
  server.SetUserActor(user, id);
  return server.worldState.GetFormAt<MpActor>(id);
}
}

TEST_CASE("Queued trigger events retain both native identities and phases",
          "[Instance]")
{
  const std::string transition =
    GENERATE("none", "source", "emitter", "reuse");
  PartOne server;
  auto& actor = AddPlayer(server, 0xff000001, 0);
  constexpr uint32_t triggerId = 0xff000002;
  server.worldState.AddForm(std::make_unique<EventCountingTrigger>(),
                            triggerId);
  const auto lifetime = server.worldState.LookupFormByIdNoLoad(triggerId);
  auto& trigger = static_cast<EventCountingTrigger&>(*lifetime);
  trigger.ForceSubscriptionsUpdate();
  actor.SetPos({ 100, 100, 100 });
  REQUIRE(trigger.inside == 1);
  REQUIRE(trigger.enters == 0);
  if (transition == "source") {
    actor.SetWorldInstance("other");
    actor.SetWorldInstance("");
  } else if (transition == "emitter") {
    trigger.SetWorldInstance("other");
    trigger.SetWorldInstance("");
  } else if (transition == "reuse") {
    server.worldState.DestroyForm(triggerId);
    server.worldState.AddForm(std::make_unique<EventCountingTrigger>(),
                              triggerId);
  }
  server.worldState.Tick();
  REQUIRE(trigger.enters == (transition == "none" ? 1 : 0));
  if (transition == "reuse") {
    const auto replacement = server.worldState.LookupFormByIdNoLoad(triggerId);
    REQUIRE(static_cast<EventCountingTrigger&>(*replacement).enters == 0);
  }
}

TEST_CASE("Private instances remove observers and restore shared visibility",
          "[Instance]")
{
  PartOne server;
  auto& owner = AddPlayer(server, 0xff000001, 0);
  auto& guest = AddPlayer(server, 0xff000002, 1);
  auto& stranger = AddPlayer(server, 0xff000003, 2);
  REQUIRE(owner.GetListeners().count(&stranger) == 1);
  server.Messages().clear();

  owner.SetWorldInstance("owner:area");
  REQUIRE(owner.GetListeners().count(&owner) == 1);
  REQUIRE(owner.GetListeners().count(&stranger) == 0);
  REQUIRE(stranger.GetEmitters().count(&owner) == 0);
  REQUIRE(std::any_of(server.Messages().begin(), server.Messages().end(),
                      [&](const PartOne::Message& m) {
                        auto msg =
                          dynamic_cast<DestroyActorMessage*>(m.message.get());
                        return m.userId == 2 && msg &&
                          msg->idx == owner.GetIdx();
                      }));

  guest.SetWorldInstance("owner:area");
  REQUIRE(owner.GetListeners().count(&guest) == 1);
  REQUIRE(guest.GetListeners().count(&stranger) == 0);
  MpObjectReference::Subscribe(&owner, &stranger);
  REQUIRE(owner.GetListeners().count(&stranger) == 0);
  stranger.ForceSubscriptionsUpdate();
  REQUIRE(stranger.GetEmitters().count(&owner) == 0);

  owner.SetWorldInstance("");
  REQUIRE(stranger.GetEmitters().count(&owner) == 1);
  REQUIRE(guest.GetEmitters().count(&owner) == 0);
}

TEST_CASE("Movement is delivered only within the active instance",
          "[Instance]")
{
  PartOne server;
  auto& owner = AddPlayer(server, 0xff000001, 0);
  auto& guest = AddPlayer(server, 0xff000002, 1);
  AddPlayer(server, 0xff000003, 2);
  owner.SetWorldInstance("owner:area");
  guest.SetWorldInstance("owner:area");
  server.Messages().clear();
  DoUpdateMovement(server, owner.GetFormId(), 0);
  REQUIRE_FALSE(server.Messages().empty());
  for (const auto& msg : server.Messages()) {
    REQUIRE(msg.userId != 2);
  }
}

TEST_CASE("Instance changes revoke simulation leases in both directions",
          "[Instance]")
{
  PartOne server;
  auto& actor = AddPlayer(server, 0xff000001, 0);
  server.worldState.hosters[0xff000002] = actor.GetFormId();
  server.worldState.hosters[actor.GetFormId()] = 0xff000003;
  server.worldState.hosters[0xff000004] = 0xff000005;
  actor.SetWorldInstance("private");
  REQUIRE(server.worldState.hosters.size() == 1);
  REQUIRE(server.worldState.hosters.at(0xff000004) == 0xff000005);
}

TEST_CASE("Cross instance activations and container transfers are rejected",
          "[Instance]")
{
  PartOne server;
  auto& actor = AddPlayer(server, 0xff000001, 0);
  auto& target = AddPlayer(server, 0xff000002, 1);
  actor.SetWorldInstance("private");
  REQUIRE_THROWS_WITH(target.Activate(actor), "World instance does not match");
  REQUIRE_THROWS_WITH(target.PutItem(actor, Inventory::Entry(0xf, 1)),
                      "World instance does not match");
  REQUIRE_THROWS_WITH(target.TakeItem(actor, Inventory::Entry(0xf, 1)),
                      "World instance does not match");
}

TEST_CASE("Spatial visitors cannot discover another instance", "[Instance]")
{
  PartOne server;
  auto& actor = AddPlayer(server, 0xff000001, 0);
  auto& other = AddPlayer(server, 0xff000002, 1);
  other.SetWorldInstance("private");
  actor.VisitNeighbours(
    [&](MpObjectReference* ref) { REQUIRE(ref != &other); });
}

TEST_CASE("World instance survives serialization and old saves remain shared",
          "[Instance]")
{
  MpChangeForm source;
  source.worldInstance = "campaign:owner:location";
  auto json = MpChangeForm::ToJson(source);
  simdjson::dom::parser parser;
  auto parsed = parser.parse(json.dump()).value();
  auto restored = MpChangeForm::JsonToChangeForm(parsed);
  REQUIRE(restored.worldInstance == source.worldInstance);
  REQUIRE(restored == source);
  restored.worldInstance = "another";
  REQUIRE(restored != source);
  json.erase("worldInstance");
  parsed = parser.parse(json.dump()).value();
  REQUIRE(MpChangeForm::JsonToChangeForm(parsed).worldInstance.empty());
  json["worldInstance"] = 42;
  parsed = parser.parse(json.dump()).value();
  REQUIRE_THROWS(MpChangeForm::JsonToChangeForm(parsed));
}

TEST_CASE("Invalid instance IDs do not mutate visibility", "[Instance]")
{
  PartOne server;
  auto& actor = AddPlayer(server, 0xff000001, 0);
  auto& other = AddPlayer(server, 0xff000002, 1);
  REQUIRE_THROWS(actor.SetWorldInstance(std::string(129, 'x')));
  REQUIRE(actor.GetWorldInstance().empty());
  REQUIRE(actor.GetListeners().count(&other) == 1);
}

TEST_CASE("Forged cross instance hits spells and host requests are rejected",
          "[Instance]")
{
  PartOne server;
  auto& actor = AddPlayer(server, 0xff000001, 0);
  auto& target = AddPlayer(server, 0xff000002, 1);
  target.SetWorldInstance("private");
  RawMessageData raw;
  raw.userId = 0;
  HitMessage hit;
  hit.data.aggressor = 0x14;
  hit.data.target = target.GetFormId();
  REQUIRE_NOTHROW(server.GetActionListener().OnHit(raw, hit));
  REQUIRE(target.GetActorValues().healthPercentage == 1.f);
  SpellCastMessage spell;
  spell.data.caster = 0x14;
  spell.data.target = target.GetFormId();
  Equipment equipment;
  equipment.leftSpell = 0x12fcc;
  actor.SetEquipment(equipment);
  spell.data.spell = *equipment.leftSpell;
  spell.data.interruptCast = true;
  const auto packet =
    MakeMessage({ { "t", MsgType::SpellCast },
                  { "data",
                    { { "caster", 0x14 },
                      { "target", target.GetFormId() },
                      { "spell", spell.data.spell },
                      { "interruptCast", true },
                      { "isDualCasting", false },
                      { "castingSource", 0 },
                      { "aimAngle", 0 },
                      { "aimHeading", 0 },
                      { "actorAnimationVariables",
                        { { "booleans", nlohmann::json::array() },
                          { "floats", nlohmann::json::array() },
                          { "integers", nlohmann::json::array() } } } } } });
  raw.unparsed = reinterpret_cast<Networking::PacketData>(packet.data());
  raw.unparsedLength = packet.size();
  server.Messages().clear();
  REQUIRE_NOTHROW(server.GetActionListener().OnSpellCast(raw, spell));
  REQUIRE(server.Messages().empty());
  // Positive control: the identical equipped cast is forwarded in shared
  // space.
  target.SetWorldInstance("");
  server.Messages().clear();
  REQUIRE_NOTHROW(server.GetActionListener().OnSpellCast(raw, spell));
  REQUIRE(server.Messages().size() == 2);
  target.SetWorldInstance("private");

  server.CreateActor(0xff000003, { 1, 2, 3 }, 0, 0x3c);
  auto& npc = server.worldState.GetFormAt<MpActor>(0xff000003);
  npc.SetWorldInstance("private");
  HostMessage host;
  host.remoteId = npc.GetFormId();
  server.GetActionListener().OnHostAttempt(raw, host);
  REQUIRE(server.worldState.hosters.count(npc.GetFormId()) == 0);
  // Even an obsolete/injected lease cannot authorize a different scope.
  server.worldState.hosters[npc.GetFormId()] = actor.GetFormId();
  hit.data.aggressor = npc.GetFormId();
  REQUIRE_NOTHROW(server.GetActionListener().OnHit(raw, hit));
  REQUIRE(target.GetActorValues().healthPercentage == 1.f);
}

TEST_CASE("Script inventory transfers do not bypass instance isolation",
          "[Instance]")
{
  PartOne server;
  auto& source = AddPlayer(server, 0xff000001, 0);
  auto& target = AddPlayer(server, 0xff000002, 1);
  source.AddItem(0xf, 10);
  target.SetWorldInstance("private");
  REQUIRE_THROWS(source.RemoveItem(0xf, 5, &target));
  REQUIRE(source.GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(target.GetInventory().GetItemCount(0xf) == 0);
}

TEST_CASE("Projection gate blocks binary and JSON gameplay without losing the "
          "connection",
          "[Instance][Projection]")
{
  PartOne server;
  auto& actor = AddPlayer(server, 0xff000001, 0);
  AddPlayer(server, 0xff000002, 1);
  auto& listener = server.GetActionListener();
  server.serverState.userInfo[0]->gameplayBlocked = true;
  for (int type = 0; type < 128; ++type)
    REQUIRE(listener.IsMessageAllowed(0, static_cast<MsgType>(type)) ==
            (static_cast<MsgType>(type) == MsgType::CustomPacket));
  REQUIRE(listener.IsMessageAllowed(1, MsgType::UpdateMovement));
  server.Messages().clear();
  DoUpdateMovement(server, actor.GetFormId(), 0);
  REQUIRE(server.Messages().empty());
  UpdateMovementMessage movement;
  movement.idx = actor.GetIdx();
  movement.data.worldOrCell = 0x3c;
  movement.data.pos = { 1, 2, 3 };
  movement.data.rot = { 0, 0, 0 };
  SLNet::BitStream stream;
  MessageSerializerFactory::CreateMessageSerializer()->Serialize(movement,
                                                                 stream);
  PacketParser parser;
  parser.TransformPacketIntoAction(0, stream.GetData(),
                                   stream.GetNumberOfBytesUsed(), listener);
  REQUIRE(server.Messages().empty());
  REQUIRE(server.serverState.IsConnected(0));
  REQUIRE(server.GetUserActor(0) == actor.GetFormId());
  server.serverState.userInfo[0]->gameplayBlocked = false;
  DoUpdateMovement(server, actor.GetFormId(), 0);
  REQUIRE_FALSE(server.Messages().empty());
  server.serverState.userInfo[0]->gameplayBlocked = true;
  DoDisconnect(server, 0);
  DoConnect(server, 0);
  REQUIRE_FALSE(server.serverState.userInfo[0]->gameplayBlocked);
}

TEST_CASE(
  "A blocked connection accepts projection control and initial login only",
  "[Instance][Projection]")
{
  PartOne server;
  DoConnect(server, 0);
  auto events = FakeListener::New();
  server.AddListener(events);
  server.serverState.userInfo[0]->gameplayBlocked = true;
  const auto send = [&](const std::string& type) {
    DoMessage(server, 0,
              { { "t", MsgType::CustomPacket },
                { "contentJsonDump",
                  nlohmann::json({ { "customPacketType", type } }).dump() } });
  };
  send("boyhouse.instance");
  send("boyhouse.questJournal");
  send("arbitrary-game-action");
  REQUIRE(events->str().empty());
  send("loginWithSkympIo");
  REQUIRE_FALSE(events->str().empty());
  events->clear();
  server.CreateActor(0xff000001, { 1, 2, 3 }, 0, 0x3c);
  server.SetUserActor(0, 0xff000001);
  send("loginWithSkympIo");
  REQUIRE(events->str().empty());
  send("boyhouse.projection");
  REQUIRE_FALSE(events->str().empty());
  events->clear();
  server.serverState.userInfo[0]->gameplayBlocked = false;
  send("boyhouse.instance");
  REQUIRE_FALSE(events->str().empty());
}
