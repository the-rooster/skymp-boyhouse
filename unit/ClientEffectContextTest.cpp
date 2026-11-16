#include "ClientEffectContext.h"
#include "ActionListener.h"
#include "SetInventoryMessage.h"
#include "SpSnippetMessage.h"
#include "TestUtils.hpp"
#include <algorithm>

namespace {
MpActor& Player(PartOne& server, uint32_t id, Networking::UserId user)
{
  DoConnect(server, user);
  server.CreateActor(id, { 1, 2, 3 }, 0, 0x3c);
  server.SetUserActor(user, id);
  return server.worldState.GetFormAt<MpActor>(id);
}

void QueueEffect(MpActor& actor)
{
  SpSnippetMessage message;
  message.class_ = "Debug";
  message.function = "Notification";
  message.snippetIdx = std::numeric_limits<uint32_t>::max();
  actor.SendToUserDeferred(message, true, 1, false);
}

size_t Effects(PartOne& server)
{
  return std::count_if(server.Messages().begin(), server.Messages().end(),
                       [](const PartOne::Message& m) {
                         return dynamic_cast<SpSnippetMessage*>(
                                  m.message.get()) != nullptr;
                       });
}
}

TEST_CASE("Deferred client effects expire across a phase round trip",
          "[Instance][Projection][ClientEffect]")
{
  PartOne server;
  auto& actor = Player(server, 0xff000001, 0);
  QueueEffect(actor);
  server.Messages().clear();
  server.Tick();
  REQUIRE(Effects(server) == 1);

  QueueEffect(actor);
  actor.AddItem(0xf, 3);
  actor.SendInventoryUpdate();
  actor.SetWorldInstance("private");
  actor.SetWorldInstance("");
  server.Messages().clear();
  server.Tick();
  REQUIRE(Effects(server) == 0);
  // Inventory remains personal to this same actor, so changing campaigns does
  // not lose an already queued inventory notification.
  REQUIRE(std::any_of(server.Messages().begin(), server.Messages().end(),
                      [](const PartOne::Message& m) {
                        auto inventory =
                          dynamic_cast<SetInventoryMessage*>(m.message.get());
                        return inventory &&
                          inventory->inventory.GetItemCount(0xf) == 3;
                      }));
}

TEST_CASE(
  "Deferred messages cannot reach a replacement actor with the same ID",
  "[Instance][Projection][ClientEffect]")
{
  PartOne server;
  auto& actor = Player(server, 0xff000001, 0);
  auto originalLifetime =
    server.worldState.LookupFormByIdNoLoad(actor.GetFormId());
  QueueEffect(actor);
  actor.SendInventoryUpdate();
  server.DestroyActor(0xff000001);
  server.CreateActor(0xff000001, { 1, 2, 3 }, 0, 0x3c);
  server.SetUserActor(0, 0xff000001);
  server.Messages().clear();
  server.Tick();
  REQUIRE(server.Messages().empty());
  REQUIRE(originalLifetime !=
          server.worldState.LookupFormByIdNoLoad(0xff000001));
}

TEST_CASE("A guest client effect depends on the host visit and lifetime",
          "[Instance][Projection][ClientEffect]")
{
  PartOne server;
  auto& host = Player(server, 0xff000001, 0);
  auto& guest = Player(server, 0xff000002, 1);
  const std::string phase = "campaign:4278190081:fixture";
  host.SetWorldInstance(phase);
  guest.SetWorldInstance(phase);
  auto context = ClientEffectContext::Capture(guest);
  REQUIRE(context.IsCurrent(guest));
  QueueEffect(guest);
  host.SetWorldInstance("");
  host.SetWorldInstance(phase);
  REQUIRE_FALSE(context.IsCurrent(guest));
  server.Messages().clear();
  server.Tick();
  REQUIRE(Effects(server) == 0);

  context = ClientEffectContext::Capture(guest);
  auto hostLifetime = server.worldState.LookupFormByIdNoLoad(host.GetFormId());
  server.DestroyActor(0xff000001);
  server.CreateActor(0xff000001, { 1, 2, 3 }, 0, 0x3c);
  server.worldState.GetFormAt<MpActor>(0xff000001).SetWorldInstance(phase);
  REQUIRE_FALSE(context.IsCurrent(guest));
}

TEST_CASE("A late snippet reply rejects an expired campaign continuation",
          "[Instance][Projection][ClientEffect]")
{
  PartOne server;
  auto& actor = Player(server, 0xff000001, 0);
  int resolved = 0, rejected = 0;
  Viet::Promise<VarValue> first;
  first.Then([&](const VarValue&) { ++resolved; }).Catch([&](const char*) {
    ++rejected;
  });
  const auto firstIndex = actor.NextSnippetIndex(first);
  actor.ResolveSnippet(firstIndex, VarValue::None());
  actor.ResolveSnippet(firstIndex, VarValue::None());
  REQUIRE(resolved == 1);
  REQUIRE(rejected == 0);

  Viet::Promise<VarValue> second;
  second.Then([&](const VarValue&) { ++resolved; }).Catch([&](const char*) {
    ++rejected;
  });
  const auto secondIndex = actor.NextSnippetIndex(second);
  actor.SetWorldInstance("private");
  actor.SetWorldInstance("");
  actor.ResolveSnippet(secondIndex, VarValue::None());
  actor.ResolveSnippet(secondIndex, VarValue::None());
  REQUIRE(resolved == 1);
  REQUIRE(rejected == 1);
}

TEST_CASE("Host departure cancels a guest wait even when no reply arrives",
          "[Instance][Projection][ClientEffect]")
{
  PartOne server;
  auto& host = Player(server, 0xff000001, 0);
  auto& guest = Player(server, 0xff000002, 1);
  host.SetWorldInstance("campaign:4278190081:fixture");
  guest.SetWorldInstance(host.GetWorldInstance());
  bool resolved = false, rejected = false;
  Viet::Promise<VarValue> promise;
  promise.Then([&](const VarValue&) { resolved = true; })
    .Catch([&](const char*) { rejected = true; });
  guest.NextSnippetIndex(promise);
  host.SetWorldInstance("");
  server.Tick();
  REQUIRE(rejected);
  REQUIRE_FALSE(resolved);
}

TEST_CASE("Client effect cancellation can enqueue a fresh command safely",
          "[Instance][Projection][ClientEffect]")
{
  PartOne server;
  auto& actor = Player(server, 0xff000001, 0);
  int rejected = 0;
  Viet::Promise<VarValue> promise;
  promise.Catch([&](const char*) {
    ++rejected;
    QueueEffect(actor);
  });
  actor.NextSnippetIndex(promise);
  QueueEffect(actor);
  actor.SetWorldInstance("private");
  server.Messages().clear();
  server.Tick();
  REQUIRE(rejected == 1);
  REQUIRE(Effects(server) == 0);
  server.Tick();
  REQUIRE(Effects(server) == 1);
  server.Tick();
  REQUIRE(Effects(server) == 1);
}

TEST_CASE("Disabling a snippet actor rejects rather than resumes its script",
          "[Instance][Projection][ClientEffect]")
{
  PartOne server;
  auto& actor = Player(server, 0xff000001, 0);
  int resolved = 0, rejected = 0;
  Viet::Promise<VarValue> promise;
  promise.Then([&](const VarValue&) { ++resolved; }).Catch([&](const char*) {
    ++rejected;
  });
  const auto index = actor.NextSnippetIndex(promise);
  actor.Disable();
  actor.ResolveSnippet(index, VarValue::None());
  REQUIRE(resolved == 0);
  REQUIRE(rejected == 1);
  REQUIRE_THROWS(ClientEffectContext::Capture(actor));
}

TEST_CASE(
  "A snippet continuation can disable its actor without iterator reuse",
  "[Instance][Projection][ClientEffect]")
{
  PartOne server;
  auto& actor = Player(server, 0xff000001, 0);
  int rejected = 0;
  Viet::Promise<VarValue> first, second;
  first.Then([&](const VarValue&) {
    second.Catch([&](const char*) { ++rejected; });
    actor.NextSnippetIndex(second);
    actor.Disable();
  });
  const auto index = actor.NextSnippetIndex(first);
  REQUIRE_NOTHROW(actor.ResolveSnippet(index, VarValue::None()));
  REQUIRE(rejected == 1);
}

TEST_CASE("Snippet reply indices cannot alias after signed narrowing",
          "[Instance][Projection][ClientEffect]")
{
  PartOne server;
  auto& actor = Player(server, 0xff000001, 0);
  int resolved = 0;
  Viet::Promise<VarValue> promise;
  promise.Then([&](const VarValue&) { ++resolved; });
  const auto index = actor.NextSnippetIndex(promise);
  const auto reply = [&](int64_t value) {
    DoMessage(server, 0,
              { { "t", MsgType::FinishSpSnippet },
                { "snippetIdx", value },
                          { "returnValue", true } });
  };
  reply(static_cast<int64_t>(index) + 0x100000000LL);
  reply(static_cast<int64_t>(index) - 0x100000000LL);
  REQUIRE(resolved == 0);
  reply(index);
  REQUIRE(resolved == 1);
}

TEST_CASE("Cancellation retains the actor through callbacks that destroy it",
          "[Instance][Projection][ClientEffect]")
{
  PartOne server;
  auto& actor = Player(server, 0xff000001, 0);
  int rejected = 0;
  Viet::Promise<VarValue> first, second;
  first.Catch([&](const char*) {
    ++rejected;
    server.DestroyActor(0xff000001);
  });
  second.Catch([&](const char*) { ++rejected; });
  actor.NextSnippetIndex(first);
  actor.NextSnippetIndex(second);
  REQUIRE_NOTHROW(actor.Disable());
  REQUIRE(rejected == 2);
  REQUIRE_FALSE(server.worldState.LookupFormByIdNoLoad(0xff000001));
}
