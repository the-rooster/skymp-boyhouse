#pragma once
#include "ActorsMap.h"
#include "ClientEffectContext.h"
#include "Config.h"
#include <Networking.h>
#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <simdjson.h>
#include <unordered_map>

class MpActor;

struct PacketHistoryElement
{
  size_t offset = 0;
  size_t length = 0;
  uint64_t timeMs = 0;
};

struct PacketHistory
{
  std::vector<uint8_t> buffer;
  std::deque<PacketHistoryElement> packets;
};

struct Playback
{
  PacketHistory history;
  std::chrono::time_point<std::chrono::steady_clock> startTime;
};

struct DeferredMessage
{
  std::vector<uint8_t> packetData;
  bool packetReliable = false;
  uint32_t actorIdExpected = 0;
  std::string packetEpochExpected;
  std::weak_ptr<MpForm> actorLifetimeExpected;
  // Inventory snapshots are personal to the actor; executable snippets also
  // require the original campaign context.
  std::optional<ClientEffectContext> effectContext;
};

struct UserInfo
{
  // A held token identifies this connection even when its slot, GUID, and
  // allocator address are reused by a reconnect.
  std::shared_ptr<const uint8_t> connectionIdentity =
    std::make_shared<const uint8_t>(0);
  bool isDisconnecting = false;
  // Connection-local handshake gate. Never persisted or inherited by a reused
  // user slot. Trusted server code releases it after projection
  // acknowledgement.
  bool gameplayBlocked = false;
  // Projection failure is terminal for this connection; a delayed ack must
  // not reopen its gameplay gate while transport disconnection is pending.
  bool campaignVisibilityFailed = false;
  // Empty retains the legacy transport. Once enabled, gameplay requires this
  // connection's current epoch even after its application gate opens.
  std::string packetEpoch;

  bool isPacketHistoryRecording = false;
  PacketHistory packetHistory;
  std::optional<std::chrono::time_point<std::chrono::steady_clock>>
    packetHistoryStartTime;

  std::vector<std::vector<DeferredMessage>> deferredChannels;

  std::string guid;
};

class ServerState
{
public:
  ServerState() { userInfo.resize(kMaxPlayers); }

  std::vector<std::unique_ptr<UserInfo>> userInfo;
  Networking::UserId maxConnectedId = 0;
  ActorsMap actorsMap;
  Networking::UserId disconnectingUserId = Networking::InvalidUserId;

  std::map<Networking::UserId, Playback>
    activePlaybacks; // do not modify directly, use requestedPlaybacks
  std::map<Networking::UserId, Playback> requestedPlaybacks;

  void Connect(Networking::UserId userId, const std::string& guid);
  void Disconnect(Networking::UserId userId) noexcept;
  bool IsConnected(Networking::UserId userId) const;
  MpActor* ActorByUser(Networking::UserId userId);
  const std::string& UserGuid(Networking::UserId userId);
  Networking::UserId UserByActor(MpActor* actor);
  void EnsureUserExists(Networking::UserId userId);
};
