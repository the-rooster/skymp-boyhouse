import {
  getExtraContainerChanges,
  getContainer,
  BaseExtraList,
  ExtraHealth,
  ObjectReference,
  TESModPlatform,
  Game,
  storage,
  ExtraCount,
  ExtraEnchantment,
  ExtraPoison,
  ExtraSoul,
  ExtraTextDisplayData,
  ExtraCharge,
  Enchantment,
  Potion,
  Actor,
  Ammo,
  printConsole,
  ActorBase,
  FormType,
  Form,
} from "skyrimPlatform";

export interface Extra {
  health?: number;
  enchantmentId?: number;
  maxCharge?: number;
  removeEnchantmentOnUnequip?: boolean;
  chargePercent?: number;
  name?: string;
  soul?: 0 | 1 | 2 | 3 | 4 | 5;
  poisonId?: number;
  poisonCount?: number;
  worn?: boolean;
  wornLeft?: boolean;
}

export interface BasicEntry {
  baseId: number;
  count: number;
}

export type Entry = BasicEntry & Extra;

export interface Inventory {
  entries: Entry[];
}

// 'loxsword (Legendary)' => 'loxsword'
const getRealName = (s?: string): string => {
  if (!s) {
    return s as string;
  }

  const arr = s.split(" ");
  if (arr.length && arr[arr.length - 1].match(/^\(.*\)$/)) {
    arr.pop();
  }
  return arr.join(" ");
};

// 'aaaaaaaaaaaaaaaa' => 'aaa...'
const cropName = (s?: string): string => {
  if (!s) {
    return s as string;
  }

  const max = 128;
  return s.length >= max
    ? s
      .split("")
      .filter((x, i) => i < max)
      .join("")
      .concat("...")
    : s;
};

const checkIfNameIsGeneratedByGame = (
  aStr: string,
  bStr: string,
  formName: string
) => {
  if (!aStr.length && bStr.startsWith(formName)) {
    const bEnding = bStr.substr(formName.length);
    if (bEnding.match(/^\s\(.*\)$/)) {
      return true;
    }
  }
  return false;
};

const namesEqual = (a: Entry, b: Entry): boolean => {
  const aStr = a.name || "";
  const bStr = b.name || "";
  if (cropName(getRealName(aStr)) === cropName(getRealName(bStr))) {
    return true;
  }

  if (a.baseId === b.baseId) {
    const form = Game.getFormEx(a.baseId);
    if (form) {
      const formName = form.getName();
      if (
        checkIfNameIsGeneratedByGame(aStr, bStr, formName) ||
        checkIfNameIsGeneratedByGame(bStr, aStr, formName)
      )
        return true;
    }
  }

  return false;
};

const extrasEqual = (a: Entry, b: Entry, ignoreWorn = false) => {
  return (
    a.health === b.health &&
    a.enchantmentId === b.enchantmentId &&
    a.maxCharge === b.maxCharge &&
    !!a.removeEnchantmentOnUnequip === !!b.removeEnchantmentOnUnequip &&
    //a.chargePercent === b.chargePercent &&
    //namesEqual(a, b) &&
    a.soul === b.soul &&
    a.poisonId === b.poisonId &&
    a.poisonCount === b.poisonCount &&
    ((!!a.worn === !!b.worn && !!a.wornLeft === !!b.wornLeft) || ignoreWorn)
  );
};

// Equipment must identify the actual worn variant, not just its base form.
// Legacy general inventory comparisons ignore charge/name differences, so
// their looser comparison is insufficient to certify a self item instance.
export const equipmentEntryExtrasEqual = (a: Entry, b: Entry): boolean => {
  const actualName = (entry: Entry) => entry.name || Game.getFormEx(entry.baseId)?.getName() || '';
  const floatEqual = (left: number, right: number) => Number.isFinite(left) && Number.isFinite(right) &&
    Number.isFinite(Math.fround(left)) && Math.fround(left) === Math.fround(right);
  return floatEqual(a.health === undefined ? 1 : a.health, b.health === undefined ? 1 : b.health) &&
    a.enchantmentId === b.enchantmentId && a.maxCharge === b.maxCharge &&
    !!a.removeEnchantmentOnUnequip === !!b.removeEnchantmentOnUnequip &&
    a.soul === b.soul && a.poisonId === b.poisonId && a.poisonCount === b.poisonCount &&
    ((a.name || '') === (b.name || '') || actualName(a) === actualName(b)) &&
    (a.chargePercent === undefined ? b.chargePercent === undefined :
      b.chargePercent !== undefined && floatEqual(a.chargePercent, b.chargePercent));
};

export const hasExtras = (e: Entry): boolean => {
  return !extrasEqual(e, { baseId: 0, count: 0 });
};

const extractExtraData = (
  refr: ObjectReference,
  extraList: BaseExtraList | null,
  out: Entry,
  preserveEquipmentIdentity = false,
): void => {
  // I see that ExtraWorn is not emitted for 0xFF actors when arrows are equipped. Fixing
  const item = Game.getFormEx(out.baseId);
  if (Ammo.from(item)) {
    const actor = Actor.from(refr);
    if (actor && actor.isEquipped(item)) {
      out.worn = true;
    }
  }

  (extraList || []).forEach((extra) => {
    switch (extra.type) {
      case "Health":
        out.health = preserveEquipmentIdentity ? Math.fround((extra as ExtraHealth).health) :
          Math.round((extra as ExtraHealth).health * 10) / 10;

        // No ExtraHealth means the default multiplier of exactly one.
        if (out.health === 1) {
          delete out.health;
        }
        break;
      case "Count":
        out.count = (extra as ExtraCount).count;
        break;
      case "Enchantment":
        if (preserveEquipmentIdentity && typeof (extra as ExtraEnchantment).removeOnUnequip !== 'boolean')
          throw Error('Native inventory reader does not expose enchantment removal metadata');
        out.enchantmentId = (extra as ExtraEnchantment).enchantmentId;
        out.maxCharge = (extra as ExtraEnchantment).maxCharge;
        out.removeEnchantmentOnUnequip = (
          extra as ExtraEnchantment
        ).removeOnUnequip;
        break;
      case "Charge":
        out.chargePercent = (extra as ExtraCharge).charge;
        break;
      case "Poison":
        out.poisonId = (extra as ExtraPoison).poisonId;
        out.poisonCount = (extra as ExtraPoison).count;
        break;
      case "Soul":
        out.soul = (extra as ExtraSoul).soul;
        if (preserveEquipmentIdentity && (!Number.isInteger(out.soul) || out.soul < 0 || out.soul > 5))
          throw Error('Native inventory reader returned an invalid contained soul');
        break;
      case "TextDisplayData":
        out.name = (extra as ExtraTextDisplayData).name;
        break;
      case "Worn":
        out.worn = true;
        break;
      case "WornLeft":
        out.wornLeft = true;
        break;
    }
  });
};

const squash = (inv: Inventory, preserveEquipmentIdentity = false): Inventory => {
  const res = new Array<Entry>();
  inv.entries.forEach((e) => {
    const same = res.find((x) => e.baseId === x.baseId && (preserveEquipmentIdentity ?
      equipmentEntryExtrasEqual(x, e) && !!x.worn === !!e.worn && !!x.wornLeft === !!e.wornLeft : extrasEqual(x, e)));
    if (same) {
      same.count += e.count;
    } else {
      res.push(JSON.parse(JSON.stringify(e)));
    }
  });
  return { entries: res.filter((x) => x.count !== 0) };
};

const getExtraContainerChangesAsInventory = (
  refr: ObjectReference,
  preserveEquipmentIdentity = false,
): Inventory => {
  const extraContainerChanges = getExtraContainerChanges(refr.getFormID());
  const entries = new Array<Entry>();

  (extraContainerChanges || []).forEach((changesEntry) => {
    const entry: Entry = {
      baseId: changesEntry.baseId,
      count: changesEntry.countDelta,
    };

    (changesEntry.extendDataList || []).forEach((extraList) => {
      const e: Entry = {
        baseId: entry.baseId,
        count: 1,
      };
      extractExtraData(refr, extraList, e, preserveEquipmentIdentity);
      entries.push(e);
      entry.count -= e.count;
    });

    if (entry.count !== 0) {
      entries.push(entry);
    }
  });

  let res: Inventory = { entries };
  res = squash(res, preserveEquipmentIdentity);
  return res;
};

const getBaseContainerAsInventory = (refr: ObjectReference): Inventory => {
  return {
    entries: getContainer((refr.getBaseObject() as ActorBase).getFormID()),
  };
};

export const sumInventories = (lhs: Inventory, rhs: Inventory, preserveEquipmentIdentity = false): Inventory => {
  const detailed = (entry: Entry) => hasExtras(entry) || (preserveEquipmentIdentity &&
    !equipmentEntryExtrasEqual(entry, { baseId: entry.baseId, count: 0 }));
  const leftEntriesWithExtras = lhs.entries.filter(detailed);
  const rightEntriesWithExtras = rhs.entries.filter(detailed);
  const leftEntriesSimple = lhs.entries.filter(e => !detailed(e));
  const rightEntriesSimple = rhs.entries.filter(e => !detailed(e));

  leftEntriesSimple.forEach((e) => {
    const matching = rightEntriesSimple.find((x) => x.baseId === e.baseId);
    if (matching) {
      e.count += matching.count;
      matching.count = 0;
    }
  });

  return {
    entries: leftEntriesWithExtras
      .concat(rightEntriesWithExtras)
      .concat(leftEntriesSimple)
      .concat(rightEntriesSimple)
      .filter((e) => e.count !== 0),
  };
};

export const removeSimpleItemsAsManyAsPossible = (
  inv: Inventory,
  baseId: number,
  count: number
): Inventory => {
  const res: Inventory = { entries: [] };
  res.entries = JSON.parse(JSON.stringify(inv.entries));

  const entry = res.entries.find((e) => !hasExtras(e) && e.baseId === baseId);
  if (entry) {
    entry.count -= count;
  }

  res.entries = res.entries.filter((e) => e.count > 0);
  return res;
};

export const getDiff = (
  lhs: Inventory,
  rhs: Inventory,
  ignoreWorn: boolean,
  preserveEquipmentIdentity = false,
): Inventory => {
  const copy = (inventory: Inventory): Inventory => {
    const result: Inventory = JSON.parse(JSON.stringify(inventory));
    if (!preserveEquipmentIdentity) return result;
    if (ignoreWorn) result.entries.forEach(entry => { delete entry.worn; delete entry.wornLeft; });
    return squash(result, true);
  };
  const lhsCopy = copy(lhs);
  const rhsCopy = copy(rhs);

  rhsCopy.entries.forEach((e) => {
    const sameFromLeft = lhsCopy.entries.find(
      (x) => x.baseId === e.baseId && (preserveEquipmentIdentity ? equipmentEntryExtrasEqual(x, e) &&
        (ignoreWorn || (!!x.worn === !!e.worn && !!x.wornLeft === !!e.wornLeft)) : extrasEqual(x, e, ignoreWorn))
    );
    if (sameFromLeft) {
      sameFromLeft.count -= e.count;
    } else {
      lhsCopy.entries.push(e);
      lhsCopy.entries[lhsCopy.entries.length - 1].count *= -1;
    }
  });

  return { entries: lhsCopy.entries.filter((x) => x.count !== 0) };
};

export const getInventory = (refr: ObjectReference): Inventory => {
  return squash(
    sumInventories(
      getBaseContainerAsInventory(refr),
      getExtraContainerChangesAsInventory(refr)
    )
  );
};

// Equipment selection must retain variants that normal inventory sync merges
// (particularly display names and charges). Preserve identity at every merge,
// not just when comparing the already-squashed final array.
export const assertInventoryCounts = (inventory: Inventory): void => {
  const totals = new Map<number, number>();
  for (const entry of inventory.entries) {
    const total = (totals.get(entry.baseId) || 0) + entry.count;
    if (!Number.isInteger(entry.count) || entry.count < 0 || total > 0x7fffffff)
      throw Error('Player inventory contains an invalid item count');
    totals.set(entry.baseId, total);
  }
};

export const getEquipmentInventory = (refr: ObjectReference): Inventory => {
  const inventory = squash(sumInventories(getBaseContainerAsInventory(refr),
    getExtraContainerChangesAsInventory(refr, true), true), true);
  assertInventoryCounts(inventory);
  return inventory;
};

// Read back native state; applyInventory's boolean only reports dispatch.
// Self readiness preserves represented instance metadata. Worn state belongs
// to the separate equipment snapshot; count grouping is immaterial.
export const isInventorySettled = (refr: ObjectReference, expected: Inventory): boolean => {
  assertInventoryCounts(expected);
  return getDiff(expected, getEquipmentInventory(refr), true, true).entries.length === 0;
};

const basesReset = (): Set<number> => {
  if (storage["basesResetExists"] !== true) {
    storage["basesResetExists"] = true;
    storage["basesReset"] = new Set<number>();
  }
  return storage["basesReset"] as Set<number>;
};

const resetBase = (refr: ObjectReference): void => {
  const base = refr.getBaseObject();
  const baseId = base ? base.getFormID() : 0;
  if (!basesReset().has(baseId)) {
    basesReset().add(baseId);
    TESModPlatform.resetContainer(base);

    refr.removeAllItems(null, false, true);
  }
};

// The legacy writer cannot select arbitrary existing item instances or encode
// every Extra value. Refuse those restores before resetBase can erase anything.
// An exact observation needs no writer, including an existing depleted weapon.
const preflightPlayerInventory = (refr: ObjectReference, expected: Inventory): boolean => {
  assertInventoryCounts(expected);
  const actual = getEquipmentInventory(refr);
  const strictDiff = getDiff(expected, actual, true, true);
  if (!strictDiff.entries.length) return true;
  const refuse = () => { throw Error('Player inventory requires unsupported native item-instance restoration'); };
  if (actual.entries.some(entry => entry.count > 0 && (entry.worn || entry.wornLeft) && entry.removeEnchantmentOnUnequip))
    refuse();
  const float32 = (value: number) => Number.isFinite(value) && Number.isFinite(Math.fround(value));
  for (const entry of expected.entries) {
    if (!Number.isInteger(entry.count) || entry.count < 0 || entry.count > 0x7fffffff) refuse();
    if (!entry.count) continue;
    const form = Game.getFormEx(entry.baseId);
    if (!form) refuse();
    const type = form!.getType();
    if (entry.health !== undefined && (!float32(entry.health) || Math.fround(entry.health) < 1)) refuse();
    // Absence means full charge on an enchanted weapon. AddItemEx's zero
    // argument omits ExtraCharge, so explicit zero cannot be written by it.
    if (entry.chargePercent !== undefined && (!float32(entry.chargePercent) || Math.fround(entry.chargePercent) <= 0)) refuse();
    if (entry.name !== undefined && (typeof entry.name !== 'string' || entry.name.length >= 128 || entry.name.includes('\0'))) refuse();
    if (entry.enchantmentId !== undefined) {
      if (!Enchantment.from(Game.getFormEx(entry.enchantmentId)) || !Number.isInteger(entry.maxCharge) ||
          entry.maxCharge! < 0 || entry.maxCharge! > 0xffff) refuse();
    } else if (entry.maxCharge !== undefined || entry.removeEnchantmentOnUnequip) refuse();
    if (entry.soul !== undefined && (!Number.isInteger(entry.soul) || entry.soul < 1 || entry.soul > 5)) refuse();
    if (entry.poisonId !== undefined) {
      if (!Potion.from(Game.getFormEx(entry.poisonId)) || !Number.isInteger(entry.poisonCount) ||
          entry.poisonCount! < 0 || entry.poisonCount! > 0x7fffffff) refuse();
    } else if (entry.poisonCount !== undefined) refuse();
    if (Ammo.from(form)) {
      if (!equipmentEntryExtrasEqual(entry, { baseId: entry.baseId, count: entry.count })) refuse();
    } else if (entry.count > 1000 && type !== FormType.Misc && type !== FormType.Potion && type !== FormType.Ingredient) refuse();
  }
  const legacyDiff = getDiff(expected, getInventory(refr), true);
  if (getDiff(strictDiff, legacyDiff, true, true).entries.length) refuse();
  for (const entry of strictDiff.entries.filter(entry => entry.count < 0)) {
    const variants = actual.entries.filter(item => item.baseId === entry.baseId && item.count > 0);
    if (variants.some(item => !equipmentEntryExtrasEqual(item, { baseId: item.baseId, count: item.count }))) refuse();
    const form = Game.getFormEx(entry.baseId);
    const type = form?.getType();
    if (-entry.count > 1000 && type !== FormType.Misc && type !== FormType.Potion && type !== FormType.Ingredient) refuse();
  }
  return false;
};

export const applyInventory = (
  refr: ObjectReference,
  newInventory: Inventory,
  enableCrashProtection: boolean,
  ignoreWorn = false
): boolean => {
  if (refr.getFormID() === 0x14 && preflightPlayerInventory(refr, newInventory)) return true;
  resetBase(refr);
  const diff = getDiff(newInventory, getInventory(refr), ignoreWorn).entries;

  let res = true;

  diff.sort((a, b) => (a.count < b.count ? -1 : 1));
  diff.forEach((e, i) => {
    if (i > 0 && enableCrashProtection) {
      res = false;
      return;
    }
    let absCount = Math.abs(e.count);

    let queueNiNodeUpdateNeeded = false;

    const worn = !!e.worn;
    const wornLeft = !!e.wornLeft;

    let oneStepCount = e.count / absCount;

    const f = Game.getFormEx(e.baseId);
    if (!f) {
      return printConsole(`Bad form ID ${e.baseId.toString(16)}`);
    }
    const type = f.getType();
    // For misc items, potions and ingredients we don't want to split them into multiple items
    // This was made to fix a performance issue with users having 10000+ of misc items (i.e. gold)
    if (
      type === FormType.Misc ||
      type === FormType.Potion ||
      type === FormType.Ingredient
    ) {
      absCount = 1;
      oneStepCount = e.count;
    } else {
      if (absCount > 1000) {
        absCount = 1;
        oneStepCount = 1;

        // Also for arrows with strange count
        if (worn && e.count < 0) {
          absCount = 0;
        }
      }

      if (e.count > 1 && Ammo.from(Game.getFormEx(e.baseId))) {
        absCount = 1;
        oneStepCount = e.count;
        if (e.count > 60000) {
          // Why would actor have 60k arrows?
          e.count = 1;
        }
      }
    }

    for (let i = 0; i < absCount; ++i) {
      if (worn || wornLeft) {
        TESModPlatform.pushWornState(!!worn, !!wornLeft);
        queueNiNodeUpdateNeeded = true;
      }

      let addItemExArgs: [
        ObjectReference,
        Form,
        number,
        number,
        Enchantment | null,
        number,
        boolean,
        number,
        string,
        number,
        Potion | null,
        number,
      ];

      addItemExArgs = [
        refr,
        f,
        oneStepCount,
        e.health ? e.health : 1,
        e.enchantmentId
          ? Enchantment.from(Game.getFormEx(e.enchantmentId))
          : null,
        e.maxCharge ? e.maxCharge : 0,
        !!e.removeEnchantmentOnUnequip,
        e.chargePercent ? e.chargePercent : 0,
        e.name ? cropName(e.name) : f.getName(),
        e.soul ? e.soul : 0,
        e.poisonId ? Potion.from(Game.getFormEx(e.poisonId)) : null,
        e.poisonCount ? e.poisonCount : 0
      ];

      const argsToPrint = addItemExArgs.map((arg) => {
        if (arg instanceof ObjectReference) {
          return `ObjectReference(${arg.getFormID().toString(16)})`;
        } else if (arg instanceof Form) {
          return `Form(${arg.getFormID().toString(16)})`;
        } else {
          return JSON.stringify(arg);
        }
      });

      printConsole(
        `TESModPlatform.addItemEx(${argsToPrint.join(", ")})`
      );

      TESModPlatform.addItemEx(...addItemExArgs);
    }

    if (queueNiNodeUpdateNeeded) {
      const ac = Actor.from(refr);
      if (ac) {
        ac.queueNiNodeUpdate();
      }
    }
  });

  return res;
};
