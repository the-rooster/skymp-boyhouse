import {
  Actor,
  Ammo,
  Game,
  ObjectReference,
  Spell,
  Ui,
  setInventory,
} from 'skyrimPlatform';

import { Entry, Inventory, getInventory, getEquipmentInventory, equipmentEntryExtrasEqual, assertInventoryCounts } from './inventory';

export const enum SpellType {
  Left,
  Right,
  Voice,
  Instant,
}

export const getEquipedSpell = (
  refr: ObjectReference,
  spellType: SpellType,
): number => {
  const actor = Actor.from(refr);

  if (!actor) {
    return 0;
  }

  switch (spellType) {
    case SpellType.Left: {
      const spell = actor.getEquippedSpell(SpellType.Left);
      return spell ? spell.getFormID() : 0;
    }
    case SpellType.Right: {
      const spell = actor.getEquippedSpell(SpellType.Right);
      return spell ? spell.getFormID() : 0;
    }
    case SpellType.Voice: {
      const spell = actor.getEquippedSpell(SpellType.Voice);
      return spell ? spell.getFormID() : 0;
    }
    case SpellType.Instant: {
      const spell = actor.getEquippedSpell(SpellType.Instant);
      return spell ? spell.getFormID() : 0;
    }
    default: {
      return 0;
    }
  }
};

export interface Equipment {
  inv: Inventory;
  leftSpell?: number;
  rightSpell?: number;
  voiceSpell?: number;
  instantSpell?: number;
  numChanges: number;
}

const filterWorn = (inv: Inventory): Inventory => {
  return { entries: inv.entries.filter((x) => x.worn || x.wornLeft) };
};

const removeUnnecessaryExtra = (inv: Inventory, ignoreAmmo: boolean): Inventory => {
  return {
    entries: inv.entries.map((x) => {
      const r: Entry = JSON.parse(JSON.stringify(x));
      r.chargePercent = r.maxCharge;
      if (ignoreAmmo) {
        r.count = Ammo.from(Game.getFormEx(x.baseId)) ? r.count : 1;
      } else {
        r.count = Ammo.from(Game.getFormEx(x.baseId)) ? 1000 : 1;
      }
      delete r.name;
      return r;
    }),
  };
};

export const getEquipment = (ac: Actor, numChanges: number): Equipment => {
  return {
    inv: ac.getFormID() === 0x14 ? getEquipmentInventory(ac) : getInventory(ac),
    leftSpell: getEquipedSpell(ac, SpellType.Left),
    rightSpell: getEquipedSpell(ac, SpellType.Right),
    voiceSpell: getEquipedSpell(ac, SpellType.Voice),
    instantSpell: getEquipedSpell(ac, SpellType.Instant),
    numChanges,
  };
};

export const syncSpellEquipment = (
  ac: Actor,
  spellBaseId: number | undefined,
  spellType: SpellType,
) => {
  if (spellBaseId !== undefined && spellBaseId > 0) {
    ac.equipSpell(Spell.from(Game.getFormEx(spellBaseId)), spellType);
  } else {
    const equipedSpell = ac.getEquippedSpell(spellType);

    if (equipedSpell) {
      ac.unequipSpell(equipedSpell, spellType);
    }
  }
};

export const applyEquipment = (ac: Actor, eq: Equipment): boolean => {
  ac.removeAllItems(null, false, true);

  ac.unequipAll();

  ac.removeAllItems(null, false, true);

  const newInventory = removeUnnecessaryExtra(filterWorn(eq.inv), ac.getFormID() === 0x14);

  setInventory(ac.getFormID(), newInventory);

  syncSpellEquipment(ac, eq.leftSpell, SpellType.Left);
  syncSpellEquipment(ac, eq.rightSpell, SpellType.Right);
  syncSpellEquipment(ac, eq.voiceSpell, SpellType.Voice);
  syncSpellEquipment(ac, eq.instantSpell, SpellType.Instant);

  return true;
};

const wornSlots = (inventory: Inventory): { item: Entry; left: boolean }[] => {
  const slots: { item: Entry; left: boolean }[] = [];
  for (const entry of inventory.entries) {
    if (entry.count <= 0) continue;
    if (entry.worn) slots.push({ item: entry, left: false });
    if (entry.wornLeft) slots.push({ item: entry, left: true });
  }
  return slots;
};

export const isPlayerEquipmentSettled = (ac: Actor, eq: Equipment): boolean => {
  assertInventoryCounts(eq.inv);
  const actual = wornSlots(getEquipmentInventory(ac)), expected = wornSlots(eq.inv);
  if (actual.length !== expected.length) return false;
  for (const wanted of expected) {
    const index = actual.findIndex(slot => slot.left === wanted.left &&
      slot.item.baseId === wanted.item.baseId && equipmentEntryExtrasEqual(slot.item, wanted.item));
    if (index < 0) return false;
    actual.splice(index, 1);
  }
  return [eq.leftSpell, eq.rightSpell, eq.voiceSpell, eq.instantSpell].every((spell, slot) =>
    getEquipedSpell(ac, slot) === (spell || 0));
};

// Self restoration follows an observed full-inventory restore. Never queue
// setInventory here: its additive game-thread task can race the inventory
// reconciler and duplicate items. Re-equipping existing items is retryable.
export const applyPlayerEquipment = (ac: Actor, eq: Equipment): void => {
  if (ac.getFormID() !== 0x14) throw Error('Player equipment requires the local player');
  if (isPlayerEquipmentSettled(ac, eq)) return;
  const inventory = getEquipmentInventory(ac);
  // unequipAll and even direct equip can remove another worn item's temporary
  // enchantment. Exact instance-preserving transitions need a native API.
  if (inventory.entries.some(item => item.count > 0 && (item.worn || item.wornLeft) && item.removeEnchantmentOnUnequip))
    throw Error('Player equipment cannot safely remove a worn temporary enchantment');
  for (const entry of filterWorn(eq.inv).entries) {
    const form = Game.getFormEx(entry.baseId);
    if (!form || ac.getItemCount(form) <= 0) throw Error('Equipped item is absent from the authoritative inventory');
    // EquipItemEx selects by base ID. With differing variants it can pick the
    // wrong enchanted/tempered/named item; refuse before changing any gear.
    const variants = inventory.entries.filter(item => item.baseId === entry.baseId && item.count > 0);
    if (!variants.length || variants.some(item => !equipmentEntryExtrasEqual(item, entry)))
      throw Error('Ambiguous equipment requires item-instance selection');
  }
  ac.unequipAll();
  for (const entry of filterWorn(eq.inv).entries) {
    const form = Game.getFormEx(entry.baseId);
    if (entry.worn) ac.equipItemEx(form, 1, false, false);
    if (entry.wornLeft) ac.equipItemEx(form, 2, false, false);
  }
  syncSpellEquipment(ac, eq.leftSpell, SpellType.Left);
  syncSpellEquipment(ac, eq.rightSpell, SpellType.Right);
  syncSpellEquipment(ac, eq.voiceSpell, SpellType.Voice);
  syncSpellEquipment(ac, eq.instantSpell, SpellType.Instant);
};

export const isBadMenuShown = (): boolean => {
  return (
    Ui.isMenuOpen('InventoryMenu') ||
    Ui.isMenuOpen('FavoritesMenu') ||
    Ui.isMenuOpen('MagicMenu') ||
    Ui.isMenuOpen('ContainerMenu') ||
    Ui.isMenuOpen('Crafting Menu') // Actually I don't think it causes crashes
  );
};
