#include "../skyrim-platform/src/platform_se/skyrim_platform/PlayerLoadoutPlan.h"
#include <catch2/catch_all.hpp>
#include <limits>

namespace {
using namespace PlayerLoadout;
Metadata Charge(float value)
{
  Metadata result;
  result.charge = value;
  return result;
}
Item Object(uint32_t base, uint32_t count, Metadata extras = {})
{
  return { base, count, std::move(extras) };
}
Stack Instance(uint64_t token, Item item, bool right = false,
               bool left = false)
{
  Stack result;
  result.token = token;
  result.item = std::move(item);
  result.right = right;
  result.left = left;
  return result;
}
Snapshot World(std::vector<Stack> stacks = {})
{
  Snapshot result;
  result.stamp = 1;
  result.stacks = std::move(stacks);
  result.bases = { { 10, { "Sword", true, true, true, 1, 2 } },
                   { 20, { "Potion", true, false, false, 0, 0 } },
                   { 30, { "Armor", true, true, false, 4, 0 } } };
  result.spellBases = {
    { 100,
      { { true, true, true, true },
        { 2, 1, 8, 16 },
        { true, true, false, false } } },
    { 200,
      { { true, true, true, true },
        { 2, 1, 8, 16 },
        { true, true, false, false } } },
    { 300, { { true, true, false, false }, { 3, 3, 0, 0 }, {} } }
  };
  return result;
}
Target Inventory(std::vector<Item> items)
{
  Target result;
  result.inventory = std::move(items);
  return result;
}
Target Gear(std::vector<WornItem> items, std::array<uint32_t, 4> spells = {})
{
  Target result;
  result.equipment = Equipment{ std::move(items), spells };
  return result;
}
Snapshot Fresh(Snapshot before, std::vector<Stack> stacks)
{
  ++before.stamp;
  before.stacks = std::move(stacks);
  return before;
}
}

TEST_CASE("Loadout planning preserves exact metadata and distinguishes absent "
          "charge from zero",
          "[PlayerLoadoutPlan]")
{
  auto current = World({ Instance(1, Object(10, 2, Charge(0))) });
  REQUIRE(BuildPlan(current, Inventory({ Object(10, 2, Charge(0)) })).matches);
  const auto plan = BuildPlan(current, Inventory({ Object(10, 2) }));
  REQUIRE_FALSE(plan.matches);
  REQUIRE(plan.operations.size() == 2);
  REQUIRE(plan.operations[0].kind == OperationKind::Remove);
  REQUIRE(plan.operations[0].token == 1);
  REQUIRE(plan.operations[0].item.extras.charge == 0.f);
  REQUIRE(plan.operations[1].kind == OperationKind::Add);
  REQUIRE_FALSE(plan.operations[1].item.extras.charge);
  current.stacks[0].unknownExtras = true;
  REQUIRE_THROWS(BuildPlan(current, Inventory({ Object(10, 2) })));
}

TEST_CASE("Loadout canonicalization preserves float32 health names and typed "
          "zero extras",
          "[PlayerLoadoutPlan]")
{
  Metadata exact;
  exact.health = 1.04f;
  exact.charge = 0.f;
  exact.name = "Sword (Ice)";
  exact.enchantmentId = 100;
  exact.maxCharge = 0;
  exact.removeEnchantmentOnUnequip = false;
  exact.soul = 0;
  exact.poisonId = 200;
  exact.poisonCount = 0;
  const auto current = World({ Instance(1, Object(10, 1, exact)) });
  REQUIRE(BuildPlan(current, Inventory({ Object(10, 1, exact) })).matches);
  auto changed = exact;
  changed.name = "Sword (Fire)";
  REQUIRE_FALSE(
    BuildPlan(current, Inventory({ Object(10, 1, changed) })).matches);
  changed = exact;
  changed.health = 1.03f;
  REQUIRE_FALSE(
    BuildPlan(current, Inventory({ Object(10, 1, changed) })).matches);
  Metadata defaults;
  defaults.health = 1.f;
  defaults.name = "Sword";
  REQUIRE(CanonicalTarget(current, Inventory({ Object(10, 1, defaults) })) ==
          CanonicalTarget(current, Inventory({ Object(10, 1) })));
  for (const auto bad : { std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN(), -1.f }) {
    changed = exact;
    changed.health = bad;
    REQUIRE_THROWS(BuildPlan(current, Inventory({ Object(10, 1, changed) })));
  }
  changed = exact;
  changed.maxCharge = 65536;
  REQUIRE_THROWS(BuildPlan(current, Inventory({ Object(10, 1, changed) })));
  changed = exact;
  changed.soul = 6;
  REQUIRE_THROWS(BuildPlan(current, Inventory({ Object(10, 1, changed) })));
  changed = exact;
  changed.name = std::string(kMaxNameBytes + 1, 'x');
  REQUIRE_THROWS(BuildPlan(current, Inventory({ Object(10, 1, changed) })));
}

TEST_CASE("Loadout retention prioritizes protected instances and removes "
          "precise partial surplus",
          "[PlayerLoadoutPlan]")
{
  auto current =
    World({ Instance(1, Object(10, 2)), Instance(2, Object(10, 3)) });
  current.stacks[0].unknownExtras = true;
  current.stacks[0].questProtected = true;
  const auto target = Inventory({ Object(10, 3) });
  const auto plan = BuildPlan(current, target);
  REQUIRE(plan.operations.size() == 1);
  const auto& remove = plan.operations.front();
  REQUIRE(remove.kind == OperationKind::Remove);
  REQUIRE(remove.token == 2);
  REQUIRE(remove.item.count == 2);
  auto after = current;
  ++after.stamp;
  after.stacks[0].token = 41;
  after.stacks[1].token = 42;
  after.stacks[1].item.count = 1;
  REQUIRE(Observe(remove, current, after) == Observation::Applied);
  REQUIRE(BuildPlan(after, target).matches);
  REQUIRE_THROWS(BuildPlan(current, Inventory({ Object(10, 1) })));
}

TEST_CASE("Loadout completes destructive and allocation preflight before "
          "exposing an action",
          "[PlayerLoadoutPlan]")
{
  auto current =
    World({ Instance(1, Object(10, 1)), Instance(2, Object(20, 1)) });
  current.stacks[1].questProtected = true;
  REQUIRE_THROWS(BuildPlan(current, Inventory({ Object(10, 2) })));
  current.stacks[1].questProtected = false;
  current.bases.at(10).canAdd = false;
  REQUIRE_THROWS(BuildPlan(current, Inventory({ Object(10, 2) })));
  current.bases.at(10).canAdd = true;
  current.stacks[0].canRemove =
    false; // Plain stack mixed with opaque extra lists.
  REQUIRE_THROWS(BuildPlan(current, Inventory({ Object(20, 2) })));
  REQUIRE(
    BuildPlan(current, Inventory({ Object(10, 1), Object(20, 1) })).matches);
}

TEST_CASE(
  "Loadout equipment chooses exact same-base charge and poison variants",
  "[PlayerLoadoutPlan]")
{
  auto wanted = Charge(0);
  wanted.name = "Twin blade";
  wanted.poisonId = 200;
  wanted.poisonCount = 2;
  auto other = wanted;
  other.poisonCount = 1;
  auto charged = wanted;
  charged.charge = 20.f;
  const auto current = World({ Instance(1, Object(10, 1, other)),
                               Instance(2, Object(10, 1, wanted)),
                               Instance(3, Object(10, 1, charged)) });
  const auto target = Gear({ { 10, wanted, true, false } });
  const auto plan = BuildPlan(current, target);
  REQUIRE(plan.operations.size() == 1);
  REQUIRE(plan.operations[0].kind == OperationKind::Equip);
  REQUIRE(plan.operations[0].token == 2);
  auto after = current;
  ++after.stamp;
  after.stacks[1].right = true;
  for (auto& stack : after.stacks)
    stack.token += 10;
  REQUIRE(Observe(plan.operations[0], current, after) == Observation::Applied);
  REQUIRE(BuildPlan(after, target).matches);
  REQUIRE(after.stacks.size() ==
          3); // Equipment-only never discards inventory.
  auto absent = wanted;
  absent.charge = 21.f;
  REQUIRE_THROWS(BuildPlan(current, Gear({ { 10, absent, true, false } })));
}

TEST_CASE("Loadout equips exact quest-owned opaque instances while refusing "
          "destructive removal",
          "[PlayerLoadoutPlan]")
{
  auto current = World({ Instance(1, Object(10, 1), true) });
  current.stacks[0].unknownExtras = true;
  current.stacks[0].questProtected = true;
  REQUIRE(BuildPlan(current, Gear({ { 10, {}, true, false } })).matches);
  const auto unequip = BuildPlan(current, Gear({})).operations.front();
  REQUIRE(unequip.kind == OperationKind::Unequip);
  auto after = current;
  ++after.stamp;
  after.stacks[0].right = false;
  REQUIRE(Observe(unequip, current, after) == Observation::Applied);
  REQUIRE(after.stacks[0].unknownExtras);
  REQUIRE(after.stacks[0].questProtected);
  REQUIRE_THROWS(BuildPlan(current, Inventory({})));
  current.stacks[0].right = false;
  const auto equip =
    BuildPlan(current, Gear({ { 10, {}, true, false } })).operations.front();
  REQUIRE(equip.kind == OperationKind::Equip);
  REQUIRE(equip.token == 1);
  after = current;
  ++after.stamp;
  after.stacks[0].right = true;
  REQUIRE(Observe(equip, current, after) == Observation::Applied);
  current.stacks[0].canEquip = false;
  REQUIRE_THROWS(BuildPlan(current, Gear({ { 10, {}, true, false } })));
  // A new eligible equivalent may be allocated while the restricted copy
  // stays.
  auto target = Inventory({ Object(10, 2) });
  target.equipment = Equipment{ { { 10, {}, true, false } }, {} };
  REQUIRE(BuildPlan(current, target).operations.front().kind ==
          OperationKind::Add);
}

TEST_CASE(
  "Loadout hand transitions retain correct flags without an unequip loop",
  "[PlayerLoadoutPlan]")
{
  const auto current = World({ Instance(1, Object(10, 2), true) });
  const auto both = Gear({ { 10, {}, true, true } });
  const auto plan = BuildPlan(current, both);
  REQUIRE(plan.operations.size() == 1);
  REQUIRE(plan.operations[0].kind == OperationKind::Equip);
  REQUIRE(plan.operations[0].left);
  REQUIRE_FALSE(plan.operations[0].useWornUnit);
  auto after = current;
  ++after.stamp;
  after.stacks[0].left = true;
  REQUIRE(Observe(plan.operations[0], current, after) == Observation::Applied);
  REQUIRE(BuildPlan(after, both).matches);
  const auto right = Gear({ { 10, {}, true, false } });
  const auto narrow = BuildPlan(after, right);
  REQUIRE(narrow.operations.front().kind == OperationKind::Unequip);
  REQUIRE(narrow.operations.front().left);
  auto restored = current;
  restored.stamp = after.stamp + 1;
  REQUIRE(Observe(narrow.operations.front(), after, restored) ==
          Observation::Applied);
  REQUIRE(BuildPlan(restored, right).matches);
  const auto oneCopy = World({ Instance(1, Object(10, 1), true) });
  REQUIRE_THROWS(BuildPlan(oneCopy, both));
  auto impossible = oneCopy;
  impossible.stacks[0].left = true;
  REQUIRE_THROWS(BuildPlan(impossible, both));
  // A genuine two-handed weapon occupies both masks using one right flag.
  auto twoHanded = oneCopy;
  twoHanded.bases.at(10).leftAllowed = false;
  twoHanded.bases.at(10).rightSlots = 3;
  REQUIRE(BuildPlan(twoHanded, right).matches);
  REQUIRE_THROWS(BuildPlan(twoHanded, both));
}

TEST_CASE("Loadout rejects conflicting slots and temporary-enchantment "
          "destruction upfront",
          "[PlayerLoadoutPlan]")
{
  auto current =
    World({ Instance(1, Object(10, 2)), Instance(2, Object(30, 1)) });
  REQUIRE_THROWS(BuildPlan(
    current, Gear({ { 10, {}, true, false }, { 10, {}, true, false } })));
  REQUIRE_FALSE(
    BuildPlan(current,
              Gear({ { 10, {}, true, false }, { 30, {}, true, false } }))
      .matches);
  REQUIRE_THROWS(BuildPlan(current, Gear({ { 20, {}, true, false } })));
  Metadata temporary;
  temporary.enchantmentId = 100;
  temporary.maxCharge = 25;
  temporary.removeEnchantmentOnUnequip = true;
  current = World({ Instance(1, Object(10, 1, temporary), true) });
  REQUIRE(
    BuildPlan(current, Gear({ { 10, temporary, true, false } })).matches);
  REQUIRE_THROWS(BuildPlan(current, Inventory({ Object(20, 1) })));
  REQUIRE_THROWS(BuildPlan(current, Gear({})));
}

TEST_CASE("Loadout preflights equipment against the final managed inventory",
          "[PlayerLoadoutPlan]")
{
  const auto current = World();
  auto target = Inventory({ Object(10, 1, Charge(0)) });
  target.equipment = Equipment{ { { 10, Charge(0), true, false } }, {} };
  const auto plan = BuildPlan(current, target);
  REQUIRE(plan.operations.front().kind == OperationKind::Add);
  auto after = Fresh(current, { Instance(4, Object(10, 1, Charge(0))) });
  REQUIRE(Observe(plan.operations.front(), current, after) ==
          Observation::Applied);
  REQUIRE(BuildPlan(after, target).operations.front().token == 4);
  target.inventory = std::vector<Item>{ Object(10, 1, Charge(1)) };
  REQUIRE_THROWS(BuildPlan(current, target));
}

TEST_CASE("Loadout observation accepts exact effects and stack splitting but "
          "not dispatch receipts",
          "[PlayerLoadoutPlan]")
{
  const auto before = World({ Instance(1, Object(20, 2)) });
  const auto op =
    BuildPlan(before, Inventory({ Object(20, 5) })).operations.front();
  auto unchanged = before;
  ++unchanged.stamp;
  REQUIRE(Observe(op, before, unchanged) == Observation::Pending);
  const auto split = Fresh(
    before, { Instance(10, Object(20, 2)), Instance(11, Object(20, 3)) });
  REQUIRE(Observe(op, before, split) == Observation::Applied);
  const auto tooMany = Fresh(before, { Instance(12, Object(20, 6)) });
  REQUIRE(Observe(op, before, tooMany) == Observation::Unexpected);
  const auto unrelated = Fresh(
    before, { Instance(12, Object(20, 5)), Instance(13, Object(10, 1)) });
  REQUIRE(Observe(op, before, unrelated) == Observation::Unexpected);
  auto stale = op;
  ++stale.stamp;
  REQUIRE_THROWS(Observe(stale, before, split));
}

TEST_CASE(
  "Loadout equipped stack splitting and spells are observed independently",
  "[PlayerLoadoutPlan]")
{
  const auto before = World({ Instance(1, Object(10, 2)) });
  auto target = Gear({ { 10, {}, true, false } }, { 100, 0, 0, 0 });
  const auto equip = BuildPlan(before, target).operations.front();
  const auto after = Fresh(
    before, { Instance(2, Object(10, 1), true), Instance(3, Object(10, 1)) });
  REQUIRE(Observe(equip, before, after) == Observation::Applied);
  const auto spell = BuildPlan(after, target).operations.front();
  REQUIRE(spell.kind == OperationKind::Spell);
  auto changed = after;
  ++changed.stamp;
  changed.spells[0] = 100;
  REQUIRE(Observe(spell, after, changed) == Observation::Applied);
  REQUIRE(BuildPlan(changed, target).matches);
  changed.spells[1] = 200;
  REQUIRE(Observe(spell, after, changed) == Observation::Unexpected);
}

TEST_CASE("Loadout revisions deduplicate canonical requests without replaying "
          "pending additions",
          "[PlayerLoadoutPlan][PlayerLoadoutLifecycle]")
{
  const auto before = World();
  const auto target =
    CanonicalTarget(before, Inventory({ Object(20, 3), Object(20, 2) }));
  const auto same = CanonicalTarget(before, Inventory({ Object(20, 5) }));
  REQUIRE(CheckRevision({}, nullptr, 1, target) == RevisionDecision::Accept);
  REQUIRE(CheckRevision(1, &target, 1, same) == RevisionDecision::Duplicate);
  REQUIRE_THROWS(CheckRevision(2, &target, 1, target));
  REQUIRE_THROWS(CheckRevision(1, &target, 1, Inventory({ Object(20, 6) })));
  REQUIRE_THROWS(CheckRevision({}, nullptr, 0, target));
  REQUIRE_THROWS(CheckRevision({}, nullptr, kMaxRevision + 1, target));
  PendingOperation pending;
  const auto op = BuildPlan(before, target).operations.front();
  pending.Arm(op, before, 1);
  pending.MarkDispatched();
  REQUIRE_THROWS(pending.MarkDispatched());
  REQUIRE_THROWS(pending.Arm(op, before, 1));
  auto sameState = before;
  ++sameState.stamp;
  REQUIRE(pending.ObserveFresh(sameState, 1) == Observation::Pending);
  REQUIRE(pending.HasPending());
  // Superseding the desired revision does not touch the global effect barrier.
  const auto replacement = Inventory({ Object(20, 7) });
  REQUIRE(CheckRevision(1, &target, 2, replacement) ==
          RevisionDecision::Accept);
  REQUIRE_THROWS(pending.Arm(op, before, 1));
  auto applied = Fresh(sameState, { Instance(2, Object(20, 5)) });
  REQUIRE(pending.ObserveFresh(applied, 1) == Observation::Applied);
  REQUIRE_FALSE(pending.HasPending());
  const auto next = BuildPlan(applied, replacement).operations.front();
  REQUIRE(next.item.count == 2); // Only the still-missing deficit is added.
}

TEST_CASE(
  "Loadout input normalization deduplicates without authorizing native slots",
  "[PlayerLoadoutPlan][PlayerLoadoutLifecycle]")
{
  auto input =
    Inventory({ Object(10, 1), Object(20, 2), Object(10, 2), Object(30, 1) });
  input.equipment =
    Equipment{ { { 10, {}, false, true }, { 30, {}, true, false } },
               { 0, 100, 200, 0 } };
  auto reordered = input;
  std::reverse(reordered.inventory->begin(), reordered.inventory->end());
  std::reverse(reordered.equipment->items.begin(),
               reordered.equipment->items.end());
  const auto canonical = CanonicalInputTarget(input);
  const auto repeated = CanonicalInputTarget(reordered);
  REQUIRE(canonical == repeated);
  REQUIRE(canonical.inventory->size() == 3);
  REQUIRE(canonical.inventory->front().count == 3);
  REQUIRE(canonical.equipment->spells == input.equipment->spells);
  REQUIRE(CheckRevision(1, &canonical, 1, repeated) ==
          RevisionDecision::Duplicate);
  auto current =
    World({ Instance(1, Object(10, 3)), Instance(2, Object(20, 2)),
            Instance(3, Object(30, 1)) });
  REQUIRE_THROWS(
    BuildPlan(current, canonical)); // Other-slot mutation is unsupported.
  input.equipment->spells[2] = 0;
  input.equipment->spells[0] =
    100; // Conflicts with the desired left-hand sword.
  REQUIRE_THROWS(BuildPlan(current, CanonicalInputTarget(input)));
  REQUIRE_THROWS(CanonicalInputTarget(Inventory({ Object(0, 1) })));
  REQUIRE_THROWS(CanonicalInputTarget(Inventory({ Object(10, 0) })));
}

TEST_CASE(
  "Loadout spell and item transitions clear conflicting slots before equips",
  "[PlayerLoadoutPlan]")
{
  auto before = World({ Instance(1, Object(10, 1)) });
  before.spells[1] = 100;
  const auto sword = Gear({ { 10, {}, true, false } });
  const auto clear = BuildPlan(before, sword).operations.front();
  REQUIRE(clear.kind == OperationKind::Spell);
  REQUIRE(clear.spellSlot == 1);
  REQUIRE(clear.spellId == 0);
  auto emptyHand = before;
  ++emptyHand.stamp;
  emptyHand.spells[1] = 0;
  REQUIRE(Observe(clear, before, emptyHand) == Observation::Applied);
  const auto equip = BuildPlan(emptyHand, sword).operations.front();
  REQUIRE(equip.kind == OperationKind::Equip);
  auto worn = Fresh(emptyHand, { Instance(2, Object(10, 1), true) });
  REQUIRE(Observe(equip, emptyHand, worn) == Observation::Applied);
  REQUIRE(BuildPlan(worn, sword).matches);

  const auto magic = Gear({}, { 0, 100, 0, 0 });
  const auto unequip = BuildPlan(worn, magic).operations.front();
  REQUIRE(unequip.kind == OperationKind::Unequip);
  REQUIRE(Observe(unequip, worn, Fresh(worn, before.stacks)) ==
          Observation::Applied);
  const auto spell = BuildPlan(emptyHand, magic).operations.front();
  REQUIRE(spell.kind == OperationKind::Spell);
  REQUIRE(spell.spellId == 100);
  auto contradictory = Gear({ { 10, {}, true, false } }, { 0, 100, 0, 0 });
  contradictory.inventory = std::vector<Item>{ Object(10, 2) };
  REQUIRE_THROWS(BuildPlan(before, contradictory));
  before.bases.at(10).rightSlots = 3;
  REQUIRE_THROWS(
    BuildPlan(before, Gear({ { 10, {}, true, false } }, { 100, 0, 0, 0 })));
}

TEST_CASE(
  "Unmodeled spell transitions refuse before unrelated inventory effects",
  "[PlayerLoadoutPlan]")
{
  auto before = World();
  before.spells[2] = 100;
  auto target = Inventory({ Object(20, 1) });
  target.equipment = Equipment{ {}, before.spells };
  REQUIRE(BuildPlan(before, target).operations.front().kind ==
          OperationKind::Add);
  target.equipment->spells[2] = 200;
  REQUIRE_THROWS(BuildPlan(before, target));
  target.equipment->spells[2] = 0;
  REQUIRE_THROWS(BuildPlan(before, target));
  before.spells = { 300, 300, 0, 0 };
  target.equipment->spells = before.spells;
  REQUIRE(BuildPlan(before, target).operations.front().kind ==
          OperationKind::Add);
  target.equipment->spells = {};
  REQUIRE_THROWS(BuildPlan(before, target));
  before.spells = {};
  target.equipment->spells = { 300, 0, 0, 0 };
  REQUIRE_THROWS(BuildPlan(before, target));
  target.equipment->spells = { 999, 0, 0, 0 };
  REQUIRE_THROWS(BuildPlan(before, target));
}

TEST_CASE(
  "Loadout adapter preflight sees future gear behind inventory changes",
  "[PlayerLoadoutPlan]")
{
  auto target = Inventory({ Object(10, 1, Charge(0)) });
  target.equipment = Equipment{ { { 10, Charge(0), true, false } }, {} };
  auto plan = BuildPlan(World(), target);
  REQUIRE(plan.operations.size() == 1);
  REQUIRE(plan.operations.front().kind == OperationKind::Add);
  REQUIRE(plan.preflightOperations.size() == 2);
  REQUIRE(plan.preflightOperations.back().kind == OperationKind::Equip);
  REQUIRE(plan.preflightOperations.back().token == 0);
  REQUIRE(plan.preflightOperations.back().item.extras.charge == 0.f);
  const auto before = World({ Instance(1, Object(10, 2, Charge(0)), true) });
  plan = BuildPlan(before, target);
  REQUIRE(plan.operations.front().kind == OperationKind::Unequip);
  REQUIRE(std::any_of(plan.preflightOperations.begin(),
                      plan.preflightOperations.end(), [](const Operation& op) {
                        return op.kind == OperationKind::Equip &&
                          op.token == 0;
                      }));
  REQUIRE(
    BuildPlan(World({ Instance(1, Object(10, 1, Charge(0)), true) }), target)
      .preflightOperations.empty());
}

TEST_CASE("Loadout uncertainty survives job cancellation and only a new "
          "native world clears it",
          "[PlayerLoadoutPlan][PlayerLoadoutLifecycle]")
{
  const auto before = World();
  const auto op =
    BuildPlan(before, Inventory({ Object(20, 1) })).operations.front();
  PendingOperation pending;
  pending.Arm(op, before, 4);
  pending.MarkDispatched();
  pending.Quarantine("Observation timed out");
  REQUIRE(pending.IsQuarantined());
  REQUIRE(pending.HasPending());
  pending.InvalidateWorld(4);
  REQUIRE(pending.HasPending());
  REQUIRE_THROWS(pending.Arm(op, before, 4));
  REQUIRE_THROWS(pending.InvalidateWorld(3));
  pending.InvalidateWorld(5);
  REQUIRE_FALSE(pending.HasPending());
  REQUIRE_FALSE(pending.IsQuarantined());
  pending.Arm(op, before, 5);
  pending.MarkDispatched();
  REQUIRE_THROWS(
    pending.ObserveFresh(Fresh(before, { Instance(2, Object(20, 1)) }), 4));
  REQUIRE(pending.HasPending());
  REQUIRE(pending.WorldGeneration() == 5);
  REQUIRE(pending.ObserveFresh(Fresh(before, { Instance(2, Object(20, 2)) }),
                               5) == Observation::Unexpected);
  REQUIRE(pending.IsQuarantined());
}

TEST_CASE("Loadout settlement requires distinct matching snapshots after "
          "pending effects finish",
          "[PlayerLoadoutPlan][PlayerLoadoutLifecycle]")
{
  SettlementEvidence evidence;
  REQUIRE_FALSE(evidence.ObserveMatch(1, true));
  REQUIRE_FALSE(evidence.ObserveMatch(1, true));
  REQUIRE(evidence.ObserveMatch(2, true));
  REQUIRE_FALSE(evidence.ObserveMatch(3, false));
  REQUIRE_FALSE(evidence.ObserveMatch(4, true));
  REQUIRE(evidence.ObserveMatch(5, true));
  REQUIRE_THROWS(evidence.ObserveMatch(4, true));
  evidence.Reset();
  REQUIRE_FALSE(evidence.ObserveMatch(6, true));
}

TEST_CASE(
  "Settled loadout requests are terminal while canceled effects still drain",
  "[PlayerLoadoutPlan][PlayerLoadoutLifecycle]")
{
  const auto target = CanonicalTarget(World(), Inventory({ Object(20, 1) }));
  REQUIRE(NeedsWork(true, "queued", false, false));
  REQUIRE(NeedsWork(true, "working", false, false));
  REQUIRE(NeedsWork(true, "observing", false, false));
  REQUIRE_FALSE(NeedsWork(false, "queued", false, false));
  REQUIRE_FALSE(NeedsWork(true, "idle", false, false));
  REQUIRE_FALSE(NeedsWork(true, "settled", false, false));
  REQUIRE(CheckRevision(1, &target, 1, target) == RevisionDecision::Duplicate);
  REQUIRE_FALSE(NeedsWork(true, "settled", false, false));
  REQUIRE(CheckRevision(1, &target, 2, target) == RevisionDecision::Accept);
  REQUIRE(NeedsWork(true, "queued", false, false));
  for (const auto status : { "settled", "superseded", "failed", "idle" }) {
    REQUIRE_FALSE(NeedsWork(true, status, false, false));
    REQUIRE(NeedsWork(true, status, true, false));
    REQUIRE_FALSE(NeedsWork(true, status, true, true));
  }
  REQUIRE(NeedsWork(false, "idle", true, false));
  REQUIRE_FALSE(NeedsWork(true, "queued", false, true));
}

TEST_CASE("Loadout request budget excludes menus and unknown sampling gaps",
          "[PlayerLoadoutPlan][PlayerLoadoutLifecycle]")
{
  EligibleBudget budget(30000, 1000);
  REQUIRE_FALSE(budget.Observe(0, false));
  REQUIRE_FALSE(budget.Observe(60000, false));
  REQUIRE_FALSE(budget.Observe(60001, true));
  REQUIRE_FALSE(budget.Observe(60501, true));
  REQUIRE(budget.Elapsed() == 500);
  REQUIRE_FALSE(budget.Observe(60502, false));
  REQUIRE_FALSE(budget.Observe(120000, true));
  REQUIRE(budget.Elapsed() == 500);
  REQUIRE_FALSE(budget.Observe(180000, true));
  REQUIRE(budget.Elapsed() == 500);
  for (uint64_t i = 1; i < 59; ++i)
    REQUIRE_FALSE(budget.Observe(180000 + i * 500, true));
  REQUIRE(budget.Observe(209500, true));
  REQUIRE(budget.Elapsed() == 30000);
  REQUIRE_THROWS(budget.Observe(1, true));
  budget.Reset();
  REQUIRE_FALSE(budget.Observe(210000, true));
  REQUIRE(budget.Elapsed() == 0);
  REQUIRE_THROWS(EligibleBudget(0));
}

TEST_CASE("Loadout work and operation counts obey explicit bridge bounds",
          "[PlayerLoadoutPlan]")
{
  auto current = World();
  std::vector<Item> desired;
  for (uint32_t i = 0; i < kMaxStacks; ++i) {
    const uint32_t id = 1000 + i;
    current.bases.emplace(id, BaseInfo{ "Item", true });
    current.stacks.push_back(Instance(i + 1, Object(id, 2)));
    desired.push_back(Object(id, 1));
  }
  const auto plan = BuildPlan(current, Inventory(desired));
  REQUIRE(plan.operations.size() == kMaxStacks);
  for (const auto& op : plan.operations) {
    REQUIRE(op.kind == OperationKind::Remove);
    REQUIRE(op.item.count == 1);
  }
  current.stacks.push_back(Instance(kMaxStacks + 1, Object(10, 1)));
  REQUIRE_THROWS(BuildPlan(current, Inventory(desired)));
  desired.push_back(Object(10, 1));
  REQUIRE_THROWS(CanonicalTarget(current, Inventory(desired)));
  current = World();
  const auto add =
    BuildPlan(current, Inventory({ Object(20, 100000) })).operations.front();
  const auto chunk = BoundOperation(add, 32767);
  REQUIRE(chunk.item.count == 32767);
  const auto after = Fresh(current, { Instance(1, Object(20, 32767)) });
  REQUIRE(Observe(chunk, current, after) == Observation::Applied);
  REQUIRE(BuildPlan(after, Inventory({ Object(20, 100000) }))
            .operations.front()
            .item.count == 67233);
  REQUIRE_THROWS(BoundOperation(add, 0));
  REQUIRE_THROWS(BuildPlan(current, Inventory({ Object(20, 0) })));
  REQUIRE_THROWS(
    BuildPlan(current, Inventory({ Object(20, kMaxCount), Object(20, 1) })));
}
