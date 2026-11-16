const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '../../skymp5-client');
const ts = require(require.resolve('typescript', { paths: [root, path.resolve(root, '../skymp5-server')] }));
const code = ts.transpileModule(fs.readFileSync(path.join(root, 'src/sync/nativePlayerLoadout.ts'), 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
}).outputText;
const helper = { exports: {} };
vm.runInNewContext(code, { module: helper, exports: helper.exports });
const { playerLoadoutTarget: target, playerLoadoutTargetKey: key } = helper.exports;
const inventory = entries => ({ inventory: { entries } });
const equipment = (entries, spells = {}) => ({ equipment: { inv: { entries }, ...spells } });
const item = (count = 1, extras = {}) => ({ baseId: 10, count, ...extras });

test('inventory keys compare validated multisets regardless of row order or stack splits', () => {
  const whole = inventory([item(3, { name: 'A', chargePercent: 0 }), item(2, { name: 'B', chargePercent: 75 })]);
  const split = inventory([item(1, { name: 'B', chargePercent: 75, worn: true }),
    item(1, { name: 'A', chargePercent: 0 }), item(1, { name: 'B', chargePercent: 75 }),
    item(2, { name: 'A', chargePercent: 0, wornLeft: true })]);
  assert.equal(key(whole), key(split));
  assert.notEqual(key(whole), key(inventory([item(2, { name: 'A', chargePercent: 0 }), item(3, { name: 'B', chargePercent: 75 })])));
});

test('keys preserve represented metadata including absent charge versus explicit zero', () => {
  const plain = key(inventory([item()]));
  for (const extras of [{ chargePercent: 0 }, { soul: 0 }, { health: 1.04 }, { name: 'Sword' },
    { enchantmentId: 11, maxCharge: 0 }, { poisonId: 12, poisonCount: 0 }])
    assert.notEqual(plain, key(inventory([item(1, extras)])), JSON.stringify(extras));
  const enchanted = { enchantmentId: 11, maxCharge: 200, removeEnchantmentOnUnequip: false };
  assert.notEqual(key(inventory([item(1, enchanted)])), key(inventory([item(1, { ...enchanted, removeEnchantmentOnUnequip: true })])));
  assert.notEqual(key(inventory([item(1, { name: 'Sword' })])), key(inventory([item(1, { name: 'Sword (Fine)' })])));
});

test('only engine-independent float32 and default metadata normalization shares a key', () => {
  assert.equal(key(inventory([item(1, { health: 1, name: '' })])), key(inventory([item()])));
  assert.equal(key(inventory([item(1, { health: 1.04 })])), key(inventory([item(1, { health: Math.fround(1.04) })])));
  assert.notEqual(key(inventory([item(1, { health: 1.04 })])), key(inventory([item(1, { health: 1.03 })])));
  assert.equal(key(inventory([item(1, { enchantmentId: 11, maxCharge: 0 })])),
    key(inventory([item(1, { enchantmentId: 11, maxCharge: 0, removeEnchantmentOnUnequip: false })])));
  assert.equal(key(equipment([])), key(equipment([], { leftSpell: 0, rightSpell: 0, voiceSpell: 0, instantSpell: 0 })));
  assert.notEqual(key(equipment([])), key(equipment([], { leftSpell: 42 })));
});

test('equipment keys select one copy per worn flag and retain selected-copy multiplicity', () => {
  assert.equal(key(equipment([item(1, { worn: true })])), key(equipment([item(8, { worn: true }), item(9, { name: 'Unworn' })])));
  assert.equal(key(equipment([item(2, { worn: true, wornLeft: true })])),
    key(equipment([item(1, { wornLeft: true }), item(1, { worn: true })])));
  assert.notEqual(key(equipment([item(1, { worn: true })])), key(equipment([item(1, { wornLeft: true })])));
  assert.notEqual(key(equipment([item(1, { worn: true })])), key(equipment([item(1, { worn: true }), item(1, { worn: true })])));
  assert.throws(() => key(equipment([item(1, { worn: true, wornLeft: true })])), /dual-wield count/);
  assert.throws(() => key(equipment(Array.from({ length: 129 }, () => item(1, { worn: true })))), /equipped copies/);
});

test('invalid individual counts never disappear through cancellation or aggregation', () => {
  for (const count of [0, -1, 0.5, NaN, Infinity, 0x80000000, Number.MAX_SAFE_INTEGER]) {
    assert.throws(() => key(inventory([item(1), item(count)])), /Invalid native player loadout/);
    assert.throws(() => key(equipment([item(count)])), /Invalid native player loadout/);
  }
  assert.throws(() => key(inventory([item(0x7fffffff), item()])), /integer/);
  assert.throws(() => key(inventory([item(0x7fffffff, { name: 'A' }), item(1, { name: 'B' })])), /integer/);
  assert.doesNotThrow(() => key(inventory([item(0x7fffffff)])));
});

test('invalid metadata and ignored equipment rows are validated before deduplication', () => {
  const bad = [{ extra: undefined }, { health: NaN }, { health: 1e-50 }, { health: 3.4028235e38 },
    { chargePercent: -1 }, { worn: 1 }, { name: 'bad\0name' }, { name: '💎'.repeat(257) },
    { enchantmentId: 11 }, { maxCharge: 0 }, { removeEnchantmentOnUnequip: false },
    { enchantmentId: 11, maxCharge: 65536 }, { soul: 6 }, { poisonId: 12 }, { poisonCount: 0 }];
  for (const extras of bad) {
    assert.throws(() => key(inventory([item(1, extras)])), /Invalid native player loadout/, JSON.stringify(extras));
    assert.throws(() => key(equipment([item(1, extras)])), /Invalid native player loadout/, JSON.stringify(extras));
  }
  assert.doesNotThrow(() => key(inventory([item(1, { name: '💎'.repeat(256) })])));
});

test('malformed collection and spell fields cannot be erased by cloning', () => {
  const sparse = []; sparse.length = 1;
  for (const value of [{}, inventory(sparse), inventory([null]), inventory(Array(4097).fill(item())),
    { inventory: { entries: [], extra: undefined } }, { inventory: { entries: [] }, extra: undefined },
    equipment([], { leftSpell: NaN }), equipment([], { rightSpell: -1 }), equipment([], { voiceSpell: '1' })])
    assert.throws(() => key(value), /Invalid native player loadout/);
  assert.throws(() => target({ entries: [item(1, { health: NaN })] }), /float/);
  assert.throws(() => target({ entries: [item(1, { extra: undefined })] }), /field/);
  assert.throws(() => target(undefined, { inv: { entries: [] }, numChanges: 1, unknown: undefined }), /field/);
});

test('immutable request retains validated original rows while the key alone aggregates', () => {
  const inv = { entries: [item(1, { chargePercent: 0 }), item(2, { chargePercent: 0 })] };
  const eq = { inv: { entries: [item(3, { chargePercent: 0, wornLeft: true })] }, numChanges: 7, leftSpell: 0 };
  const request = target(inv, eq);
  assert.equal(JSON.stringify(request), JSON.stringify({ inventory: inv, equipment: { inv: eq.inv, leftSpell: 0 } }));
  assert.equal(request.inventory.entries.length, 2);
  const before = key(request);
  inv.entries[0].chargePercent = 25; eq.inv.entries[0].wornLeft = false;
  assert.equal(key(request), before);
  assert.equal(request.inventory.entries[0].chargePercent, 0);
  assert.equal(request.equipment.inv.entries[0].wornLeft, true);
});
