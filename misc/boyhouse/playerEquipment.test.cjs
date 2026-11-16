const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '../../skymp5-client');
const ts = require(require.resolve('typescript', { paths: [root, path.resolve(root, '../skymp5-server')] }));
const code = ts.transpileModule(fs.readFileSync(path.join(root, 'src/sync/equipment.ts'), 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
}).outputText;
const inventoryCode = ts.transpileModule(fs.readFileSync(path.join(root, 'src/sync/inventory.ts'), 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
}).outputText;

function harness() {
  let inventory = { entries: [] }, spells = [0, 0, 0, 0];
  const calls = [], forms = new Map([10, 20, 30, 40].map(id => [id, { getFormID: () => id, getName: () => `Item ${id}` }]));
  const actor = {
    getFormID: () => 0x14, getItemCount: form => inventory.entries.filter(e => e.baseId === form.getFormID()).reduce((n, e) => n + e.count, 0),
    getEquippedSpell: slot => spells[slot] ? forms.get(spells[slot]) : null,
    unequipAll: () => calls.push(['unequip']),
    equipItemEx: (...args) => calls.push(['equip', args[0].getFormID(), ...args.slice(1)]),
    equipSpell: (form, slot) => calls.push(['spell', form.getFormID(), slot]),
    unequipSpell: (form, slot) => calls.push(['unspell', form.getFormID(), slot]),
    removeAllItems: () => assert.fail('Self restoration must not remove all inventory'),
  };
  const sp = { Game: { getFormEx: id => forms.get(id) || null },
    Actor: { from: value => value }, Spell: { from: value => value },
    setInventory: () => assert.fail('Self restoration must never queue additive setInventory') };
  const inventoryModule = { exports: {} };
  vm.runInNewContext(inventoryCode, { module: inventoryModule, exports: inventoryModule.exports,
    require: id => { assert.equal(id, 'skyrimPlatform'); return sp; } });
  const module = { exports: {} };
  vm.runInNewContext(code, { module, exports: module.exports, require(id) {
    if (id === './inventory') return { ...inventoryModule.exports,
      getInventory: () => inventory, getEquipmentInventory: () => inventory };
    assert.equal(id, 'skyrimPlatform');
    return sp;
  } });
  return { actor, calls, api: module.exports, inventory: value => inventory = value,
    spells: value => spells = value };
}

test('self equipment restoration only equips existing items with correct hand slots and spells', () => {
  const h = harness(), eq = { inv: { entries: [
    { baseId: 10, count: 1, worn: true }, { baseId: 20, count: 1, wornLeft: true },
  ] }, leftSpell: 30, voiceSpell: 40 };
  h.inventory({ entries: [{ baseId: 10, count: 1 }, { baseId: 20, count: 1 }] });
  h.api.applyPlayerEquipment(h.actor, eq);
  assert.deepEqual(h.calls, [['unequip'], ['equip', 10, 1, false, false], ['equip', 20, 2, false, false],
    ['spell', 30, 0], ['spell', 40, 2]]);
  assert.equal(h.api.isPlayerEquipmentSettled(h.actor, eq), false, 'Dispatch does not mutate observation');
  h.inventory(eq.inv); h.spells([30, 0, 40, 0]);
  assert.equal(h.api.isPlayerEquipmentSettled(h.actor, eq), true);
  h.spells([0, 0, 40, 0]); assert.equal(h.api.isPlayerEquipmentSettled(h.actor, eq), false);
});

test('equipment readiness detects wrong hands and extra equipped items without using total stack counts', () => {
  const h = harness(), eq = { inv: { entries: [{ baseId: 10, count: 100, worn: true }] } };
  h.inventory({ entries: [{ baseId: 10, count: 50, wornLeft: true }] });
  assert.equal(h.api.isPlayerEquipmentSettled(h.actor, eq), false);
  h.inventory({ entries: [{ baseId: 10, count: 50, worn: true }, { baseId: 20, count: 1, worn: true }] });
  assert.equal(h.api.isPlayerEquipmentSettled(h.actor, eq), false);
  h.inventory({ entries: [{ baseId: 10, count: 50, worn: true }] });
  assert.equal(h.api.isPlayerEquipmentSettled(h.actor, eq), true);
});

test('absent authoritative gear is refused before unequipping or inventing an item', () => {
  const h = harness();
  assert.throws(() => h.api.applyPlayerEquipment(h.actor, { inv: { entries: [{ baseId: 10, count: 1, worn: true }] } }), /absent/);
  assert.deepEqual(h.calls, []);
});

test('same-base variants cannot falsely settle or be selected ambiguously', () => {
  for (const variant of [{ health: 1.5 }, { enchantmentId: 99, maxCharge: 200 },
    { name: 'Named sword' }, { chargePercent: 25 }, { soul: 3 }, { poisonId: 20, poisonCount: 1 }]) {
    const h = harness(), wanted = { baseId: 10, count: 1, worn: true, ...variant };
    const eq = { inv: { entries: [wanted] } };
    h.inventory({ entries: [{ baseId: 10, count: 1, worn: true }, { ...wanted, worn: false }] });
    assert.equal(h.api.isPlayerEquipmentSettled(h.actor, eq), false, JSON.stringify(variant));
    assert.throws(() => h.api.applyPlayerEquipment(h.actor, eq), /item-instance/);
    assert.deepEqual(h.calls, [], 'Ambiguity is detected before unequipping');
    h.inventory({ entries: [{ ...wanted, worn: false }] });
    h.api.applyPlayerEquipment(h.actor, eq);
    h.inventory(eq.inv); assert.equal(h.api.isPlayerEquipmentSettled(h.actor, eq), true);
  }
});
