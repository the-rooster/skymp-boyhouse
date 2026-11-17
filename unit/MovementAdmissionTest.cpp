#include "ActionListener.h"
#include "AnimationData.h"
#include "CampaignFrameMessage.h"
#include "MessageSerializerFactory.h"
#include "PacketParser.h"
#include "TestUtils.hpp"
#include "UpdateMovementMessage.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <slikenet/BitStream.h>

namespace {
std::vector<uint8_t> MovementPacket(const UpdateMovementMessage& message,
                                    bool json)
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

class MovementTrigger : public MpObjectReference
{
public:
  MovementTrigger()
    : MpObjectReference(
        LocationalData{ { 100, 100, 100 }, {}, FormDesc::Tamriel() },
        FormCallbacks::DoNothing(), 0, "ACTI", NiPoint3{ 10, 10, 10 })
  {
  }

  void SendPapyrusEvent(const char* name, const VarValue*, size_t) override
  {
    if (std::string(name) != "OnTrigger")
      return;
    ++inside;
    // A callback may teleport through this trigger again.
    auto callback = std::move(onInside);
    onInside = {};
    if (callback)
      callback();
  }

  std::function<void()> onInside;
  unsigned inside = 0;
};

struct MovementFixture
{
  static constexpr uint32_t ObserverId = 0xff008001;
  static constexpr uint32_t PlayerId = 0xff008002;
  static constexpr uint32_t NpcId = 0xff008003;
  PartOne server;
  PacketParser parser;
  MpActor *player, *observer;

  MovementFixture()
  {
    // Static cell IDs need a plugin identity even without loading an ESPM.
    server.worldState.espmFiles = { "Skyrim.esm" };
    // Exercise idx decoding with a nonzero player index on both wire formats.
    observer = &Player(ObserverId, 1);
    player = &Player(PlayerId, 0);
    Prime(*player);
    server.Messages().clear();
  }

  MpActor& Player(uint32_t id, Networking::UserId user)
  {
    DoConnect(server, user);
    server.CreateActor(id, { 1, 2, 3 }, 0, 0x3c);
    server.SetUserActor(user, id);
    return server.worldState.GetFormAt<MpActor>(id);
  }

  MpActor& HostedNpc()
  {
    server.CreateActor(NpcId, { 1, 2, 3 }, 0, 0x3c);
    auto& actor = server.worldState.GetFormAt<MpActor>(NpcId);
    server.worldState.hosters[NpcId] = player->GetFormId();
    Prime(actor);
    server.Messages().clear();
    return actor;
  }

  void Prime(MpActor& actor)
  {
    actor.IncreaseBlockCount();
    actor.IncreaseBlockCount();
    actor.SetAnimationVariableBool(AnimationVariableBool::kVariable_IsBlocking,
                                   true);
    actor.SetLastAnimEvent(AnimationData{ "sentinel", 7 });
    auto& times = server.worldState.lastMovUpdateByIdx;
    times.resize(std::max(times.size(), size_t(actor.GetIdx()) + 1));
    times[actor.GetIdx()] =
      std::chrono::system_clock::time_point(std::chrono::milliseconds(1234));
  }

  UpdateMovementMessage Packet(MpActor& actor)
  {
    UpdateMovementMessage message;
    message.idx = actor.GetIdx();
    message.data.worldOrCell = 0x3c;
    message.data.pos = { 100, 100, 100 };
    message.data.rot = { 10, 20, 30 };
    message.data.direction = 0.25f;
    message.data.healthPercentage = 0.75f;
    message.data.speed = 120;
    message.data.runMode = "Running";
    message.data.isInJumpState = true;
    message.data.isSneaking = true;
    message.data.isWeapDrawn = true;
    message.data.lookAt = std::array<float, 3>{ 5, 6, 7 };
    return message;
  }

  void Send(const UpdateMovementMessage& message, bool json = false,
            Networking::UserId user = 0)
  {
    const auto bytes = MovementPacket(message, json);
    parser.TransformPacketIntoAction(user, bytes.data(), bytes.size(),
                                     server.GetActionListener());
  }

  MovementTrigger& Trigger()
  {
    auto trigger = std::make_unique<MovementTrigger>();
    auto& result = *trigger;
    server.worldState.AddForm(std::move(trigger), 0xff008004);
    result.ForceSubscriptionsUpdate();
    server.Messages().clear();
    return result;
  }

  size_t Count(MsgType type,
               Networking::UserId user = Networking::InvalidUserId)
  {
    return std::count_if(
      server.Messages().begin(), server.Messages().end(),
      [&](const PartOne::Message& message) {
        auto value = message.j;
        if (const auto frame =
              dynamic_cast<CampaignFrameMessage*>(message.message.get())) {
          const auto decoded =
            PartOne::GetMessageSerializerInstance().Deserialize(
              frame->payload.data(), frame->payload.size());
          REQUIRE(decoded);
          decoded->message->WriteJson(value);
        }
        return value.at("t") == static_cast<int>(type) &&
          (user == Networking::InvalidUserId || message.userId == user);
      });
  }

  nlohmann::json Snapshot(MpActor& actor)
  {
    const auto& pos = actor.GetPos();
    const auto& angle = actor.GetAngle();
    return { { "pos", { pos.x, pos.y, pos.z } },
             { "angle", { angle.x, angle.y, angle.z } },
             { "cell", actor.GetCellOrWorld().ToString() },
             { "phase", actor.GetWorldInstance() },
             { "epoch", actor.GetWorldInstanceEpoch() },
             { "teleport", actor.GetTeleportFlag() },
             { "blockCount", actor.GetBlockCount() },
             { "blocking", actor.GetAnimationVariableBool("IsBlocking") },
             { "sneaking", actor.GetAnimationVariableBool("IsSneaking") },
             { "jumping", actor.GetAnimationVariableBool("bInJumpState") },
             { "weapon",
               actor.GetAnimationVariableBool("_skymp_isWeapDrawn") },
             { "animation", actor.GetLastAnimEventAsJson() } };
  }
};

class MovementSendTarget : public Networking::ISendTarget
{
public:
  void Send(Networking::UserId user, Networking::PacketData data,
            size_t length, bool) override
  {
    const auto decoded =
      PartOne::GetMessageSerializerInstance().Deserialize(data, length);
    if (!decoded ||
        !dynamic_cast<UpdateMovementMessage*>(decoded->message.get()))
      return;
    movementRecipients.push_back(user);
    nlohmann::json payload;
    decoded->message->WriteJson(payload);
    movementPayloads.push_back(std::move(payload));
    auto callback = std::move(onMovement);
    onMovement = {};
    if (callback)
      callback(user);
  }

  std::function<void(Networking::UserId)> onMovement;
  std::vector<Networking::UserId> movementRecipients;
  std::vector<nlohmann::json> movementPayloads;
};
}

TEST_CASE(
  "Authorized movement commits before forwarding JSON and binary packets",
  "[MovementAdmission]")
{
  const bool json = GENERATE(false, true);
  const bool hosted = GENERATE(false, true);
  MovementFixture f;
  auto& actor = hosted ? f.HostedNpc() : *f.player;
  auto& trigger = f.Trigger();
  const auto previousTime =
    f.server.worldState.lastMovUpdateByIdx[actor.GetIdx()];
  const auto packet = f.Packet(actor);
  f.Send(packet, json);
  REQUIRE(trigger.inside == 1);
  REQUIRE(actor.GetPos() == NiPoint3{ 100, 100, 100 });
  REQUIRE(actor.GetAngle() == NiPoint3{ 10, 20, 30 });
  REQUIRE(actor.GetBlockCount() == 3);
  REQUIRE_FALSE(actor.GetAnimationVariableBool("IsBlocking"));
  REQUIRE(actor.GetAnimationVariableBool("IsSneaking"));
  REQUIRE(actor.GetAnimationVariableBool("bInJumpState"));
  REQUIRE(actor.GetAnimationVariableBool("_skymp_isWeapDrawn"));
  REQUIRE_FALSE(actor.GetLastAnimEvent());
  REQUIRE(f.server.worldState.lastMovUpdateByIdx[actor.GetIdx()] !=
          previousTime);
  REQUIRE(f.Count(MsgType::UpdateMovement, 0) == 1);
  REQUIRE(f.Count(MsgType::UpdateMovement, 1) == 1);
  REQUIRE(f.Count(MsgType::Teleport2) == 0);
  nlohmann::json expected;
  packet.WriteJson(expected);
  for (const auto& sent : f.server.Messages()) {
    if (sent.j.at("t") == static_cast<int>(MsgType::UpdateMovement)) {
      REQUIRE(sent.j == expected);
      REQUIRE_FALSE(sent.reliable);
    }
  }
}

TEST_CASE(
  "Nonfinite movement is rejected before flags authority or trigger effects",
  "[MovementAdmission]")
{
  // Binary floats preserve NaN and infinities; JSON represents neither.
  const unsigned field =
    GENERATE(0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u);
  const float invalid = GENERATE(std::numeric_limits<float>::quiet_NaN(),
                                 std::numeric_limits<float>::infinity(),
                                 -std::numeric_limits<float>::infinity());
  CAPTURE(field, invalid);
  MovementFixture f;
  auto& trigger = f.Trigger();
  f.player->SetTeleportFlag(true);
  auto packet = f.Packet(*f.player);
  if (field < 3)
    packet.data.pos[field] = invalid;
  else if (field < 6)
    packet.data.rot[field - 3] = invalid;
  else if (field < 9)
    (*packet.data.lookAt)[field - 6] = invalid;
  else if (field == 9)
    packet.data.direction = invalid;
  else if (field == 10)
    packet.data.healthPercentage = invalid;
  else
    packet.data.speed = invalid;
  const auto before = f.Snapshot(*f.player);
  const auto times = f.server.worldState.lastMovUpdateByIdx;
  f.Send(packet);
  REQUIRE(f.Snapshot(*f.player) == before);
  REQUIRE(f.server.worldState.lastMovUpdateByIdx == times);
  REQUIRE(trigger.inside == 0);
  REQUIRE(f.server.Messages().empty());

  // Even a forged lease on another player is untouched by a malformed packet.
  f.server.worldState.hosters[f.observer->GetFormId()] = f.player->GetFormId();
  packet.idx = f.observer->GetIdx();
  const auto otherBefore = f.Snapshot(*f.observer);
  const auto leases = f.server.worldState.hosters;
  f.Send(packet);
  REQUIRE(f.Snapshot(*f.observer) == otherBefore);
  REQUIRE(f.server.worldState.hosters == leases);
  REQUIRE(f.server.Messages().empty());
}

TEST_CASE("Illegal finite movement corrects only the owning player",
          "[MovementAdmission]")
{
  const bool json = GENERATE(false, true);
  const bool hosted = GENERATE(false, true);
  const bool wrongCell = GENERATE(false, true);
  MovementFixture f;
  auto& actor = hosted ? f.HostedNpc() : *f.player;
  auto& trigger = f.Trigger();
  auto packet = f.Packet(actor);
  if (wrongCell)
    packet.data.worldOrCell = 0x12345;
  else
    packet.data.pos = { 4097, 2,
                        3 }; // Exactly the disallowed 4096-unit boundary.
  const auto before = f.Snapshot(actor);
  const auto times = f.server.worldState.lastMovUpdateByIdx;
  f.Send(packet, json);
  REQUIRE(f.Snapshot(actor) == before);
  REQUIRE(f.server.worldState.lastMovUpdateByIdx == times);
  REQUIRE(trigger.inside == 0);
  REQUIRE(f.Count(MsgType::UpdateMovement) == 0);
  REQUIRE(f.server.Messages().size() == (hosted ? 0 : 1));
  if (!hosted) {
    const auto& correction = f.server.Messages().front();
    REQUIRE(correction.userId == 0);
    REQUIRE(correction.reliable);
    REQUIRE(correction.j ==
            nlohmann::json{ { "t", static_cast<int>(MsgType::Teleport2) },
                            { "pos", { 1, 2, 3 } },
                            { "rot", { 0, 0, 0 } },
                            { "worldOrCell", 0x3c } });
  }
}

TEST_CASE("Teleport rejection consumes one finite authorized movement packet",
          "[MovementAdmission]")
{
  const bool json = GENERATE(false, true);
  MovementFixture f;
  f.player->SetTeleportFlag(true);
  const auto packet = f.Packet(*f.player);
  auto malformed = packet;
  malformed.data.speed = std::numeric_limits<float>::quiet_NaN();
  f.Send(malformed);
  REQUIRE(f.player->GetTeleportFlag());
  REQUIRE(f.server.Messages().empty());

  const auto times = f.server.worldState.lastMovUpdateByIdx;
  f.Send(packet, json);
  REQUIRE_FALSE(f.player->GetTeleportFlag());
  REQUIRE(f.player->GetPos() == NiPoint3{ 1, 2, 3 });
  REQUIRE(f.player->GetBlockCount() == 2);
  REQUIRE(f.server.worldState.lastMovUpdateByIdx == times);
  REQUIRE(f.Count(MsgType::Teleport2, 0) == 1);
  REQUIRE(f.Count(MsgType::UpdateMovement) == 0);
  f.server.Messages().clear();
  f.Send(packet, json);
  REQUIRE(f.player->GetPos() == NiPoint3{ 100, 100, 100 });
  REQUIRE(f.Count(MsgType::Teleport2) == 0);
  REQUIRE(f.Count(MsgType::UpdateMovement) == 2);
}

TEST_CASE(
  "Movement requires player ownership or the current NPC host in phase",
  "[MovementAdmission]")
{
  const bool json = GENERATE(false, true);
  const std::string denial =
    GENERATE("player", "missing-host", "wrong-host", "phase");
  MovementFixture f;
  auto& actor = denial == "player" ? *f.observer : f.HostedNpc();
  if (denial == "player")
    f.server.worldState.hosters[actor.GetFormId()] = f.player->GetFormId();
  else if (denial == "missing-host")
    f.server.worldState.hosters.erase(actor.GetFormId());
  else if (denial == "wrong-host")
    f.server.worldState.hosters[actor.GetFormId()] = f.observer->GetFormId();
  else
    actor.SetWorldInstance("unrelated-phase");
  actor.SetTeleportFlag(true);
  f.server.Messages().clear();
  const auto before = f.Snapshot(actor);
  const auto times = f.server.worldState.lastMovUpdateByIdx;
  f.Send(f.Packet(actor), json);
  REQUIRE(f.Snapshot(actor) == before);
  REQUIRE(f.server.worldState.lastMovUpdateByIdx == times);
  REQUIRE(f.Count(MsgType::UpdateMovement) == 0);
  REQUIRE(f.Count(MsgType::Teleport2) == 0);
  REQUIRE(f.Count(MsgType::HostStop, 0) == 1);
  if (denial == "player")
    REQUIRE(f.server.worldState.hosters.count(actor.GetFormId()) == 0);
}

TEST_CASE("Reentrant movement callbacks invalidate the unfinished packet",
          "[MovementAdmission]")
{
  const std::string change =
    GENERATE("remove", "replace", "teleport", "rotate", "phase-roundtrip",
             "disconnect", "reconnect", "packet-epoch", "rehost",
             "requester-phase", "requester-replace");
  CAPTURE(change);
  MovementFixture f;
  const bool hosted = change == "rehost" || change == "requester-phase" ||
    change == "requester-replace";
  auto& actor = hosted ? f.HostedNpc() : *f.player;
  const auto lifetime =
    f.server.worldState.LookupFormByIdNoLoad(actor.GetFormId());
  auto& trigger = f.Trigger();
  const auto packet = f.Packet(actor);
  nlohmann::json afterCallback;
  std::shared_ptr<MpForm> replacement;
  nlohmann::json replacementSnapshot;
  const auto times = f.server.worldState.lastMovUpdateByIdx;
  unsigned callbacks = 0;
  trigger.onInside = [&] {
    ++callbacks;
    REQUIRE(actor.GetBlockCount() == 2);
    REQUIRE(actor.GetAngle() == NiPoint3{ 0, 0, 0 });
    REQUIRE(actor.GetLastAnimEvent());
    if (change == "remove" || change == "replace") {
      const auto id = actor.GetFormId();
      f.server.DestroyActor(id);
      if (change == "replace") {
        f.server.CreateActor(id, { 700, 800, 900 }, 45, 0x3c);
        f.server.SetUserActor(0, id);
        replacement = f.server.worldState.LookupFormByIdNoLoad(id);
        replacementSnapshot = f.Snapshot(*replacement->AsActor());
      }
    } else if (change == "teleport") {
      actor.Teleport(LocationalData{
        { 700, 800, 900 }, { 40, 50, 60 }, FormDesc::Tamriel() });
    } else if (change == "rotate") {
      actor.SetAngle({ 40, 50, 60 });
    } else if (change == "phase-roundtrip") {
      actor.SetWorldInstance("temporary-phase");
      actor.SetWorldInstance("");
    } else if (change == "disconnect" || change == "reconnect") {
      DoDisconnect(f.server, 0);
      if (change == "reconnect") {
        DoConnect(f.server, 0);
        f.server.SetUserActor(0, actor.GetFormId());
      }
    } else if (change == "packet-epoch") {
      f.server.serverState.userInfo[0]->gameplayBlocked = true;
      f.server.BeginPacketEpoch(0);
    } else if (change == "rehost") {
      f.server.worldState.hosters[actor.GetFormId()] = f.observer->GetFormId();
    } else if (change == "requester-phase") {
      f.player->SetWorldInstance("temporary-phase");
      f.player->SetWorldInstance("");
    } else {
      f.server.DestroyActor(MovementFixture::PlayerId);
      f.server.CreateActor(MovementFixture::PlayerId, { 1, 2, 3 }, 0, 0x3c);
      f.server.SetUserActor(0, MovementFixture::PlayerId);
    }
    afterCallback = f.Snapshot(actor);
  };
  f.Send(packet);
  REQUIRE(callbacks == 1);
  REQUIRE(f.Snapshot(actor) == afterCallback);
  REQUIRE(f.server.worldState.lastMovUpdateByIdx == times);
  REQUIRE(f.Count(MsgType::UpdateMovement) == 0);
  if (replacement)
    REQUIRE(f.Snapshot(*replacement->AsActor()) == replacementSnapshot);
}

TEST_CASE(
  "Movement publication rechecks sender and later recipient connections",
  "[MovementAdmission]")
{
  const std::string change =
    GENERATE("source-phase", "source-replace", "later-recipient");
  MovementSendTarget target;
  MovementFixture f;
  f.Player(0xff008005, 2);
  f.Player(0xff008006, 3);
  auto packet = f.Packet(*f.player);
  const auto recipients = f.player->GetActorListeners();
  REQUIRE(recipients.size() == 4);
  const auto previousTime =
    f.server.worldState.lastMovUpdateByIdx[f.player->GetIdx()];
  Networking::UserId replacedUser = Networking::InvalidUserId;
  target.onMovement = [&](Networking::UserId firstUser) {
    // The first recipient must already observe the entire accepted update.
    REQUIRE(f.player->GetPos() == NiPoint3{ 100, 100, 100 });
    REQUIRE(f.player->GetAngle() == NiPoint3{ 10, 20, 30 });
    REQUIRE(f.player->GetBlockCount() == 3);
    REQUIRE_FALSE(f.player->GetLastAnimEvent());
    REQUIRE(f.server.worldState.lastMovUpdateByIdx[f.player->GetIdx()] !=
            previousTime);
    if (change == "source-phase") {
      f.player->SetWorldInstance("after-first-send");
      return;
    }
    if (change == "source-replace") {
      f.server.DestroyActor(MovementFixture::PlayerId);
      f.server.CreateActor(MovementFixture::PlayerId, { 700, 800, 900 }, 45,
                           0x3c);
      f.server.SetUserActor(0, MovementFixture::PlayerId);
      return;
    }
    // Select a peer not yet visited, independently of subscription ordering.
    for (auto* recipient : recipients) {
      const auto user = f.server.serverState.UserByActor(recipient);
      if (user == firstUser || user == 0)
        continue;
      replacedUser = user;
      const auto id = recipient->GetFormId();
      DoDisconnect(f.server, user);
      DoConnect(f.server, user);
      f.server.SetUserActor(user, id);
      break;
    }
  };
  f.server.SetSendTarget(&target);
  f.Send(packet);
  if (change != "later-recipient") {
    REQUIRE(target.movementRecipients.size() == 1);
  } else {
    REQUIRE(replacedUser != Networking::InvalidUserId);
    REQUIRE(target.movementRecipients.size() == 3);
    REQUIRE(std::find(target.movementRecipients.begin(),
                      target.movementRecipients.end(),
                      replacedUser) == target.movementRecipients.end());
  }
  f.server.SetSendTarget(nullptr);
}

TEST_CASE("Newer nested movement supersedes the outer packet even at the same "
          "transform",
          "[MovementAdmission]")
{
  const std::string boundary = GENERATE("trigger", "publication");
  const bool validNested = GENERATE(false, true);
  CAPTURE(boundary, validNested);
  MovementSendTarget target;
  MovementFixture f;
  auto outer = f.Packet(*f.player);
  auto newer = outer;
  // Inside SetPos the rotation is still the pre-packet rotation. During
  // publication the full outer transform is already committed.
  if (boundary == "trigger")
    newer.data.rot = { 0, 0, 0 };
  newer.data.isBlocking = true;
  newer.data.isSneaking = false;
  newer.data.isInJumpState = false;
  newer.data.isWeapDrawn = false;
  newer.data.runMode = "Standing";
  newer.data.healthPercentage = 0.5f;
  if (!validNested)
    newer.data.speed = std::numeric_limits<float>::infinity();

  unsigned nestedCalls = 0;
  nlohmann::json afterNested;
  auto timesAfterNested = f.server.worldState.lastMovUpdateByIdx;
  const auto sendNewer = [&] {
    ++nestedCalls;
    f.Send(newer);
    afterNested = f.Snapshot(*f.player);
    timesAfterNested = f.server.worldState.lastMovUpdateByIdx;
  };
  if (boundary == "trigger")
    f.Trigger().onInside = sendNewer;
  else
    target.onMovement = [&](Networking::UserId) { sendNewer(); };
  f.server.SetSendTarget(&target);
  f.Send(outer);
  REQUIRE(nestedCalls == 1);

  nlohmann::json expected;
  if (validNested) {
    newer.WriteJson(expected);
    REQUIRE(f.Snapshot(*f.player) == afterNested);
    REQUIRE(f.server.worldState.lastMovUpdateByIdx == timesAfterNested);
    REQUIRE(f.player->GetBlockCount() == 0);
    REQUIRE(f.player->GetAnimationVariableBool("IsBlocking"));
    REQUIRE_FALSE(f.player->GetAnimationVariableBool("IsSneaking"));
    REQUIRE_FALSE(f.player->GetAnimationVariableBool("bInJumpState"));
    REQUIRE_FALSE(f.player->GetAnimationVariableBool("_skymp_isWeapDrawn"));
    REQUIRE(target.movementPayloads.size() == (boundary == "trigger" ? 2 : 3));
    if (boundary == "publication") {
      nlohmann::json original;
      outer.WriteJson(original);
      REQUIRE(target.movementPayloads.front() == original);
    }
  } else {
    // A rejected nested packet cannot cancel or partially replace its caller.
    outer.WriteJson(expected);
    REQUIRE(f.player->GetAngle() == NiPoint3{ 10, 20, 30 });
    REQUIRE(f.player->GetBlockCount() == 3);
    REQUIRE_FALSE(f.player->GetAnimationVariableBool("IsBlocking"));
    REQUIRE(f.player->GetAnimationVariableBool("IsSneaking"));
    REQUIRE(target.movementPayloads.size() == 2);
  }
  for (Networking::UserId user : { 0, 1 }) {
    nlohmann::json last;
    size_t count = 0;
    for (size_t i = 0; i < target.movementRecipients.size(); ++i) {
      if (target.movementRecipients[i] == user) {
        last = target.movementPayloads[i];
        ++count;
      }
    }
    REQUIRE(count >= 1);
    REQUIRE(last == expected);
  }
  f.server.SetSendTarget(nullptr);
}
