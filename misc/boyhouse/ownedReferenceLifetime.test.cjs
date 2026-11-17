const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const { EventEmitter } = require('node:events');
const { OwnedReferenceLifetime } = require('../../skymp5-client/src/campaign/ownedReferenceLifetime.ts');
const root = path.resolve(__dirname, '../../skymp5-client');
const ts = require(require.resolve('typescript', { paths: [root, path.resolve(root, '../skymp5-server')] }));
const id = 0xff000001;

function load(file, dependencies, extra = {}) {
  const compiled = ts.transpileModule(fs.readFileSync(path.join(root, 'src', file), 'utf8'), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
  }).outputText;
  const module = { exports: {} };
  vm.runInNewContext(compiled, { module, exports: module.exports, ...extra, require: name => {
    if (!(name in dependencies)) throw Error(`Unexpected dependency ${name}`);
    return dependencies[name];
  } }, { filename: file });
  return module.exports;
}

function fixture(count = 1) {
  const forms = new Map(Array.from({ length: count }, (_, i) => [id + i,
    { handle: i + 1, base: 123, deleted: false, dead: true, disabled: true, loaded: true, position: [0, 0, 0] }]));
  const calls = [], jobs = [], errors = [], released = [];
  let frame = 0, now = 0;
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
      setActorValue() { valid(); }, startDeferredKill() { valid(); forms.get(formId).deferredKill = true; }, setDisplayName() { valid(); },
      isDeleted() { valid(); return forms.get(formId).deleted; },
      isDisabled() { valid(); return !!forms.get(formId).disabled; },
      isDead() { valid(); return !!forms.get(formId).dead; },
      is3DLoaded() { valid(); return forms.get(formId).loaded !== false; },
      isAIEnabled() { valid(); return forms.get(formId).ai !== false; },
      enableAI(enabled) { valid(); forms.get(formId).ai = enabled; },
      getPositionX() { valid(); return forms.get(formId).position?.[0] || 0; },
      getPositionY() { valid(); return forms.get(formId).position?.[1] || 0; },
      getPositionZ() { valid(); return forms.get(formId).position?.[2] || 0; },
      setPosition(...position) { valid(); return work('position', formId, () => {
        if (!forms.get(formId).ignorePosition) forms.get(formId).position = position;
      }); },
      setAngle() { valid(); return work('angle', formId); },
      enable() { valid(); return work('enable', formId, () => { forms.get(formId).disabled = false; }); },
      resurrect() { valid(); return work('resurrect', formId, () => { forms.get(formId).dead = false; }); },
      endDeferredKill() { valid(); calls.push(['endDeferredKill', formId]); },
      killSilent() { valid(); calls.push(['killSilent', formId]); forms.get(formId).dead = true; },
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
  }, { Date: { now: () => now } });
  return { lifetime, adapter, forms, calls, jobs, errors, released, work, sp,
    tick() { ++frame; lifetime.update(); },
    spawn(reference, complete, appearance = null, rotation, dead = false) {
      new SpawnProcess(appearance, [1, 2, 3], reference, lifetime, complete, rotation, dead);
    },
    advance(ms) { now += ms; },
    async drain(limit = 20) {
      for (let i = 0; i < limit; ++i) {
        if (jobs.length) await this.complete();
        await Promise.resolve(); this.tick();
      }
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
  await f.complete(); f.tick(); assert.equal(ready, 0, 'Native completion is not observed readiness');
  await f.drain(); assert.equal(ready, 1);
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

test('an initially dead model creates an observed corpse without resurrecting it', async () => {
  const f = fixture(), reference = f.lifetime.claim(id); let ready = 0;
  f.forms.get(id).dead = false;
  f.spawn(reference, () => ++ready, null, [0, 0, 90], true); f.tick(); await f.drain();
  assert.equal(ready, 1); assert.equal(f.forms.get(id).dead, true);
  assert.equal(f.calls.filter(c => c[0] === 'killSilent').length, 1);
  assert.equal(f.calls.some(c => c[0] === 'resurrect'), false); assert.equal(f.errors.length, 0);
});

test('native completion waits for a loaded model at the target position and stable observations', async () => {
  const f = fixture(), reference = f.lifetime.claim(id); let ready = 0;
  f.forms.get(id).position = [1000, 0, 0]; f.forms.get(id).ignorePosition = true;
  f.forms.get(id).loaded = false; f.spawn(reference, () => ++ready); f.tick(); await f.drain();
  assert.equal(ready, 0);
  f.forms.get(id).loaded = true; await f.drain();
  assert.equal(ready, 0);
  f.forms.get(id).position = [1, 2, 3]; await f.drain(1); assert.equal(ready, 0);
  await f.drain(5); assert.equal(ready, 1);
});

for (const ai of [true, false]) test(`spawn holds AI until observed and restores its original ${ai ? 'enabled' : 'disabled'} state`, async () => {
  const f = fixture(), reference = f.lifetime.claim(id); f.forms.get(id).ai = ai;
  let ready = 0; f.spawn(reference, () => ++ready); f.tick();
  assert.equal(f.forms.get(id).ai, false); assert.equal(ready, 0);
  await f.drain(); assert.equal(ready, 1); assert.equal(f.forms.get(id).ai, ai);
  assert.equal(f.calls.filter(c => c[0] === 'position').length, 2, 'Loaded placement must follow disabled placement');
});

test('an AI restoration no-op fails the spawn instead of leaving an acknowledged frozen actor', async () => {
  const f = fixture(), reference = f.lifetime.claim(id); let ready = 0;
  const actor = f.sp.Actor.from;
  f.sp.Actor.from = value => {
    const result = actor(value);
    return result ? { ...result, enableAI(enabled) { if (!enabled) result.enableAI(false); } } : null;
  };
  f.spawn(reference, () => ++ready); f.tick(); await f.drain();
  assert.equal(ready, 0); assert.match(reference.error, /restore its original AI/);
  assert.equal(f.forms.get(id).deleted, true);
});

test('a native enable no-op retires the failed spawn instead of reporting readiness', async () => {
  const f = fixture(), reference = f.lifetime.claim(id); let ready = 0;
  f.spawn(reference, () => ++ready); f.tick(); await f.complete(); f.tick();
  assert.equal(f.jobs[0].name, 'enable'); await f.complete({ apply: false }); f.tick(); await f.drain();
  assert.equal(ready, 0); assert.match(reference.error, /enable did not take effect/);
  assert.equal(f.forms.get(id).deleted, true);
});

test('an actor that refuses the requested death state times out without acknowledging readiness', async () => {
  const f = fixture(), reference = f.lifetime.claim(id); let ready = 0;
  f.forms.get(id).dead = false;
  f.sp.Actor.from = value => value ? { ...value, killSilent() {} } : null;
  f.spawn(reference, () => ++ready, null, undefined, true); f.tick(); await f.drain();
  assert.equal(ready, 0); f.advance(31000); await f.drain();
  assert.match(reference.error, /deadline/); assert.equal(f.forms.get(id).deleted, true); assert.equal(ready, 0);
});

test('cancelling during model observation prevents a later loaded model from reporting ready', async () => {
  const f = fixture(), reference = f.lifetime.claim(id); let ready = 0;
  f.forms.get(id).loaded = false; f.spawn(reference, () => ++ready); f.tick(); await f.drain();
  f.lifetime.retire(reference); f.forms.get(id).loaded = true; await f.drain();
  assert.equal(ready, 0); assert.equal(f.forms.get(id).deleted, true); assert.equal(f.errors.length, 0);
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
    getSpawnReadiness() { return { ready: true, pending: [], failed: [], outsideWorld: 0 }; }
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

test('world readiness requires the current loaded world, view gate, self model, and connection', () => {
  const f = worldFixture(), model = { playerCharacterFormIdx: 0, forms: [{ movement: { worldOrCell: 60 } }] };
  assert.equal(f.view.getSpawnReadiness(model).ready, false);
  f.view.setFormViewUpdateAllowed(true); assert.equal(f.view.getSpawnReadiness(model).ready, true);
  model.forms[0].movement.worldOrCell = 61; assert.equal(f.view.getSpawnReadiness(model).ready, false);
  model.forms[0].movement.worldOrCell = 60;
  f.setConnected(false); assert.equal(f.view.getSpawnReadiness(model).ready, false);
  f.setConnected(true); f.setSinglePlayer(true); assert.equal(f.view.getSpawnReadiness(model).ready, false);
  f.setSinglePlayer(false); f.native('preLoadGame'); assert.equal(f.view.getSpawnReadiness(model).ready, false);
  f.event('gameLoad', {}); f.view.setFormViewUpdateAllowed(true);
  assert.equal(f.view.getSpawnReadiness(model).ready, true);
  f.sp.storage.view = {}; assert.equal(f.view.getSpawnReadiness(model).ready, false);
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

for (const explicitlyBlocked of [false, true]) test(`failed load releases view loading while preserving an explicit gate: ${explicitlyBlocked}`, async () => {
  const f = worldFixture(), model = { playerCharacterFormIdx: 0, forms: [{ movement: { worldOrCell: 60 } }] };
  f.tick();
  if (explicitlyBlocked) f.view.setFormViewUpdateAllowed(false);
  f.native('preLoadGame'); await f.finishWait(0);
  f.native('postLoadGame', { success: false });
  assert.equal(f.waits.length, 1, 'The native tick must not call a Papyrus wait');
  f.tick(); assert.equal(f.waits.length, explicitlyBlocked ? 1 : 2);
  if (!explicitlyBlocked) await f.finishWait(1);
  f.tick(); assert.equal(f.view.getSpawnReadiness(model).ready, !explicitlyBlocked);
  f.view.setFormViewUpdateAllowed(true);
  assert.equal(f.view.getSpawnReadiness(model).ready, true, 'The loading flag was cleared independently of the view gate');
});

test('new-game replacement destroys views immediately but starts its Papyrus wait only on update', async () => {
  const f = worldFixture(); f.tick();
  const oldViews = f.arrays.at(-2);
  f.event('gameLoad', { isCausedBySkyrimPlatform: false, isNewGame: true });
  assert.equal(oldViews.resizes.at(-1), 0); assert.equal(f.waits.length, 1);
  await f.finishWait(0); f.tick();
  assert.equal(f.waits.length, 2);
  assert.equal(f.arrays.at(-2).updates.at(-1), 0, 'An old world wait cannot reopen replacement views');
  await f.finishWait(1); f.tick(); assert.equal(f.arrays.at(-2).updates.at(-1), 1);
  f.event('gameLoad', { isCausedBySkyrimPlatform: false, isNewGame: true });
  f.view.setFormViewUpdateAllowed(false); f.tick();
  assert.equal(f.waits.length, 2, 'A newer explicit gate invalidates the deferred new-game wait');
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
    const newId = nextId++; f.forms.set(newId,
      { handle: newId - id + 1, base: 123, deleted: false, dead: false, disabled: true, loaded: true, position: [0, 0, 0] });
    return resolve(newId);
  } });
  Object.assign(f.sp, { getReferenceHandle: f.adapter.identity, ActorBase: { from: () => null },
    FormType: { NPC: 43 }, printConsole() {}, storage: {} });
  class OwnedReferenceService {} class WorldCleanerService {} class GamemodeUpdateService {} class NativeReferenceOperationService {} class ActorFactionsService {}
  let nativeBusy = false;
  const factions = { progress: { ready: false }, requests: [], retired: [],
    newOwner: () => 'view-factions', request(...args) { this.requests.push(args); },
    hasOwner() { return this.requests.length > 0; },
    readiness() { return this.progress; }, retire(owner) { this.retired.push(owner); } };
  const protections = [], visible = [];
  const controller = { emitter: new EventEmitter(), lookupListener(type) {
    if (type === OwnedReferenceService) return { references: f.lifetime };
    if (type === NativeReferenceOperationService) return { operations: { isBusy: () => nativeBusy } };
    if (type === ActorFactionsService) return { leases: factions };
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
    '../services/services/nativeReferenceOperationService': { NativeReferenceOperationService },
    '../services/services/actorFactionsService': { ActorFactionsService },
  });
  const view = new FormView(id + 123);
  // This fixture targets lifecycle, not combat/rendering or model application.
  view.applyAll = () => {};
  const model = { baseId: 123, refrId: id + 123, movement: { worldOrCell: 60, pos: [1, 2, 3], rot: [0, 0, 0] } };
  return { ...f, view, model, protections, visible, factions, update: () => view.update(model),
    setNativeBusy(value) { nativeBusy = value; }, setNullSpawn(value) { nullSpawn = value; } };
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
  for (let i = 0; i < 24 && !f.visible.length; ++i) {
    if (f.jobs.length) await f.complete(); await Promise.resolve(); f.tick(); f.update();
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

test('FormView cannot publish readiness or gameplay before managed factions settle', async () => {
  const f = formViewFixture();
  f.model.campaignFactions = [{ factionId: 0x2bf9b, rank: -1, isMember: false }];
  f.update(); f.tick(); await f.drain(); f.update();
  assert.equal(f.view.getSpawnReadiness(f.model).state, 'pending');
  assert.deepEqual(f.visible, []);
  assert.equal(f.factions.requests[0][1], id);
  f.factions.progress = { ready: true }; f.update();
  assert.equal(f.view.getSpawnReadiness(f.model).state, 'ready');
  assert.deepEqual(f.visible, [id]);
  for (let i = 0; i < 100; ++i) assert.equal(f.view.getSpawnReadiness(f.model).state, 'ready');
  assert.equal(f.factions.requests.length, 1, 'Stable frames must not resubmit or recanonicalize full faction snapshots');
  f.factions.progress = { ready: false, error: 'native rank mismatch' };
  assert.equal(f.view.getSpawnReadiness(f.model).state, 'failed');
  f.view.destroy(); assert.deepEqual(f.factions.retired, ['view-factions']);
});

test('FormView spawns an initially dead model and never applies the live-actor immortality override', async () => {
  const f = formViewFixture(); f.model.isDead = true; f.update(); f.tick(); await f.drain(); f.update();
  assert.equal(f.forms.get(id).dead, true); assert.notEqual(f.forms.get(id).deferredKill, true);
  assert.equal(f.view.getSpawnReadiness(f.model).state, 'ready'); assert.deepEqual(f.visible, [id]);
  f.setNativeBusy(true); assert.equal(f.view.getSpawnReadiness(f.model).state, 'pending');
  f.setNativeBusy(false); f.forms.get(id).handle = 999;
  assert.equal(f.view.getSpawnReadiness(f.model).state, 'pending', 'Reused local identity cannot certify readiness');
});

test('a changed death state supersedes an unfinished FormView spawn without losing the new corpse', async () => {
  const f = formViewFixture(); f.update(); f.tick(); f.model.isDead = true; f.update();
  assert.equal(f.view.getLocalRefrId(), id + 1); await f.drain(32); f.update();
  assert.equal(f.forms.get(id).deleted, true); assert.equal(f.forms.get(id + 1).dead, true);
  assert.equal(f.view.getSpawnReadiness(f.model).state, 'ready'); assert.deepEqual(f.visible, [id + 1]);
});

test('spawn readiness exposes a failed owned spawn and never accepts another remote identity', async () => {
  const f = formViewFixture(); f.update(); f.tick();
  await f.complete({ error: Error('position rejected') }); f.tick();
  const result = f.view.getSpawnReadiness(f.model);
  assert.equal(result.state, 'failed'); assert.match(result.error, /position rejected/);
  assert.equal(f.view.getSpawnReadiness({ ...f.model, refrId: 1234 }).state, 'pending');
});

test('array readiness checks all in-world remote identities and distinguishes pending, failed, and out-of-world forms', () => {
  class FormView {
    constructor(remote) { this.remote = remote; this.state = 'ready'; }
    getRemoteRefrId() { return this.remote; }
    getSpawnReadiness() { return { state: this.state, error: 'native failed' }; }
    destroy() {} update() {}
  }
  const { FormViewArray } = load('view/formViewArray.ts', {
    './formView': { FormView }, '../services/spApiInteractor': {}, '../services/services/gamemodeUpdateService': {},
  });
  const views = new FormViewArray();
  const model = { playerCharacterFormIdx: 0, forms: [
    { refrId: 1 }, { refrId: 2, movement: { worldOrCell: 60 } },
    { refrId: 3, movement: { worldOrCell: 61 } }, { refrId: 4, movement: { worldOrCell: 60 } },
  ] };
  views.updateForm(model.forms[1], 1);
  let result = views.getSpawnReadiness(model, 60);
  assert.equal(result.ready, false); assert.deepEqual(Array.from(result.pending), [4]); assert.equal(result.outsideWorld, 1);
  views.updateForm(model.forms[3], 3); assert.equal(views.getSpawnReadiness(model, 60).ready, true);
  views.getNthFormView(3).state = 'failed'; result = views.getSpawnReadiness(model, 60);
  assert.equal(result.ready, false); assert.equal(result.failed[0].remoteId, 4);
  model.forms[1].refrId = 99; assert.deepEqual(Array.from(views.getSpawnReadiness(model, 60).pending), [99]);
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

for (const loadNewWorld of [false, 'gameLoad', 'failedLoad']) test(`service recovery ${loadNewWorld === 'gameLoad' ? 'discards a confirmed replacement world before first update' : `restores protection until pending cleanup completes (${loadNewWorld || 'reload'})`}`, async () => {
  const f = fixture(), reference = f.lifetime.claim(id);
  f.lifetime.enqueue(reference, () => f.work('pending')); f.tick();
  const events = new EventEmitter(), emitter = new EventEmitter(), changes = [];
  const controller = { on: (...args) => events.on(...args), once: (...args) => events.once(...args), emitter,
    lookupListener: () => ({ modWcProtection: (...args) => changes.push(args), operations: { isBusy: () => false }, prepareDisposal: () => true }) };
  const { OwnedReferenceService } = load('services/services/ownedReferenceService.ts', {
    '../../campaign/ownedReferenceLifetime': { OwnedReferenceLifetime }, '../../logging': { logError() {} },
    './clientListener': { ClientListener: class {} }, './worldCleanerService': {},
    './nativeReferenceOperationService': {},
    './actorFactionsService': {},
  });
  const service = new OwnedReferenceService({
    storage: { boyhouseOwnedReferences: f.lifetime.state }, getReferenceHandle: f.adapter.identity,
    Game: { getFormEx: formId => ({ delete: () => f.work('delete', formId, () => { f.forms.get(formId).deleted = true; }) }) },
    Actor: { from: () => null }, ObjectReference: { from: value => value },
  }, controller);
  if (loadNewWorld === 'failedLoad') events.emit('preLoadGame');
  else if (loadNewWorld) emitter.emit('gameLoad', {});
  events.emit('update'); await f.complete(); events.emit('update');
  if (loadNewWorld === 'failedLoad') {
    assert.equal(service.references.state.references.size, 1, 'The old handle must remain owned during the attempt');
    assert.equal(f.jobs.length, 0, 'No deletion may start while loading');
    events.emit('postLoadGame'); events.emit('update');
    assert.equal(f.jobs.length, 0, 'A missing result cannot resume native work');
    events.emit('postLoadGame', { success: false }); events.emit('update');
  }
  if (loadNewWorld === 'gameLoad') {
    assert.deepEqual(changes, []); assert.equal(f.jobs.length, 0);
  } else {
    assert.deepEqual(changes, [[id, 1]]); assert.equal(f.jobs[0].name, 'delete');
    await f.complete(); events.emit('update'); assert.deepEqual(changes, [[id, 1], [id, -1]]);
  }
  assert.equal(service.references.state.references.size, 0);
});
