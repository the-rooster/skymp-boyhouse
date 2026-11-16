const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const { EventEmitter } = require('node:events');
const { NativeReferenceOperations } = require('../../skymp5-client/src/campaign/nativeReferenceOperations.ts');
const root = path.resolve(__dirname, '../../skymp5-client');
const ts = require(require.resolve('typescript', { paths: [root, path.resolve(root, '../skymp5-server')] }));
function load(file, dependencies = {}, extra = {}) {
  const compiled = ts.transpileModule(fs.readFileSync(path.join(root, 'src', file), 'utf8'), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
  }).outputText;
  const module = { exports: {} };
  vm.runInNewContext(compiled, { module, exports: module.exports, ...extra, require: name => {
    assert.ok(name in dependencies, `Unexpected dependency ${name}`); return dependencies[name];
  } }, { filename: file });
  return module.exports;
}
const { IdManager } = load('lib/idManager.ts');
const create = (idx = 1, refrId = 0xff000001, isMe = true) => ({
  t: 33, idx, refrId, isMe, transform: { worldOrCell: 60, pos: [100, 200, 300], rot: [0, 0, 72] },
  props: {}, customPropsJsonDumps: [],
});

for (const inProps of [false, true]) test(`initial corpse state from ${inProps ? 'props' : 'main record'} survives movement hints and changes only with authority`, () => {
  const f = fixture(), message = create(1, 0xff000001, false);
  if (inProps) message.props.isDead = true; else message.isDead = true;
  f.create(message);
  const model = f.server.getWorldModel().forms[0];
  assert.equal(model.isDead, true); assert.equal(model.movement.isDead, true);
  const movement = { ...model.movement, isDead: false };
  f.event('updateMovementMessage', { idx: 1, data: movement });
  assert.equal(model.movement.isDead, true); assert.equal(movement.isDead, false, 'Do not mutate the incoming packet');
  f.event('updatePropertyMessage', { idx: 1, propName: 'isDead', data: false });
  assert.equal(model.movement.isDead, false); assert.equal(model.isDead, false);
});

function fixture(storage = {}) {
  const events = new EventEmitter(), emitter = new EventEmitter(), calls = [], ragdolls = [], guards = [];
  let connected = true, platformLoad = false, handle = 20, frame = 0, menu = false, now = 10000;
  const forms = new Map();
  const reference = id => {
    const captured = frame;
    const valid = () => assert.equal(frame, captured, 'Native reference escaped its frame');
    return { getFormID() { valid(); return id; }, getBaseObject: () => ({ getType: () => 28, getName: () => 'Chest' }),
      activate() { valid(); calls.push(['activate', id]); },
      playAnimation(name) { valid(); calls.push(['animation', id, name]); return false; },
      setDisplayName() {}, getFurnitureReference: () => null,
    };
  };
  class PlayerSpawnService {} class LoadGameService {} class RagdollService {}
  class RespawnNeededError extends Error {}
  const view = { syncFormArray() {}, getFormViews: () => ({ destroyForm: id => calls.push(['destroy-view', id]) }) };
  const sp = { storage, mpClientPlugin: { isConnected: () => connected },
    getReferenceHandle: id => id === 0x14 || forms.has(id) ? handle : 0,
    Game: { getPlayer: () => reference(0x14), getFormEx: id => forms.has(id) || id === 0x14 ? reference(id) : null,
      quitToMainMenu: () => calls.push(['quit']) },
    Actor: { from: value => value }, ObjectReference: { from: value => value },
    Cell: { from: value => value }, WorldSpace: { from: value => value },
    Ui: { isMenuOpen: () => menu },
    TESModPlatform: { moveRefrToPosition: (ref, _cell, _world, ...pos) => calls.push(['move', ref.getFormID(), ...pos]) },
    FormType: { Container: 28, Furniture: 40 },
    interruptCast: () => calls.push(['interrupt']), castSpellImmediate: () => calls.push(['cast']),
    applyAnimationVariablesToActor: () => { calls.push(['anim-vars']); return true; },
  };
  const dependencies = {
    skyrimPlatform: sp, '../../messages': { MsgType: { Activate: 12 } },
    '../../extensions/objectReferenceEx': { ObjectReferenceEx: { dealWithRef: () => calls.push(['configure']) } },
    '../../lib/idManager': { IdManager }, '../../lib/nameof': { nameof: name => name },
    '../../sync/actorvalues': { setActorValuePercentage: (ref, name, value) => calls.push(['value', ref.getFormID(), name, value]) },
    '../../sync/appearance': { applyAppearanceToPlayer: (value, guard) => { calls.push(['appearance', value.name]); guards.push(guard); } },
    '../../view/modelApplyUtils': { ModelApplyUtils: {
      applyModelInventory: () => calls.push(['inventory']), applyModelIsOpen: () => calls.push(['open']),
      applyModelIsHarvested: () => calls.push(['harvest']), applyModelIsDisabled: () => calls.push(['disabled']),
      applyModelNodeScale() {}, applyModelNodeTextureSet() {},
    } },
    './ragdollService': { RagdollService }, './clientListener': { ClientListener: class {} },
    '../../lib/errors': { RespawnNeededError },
    '../../view/worldViewMisc': { getViewFromStorage: () => view,
      getObjectReference: () => reference(0xff001000), remoteIdToLocalId: id => id },
    '../../logging': { logTrace() {}, logError() {} }, './playerSpawnService': { PlayerSpawnService },
    './loadGameService': { LoadGameService },
  };
  const { RemoteServer } = load('services/services/remoteServer.ts', dependencies, { Date: { now: () => now } });
  const server = new RemoteServer(sp, { on: (...args) => events.on(...args), once: (...args) => events.once(...args), emitter,
    lookupListener(type) {
      if (type === PlayerSpawnService) return { request: m => calls.push(['spawn', m.idx]), cancel: () => calls.push(['cancel-spawn']) };
      if (type === LoadGameService) return { get isLoadingPlatformSave() { return platformLoad; } };
      if (type === RagdollService) return { safeRemoveRagdollFromWorld: (_actor, callback) => ragdolls.push(callback) };
      throw Error('Unexpected service');
    },
  });
  const event = (name, message) => emitter.emit(name, { message });
  return { server, emitter, events, calls, guards, ragdolls, forms, storage, RespawnNeededError,
    event, create: message => event('createActorMessage', message || create()),
    update(ms = 0) { now += ms; ++frame; events.emit('update'); },
    tick: () => events.emit('tick'), native: (...args) => events.emit(...args),
    connected(value) { connected = value; }, platformLoad(value) { platformLoad = value; },
    handle(value) { handle = value; }, menu(value) { menu = value; },
  };
}

test('remote index allocation is idempotent, sparse and safely reusable', () => {
  const ids = new IdManager();
  assert.equal(ids.allocateIdFor(0xffffffff), 0); assert.equal(ids.allocateIdFor(0xffffffff), 0);
  assert.equal(ids.allocateIdFor(1), 1); ids.freeIdFor(7); assert.equal(ids.getId(1), 1);
  ids.freeIdFor(0xffffffff); assert.equal(ids.allocateIdFor(2), 0);
  assert.equal(ids.getValueById(0), 2); assert.throws(() => ids.allocateIdFor(-1));
});

test('upgrading a preserved index manager keeps existing model slots and their gaps', () => {
  const storage = { worldModel: { forms: [create(80), undefined, create(90)], playerCharacterFormIdx: 0 },
    idManager: { getId: () => 500 } };
  const f = fixture(storage), ids = f.server.getIdManager();
  assert.equal(ids.getId(80), 0); assert.equal(ids.getId(90), 2); assert.equal(ids.allocateIdFor(5), 1);
});

test('plugin replacement preserves static identities while invalidating its old callbacks', () => {
  const f = fixture(); f.forms.set(0x100, true); f.create(create(3, 0x100, false));
  const replacement = fixture(f.storage); replacement.forms.set(0x100, true);
  replacement.event('updatePropertyMessage', { idx: 3, refrId: 0x100, propName: 'inventory', data: {} });
  f.update(); replacement.update();
  assert.equal(f.calls.some(c => c[0] === 'configure'), false);
  assert.equal(replacement.calls.filter(c => c[0] === 'inventory').length, 1);
});

test('baseline reset clears model indices and hosting and cancels queued actor effects', () => {
  const f = fixture(); f.create(); f.storage.hosted = [123];
  f.event('changeValuesMessage', { idx: 1, data: { health: 0.1 } });
  f.server.resetWorldModel(); f.create(create(1)); f.update();
  assert.equal(f.server.getWorldModel().forms.length, 1); assert.equal(f.server.getIdManager().getId(1), 0);
  assert.equal(f.storage.hosted.length, 0); assert.equal(f.calls.some(c => c[0] === 'value'), false);
});

test('duplicate creation and index reuse invalidate old per-actor callbacks without leaking slots', () => {
  const f = fixture(); f.create(); f.event('changeValuesMessage', { idx: 1, data: { health: 0.1 } });
  f.create(create()); f.update(); assert.equal(f.calls.some(c => c[0] === 'value'), false);
  assert.equal(f.server.getWorldModel().forms.length, 1);
  f.event('changeValuesMessage', { idx: 1, data: { health: 0.5 } }); f.update();
  assert.deepEqual(f.calls.filter(c => c[0] === 'value'), [['value', 0x14, 'health', 0.5]]);
});

test('unknown destruction cannot quit the game and a new self cancels queued menu exit', () => {
  const f = fixture(); f.event('destroyActorMessage', { idx: 99 }); f.update();
  f.create(); f.event('destroyActorMessage', { idx: 1 }); f.create(create(2)); f.update();
  assert.equal(f.calls.some(c => c[0] === 'quit'), false);
  f.event('destroyActorMessage', { idx: 2 }); f.update(); assert.equal(f.calls.filter(c => c[0] === 'quit').length, 1);
});

for (const boundary of ['reset', 'disconnect', 'closed', 'reload', 'personal-load'])
  test(`queued appearance cannot cross ${boundary}`, () => {
    const f = fixture(); f.create(); f.event('updateAppearanceMessage', { idx: 1, data: { name: 'stale' } });
    if (boundary === 'reset') f.server.resetWorldModel();
    if (boundary === 'disconnect') f.emitter.emit('connectionDisconnect', {});
    if (boundary === 'closed') f.connected(false);
    if (boundary === 'reload') fixture(f.storage);
    if (boundary === 'personal-load') f.native('preLoadGame');
    f.update(); assert.equal(f.calls.some(c => c[0] === 'appearance'), false);
  });

test('generated-world loading suspends initial native effects until its load event', () => {
  const f = fixture(); f.create(); f.event('updateAppearanceMessage', { idx: 1, data: { name: 'current' } });
  f.platformLoad(true); f.native('preLoadGame'); f.update();
  assert.equal(f.calls.some(c => c[0] === 'appearance'), false);
  f.emitter.emit('gameLoad', { isCausedBySkyrimPlatform: true }); f.update();
  assert.equal(f.calls.filter(c => c[0] === 'appearance').length, 1);
  assert.equal(f.guards[0](), true); f.server.resetWorldModel(); assert.equal(f.guards[0](), false);
});

for (const changed of ['none', 'epoch', 'handle']) test(`late ragdoll completion checks ${changed} before teleporting`, () => {
  const f = fixture(); f.create(); f.event('teleportMessage', { idx: 1, pos: [1, 2, 3], rot: [0, 0, 4], worldOrCell: 60 });
  f.update(); assert.equal(f.ragdolls.length, 1);
  if (changed === 'epoch') f.server.resetWorldModel();
  if (changed === 'handle') f.handle(21);
  f.ragdolls[0](); f.update();
  assert.equal(f.calls.filter(c => c[0] === 'move').length, changed === 'none' ? 1 : 0);
});

test('static reference destruction cancels creation, updates and animation retries', () => {
  const f = fixture(), message = create(3, 0x100, false); f.forms.set(0x100, true);
  message.props = { lastAnimation: 'Open' }; f.create(message); f.update(); f.update();
  assert.equal(f.calls.filter(c => c[0] === 'animation').length, 1);
  f.event('updatePropertyMessage', { idx: 3, refrId: 0x100, propName: 'inventory', data: {} });
  f.event('destroyActorMessage', { idx: 3 }); const before = f.calls.length; f.update(3000);
  assert.equal(f.calls.length, before);
});

test('reusing a static actor index invalidates work for the previous original', () => {
  const f = fixture(); f.forms.set(0x100, true); f.forms.set(0x101, true);
  f.create(create(3, 0x100, false)); f.create(create(3, 0x101, false)); f.update();
  assert.equal(f.calls.filter(c => c[0] === 'configure').length, 1);
});

test('original baseline readiness waits for application and rejects exhausted native lookup', () => {
  const f = fixture(); f.create(create(3, 0x100, false));
  assert.equal(f.server.getOriginalApplicationProgress().ready, false);
  f.update(); assert.equal(f.server.getOriginalApplicationProgress().ready, false);
  f.forms.set(0x100, true); f.update(); f.update();
  assert.equal(f.server.getOriginalApplicationProgress().ready, true);
  f.create(create(4, 0x101, false));
  for (let i = 0; i < 241; ++i) f.update();
  assert.equal(f.server.getOriginalApplicationProgress().ready, false);
  assert.match(f.server.getOriginalApplicationProgress().error, /Original reference did not load: 101/);
  f.server.resetWorldModel(); assert.equal(f.server.getOriginalApplicationProgress().ready, true);
});

test('native application errors cannot appear ready and replacing an index cancels its old pending original', () => {
  const f = fixture(), message = create(3, 0x100, false);
  Object.defineProperty(message.props, 'inventory', { get() { throw Error('Native application rejected'); } });
  f.forms.set(0x100, true); f.create(message);
  assert.throws(() => f.update(), /Native application rejected/);
  assert.equal(f.server.getOriginalApplicationProgress().ready, false);
  assert.match(f.server.getOriginalApplicationProgress().error, /Native application rejected/);
  f.create(create(3, 0x101, false)); f.create(create(3, 0x102, false)); f.forms.set(0x102, true);
  f.update(); assert.equal(f.server.getOriginalApplicationProgress().ready, true);
  for (let i = 0; i < 241; ++i) f.update();
  assert.equal(f.server.getOriginalApplicationProgress().ready, true);
});

test('projected reference resolution never falls back to a hidden original while its copy is missing', () => {
  let local = 0;
  const storage = { view: { getProjectedLocalRefrId: id => id === 0x100 ? local : undefined, getLocalRefrId: () => 0 } };
  const { remoteIdToLocalId } = load('view/worldViewMisc.ts', { skyrimPlatform: { storage },
    '../services/spApiInteractor': {}, '../services/services/remoteServer': {} });
  assert.equal(remoteIdToLocalId(0x100), 0);
  local = 0xff000123; assert.equal(remoteIdToLocalId(0x100), local);
  assert.equal(remoteIdToLocalId(0x101), 0x101);
  assert.equal(remoteIdToLocalId(0xff000001), 0);
  storage.view = { getLocalRefrId: () => 0 }; // Retained view from an older plugin.
  assert.equal(remoteIdToLocalId(0x100), 0x100);
});

function harvestFixture() {
  const events = new EventEmitter(), calls = [], completions = [], errors = [];
  let frame = 0, handle = 1, current = true, plant = true, disabled = false, harvested = true, noop = false;
  const reference = () => {
    const captured = frame, valid = () => assert.equal(frame, captured, 'Harvest wrapper escaped its native update');
    return { getFormID() { valid(); return 0x100; }, getBaseObject: () => ({ getType: () => plant ? 1 : 3 }),
      isHarvested: () => harvested, isDisabled: () => disabled, isDeleted: () => false,
      setHarvested: value => { calls.push(['harvested', value]); harvested = value; },
      disable() { valid(); calls.push(['disable']); return new Promise(resolve => completions.push(() => { if (!noop) disabled = true; resolve(); })); },
      enable() { valid(); calls.push(['enable']); disabled = false; return Promise.resolve(); },
      delete() { assert.fail('Harvesting must never delete a reference'); },
    };
  };
  const sp = { ObjectReference: { from: value => value }, FormType: { Tree: 1, Flora: 2 },
    Game: { getFormEx: () => reference() }, getReferenceHandle: () => handle,
    once: (...args) => events.once(...args),
  };
  const { ModelApplyUtils } = load('view/modelApplyUtils.ts', {
    skyrimPlatform: sp, '../sync/inventory': {}, '../logging': {},
    '../services/services/nativeReferenceOperationService': {},
    '../services/spApiInteractor': { SpApiInteractor: { getControllerInstance: () => ({ lookupListener: () => ({
      submit(id, key, current, steps) {
        return operations.submit(id, key, current, (Array.isArray(steps) ? steps : [steps]).map(step => () => step(reference())));
      },
    }) }) } },
  });
  const operations = new NativeReferenceOperations({ identity: () => handle, canStart: () => true, report: job => errors.push(job.error) });
  return { calls, errors, operations, apply(value) { ModelApplyUtils.applyModelIsHarvested(reference(), value, () => current); operations.update(); },
    visibility(value) { ModelApplyUtils.applyModelIsDisabled(reference(), value, () => current); operations.update(); },
    disabled(value) { disabled = value; }, noop(value) { noop = value; },
    replace() { ++handle; }, cancel() { current = false; }, object() { plant = false; },
    async complete() { completions.shift()(); await Promise.resolve(); },
    async drain() { for (let i = 0; i < 4; ++i) { await Promise.resolve(); ++frame; operations.update(); } },
    update() { ++frame; operations.update(); events.emit('update'); },
  };
}

for (const changed of ['none', 'identity', 'world']) test(`harvest refresh checks ${changed} on a fresh update`, async () => {
  const f = harvestFixture(); f.apply(false);
  if (changed === 'identity') f.replace();
  if (changed === 'world') f.cancel();
  await f.complete(); assert.equal(f.calls.some(c => c[0] === 'enable'), false);
  f.update(); assert.equal(f.calls.filter(c => c[0] === 'enable').length, changed === 'none' ? 1 : 0);
  await f.drain(); assert.deepEqual(f.errors, []); assert.equal(f.operations.isBusy(), false);
});

test('harvesting an ordinary object never deletes its authored reference', async () => {
  const f = harvestFixture(); f.object(); f.apply(true); await f.complete(); f.update();
  assert.deepEqual(f.calls, [['disable']]);
  await f.drain(); assert.deepEqual(f.errors, []);
});

test('harvest model refresh preserves a plant that was already disabled', async () => {
  const f = harvestFixture(); f.disabled(true); f.apply(false); await f.drain();
  assert.deepEqual(f.calls, [['harvested', false]]); assert.deepEqual(f.errors, []);
});

test('a no-op native visibility change is rejected after observing the reference', async () => {
  const f = harvestFixture(); f.noop(true); f.visibility(true); await f.complete(); await f.drain();
  assert.equal(f.errors.length, 1); assert.match(f.errors[0], /visibility/);
});

function gamemodeFixture() {
  const events = new EventEmitter(), emitter = new EventEmitter(), storage = {}, calls = [];
  let model = { forms: [{ idx: 1 }], playerCharacterFormIdx: 0 };
  class RemoteServer {} class ServerJsVerificationService {}
  const sp = { storage, settings: { 'skymp5-client': {} },
    Game: { getPlayer: () => ({}) }, ObjectReference: { from: value => value },
    printConsole: value => calls.push(value),
  };
  const dependencies = { skyrimPlatform: sp,
    '../../view/worldViewMisc': { localIdToRemoteId: v => v, remoteIdToLocalId: v => v },
    './clientListener': { ClientListener: class {} }, './remoteServer': { RemoteServer },
    './serverJsVerificationService': { ServerJsVerificationService },
    '../../logging': { logTrace() {}, logError() {} }, '../../messages': { MsgType: { CustomEvent: 9 } },
  };
  const controller = { on: (...args) => events.on(...args), once: (...args) => events.once(...args), emitter,
    lookupListener(type) {
      if (type === RemoteServer) return { getWorldModel: () => model };
      if (type === ServerJsVerificationService) return { verifyServerJs: src => ({ src }) };
      throw Error('Unexpected lookup');
    },
  };
  const { GamemodeEventSourceService } = load('services/services/gamemodeEventSourceService.ts', dependencies);
  const { GamemodeUpdateService } = load('services/services/gamemodeUpdateService.ts', dependencies);
  new GamemodeEventSourceService(sp, controller); new GamemodeUpdateService(sp, controller);
  return { emitter, storage, calls, setModel(value) { model = value; },
    update: () => events.emit('update'), tick: () => events.emit('tick'),
    gamemode() { emitter.emit('updateGamemodeDataMessage', { message: {
      eventSources: [{ name: 'test', content: 'ctx.sp.printConsole("source");' }],
      updateOwnerFunctions: [{ name: 'idx', content: 'ctx.sp.printConsole("owner");' }],
      updateNeighborFunctions: [],
    } }); },
  };
}

test('world reset expires queued event sources and previously captured event senders', () => {
  const f = gamemodeFixture(), sent = []; f.emitter.on('sendMessage', e => sent.push(e));
  f.gamemode(); const oldSend = f.storage.eventSourceContexts[0].sendEvent;
  f.emitter.emit('worldModelReset'); f.update(); oldSend('late');
  assert.equal(f.calls.length, 0); assert.equal(sent.length, 0); assert.equal(f.storage.eventSourceContexts.length, 0);
  f.gamemode(); f.update(); assert.deepEqual(f.calls, ['source']);
});

test('world reset clears owner functions and cancels pending ownership of a replaced self model', () => {
  const f = gamemodeFixture(); f.gamemode();
  f.emitter.emit('createActorMessage', { message: create() }); f.emitter.emit('worldModelReset');
  f.tick(); f.update(); assert.equal(f.storage.ownerModelSet, false); assert.equal(f.calls.length, 0);
  f.gamemode(); f.emitter.emit('createActorMessage', { message: create() }); f.tick(); f.update();
  assert.deepEqual(f.calls, ['owner', 'source']);
  f.emitter.emit('worldModelReset'); f.update(); assert.equal(f.calls.length, 2);
});

test('death recovery retires the view instead of deleting a native actor directly', () => {
  const f = fixture(); f.create(create(1, 0xff000001, false)); f.forms.set(0xff000001, true);
  f.emitter.on('applyDeathStateEvent', () => { throw new f.RespawnNeededError(); });
  f.event('deathStateContainerMessage', { tIsDead: { idx: 1, propName: 'isDead', data: false } }); f.update();
  assert.deepEqual(f.calls.filter(c => c[0] === 'destroy-view'), [['destroy-view', 0]]);
});

test('container close sends one activation and a reset cancels its old menu poll', () => {
  const f = fixture(), message = create(3, 0x100, false); f.forms.set(0x100, true); f.create(message); f.update();
  const sent = []; f.emitter.on('sendMessage', e => sent.push(e));
  f.event('openContainerMessage', { target: 0x100 }); f.update();
  assert.equal(f.calls.some(c => c[0] === 'activate'), false);
  f.update(100); f.menu(true); f.update(100);
  f.menu(false); f.update(100); f.update(100); assert.equal(sent.length, 1);
  f.event('openContainerMessage', { target: 0x100 }); f.update(100); f.menu(true); f.update(100);
  f.server.resetWorldModel(); f.menu(false); f.update(100); assert.equal(sent.length, 1);
});
