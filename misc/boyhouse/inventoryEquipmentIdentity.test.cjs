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

function harness(base = [], changes = []) {
  const calls = [];
  const form = { getFormID: () => 10, getName: () => 'Sword' };
  const actor = {
    getFormID: () => 0x14,
    getBaseObject: () => ({ getFormID: () => 7 }),
    getItemCount: () => base.filter(e => e.baseId === 10).reduce((n, e) => n + e.count, 0) +
      changes.filter(e => e.baseId === 10).reduce((n, e) => n + e.countDelta, 0),
    getEquippedSpell: () => null,
    unequipAll: () => calls.push('unequip'),
    equipItemEx: () => calls.push('equip'),
  };
  const sp = {
    Game: { getFormEx: id => id === 10 ? form : null },
    Actor: { from: value => value }, Ammo: { from: () => null },
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
  return { actor, calls, inventory: inventory.exports, equipment: equipment.exports,
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
