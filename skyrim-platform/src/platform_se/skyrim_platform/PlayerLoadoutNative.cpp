#include "PlayerLoadoutNative.h"
#include "PlayerLoadoutNativePolicy.h"
#include <atomic>

namespace {
using namespace PlayerLoadout;
constexpr uint64_t kRightHand = uint64_t(1) << 32;
constexpr uint64_t kLeftHand = uint64_t(1) << 33;
constexpr uint64_t kAmmo = uint64_t(1) << 34;
std::atomic<uint64_t> nextSnapshot{ 0 };

RE::BGSEquipSlot* DefaultSlot(size_t index)
{
  auto manager = RE::BGSDefaultObjectManager::GetSingleton();
  return manager ? manager->GetObject<RE::BGSEquipSlot>(index) : nullptr;
}
bool UsesBothHands(const RE::BGSEquipSlot* slot)
{
  return slot && slot->flags.any(RE::BGSEquipSlot::Flag::kUseAllParents);
}

RE::TESBoundObject* Bound(uint32_t id)
{
  auto form = RE::TESForm::LookupByID<RE::TESBoundObject>(id);
  if (!form)
    throw std::runtime_error("Loadout item base no longer exists");
  return form;
}

bool InventoryType(RE::FormType type)
{
  switch (type) {
    case RE::FormType::Armor:
    case RE::FormType::Weapon:
    case RE::FormType::Ammo:
    case RE::FormType::Misc:
    case RE::FormType::AlchemyItem:
    case RE::FormType::Ingredient:
    case RE::FormType::Book:
    case RE::FormType::KeyMaster:
    case RE::FormType::SoulGem:
    case RE::FormType::Scroll:
    case RE::FormType::Light:
    case RE::FormType::Apparatus:
      return true;
    default:
      return false;
  }
}

BaseInfo Base(RE::TESBoundObject* form)
{
  BaseInfo result;
  result.name = form->GetName() ? form->GetName() : "";
  result.canAdd = InventoryType(form->GetFormType());
  if (auto armor = form->As<RE::TESObjectARMO>()) {
    result.rightAllowed = true;
    result.rightSlots = static_cast<uint32_t>(armor->GetSlotMask());
    if (armor->IsShield())
      result.rightSlots |= kLeftHand;
  } else if (auto weapon = form->As<RE::TESObjectWEAP>()) {
    const auto equip = weapon->GetEquipSlot();
    const bool both = UsesBothHands(equip);
    const bool either = equip && equip == DefaultSlot(21);
    const bool single = equip &&
      (either || equip == DefaultSlot(19) || equip == DefaultSlot(20));
    const auto race = RE::PlayerCharacter::GetSingleton()->GetRace();
    const bool dual =
      race && race->data.flags.any(RE::RACE_DATA::Flag::kCanDualWield);
    result.rightAllowed = both || single;
    result.rightSlots = both ? kRightHand | kLeftHand : kRightHand;
    result.leftAllowed = !both && single && (either || dual);
    result.leftSlots = kLeftHand;
  } else if (form->GetFormType() == RE::FormType::Ammo) {
    result.rightAllowed = true;
    result.rightSlots = kAmmo;
  } else if (form->GetFormType() == RE::FormType::Light) {
    result.rightAllowed = true;
    result.rightSlots = kLeftHand;
  }
  return result;
}

void ReadExtras(RE::ExtraDataList* list, Stack& stack)
{
  if (!list)
    return;
  RE::BSReadLockGuard lock(list->GetLock());
  std::set<RE::ExtraDataType> seen;
  for (const auto& extra : *list) {
    const auto type = extra.GetType();
    if (!seen.insert(type).second)
      throw std::runtime_error("Duplicate native inventory extra record");
    switch (type) {
      case RE::ExtraDataType::kCount:
        break;
      case RE::ExtraDataType::kWorn:
        stack.right = true;
        break;
      case RE::ExtraDataType::kWornLeft:
        stack.left = true;
        break;
      case RE::ExtraDataType::kCannotWear:
        stack.canEquip = stack.canUnequip = false;
        stack.unknownExtras = true;
        break;
      case RE::ExtraDataType::kHealth:
        stack.item.extras.health =
          static_cast<const RE::ExtraHealth&>(extra).health;
        break;
      case RE::ExtraDataType::kCharge:
        stack.item.extras.charge =
          static_cast<const RE::ExtraCharge&>(extra).charge;
        break;
      case RE::ExtraDataType::kEnchantment: {
        const auto& value = static_cast<const RE::ExtraEnchantment&>(extra);
        if (!value.enchantment)
          throw std::runtime_error("Native enchantment is missing");
        stack.item.extras.enchantmentId = value.enchantment->GetFormID();
        stack.item.extras.maxCharge = value.charge;
        stack.item.extras.removeEnchantmentOnUnequip = value.removeOnUnequip;
        break;
      }
      case RE::ExtraDataType::kSoul:
        stack.item.extras.soul = static_cast<uint32_t>(
          static_cast<const RE::ExtraSoul&>(extra).GetContainedSoul());
        break;
      case RE::ExtraDataType::kPoison: {
        const auto& value = static_cast<const RE::ExtraPoison&>(extra);
        if (!value.poison)
          throw std::runtime_error("Native poison is missing");
        stack.item.extras.poisonId = value.poison->GetFormID();
        stack.item.extras.poisonCount = value.count;
        break;
      }
      case RE::ExtraDataType::kTextDisplayData: {
        const auto& value =
          static_cast<const RE::ExtraTextDisplayData&>(extra);
        stack.item.extras.name = value.displayName.c_str();
        // Name ownership can carry quest semantics beyond the display string.
        if (value.ownerQuest || value.displayNameText)
          stack.unknownExtras = true;
        break;
      }
      default:
        stack.unknownExtras = true;
        break;
    }
  }
}

bool HasMetadata(const Metadata& value)
{
  return PlayerLoadoutNativePolicy::HasMetadata(value);
}

void ValidateMetadataForms(const Item& item)
{
  const auto& extras = item.extras;
  if (extras.enchantmentId &&
      !RE::TESForm::LookupByID<RE::EnchantmentItem>(*extras.enchantmentId))
    throw std::runtime_error("Loadout enchantment form is unavailable");
  if (extras.poisonId &&
      !RE::TESForm::LookupByID<RE::AlchemyItem>(*extras.poisonId))
    throw std::runtime_error("Loadout poison form is unavailable");
}

struct ExtraListDeleter
{
  void operator()(RE::ExtraDataList* list) const
  {
    if (!list)
      return;
    auto extra = list->_extraData.GetData();
    while (extra) {
      auto next = extra->next;
      delete extra;
      extra = next;
    }
    RE::free(list->_extraData.GetPresence());
    RE::free(list);
  }
};
using OwnedExtras = std::unique_ptr<RE::ExtraDataList, ExtraListDeleter>;

void InitializeExtraList(RE::ExtraDataList& list)
{
  // CommonLib declares TESObjectREFR::extraList as an embedded ExtraDataList,
  // whose first member is an embedded BaseExtraList. It cannot be a subclass.
  // In AE >=1.6.629 that exact embedded type has a virtual destructor. Its
  // runtime accessors expose data at08/presence10/lock18; older layouts use
  // data00/presence08/lock10. No engine list-constructor binding is available.
  const auto offset = reinterpret_cast<uintptr_t>(&list._extraData.GetData()) -
    reinterpret_cast<uintptr_t>(&list);
  if (offset == 0)
    return;
  if (offset != sizeof(uintptr_t))
    throw std::runtime_error("Unsupported native extra-list layout");
  auto player = RE::PlayerCharacter::GetSingleton();
  if (!player)
    throw std::runtime_error("Native extra-list prototype is unavailable");
  const auto& prototype = player->extraList._extraData;
  uintptr_t vtable = 0;
  std::memcpy(&vtable, &prototype, sizeof(vtable));
  if (!vtable)
    throw std::runtime_error("Native extra-list vtable is unavailable");
  // Copy ONLY the exact type's vtable. Data, presence and lock remain new and
  // empty. This allocation still requires real AE runtime acceptance.
  std::memcpy(&list, &vtable, sizeof(vtable));
}

OwnedExtras MakeExtras(const Item& item)
{
  if (!HasMetadata(item.extras))
    return {};
  // ExtraDataList's runtime layout differs across SE/AE. Match the existing
  // platform allocation (24-byte BaseExtraList plus 8-byte lock) and use its
  // runtime accessors rather than sizeof on the compile-time AE shell type.
  auto memory = RE::malloc(32);
  if (!memory)
    throw std::bad_alloc();
  std::memset(memory, 0, 32);
  OwnedExtras result(static_cast<RE::ExtraDataList*>(memory));
  InitializeExtraList(*result);
  auto presence = RE::malloc<RE::BaseExtraList::PresenceBitfield>();
  if (!presence)
    throw std::bad_alloc();
  std::memset(presence, 0, sizeof(*presence));
  result->_extraData.GetPresence() = presence;
  const auto add = [&](RE::BSExtraData* raw) {
    std::unique_ptr<RE::BSExtraData> extra(raw);
    if (!raw)
      throw std::bad_alloc();
    // The list is unpublished and all represented types are unique. Link it
    // directly using the documented runtime accessors (as the platform's
    // existing writer does), so ownership transfer is unambiguous on failure.
    const auto type = static_cast<uint32_t>(raw->GetType());
    if (type >= 0x18 * 8 || result->HasType(raw->GetType()))
      throw std::runtime_error("Invalid native loadout metadata type");
    raw->next = result->_extraData.GetData();
    result->_extraData.GetData() = raw;
    result->_extraData.GetPresence()->MarkType(type, false);
    extra.release();
  };
  const auto& x = item.extras;
  if (x.health)
    add(new RE::ExtraHealth(*x.health));
  if (x.enchantmentId)
    add(new RE::ExtraEnchantment(
      RE::TESForm::LookupByID<RE::EnchantmentItem>(*x.enchantmentId),
      static_cast<uint16_t>(*x.maxCharge),
      x.removeEnchantmentOnUnequip.value_or(false)));
  if (x.charge) {
    auto extra = new RE::ExtraCharge();
    extra->charge = *x.charge;
    add(extra);
  }
  if (x.name)
    add(new RE::ExtraTextDisplayData(x.name->c_str()));
  if (x.soul)
    add(new RE::ExtraSoul(static_cast<RE::SOUL_LEVEL>(*x.soul)));
  if (x.poisonId)
    add(new RE::ExtraPoison(
      RE::TESForm::LookupByID<RE::AlchemyItem>(*x.poisonId), *x.poisonCount));
  if (item.count > 1) {
    if (item.count > 32767)
      throw std::runtime_error(
        "Native extra stack exceeds signed count range");
    add(new RE::ExtraCount(static_cast<int16_t>(item.count)));
  }
  return result;
}

const RE::BGSEquipSlot* Slot(RE::TESBoundObject* base, bool left)
{
  if (base->GetFormType() != RE::FormType::Weapon)
    return nullptr;
  auto weapon = base->As<RE::TESObjectWEAP>();
  if (UsesBothHands(weapon->GetEquipSlot()))
    return weapon->GetEquipSlot();
  return DefaultSlot(left ? 19 : 20);
}

SpellInfo Spell(uint32_t id, const std::array<uint32_t, 4>& observed)
{
  auto spell = RE::TESForm::LookupByID<RE::SpellItem>(id);
  if (!spell)
    throw std::runtime_error("Loadout spell form is unavailable");
  SpellInfo result;
  const auto equip = spell->GetEquipSlot();
  const bool both = spell->IsTwoHanded();
  for (uint32_t slot = 0; slot < 4; ++slot) {
    if (slot < 2) {
      result.allowed[slot] = equip &&
        (both || equip == DefaultSlot(21) ||
         equip == DefaultSlot(slot == 0 ? 19 : 20));
      result.slots[slot] = both ? kRightHand | kLeftHand
        : slot == 0             ? kLeftHand
                                : kRightHand;
      result.canChange[slot] = result.allowed[slot] && !both;
      // An already observed custom equip type may be retained, but its
      // unverified transition cannot authorize a builtin mutation.
      if (observed[slot] == id)
        result.allowed[slot] = true;
    } else {
      result.allowed[slot] = observed[slot] == id;
      result.slots[slot] = uint64_t(1) << (35 + slot);
    }
  }
  // Actor.psc:222-228 documents getters0Left/1Right/2Other/3Instant.
  // EquipSpell:120-124 and UnequipSpell:694-698 document mutation only0/1.
  // Retain exact2/3 and two-handed selections; their correlated mutation is
  // unsupported until modeled and accepted in the actual engine.
  return result;
}
}

bool PlayerLoadoutNative::Ready()
{
  auto player = RE::PlayerCharacter::GetSingleton();
  auto ui = RE::UI::GetSingleton();
  if (!Readable() || !ui || ui->GameIsPaused() ||
      player->GetOccupiedFurniture())
    return false;
  for (const char* menu :
       { "RaceSex Menu", "Loading Menu", "InventoryMenu", "FavoritesMenu",
         "MagicMenu", "ContainerMenu", "Crafting Menu" }) {
    if (ui->IsMenuOpen(menu))
      return false;
  }
  return true;
}

bool PlayerLoadoutNative::Readable()
{
  auto player = RE::PlayerCharacter::GetSingleton();
  return player && player->GetFormID() == 0x14 && player->Is3DLoaded();
}

PlayerLoadoutNative::Snapshot PlayerLoadoutNative::Read(
  const Target& target, const std::array<uint32_t, 4>& observedSpells)
{
  if (!Readable())
    throw std::runtime_error("Player is unavailable for loadout observation");
  Snapshot result;
  result.value.stamp = ++nextSnapshot;
  result.value.spells = observedSpells;
  auto player = RE::PlayerCharacter::GetSingleton();
  const auto inventory = player->GetInventory();
  uint64_t token = 0;
  for (const auto& [form, data] : inventory) {
    if (!form || data.first <= 0)
      continue;
    const auto id = form->GetFormID();
    result.value.bases.emplace(id, Base(form));
    const auto& entry = data.second;
    int64_t remaining = data.first;
    bool hasLists = false;
    if (entry && entry->extraLists) {
      for (auto list : *entry->extraLists) {
        if (!list)
          continue;
        hasLists = true;
        const auto count = list->GetCount();
        if (count <= 0 || count > remaining)
          throw std::runtime_error(
            "Native inventory extra counts are inconsistent");
        Stack stack;
        stack.token = ++token;
        stack.item.baseId = id;
        stack.item.count = count;
        // CommonLib InventoryEntryData::IsQuestObject is an any-list query.
        // Apply its actual alias predicate to this exact instance; otherwise
        // an ordinary added copy would spuriously inherit another copy's
        // alias.
        stack.questProtected = list->HasQuestObjectAlias();
        ReadExtras(list, stack);
        if (form->GetFormType() == RE::FormType::Ammo &&
            HasMetadata(stack.item.extras))
          stack.canEquip = stack.canUnequip = false;
        // A single extra list with both worn and spare units cannot select
        // the spare unit by pointer without a proven engine split operation.
        if (count > 1 && (stack.right || stack.left))
          stack.canEquip = false;
        result.bindings.emplace(stack.token, Binding{ list });
        result.value.stacks.push_back(std::move(stack));
        if (result.value.stacks.size() > kMaxStacks)
          throw std::runtime_error(
            "Native inventory exceeds loadout stack limit");
        remaining -= count;
      }
    }
    if (remaining) {
      Stack stack;
      stack.token = ++token;
      stack.item.baseId = id;
      stack.item.count = static_cast<uint32_t>(remaining);
      // nullptr removal/equip is base-selected. It is exact only when there
      // are no competing native extra lists for the same base.
      stack.canRemove = stack.canEquip = !hasLists;
      result.bindings.emplace(stack.token, Binding{});
      result.value.stacks.push_back(std::move(stack));
      if (result.value.stacks.size() > kMaxStacks)
        throw std::runtime_error(
          "Native inventory exceeds loadout stack limit");
    }
  }
  const auto include = [&](uint32_t id) {
    if (!result.value.bases.count(id))
      result.value.bases.emplace(id, Base(Bound(id)));
  };
  if (target.inventory)
    for (const auto& item : *target.inventory)
      include(item.baseId);
  if (target.equipment)
    for (const auto& item : target.equipment->items)
      include(item.baseId);
  const auto includeSpell = [&](uint32_t id) {
    if (id && !result.value.spellBases.count(id))
      result.value.spellBases.emplace(id, Spell(id, observedSpells));
  };
  for (auto id : observedSpells)
    includeSpell(id);
  if (target.equipment)
    for (auto id : target.equipment->spells)
      includeSpell(id);
  return result;
}

void PlayerLoadoutNative::Preflight(const Snapshot& snapshot,
                                    const Target& target, const Plan& plan)
{
  PlayerLoadoutNativePolicy::ValidateEquipSelections(plan);
  if (target.inventory)
    for (const auto& item : *target.inventory)
      ValidateMetadataForms(item);
  if (target.equipment) {
    for (const auto& item : target.equipment->items)
      ValidateMetadataForms(Item{ item.baseId, 1, item.extras });
    for (auto id : target.equipment->spells)
      if (id && !RE::TESForm::LookupByID<RE::SpellItem>(id))
        throw std::runtime_error("Loadout spell form is unavailable");
  }
  for (const auto& op : plan.preflightOperations) {
    if (op.kind == OperationKind::Spell)
      continue;
    auto base = Bound(op.item.baseId);
    ValidateMetadataForms(op.item);
    if (base->GetFormType() == RE::FormType::Ammo &&
        HasMetadata(op.item.extras))
      throw std::runtime_error(
        "Exact ammunition metadata mutation is unsupported");
    if ((op.kind == OperationKind::Equip ||
         op.kind == OperationKind::Unequip) &&
        base->GetFormType() == RE::FormType::Weapon && !Slot(base, op.left))
      throw std::runtime_error("Loadout equip slot is unavailable");
    if (op.token && !snapshot.bindings.count(op.token))
      throw std::runtime_error("Loadout operation lost its native instance");
    if (op.kind == OperationKind::Equip || op.kind == OperationKind::Unequip) {
      if (!RE::ActorEquipManager::GetSingleton())
        throw std::runtime_error("Native equip manager is unavailable");
      const bool ammo = base->GetFormType() == RE::FormType::Ammo;
      if (ammo)
        PlayerLoadoutNativePolicy::PlainAmmoCount(snapshot.value,
                                                  op.item.baseId);
      if (!ammo && !op.token && !HasMetadata(op.item.extras)) {
        // A future plain addition has no exact native list. If this base also
        // has variant lists, nullptr selection cannot prove which is equipped.
        for (const auto& stack : snapshot.value.stacks)
          if (stack.item.baseId == op.item.baseId &&
              snapshot.bindings.at(stack.token).list)
            throw std::runtime_error("Future plain equipment cannot be "
                                     "selected among native variants");
      }
    }
    if (op.kind == OperationKind::Add && HasMetadata(op.item.extras) &&
        target.inventory) {
      // ExtraCount is signed16. Until native merging/splitting is accepted,
      // do not add a metadata variant whose desired aggregate exceeds it.
      // Existing larger aggregates split across valid lists may be retained.
      for (const auto& desired : *target.inventory)
        if (desired.baseId == op.item.baseId &&
            desired.extras == op.item.extras && desired.count > 32767)
          throw std::runtime_error("Adding this metadata stack exceeds "
                                   "verified native count support");
    }
  }
}

void PlayerLoadoutNative::Apply(const Snapshot& snapshot, const Operation& op)
{
  if (op.stamp != snapshot.value.stamp)
    throw std::runtime_error("Stale loadout snapshot");
  auto player = RE::PlayerCharacter::GetSingleton();
  if (!Ready())
    throw std::runtime_error(
      "Player became unavailable before loadout mutation");
  auto base = Bound(op.item.baseId);
  if (op.kind == OperationKind::Add) {
    auto extras = MakeExtras(op.item);
    player->AddObjectToContainer(base, extras.release(),
                                 static_cast<int32_t>(op.item.count), nullptr);
    return;
  }
  const auto binding = snapshot.bindings.find(op.token);
  if (binding == snapshot.bindings.end())
    throw std::runtime_error("Loadout instance is unavailable");
  auto list = binding->second.list;
  if (op.kind == OperationKind::Remove) {
    player->RemoveItem(base, static_cast<int32_t>(op.item.count),
                       RE::ITEM_REMOVE_REASON::kRemove, list, nullptr);
    return;
  }
  auto manager = RE::ActorEquipManager::GetSingleton();
  if (!manager)
    throw std::runtime_error("Native equip manager is unavailable");
  const auto count = PlayerLoadoutNativePolicy::EquipCount(
    snapshot.value, op, base->GetFormType() == RE::FormType::Ammo);
  if (op.kind == OperationKind::Equip) {
    manager->EquipObject(player, base, list, count, Slot(base, op.left), false,
                         false, false, true);
  } else if (op.kind == OperationKind::Unequip) {
    manager->UnequipObject(player, base, list, count, Slot(base, op.left),
                           false, false, false, true);
  } else
    throw std::runtime_error("Unsupported native item operation");
}
