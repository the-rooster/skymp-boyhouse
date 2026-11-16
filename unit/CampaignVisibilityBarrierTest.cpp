#include "CampaignVisibilityBarrier.h"
#include "ActionListener.h"
#include "TestUtils.hpp"
#include <functional>

extern espm::Loader& GetEspmLoader();

namespace {
struct BarrierWorld
{
  PartOne server;
  MpActor *owner, *guest, *outsider;
  CampaignVisibilityBarrier::UserIds prepared, finished, closed;
  std::function<void()> beforeEffect, afterEffect;
  bool beforeReceipt = true, afterReceipt = true, finishSynchronized = false;
  int beforeCount = 0, afterCount = 0;
  std::unique_ptr<CampaignVisibilityBarrier> barrier;

  BarrierWorld()
  {
    server.AttachEspm(&GetEspmLoader());
    for (Networking::UserId user = 0; user < 3; ++user) {
      DoConnect(server, user);
      server.CreateActor(0xff004000 + user, { 1, 2, 3 }, 0, 0x3c);
      server.SetUserActor(user, 0xff004000 + user);
      server.serverState.userInfo[user]->gameplayBlocked = true;
      server.BeginPacketEpoch(user);
      server.serverState.userInfo[user]->gameplayBlocked = false;
    }
    DoConnect(server, 3); // Unassigned legacy connections are not observers.
    owner = server.serverState.ActorByUser(0);
    guest = server.serverState.ActorByUser(1);
    outsider = server.serverState.ActorByUser(2);
    const auto instance =
      "campaign:" + std::to_string(owner->GetFormId()) + ":barrier";
    owner->SetWorldInstance(instance);
    guest->SetWorldInstance(instance);
    barrier = std::make_unique<CampaignVisibilityBarrier>(
      server,
      [&](uint32_t id, const auto& users) {
        REQUIRE(id == owner->GetFormId());
        ++beforeCount;
        prepared = users;
        for (auto user : users) {
          REQUIRE(server.serverState.userInfo[user]->gameplayBlocked);
          REQUIRE(barrier->HoldsGate(user));
          server.BeginPacketEpoch(user);
        }
        if (beforeEffect)
          beforeEffect();
        return beforeReceipt;
      },
      [&](uint32_t id, const auto& users, bool synchronized) {
        REQUIRE(id == owner->GetFormId());
        ++afterCount;
        finished = users;
        finishSynchronized = synchronized;
        if (afterEffect)
          afterEffect();
        return afterReceipt;
      },
      [&](Networking::UserId user) { closed.push_back(user); });
  }

  void Abort()
  {
    barrier->AfterCommit(*owner, false);
    REQUIRE_FALSE(barrier->IsActive());
    REQUIRE_FALSE(finishSynchronized);
  }

  void RequireQuarantined()
  {
    REQUIRE(closed == CampaignVisibilityBarrier::UserIds{ 0, 1 });
    for (auto user : closed) {
      REQUIRE(server.serverState.userInfo[user]->gameplayBlocked);
      REQUIRE(server.serverState.userInfo[user]->campaignVisibilityFailed);
    }
    REQUIRE_FALSE(server.serverState.userInfo[2]->gameplayBlocked);
    REQUIRE_FALSE(server.serverState.userInfo[3]->gameplayBlocked);
  }
};
}

TEST_CASE("Visibility barrier captures every connected private observer",
          "[Projection][CampaignVisibilityBarrier]")
{
  BarrierWorld f;
  const auto oldEpoch = f.server.GetPacketEpoch(1);
  f.barrier->BeforeCommit(*f.owner);
  REQUIRE(f.prepared == CampaignVisibilityBarrier::UserIds{ 0, 1 });
  REQUIRE(f.barrier->IsActive());
  REQUIRE_FALSE(f.barrier->HoldsGate(2));
  REQUIRE(f.server.GetPacketEpoch(1) != oldEpoch);
  ActionListener listener(f.server);
  REQUIRE_FALSE(listener.IsPacketEpochCurrent(1, f.server.GetPacketEpoch(1)));
  REQUIRE_FALSE(listener.IsPacketEpochCurrent(1, oldEpoch));
  const auto preparedEpoch = f.server.GetPacketEpoch(1);
  bool publishNow = false;
  SECTION("Deferred publication awaiting client hello")
  {
  }
  SECTION("Publication rotates prepared epochs for fresh baselines")
  {
    publishNow = true;
    f.afterEffect = [&] {
      for (auto user : f.prepared)
        f.server.BeginPacketEpoch(user);
    };
  }
  f.barrier->AfterCommit(*f.owner, true);
  REQUIRE(f.finishSynchronized);
  REQUIRE(f.finished == f.prepared);
  REQUIRE(f.closed.empty());
  REQUIRE_FALSE(f.barrier->IsActive());
  REQUIRE_FALSE(f.barrier->HoldsGate(1));
  REQUIRE((f.server.GetPacketEpoch(1) != preparedEpoch) == publishNow);
  // Publication never opens gameplay; the normal current-token ack does.
  REQUIRE(f.server.serverState.userInfo[0]->gameplayBlocked);
  REQUIRE(f.server.serverState.userInfo[1]->gameplayBlocked);
  f.server.serverState.userInfo[1]->gameplayBlocked = false;
  REQUIRE(listener.IsPacketEpochCurrent(1, f.server.GetPacketEpoch(1)));
  REQUIRE_FALSE(listener.IsPacketEpochCurrent(1, oldEpoch));
}

TEST_CASE("Visibility barrier rejects unprepared observers before any gate",
          "[Projection][CampaignVisibilityBarrier]")
{
  BarrierWorld f;
  SECTION("Legacy observer")
  {
    f.server.serverState.userInfo[1]->packetEpoch.clear();
  }
  SECTION("Malformed observer epoch")
  {
    f.server.serverState.userInfo[1]->packetEpoch = "bad";
  }
  SECTION("Disconnect pending")
  {
    f.server.serverState.userInfo[1]->isDisconnecting = true;
  }
  SECTION("Quarantined observer")
  {
    f.server.serverState.userInfo[1]->campaignVisibilityFailed = true;
  }
  SECTION("Shared world")
  {
    f.owner->SetWorldInstance("");
    f.guest->SetWorldInstance("");
  }
  REQUIRE_THROWS(f.barrier->BeforeCommit(*f.owner));
  f.Abort();
  REQUIRE(f.beforeCount == 0);
  REQUIRE(f.afterCount == 0);
  REQUIRE(f.closed.empty());
  REQUIRE_FALSE(f.server.serverState.userInfo[0]->gameplayBlocked);
  REQUIRE_FALSE(f.server.serverState.userInfo[1]->gameplayBlocked);
}

TEST_CASE(
  "Visibility barrier pairs failed preparation and quarantines observers",
  "[Projection][CampaignVisibilityBarrier]")
{
  BarrierWorld f;
  const auto oldEpoch = f.server.GetPacketEpoch(1);
  SECTION("Missing receipt")
  {
    f.beforeReceipt = false;
  }
  SECTION("Thrown preparation")
  {
    f.beforeEffect = [] { throw std::runtime_error("projection failed"); };
  }
  SECTION("One observer did not advance")
  {
    f.beforeEffect = [&] {
      f.server.serverState.userInfo[1]->packetEpoch = oldEpoch;
    };
  }
  SECTION("One gate reopened")
  {
    f.beforeEffect = [&] {
      f.server.serverState.userInfo[1]->gameplayBlocked = false;
    };
  }
  SECTION("Observer left and returned")
  {
    f.beforeEffect = [&] {
      const auto instance = f.guest->GetWorldInstance();
      f.guest->SetWorldInstance("");
      f.guest->SetWorldInstance(instance);
    };
  }
  SECTION("New observer joined")
  {
    f.beforeEffect = [&] {
      f.outsider->SetWorldInstance(f.owner->GetWorldInstance());
    };
  }
  REQUIRE_THROWS(f.barrier->BeforeCommit(*f.owner));
  f.Abort();
  REQUIRE(f.beforeCount == 1);
  REQUIRE(f.afterCount == 1);
  REQUIRE(f.finished == f.prepared);
  f.RequireQuarantined();
}

TEST_CASE(
  "Visibility barrier cannot mistake a reused connection for its snapshot",
  "[Projection][CampaignVisibilityBarrier]")
{
  BarrierWorld f;
  const auto oldIdentity =
    f.server.serverState.userInfo[1]->connectionIdentity;
  f.beforeEffect = [&] {
    const auto guid = f.server.serverState.UserGuid(1);
    f.server.serverState.Disconnect(1);
    f.server.serverState.Connect(1, guid);
    f.server.serverState.actorsMap.Set(1, f.guest);
    f.server.serverState.userInfo[1]->gameplayBlocked = true;
    f.server.BeginPacketEpoch(1);
    f.server.serverState.userInfo[1]->gameplayBlocked = false;
  };
  REQUIRE_THROWS(f.barrier->BeforeCommit(*f.owner));
  REQUIRE(f.server.serverState.userInfo[1]->connectionIdentity != oldIdentity);
  f.Abort();
  REQUIRE(f.closed == CampaignVisibilityBarrier::UserIds{ 0 });
  REQUIRE_FALSE(f.server.serverState.userInfo[1]->campaignVisibilityFailed);
  REQUIRE_FALSE(f.server.serverState.userInfo[1]->gameplayBlocked);
}

TEST_CASE(
  "Visibility barrier preserves the outer transaction on nested failure",
  "[Projection][CampaignVisibilityBarrier]")
{
  BarrierWorld f;
  const auto nested = [&] {
    REQUIRE_THROWS(f.barrier->BeforeCommit(*f.owner));
    f.barrier->AfterCommit(*f.owner, false);
    REQUIRE(f.barrier->IsActive());
    REQUIRE(f.closed.empty());
  };
  f.beforeEffect = nested;
  f.afterEffect = nested;
  f.barrier->BeforeCommit(*f.owner);
  f.barrier->AfterCommit(*f.owner, true);
  REQUIRE(f.beforeCount == 1);
  REQUIRE(f.afterCount == 1);
  REQUIRE(f.finishSynchronized);
  REQUIRE(f.closed.empty());
  REQUIRE_FALSE(f.barrier->IsActive());
}

TEST_CASE("Visibility barrier quarantines failed publication without throwing",
          "[Projection][CampaignVisibilityBarrier]")
{
  BarrierWorld f;
  const auto oldEpoch = f.server.GetPacketEpoch(1);
  f.barrier->BeforeCommit(*f.owner);
  bool synchronized = true;
  SECTION("Commit aborted or native subscriptions failed")
  {
    synchronized = false;
  }
  SECTION("No publication receipt")
  {
    f.afterReceipt = false;
  }
  SECTION("Publication hook throws")
  {
    f.afterEffect = [] { throw std::runtime_error("manifest failed"); };
  }
  SECTION("Publication released a gate")
  {
    f.afterEffect = [&] {
      f.server.serverState.userInfo[1]->gameplayBlocked = false;
    };
  }
  SECTION("Publication restored the pre-mutation epoch")
  {
    f.afterEffect = [&] {
      f.server.serverState.userInfo[1]->packetEpoch = oldEpoch;
    };
  }
  SECTION("Owner phase changed before publication")
  {
    f.owner->SetWorldInstance("");
  }
  REQUIRE_NOTHROW(f.barrier->AfterCommit(*f.owner, synchronized));
  REQUIRE(f.afterCount == 1);
  REQUIRE(f.finished == f.prepared);
  REQUIRE_FALSE(f.barrier->IsActive());
  f.RequireQuarantined();
}

TEST_CASE(
  "Visibility barrier retains native quarantine when transport close fails",
  "[Projection][CampaignVisibilityBarrier]")
{
  BarrierWorld f;
  int attempted = 0;
  CampaignVisibilityBarrier bridge(
    f.server,
    [&](auto, const auto& users) {
      for (auto user : users)
        f.server.BeginPacketEpoch(user);
      return true;
    },
    [](auto, const auto&, bool) { return false; },
    [&](auto) {
      ++attempted;
      throw std::runtime_error("transport failed");
    });
  bridge.BeforeCommit(*f.owner);
  REQUIRE_NOTHROW(bridge.AfterCommit(*f.owner, true));
  REQUIRE(attempted == 2);
  for (int user : { 0, 1 }) {
    REQUIRE(f.server.serverState.userInfo[user]->gameplayBlocked);
    REQUIRE(f.server.serverState.userInfo[user]->campaignVisibilityFailed);
  }
  REQUIRE_FALSE(bridge.IsActive());
}
