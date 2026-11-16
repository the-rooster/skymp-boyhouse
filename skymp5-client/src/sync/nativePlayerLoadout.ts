import { Entry, Inventory } from './inventory';
import { Equipment } from './equipment';

export interface NativePlayerLoadoutTarget {
  revision: number;
  inventory?: Inventory;
  equipment?: Omit<Equipment, 'numChanges'>;
}

export interface NativePlayerLoadoutStatus {
  epoch: string;
  revision?: number;
  status: 'idle' | 'queued' | 'working' | 'observing' | 'settled' | 'failed' | 'superseded';
  error?: string;
  mutationPending: boolean;
  uncertain: boolean;
}

export interface NativePlayerLoadoutApi {
  playerLoadoutApiVersion: number;
  beginPlayerLoadout(): string;
  requestPlayerLoadout(epoch: string, target: NativePlayerLoadoutTarget): NativePlayerLoadoutStatus;
  getPlayerLoadoutStatus(epoch: string): NativePlayerLoadoutStatus;
  cancelPlayerLoadout(epoch: string): void;
}

export const requirePlayerLoadoutApi = (platform: unknown): NativePlayerLoadoutApi => {
  const api = platform as NativePlayerLoadoutApi;
  if (api.playerLoadoutApiVersion !== 1 || ['beginPlayerLoadout', 'requestPlayerLoadout',
    'getPlayerLoadoutStatus', 'cancelPlayerLoadout'].some(name => typeof (api as any)[name] !== 'function'))
    throw Error('Updated Skyrim Platform client required: native player loadout API version 1 is missing');
  return api;
};

export const checkPlayerLoadoutStatus = (value: NativePlayerLoadoutStatus, epoch: string, revision: number): NativePlayerLoadoutStatus => {
  checkPlayerLoadoutDrain(value, epoch);
  if (!value || value.epoch !== epoch || !['queued', 'working', 'observing', 'settled', 'failed', 'superseded'].includes(value.status))
    throw Error('Invalid native player loadout status');
  if (value.status === 'superseded') throw Error('Native player loadout epoch was superseded; a fresh player baseline is required');
  if (value.status === 'failed') throw Error(value.error || 'Native player loadout restoration failed');
  if (value.revision !== revision) throw Error('Native player loadout revision does not match the current snapshot');
  return value;
};

export const checkPlayerLoadoutDrain = (value: NativePlayerLoadoutStatus, epoch: string): boolean => {
  if (!value || value.epoch !== epoch || typeof value.mutationPending !== 'boolean' || typeof value.uncertain !== 'boolean')
    throw Error('Invalid native player loadout mutation barrier');
  if (value.uncertain) throw Error(value.error || 'Native player loadout mutation completion is uncertain');
  return !value.mutationPending;
};

const invalid = (field: string): never => { throw Error('Invalid native player loadout ' + field); };
const object = (value: any, fields: string[]): void => {
  if (!value || typeof value !== 'object' || Array.isArray(value)) invalid('object');
  if (Object.keys(value).some(key => !fields.includes(key))) invalid('field');
};
const integer = (value: any, maximum: number, minimum = 0): number => {
  if (!Number.isSafeInteger(value) || value < minimum || value > maximum) invalid('integer');
  return value;
};
const nativeFloat = (value: any): number => {
  if (typeof value !== 'number' || !Number.isFinite(value) || value < 0 || value > 3.4028234663852886e38 ||
      !Number.isFinite(Math.fround(value)) || (value !== 0 && Math.fround(value) === 0)) invalid('float');
  return Math.fround(value);
};
const utf8Length = (value: string): number => {
  let size = 0;
  for (let i = 0; i < value.length; ++i) {
    const code = value.charCodeAt(i), next = value.charCodeAt(i + 1);
    if (code >= 0xd800 && code <= 0xdbff && next >= 0xdc00 && next <= 0xdfff) { size += 4; ++i; }
    else size += code < 0x80 ? 1 : code < 0x800 ? 2 : 3;
  }
  return size;
};
const entryFields = ['baseId', 'count', 'health', 'chargePercent', 'name', 'enchantmentId',
  'maxCharge', 'removeEnchantmentOnUnequip', 'soul', 'poisonId', 'poisonCount', 'worn', 'wornLeft'];
const spellFields = ['leftSpell', 'rightSpell', 'voiceSpell', 'instantSpell'] as const;
const sortedJson = (value: any): string => {
  const canonical = (item: any): any => Array.isArray(item) ? item.map(canonical) :
    item && typeof item === 'object' ? Object.keys(item).sort().reduce((result, key) => {
      result[key] = canonical(item[key]); return result;
    }, {} as Record<string, unknown>) : item;
  return JSON.stringify(canonical(value));
};

// This validates the native request's represented fields before aggregation.
// It performs no engine lookups: default base names and slot/form validity are
// authoritative native checks. A zero charge remains different from absence.
const entryIdentity = (entry: Entry): Record<string, unknown> => {
  object(entry, entryFields);
  const identity: Record<string, unknown> = { baseId: integer(entry.baseId, 0xffffffff, 1) };
  integer(entry.count, 0x7fffffff, 1);
  for (const flag of ['worn', 'wornLeft', 'removeEnchantmentOnUnequip'] as const)
    if (entry[flag] !== undefined && typeof entry[flag] !== 'boolean') invalid(flag);
  if (entry.health !== undefined) {
    const value = nativeFloat(entry.health);
    if (value !== 1) identity.health = value;
  }
  if (entry.chargePercent !== undefined) identity.chargePercent = nativeFloat(entry.chargePercent);
  if (entry.name !== undefined) {
    if (typeof entry.name !== 'string' || entry.name.includes('\0') || utf8Length(entry.name) > 1024) invalid('name');
    if (entry.name) identity.name = entry.name;
  }
  if (entry.enchantmentId !== undefined) {
    identity.enchantmentId = integer(entry.enchantmentId, 0xffffffff, 1);
    identity.maxCharge = integer(entry.maxCharge, 0xffff);
    identity.removeEnchantmentOnUnequip = entry.removeEnchantmentOnUnequip === true;
  } else if (entry.maxCharge !== undefined || entry.removeEnchantmentOnUnequip !== undefined) invalid('orphan enchantment');
  if (entry.soul !== undefined) identity.soul = integer(entry.soul, 5);
  if (entry.poisonId !== undefined) {
    identity.poisonId = integer(entry.poisonId, 0xffffffff, 1);
    identity.poisonCount = integer(entry.poisonCount, 0x7fffffff);
  } else if (entry.poisonCount !== undefined) invalid('orphan poison');
  return identity;
};

const entries = (inventory: Inventory): { entry: Entry; identity: Record<string, unknown> }[] => {
  object(inventory, ['entries']);
  if (!Array.isArray(inventory.entries) || inventory.entries.length > 4096) invalid('entries');
  const result: { entry: Entry; identity: Record<string, unknown> }[] = [];
  for (let i = 0; i < inventory.entries.length; ++i) {
    if (!Object.prototype.hasOwnProperty.call(inventory.entries, i)) invalid('sparse entries');
    const entry = inventory.entries[i];
    result.push({ entry, identity: entryIdentity(entry) });
  }
  return result;
};

export const playerLoadoutTargetKey = (target: Omit<NativePlayerLoadoutTarget, 'revision'>): string => {
  object(target, ['inventory', 'equipment']);
  if (target.inventory === undefined && target.equipment === undefined) invalid('empty target');
  const normalized: Record<string, unknown> = {};
  if (target.inventory !== undefined) {
    const counts = new Map<string, number>(), totals = new Map<number, number>();
    for (const { entry, identity } of entries(target.inventory)) {
      const total = (totals.get(entry.baseId) || 0) + entry.count;
      integer(total, 0x7fffffff, 1); totals.set(entry.baseId, total);
      const key = sortedJson(identity);
      counts.set(key, (counts.get(key) || 0) + entry.count);
    }
    normalized.inventory = Array.from(counts.entries()).sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0);
  }
  if (target.equipment !== undefined) {
    object(target.equipment, ['inv', ...spellFields]);
    const copies: string[] = [];
    for (const { entry, identity } of entries(target.equipment.inv)) {
      if (entry.worn && entry.wornLeft && entry.count < 2) invalid('dual-wield count');
      if (entry.worn) copies.push(sortedJson({ ...identity, left: false }));
      if (entry.wornLeft) copies.push(sortedJson({ ...identity, left: true }));
    }
    if (copies.length > 128) invalid('equipped copies');
    normalized.equipment = { copies: copies.sort(), spells: spellFields.map(field =>
      target.equipment![field] === undefined ? 0 : integer(target.equipment![field], 0xffffffff)) };
  }
  return sortedJson(normalized);
};

// Keep the actual request independent of incoming snapshots. Validate before
// JSON cloning can erase nonfinite values/unknown undefined fields. The key is
// canonical; the request retains the validated original rows and metadata.
export const playerLoadoutTarget = (inventory?: Inventory, equipment?: Equipment): Omit<NativePlayerLoadoutTarget, 'revision'> => {
  if (equipment !== undefined) object(equipment, ['inv', 'numChanges', ...spellFields]);
  const worn = equipment && { inv: equipment.inv, leftSpell: equipment.leftSpell,
    rightSpell: equipment.rightSpell, voiceSpell: equipment.voiceSpell, instantSpell: equipment.instantSpell };
  const target = { inventory, equipment: worn };
  playerLoadoutTargetKey(target);
  return JSON.parse(JSON.stringify(target));
};
