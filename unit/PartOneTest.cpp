#include "TestUtils.hpp"
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>

using Catch::Matchers::ContainsSubstring;

TEST_CASE("PartOne API doesn't crash when bad userId passed", "[PartOne]")
{
  PartOne partOne;

  REQUIRE_THROWS_WITH(partOne.GetUserActor(Networking::InvalidUserId),
                      ContainsSubstring("User with id 65535 doesn't exist"));

  REQUIRE_THROWS_WITH(partOne.SetUserActor(Networking::InvalidUserId, 0),
                      ContainsSubstring("User with id 65535 doesn't exist"));
}

TEST_CASE("SetUserActor doesn't accept disabled actors", "[PartOne]")
{

  PartOne partOne;
  partOne.CreateActor(0xff000000, { 0, 0, 0 }, 0.0, 0x3c);
  partOne.SetEnabled(0xff000000, false);

  DoConnect(partOne, 0);

  REQUIRE_THROWS_WITH(partOne.SetUserActor(0, 0xff000000),
                      ContainsSubstring("Actor with id ff000000 is disabled"));
}

TEST_CASE("OnConnect/OnDisconnect", "[PartOne]")
{
  auto lst = FakeListener::New();
  PartOne partOne(lst);

  DoConnect(partOne, 0);
  DoDisconnect(partOne, 0);

  REQUIRE_THAT(lst->str(), ContainsSubstring("OnConnect(0)\nOnDisconnect(0)"));
}

TEST_CASE("OnCustomPacket", "[PartOne]")
{
  auto lst = FakeListener::New();
  PartOne partOne(lst);

  DoConnect(partOne, 0);
  DoMessage(partOne, 0,
            nlohmann::json{
              { "t", MsgType::CustomPacket },
              { "contentJsonDump", nlohmann::json{ { "x", "y" } }.dump() } });
  REQUIRE_THAT(lst->str(),
               ContainsSubstring("OnCustomPacket(0, {\"x\":\"y\"})"));
}

TEST_CASE("Messages for non-existent users", "[PartOne]")
{
  struct LoggerGuard
  {
    std::shared_ptr<spdlog::logger> old;
    LoggerGuard() { old = spdlog::default_logger(); }
    ~LoggerGuard() { spdlog::set_default_logger(old); }
  } guard;

  std::ostringstream oss;
  auto oss_sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
  auto logger = std::make_shared<spdlog::logger>("test", oss_sink);
  logger->set_pattern("%v");
  logger->set_level(spdlog::level::err);
  spdlog::set_default_logger(logger);

  PartOne partOne;

  DoMessage(partOne, 0,
            nlohmann::json{
              { "t", MsgType::CustomPacket },
              { "contentJsonDump", nlohmann::json{ { "x", "y" } }.dump() } });

  REQUIRE_THAT(
    oss.str(),
    ContainsSubstring("PartOne::HandleMessagePacket - received "
                      "Message packet from non-existing user 0, ignoring"));

  // Check valid case
  oss.str("");
  oss.clear();

  DoConnect(partOne, 0);

  DoMessage(partOne, 0,
            nlohmann::json{
              { "t", MsgType::CustomPacket },
              { "contentJsonDump", nlohmann::json{ { "x", "y" } }.dump() } });

  REQUIRE(oss.str().empty());

  DoDisconnect(partOne, 0);

  DoMessage(partOne, 0,
            nlohmann::json{
              { "t", MsgType::CustomPacket },
              { "contentJsonDump", nlohmann::json{ { "x", "y" } }.dump() } });

  REQUIRE_THAT(
    oss.str(),
    ContainsSubstring("PartOne::HandleMessagePacket - received "
                      "Message packet from non-existing user 0, ignoring"));
}

TEST_CASE("Disconnect event sent before user actually disconnects",
          "[PartOne]")
{
  static auto partOne = std::make_shared<PartOne>();

  using Base = FakeListener;
  class MyFakeListener : public Base
  {
  public:
    void OnDisconnect(Networking::UserId userId) override
    {
      REQUIRE(partOne->IsConnected(userId));
      Base::OnDisconnect(userId);
    }
  };
  auto lst = std::make_shared<MyFakeListener>();
  partOne->AddListener(lst);

  REQUIRE(!partOne->IsConnected(0));
  DoConnect(*partOne, 0);
  REQUIRE(partOne->IsConnected(0));
  DoDisconnect(*partOne, 0);
  REQUIRE(!partOne->IsConnected(0));

  REQUIRE_THAT(lst->str(), ContainsSubstring("OnConnect(0)\nOnDisconnect(0)"));
  partOne.reset();
}

TEST_CASE("Server custom packet", "[PartOne]")
{

  PartOne partOne;

  DoConnect(partOne, 1);

  partOne.SendCustomPacket(1, nlohmann::json({ { "x", "y" } }).dump());
  REQUIRE(partOne.Messages().size() == 1);
  REQUIRE(partOne.Messages()[0].j.dump() ==
          nlohmann::json{
            { "t", static_cast<int>(MsgType::CustomPacket) },
            { "contentJsonDump", nlohmann::json{ { "x", "y" } }.dump() } }
            .dump());
  REQUIRE(partOne.Messages()[0].userId == 1);
  REQUIRE(partOne.Messages()[0].reliable);
}

TEST_CASE("Disconnect callbacks cannot erase a replacement connection",
          "[PartOne][CampaignResume]")
{
  struct ReconnectListener : FakeListener
  {
    explicit ReconnectListener(PartOne& server)
      : server(server)
    {
    }
    void OnDisconnect(Networking::UserId user) override
    {
      DoConnect(server, user);
    }
    PartOne& server;
  };
  PartOne server;
  server.AddListener(std::make_shared<ReconnectListener>(server));
  auto trailing = FakeListener::New();
  server.AddListener(trailing);
  std::shared_ptr<const uint8_t> original;
  SECTION("Assigned slot")
  {
    DoConnect(server, 0);
    server.CreateActor(0xff001234, {}, 0, 0x3c);
    server.SetUserActor(0, 0xff001234);
    original = server.serverState.userInfo[0]->connectionIdentity;
  }
  SECTION("Unassigned slot")
  {
    REQUIRE_FALSE(server.IsConnected(0));
  }
  DoDisconnect(server, 0);
  REQUIRE(server.IsConnected(0));
  REQUIRE(server.serverState.userInfo[0]->connectionIdentity != original);
  REQUIRE(server.GetUserActor(0) == 0);
  REQUIRE(trailing->str().find("OnDisconnect") == std::string::npos);
}
