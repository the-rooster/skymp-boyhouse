#include "ActionListener.h"
#include "CampaignFrameMessage.h"
#include "CustomPacketMessage.h"
#include "MessageSerializerFactory.h"
#include "PacketParser.h"
#include "TestUtils.hpp"
#include "UpdateAppearanceMessage.h"
#include "UpdateMovementMessage.h"
#include <limits>
#include <slikenet/BitStream.h>

namespace {
std::vector<uint8_t> EncodeCreationPacket(const IMessageBase& message,
                                         bool json = false)
{
  if (json) {
    nlohmann::json value;
    message.WriteJson(value);
    const auto text = MakeMessage(value);
    return { text.begin(), text.end() };
  }
  SLNet::BitStream stream;
  MessageSerializerFactory::CreateMessageSerializer()->Serialize(message,
                                                                 stream);
  return { stream.GetData(),
           stream.GetData() + stream.GetNumberOfBytesUsed() };
}

struct CreationFixture
{
  PartOne server;
  PacketParser parser;
  std::shared_ptr<FakeListener> events = FakeListener::New();
  MpActor *actor, *observer;
  UpdateAppearanceMessage appearance;

  CreationFixture()
  {
    // Allocate another actor first: the creating player's idx is not zero.
    observer = &Player(0xff006001, 1);
    actor = &Player(0xff006002, 0);
    appearance.idx = actor->GetIdx();
    appearance.data = Appearance::FromJson(jAppearance.at("data"));
    actor->SetRaceMenuOpen(true);
    server.AddListener(events);
    server.Messages().clear();
  }

  MpActor& Player(uint32_t id, Networking::UserId user)
  {
    DoConnect(server, user);
    server.CreateActor(id, { 1, 2, 3 }, 0, 0x3c);
    server.SetUserActor(user, id);
    return server.worldState.GetFormAt<MpActor>(id);
  }

  std::string Epoch(Networking::UserId user = 0, bool release = true)
  {
    server.serverState.userInfo[user]->gameplayBlocked = true;
    auto epoch = server.BeginPacketEpoch(user);
    server.serverState.userInfo[user]->gameplayBlocked = !release;
    return epoch;
  }

  void SendBytes(std::vector<uint8_t> bytes, const std::string& epoch = "",
                  Networking::UserId user = 0)
  {
    if (!epoch.empty()) {
      CampaignFrameMessage frame;
      frame.epoch = epoch;
      frame.payload = std::move(bytes);
      bytes = EncodeCreationPacket(frame);
    }
    parser.TransformPacketIntoAction(user, bytes.data(), bytes.size(),
                                     server.GetActionListener());
  }

  void Send(const IMessageBase& message, bool json = false,
             const std::string& epoch = "", Networking::UserId user = 0)
  {
    SendBytes(EncodeCreationPacket(message, json), epoch, user);
  }

  void Custom(const std::string& content, bool json = false,
               const std::string& epoch = "")
  {
    CustomPacketMessage message;
    message.contentJsonDump = content;
    Send(message, json, epoch);
  }

  UpdateMovementMessage Movement() const
  {
    UpdateMovementMessage message;
    message.idx = actor->GetIdx();
    message.data.worldOrCell = 0x3c;
    message.data.pos = { 8, 2, 3 };
    message.data.rot = { 0, 0, 0 };
    message.data.healthPercentage = 1;
    return message;
  }
};
}

TEST_CASE("Character creation blocks binary and JSON gameplay",
          "[Instance][Projection][CharacterCreation]")
{
  CreationFixture f;
  const bool json = GENERATE(false, true);
  const bool framed = GENERATE(false, true);
  const auto epoch = framed ? f.Epoch() : std::string{};
  auto& listener = f.server.GetActionListener();
  for (int type = 0; type < 128; ++type) {
    const auto message = static_cast<MsgType>(type);
    REQUIRE(listener.IsMessageAllowed(0, message, framed) ==
            (message == MsgType::CustomPacket ||
             message == MsgType::UpdateAppearance));
  }
  REQUIRE(listener.IsMessageAllowed(1, MsgType::UpdateMovement));
  const auto before = f.actor->GetPos();
  f.Send(f.Movement(), json, epoch);
  REQUIRE(f.actor->GetPos() == before);
  REQUIRE(f.server.Messages().empty());
  REQUIRE(f.server.IsConnected(0));
  REQUIRE(f.server.GetUserActor(0) == f.actor->GetFormId());
  REQUIRE(f.actor->IsRaceMenuOpen());
}

TEST_CASE("Creation appearance requires released projection and current epoch",
          "[Instance][Projection][CharacterCreation]")
{
  CreationFixture f;
  const bool json = GENERATE(false, true);
  const auto stale = f.Epoch();
  const auto current = f.Epoch(0, false);
  REQUIRE(stale != current);
  f.Send(f.appearance, json, current);
  REQUIRE(f.actor->IsRaceMenuOpen());
  REQUIRE_FALSE(f.actor->GetAppearance());
  REQUIRE(f.server.Messages().empty());

  f.server.serverState.userInfo[0]->gameplayBlocked = false;
  f.Send(f.appearance, json, stale);
  f.Send(f.appearance, json); // Unframed submission cannot bypass the epoch.
  REQUIRE(f.actor->IsRaceMenuOpen());
  REQUIRE_FALSE(f.actor->GetAppearance());
  REQUIRE(f.server.Messages().empty());

  f.Send(f.appearance, json, current);
  REQUIRE_FALSE(f.actor->IsRaceMenuOpen());
  REQUIRE(f.actor->GetAppearance());
  REQUIRE(*f.actor->GetAppearance() == *f.appearance.data);
}

TEST_CASE("Foreign or absent appearance data cannot finish character creation",
          "[Instance][Projection][CharacterCreation]")
{
  CreationFixture f;
  const bool json = GENERATE(false, true);
  const auto epoch = f.Epoch();
  auto foreign = f.appearance;
  foreign.idx = f.observer->GetIdx();
  f.Send(foreign, json, epoch);
  REQUIRE(f.actor->IsRaceMenuOpen());
  REQUIRE_FALSE(f.actor->GetAppearance());
  REQUIRE_FALSE(f.observer->GetAppearance());
  REQUIRE(f.server.Messages().empty());

  auto absent = f.appearance;
  absent.data.reset();
  f.Send(absent, json, epoch);
  REQUIRE(f.actor->IsRaceMenuOpen());
  REQUIRE_FALSE(f.actor->GetAppearance());
  REQUIRE(f.server.Messages().empty());

  f.Send(f.appearance, json, epoch);
  REQUIRE_FALSE(f.actor->IsRaceMenuOpen());
  REQUIRE(*f.actor->GetAppearance() == *f.appearance.data);
}

TEST_CASE("Malformed appearance packets leave the creation gate closed",
          "[Instance][Projection][CharacterCreation]")
{
  CreationFixture f;
  const auto epoch = f.Epoch();
  nlohmann::json malformed;
  f.appearance.WriteJson(malformed);
  SECTION("Invalid actor index type") { malformed["idx"] = "foreign"; }
  SECTION("Invalid appearance object") { malformed["data"] = 42; }
  SECTION("Invalid appearance field") { malformed["data"]["name"] = false; }
  const auto text = MakeMessage(malformed);
  REQUIRE_THROWS(f.SendBytes({ text.begin(), text.end() }, epoch));
  REQUIRE(f.actor->IsRaceMenuOpen());
  REQUIRE_FALSE(f.actor->GetAppearance());
  REQUIRE(f.server.Messages().empty());
  REQUIRE(f.server.IsConnected(0));
  f.Send(f.appearance, false, epoch);
  REQUIRE_FALSE(f.actor->IsRaceMenuOpen());
}

TEST_CASE("Truncated binary appearance cannot finish character creation",
          "[Instance][Projection][CharacterCreation]")
{
  CreationFixture f;
  const auto epoch = f.Epoch();
  const auto packet = EncodeCreationPacket(f.appearance);
  for (size_t length = 2; length < packet.size(); ++length) {
    CAPTURE(length);
    REQUIRE_THROWS(
      f.SendBytes({ packet.begin(), packet.begin() + length }, epoch));
    REQUIRE(f.actor->IsRaceMenuOpen());
    REQUIRE_FALSE(f.actor->GetAppearance());
    REQUIRE(f.server.Messages().empty());
  }
  f.Send(f.appearance, false, epoch);
  REQUIRE_FALSE(f.actor->IsRaceMenuOpen());
}

TEST_CASE("Nonfinite binary appearance cannot finish character creation",
          "[Instance][Projection][CharacterCreation]")
{
  CreationFixture f;
  const auto epoch = f.Epoch();
  auto invalid = f.appearance;
  SECTION("Nonfinite weight")
  {
    invalid.data->weight = std::numeric_limits<float>::quiet_NaN();
  }
  SECTION("Nonfinite face morph")
  {
    invalid.data->options[0] = std::numeric_limits<float>::infinity();
  }
  SECTION("Nonfinite face preset")
  {
    invalid.data->presets[0] = -std::numeric_limits<float>::infinity();
  }
  f.Send(invalid, false, epoch);
  REQUIRE(f.actor->IsRaceMenuOpen());
  REQUIRE_FALSE(f.actor->GetAppearance());
  REQUIRE(f.server.Messages().empty());
  f.Send(f.appearance, false, epoch);
  REQUIRE_FALSE(f.actor->IsRaceMenuOpen());
}

TEST_CASE(
  "Creation permits only projection custom packets for assigned actors",
  "[Instance][Projection][CharacterCreation]")
{
  CreationFixture f;
  const bool json = GENERATE(false, true);
  const bool framed = GENERATE(false, true);
  const auto current = f.Epoch();
  const auto epoch = framed ? current : std::string{};
  for (const auto* type :
       { "boyhouse.instance", "boyhouse.questJournal", "loginWithSkympIo",
         "arbitrary-game-action", "" }) {
    f.Custom(nlohmann::json({ { "customPacketType", type } }).dump(), json,
             epoch);
  }
  for (const auto* content : { "{}", "null", "[]",
                               "{\"customPacketType\":42}" })
    f.Custom(content, json, epoch);
  REQUIRE(f.events->str().empty());
  f.Custom(R"({"customPacketType":"boyhouse.projection","kind":"ready"})",
           json, epoch);
  REQUIRE_FALSE(f.events->str().empty());

  // A fresh projection barrier needs its unframed handshake even while the
  // same actor is still creating a character. Framed traffic stays blocked.
  f.events->clear();
  f.server.serverState.userInfo[0]->gameplayBlocked = true;
  f.Custom(R"({"customPacketType":"boyhouse.projection","kind":"ready"})",
           json, current);
  REQUIRE(f.events->str().empty());
  f.Custom(R"({"customPacketType":"boyhouse.projection","kind":"ready"})",
           json);
  REQUIRE_FALSE(f.events->str().empty());
  REQUIRE(f.actor->IsRaceMenuOpen());
}

TEST_CASE(
  "Accepted creation reliably echoes the exact appearance to its owner",
  "[Instance][Projection][CharacterCreation]")
{
  CreationFixture f;
  const bool json = GENERATE(false, true);
  const bool framed = GENERATE(false, true);
  const auto ownerEpoch = framed ? f.Epoch() : std::string{};
  const auto observerEpoch = framed ? f.Epoch(1) : std::string{};
  f.Send(f.appearance, json, ownerEpoch);
  REQUIRE_FALSE(f.actor->IsRaceMenuOpen());
  REQUIRE(f.actor->GetAppearance());
  REQUIRE(*f.actor->GetAppearance() == *f.appearance.data);
  REQUIRE(f.server.Messages().size() == 2);
  bool echoedOwner = false;
  for (const auto& sent : f.server.Messages()) {
    REQUIRE(sent.reliable);
    REQUIRE((sent.userId == 0 || sent.userId == 1));
    const IMessageBase* message = sent.message.get();
    std::optional<DeserializeResult> decoded;
    if (framed) {
      const auto* frame = dynamic_cast<const CampaignFrameMessage*>(message);
      REQUIRE(frame);
      REQUIRE(frame->epoch == (sent.userId == 0 ? ownerEpoch : observerEpoch));
      decoded =
        MessageSerializerFactory::CreateMessageSerializer()->Deserialize(
          frame->payload.data(), frame->payload.size());
      REQUIRE(decoded);
      REQUIRE(decoded->msgType == MsgType::UpdateAppearance);
      // Exactly one frame, not a nested frame.
      message = decoded->message.get();
    }
    const auto* appearance =
      dynamic_cast<const UpdateAppearanceMessage*>(message);
    REQUIRE(appearance);
    REQUIRE(appearance->idx == f.actor->GetIdx());
    REQUIRE(appearance->data);
    REQUIRE(*appearance->data == *f.appearance.data);
    echoedOwner |= sent.userId == 0;
  }
  REQUIRE(echoedOwner);
  f.server.Messages().clear();
  auto late = f.appearance;
  late.data->name = "Unaccepted late edit";
  f.Send(late, json, ownerEpoch);
  REQUIRE(*f.actor->GetAppearance() == *f.appearance.data);
  REQUIRE(f.server.Messages().empty());

  f.Send(f.Movement(), json, ownerEpoch);
  REQUIRE_FALSE(f.server.Messages().empty());
  f.events->clear();
  f.Custom(R"({"customPacketType":"boyhouse.questJournal"})", json,
           ownerEpoch);
  REQUIRE_FALSE(f.events->str().empty());
}

TEST_CASE(
  "A new baseline confirms accepted appearance after an epoch rotation",
  "[Instance][Projection][CharacterCreation]")
{
  CreationFixture f;
  const auto first = f.Epoch();
  f.Send(f.appearance, false, first);
  REQUIRE_FALSE(f.actor->IsRaceMenuOpen());
  const auto current = f.Epoch(0, false);
  const auto baseline = f.server.GetWorldBaseline(0, current);
  const auto& self = baseline.at("messages").at(1);
  REQUIRE(baseline.at("packetEpoch") == current);
  REQUIRE(self.at("isMe") == true);
  REQUIRE(self.at("idx") == f.actor->GetIdx());
  REQUIRE(self.at("props").at("isRaceMenuOpen") == false);
  REQUIRE(self.at("appearance") ==
          nlohmann::json::parse(f.appearance.data->ToJson()));
  f.server.serverState.userInfo[0]->gameplayBlocked = false;
  f.server.Messages().clear();
  f.Send(f.Movement(), false, first);
  REQUIRE(f.server.Messages().empty());
  f.Send(f.Movement(), false, current);
  REQUIRE_FALSE(f.server.Messages().empty());
}
