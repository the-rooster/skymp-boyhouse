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
const spawn = (idx, creation = true) => ({ idx, refrId: 0xff000000 + idx, isMe: true,
  equipment: equipment(idx), props: { isRaceMenuOpen: creation, inventory: inventory(idx) } });

function harness(storage = {}, clock = { now: 10000 }) {
  const events = new EventEmitter(), emitter = new EventEmitter(), applied = [];
  let shown = false, badMenu = false, retryInventory = false;
  let nativeInventory, nativeEquipment, delayedEquipment = false, paused = false, loaded = true;
  let duringEquipment = () => {}, duringInventory = () => {};
  let appearance = { name: 'Finished character', raceId: 1, options: [0.2], tints: [] };
  const player = { unequipAll: () => { applied.push('unequip'); nativeEquipment = undefined; }, is3DLoaded: () => loaded };
  const dependencies = {
    './clientListener': { ClientListener: class {} },
    '../../sync/appearance': { getAppearance: () => structuredClone(appearance) },
    '../../sync/equipment': {
      isBadMenuShown: () => badMenu,
      isPlayerEquipmentSettled: (actor, eq) => JSON.stringify(eq) === JSON.stringify(nativeEquipment),
      applyPlayerEquipment(actor, eq) {
        assert.equal(actor, player); applied.push(['equipment', eq]);
        if (!delayedEquipment) nativeEquipment = structuredClone(eq);
        duringEquipment();
      },
    },
    '../../sync/inventory': {
      isInventorySettled: (actor, inv) => JSON.stringify(inv) === JSON.stringify(nativeInventory),
      applyInventory(actor, inv, protection, ignoreWorn) {
        assert.equal(actor, player); assert.equal(protection, false); assert.equal(ignoreWorn, true);
        applied.push(['inventory', inv]);
        if (!retryInventory) nativeInventory = structuredClone(inv);
        duringInventory(); return true; // Dispatch may succeed without changing native state.
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
    Utility: { isInMenuMode: () => paused },
  }, { on: (...args) => events.on(...args), emitter });
  return { service, emitter, applied, storage, clock, events,
    update(ms = 0) { clock.now += ms; events.emit('update'); },
    settle() { for (let i = 0; i < 6; ++i) { clock.now += 250; events.emit('update'); } },
    tick(ms = 0) { clock.now += ms; events.emit('tick'); },
    appearance(value) { appearance = value; },
    shown(value) { shown = value; }, badMenu(value) { badMenu = value; },
    retryInventory(value) { retryInventory = value; },
    delayedEquipment(value) { delayedEquipment = value; },
    observeInventory(value) { nativeInventory = value; },
    observeEquipment(value) { nativeEquipment = value; },
    paused(value) { paused = value; }, loaded(value) { loaded = value; },
    duringEquipment(fn) { duringEquipment = fn; }, duringInventory(fn) { duringInventory = fn; },
    message(event, message) { emitter.emit(event, { message }); },
    confirm() { const pending = service.pendingAppearance; assert.ok(pending);
      emitter.emit('updateAppearanceMessage', { message: { idx: pending.actorIdx, data: pending.data } }); },
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
  assert.equal(h.service.blocksGameplayInput, true); assert.equal(h.applied.length, 2);
  h.confirm(); h.settle();
  assert.deepEqual(h.applied, ['unequip', 'open', ['inventory', inventory(30)], ['equipment', equipment(30)]]);
  assert.equal(h.service.blocksEquipmentInput, false);
  h.update(5001);
  assert.equal(h.applied.length, 4, 'A periodic successful observation does not dispatch inventory again');
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
    h.service.markSpawnReady(current); h.settle();
    assert.deepEqual(h.applied.slice(1), [['inventory', inventory(2)], ['equipment', equipment(2)]]);
  });
}

test('game loading only retains loadout for the generated multiplayer save', () => {
  const h = harness(); h.start(1, false);
  h.emitter.emit('gameLoad', { isCausedBySkyrimPlatform: true }); h.settle();
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
  h.retryInventory(false); h.settle();
  assert.equal(h.service.blocksEquipmentInput, false);
  assert.deepEqual(h.applied.map(x => x[0]), ['inventory', 'inventory', 'equipment']);
});

test('a hot reload retains the open preview gate and queued inventory', () => {
  const h = harness(); h.start(); h.update(); h.update(300); h.shown(true); h.update();
  h.message('setInventoryMessage', { inventory: inventory(30) });
  const reloaded = harness(h.storage, h.clock);
  reloaded.shown(true); reloaded.update(20000);
  assert.equal(reloaded.service.blocksEquipmentInput, true);
  assert.equal(reloaded.applied.length, 0);
  reloaded.shown(false); reloaded.update(); reloaded.update(); reloaded.confirm(); reloaded.settle();
  assert.deepEqual(reloaded.applied, [['inventory', inventory(30)], ['equipment', equipment(1)]]);
});

test('a reset during the inventory guard cannot apply the previous character loadout', () => {
  const h = harness(); h.start(1, false);
  h.emitter.once('queryBlockSetInventoryEvent', () => h.emitter.emit('connectionDisconnect'));
  h.update(); assert.equal(h.applied.length, 0);
});

test('a reset from native equipment application cannot continue into stale inventory', () => {
  const h = harness(); h.start(1, false);
  h.duringEquipment(() => h.emitter.emit('connectionDisconnect'));
  h.settle(); assert.deepEqual(h.applied, [['inventory', inventory(1)], ['equipment', equipment(1)]]);
  assert.equal(h.storage.pcInv, undefined);
});

test('an inventory replacement raised during native application remains pending', () => {
  const h = harness(); h.start(1, false);
  h.duringInventory(() => {
    h.duringInventory(() => {});
    h.message('setInventoryMessage', { inventory: inventory(20) });
  });
  h.update(); assert.equal(h.service.blocksEquipmentInput, true);
  h.settle(); assert.deepEqual(h.applied.at(-2), ['inventory', inventory(20)]);
  assert.equal(h.service.blocksEquipmentInput, false);
});

test('an equipment echo during normal play does not rebuild inventory or cause an equip loop', () => {
  const h = harness(); h.start(1, false); h.settle();
  h.message('updateEquipmentMessage', { idx: 1, data: equipment(20) }); h.update();
  assert.equal(h.applied.length, 2);
  assert.equal(h.service.blocksEquipmentInput, false);
  h.message('setRaceMenuOpenMessage', { open: true });
  h.update(); h.update(300); h.shown(true); h.update(); h.shown(false); h.update(); h.update(); h.confirm(); h.settle();
  assert.deepEqual(h.applied.at(-1), ['equipment', equipment(20)]);
});

test('creation opens only after projection release and human editing has no projection deadline', () => {
  const h = harness(); h.emitter.emit('campaignProjectionPending'); h.start();
  h.update(20000); h.tick();
  assert.equal(h.applied.length, 0);
  assert.equal(h.service.projectionReadiness.ready, true);
  assert.equal(h.service.blocksGameplayInput, true);
  h.emitter.emit('campaignProjectionReady'); h.update(); h.update(300);
  assert.deepEqual(h.applied, ['unequip', 'open']);
  h.shown(true); h.update(); h.tick(20 * 60 * 1000);
  assert.equal(h.service.projectionReadiness.error, undefined);
  assert.equal(h.service.blocksGameplayInput, true);
  h.shown(false); h.update(); h.update();
  assert.ok(h.service.pendingAppearance);
  h.confirm(); h.settle(); assert.equal(h.service.blocksGameplayInput, false);
});

test('an absent native menu or missing appearance acknowledgement fails closed', () => {
  for (const stage of ['opening', 'confirming']) {
    const h = harness(), failures = [];
    h.emitter.on('playerLoadoutFailed', failure => failures.push(failure));
    h.start(); h.update(); h.update(300);
    if (stage === 'confirming') {
      h.shown(true); h.update(); h.shown(false); h.update(); h.update();
    }
    h.tick(30000);
    assert.equal(failures.length, 1); assert.match(failures[0].error, new RegExp(stage));
    assert.equal(h.service.blocksGameplayInput, true);
    assert.equal(h.service.pendingAppearance, undefined);
    h.tick(30000); assert.equal(failures.length, 1);
    assert.deepEqual(h.applied, ['unequip', 'open']);
  }
});

test('only the matching self appearance echo completes creation and latest loadout then restores', () => {
  const h = harness(), completed = [];
  h.emitter.on('playerCreationComplete', () => completed.push(true));
  h.start(); h.update(); h.update(300); h.shown(true); h.update();
  h.shown(false); h.update(); h.update();
  const pending = h.service.pendingAppearance;
  h.message('updateAppearanceMessage', { idx: 2, data: pending.data });
  h.message('updateAppearanceMessage', { idx: 1, data: { ...pending.data, name: 'Other name' } });
  h.message('setRaceMenuOpenMessage', { open: false });
  h.update(); assert.equal(h.service.pendingAppearance, pending);
  assert.deepEqual(h.applied, ['unequip', 'open']);
  h.message('setInventoryMessage', { inventory: inventory(60) });
  h.message('updateAppearanceMessage', { idx: 1, data: Object.fromEntries(Object.entries(pending.data).reverse()) });
  h.retryInventory(true); h.update();
  assert.equal(h.service.pendingAppearance, undefined); assert.equal(h.service.blocksGameplayInput, true);
  assert.equal(completed.length, 0);
  h.retryInventory(false); h.settle();
  assert.equal(completed.length, 1); assert.equal(h.service.blocksGameplayInput, false);
  assert.deepEqual(h.applied.at(-2), ['inventory', inventory(60)]);
});

test('same-player reprojection preserves editing without reopening or overwriting the draft', () => {
  const h = harness(); const old = h.start();
  h.update(); h.update(300); h.shown(true); h.update();
  h.emitter.emit('campaignProjectionPending'); h.service.cancelSpawn(old);
  const generation = h.service.beginSpawn({ ...spawn(1), idx: 22 });
  assert.equal(h.service.shouldApplySpawnAppearance(generation), false);
  h.service.markSpawnReady(generation); h.update();
  assert.equal(h.service.projectionReadiness.ready, true);
  h.emitter.emit('campaignProjectionReady'); h.update();
  assert.deepEqual(h.applied, ['unequip', 'open']);
  h.shown(false); h.update(); h.update();
  assert.equal(h.service.pendingAppearance.actorIdx, 22);
  h.confirm(); h.settle(); assert.equal(h.service.blocksGameplayInput, false);
});

test('same-player reprojection retains a final snapshot and can confirm it from the authoritative baseline', () => {
  for (const accepted of [false, true]) {
    const h = harness(); const old = h.start();
    h.update(); h.update(300); h.shown(true); h.update(); h.shown(false); h.update(); h.update();
    const snapshot = h.service.pendingAppearance.data;
    h.emitter.emit('campaignProjectionPending'); h.service.cancelSpawn(old); h.tick(25000);
    const generation = h.service.beginSpawn({ ...spawn(1, !accepted), idx: 22,
      appearance: accepted ? snapshot : { ...snapshot, name: 'Old server appearance' } });
    h.service.markSpawnReady(generation); h.update();
    assert.equal(h.service.pendingAppearance, undefined, 'No appearance can leave during projection');
    h.emitter.emit('campaignProjectionReady');
    if (!accepted) {
      assert.deepEqual(h.service.pendingAppearance.data, snapshot);
      assert.equal(h.service.pendingAppearance.actorIdx, 22);
      h.confirm();
    }
    h.settle(); assert.equal(h.service.blocksGameplayInput, false);
    assert.deepEqual(h.applied.filter(call => call === 'open'), ['open']);
  }
});

test('actor replacement or a conflicting completed baseline cannot steal an unfinished creation', () => {
  const h = harness(); const old = h.start();
  h.update(); h.update(300); h.shown(true); h.update();
  h.emitter.emit('campaignProjectionPending'); h.service.cancelSpawn(old);
  assert.throws(() => h.service.beginSpawn(spawn(2)), /replace the player/);
  assert.throws(() => h.service.beginSpawn(spawn(1, false)), /revoked/);
});

test('ordinary inventory replication does not repeatedly lock movement after initial loadout application', () => {
  const h = harness(); h.start(1, false); h.settle();
  assert.equal(h.service.blocksGameplayInput, false);
  h.message('setInventoryMessage', { inventory: inventory(99) });
  h.retryInventory(true); h.update();
  assert.equal(h.service.blocksEquipmentInput, true);
  assert.equal(h.service.blocksGameplayInput, false);
});

test('native menu-open events still prepare an unequipped preview on the next safe update', () => {
  const h = harness(); h.start(); h.events.emit('menuOpen', { name: 'RaceSex Menu' });
  assert.equal(h.applied.length, 0);
  h.shown(true); h.update();
  assert.deepEqual(h.applied, ['unequip']);
  h.tick(60000); assert.equal(h.service.projectionReadiness.error, undefined);
});

test('a new authoritative creation request supersedes the previous restoration', () => {
  const h = harness(); const old = h.start();
  h.update(); h.update(300); h.shown(true); h.update(); h.shown(false); h.update(); h.update();
  h.confirm();
  h.emitter.emit('campaignProjectionPending'); h.service.cancelSpawn(old);
  const generation = h.service.beginSpawn(spawn(1, true)); h.service.markSpawnReady(generation);
  h.update(); assert.equal(h.applied.filter(call => call === 'open').length, 1);
  h.emitter.emit('campaignProjectionReady'); h.update(); h.update(300); h.shown(true); h.update();
  assert.equal(h.applied.filter(call => call === 'open').length, 2);
  assert.equal(h.service.projectionReadiness.error, undefined);
  assert.equal(h.service.blocksGameplayInput, true);
});

test('a native error from superseded loadout work cannot fail its replacement', () => {
  const h = harness(), failures = [];
  h.emitter.on('playerLoadoutFailed', failure => failures.push(failure));
  h.start(1, false);
  h.duringEquipment(() => {
    h.duringEquipment(() => {});
    h.start(2, false); throw Error('Obsolete native work failed');
  });
  h.settle(); assert.equal(failures.length, 0);
  h.settle(); assert.equal(h.service.projectionReadiness.ready, true);
});

test('successful dispatch cannot release creation before inventory and equipment are observed on later updates', () => {
  const h = harness(), completed = [];
  h.emitter.on('playerCreationComplete', () => completed.push(true));
  h.start(); h.update(); h.update(300); h.shown(true); h.update();
  h.shown(false); h.update(); h.update(); h.confirm();
  h.retryInventory(true); h.delayedEquipment(true);
  h.update();
  assert.deepEqual(h.applied.slice(2), [['inventory', inventory(1)]]);
  for (let i = 0; i < 10; ++i) h.update();
  assert.equal(h.applied.length, 3, 'No additive retry every frame');
  assert.equal(h.service.blocksGameplayInput, true);
  h.observeInventory(inventory(1)); h.update();
  assert.equal(h.applied.length, 3, 'One observed sample is insufficient');
  h.update();
  assert.deepEqual(h.applied.at(-1), ['equipment', equipment(1)]);
  assert.equal(h.service.blocksGameplayInput, true);
  for (let i = 0; i < 10; ++i) h.update();
  assert.equal(h.applied.length, 4, 'An equipment retry cannot add or rebuild inventory');
  assert.equal(completed.length, 0);
  h.observeEquipment(equipment(1)); h.update();
  assert.equal(completed.length, 0);
  h.update();
  assert.equal(completed.length, 1);
  assert.equal(h.service.blocksGameplayInput, false);
});

test('native no-op restoration remains gated and fails within its explicit deadline', () => {
  for (const stage of ['inventory', 'equipment']) {
    const h = harness(), failures = [];
    h.emitter.on('playerLoadoutFailed', e => failures.push(e));
    h.start(1, false);
    h.retryInventory(stage === 'inventory'); h.delayedEquipment(stage === 'equipment');
    h.settle();
    assert.equal(h.service.blocksGameplayInput, true);
    assert.equal(h.service.projectionReadiness.ready, false);
    h.tick(30000);
    assert.equal(failures.length, 1);
    assert.match(failures[0].error, /timed out/);
  }
});

test('loadout waits for unpaused loaded native state and does not equip against an unsettled replacement inventory', () => {
  const h = harness(); h.start(1, false);
  h.paused(true); h.update(); h.update(250); assert.equal(h.applied.length, 0);
  h.paused(false); h.loaded(false); h.update(); assert.equal(h.applied.length, 0);
  h.loaded(true); h.update(); h.update();
  h.message('setInventoryMessage', { inventory: inventory(20) });
  h.update(); assert.deepEqual(h.applied.map(x => x[0]), ['inventory', 'inventory']);
  assert.equal(h.service.blocksGameplayInput, true);
  h.settle(); assert.equal(h.service.blocksGameplayInput, false);
});

test('a replacement equipment snapshot resets the required stable native observations', () => {
  const h = harness(); h.start(1, false); h.update(); h.update(); h.update(); h.update();
  h.message('updateEquipmentMessage', { idx: 1, data: equipment(20) });
  h.delayedEquipment(true); h.update();
  assert.equal(h.service.blocksGameplayInput, true);
  h.observeEquipment(equipment(20)); h.update();
  assert.equal(h.service.blocksGameplayInput, true);
  h.update(); assert.equal(h.service.blocksGameplayInput, false);
});
