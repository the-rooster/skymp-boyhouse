#include "ActionListener.h"
#include "CampaignFrameMessage.h"
#include "CustomPacketMessage.h"
#include "MessageSerializerFactory.h"
#include "MinPacketId.h"
#include "MpClientPlugin.h"
#include "PacketParser.h"
#include "TestUtils.hpp"
#include "UpdateMovementMessage.h"
#include <slikenet/BitStream.h>

namespace {
std::vector<uint8_t> Encode(const IMessageBase& message)
{
  SLNet::BitStream stream;
  MessageSerializerFactory::CreateMessageSerializer()->Serialize(message,
                                                                 stream);
  return { stream.GetData(),
           stream.GetData() + stream.GetNumberOfBytesUsed() };
}

CampaignFrameMessage Frame(const std::string& epoch,
                           const IMessageBase& message)
{
  CampaignFrameMessage frame;
  frame.epoch = epoch;
  frame.payload = Encode(message);
  return frame;
}

MpActor& Player(PartOne& server, uint32_t id, Networking::UserId user)
{
  DoConnect(server, user);
  server.CreateActor(id, { 1, 2, 3 }, 0, 0x3c);
  server.SetUserActor(user, id);
  return server.worldState.GetFormAt<MpActor>(id);
}

std::string Advance(PartOne& server, Networking::UserId user)
{
  server.serverState.userInfo[user]->gameplayBlocked = true;
  auto epoch = server.BeginPacketEpoch(user);
  server.serverState.userInfo[user]->gameplayBlocked = false;
  return epoch;
}
}

TEST_CASE("Campaign frames preserve binary movement and native JSON views",
          "[Projection][Serialization][CampaignFrame]")
{
  UpdateMovementMessage movement;
  movement.idx = 7;
  movement.data.worldOrCell = 0x3c;
  movement.data.pos = { 25, -32, 100 };
  movement.data.rot = { 0, 0, 90 };
  movement.data.runMode = "Running";
  auto frame = Frame("0123456789abcdef0123456789abcdef", movement);
  const auto bytes = Encode(frame);
  REQUIRE(bytes.size() == frame.payload.size() + 22);
  auto decoded =
    MessageSerializerFactory::CreateMessageSerializer()->Deserialize(
      bytes.data(), bytes.size());
  REQUIRE(decoded);
  REQUIRE(decoded->msgType == MsgType::CampaignFrame);
  const auto& result = static_cast<CampaignFrameMessage&>(*decoded->message);
  REQUIRE(result.epoch == frame.epoch);
  REQUIRE(result.payload == frame.payload);
  nlohmann::json output, expected;
  result.WriteJson(output);
  movement.WriteJson(expected);
  REQUIRE(output["message"] == expected);

  // This is the same JSON serialization path used by MpClientPlugin::Send.
  SLNet::BitStream stream;
  MessageSerializerFactory::CreateMessageSerializer()->Serialize(
    output.dump().c_str(), stream);
  REQUIRE(std::vector<uint8_t>(stream.GetData(),
                               stream.GetData() +
                                 stream.GetNumberOfBytesUsed()) == bytes);
  output.erase("message");
  output["raw"] = frame.payload;
  stream.Reset();
  MessageSerializerFactory::CreateMessageSerializer()->Serialize(
    output.dump().c_str(), stream);
  REQUIRE(std::vector<uint8_t>(stream.GetData(),
                               stream.GetData() +
                                 stream.GetNumberOfBytesUsed()) == bytes);
}

TEST_CASE(
  "Campaign frames reject truncation, trailing bytes and nested frames",
  "[Projection][Serialization][CampaignFrame]")
{
  CustomPacketMessage message;
  message.contentJsonDump = R"({"customPacketType":"test"})";
  auto frame = Frame(std::string(32, 'a'), message);
  const auto bytes = Encode(frame);
  for (size_t length = 2; length < bytes.size(); ++length) {
    auto serializer = MessageSerializerFactory::CreateMessageSerializer();
    REQUIRE_THROWS(serializer->Deserialize(bytes.data(), length));
  }
  auto trailing = bytes;
  trailing.push_back(0);
  REQUIRE_THROWS(
    MessageSerializerFactory::CreateMessageSerializer()->Deserialize(
      trailing.data(), trailing.size()));

  auto nested = frame;
  nested.payload = bytes;
  REQUIRE_THROWS(Encode(nested));
  const std::string nestedJson =
    R"({"t":34,"epoch":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","message":{"t":1,"contentJsonDump":"{}"}})";
  nested.payload.assign(1, static_cast<uint8_t>(Networking::MinPacketId));
  nested.payload.insert(nested.payload.end(), nestedJson.begin(),
                        nestedJson.end());
  REQUIRE_THROWS(Encode(nested));

  // Refuse declared lengths before allocating, and keep the previous object
  // intact when a decode fails midway.
  SLNet::BitStream huge;
  huge.Write(static_cast<uint8_t>(MsgType::CampaignFrame));
  for (int i = 0; i < 16; ++i)
    huge.Write(static_cast<uint8_t>(0));
  huge.Write(static_cast<uint32_t>(CampaignFrameMessage::kMaxPayload + 1));
  auto before = frame.payload;
  REQUIRE_THROWS(frame.ReadBinary(huge));
  REQUIRE(frame.payload == before);
  REQUIRE(frame.epoch == std::string(32, 'a'));
}

TEST_CASE(
  "Campaign frame JSON requires one bounded payload and a canonical epoch",
  "[Projection][Serialization][CampaignFrame]")
{
  const nlohmann::json valid = { { "t", MsgType::CampaignFrame },
                                 { "epoch", std::string(32, 'f') },
                                 { "message",
                                   { { "t", MsgType::CustomPacket },
                                     { "contentJsonDump", "{}" } } } };
  const auto decode = [](const nlohmann::json& json) {
    simdjson::dom::parser parser;
    CampaignFrameMessage frame;
    frame.ReadJson(parser.parse(json.dump()).value());
  };
  REQUIRE_NOTHROW(decode(valid));
  for (const std::string& epoch : std::vector<std::string>{
         "", std::string(31, 'a'), std::string(32, 'G'),
         std::string(33, 'a') }) {
    auto invalid = valid;
    invalid["epoch"] = epoch;
    REQUIRE_THROWS(decode(invalid));
  }
  auto both = valid;
  both["raw"] = { 134, 1 };
  REQUIRE_THROWS(decode(both));
  auto neither = valid;
  neither.erase("message");
  REQUIRE_THROWS(decode(neither));
  auto badByte = neither;
  badByte["raw"] = { 134, 1, 256 };
  REQUIRE_THROWS(decode(badByte));
  auto nested = valid;
  nested["message"] = valid;
  REQUIRE_THROWS(decode(nested));
}

TEST_CASE("A reopened gameplay gate still rejects old and unframed movement",
          "[Instance][Projection][CampaignFrame]")
{
  PartOne server;
  auto& actor = Player(server, 0xff000001, 0);
  Player(server, 0xff000002, 1);
  UpdateMovementMessage movement;
  movement.idx = actor.GetIdx();
  movement.data.worldOrCell = 0x3c;
  movement.data.pos = { 1, 2, 3 };
  movement.data.rot = { 0, 0, 0 };
  const auto oldEpoch = Advance(server, 0);
  auto packet = Encode(Frame(oldEpoch, movement));
  PacketParser parser;
  const auto send = [&](const std::vector<uint8_t>& data) {
    parser.TransformPacketIntoAction(0, data.data(), data.size(),
                                     server.GetActionListener());
  };
  server.Messages().clear();
  send(packet);
  REQUIRE_FALSE(server.Messages().empty());

  const auto current = Advance(server, 0);
  REQUIRE(current != oldEpoch);
  server.Messages().clear();
  send(packet);
  send(Encode(movement));
  DoUpdateMovement(server, actor.GetFormId(), 0);
  REQUIRE(server.Messages().empty());
  packet = Encode(Frame(current, movement));
  server.serverState.userInfo[0]->gameplayBlocked = true;
  send(packet);
  REQUIRE(server.Messages().empty());
  server.serverState.userInfo[0]->gameplayBlocked = false;
  send(packet);
  REQUIRE_FALSE(server.Messages().empty());
}

TEST_CASE("Each recipient receives its own epoch without changing reliability",
          "[Instance][Projection][CampaignFrame]")
{
  PartOne server;
  auto& actor = Player(server, 0xff000001, 0);
  Player(server, 0xff000002, 1);
  auto first = Advance(server, 0), second = Advance(server, 1);
  UpdateMovementMessage movement;
  movement.idx = actor.GetIdx();
  movement.data.worldOrCell = 0x3c;
  movement.data.pos = { 1, 2, 3 };
  movement.data.rot = { 0, 0, 0 };
  auto packet = Encode(Frame(first, movement));
  server.Messages().clear();
  PacketParser().TransformPacketIntoAction(0, packet.data(), packet.size(),
                                           server.GetActionListener());
  REQUIRE(server.Messages().size() == 2);
  for (const auto& sent : server.Messages()) {
    REQUIRE(sent.j["t"] == static_cast<uint8_t>(MsgType::CampaignFrame));
    REQUIRE(sent.j["epoch"] == (sent.userId == 0 ? first : second));
    REQUIRE(sent.j["message"]["t"] ==
            static_cast<uint8_t>(MsgType::UpdateMovement));
    REQUIRE_FALSE(sent.reliable);
  }
}

TEST_CASE("Unframed custom gameplay cannot bypass an enabled packet epoch",
          "[Instance][Projection][CampaignFrame]")
{
  PartOne server;
  DoConnect(server, 0);
  auto events = FakeListener::New();
  server.AddListener(events);
  auto epoch = Advance(server, 0);
  const auto send = [&](std::string type, bool framed) {
    CustomPacketMessage packet;
    packet.contentJsonDump =
      nlohmann::json({ { "customPacketType", type } }).dump();
    auto data = framed ? Encode(Frame(epoch, packet)) : Encode(packet);
    PacketParser().TransformPacketIntoAction(0, data.data(), data.size(),
                                             server.GetActionListener());
  };
  send("boyhouse.instance", false);
  REQUIRE(events->str().empty());
  send("loginWithSkympIo", false);
  REQUIRE_FALSE(events->str().empty());
  events->clear();
  send("boyhouse.projection", false);
  REQUIRE_FALSE(events->str().empty());
  events->clear();
  send("boyhouse.instance", true);
  REQUIRE_FALSE(events->str().empty());
}

TEST_CASE("Epoch rotation discards queued packets instead of retagging them",
          "[Instance][Projection][CampaignFrame]")
{
  PartOne server;
  auto& actor = Player(server, 0xff000001, 0);
  REQUIRE_THROWS(server.BeginPacketEpoch(0));
  auto epoch = Advance(server, 0);
  actor.SendInventoryUpdate();
  Advance(server, 0);
  server.Messages().clear();
  server.Tick();
  REQUIRE(server.Messages().empty());
  actor.SendInventoryUpdate();
  server.Tick();
  REQUIRE(server.Messages().size() == 1);
  REQUIRE(server.Messages()[0].j["epoch"] == server.GetPacketEpoch(0));
  server.Messages().clear();
  server.SendCustomPacket(
    0, R"({"customPacketType":"boyhouse.projection","kind":"pause"})");
  REQUIRE(server.Messages()[0].j["t"] ==
          static_cast<uint8_t>(MsgType::CustomPacket));
  DoDisconnect(server, 0);
  DoConnect(server, 0);
  REQUIRE(server.GetPacketEpoch(0).empty());
  REQUIRE(Advance(server, 0) != epoch);
}

namespace {
struct TickProbe
{
  bool ticking = false, destroyed = false, destroyedDuringTick = false;
};

class CallbackClient : public Networking::IClient
{
public:
  explicit CallbackClient(TickProbe& probe_)
    : probe(probe_)
  {
  }
  ~CallbackClient() override
  {
    probe.destroyed = true;
    probe.destroyedDuringTick = probe.ticking;
  }
  void Send(Networking::PacketData, size_t, bool) override {}
  bool IsConnected() const override { return true; }
  void Tick(OnPacket callback, void* state) override
  {
    auto& currentProbe = probe;
    currentProbe.ticking = true;
    callback(state, Networking::PacketType::ClientSideConnectionAccepted,
             nullptr, 0, "");
    static const uint8_t packet[] = { 134, '{', '}' };
    callback(state, Networking::PacketType::Message, packet, sizeof(packet),
             "");
    callback(state, Networking::PacketType::ClientSideDisconnect, nullptr, 0,
             "");
    currentProbe.ticking = false;
  }

private:
  TickProbe& probe;
};

bool LeaveRaw(const uint8_t*, size_t, std::string&)
{
  return false;
}
}

TEST_CASE(
  "Closing the native client in its callback retains its tick lifetime",
  "[Projection][Networking][CampaignFrame]")
{
  TickProbe probe;
  MpClientPlugin::State client;
  client.cl = std::make_shared<CallbackClient>(probe);
  struct Context
  {
    MpClientPlugin::State* client;
    int received = 0;
  } context{ &client };
  MpClientPlugin::Tick(
    client,
    [](int32_t, const char*, size_t, const char*, void* opaque) {
      auto& ctx = *static_cast<Context*>(opaque);
      ++ctx.received;
      MpClientPlugin::DestroyClient(*ctx.client);
    },
    LeaveRaw, &context);
  REQUIRE(context.received == 1);
  REQUIRE(probe.destroyed);
  REQUIRE_FALSE(probe.destroyedDuringTick);
}

TEST_CASE(
  "A replacement native connection cannot receive the old packet batch",
  "[Projection][Networking][CampaignFrame]")
{
  TickProbe original, replacement;
  MpClientPlugin::State client;
  client.cl = std::make_shared<CallbackClient>(original);
  struct Context
  {
    MpClientPlugin::State* client;
    TickProbe* replacement;
    int received = 0;
  } context{ &client, &replacement };
  const auto receive = [](int32_t, const char*, size_t, const char*,
                          void* opaque) {
    auto& ctx = *static_cast<Context*>(opaque);
    if (++ctx.received == 1)
      ctx.client->cl = std::make_shared<CallbackClient>(*ctx.replacement);
  };
  MpClientPlugin::Tick(client, receive, LeaveRaw, &context);
  REQUIRE(context.received == 1);
  REQUIRE(original.destroyed);
  REQUIRE_FALSE(original.destroyedDuringTick);
  REQUIRE_FALSE(replacement.destroyed);
  MpClientPlugin::Tick(client, receive, LeaveRaw, &context);
  REQUIRE(context.received == 4);
  MpClientPlugin::DestroyClient(client);
  REQUIRE(replacement.destroyed);
  REQUIRE_FALSE(replacement.destroyedDuringTick);
}
