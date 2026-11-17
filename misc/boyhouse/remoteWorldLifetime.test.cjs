const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const { EventEmitter } = require('node:events');
const { NativeReferenceOperations } = require('../../skymp5-client/src/campaign/nativeReferenceOperations.ts');
const { CampaignPacketChannel } = require('../../skymp5-client/src/campaign/campaignPacketChannel.ts');
const { WorldProjectionReplica } = require('../../skymp5-client/src/campaign/worldProjection.ts');
const { ProjectionFeed } = require('../../skymp5-server/ts/campaign/projectionFeed.js');
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
const { WorldProjectionCoordinator } = load('campaign/worldProjectionCoordinator.ts', { './worldProjection': { WorldProjectionReplica } });
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
  let valueFailure, onValue = () => {}, actorAvailable = true;
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
  class PlayerSpawnService {} class PlayerLoadoutService {} class LoadGameService {} class RagdollService {} class ActorFactionsService {}
  const loadout = { preservesCreationAppearance: false };
  class RespawnNeededError extends Error {}
  const view = { syncFormArray() {}, getFormViews: () => ({ destroyForm: id => calls.push(['destroy-view', id]) }) };
  const sp = { storage, mpClientPlugin: { isConnected: () => connected },
    getReferenceHandle: id => id === 0x14 || forms.has(id) ? handle : 0,
    Game: { getPlayer: () => reference(0x14), getFormEx: id => forms.has(id) || id === 0x14 ? reference(id) : null,
      quitToMainMenu: () => calls.push(['quit']) },
    Actor: { from: value => actorAvailable ? value : null }, ObjectReference: { from: value => value },
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
    '../../sync/actorvalues': { setActorValuePercentage: (ref, name, value) => {
      if (valueFailure) throw Error(valueFailure);
      calls.push(['value', ref.getFormID(), name, value]); onValue();
    } },
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
    './playerLoadoutService': { PlayerLoadoutService },
    './actorFactionsService': { ActorFactionsService },
    '../../sync/campaignFactions': load('sync/campaignFactions.ts'),
  };
  const { RemoteServer } = load('services/services/remoteServer.ts', dependencies, { Date: { now: () => now } });
  const server = new RemoteServer(sp, { on: (...args) => events.on(...args), once: (...args) => events.once(...args), emitter,
    lookupListener(type) {
      if (type === PlayerSpawnService) return { request: m => calls.push(['spawn', m.idx]), cancel: () => calls.push(['cancel-spawn']) };
      if (type === LoadGameService) return { get isLoadingPlatformSave() { return platformLoad; } };
      if (type === PlayerLoadoutService) return loadout;
      if (type === ActorFactionsService) return { updateSelf: (...args) => calls.push(['self-factions', ...args]) };
      if (type === RagdollService) return { safeRemoveRagdollFromWorld: (_actor, callback) => ragdolls.push(callback) };
      throw Error('Unexpected service');
    },
  });
  const event = (name, message) => emitter.emit(name, { message });
  return { server, emitter, events, calls, guards, ragdolls, forms, storage, RespawnNeededError, loadout,
    event, create: message => event('createActorMessage', message || create()),
    update(ms = 0) { now += ms; ++frame; events.emit('update'); },
    tick: () => events.emit('tick'), native: (...args) => events.emit(...args),
    connected(value) { connected = value; }, platformLoad(value) { platformLoad = value; },
    handle(value) { handle = value; }, menu(value) { menu = value; },
    valueFailure(value) { valueFailure = value; }, onValue(callback) { onValue = callback; },
    actorAvailable(value) { actorAvailable = value; },
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

test('actor-value readiness stays pending across loading and owns its received payload', () => {
  const f = fixture(); f.create();
  const message = { idx: 1, data: { health: 0.1, stamina: 0.2, magicka: 0.3 } };
  f.event('changeValuesMessage', message); message.data.health = 0.9;
  assert.equal(f.server.getActorValueApplicationProgress().ready, false);
  f.platformLoad(true); f.native('preLoadGame'); f.update(); f.update();
  assert.equal(f.server.getActorValueApplicationProgress().ready, false);
  assert.equal(f.calls.some(call => call[0] === 'value'), false);
  f.emitter.emit('gameLoad', { isCausedBySkyrimPlatform: true }); f.update();
  assert.equal(f.server.getActorValueApplicationProgress().ready, true);
  assert.deepEqual(f.calls.filter(call => call[0] === 'value'), [
    ['value', 0x14, 'health', 0.1], ['value', 0x14, 'stamina', 0.2], ['value', 0x14, 'magicka', 0.3],
  ]);
});

for (const failure of ['setter', 'missing', 'nonfinite']) test(`actor-value ${failure} failure blocks readiness until superseded`, () => {
  const f = fixture(); f.create();
  if (failure === 'setter') f.valueFailure('native value failed');
  if (failure === 'missing') f.actorAvailable(false);
  f.event('changeValuesMessage', { idx: 1, data: { health: failure === 'nonfinite' ? NaN : 0.1 } }); f.update();
  const progress = f.server.getActorValueApplicationProgress();
  assert.equal(progress.ready, false); assert.ok(progress.error);
  f.valueFailure(undefined); f.actorAvailable(true);
  f.event('changeValuesMessage', { idx: 1, data: { health: 0.7 } }); f.update();
  assert.equal(f.server.getActorValueApplicationProgress().error, progress.error, 'A later success cannot hide an earlier failed write');
  f.server.resetWorldModel(); f.create();
  assert.equal(f.server.getActorValueApplicationProgress().ready, true);
});

for (const boundary of ['reset', 'replacement', 'failed-load', 'disconnect']) test(`actor-value pending work cannot poison the next ${boundary} lifetime`, () => {
  const f = fixture(); f.create(); f.event('changeValuesMessage', { idx: 1, data: { health: 0.1 } });
  if (boundary === 'reset') f.server.resetWorldModel();
  if (boundary === 'replacement') f.create();
  if (boundary === 'failed-load') { f.platformLoad(true); f.native('preLoadGame'); f.native('postLoadGame', { success: false }); }
  if (boundary === 'disconnect') f.emitter.emit('connectionDisconnect', {});
  assert.equal(f.server.getActorValueApplicationProgress().ready, true);
  f.create(); f.event('changeValuesMessage', { idx: 1, data: { health: 0.8 } }); f.update();
  assert.deepEqual(f.calls.filter(call => call[0] === 'value'), [['value', 0x14, 'health', 0.8]]);
  assert.equal(f.server.getActorValueApplicationProgress().ready, true);
});

test('a reentrant baseline replacement between value writes stops the old actor update', () => {
  const f = fixture(); f.create(); f.onValue(() => { f.server.resetWorldModel(); f.create(); });
  f.event('changeValuesMessage', { idx: 1, data: { health: 0.1, stamina: 0.2, magicka: 0.3 } }); f.update();
  assert.deepEqual(f.calls.filter(call => call[0] === 'value'), [['value', 0x14, 'health', 0.1]]);
  assert.equal(f.server.getActorValueApplicationProgress().ready, true);
});

test('an epoch replacement cannot settle its new catch-up with an old queued callback', () => {
  const f = fixture(), channel = new CampaignPacketChannel();
  const first = 'a'.repeat(32), second = 'b'.repeat(32);
  const deliver = message => f.event('changeValuesMessage', message);
  channel.select(first); f.server.resetWorldModel(); f.create();
  channel.incoming({ t: 34, epoch: first, message: { t: 16, idx: 1, data: { health: 0.1 } } }, deliver);
  channel.drain(first, deliver);
  channel.select(second); f.server.resetWorldModel(); f.create();
  channel.incoming({ t: 34, epoch: first, message: { t: 16, idx: 1, data: { health: 0.2 } } }, deliver);
  channel.incoming({ t: 34, epoch: second, message: { t: 16, idx: 1, data: { health: 0.8 } } }, deliver);
  channel.drain(second, deliver);
  assert.equal(f.server.getActorValueApplicationProgress().ready, false);
  f.update();
  assert.deepEqual(f.calls.filter(call => call[0] === 'value'), [['value', 0x14, 'health', 0.8]]);
  assert.equal(f.server.getActorValueApplicationProgress().ready, true);
});

for (const fail of [false, true]) test(`reliable epoch catch-up ${fail ? 'failure closes' : 'settles before'} projection release`, () => {
  const f = fixture(), channel = new CampaignPacketChannel(), packets = [], releases = [], closed = [];
  const actor = 0xff000001, epoch = 'a'.repeat(32);
  const feed = new ProjectionFeed({ connected: () => true, actor: () => actor, instance: () => '',
    read: () => ({ schemaVersion: 1, actor, campaignOwner: actor, instance: '', contextEpoch: '1:1', identity: '', references: [] }),
    block() {}, beginEpoch: () => epoch, epoch: () => epoch,
    baseline: () => ({ schemaVersion: 1, actor, packetEpoch: epoch, messages: [
      { t: 32, eventSources: [], updateOwnerFunctions: [], updateNeighborFunctions: [] },
      { ...create(), props: { inventory: { entries: [] }, isRaceMenuOpen: false } },
    ] }), send: (_user, packet) => packets.push(packet), close: (_user, reason) => closed.push(reason),
  });
  const deliver = message => f.event('changeValuesMessage', message);
  const coordinator = new WorldProjectionCoordinator({
    cancel: () => f.server.resetWorldModel(), drained: () => ({ ready: true }),
    apply: (_projection, world) => world.messages.filter(message => message.t === 33).forEach(message => f.create(message)),
    observe: () => f.server.getActorValueApplicationProgress(), release: () => releases.push('ready'), disconnect() {},
  }, {
    block: token => token ? channel.select(token) : channel.requireEpoch(), send: packet => feed.packet(0, packet),
    drain: token => channel.drain(token, deliver), release: token => channel.release(token, deliver), close: reason => closed.push(reason),
  });
  feed.connect(0); coordinator.connect('catch-up');
  while (packets.length) coordinator.receive(packets.shift());
  coordinator.advance(false); coordinator.advance(true);
  assert.equal(coordinator.status.phase, 'awaiting-ready');
  channel.incoming({ t: 34, epoch: 'b'.repeat(32), message: { t: 16, idx: 1, data: { health: 0.9 } } }, deliver);
  channel.incoming({ t: 34, epoch, message: { t: 16, idx: 1, data: { health: 0.4 } } }, deliver);
  while (packets.length) coordinator.receive(packets.shift());
  coordinator.advance(true);
  assert.equal(coordinator.status.phase, 'releasing'); assert.equal(channel.ready, false); assert.equal(releases.length, 0);
  if (fail) f.valueFailure('catch-up native failure');
  f.update(); coordinator.advance(true);
  assert.equal(coordinator.status.phase, fail ? 'failed' : 'ready');
  assert.equal(channel.ready, !fail); assert.equal(releases.length, fail ? 0 : 1);
  assert.equal(closed.length > 0, fail);
  assert.deepEqual(f.calls.filter(call => call[0] === 'value'), fail ? [] : [['value', 0x14, 'health', 0.4]]);
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

test('failed personal load keeps invalidated effects cancelled and releases the loading gate for a new baseline', () => {
  const f = fixture(); f.create(); f.event('updateAppearanceMessage', { idx: 1, data: { name: 'stale' } });
  f.native('preLoadGame'); f.native('postLoadGame', { success: false }); f.update();
  assert.equal(f.calls.some(call => call[0] === 'appearance'), false);
  f.create(create(2)); f.event('updateAppearanceMessage', { idx: 2, data: { name: 'fresh' } }); f.update();
  assert.deepEqual(f.calls.filter(call => call[0] === 'appearance'), [['appearance', 'fresh']]);
});

test('failed generated load cannot resume deferred bootstrap effects in the surviving old world', () => {
  const f = fixture(); f.create(); f.event('updateAppearanceMessage', { idx: 1, data: { name: 'bootstrap' } });
  f.platformLoad(true); f.native('preLoadGame'); f.update();
  f.native('postLoadGame', { success: false }); f.update();
  assert.equal(f.calls.some(call => call[0] === 'appearance'), false);
});

test('creation preserves its face and accepted echo without queuing another native rebuild', () => {
  const f = fixture(); f.create(); f.loadout.preservesCreationAppearance = true;
  f.event('updateAppearanceMessage', { idx: 1, data: { name: 'Creator face' } }); f.update();
  assert.equal(f.calls.some(c => c[0] === 'appearance'), false);
  assert.equal(f.server.getWorldModel().forms[0].appearance.name, 'Creator face');
  f.loadout.preservesCreationAppearance = false;
  f.event('updateAppearanceMessage', { idx: 1, data: { name: 'Later server edit' } }); f.update();
  assert.equal(f.calls.filter(c => c[0] === 'appearance').length, 1);
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

test('faction properties target their current actor model and never an authored original', () => {
  const f = fixture(), message = create();
  const snapshot = [{ factionId: 0x2bf9b, rank: -1, isMember: false }];
  message.customPropsJsonDumps.push({ propName: 'campaignFactions', propValueJsonDump: JSON.stringify(snapshot) });
  f.create(message);
  assert.deepEqual(JSON.parse(JSON.stringify(f.server.getWorldModel().forms[0].campaignFactions)), snapshot);
  f.event('updatePropertyMessage', { idx: 1, propName: 'campaignFactions', data: [] });
  assert.deepEqual(JSON.parse(JSON.stringify(f.calls.filter(call => call[0] === 'self-factions'))), [['self-factions', 1, []]]);
  assert.throws(() => f.event('updatePropertyMessage', { idx: 1, propName: 'campaignFactions', data: [{ factionId: 1, rank: 0 }] }));
  assert.deepEqual(JSON.parse(JSON.stringify(f.server.getWorldModel().forms[0].campaignFactions)), []);
  const original = create(9, 0x100, false);
  original.customPropsJsonDumps = message.customPropsJsonDumps;
  assert.throws(() => f.create(original), /authored reference/);
});
