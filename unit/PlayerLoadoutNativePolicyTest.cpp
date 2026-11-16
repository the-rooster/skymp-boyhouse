#include "../skyrim-platform/src/platform_se/skyrim_platform/PlayerLoadoutNativePolicy.h"
#include <catch2/catch_all.hpp>

namespace {
using namespace PlayerLoadout;
using namespace PlayerLoadoutNativePolicy;
Stack StackOf(uint64_t token, uint32_t count, Metadata extras = {},
              bool right = false)
{
  Stack result;
  result.token = token;
  result.item = { 10, count, std::move(extras) };
  result.right = right;
  return result;
}
Snapshot SnapshotOf(std::vector<Stack> stacks)
{
  Snapshot result;
  result.stamp = 1;
  result.stacks = std::move(stacks);
  result.bases = { { 10, { "Item", true, true, true, 1, 2 } } };
  return result;
}
Target DualWield()
{
  Target result;
  result.equipment =
    Equipment{ { { 10, {}, true, false }, { 10, {}, false, true } }, {} };
  return result;
}
}

TEST_CASE("Native ammo equipment uses the whole observed count without "
          "inventing inventory changes",
          "[player-loadout-native]")
{
  auto before = SnapshotOf({ StackOf(1, 80), StackOf(2, 120) });
  Target target;
  target.equipment = Equipment{ { { 10, {}, true, false } }, {} };
  const auto plan = BuildPlan(before, target);
  const auto op = plan.operations.front();
  REQUIRE(op.item.count == 1);
  REQUIRE(EquipCount(before, op, true) == 200);
  REQUIRE(EquipCount(before, op, false) == 1);
  auto after = SnapshotOf({ StackOf(50, 200, {}, true) });
  after.stamp = 2;
  REQUIRE(Observe(op, before, after) == Observation::Applied);
  const auto clear =
    BuildPlan(after, Target{ {}, Equipment{} }).operations.front();
  REQUIRE(clear.kind == OperationKind::Unequip);
  REQUIRE(EquipCount(after, clear, true) == 200);
  REQUIRE_THROWS(EquipCount(SnapshotOf({}), op, true));
}

TEST_CASE("Native ammo preflight refuses all competing metadata and protected "
          "variants",
          "[player-loadout-native]")
{
  auto snapshot = SnapshotOf({ StackOf(1, 50), StackOf(2, 30) });
  REQUIRE(PlainAmmoCount(snapshot, 10) == 80);
  snapshot.stacks[1].item.extras.charge = 0.f;
  REQUIRE_THROWS(PlainAmmoCount(snapshot, 10));
  snapshot.stacks[1].item.extras = {};
  snapshot.stacks[1].questProtected = true;
  REQUIRE_THROWS(PlainAmmoCount(snapshot, 10));
  snapshot.stacks[1].questProtected = false;
  snapshot.stacks[1].unknownExtras = true;
  REQUIRE_THROWS(PlainAmmoCount(snapshot, 10));
  snapshot.stacks[1].unknownExtras = false;
  snapshot.stacks[1].item.count = kMaxCount;
  REQUIRE_THROWS(PlainAmmoCount(snapshot, 10));
}

TEST_CASE("Native full preflight rejects repeated selectors before a "
          "shared-stack equip",
          "[player-loadout-native]")
{
  const auto plan = BuildPlan(SnapshotOf({ StackOf(1, 2) }), DualWield());
  REQUIRE(plan.preflightOperations.size() == 2);
  REQUIRE_THROWS(ValidateEquipSelections(plan));
  auto already = SnapshotOf({ StackOf(1, 2, {}, true) });
  already.stacks.front().left = true;
  const auto exact = BuildPlan(already, DualWield());
  REQUIRE(exact.matches);
  REQUIRE_NOTHROW(ValidateEquipSelections(exact));
}

TEST_CASE("Native full preflight rejects future repeated selectors before "
          "adding the stack",
          "[player-loadout-native]")
{
  auto target = DualWield();
  target.inventory = std::vector<Item>{ { 10, 2, {} } };
  const auto plan = BuildPlan(SnapshotOf({}), target);
  REQUIRE(plan.operations.front().kind == OperationKind::Add);
  REQUIRE_THROWS(ValidateEquipSelections(plan));
}

TEST_CASE("Native distinct exact instances remain selectable for two hands",
          "[player-loadout-native]")
{
  auto before = SnapshotOf({ StackOf(1, 1), StackOf(2, 1) });
  REQUIRE_NOTHROW(ValidateEquipSelections(BuildPlan(before, DualWield())));
  Metadata depleted;
  depleted.charge = 0.f;
  before.stacks[1].item.extras = depleted;
  auto target = DualWield();
  target.equipment->items[1].extras = depleted;
  REQUIRE_NOTHROW(ValidateEquipSelections(BuildPlan(before, target)));
}
