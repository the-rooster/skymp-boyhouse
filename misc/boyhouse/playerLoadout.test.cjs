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
const nativeHelperCode = ts.transpileModule(fs.readFileSync(path.join(root,
  'src/sync/nativePlayerLoadout.ts'), 'utf8'), { compilerOptions: {
  module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020,
} }).outputText;
const nativeHelper = { exports: {} };
vm.runInNewContext(nativeHelperCode, { module: nativeHelper, exports: nativeHelper.exports });

const inventory = baseId => ({ entries: [{ baseId, count: 1, worn: true }] });
const equipment = baseId => ({ inv: inventory(baseId), numChanges: 1 });
const spawn = (idx, creation = true) => ({ idx, refrId: 0xff000000 + idx, isMe: true,
  equipment: equipment(idx), props: { isRaceMenuOpen: creation, inventory: inventory(idx) } });

function harness(storage = {}, clock = { now: 10000 }) {
  const events = new EventEmitter(), emitter = new EventEmitter(), applied = [];
  const requests = [], cancellations = [];
  let activeEpoch, nextEpoch = 0, job, jobSettled = false, statusOverride, lastAttempt = -Infinity;
  let mutationPending = false, uncertain = false;
  let shown = false, badMenu = false, retryInventory = false;
  let nativeInventory, nativeEquipment, delayedEquipment = false, paused = false, loaded = true;
  let duringEquipment = () => {}, duringInventory = () => {};
  let appearance = { name: 'Finished character', raceId: 1, options: [0.2], tints: [] };
  const player = { unequipAll: () => { applied.push('unequip'); nativeEquipment = undefined; }, is3DLoaded: () => loaded };
  const applyNativeEquipment = eq => {
    applied.push(['equipment', eq]);
    if (!delayedEquipment) nativeEquipment = structuredClone(eq);
    duringEquipment();
  };
  const applyNativeInventory = inv => {
    applied.push(['inventory', inv]);
    if (!retryInventory) nativeInventory = structuredClone(inv);
    duringInventory();
  };
  const dependencies = {
    '../../sync/nativePlayerLoadout': nativeHelper.exports,
    './clientListener': { ClientListener: class {} },
    '../../sync/appearance': { getAppearance: () => structuredClone(appearance) },
    '../../sync/equipment': {
      isBadMenuShown: () => badMenu,
      isPlayerEquipmentSettled: (actor, eq) => JSON.stringify(eq) === JSON.stringify(nativeEquipment),
      applyPlayerEquipment() { assert.fail('Self restoration must use the native epoch API'); },
    },
    '../../sync/inventory': {
      getEquipmentInventory: () => nativeInventory || { entries: [] },
      isInventorySettled: (actor, inv) => JSON.stringify(inv) === JSON.stringify(nativeInventory),
      applyInventory() { assert.fail('Self restoration must never call the legacy inventory writer'); },
    },
  };
  const module = { exports: {} };
  vm.runInNewContext(compiled, { module, exports: module.exports, Date: { now: () => clock.now },
    require: id => {
      assert.ok(id in dependencies, `Unexpected dependency ${id}`); return dependencies[id];
    },
  });
  const platform = { storage,
    Game: { getPlayer: () => player, showRaceMenu: () => applied.push('open') },
    Ui: { isMenuOpen: () => shown },
    Utility: { isInMenuMode: () => paused },
    playerLoadoutApiVersion: 1,
    beginPlayerLoadout() { activeEpoch = `epoch-${++nextEpoch}`; job = undefined; return activeEpoch; },
    requestPlayerLoadout(epoch, target) {
      if (epoch !== activeEpoch) return { epoch, status: 'superseded', mutationPending, uncertain };
      requests.push({ epoch, target: structuredClone(target) });
      if (job?.revision !== target.revision) { job = structuredClone(target); jobSettled = false; lastAttempt = -Infinity; }
      else assert.deepEqual(job, target, 'Same revision must carry the same target');
      return { epoch, revision: job.revision, status: 'queued', mutationPending, uncertain };
    },
    getPlayerLoadoutStatus(epoch) {
      if (epoch !== activeEpoch) return { epoch, status: 'superseded', mutationPending, uncertain };
      const status = { epoch, revision: job?.revision, status: 'settled', mutationPending, uncertain };
      if (statusOverride) return { ...status, ...statusOverride };
      if (!job) return { epoch, status: 'idle', mutationPending, uncertain };
      if (jobSettled) return status;
      if (job.inventory && JSON.stringify(job.inventory) !== JSON.stringify(nativeInventory)) {
        if (clock.now - lastAttempt >= 250) {
          lastAttempt = clock.now;
          applyNativeInventory(job.inventory);
        }
        return { ...status, status: 'working' };
      }
      const targetEquipment = job.equipment && { ...job.equipment, numChanges: 1 };
      if (targetEquipment && JSON.stringify(targetEquipment) !== JSON.stringify(nativeEquipment)) {
        if (clock.now - lastAttempt >= 250) {
          lastAttempt = clock.now;
          applyNativeEquipment(targetEquipment);
        }
        return { ...status, status: 'working' };
      }
      jobSettled = true;
      return status;
    },
    cancelPlayerLoadout(epoch) { cancellations.push(epoch); if (epoch === activeEpoch) { activeEpoch = undefined; job = undefined; } },
  };
  const service = new module.exports.PlayerLoadoutService(platform, { on: (...args) => events.on(...args), emitter });
  return { service, emitter, applied, storage, clock, events, requests, cancellations, platform,
    nativeStatus(value) { statusOverride = value; }, invalidateNative() { activeEpoch = undefined; },
    mutationBarrier(pending, unknown = false) { mutationPending = pending; uncertain = unknown; },
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
  assert.equal(h.requests[0].target.inventory.entries[0].baseId, 2);
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
  h.badMenu(false); h.retryInventory(true); h.update(); h.update();
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
  assert.equal(reloaded.service.projectionReadiness.ready, false);
  const fresh = reloaded.service.beginSpawn({ ...spawn(1), props: {
    isRaceMenuOpen: true, inventory: inventory(30),
  } });
  reloaded.service.markSpawnReady(fresh);
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
  h.nativeStatus({ status: 'settled' }); // A native status alone is never sufficient evidence.
  h.update();
  assert.equal(h.requests.length, 1);
  assert.deepEqual(h.applied.slice(2), []);
  for (let i = 0; i < 10; ++i) h.update();
  assert.equal(h.requests.length, 1, 'An unchanged pending target is requested once');
  assert.equal(h.service.blocksGameplayInput, true);
  h.observeInventory(inventory(1)); h.update();
  h.update();
  assert.equal(h.service.blocksGameplayInput, true);
  for (let i = 0; i < 10; ++i) h.update();
  assert.equal(h.requests.length, 1);
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
  h.update(); assert.equal(h.requests.length, 2);
  assert.deepEqual(h.applied.map(x => x[0]), ['inventory'], 'Changed target does not poll obsolete equipment work');
  h.update(250); assert.deepEqual(h.applied.map(x => x[0]), ['inventory', 'inventory']);
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

test('native requests preserve exact variant targets and dedupe unchanged snapshots', () => {
  const h = harness(), inv = { entries: [
    { baseId: 10, count: 1, name: 'Twin', health: Math.fround(1.04), chargePercent: 0 },
    { baseId: 10, count: 1, name: 'Twin', health: Math.fround(1.04), chargePercent: 75 },
  ] }, eq = { inv: { entries: [{ ...inv.entries[1], wornLeft: true }] }, numChanges: 5, voiceSpell: 42 };
  const generation = h.service.beginSpawn({ ...spawn(1, false), equipment: eq, props: { inventory: inv } });
  h.service.markSpawnReady(generation); h.nativeStatus({ status: 'queued' }); h.update();
  assert.deepEqual(h.requests[0].target, { revision: 1, inventory: inv,
    equipment: { inv: eq.inv, voiceSpell: 42 } });
  h.message('setInventoryMessage', { inventory: structuredClone(inv) });
  h.message('updateEquipmentMessage', { idx: 1, data: { ...eq, numChanges: 6 } });
  for (let i = 0; i < 5; ++i) h.update(250);
  assert.equal(h.requests.length, 1);
  const changed = structuredClone(inv); changed.entries[0].chargePercent = 5;
  h.message('setInventoryMessage', { inventory: changed }); h.update();
  assert.equal(h.requests.length, 2);
  assert.equal(h.requests[1].epoch, h.requests[0].epoch);
  assert.equal(h.requests[1].target.revision, 2);
  assert.equal(h.requests[0].target.inventory.entries[0].chargePercent, 0, 'An old immutable request is never patched');
});

test('native settlement requires two later readbacks and cannot reuse matches observed during work', () => {
  for (const status of [{ status: 'working' }, { status: 'observing' }, { status: 'settled', mutationPending: true }]) {
    const h = harness(); h.start(1, false);
    h.observeInventory(inventory(1)); h.observeEquipment(equipment(1));
    h.nativeStatus(status); h.update(); h.update(); h.update(); h.update();
    assert.equal(h.service.blocksGameplayInput, true);
    h.nativeStatus({ status: 'settled' }); h.update();
    assert.equal(h.service.blocksGameplayInput, true, 'Earlier matches are not post-settlement evidence');
    h.update(); assert.equal(h.service.blocksGameplayInput, false);
  }
});

test('reordered and split equivalent inventory snapshots retain one active native revision', () => {
  const h = harness(), a = { baseId: 10, count: 3, name: 'A', chargePercent: 0 },
    b = { baseId: 10, count: 2, name: 'B', chargePercent: 75 };
  const inv = { entries: [a, b] }, eq = { inv: { entries: [{ ...a, wornLeft: true }] }, numChanges: 1 };
  const generation = h.service.beginSpawn({ ...spawn(1, false), equipment: eq, props: { inventory: inv } });
  h.service.markSpawnReady(generation); h.nativeStatus({ status: 'observing' }); h.update();
  h.message('setInventoryMessage', { inventory: { entries: [{ ...b, count: 1 }, { ...a, count: 1 },
    { ...b, count: 1, worn: true }, { ...a, count: 2 }] } });
  h.message('updateEquipmentMessage', { idx: 1, data: { inv: { entries: [b, { ...a, count: 1, wornLeft: true }] }, numChanges: 2 } });
  h.update(); h.update(); assert.equal(h.requests.length, 1);
  assert.deepEqual(h.requests[0].target.inventory, inv, 'The original request remains immutable');
  h.message('setInventoryMessage', { inventory: { entries: [{ ...a, chargePercent: undefined }, b] } });
  h.update(); assert.equal(h.requests.length, 2);
  assert.equal(h.requests[1].target.revision, 2, 'Absent charge is a distinct target');
});

test('an invalid replacement cannot hide behind an otherwise equivalent active target key', () => {
  for (const invalid of [
    { entries: [{ baseId: 1, count: 1 }, { baseId: 1, count: 1 }, { baseId: 1, count: -1 }] },
    { entries: [{ baseId: 1, count: 1, health: NaN }] },
    { entries: [{ baseId: 1, count: 1, unknown: undefined }] },
  ]) {
    const h = harness(), failures = [];
    h.emitter.on('playerLoadoutFailed', event => failures.push(event));
    h.start(1, false); h.nativeStatus({ status: 'observing' }); h.update();
    h.message('setInventoryMessage', { inventory: invalid }); h.update();
    assert.equal(failures.length, 1); assert.match(failures[0].error, /Invalid native player loadout/);
    assert.equal(h.requests.length, 1); assert.deepEqual(h.cancellations, [h.requests[0].epoch]);
    assert.equal(h.service.blocksGameplayInput, true); assert.deepEqual(h.applied, []);
  }
});

test('old clients fail clearly before preview or any native restoration begins', () => {
  for (const missing of ['playerLoadoutApiVersion', 'beginPlayerLoadout', 'requestPlayerLoadout',
    'getPlayerLoadoutStatus', 'cancelPlayerLoadout']) {
    const h = harness(), failures = [];
    h.emitter.on('playerLoadoutFailed', event => failures.push(event));
    delete h.platform[missing]; h.start(); h.update();
    assert.equal(failures.length, 1); assert.match(failures[0].error, /Updated Skyrim Platform client required/);
    assert.deepEqual(h.applied, []); assert.deepEqual(h.requests, []);
    assert.equal(h.service.blocksGameplayInput, true);
  }
});

test('native epoch invalidation never silently reopens and reapplies the old campaign', () => {
  const h = harness(), failures = [];
  h.emitter.on('playerLoadoutFailed', event => failures.push(event));
  h.start(1, false); h.update(); h.invalidateNative(); h.update(); h.update();
  assert.equal(failures.length, 1); assert.match(failures[0].error, /fresh player baseline/);
  assert.equal(h.requests.length, 1); assert.equal(h.service.blocksGameplayInput, true);
  assert.deepEqual(h.applied, []);
  h.start(2, false); h.settle();
  assert.equal(h.requests.length, 2); assert.notEqual(h.requests[1].epoch, h.requests[0].epoch);
  assert.equal(h.service.blocksGameplayInput, false);
});

test('native failures and mismatched status identities fail closed and cancel the request', () => {
  for (const status of [{ status: 'failed', error: 'Unsupported native extra' }, { epoch: 'wrong' }, { revision: 99 }, { status: 'idle' }]) {
    const h = harness(), failures = [];
    h.emitter.on('playerLoadoutFailed', event => failures.push(event));
    h.start(1, false); h.update(); h.nativeStatus(status); h.update();
    assert.equal(failures.length, 1); assert.equal(h.service.blocksGameplayInput, true);
    assert.deepEqual(h.cancellations, [h.requests[0].epoch]);
    assert.deepEqual(h.applied, []);
  }
});

test('reset destroy and creation transitions cancel queued native requests immediately', () => {
  for (const stop of [h => h.emitter.emit('connectionDisconnect'),
    h => h.message('destroyActorMessage', { idx: 1 }),
    h => h.message('setRaceMenuOpenMessage', { open: true }),
    h => h.service.cancelSpawn(h.service.generation),
    h => h.emitter.emit('gameLoad', { isCausedBySkyrimPlatform: false })]) {
    const h = harness(); h.start(1, false); h.update();
    stop(h);
    assert.deepEqual(h.cancellations, [h.requests[0].epoch]);
    assert.deepEqual(h.applied, []);
  }
});

test('projection cancellation requires a fresh baseline and can restore it before projection release', () => {
  const h = harness(); h.start(1, false); h.update();
  h.emitter.emit('campaignProjectionPending');
  assert.deepEqual(h.cancellations, [h.requests[0].epoch]);
  h.update(); h.update();
  assert.equal(h.requests.length, 1); assert.equal(h.service.projectionReadiness.ready, false);
  h.start(2, false); h.settle();
  assert.equal(h.requests.length, 2); assert.notEqual(h.requests[1].epoch, h.requests[0].epoch);
  assert.equal(h.service.projectionReadiness.ready, true);
  h.emitter.emit('campaignProjectionReady');
  assert.equal(h.service.blocksGameplayInput, false);
});

test('normal inventory updates leave equipment unmanaged and later drift starts a fresh revision', () => {
  const h = harness(); h.start(1, false); h.settle();
  h.message('updateEquipmentMessage', { idx: 1, data: equipment(20) });
  h.message('setInventoryMessage', { inventory: inventory(30) }); h.settle();
  assert.equal(h.requests.length, 2);
  assert.equal(h.requests[1].target.equipment, undefined);
  assert.equal(h.requests[1].target.inventory.entries[0].baseId, 30);
  h.observeInventory(inventory(99)); h.update(5001);
  assert.equal(h.requests.length, 3);
  assert.equal(h.requests[2].epoch, h.requests[1].epoch);
  assert.equal(h.requests[2].target.revision, h.requests[1].target.revision + 1);
  h.settle(); assert.equal(h.service.blocksEquipmentInput, false);
});

test('character preview waits for cancelled native mutations to drain', () => {
  for (const retained of [false, true]) {
    const h = harness();
    if (retained) {
      h.start(1, false); h.update();
      h.message('setRaceMenuOpenMessage', { open: true });
    } else h.start();
    h.mutationBarrier(true); h.update(); h.update(1000);
    assert.deepEqual(h.applied, [], 'No preview unequip may race a previously claimed mutation');
    assert.equal(h.cancellations.length, 1);
    assert.equal(h.service.blocksGameplayInput, true);
    h.mutationBarrier(false); h.update();
    assert.deepEqual(h.applied, ['unequip']);
    h.update(300); assert.deepEqual(h.applied, ['unequip', 'open']);
  }
});

test('uncertain cancellation cannot be treated as a safe preview boundary', () => {
  const h = harness(), failures = [];
  h.emitter.on('playerLoadoutFailed', event => failures.push(event));
  h.start(); h.mutationBarrier(false, true); h.update();
  assert.equal(failures.length, 1); assert.match(failures[0].error, /uncertain/);
  assert.deepEqual(h.applied, []); assert.equal(h.service.blocksGameplayInput, true);
});

test('a globally pending mutation blocks release even when status and observations say settled', () => {
  const h = harness(); h.start(1, false);
  h.observeInventory(inventory(1)); h.observeEquipment(equipment(1));
  h.nativeStatus({ status: 'settled' }); h.mutationBarrier(true);
  h.update(); h.update(); h.update();
  assert.equal(h.service.blocksGameplayInput, true);
  h.mutationBarrier(false); h.update();
  assert.equal(h.service.blocksGameplayInput, true);
  h.update();
  assert.equal(h.service.blocksGameplayInput, false);
  h.invalidateNative(); h.update();
  assert.match(h.service.projectionReadiness.error, /fresh player baseline/,
    'Independent engine epoch invalidation is checked even between periodic inventory checks');
});

test('hot reload retires stored native work and cannot reuse old spawn readiness', () => {
  const old = harness(), generation = old.start(1, false); old.update();
  const epoch = old.requests[0].epoch;
  const next = harness(old.storage, old.clock);
  assert.deepEqual(next.cancellations, [epoch]);
  next.service.markSpawnReady(generation); next.update(); next.tick(60000);
  assert.equal(next.requests.length, 0);
  assert.equal(next.service.blocksGameplayInput, true);
  assert.equal(next.service.projectionReadiness.ready, false);
  assert.equal(next.service.projectionReadiness.error, undefined, 'A stale deadline does not fail the replacement baseline');
  next.start(2, false); next.settle();
  assert.equal(next.requests.length, 1);
  assert.equal(next.requests[0].target.inventory.entries[0].baseId, 2);
  assert.equal(next.service.blocksGameplayInput, false);
});

test('generated-save bootstrap cannot begin a native epoch before fresh spawn readiness', () => {
  const h = harness(), generation = h.service.beginSpawn(spawn(1, false));
  h.events.emit('preLoadGame');
  h.emitter.emit('gameLoad', { isCausedBySkyrimPlatform: true });
  h.update(); assert.equal(h.requests.length, 0);
  h.service.markSpawnReady(generation); h.settle();
  assert.equal(h.requests.length, 1);
  assert.equal(h.service.blocksGameplayInput, false);
});

test('real projection coordinator releases full transfers after their self loadouts settle', () => {
  const load = (file, dependencies = {}) => {
    const code = ts.transpileModule(fs.readFileSync(path.join(root, 'src', file), 'utf8'), {
      compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
    }).outputText;
    const module = { exports: {} };
    vm.runInNewContext(code, { module, exports: module.exports, require: id => {
      assert.ok(id in dependencies, id); return dependencies[id];
    } });
    return module.exports;
  };
  const { WorldProjectionCoordinator } = load('campaign/worldProjectionCoordinator.ts', {
    './worldProjection': load('campaign/worldProjection.ts'),
  });
  const h = harness(), messages = [], baselines = [], sessionId = 'loadout-projection';
  const envelope = sequence => ({ customPacketType: 'boyhouse.projection', protocolVersion: 2,
    sessionId, sequence, token: sequence.toString(16).padStart(32, '0') });
  const engine = {
    cancel() { h.service.cancelSpawn(h.service.generation); },
    drained: () => ({ ready: true }),
    apply(_projection, baseline) {
      const self = baseline.messages.find(message => message.isMe);
      baselines.push(self.idx);
      const generation = h.service.beginSpawn(self); h.service.markSpawnReady(generation);
    },
    observe: () => h.service.projectionReadiness,
    release() {}, disconnect() {},
  };
  const coordinator = new WorldProjectionCoordinator(engine, {
    block() { h.emitter.emit('campaignProjectionPending'); },
    send(message) {
      messages.push(message);
      if (message.action === 'ack') coordinator.receive({ ...envelope(message.sequence), kind: 'ready' });
    },
    drain: () => true, release: () => true,
    close(error) { assert.fail(error); },
  }, () => h.clock.now);
  const transfer = (sequence, idx) => {
    const actor = 0xff000001, frame = envelope(sequence);
    const baseline = { schemaVersion: 1, actor, packetEpoch: frame.token, messages: [
      { t: 32, eventSources: [], updateOwnerFunctions: [], updateNeighborFunctions: [] },
      { ...spawn(1, false), t: 33, idx, transform: { pos: [0, 0, 0], rot: [0, 0, 0], worldOrCell: 60 }, customPropsJsonDumps: [] },
    ] };
    const text = JSON.stringify(baseline);
    assert.equal(coordinator.receive({ ...frame, kind: 'begin', count: 0, chunks: 0,
      metadata: { schemaVersion: 1, actor, campaignOwner: actor, instance: '', contextEpoch: `1:${sequence}`, identity: '' },
      baselineLength: text.length, baselineChunks: 1 }), true);
    h.update(); assert.equal(h.service.projectionReadiness.ready, false);
    assert.equal(coordinator.receive({ ...frame, kind: 'baseline', index: 0, data: text }), true);
    coordinator.advance(false);
    for (let i = 0; i < 8; ++i) {
      h.update(250); const before = coordinator.status.phase; coordinator.advance(true);
      if (before !== 'ready' && coordinator.status.phase === 'ready') h.emitter.emit('campaignProjectionReady');
    }
    assert.equal(coordinator.status.phase, 'ready');
    assert.equal(h.service.projectionReadiness.ready, true);
    assert.equal(h.service.blocksGameplayInput, false);
  };
  coordinator.connect(sessionId);
  transfer(1, 1);
  assert.equal(coordinator.receive({ ...envelope(2), kind: 'delta' }), false);
  assert.equal(coordinator.status.phase, 'ready', 'There is no accepted delta-only blocking path');
  transfer(2, 22); // begin without a preceding pause still contains a full self baseline.
  assert.equal(coordinator.receive({ ...envelope(3), kind: 'pause' }), true);
  h.update(); h.update(); assert.equal(h.requests.length, 2);
  assert.equal(h.service.projectionReadiness.ready, false);
  transfer(4, 23);
  assert.deepEqual(baselines, [1, 22, 23]);
  assert.equal(messages.filter(message => message.action === 'ack').length, 3);
});

test('character preview never strips a temporary enchantment from a worn item', () => {
  for (const shown of [false, true]) {
    for (const hand of ['worn', 'wornLeft']) {
      const h = harness(), failures = [];
      const native = { entries: [{ baseId: 10, count: 1, [hand]: true,
        enchantmentId: 20, maxCharge: 100, removeEnchantmentOnUnequip: true }] };
      h.emitter.on('playerLoadoutFailed', value => failures.push(value));
      h.start(); h.observeInventory(native); h.shown(shown); h.update();
      assert.equal(failures.length, 1); assert.match(failures[0].error, /preview.*temporary enchantment/);
      assert.deepEqual(h.applied, [], 'Neither unequipAll nor menu opening may precede refusal');
      assert.equal(native.entries[0].removeEnchantmentOnUnequip, true);
      assert.equal(h.service.blocksGameplayInput, true);
    }
  }
  const h = harness(); h.start();
  h.observeInventory({ entries: [{ baseId: 10, count: 1, enchantmentId: 20,
    maxCharge: 100, removeEnchantmentOnUnequip: true }] });
  h.update(); assert.deepEqual(h.applied, ['unequip'], 'An unworn temporary enchantment is not touched by preview');
});
