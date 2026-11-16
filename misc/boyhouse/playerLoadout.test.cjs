const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { EventEmitter } = require('node:events');
const root = path.resolve(__dirname, '../../skymp5-client');
const ts = require(require.resolve('typescript', { paths: [root, path.resolve(root, '../skymp5-server')] }));
const compiled = ts.transpileModule(fs.readFileSync(path.join(root,
  'src/services/services/playerLoadoutService.ts'), 'utf8'), { compilerOptions: {
  module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020,
} }).outputText;

const inventory = baseId => ({ entries: [{ baseId, count: 1, worn: true }] });
const equipment = baseId => ({ inv: inventory(baseId), numChanges: 1 });
const spawn = (idx, creation = true) => ({ idx, isMe: true,
  equipment: equipment(idx), props: { isRaceMenuOpen: creation, inventory: inventory(idx) } });

function harness(storage = {}, clock = { now: 10000 }) {
  const events = new EventEmitter(), emitter = new EventEmitter(), applied = [];
  let shown = false, badMenu = false, retryInventory = false;
  let duringEquipment = () => {}, duringInventory = () => {};
  const player = { unequipAll: () => applied.push('unequip') };
  const dependencies = {
    './clientListener': { ClientListener: class {} },
    '../../sync/equipment': {
      isBadMenuShown: () => badMenu,
      applyEquipment(actor, eq) {
        assert.equal(actor, player); applied.push(['equipment', eq]); duringEquipment(); return true;
      },
    },
    '../../sync/inventory': {
      applyInventory(actor, inv, protection, ignoreWorn) {
        assert.equal(actor, player); assert.equal(protection, false); assert.equal(ignoreWorn, true);
        applied.push(['inventory', inv]); duringInventory(); return !retryInventory;
      },
    },
  };
  const module = { exports: {} };
  vm.runInNewContext(compiled, { module, exports: module.exports, Date: { now: () => clock.now },
    require: id => {
      assert.ok(id in dependencies, `Unexpected dependency ${id}`); return dependencies[id];
    },
  });
  const service = new module.exports.PlayerLoadoutService({ storage,
    Game: { getPlayer: () => player, showRaceMenu: () => applied.push('open') },
    Ui: { isMenuOpen: () => shown },
  }, { on: (...args) => events.on(...args), emitter });
  return { service, emitter, applied, storage, clock,
    update(ms = 0) { clock.now += ms; events.emit('update'); },
    shown(value) { shown = value; }, badMenu(value) { badMenu = value; },
    retryInventory(value) { retryInventory = value; },
    duringEquipment(fn) { duringEquipment = fn; }, duringInventory(fn) { duringInventory = fn; },
    message(event, message) { emitter.emit(event, { message }); },
    start(idx = 1, creation = true) { const generation = service.beginSpawn(spawn(idx, creation));
      service.markSpawnReady(generation); return generation; },
  };
}

test('creation keeps the latest loadout queued through slow opening and editing, then restores it once', () => {
  const h = harness(), generation = h.start();
  h.update(); h.update(299);
  assert.deepEqual(h.applied, ['unequip']);
  h.message('setRaceMenuOpenMessage', { open: true }); // Duplicate server request.
  h.update(1); h.service.markSpawnReady(generation); h.update(20000);
  assert.deepEqual(h.applied, ['unequip', 'open']);
  assert.equal(h.service.blocksEquipmentInput, true); // Never assume a successful open.
  h.shown(true); h.update();
  h.message('setInventoryMessage', { inventory: inventory(20) });
  h.message('setInventoryMessage', { inventory: inventory(30) });
  h.message('updateEquipmentMessage', { idx: 1, data: equipment(30) });
  h.message('updateEquipmentMessage', { idx: 2, data: equipment(99) }); // Another player.
  h.update(20000);
  assert.deepEqual(h.applied, ['unequip', 'open']);
  assert.deepEqual(h.storage.pcInv, inventory(30));
  h.shown(false); h.update(); h.update();
  assert.deepEqual(h.applied, ['unequip', 'open', ['equipment', equipment(30)], ['inventory', inventory(30)]]);
  assert.equal(h.service.blocksEquipmentInput, false);
  h.update(5001);
  assert.deepEqual(h.applied.at(-1), ['inventory', inventory(30)]);
  assert.equal(h.applied.length, 5);
});

test('canceling an obsolete spawn cannot erase the current player loadout', () => {
  const h = harness(), old = h.service.beginSpawn(spawn(1, false));
  const current = h.service.beginSpawn(spawn(2, false));
  h.service.cancelSpawn(old); h.service.markSpawnReady(current); h.update();
  assert.ok(h.applied.some(call => Array.isArray(call) && call[0] === 'inventory' && call[1].entries[0].baseId === 2));
  h.service.cancelSpawn(current); assert.equal(h.storage.pcInv, undefined);
});

for (const event of ['connectionAccepted', 'connectionDisconnect', 'connectionFailed', 'connectionDenied']) {
  test(`${event} discards old snapshots and delayed spawn readiness`, () => {
    const h = harness(), old = h.start();
    h.update(); h.emitter.emit(event);
    assert.equal(h.storage.pcInv, undefined);
    const current = h.service.beginSpawn(spawn(2, false));
    h.service.markSpawnReady(old); h.update(20000);
    assert.deepEqual(h.applied, ['unequip']);
    h.service.markSpawnReady(current); h.update();
    assert.deepEqual(h.applied.slice(1), [['equipment', equipment(2)], ['inventory', inventory(2)]]);
  });
}

test('game loading only retains loadout for the generated multiplayer save', () => {
  const h = harness(); h.start(1, false);
  h.emitter.emit('gameLoad', { isCausedBySkyrimPlatform: true }); h.update();
  assert.equal(h.applied.length, 2);
  h.emitter.emit('gameLoad', { isCausedBySkyrimPlatform: false }); h.update(20000);
  assert.equal(h.applied.length, 2);
  assert.equal(h.storage.pcInv, undefined);
  assert.equal(h.service.blocksEquipmentInput, true);
});

test('blocked application and native retries keep the loadout pending', () => {
  const h = harness(); h.start(1, false);
  const block = event => event.block(); h.emitter.on('queryBlockSetInventoryEvent', block);
  h.update(); assert.equal(h.applied.length, 0);
  h.emitter.off('queryBlockSetInventoryEvent', block);
  h.badMenu(true); h.update(); assert.equal(h.applied.length, 0);
  h.badMenu(false); h.retryInventory(true); h.update();
  assert.equal(h.service.blocksEquipmentInput, true);
  h.retryInventory(false); h.update();
  assert.equal(h.service.blocksEquipmentInput, false);
  assert.deepEqual(h.applied.map(x => x[0]), ['equipment', 'inventory', 'inventory']);
});

test('a hot reload retains the open preview gate and queued inventory', () => {
  const h = harness(); h.start(); h.update(); h.update(300); h.shown(true); h.update();
  h.message('setInventoryMessage', { inventory: inventory(30) });
  const reloaded = harness(h.storage, h.clock);
  reloaded.shown(true); reloaded.update(20000);
  assert.equal(reloaded.service.blocksEquipmentInput, true);
  assert.equal(reloaded.applied.length, 0);
  reloaded.shown(false); reloaded.update();
  assert.deepEqual(reloaded.applied, [['equipment', equipment(1)], ['inventory', inventory(30)]]);
});

test('a reset during the inventory guard cannot apply the previous character loadout', () => {
  const h = harness(); h.start(1, false);
  h.emitter.once('queryBlockSetInventoryEvent', () => h.emitter.emit('connectionDisconnect'));
  h.update(); assert.equal(h.applied.length, 0);
});

test('a reset from native equipment application cannot continue into stale inventory', () => {
  const h = harness(); h.start(1, false);
  h.duringEquipment(() => h.emitter.emit('connectionDisconnect'));
  h.update(); assert.deepEqual(h.applied, [['equipment', equipment(1)]]);
  assert.equal(h.storage.pcInv, undefined);
});

test('an inventory replacement raised during native application remains pending', () => {
  const h = harness(); h.start(1, false);
  h.duringInventory(() => {
    h.duringInventory(() => {});
    h.message('setInventoryMessage', { inventory: inventory(20) });
  });
  h.update(); assert.equal(h.service.blocksEquipmentInput, true);
  h.update(); assert.deepEqual(h.applied.at(-1), ['inventory', inventory(20)]);
  assert.equal(h.service.blocksEquipmentInput, false);
});

test('an equipment echo during normal play does not rebuild inventory or cause an equip loop', () => {
  const h = harness(); h.start(1, false); h.update();
  h.message('updateEquipmentMessage', { idx: 1, data: equipment(20) }); h.update();
  assert.equal(h.applied.length, 2);
  assert.equal(h.service.blocksEquipmentInput, false);
  h.message('setRaceMenuOpenMessage', { open: true });
  h.update(); h.update(300); h.shown(true); h.update(); h.shown(false); h.update();
  assert.deepEqual(h.applied.at(-2), ['equipment', equipment(20)]);
});
