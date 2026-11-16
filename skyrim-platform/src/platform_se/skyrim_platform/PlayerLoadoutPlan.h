#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// No engine pointers or engine/JS dependencies. Tokens are valid only in the
// snapshot which supplied them. The adapter must execute at most one operation
// and observe its effect before obtaining another plan from a fresh snapshot.
namespace PlayerLoadout {
constexpr uint32_t kMaxCount = 0x7fffffff;
constexpr uint64_t kMaxRevision = 9007199254740991ULL;
// Bridge resource policies, not engine limits. Inventory work is O(N log N)
// plus at most kMaxEquipped*N matching work, with no unbounded all-pairs scan.
constexpr size_t kMaxStacks = 4096, kMaxEquipped = 128;
constexpr size_t kMaxNameBytes = 1024;

struct Metadata
{
  std::optional<float> health, charge;
  std::optional<std::string> name;
  std::optional<uint32_t> enchantmentId, maxCharge, soul, poisonId,
    poisonCount;
  std::optional<bool> removeEnchantmentOnUnequip;

  auto Key() const
  {
    return std::tie(health, charge, name, enchantmentId, maxCharge, soul,
                    poisonId, poisonCount, removeEnchantmentOnUnequip);
  }
  bool operator==(const Metadata& other) const { return Key() == other.Key(); }
  bool operator<(const Metadata& other) const { return Key() < other.Key(); }
};
struct Item
{
  uint32_t baseId = 0, count = 0;
  Metadata extras;
  bool operator==(const Item& other) const
  {
    return baseId == other.baseId && count == other.count &&
      extras == other.extras;
  }
};
struct WornItem
{
  uint32_t baseId = 0;
  Metadata extras;
  bool right = false, left = false;
  auto Key() const { return std::tie(baseId, extras, right, left); }
  bool operator==(const WornItem& other) const { return Key() == other.Key(); }
  bool operator<(const WornItem& other) const { return Key() < other.Key(); }
};
struct Equipment
{
  std::vector<WornItem> items;
  std::array<uint32_t, 4> spells{};
  bool operator==(const Equipment& other) const
  {
    return items == other.items && spells == other.spells;
  }
};
struct Target
{
  std::optional<std::vector<Item>> inventory;
  std::optional<Equipment> equipment;
  bool operator==(const Target& other) const
  {
    return inventory == other.inventory && equipment == other.equipment;
  }
};
struct BaseInfo
{
  std::string name;
  bool canAdd = false, rightAllowed = false, leftAllowed = false;
  // The adapter resolves biped, hand/two-hand and ammunition conflicts into
  // these masks. An allowed zero-mask item consumes no exclusive slot.
  uint64_t rightSlots = 0, leftSlots = 0;
};
struct SpellInfo
{
  // The adapter supplies authoritative equip-type masks and permitted slots.
  // A correlated or unsupported mutation can be retained when already exact,
  // but canChange must be false until its complete effect is modeled.
  std::array<bool, 4> allowed{};
  std::array<uint64_t, 4> slots{};
  std::array<bool, 4> canChange{};
};
struct Stack
{
  uint64_t token = 0;
  Item item;
  // Each flag represents one equipped copy. Both flags require count >= 2;
  // a two-handed weapon uses right alone with a mask occupying both hands.
  bool right = false, left = false;
  bool unknownExtras = false, questProtected = false;
  bool canRemove = true, canEquip = true, canUnequip = true;
};
struct Snapshot
{
  uint64_t stamp = 0;
  std::vector<Stack> stacks;
  std::map<uint32_t, BaseInfo> bases;
  std::array<uint32_t, 4> spells{};
  std::map<uint32_t, SpellInfo> spellBases;
};
enum class OperationKind
{
  Add,
  Remove,
  Equip,
  Unequip,
  Spell
};
struct Operation
{
  OperationKind kind = OperationKind::Add;
  uint64_t stamp = 0, token = 0;
  Item item;
  bool left = false;
  // Reserved for compatibility with the initial adapter draft; always false.
  // Equip consumes a distinct unworn unit, never the other hand's copy.
  bool useWornUnit = false;
  uint32_t spellSlot = 0, spellId = 0;
};
struct Plan
{
  bool matches = false;
  std::vector<Operation> operations;
  // All required intents, including future gear after inventory mutations.
  // These are for complete adapter preflight only, never execution. A future
  // instance which does not exist yet has token 0 and must be replanned.
  std::vector<Operation> preflightOperations;
};
enum class Observation
{
  Pending,
  Applied,
  Unexpected
};

namespace Detail {
using Key = std::pair<uint32_t, Metadata>;
inline const BaseInfo& Base(const Snapshot& snapshot, uint32_t id)
{
  const auto it = snapshot.bases.find(id);
  if (!id || it == snapshot.bases.end())
    throw std::runtime_error("Loadout base metadata is unavailable");
  return it->second;
}
inline const SpellInfo& Spell(const Snapshot& snapshot, uint32_t id,
                              uint32_t slot)
{
  const auto it = snapshot.spellBases.find(id);
  if (!id || slot >= 4 || it == snapshot.spellBases.end() ||
      !it->second.allowed[slot])
    throw std::runtime_error("Loadout spell slot metadata is unavailable");
  return it->second;
}
inline Operation OnSpell(const Snapshot& snapshot, uint32_t slot, uint32_t id)
{
  Operation result;
  result.kind = OperationKind::Spell;
  result.stamp = snapshot.stamp;
  result.spellSlot = slot;
  result.spellId = id;
  return result;
}
inline Metadata Canonical(const Metadata& input, const BaseInfo& base)
{
  auto result = input;
  for (const auto value : { result.health, result.charge })
    if (value && (!std::isfinite(*value) || *value < 0))
      throw std::runtime_error("Invalid loadout floating-point metadata");
  if (result.health == 1.f)
    result.health.reset();
  if (result.name) {
    if (result.name->find('\0') != std::string::npos ||
        result.name->size() > kMaxNameBytes)
      throw std::runtime_error("Invalid loadout display name");
    if (result.name->empty() || *result.name == base.name)
      result.name.reset();
  }
  if (result.enchantmentId) {
    if (!*result.enchantmentId || !result.maxCharge ||
        *result.maxCharge > 65535)
      throw std::runtime_error("Incomplete loadout enchantment metadata");
    result.removeEnchantmentOnUnequip =
      result.removeEnchantmentOnUnequip.value_or(false);
  } else if (result.maxCharge || result.removeEnchantmentOnUnequip) {
    throw std::runtime_error("Orphan loadout enchantment metadata");
  }
  if (result.soul && *result.soul > 5)
    throw std::runtime_error("Invalid loadout soul level");
  if (result.poisonId) {
    if (!*result.poisonId || !result.poisonCount ||
        *result.poisonCount > kMaxCount)
      throw std::runtime_error("Incomplete loadout poison metadata");
  } else if (result.poisonCount) {
    throw std::runtime_error("Orphan loadout poison metadata");
  }
  return result;
}
inline Key ItemKey(const Snapshot& snapshot, const Item& item)
{
  return { item.baseId, Canonical(item.extras, Base(snapshot, item.baseId)) };
}
inline void AddCount(std::map<Key, uint32_t>& counts, const Key& key,
                     uint32_t count)
{
  auto& value = counts[key];
  if (!count || count > kMaxCount || value > kMaxCount - count)
    throw std::runtime_error("Invalid or overflowing loadout item count");
  value += count;
}
inline std::map<Key, uint32_t> Counts(const Snapshot& snapshot)
{
  if (!snapshot.stamp || snapshot.stacks.size() > kMaxStacks)
    throw std::runtime_error("Invalid loadout snapshot");
  std::map<uint64_t, bool> tokens;
  std::map<Key, uint32_t> counts;
  std::map<uint32_t, uint64_t> baseTotals;
  for (const auto& stack : snapshot.stacks) {
    if (!stack.token || !tokens.emplace(stack.token, true).second)
      throw std::runtime_error("Duplicate or missing loadout instance token");
    const auto key = ItemKey(snapshot, stack.item);
    if (uint32_t(stack.right) + uint32_t(stack.left) > stack.item.count)
      throw std::runtime_error(
        "Equipped loadout copies exceed the stack count");
    AddCount(counts, key, stack.item.count);
    if ((baseTotals[stack.item.baseId] += stack.item.count) > kMaxCount)
      throw std::runtime_error("Loadout base count overflow");
  }
  return counts;
}
inline void Mutable(const Stack& stack, OperationKind kind)
{
  if ((kind == OperationKind::Remove &&
       (stack.unknownExtras || stack.questProtected || !stack.canRemove)) ||
      (kind == OperationKind::Equip && !stack.canEquip) ||
      (kind == OperationKind::Unequip &&
       (!stack.canUnequip ||
        stack.item.extras.removeEnchantmentOnUnequip.value_or(false))))
    throw std::runtime_error(
      "Loadout operation cannot preserve this item instance");
}
inline Operation OnStack(const Snapshot& snapshot, const Stack& stack,
                         OperationKind kind, uint32_t count = 1,
                         bool left = false)
{
  Mutable(stack, kind);
  Operation result;
  result.kind = kind;
  result.stamp = snapshot.stamp;
  result.token = stack.token;
  result.item = { stack.item.baseId, count,
                  ItemKey(snapshot, stack.item).second };
  result.left = left;
  return result;
}
}

inline Target CanonicalTarget(const Snapshot& snapshot, const Target& target)
{
  if (!target.inventory && !target.equipment)
    throw std::runtime_error("Loadout request has no managed component");
  Target result;
  if (target.inventory) {
    if (target.inventory->size() > kMaxStacks)
      throw std::runtime_error("Too many desired loadout entries");
    std::map<Detail::Key, uint32_t> counts;
    std::map<uint32_t, uint64_t> baseTotals;
    for (const auto& item : *target.inventory) {
      Detail::AddCount(counts, Detail::ItemKey(snapshot, item), item.count);
      if ((baseTotals[item.baseId] += item.count) > kMaxCount)
        throw std::runtime_error("Desired loadout base count overflow");
    }
    result.inventory.emplace();
    for (const auto& [key, count] : counts)
      result.inventory->push_back({ key.first, count, key.second });
  }
  if (target.equipment) {
    if (target.equipment->items.size() > kMaxEquipped)
      throw std::runtime_error("Too many desired equipped items");
    result.equipment = Equipment{ {}, target.equipment->spells };
    uint64_t occupied = 0;
    for (auto item : target.equipment->items) {
      const auto& base = Detail::Base(snapshot, item.baseId);
      item.extras = Detail::Canonical(item.extras, base);
      if ((!item.right && !item.left) || (item.right && !base.rightAllowed) ||
          (item.left && !base.leftAllowed))
        throw std::runtime_error("Unsupported loadout equipment slot");
      const auto slots =
        (item.right ? base.rightSlots : 0) | (item.left ? base.leftSlots : 0);
      if (occupied & slots)
        throw std::runtime_error("Conflicting desired equipment slots");
      occupied |= slots;
      if (item.right)
        result.equipment->items.push_back(
          { item.baseId, item.extras, true, false });
      if (item.left)
        result.equipment->items.push_back(
          { item.baseId, item.extras, false, true });
    }
    if (result.equipment->items.size() > kMaxEquipped)
      throw std::runtime_error("Too many desired equipped copies");
    std::map<uint32_t, uint64_t> spellOccupancy;
    for (uint32_t slot = 0; slot < 4; ++slot) {
      const auto id = result.equipment->spells[slot];
      if (!id)
        continue;
      const auto slots = Detail::Spell(snapshot, id, slot).slots[slot];
      // Some retained correlated spells appear in more than one selected
      // slot. Their identical occupancy is one spell, not a second item.
      const auto already = spellOccupancy[id];
      if ((occupied & slots) & ~already)
        throw std::runtime_error("Conflicting desired item and spell slots");
      occupied |= slots;
      spellOccupancy[id] |= slots;
    }
    std::sort(result.equipment->items.begin(), result.equipment->items.end());
  }
  return result;
}

// Input-only normalization supports deterministic request deduplication before
// a native snapshot is available. Permissive synthetic metadata is never a
// mutation authorization: BuildPlan must validate the real snapshot afterward.
inline Target CanonicalInputTarget(const Target& input)
{
  Snapshot schema;
  const auto addBase = [&](uint32_t id) {
    schema.bases[id] = BaseInfo{ "", true, true, true, 0, 0 };
  };
  if (input.inventory)
    for (const auto& item : *input.inventory)
      addBase(item.baseId);
  if (input.equipment) {
    for (const auto& item : input.equipment->items)
      addBase(item.baseId);
    for (const auto id : input.equipment->spells)
      if (id) {
        SpellInfo spell;
        spell.allowed.fill(true);
        spell.canChange.fill(true);
        schema.spellBases[id] = spell;
      }
  }
  return CanonicalTarget(schema, input);
}

inline Plan BuildPlan(const Snapshot& snapshot, const Target& input)
{
  const auto current = Detail::Counts(snapshot);
  const auto target = CanonicalTarget(snapshot, input);
  // Validate every spell transition before exposing any inventory mutation.
  // The bridge currently models independent selected-slot changes only.
  std::vector<Operation> spellClears, spellEquips;
  if (target.equipment)
    for (uint32_t slot = 0; slot < 4; ++slot) {
      const auto oldId = snapshot.spells[slot];
      const auto newId = target.equipment->spells[slot];
      if (oldId == newId)
        continue;
      for (const auto id : { oldId, newId })
        if (id && !Detail::Spell(snapshot, id, slot).canChange[slot])
          throw std::runtime_error("Loadout spell transition is unsupported");
      if (oldId)
        spellClears.push_back(Detail::OnSpell(snapshot, slot, 0));
      if (newId)
        spellEquips.push_back(Detail::OnSpell(snapshot, slot, newId));
    }
  auto finalCounts = current;
  if (target.inventory) {
    finalCounts.clear();
    for (const auto& item : *target.inventory)
      Detail::AddCount(finalCounts, Detail::ItemKey(snapshot, item),
                       item.count);
  }
  // Allocate retained counts before deciding effects. Unknown/protected stacks
  // take priority so an ordinary equivalent instance can supply any surplus.
  std::vector<size_t> order(snapshot.stacks.size());
  for (size_t i = 0; i < order.size(); ++i)
    order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    const auto rank = [](const Stack& s) {
      return std::make_tuple(s.unknownExtras || s.questProtected ||
                               !s.canRemove,
                             s.right || s.left);
    };
    return rank(snapshot.stacks[a]) > rank(snapshot.stacks[b]);
  });
  auto remaining = finalCounts;
  std::vector<uint32_t> keep(snapshot.stacks.size());
  std::vector<Detail::Key> stackKeys;
  stackKeys.reserve(snapshot.stacks.size());
  for (const auto& stack : snapshot.stacks)
    stackKeys.push_back(Detail::ItemKey(snapshot, stack.item));
  std::map<Detail::Key, std::vector<size_t>> groups;
  for (const auto i : order) {
    const auto& stack = snapshot.stacks[i];
    const auto& key = stackKeys[i];
    groups[key].push_back(i);
    auto& needed = remaining[key];
    keep[i] = std::min(needed, stack.item.count);
    needed -= keep[i];
  }

  Plan result;
  // Validate every removal and deficit before exposing even the first action.
  for (size_t i = 0; i < snapshot.stacks.size(); ++i) {
    const auto& stack = snapshot.stacks[i];
    if (keep[i] == stack.item.count)
      continue;
    Detail::Mutable(stack, OperationKind::Remove);
    if (stack.right || stack.left) {
      // Conservatively unequip before count-changing an equipped extra list.
      if (stack.right)
        result.operations.push_back(
          Detail::OnStack(snapshot, stack, OperationKind::Unequip));
      if (stack.left)
        result.operations.push_back(
          Detail::OnStack(snapshot, stack, OperationKind::Unequip, 1, true));
    } else {
      result.operations.push_back(Detail::OnStack(
        snapshot, stack, OperationKind::Remove, stack.item.count - keep[i]));
    }
  }
  for (const auto& [key, count] : remaining) {
    if (!count)
      continue;
    if (!Detail::Base(snapshot, key.first).canAdd)
      throw std::runtime_error("Loadout deficit cannot be allocated safely");
    Operation add;
    add.kind = OperationKind::Add;
    add.stamp = snapshot.stamp;
    add.item = { key.first, count, key.second };
    result.operations.push_back(std::move(add));
  }
  result.preflightOperations = result.operations;
  result.preflightOperations.insert(result.preflightOperations.end(),
                                    spellClears.begin(), spellClears.end());
  result.preflightOperations.insert(result.preflightOperations.end(),
                                    spellEquips.begin(), spellEquips.end());

  if (target.equipment) {
    const auto& desired = target.equipment->items;
    std::map<Detail::Key, uint32_t> wornDemand;
    for (const auto& item : desired) {
      const Detail::Key key{ item.baseId, item.extras };
      if (++wornDemand[key] >
          (finalCounts.count(key) ? finalCounts.at(key) : 0))
        throw std::runtime_error(
          "Requested equipped variant is absent from final inventory");
    }
    std::vector<bool> matched(desired.size(), false);
    std::map<WornItem, std::vector<size_t>> desiredIndices;
    for (size_t j = 0; j < desired.size(); ++j)
      desiredIndices[desired[j]].push_back(j);
    std::vector<uint32_t> used(snapshot.stacks.size());
    std::vector<Operation> unequips, equips;
    for (size_t i = 0; i < snapshot.stacks.size(); ++i) {
      const auto& stack = snapshot.stacks[i];
      if (!stack.right && !stack.left)
        continue;
      for (const bool left : { false, true }) {
        if (!(left ? stack.left : stack.right))
          continue;
        const WornItem worn{ stack.item.baseId, stackKeys[i].second, !left,
                             left };
        size_t found = desired.size();
        const auto available = desiredIndices.find(worn);
        if (keep[i] > used[i] && available != desiredIndices.end() &&
            !available->second.empty()) {
          found = available->second.back();
          available->second.pop_back();
        }
        if (found < desired.size()) {
          matched[found] = true;
          ++used[i];
          if (keep[i] < stack.item.count) {
            Operation futureEquip;
            futureEquip.kind = OperationKind::Equip;
            futureEquip.stamp = snapshot.stamp;
            futureEquip.item = { stack.item.baseId, 1, stackKeys[i].second };
            futureEquip.left = left;
            result.preflightOperations.push_back(std::move(futureEquip));
          }
        } else
          unequips.push_back(
            Detail::OnStack(snapshot, stack, OperationKind::Unequip, 1, left));
      }
    }
    // Even if inventory changes must occur first, check retained immutable
    // items cannot be the only way to satisfy a future equipment requirement.
    auto future = remaining;
    for (size_t j = 0; j < desired.size(); ++j) {
      if (matched[j])
        continue;
      const auto& wanted = desired[j];
      const Detail::Key key{ wanted.baseId, wanted.extras };
      size_t selected = snapshot.stacks.size();
      if (const auto group = groups.find(key); group != groups.end())
        for (const auto i : group->second) {
          const auto& stack = snapshot.stacks[i];
          if (keep[i] <= used[i] || !stack.canEquip)
            continue;
          selected = i;
          break;
        }
      if (selected == snapshot.stacks.size()) {
        if (future[key]) {
          --future[key];
          Operation futureEquip;
          futureEquip.kind = OperationKind::Equip;
          futureEquip.stamp = snapshot.stamp;
          futureEquip.item = { wanted.baseId, 1, wanted.extras };
          futureEquip.left = wanted.left;
          result.preflightOperations.push_back(std::move(futureEquip));
          continue;
        }
        throw std::runtime_error(
          "Requested equipment cannot select a mutable exact instance");
      }
      const auto& stack = snapshot.stacks[selected];
      ++used[selected];
      if (wanted.right) {
        auto op = Detail::OnStack(snapshot, stack, OperationKind::Equip);
        equips.push_back(std::move(op));
      }
      if (wanted.left) {
        auto op =
          Detail::OnStack(snapshot, stack, OperationKind::Equip, 1, true);
        equips.push_back(std::move(op));
      }
    }
    result.preflightOperations.insert(result.preflightOperations.end(),
                                      unequips.begin(), unequips.end());
    result.preflightOperations.insert(result.preflightOperations.end(),
                                      equips.begin(), equips.end());
    if (result.operations.empty()) {
      // Replan after each unequip rather than use selectors whose worn state
      // may have changed. The adapter only executes operations.front().
      if (!unequips.empty())
        result.operations = std::move(unequips);
      else if (!spellClears.empty())
        result.operations = std::move(spellClears);
      else if (!equips.empty())
        result.operations = std::move(equips);
      else
        result.operations = std::move(spellEquips);
    }
  }
  result.matches = result.operations.empty();
  return result;
}

namespace Detail {
using StateKey = std::tuple<uint32_t, Metadata, bool, bool, bool, bool>;
using State = std::map<StateKey, int64_t>;
inline State StateOf(const Snapshot& snapshot)
{
  Counts(snapshot);
  State result;
  for (const auto& stack : snapshot.stacks) {
    const auto key = ItemKey(snapshot, stack.item);
    const auto worn = uint32_t(stack.right) + uint32_t(stack.left);
    if (stack.item.count > worn)
      result[{ key.first, key.second, false, false, stack.unknownExtras,
               stack.questProtected }] += stack.item.count - worn;
    if (stack.right)
      ++result[{ key.first, key.second, true, false, stack.unknownExtras,
                 stack.questProtected }];
    if (stack.left)
      ++result[{ key.first, key.second, false, true, stack.unknownExtras,
                 stack.questProtected }];
  }
  return result;
}
inline void Clean(State& state)
{
  for (auto it = state.begin(); it != state.end();) {
    if (it->second < 0)
      throw std::runtime_error("Invalid planned loadout state transition");
    if (!it->second)
      it = state.erase(it);
    else
      ++it;
  }
}
}

inline Observation Observe(const Operation& operation, const Snapshot& before,
                           const Snapshot& after)
{
  if (!operation.stamp || operation.stamp != before.stamp || !after.stamp)
    throw std::runtime_error("Loadout operation belongs to another snapshot");
  const auto initial = Detail::StateOf(before);
  const auto actual = Detail::StateOf(after);
  auto expected = initial;
  auto spells = before.spells;
  if (operation.kind == OperationKind::Add) {
    const auto key = Detail::ItemKey(before, operation.item);
    if (!operation.item.count || operation.item.count > kMaxCount)
      throw std::runtime_error("Invalid loadout addition");
    expected[{ key.first, key.second, false, false, false, false }] +=
      operation.item.count;
  } else if (operation.kind == OperationKind::Spell) {
    if (operation.spellSlot >= 4)
      throw std::runtime_error("Invalid loadout spell slot");
    if (spells[operation.spellSlot] == operation.spellId)
      throw std::runtime_error("Loadout spell operation has no effect");
    for (const auto id : { spells[operation.spellSlot], operation.spellId })
      if (id &&
          !Detail::Spell(before, id, operation.spellSlot)
             .canChange[operation.spellSlot])
        throw std::runtime_error(
          "Loadout spell effect is not independently modeled");
    spells[operation.spellSlot] = operation.spellId;
  } else {
    const auto it = std::find_if(
      before.stacks.begin(), before.stacks.end(),
      [&](const Stack& stack) { return stack.token == operation.token; });
    if (it == before.stacks.end())
      throw std::runtime_error("Stale loadout instance token");
    const auto& stack = *it;
    Detail::Mutable(stack, operation.kind);
    const auto key = Detail::ItemKey(before, stack.item);
    if (key != Detail::ItemKey(before, operation.item))
      throw std::runtime_error("Loadout operation metadata changed");
    Detail::StateKey from{
      key.first, key.second,          false,
      false,     stack.unknownExtras, stack.questProtected
    };
    if (operation.kind == OperationKind::Remove) {
      if (!operation.item.count || operation.item.count > stack.item.count ||
          stack.right || stack.left)
        throw std::runtime_error("Unsafe planned loadout removal");
      expected[from] -= operation.item.count;
    } else {
      if (operation.useWornUnit || operation.item.count != 1)
        throw std::runtime_error("Invalid equipped-copy operation");
      if (operation.kind == OperationKind::Unequip) {
        if (!(operation.left ? stack.left : stack.right))
          throw std::runtime_error(
            "Loadout item is not equipped in that hand");
        std::get<2>(from) = !operation.left;
        std::get<3>(from) = operation.left;
      } else if (operation.kind == OperationKind::Equip &&
                 stack.item.count <=
                   uint32_t(stack.right) + uint32_t(stack.left)) {
        throw std::runtime_error("No unworn loadout copy is available");
      }
      auto to = from;
      const auto index = operation.left ? 3 : 2;
      const bool wasSet =
        operation.left ? std::get<3>(from) : std::get<2>(from);
      const bool equip = operation.kind == OperationKind::Equip;
      if (wasSet == equip)
        throw std::runtime_error(
          "Loadout operation does not change an equipment slot");
      if (index == 3)
        std::get<3>(to) = equip;
      else
        std::get<2>(to) = equip;
      --expected[from];
      ++expected[to];
    }
  }
  Detail::Clean(expected);
  if (actual == expected && after.spells == spells)
    return Observation::Applied;
  if (actual == initial && after.spells == before.spells)
    return Observation::Pending;
  return Observation::Unexpected;
}

enum class RevisionDecision
{
  Accept,
  Duplicate
};
// Only an explicitly accepted request schedules new work. A settled request
// is terminal even if the observed player state later changes; its owner must
// submit a newer revision. The global effect barrier can still require
// draining after that request is canceled or superseded. Quarantine never
// dispatches.
inline bool NeedsWork(bool hasRevision, const std::string& status,
                      bool mutationPending, bool uncertain)
{
  if (uncertain)
    return false;
  if (mutationPending)
    return true;
  return hasRevision &&
    (status == "queued" || status == "working" || status == "observing");
}

// Request readiness time counts only observed, consecutive eligible samples.
// Long sampling gaps are unknown (e.g. a paused game), so do not consume it.
// This budget does not replace the strict wall deadline of a dispatched
// effect.
class EligibleBudget
{
public:
  explicit EligibleBudget(uint64_t maximumMs = 30000, uint64_t maxGapMs = 1000)
    : maximum(maximumMs)
    , maximumGap(maxGapMs)
  {
    if (!maximum || !maximumGap)
      throw std::runtime_error("Invalid loadout eligible-time budget");
  }
  void Reset()
  {
    previous.reset();
    previousEligible = false;
    elapsed = 0;
  }
  uint64_t Elapsed() const { return elapsed; }
  bool Observe(uint64_t nowMs, bool eligible)
  {
    if (previous && nowMs < *previous)
      throw std::runtime_error("Loadout eligible time must be monotonic");
    if (previous && previousEligible && eligible) {
      const auto delta = nowMs - *previous;
      if (delta <= maximumGap)
        elapsed += std::min(delta, maximum - elapsed);
    }
    previous = nowMs;
    previousEligible = eligible;
    return elapsed >= maximum;
  }

private:
  uint64_t maximum, maximumGap, elapsed = 0;
  std::optional<uint64_t> previous;
  bool previousEligible = false;
};

inline Operation BoundOperation(Operation operation, uint32_t maxCount)
{
  if (!maxCount || maxCount > kMaxCount)
    throw std::runtime_error("Invalid native loadout operation bound");
  if (operation.kind == OperationKind::Add ||
      operation.kind == OperationKind::Remove)
    operation.item.count = std::min(operation.item.count, maxCount);
  return operation;
}
// The adapter owns epoch validation and the current job. Store CanonicalTarget
// output as its target, so reordered rows and default names deduplicate too.
inline RevisionDecision CheckRevision(std::optional<uint64_t> previousRevision,
                                      const Target* previousTarget,
                                      uint64_t revision, const Target& target)
{
  if (!revision || revision > kMaxRevision)
    throw std::runtime_error(
      "Loadout revision must be a positive safe integer");
  if (previousRevision) {
    if (revision < *previousRevision)
      throw std::runtime_error("Stale loadout revision");
    if (revision == *previousRevision) {
      if (!previousTarget || !(*previousTarget == target))
        throw std::runtime_error("Conflicting loadout revision");
      return RevisionDecision::Duplicate;
    }
  }
  return RevisionDecision::Accept;
}

// This barrier is global to the native world, independent of the current job.
// Cancel, supersession and begin-epoch must never destroy it: the old
// operation may already have run. Only observed completion or explicit world
// invalidation removes it. The adapter supplies its mutex and a bounded
// timeout.
class PendingOperation
{
public:
  bool HasPending() const { return operation.has_value(); }
  bool IsDispatched() const { return dispatched; }
  bool IsQuarantined() const { return !error.empty(); }
  uint64_t WorldGeneration() const { return worldGeneration; }
  const std::string& Error() const { return error; }
  const Operation& GetOperation() const { return operation.value(); }
  const Snapshot& Before() const { return before.value(); }

  void Arm(const Operation& next, const Snapshot& snapshot,
           uint64_t generation)
  {
    if (!generation || (worldGeneration && generation != worldGeneration))
      throw std::runtime_error(
        "Loadout operation has a stale world generation");
    if (operation || IsQuarantined())
      throw std::runtime_error(
        "An earlier loadout effect is still unresolved");
    if (!next.stamp || next.stamp != snapshot.stamp)
      throw std::runtime_error("Loadout operation has a stale snapshot");
    // Validate the operation against its immutable baseline before
    // publication.
    Observe(next, snapshot, snapshot);
    auto saved = snapshot;
    operation = next;
    before = std::move(saved);
    worldGeneration = generation;
    dispatched = false;
    lastObservationStamp = snapshot.stamp;
  }
  void MarkDispatched()
  {
    if (!operation || dispatched || IsQuarantined())
      throw std::runtime_error("Loadout effect may be dispatched only once");
    dispatched = true;
  }
  Observation ObserveFresh(const Snapshot& snapshot, uint64_t generation)
  {
    if (!generation || generation != worldGeneration)
      throw std::runtime_error(
        "Stale world observation cannot change the loadout barrier");
    if (!operation || !dispatched)
      throw std::runtime_error("No dispatched loadout effect to observe");
    if (IsQuarantined())
      return Observation::Unexpected;
    if (snapshot.stamp <= lastObservationStamp)
      return Observation::Pending;
    lastObservationStamp = snapshot.stamp;
    const auto result = Observe(*operation, *before, snapshot);
    if (result == Observation::Applied) {
      operation.reset();
      before.reset();
      dispatched = false;
    } else if (result == Observation::Unexpected) {
      Quarantine("Native loadout effect changed unexpected state");
    }
    return result;
  }
  void Quarantine(std::string reason)
  {
    if (reason.empty())
      reason = "Native loadout effect did not settle";
    if (error.empty())
      error = std::move(reason);
  }
  void InvalidateWorld(uint64_t generation)
  {
    if (!generation || generation < worldGeneration)
      throw std::runtime_error("World generations must increase");
    if (generation == worldGeneration)
      return;
    operation.reset();
    before.reset();
    error.clear();
    dispatched = false;
    lastObservationStamp = 0;
    worldGeneration = generation;
  }

private:
  uint64_t worldGeneration = 0, lastObservationStamp = 0;
  std::optional<Operation> operation;
  std::optional<Snapshot> before;
  bool dispatched = false;
  std::string error;
};

class SettlementEvidence
{
public:
  void Reset()
  {
    lastStamp = 0;
    samples = 0;
  }
  bool ObserveMatch(uint64_t stamp, bool matches)
  {
    if (!stamp || stamp < lastStamp)
      throw std::runtime_error("Stale loadout settlement observation");
    if (!matches) {
      samples = 0;
      lastStamp = stamp;
      return false;
    }
    if (stamp != lastStamp) {
      lastStamp = stamp;
      samples = std::min(samples + 1, 2u);
    }
    return samples >= 2;
  }

private:
  uint64_t lastStamp = 0;
  unsigned samples = 0;
};
}
