const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '../../skymp5-client');
const ts = require(require.resolve('typescript', { paths: [root, path.resolve(root, '../skymp5-server')] }));
const compile = file => ts.transpileModule(fs.readFileSync(path.join(root, file), 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
}).outputText;
const inventoryCode = compile('src/sync/inventory.ts');
const equipmentCode = compile('src/sync/equipment.ts');
const plain = value => JSON.parse(JSON.stringify(value));

function harness(base = [], changes = [], type = 41) {
  const calls = [];
  const form = { getFormID: () => 10, getName: () => 'Sword', getType: () => type };
  const actor = {
    getFormID: () => 0x14,
    getBaseObject: () => ({ getFormID: () => 7 }),
    getItemCount: () => base.filter(e => e.baseId === 10).reduce((n, e) => n + e.count, 0) +
      changes.filter(e => e.baseId === 10).reduce((n, e) => n + e.countDelta, 0),
    getEquippedSpell: () => null,
    unequipAll: () => calls.push('unequip'),
    equipItemEx: () => calls.push('equip'),
    removeAllItems: () => calls.push('clear'),
    queueNiNodeUpdate: () => calls.push('node'),
  };
  const sp = {
    Game: { getFormEx: id => id === 10 ? form : [20, 30].includes(id) ? { getFormID: () => id } : null },
    Actor: { from: value => value }, Ammo: { from: value => value === form && type === 42 ? value : null },
    Enchantment: { from: value => value?.getFormID() === 20 ? value : null },
    Potion: { from: value => value?.getFormID() === 30 ? value : null },
    FormType: { Misc: 32, Potion: 46, Ingredient: 30 },
    ObjectReference: class {}, Form: class {}, storage: {}, printConsole() {},
    TESModPlatform: { resetContainer: () => calls.push('reset'),
      pushWornState: () => calls.push('worn'), addItemEx: (...args) => calls.push(['add', ...args]) },
    getContainer: () => plain(base),
    getExtraContainerChanges: () => plain(changes),
  };
  const inventory = { exports: {} };
  vm.runInNewContext(inventoryCode, { module: inventory, exports: inventory.exports,
    require: id => { assert.equal(id, 'skyrimPlatform'); return sp; } });
  const equipment = { exports: {} };
  vm.runInNewContext(equipmentCode, { module: equipment, exports: equipment.exports, require: id => {
    if (id === './inventory') return inventory.exports;
    assert.equal(id, 'skyrimPlatform'); return sp;
  } });
  return { actor, calls, storage: sp.storage, inventory: inventory.exports, equipment: equipment.exports,
    read: () => plain(inventory.exports.getEquipmentInventory(actor)) };
}

const named = name => ({ type: 'TextDisplayData', name });
const charge = value => ({ type: 'Charge', charge: value });

test('native same-base names and charges remain distinct before equipment selection', () => {
  for (const [extras, first, second] of [
    [[[named('Wanted blade')], [named('Other blade')]], { name: 'Wanted blade' }, { name: 'Other blade' }],
    [[[charge(25)], [charge(75)]], { chargePercent: 25 }, { chargePercent: 75 }],
  ]) {
    const h = harness([], [{ baseId: 10, countDelta: 2, extendDataList: extras }]);
    assert.deepEqual(h.read(), { entries: [
      { baseId: 10, count: 1, ...first }, { baseId: 10, count: 1, ...second },
    ] });
    assert.throws(() => h.equipment.applyPlayerEquipment(h.actor, {
      inv: { entries: [{ baseId: 10, count: 1, worn: true, ...first }] },
    }), /item-instance/);
    assert.deepEqual(h.calls, [], 'Ambiguity must be observed before any equipment mutation');
  }
});

test('base-container count cancellation preserves the named native instance', () => {
  const h = harness([{ baseId: 10, count: 2 }], [{ baseId: 10, countDelta: 0,
    extendDataList: [[named('Wanted blade')]] }]);
  const entries = h.read().entries;
  assert.equal(entries.reduce((n, e) => n + e.count, 0), 2);
  assert.deepEqual(entries.find(e => e.name === 'Wanted blade'), { baseId: 10, count: 1, name: 'Wanted blade' });
  assert.deepEqual(entries.find(e => !e.name), { baseId: 10, count: 1 });
  assert.throws(() => h.equipment.applyPlayerEquipment(h.actor, {
    inv: { entries: [{ baseId: 10, count: 1, name: 'Wanted blade', worn: true }] },
  }), /item-instance/);
  assert.deepEqual(h.calls, []);
});

test('identical native variants can merge without losing their equipment identity', () => {
  const h = harness([], [{ baseId: 10, countDelta: 2,
    extendDataList: [[named('Wanted blade'), charge(25)], [named('Wanted blade'), charge(25)]] }]);
  assert.deepEqual(h.read(), { entries: [{ baseId: 10, count: 2, name: 'Wanted blade', chargePercent: 25 }] });
  h.equipment.applyPlayerEquipment(h.actor, {
    inv: { entries: [{ baseId: 10, count: 1, name: 'Wanted blade', chargePercent: 25, worn: true }] },
  });
  assert.deepEqual(h.calls, ['unequip', 'equip']);
});

test('different parenthesized custom names cannot satisfy worn equipment readiness', () => {
  const h = harness([], [{ baseId: 10, countDelta: 1,
    extendDataList: [[named('Sword (Ice)'), { type: 'Worn' }]] }]);
  assert.equal(h.equipment.isPlayerEquipmentSettled(h.actor, {
    inv: { entries: [{ baseId: 10, count: 1, name: 'Sword (Fire)', worn: true }] },
  }), false);
  assert.equal(h.equipment.isPlayerEquipmentSettled(h.actor, {
    inv: { entries: [{ baseId: 10, count: 1, name: 'Sword (Ice)', worn: true }] },
  }), true);
});

test('native float charge normalization preserves an otherwise identical equipped item', () => {
  const h = harness([], [{ baseId: 10, countDelta: 1,
    extendDataList: [[charge(Math.fround(0.1)), { type: 'Worn' }]] }]);
  assert.equal(h.equipment.isPlayerEquipmentSettled(h.actor, {
    inv: { entries: [{ baseId: 10, count: 1, chargePercent: 0.1, worn: true }] },
  }), true);
});

test('absent and explicit default display names identify the same native worn item', () => {
  for (const actualName of [undefined, '', 'Sword']) {
    const extraList = [{ type: 'Worn' }];
    if (actualName !== undefined) extraList.push(named(actualName));
    const h = harness([], [{ baseId: 10, countDelta: 1, extendDataList: [extraList] }]);
    for (const expectedName of [undefined, '', 'Sword']) {
      const entry = { baseId: 10, count: 1, worn: true };
      if (expectedName !== undefined) entry.name = expectedName;
      assert.equal(h.equipment.isPlayerEquipmentSettled(h.actor, { inv: { entries: [entry] } }), true);
    }
  }
});

test('depleted charge and absent native charge never certify one another', () => {
  for (const [extras, wanted] of [[[], { chargePercent: 0 }], [[charge(0)], {}]]) {
    const h = harness([], [{ baseId: 10, countDelta: 1, extendDataList: [[...extras, { type: 'Worn' }]] }]);
    const expected = { entries: [{ baseId: 10, count: 1, worn: true, ...wanted }] };
    assert.equal(h.inventory.isInventorySettled(h.actor, expected), false);
    assert.equal(h.equipment.isPlayerEquipmentSettled(h.actor, { inv: expected }), false);
    assert.throws(() => h.inventory.applyInventory(h.actor, expected, false, true), /unsupported native/);
    assert.deepEqual(h.calls, []);
    assert.deepEqual(Object.keys(h.storage), [], 'Even the base-reset marker must remain untouched');
  }
});

test('an already matching depleted item settles without invoking the legacy writer', () => {
  const h = harness([], [{ baseId: 10, countDelta: 1, extendDataList: [[charge(0), { type: 'Worn' }]] }]);
  const expected = { entries: [{ baseId: 10, count: 1, chargePercent: 0, worn: true }] };
  assert.equal(h.inventory.isInventorySettled(h.actor, expected), true);
  assert.equal(h.equipment.isPlayerEquipmentSettled(h.actor, { inv: expected }), true);
  assert.equal(h.inventory.applyInventory(h.actor, expected, false, true), true);
  h.equipment.applyPlayerEquipment(h.actor, { inv: expected });
  assert.deepEqual(h.calls, []);
  assert.deepEqual(Object.keys(h.storage), []);
});

test('self health preserves float32 precision while legacy general reads retain rounding', () => {
  const health = Math.fround(1.04);
  const h = harness([], [{ baseId: 10, countDelta: 1,
    extendDataList: [[{ type: 'Health', health }, { type: 'Worn' }]] }]);
  assert.equal(h.read().entries[0].health, health);
  assert.equal(h.inventory.getInventory(h.actor).entries[0].health, undefined);
  assert.equal(h.equipment.getEquipment(h.actor, 1).inv.entries[0].health, health);
  for (const wanted of [undefined, 1, 1.03]) {
    const expected = { entries: [{ baseId: 10, count: 1, health: wanted, worn: true }] };
    assert.equal(h.inventory.isInventorySettled(h.actor, expected), false);
    assert.equal(h.equipment.isPlayerEquipmentSettled(h.actor, { inv: expected }), false);
    assert.throws(() => h.inventory.applyInventory(h.actor, expected, false, true), /unsupported native/);
    assert.deepEqual(h.calls, []);
  }
  const exact = { entries: [{ baseId: 10, count: 1, health: 1.04, worn: true }] };
  assert.equal(h.inventory.isInventorySettled(h.actor, exact), true);
  assert.equal(h.equipment.isPlayerEquipmentSettled(h.actor, { inv: exact }), true);
  assert.equal(h.inventory.applyInventory(h.actor, exact, false, true), true);
  assert.deepEqual(h.calls, []);
});

test('strict reads keep nearby health variants separate and inventory counts ignore worn grouping', () => {
  const h = harness([], [{ baseId: 10, countDelta: 3, extendDataList: [
    [{ type: 'Health', health: Math.fround(1.01) }],
    [{ type: 'Health', health: Math.fround(1.04) }],
    [{ type: 'Health', health: Math.fround(1.04) }, { type: 'Worn' }],
  ] }]);
  assert.equal(h.read().entries.length, 3);
  assert.equal(h.inventory.isInventorySettled(h.actor, { entries: [
    { baseId: 10, count: 2, health: 1.04 }, { baseId: 10, count: 1, health: 1.01 },
  ] }), true);
  assert.equal(h.inventory.isInventorySettled(h.actor, { entries: [
    { baseId: 10, count: 3, health: 1.04 },
  ] }), false);
});

test('native enchantment and soul metadata must be complete before self readiness', () => {
  for (const bad of [
    { type: 'Enchantment', enchantmentId: 20, maxCharge: 100 },
    { type: 'Soul', soul: 156 },
  ]) {
    const h = harness([], [{ baseId: 10, countDelta: 1, extendDataList: [[bad]] }]);
    assert.throws(() => h.read(), /Native inventory reader/);
    assert.throws(() => h.inventory.applyInventory(h.actor, { entries: [] }, false, true), /Native inventory reader/);
    assert.deepEqual(h.calls, []);
  }
  const h = harness([], [{ baseId: 10, countDelta: 1, extendDataList: [[
    { type: 'Enchantment', enchantmentId: 20, maxCharge: 100, removeOnUnequip: false },
    { type: 'Soul', soul: 3 }, { type: 'Poison', poisonId: 30, count: 2 },
  ]] }]);
  const entry = { baseId: 10, count: 1, enchantmentId: 20, maxCharge: 100,
    removeEnchantmentOnUnequip: false, soul: 3, poisonId: 30, poisonCount: 2 };
  assert.equal(h.inventory.isInventorySettled(h.actor, { entries: [entry] }), true);
  for (const change of [{ maxCharge: 101 }, { removeEnchantmentOnUnequip: true }, { soul: 4 },
    { poisonId: 20 }, { poisonCount: 1 }])
    assert.equal(h.inventory.isInventorySettled(h.actor, { entries: [{ ...entry, ...change }] }), false);
});

test('legacy self writer rejects unrepresentable targets before any reset or dispatch', () => {
  for (const extra of [{ chargePercent: 0 }, { health: 0.9 }, { name: 'x'.repeat(128) },
    { enchantmentId: 20, maxCharge: 65536 }, { soul: 0 }, { poisonId: 30, poisonCount: -1 }, { count: 1001 }]) {
    const h = harness();
    assert.throws(() => h.inventory.applyInventory(h.actor,
      { entries: [{ baseId: 10, count: 1, ...extra }] }, false, true), /unsupported native/);
    assert.deepEqual(h.calls, []);
    assert.deepEqual(Object.keys(h.storage), []);
  }
  const ammo = harness([], [], 42);
  assert.throws(() => ammo.inventory.applyInventory(ammo.actor,
    { entries: [{ baseId: 10, count: 1, name: 'Custom arrow' }] }, false, true), /unsupported native/);
  assert.deepEqual(ammo.calls, []);
  const matchingZero = harness([], [{ baseId: 10, countDelta: 1, extendDataList: [[charge(0)]] }]);
  assert.throws(() => matchingZero.inventory.applyInventory(matchingZero.actor,
    { entries: [{ baseId: 10, count: 2, chargePercent: 0 }] }, false, true), /unsupported native/);
  assert.deepEqual(matchingZero.calls, []);
});

test('inventory writes cannot reset a currently worn temporary enchantment', () => {
  const h = harness([], [{ baseId: 10, countDelta: 1, extendDataList: [[
    { type: 'Enchantment', enchantmentId: 20, maxCharge: 100, removeOnUnequip: true }, { type: 'Worn' },
  ]] }]);
  const entry = { baseId: 10, count: 1, enchantmentId: 20, maxCharge: 100, removeEnchantmentOnUnequip: true };
  assert.equal(h.inventory.applyInventory(h.actor, { entries: [entry] }, false, true), true);
  assert.throws(() => h.inventory.applyInventory(h.actor, { entries: [{ ...entry, count: 2 }] }, false, true), /unsupported native/);
  assert.deepEqual(h.calls, []);
  assert.deepEqual(Object.keys(h.storage), []);
});

test('legacy writer cannot repair name-only differences or remove a chosen variant', () => {
  for (const wanted of [
    [{ baseId: 10, count: 2, name: 'Different' }],
    [{ baseId: 10, count: 1, name: 'Wanted' }],
  ]) {
    const h = harness([], [{ baseId: 10, countDelta: 2,
      extendDataList: [[named('Wanted')], [named('Other')]] }]);
    assert.equal(h.inventory.isInventorySettled(h.actor, { entries: wanted }), false);
    assert.throws(() => h.inventory.applyInventory(h.actor, { entries: wanted }, false, true), /unsupported native/);
    assert.deepEqual(h.calls, []);
  }
});

test('plain supported missing items still reach the existing writer', () => {
  const h = harness();
  h.inventory.applyInventory(h.actor, { entries: [{ baseId: 10, count: 1 }] }, false, true);
  assert.deepEqual(h.calls.slice(0, 2), ['reset', 'clear']);
  const writes = h.calls.filter(call => Array.isArray(call) && call[0] === 'add');
  assert.equal(writes.length, 1);
  assert.equal(writes[0][3], 1);
});

test('malformed count rows cannot cancel to a false settled snapshot', () => {
  for (const count of [-1, 0.5, Infinity]) {
    const h = harness(), expected = { entries: [{ baseId: 10, count: 1 }, { baseId: 10, count }] };
    assert.throws(() => h.inventory.isInventorySettled(h.actor, expected), /invalid item count/);
    assert.throws(() => h.inventory.applyInventory(h.actor, expected, false, true), /invalid item count/);
    assert.throws(() => h.equipment.isPlayerEquipmentSettled(h.actor, { inv: expected }), /invalid item count/);
    assert.deepEqual(h.calls, []);
  }
});
