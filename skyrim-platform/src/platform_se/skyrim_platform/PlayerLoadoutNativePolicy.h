#pragma once
#include "PlayerLoadoutPlan.h"

// Engine adapter policies kept portable so regressions exercise the same
// selectors/counts used for real native calls, without mocking engine success.
namespace PlayerLoadoutNativePolicy {
using namespace PlayerLoadout;
inline bool HasMetadata(const Metadata& value)
{
  return value.health || value.charge || value.name || value.enchantmentId ||
    value.maxCharge || value.removeEnchantmentOnUnequip || value.soul ||
    value.poisonId || value.poisonCount;
}

inline uint32_t PlainAmmoCount(const Snapshot& snapshot, uint32_t baseId)
{
  uint64_t total = 0;
  for (const auto& stack : snapshot.stacks) {
    if (stack.item.baseId != baseId)
      continue;
    // Native ammo equip is base-selected, so even a competing unworn variant
    // makes it impossible to prove which instance's auxiliary data is worn.
    if (HasMetadata(stack.item.extras) || stack.unknownExtras ||
        stack.questProtected)
      throw std::runtime_error(
        "Ammunition equipment has ambiguous native variants");
    total += stack.item.count;
    if (!stack.item.count || total > kMaxCount)
      throw std::runtime_error("Ammunition equipment count is invalid");
  }
  return static_cast<uint32_t>(total);
}

inline uint32_t EquipCount(const Snapshot& snapshot,
                           const Operation& operation, bool ammo)
{
  if (!ammo)
    return 1;
  const auto count = PlainAmmoCount(snapshot, operation.item.baseId);
  if (!count)
    throw std::runtime_error("Ammunition equipment is absent");
  // SKSE PapyrusActor.cpp EquipItemEx and UnequipItemEx both pass the complete
  // itemCount for ammo. A planner gear operation represents one worn marker,
  // not a request to equip a single arrow or to change inventory quantities.
  return count;
}

inline void ValidateEquipSelections(const Plan& plan)
{
  std::map<uint64_t, unsigned> existing;
  std::map<std::pair<uint32_t, Metadata>, unsigned> future;
  for (const auto& operation : plan.preflightOperations) {
    if (operation.kind != OperationKind::Equip)
      continue;
    const auto count = operation.token
      ? ++existing[operation.token]
      : ++future[{ operation.item.baseId, operation.item.extras }];
    if (count > 1)
      throw std::runtime_error(
        "Multiple equipment copies require unverified native stack splitting");
  }
}
}
