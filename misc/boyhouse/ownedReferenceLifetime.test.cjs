const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const { EventEmitter } = require('node:events');
const { OwnedReferenceLifetime } = require('../../skymp5-client/src/campaign/ownedReferenceLifetime.ts');
const root = path.resolve(__dirname, '../../skymp5-client');
const ts = require(require.resolve('typescript', { paths: [root, path.resolve(root, '../skymp5-server')] }));
const id = 0xff000001;

function load(file, dependencies) {
  const compiled = ts.transpileModule(fs.readFileSync(path.join(root, 'src', file), 'utf8'), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
  }).outputText;
  const module = { exports: {} };
  vm.runInNewContext(compiled, { module, exports: module.exports, require: name => {
    if (!(name in dependencies)) throw Error(`Unexpected dependency ${name}`);
    return dependencies[name];
  } }, { filename: file });
  return module.exports;
}

function fixture(count = 1) {
  const forms = new Map(Array.from({ length: count }, (_, i) => [id + i, { handle: i + 1, base: 123, deleted: false }]));
  const calls = [], jobs = [], errors = [], released = [];
  let frame = 0;
  const work = (name, formId = id, effect = () => {}) => {
    calls.push([name, formId]);
    return new Promise((resolve, reject) => jobs.push({ name, resolve, reject, effect }));
  };
  const adapter = {
    identity: formId => forms.get(formId)?.deleted ? 0 : forms.get(formId)?.handle || 0,
    dispose: (formId, handle) => {
      const form = forms.get(formId); assert.equal(form.handle, handle);
      return work('delete', formId, () => { form.deleted = true; });
    },
    released: formId => released.push(formId), report: (formId, error) => errors.push({ formId, error }),
  };
  const lifetime = new OwnedReferenceLifetime(adapter);
  const makeWrapper = (formId, actor = true) => {
    const capturedFrame = frame;
    const valid = () => assert.equal(capturedFrame, frame, 'Native wrapper survived its callback');
    return {
      actor,
      getFormID() { valid(); return formId; },
      getBaseObject() { valid(); return { getFormID: () => forms.get(formId).base, getType: () => 43 }; },
      getRace() { valid(); return null; },
      setActorValue() { valid(); }, startDeferredKill() { valid(); }, setDisplayName() { valid(); },
      isDeleted() { valid(); return forms.get(formId).deleted; },
      setPosition() { valid(); return work('position', formId); },
      setAngle() { valid(); return work('angle', formId); },
      enable() { valid(); return work('enable', formId); },
      resurrect() { valid(); return work('resurrect', formId); },
      setMotionType() { valid(); return work('motion', formId); },
    };
  };
  let actor = true;
  const sp = {
    Game: { getFormEx: formId => forms.has(formId) ? makeWrapper(formId, actor) : null },
    ObjectReference: { from: value => value }, Actor: { from: value => value?.actor ? value : null },
    MotionType: { Keyframed: 4 },
  };
  const { SpawnProcess } = load('view/spawnProcess.ts', {
    skyrimPlatform: sp,
    '../sync/appearance': { applyTints: ref => { calls.push(['tints', ref.getFormID()]); forms.get(id).base = 7; } },
    '../extensions/objectReferenceEx': { ObjectReferenceEx: { dealWithRef(ref, base, motion) {
      assert.equal(motion, false, 'Motion changes must use the tracked queue'); calls.push(['prepare', ref.getFormID()]);
    } } },
  });
  return { lifetime, adapter, forms, calls, jobs, errors, released, work, sp,
    tick() { ++frame; lifetime.update(); },
    spawn(reference, complete, appearance = null, rotation) {
      new SpawnProcess(appearance, [1, 2, 3], reference, lifetime, complete, rotation);
    },
    setActor(value) { actor = value; },
    async complete({ error, apply = true } = {}) {
      assert.ok(jobs.length, 'Expected a pending native job'); const job = jobs.shift();
      if (apply) job.effect();
      if (error) job.reject(error); else job.resolve();
      await Promise.resolve(); await Promise.resolve();
    },
  };
}

test('actor spawn serializes native stages and reports ready only after their completion', async () => {
  const f = fixture(), reference = f.lifetime.claim(id); let ready = 0;
  f.spawn(reference, () => ++ready, { tints: [] });
  f.tick(); assert.deepEqual(f.calls.map(c => c[0]), ['position']);
  await f.complete(); assert.equal(f.calls.length, 1, 'Promise completion must not call the engine');
  f.tick(); assert.deepEqual(f.calls.map(c => c[0]), ['position', 'tints', 'enable']);
  await f.complete(); f.tick(); assert.equal(ready, 0);
  await f.complete(); f.tick(); assert.equal(ready, 1);
  assert.equal(f.forms.get(id).base, 7, 'Tint base-ID adjustment does not invalidate reference identity');
  assert.equal(f.errors.length, 0);
});

for (const cancelAt of ['queued', 'position', 'enable', 'resurrect']) {
  test(`cancellation during ${cancelAt} drains native work before deletion and never signals ready`, async () => {
    const f = fixture(), reference = f.lifetime.claim(id); let ready = 0;
    f.spawn(reference, () => ++ready);
    if (cancelAt !== 'queued') {
      f.tick();
      while (f.jobs[0].name !== cancelAt) { await f.complete(); f.tick(); }
    }
    f.lifetime.retire(reference); f.lifetime.retire(reference);
    f.tick();
    assert.equal(f.lifetime.hasRetiringReferences(), true);
    if (cancelAt !== 'queued') {
      assert.equal(f.calls.some(c => c[0] === 'delete'), false, 'No delete races an outstanding native operation');
      await f.complete(); f.tick();
    }
    assert.equal(f.jobs.length, 1); assert.equal(f.jobs[0].name, 'delete');
    await f.complete(); f.tick();
    assert.equal(f.lifetime.hasRetiringReferences(), false);
    assert.equal(ready, 0); assert.deepEqual(f.released, [id]);
    assert.equal(f.calls.filter(c => c[0] === 'delete').length, 1);
  });
}

test('object angle, movement and physics work are tracked and canceled without stale completion', async () => {
  const f = fixture(); f.setActor(false); const reference = f.lifetime.claim(id); let ready = false;
  f.spawn(reference, () => ready = true, null, [0, 0, 90]);
  f.tick(); assert.equal(f.jobs[0].name, 'angle');
  await f.complete(); f.tick(); assert.equal(f.jobs[0].name, 'position');
  await f.complete(); f.tick(); await f.complete(); f.tick();
  assert.equal(f.jobs[0].name, 'motion');
  f.lifetime.retire(reference); await f.complete(); f.tick();
  assert.equal(ready, false); assert.equal(f.jobs[0].name, 'delete');
  await f.complete(); f.tick(); assert.equal(f.errors.length, 0);
});

test('new-save generation prevents old completion or deletion against a matching FormID and handle', async () => {
  const f = fixture(), reference = f.lifetime.claim(id); let ready = false;
  f.spawn(reference, () => ready = true); f.tick(); f.lifetime.retire(reference);
  f.lifetime.forgetWorld(); f.forms.set(id, { handle: reference.handle, base: 123, deleted: false });
  await f.complete(); f.tick();
  assert.equal(f.jobs.length, 0); assert.equal(ready, false); assert.equal(f.forms.get(id).deleted, false);
  assert.equal(f.released.length, 0, 'Do not alter new-world protection counters');
  assert.equal(f.lifetime.enqueue(reference, () => assert.fail('Stale lease accepted')), false);
});

test('same-world ID reuse with the same base cannot receive old enable, resurrection, or deletion', async () => {
  const f = fixture(), reference = f.lifetime.claim(id); f.spawn(reference, () => assert.fail('Stale ready'));
  f.tick(); f.forms.set(id, { handle: 99, base: 123, deleted: false });
  await f.complete(); f.tick();
  assert.deepEqual(f.calls, [['position', id]]); assert.equal(f.forms.get(id).deleted, false);
  assert.equal(f.lifetime.isCurrent(reference), false);
  assert.equal(f.lifetime.claim(id).handle, 99);
});

test('hot reload retires old callbacks, waits for in-flight work, and adopts cleanup', async () => {
  const f = fixture(), reference = f.lifetime.claim(id); f.spawn(reference, () => assert.fail('Old view became ready'));
  f.tick();
  const reloaded = new OwnedReferenceLifetime(f.adapter, f.lifetime.state);
  reloaded.retireAll(); reloaded.update(); assert.equal(f.jobs.length, 1);
  await f.complete(); reloaded.update(); assert.equal(f.jobs[0].name, 'delete');
  await f.complete(); reloaded.update(); assert.equal(reloaded.state.references.size, 0);
});

test('native spawn rejection retires the reference and cleans it up without an unhandled rejection', async () => {
  const f = fixture(), reference = f.lifetime.claim(id); f.spawn(reference, () => assert.fail('Failed spawn ready'));
  f.tick(); await f.complete({ error: Error('position failed') }); f.tick(); f.tick();
  assert.equal(f.jobs[0].name, 'delete'); await f.complete(); f.tick();
  assert.equal(f.errors.length, 1); assert.match(reference.error, /position failed/);
  assert.equal(f.lifetime.state.references.size, 0);
});

for (const reject of [false, true]) test(`native deletion ${reject ? 'rejection' : 'no-op'} cannot falsely complete a transition`, async () => {
  const f = fixture(), reference = f.lifetime.claim(id); f.lifetime.retire(reference); f.tick();
  await f.complete({ apply: false, error: reject ? Error('delete failed') : undefined }); f.tick();
  for (let i = 0; i < 10; ++i) f.tick();
  assert.equal(f.lifetime.hasRetiringReferences(), true); assert.equal(f.released.length, 0);
  assert.equal(f.calls.length, 1); assert.equal(f.errors.length, 1);
});

test('bounded update work and ownership limits preserve all outstanding cleanup records', () => {
  const f = fixture(80);
  for (const formId of f.forms.keys()) {
    const reference = f.lifetime.claim(formId); f.lifetime.enqueue(reference, () => f.work('job', formId));
  }
  f.tick(); assert.equal(f.calls.length, 16);
  assert.throws(() => f.lifetime.claim(id), /already owned/);
  assert.throws(() => f.lifetime.claim(0x14), /temporary/);
  const limited = new OwnedReferenceLifetime(f.adapter, undefined, 1); limited.claim(id);
  assert.throws(() => limited.claim(id + 1), /limit/);
  assert.equal(limited.state.references.size, 1);
});

test('cancellation from the completion callback discards newly queued work before native execution', async () => {
  const f = fixture(), reference = f.lifetime.claim(id);
  f.lifetime.enqueue(reference, () => f.work('first'), () => {
    f.lifetime.enqueue(reference, () => assert.fail('Canceled followup ran'));
    f.lifetime.retire(reference);
  });
  f.tick(); await f.complete(); f.tick();
  assert.deepEqual(f.calls.map(c => c[0]), ['first', 'delete']);
});

function worldFixture() {
  const events = new EventEmitter(), emitter = new EventEmitter(), waits = [], arrays = [];
  let connected = true, singlePlayer = false, inConnectionCallback = false;
  class FormViewArray {
    constructor() { this.resizes = []; this.updates = []; arrays.push(this); }
    resize(size) { this.resizes.push(size); }
    updateAll(model) { this.updates.push(model.forms.length); }
  }
  class SinglePlayerService {} class RemoteServer {} class NetworkingService {}
  const { WorldView } = load('view/worldView.ts', {
    './formViewArray': { FormViewArray }, './playerCharacterDataHolder': { PlayerCharacterDataHolder: { updateData() {} } },
    '../services/services/clientListener': { ClientListener: class {} }, '../logging': { logTrace() {} },
    '../services/services/singlePlayerService': { SinglePlayerService }, '../services/services/remoteServer': { RemoteServer },
    '../services/services/networkingService': { NetworkingService },
  });
  const sp = { storage: {}, settings: { 'skymp5-client': {} }, Utility: { wait: () => {
    assert.equal(inConnectionCallback, false, 'Papyrus waits are forbidden in the connection tick');
    return new Promise(resolve => waits.push(resolve));
  } },
    Game: { getPlayer: () => ({ getWorldSpace: () => ({ getFormID: () => 60 }) }) } };
  const controller = { on: (...args) => events.on(...args), once: (...args) => events.once(...args), emitter,
    lookupListener(type) {
      if (type === SinglePlayerService) return { isSinglePlayer: singlePlayer };
      if (type === NetworkingService) return { isConnected: () => connected };
      if (type === RemoteServer) return { getWorldModel: () => ({ forms: [1], playerCharacterFormIdx: -1 }) };
      throw Error('Unexpected lookup');
    } };
  const view = new WorldView(sp, controller);
  return { view, arrays, waits, sp, event: (...args) => {
    inConnectionCallback = args[0] === 'connectionAccepted';
    try { emitter.emit(...args); } finally { inConnectionCallback = false; }
  }, native: (...args) => events.emit(...args), tick: () => events.emit('update'),
    setConnected(value) { connected = value; }, setSinglePlayer(value) { singlePlayer = value; },
    async finishWait(index = 0) { waits[index](); await Promise.resolve(); } };
}

test('connection resumption starts its native wait on update and only for the current gate', async () => {
  const f = worldFixture(); f.event('connectionAccepted', {});
  assert.equal(f.waits.length, 0); f.tick(); assert.equal(f.waits.length, 1);
  await f.finishWait(); f.tick(); assert.equal(f.arrays.at(-2).updates.at(-1), 1);
  f.event('connectionAccepted', {}); f.view.setFormViewUpdateAllowed(false); f.tick();
  assert.equal(f.waits.length, 1, 'An intervening gate must discard the queued resume');
  assert.equal(f.arrays.at(-2).updates.at(-1), 0);
});

test('a pending game-time wait cannot reopen views after an explicit phase gate', async () => {
  const f = worldFixture(); f.tick(); f.view.setFormViewUpdateAllowed(false);
  await f.finishWait(); f.tick(); assert.equal(f.arrays[0].updates.at(-1), 0);
  f.view.setFormViewUpdateAllowed(true); f.tick(); assert.equal(f.arrays[0].updates.at(-1), 1);
});

test('an explicit gate survives startup, game loads, and reconnects until the caller releases it', async () => {
  const f = worldFixture(); f.view.setFormViewUpdateAllowed(false); f.tick();
  assert.equal(f.waits.length, 0, 'Startup must not schedule a resume after an explicit gate');
  f.event('gameLoad', {}); f.event('connectionAccepted', {}); f.tick();
  assert.equal(f.waits.length, 0, 'Automatic load/connect resumption must preserve the explicit gate');
  f.view.waitGameTimeAndAllowFormViewUpdate(1); await f.finishWait(); f.tick();
  assert.equal(f.arrays.at(-2).updates.at(-1), 1);
});

test('a delayed caller cannot release another transition gate or reopen a new world', async () => {
  const f = worldFixture(); const stale = f.view.setFormViewUpdateAllowed(false);
  const current = f.view.setFormViewUpdateAllowed(false);
  assert.equal(f.view.isUpdateGateCurrent(stale), false);
  f.view.waitGameTimeAndAllowFormViewUpdate(1, stale); assert.equal(f.waits.length, 0);
  f.view.waitGameTimeAndAllowFormViewUpdate(1, current); assert.equal(f.waits.length, 1);
  f.event('gameLoad', {});
  assert.equal(f.view.isUpdateGateCurrent(current), false);
  await f.finishWait(0); f.tick(); assert.equal(f.arrays.at(-2).updates.at(-1), 0);
});

for (const replaced of [false, true]) test(`delayed character-menu snippet ${replaced ? 'cannot open or resume a newer phase' : 'opens and schedules resumption for its own gate'}`, async () => {
  const f = worldFixture(); f.tick();
  const events = new EventEmitter(), emitter = new EventEmitter(), waits = []; let opened = 0;
  const { SpSnippetService } = load('services/services/spSnippetService.ts', {
    '../../messages': { MsgType: {} }, './clientListener': { ClientListener: class {} },
    '../../view/worldViewMisc': {}, '../../logging': { logTrace() {}, logError() {} }, '../../view/worldView': {},
  });
  new SpSnippetService({ Utility: { wait: () => new Promise(resolve => waits.push(resolve)) },
    Game: { showRaceMenu: () => ++opened } }, {
    emitter, once: (...args) => events.once(...args), lookupListener: () => f.view,
  });
  emitter.emit('spSnippetMessage', { message: { class: 'Game', function: 'showRaceMenu', arguments: [], snippetIdx: 0xffffffff } });
  events.emit('update'); assert.equal(waits.length, 1);
  if (replaced) f.view.setFormViewUpdateAllowed(false);
  waits[0](); await Promise.resolve(); await Promise.resolve();
  assert.equal(opened, replaced ? 0 : 1);
  assert.equal(f.waits.length, replaced ? 1 : 2);
});

test('a wait for an old world cannot allow rendering in a newly loaded world', async () => {
  const f = worldFixture(); f.tick(); f.event('gameLoad', {});
  await f.finishWait(0); f.tick(); assert.equal(f.arrays[2].updates.at(-1), 0);
  await f.finishWait(1); f.tick(); assert.equal(f.arrays[2].updates.at(-1), 1);
});

test('pre-load invalidates views and suspends rendering before the new world finishes loading', async () => {
  const f = worldFixture(); f.tick(); f.native('preLoadGame');
  await f.finishWait(0); f.tick(); assert.equal(f.arrays.at(-2).updates.length, 0);
  f.event('gameLoad', {}); await f.finishWait(1); f.tick();
  assert.equal(f.arrays.at(-2).updates.at(-1), 1);
});

test('a replacement view and single-player/disconnected sessions cannot be changed by stale timers', async () => {
  const f = worldFixture(); f.tick(); f.sp.storage.view = {};
  await f.finishWait(); f.tick(); assert.equal(f.arrays[0].updates.at(-1), 0);
  f.view.setFormViewUpdateAllowed(true); f.tick(); f.setConnected(false); f.tick();
  assert.equal(f.arrays[0].resizes.at(-1), 0);
  const updates = f.arrays.reduce((n, a) => n + a.updates.length, 0);
  f.setConnected(true); f.setSinglePlayer(true); f.tick();
  assert.equal(f.arrays.reduce((n, a) => n + a.updates.length, 0), updates);
});

function formViewFixture() {
  const f = fixture();
  let nextId = id, nullSpawn = false;
  const base = { getFormID: () => 123, getType: () => 43 };
  const resolve = f.sp.Game.getFormEx;
  f.sp.Game.getFormEx = formId => formId === 123 ? base : resolve(formId);
  f.sp.Game.getPlayer = () => ({ placeAtMe() {
    if (nullSpawn) return null;
    const newId = nextId++; f.forms.set(newId, { handle: newId - id + 1, base: 123, deleted: false });
    return resolve(newId);
  } });
  Object.assign(f.sp, { getReferenceHandle: f.adapter.identity, ActorBase: { from: () => null },
    FormType: { NPC: 43 }, printConsole() {}, storage: {} });
  class OwnedReferenceService {} class WorldCleanerService {} class GamemodeUpdateService {}
  const protections = [], visible = [];
  const controller = { emitter: new EventEmitter(), lookupListener(type) {
    if (type === OwnedReferenceService) return { references: f.lifetime };
    if (type === WorldCleanerService) return { modWcProtection: (...args) => protections.push(args) };
    if (type === GamemodeUpdateService) return { updateNeighbor: ref => visible.push(ref.getFormID()) };
    throw Error('Unexpected lookup');
  } };
  const { SpawnProcess } = load('view/spawnProcess.ts', {
    skyrimPlatform: f.sp, '../sync/appearance': {},
    '../extensions/objectReferenceEx': { ObjectReferenceEx: {} },
  });
  const { FormView } = load('view/formView.ts', {
    skyrimPlatform: f.sp, '../sync/animation': {}, '../sync/appearance': {}, '../sync/equipment': {},
    '../lib/errors': {}, '../sync/movementApply': {}, './spawnProcess': { SpawnProcess },
    '../extensions/objectReferenceEx': { ObjectReferenceEx: { getWorldOrCell: () => 60 } },
    './playerCharacterDataHolder': {}, '../sync/movementGet': {}, './hostAttempts': {}, './modelApplyUtils': {},
    './worldViewMisc': {}, '../services/spApiInteractor': { SpApiInteractor: { getControllerInstance: () => controller } },
    '../services/services/worldCleanerService': { WorldCleanerService },
    '../services/services/gamemodeUpdateService': { GamemodeUpdateService },
    '../services/services/ownedReferenceService': { OwnedReferenceService },
  });
  const view = new FormView(id + 123);
  // This fixture targets lifecycle, not combat/rendering or model application.
  view.applyAll = () => {};
  const model = { baseId: 123, refrId: id + 123, movement: { worldOrCell: 60, pos: [1, 2, 3], rot: [0, 0, 0] } };
  return { ...f, view, protections, visible, update: () => view.update(model), setNullSpawn(value) { nullSpawn = value; } };
}

test('FormView destruction is idempotent and a canceled old spawn cannot make a new view ready', async () => {
  const f = formViewFixture(); f.update(); f.tick();
  assert.equal(f.view.getLocalRefrId(), id);
  f.view.destroy(); f.view.destroy(); assert.equal(f.view.getLocalRefrId(), 0);
  f.update(); assert.equal(f.view.getLocalRefrId(), id + 1);
  await f.complete(); f.tick(); f.update();
  assert.deepEqual(f.visible, [], 'Old completion must not mark the replacement spawn ready');
  // Resolve whichever native jobs complete first; each reference must retain
  // its own chain and old cleanup must not delete the replacement actor.
  for (let i = 0; i < 12 && !f.visible.length; ++i) {
    if (f.jobs.length) await f.complete(); f.tick(); f.update();
  }
  assert.deepEqual(f.visible, [id + 1]);
  assert.equal(f.forms.get(id).deleted, true); assert.equal(f.forms.get(id + 1).deleted, false);
  assert.equal(f.calls.filter(c => c[0] === 'delete' && c[1] === id).length, 1);
});

test('FormView handles a native null spawn without dereferencing it or creating pending work', () => {
  const f = formViewFixture(); f.setNullSpawn(true); f.update(); f.tick();
  assert.equal(f.view.getLocalRefrId(), 0); assert.equal(f.lifetime.state.references.size, 0);
  assert.equal(f.calls.length, 0);
});

test('a reused model array slot retires the previous remote identity', () => {
  const created = [], destroyed = [];
  class FormView {
    constructor(remote) { this.remote = remote; created.push(remote); }
    getRemoteRefrId() { return this.remote; }
    destroy() { destroyed.push(this.remote); }
    update() {}
  }
  const { FormViewArray } = load('view/formViewArray.ts', {
    './formView': { FormView }, '../services/spApiInteractor': {}, '../services/services/gamemodeUpdateService': {},
  });
  const views = new FormViewArray(); views.updateForm({ refrId: id }, 0); views.updateForm({ refrId: id + 1 }, 0);
  assert.deepEqual(created, [id, id + 1]); assert.deepEqual(destroyed, [id]);
});

for (const loadNewWorld of [false, 'gameLoad', 'preLoadGame']) test(`service recovery ${loadNewWorld ? `discards an old world on ${loadNewWorld} before first update` : 'restores protection until pending cleanup completes'}`, async () => {
  const f = fixture(), reference = f.lifetime.claim(id);
  f.lifetime.enqueue(reference, () => f.work('pending')); f.tick();
  const events = new EventEmitter(), emitter = new EventEmitter(), changes = [];
  const controller = { on: (...args) => events.on(...args), once: (...args) => events.once(...args), emitter,
    lookupListener: () => ({ modWcProtection: (...args) => changes.push(args), operations: { isBusy: () => false } }) };
  const { OwnedReferenceService } = load('services/services/ownedReferenceService.ts', {
    '../../campaign/ownedReferenceLifetime': { OwnedReferenceLifetime }, '../../logging': { logError() {} },
    './clientListener': { ClientListener: class {} }, './worldCleanerService': {},
    './nativeReferenceOperationService': {},
  });
  const service = new OwnedReferenceService({
    storage: { boyhouseOwnedReferences: f.lifetime.state }, getReferenceHandle: f.adapter.identity,
    Game: { getFormEx: formId => ({ delete: () => f.work('delete', formId, () => { f.forms.get(formId).deleted = true; }) }) },
    Actor: { from: () => null }, ObjectReference: { from: value => value },
  }, controller);
  if (loadNewWorld === 'preLoadGame') events.emit('preLoadGame');
  else if (loadNewWorld) emitter.emit('gameLoad', {});
  events.emit('update'); await f.complete(); events.emit('update');
  if (loadNewWorld) {
    assert.deepEqual(changes, []); assert.equal(f.jobs.length, 0);
  } else {
    assert.deepEqual(changes, [[id, 1]]); assert.equal(f.jobs[0].name, 'delete');
    await f.complete(); events.emit('update'); assert.deepEqual(changes, [[id, 1], [id, -1]]);
  }
  assert.equal(service.references.state.references.size, 0);
});
