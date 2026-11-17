#include "../skyrim-platform/src/platform_se/skyrim_platform/ActorFactionsPlan.h"
#include <catch2/catch_all.hpp>

using namespace ActorFactions;

TEST_CASE("Faction planning distinguishes negative rank from membership",
          "[ActorFactionsPlan]")
{
  const Base active{ { 1, 0, false } };
  REQUIRE(Effective(active, {}, false, 1) == Entry{ 1, 0, true });
  REQUIRE(Effective(active, { { 1, -1 } }, false, 1) == Entry{ 1, -1, false });
  REQUIRE(Effective(active, { { 1, -2 } }, false, 1) == Entry{ 1, -2, true });
  REQUIRE(Effective(active, { { 1, -128 } }, false, 1) ==
          Entry{ 1, -128, true });
  REQUIRE(Effective({}, { { 1, -128 } }, false, 1) == Entry{ 1, -128, false });
  REQUIRE(Effective({}, {}, false, 1) == Entry{ 1, -2, false });
  REQUIRE_THROWS(Plan(active, {}, false, {}));
  REQUIRE_THROWS(Plan(active, {}, false, { { 1, -2, false } }));
  REQUIRE(Plan(active, {}, false, { { 1, -2, true } }) == Rows{ { 1, -2 } });
  REQUIRE(Plan(active, {}, false, { { 1, -1, false } }) == Rows{ { 1, -1 } });
}

TEST_CASE(
  "Faction planning preserves expelled player semantics and inactive bases",
  "[ActorFactionsPlan]")
{
  const Base expelled{ { 1, 2, true } };
  REQUIRE(Effective(expelled, {}, true, 1) == Entry{ 1, -2, true });
  REQUIRE(Effective(expelled, {}, false, 1) == Entry{ 1, 2, true });
  REQUIRE(Plan(expelled, {}, true, { { 1, -2, true } }).empty());
  REQUIRE(Plan(expelled, {}, true, { { 1, 2, true } }) == Rows{ { 1, 2 } });
  REQUIRE_THROWS(Plan(expelled, {}, true, {}));
  const Base inactive{ { 1, -1, false } };
  REQUIRE(Plan(inactive, {}, false, {}) == Rows{ { 1, -2 } });
  REQUIRE(Matches(inactive, { { 1, -2 } }, false, {}));
  REQUIRE(Plan({}, { { 1, -1 } }, false, {}).empty());
}

TEST_CASE("Faction plans preserve matching raw rows and reject impossible "
          "complete targets",
          "[ActorFactionsPlan]")
{
  const Base base{ { 1, 5, false } };
  const Rows original{ { 2, 3 }, { 1, 5 } };
  const Target target{ { 1, 5, true }, { 2, 3, true } };
  REQUIRE(Plan(base, original, false, target) == original);
  REQUIRE_THROWS(Plan(base, {}, false, { { 1, 5, false } }));
  REQUIRE_THROWS(Plan({}, {}, false, { { 1, -1, true } }));
  REQUIRE_THROWS(Plan({}, {}, false, { { 1, 128, true } }));
  REQUIRE_THROWS(Plan({}, {}, false, { { 1, 0, true }, { 1, 1, true } }));
  REQUIRE_THROWS(
    Plan({ { 1, 0, false }, { 1, 1, false } }, {}, false, target));
}

TEST_CASE(
  "Faction planning validates the maximum complete snapshot efficiently",
  "[ActorFactionsPlan]")
{
  Target target;
  for (uint32_t id = 1; id <= kMaxEntries; ++id)
    target.push_back({ id, 0, true });
  const auto rows = Plan({}, {}, false, target);
  REQUIRE(rows.size() == kMaxEntries);
  REQUIRE(Matches({}, rows, false, target));
  target.push_back({ static_cast<uint32_t>(kMaxEntries + 1), 0, true });
  REQUIRE_THROWS(Plan({}, {}, false, target));
}

TEST_CASE("Faction settlement requires two distinct updates after dispatch",
          "[ActorFactionsPlan]")
{
  Observations evidence;
  evidence.Reset(100);
  REQUIRE_FALSE(evidence.Match(100));
  REQUIRE_FALSE(evidence.Match(99));
  REQUIRE_FALSE(evidence.Match(101));
  REQUIRE_FALSE(evidence.Match(101));
  REQUIRE(evidence.Match(102));
  evidence.Reset(200);
  REQUIRE_FALSE(evidence.Match(199));
  REQUIRE_FALSE(evidence.Match(201));
  REQUIRE(evidence.Match(202));
}
