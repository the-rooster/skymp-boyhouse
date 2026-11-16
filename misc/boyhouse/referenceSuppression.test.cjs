const { test } = require('node:test');
const assert = require('node:assert/strict');
const { ReferenceSuppression } = require('../../skymp5-client/src/campaign/referenceSuppression.ts');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { EventEmitter } = require('node:events');
const root = path.resolve(__dirname, '../../skymp5-client');
const ts = require(require.resolve('typescript', { paths: [root, path.resolve(root, '../skymp5-server')] }));

function loadService(file, dependencies) {
  const source = fs.readFileSync(path.join(root, 'src/services/services', file), 'utf8');
  const compiled = ts.transpileModule(source, { compilerOptions: {
    module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020,
  } }).outputText;
  const module = { exports: {} };
  vm.runInNewContext(compiled, { module, exports: module.exports, require: id => {
    if (!(id in dependencies)) throw Error(`Unexpected dependency: ${id}`);
    return dependencies[id];
  } }, { filename: file });
  return module.exports;
}

function serviceFixture(options = {}) {
  const events = new EventEmitter(), emitter = new EventEmitter(), calls = [], errors = [], jobs = [];
  const forms = new Map();
  let singlePlayer = false, connected = true, searches = 0, candidate;
  const makeActor = (id, properties = {}) => {
    const state = { baseId: 123, disabled: false, blocked: false, deleted: false, dead: false,
      dialogue: false, target: null, ...properties };
    const actor = { state, getFormID: () => id, getBaseObject: () => ({ getFormID: () => state.baseId }),
      isDisabled: () => state.disabled, isActivationBlocked: () => state.blocked, isDeleted: () => state.deleted,
      isDead: () => state.dead, isInDialogueWithPlayer: () => state.dialogue, getDialogueTarget: () => state.target,
      blockActivation(blocked) { calls.push(['block', id, blocked]); state.blocked = blocked; },
      disable() { return change(true); }, enable() { return change(false); },
      getPositionX: () => 100, getPositionY: () => 200, getPositionZ: () => 300,
    };
    function change(disabled) {
      calls.push(['disabled', id, disabled]);
      return new Promise(resolve => jobs.push(() => { state.disabled = disabled; resolve(); }));
    }
    for (const method of ['delete', 'kill', 'killSilent', 'setPosition', 'setAlpha'])
      actor[method] = () => assert.fail(`Destructive cleaner call: ${method}`);
    forms.set(id, actor);
    return actor;
  };
  const player = makeActor(0x14), actor = makeActor(100, options);
  candidate = actor;
  class NetworkingService {}
  class SinglePlayerService {}
  const dependencies = {
    './clientListener': { ClientListener: class {} }, '../../logging': { logError: (...args) => errors.push(args) },
    '../../campaign/referenceSuppression': { ReferenceSuppression },
    './networkingService': { NetworkingService }, './singlePlayerService': { SinglePlayerService },
  };
  const { ReferenceSuppressionService } = loadService('referenceSuppressionService.ts', dependencies);
  dependencies['./referenceSuppressionService'] = { ReferenceSuppressionService };
  const { WorldCleanerService } = loadService('worldCleanerService.ts', dependencies);
  const sp = { storage: {}, Game: {
    getPlayer: () => player, getFormEx: id => forms.get(id),
    findRandomActor: () => { ++searches; return candidate; },
  }, ObjectReference: { from: form => form }, Actor: { from: form => form } };
  let suppression;
  const controller = { emitter, on: (...args) => events.on(...args), lookupListener(type) {
    if (type === ReferenceSuppressionService) return suppression;
    if (type === NetworkingService) return { isConnected: () => connected };
    if (type === SinglePlayerService) return { isSinglePlayer: singlePlayer };
    throw Error('Unexpected service');
  } };
  suppression = new ReferenceSuppressionService(sp, controller);
  const cleaner = new WorldCleanerService(sp, controller);
  return { actor, player, calls, errors, jobs, forms, suppression, cleaner, makeActor,
    tick: () => events.emit('update'), event: (...args) => emitter.emit(...args),
    get searches() { return searches; }, setCandidate(value) { candidate = value; },
    setSinglePlayer(value) { singlePlayer = value; }, setConnected(value) { connected = value; },
    async complete() { assert.ok(jobs.length); jobs.shift()(); await Promise.resolve(); },
  };
}

test('world cleaner never discovers or changes actors in a personal save or disconnected session', () => {
  for (const mode of ['singlePlayer', 'disconnected']) {
    const f = serviceFixture();
    if (mode === 'singlePlayer') f.setSinglePlayer(true); else f.setConnected(false);
    for (let i = 0; i < 20; ++i) f.tick();
    assert.equal(f.searches, 0); assert.equal(f.calls.length, 0);
  }
});

test('world cleaner leaves the player, protected views, deleted and disabled actors alone', () => {
  const f = serviceFixture();
  f.setCandidate(f.player); f.tick();
  f.setCandidate(f.makeActor(200, { deleted: true })); f.tick();
  f.setCandidate(f.makeActor(201, { disabled: true })); f.tick();
  f.setCandidate(f.actor); f.cleaner.modWcProtection(100, 1); f.tick(); f.tick();
  assert.equal(f.calls.length, 0);
  assert.equal(f.suppression.references.state.entries.size, 0);
});

test('disconnect restores a hidden vanilla NPC without deleting, moving, or killing it', async () => {
  const f = serviceFixture(), original = structuredClone(f.actor.state);
  f.tick(); f.tick(); await f.complete(); f.tick();
  assert.equal(f.actor.state.disabled, true); assert.equal(f.actor.state.blocked, true);
  f.setConnected(false); f.event('connectionDisconnect'); f.tick();
  assert.equal(f.actor.state.blocked, true);
  await f.complete(); f.tick();
  assert.deepEqual(f.actor.state, original);
  assert.equal(f.suppression.references.state.entries.size, 0);
  assert.equal(f.errors.length, 0);
});

test('authored corpses stay visible and their activation state returns on disconnect', () => {
  const f = serviceFixture({ dead: true });
  f.tick(); f.tick(); f.tick();
  assert.equal(f.actor.state.disabled, false); assert.equal(f.actor.state.blocked, true);
  assert.equal(f.jobs.length, 0);
  assert.equal(f.calls.length, 1);
  f.setConnected(false); f.event('connectionDisconnect'); f.tick();
  assert.equal(f.actor.state.blocked, false);
  const blocked = serviceFixture({ dead: true, blocked: true });
  blocked.tick(); blocked.tick(); blocked.setConnected(false); blocked.tick();
  assert.equal(blocked.calls.length, 0);
  assert.equal(blocked.actor.state.blocked, true);
});

test('a newly protected actor is restored after an already pending native hide', async () => {
  const f = serviceFixture(); f.tick(); f.tick();
  f.cleaner.modWcProtection(100, 1); f.tick();
  assert.equal(f.jobs.length, 1);
  await f.complete(); f.tick(); await f.complete(); f.tick();
  assert.equal(f.actor.state.disabled, false); assert.equal(f.actor.state.blocked, false);
  assert.equal(f.cleaner.getWcProtection(100), 1);
  assert.equal(f.errors.length, 0);
});

test('a quest suppression claim retains an actor after the world cleaner releases it', async () => {
  const f = serviceFixture(); f.tick();
  f.suppression.references.acquire('quest', 100); f.tick(); await f.complete(); f.tick();
  f.cleaner.modWcProtection(100, 1); f.tick();
  assert.equal(f.actor.state.disabled, true); assert.equal(f.jobs.length, 0);
  f.suppression.references.releaseOwner('quest'); f.tick(); await f.complete(); f.tick();
  assert.equal(f.actor.state.disabled, false); assert.equal(f.actor.state.blocked, false);
});

test('loading a different save discards old suppression and pending completions', async () => {
  const f = serviceFixture(); f.tick(); f.tick();
  f.event('gameLoad', { isCausedBySkyrimPlatform: false }); f.setSinglePlayer(true);
  const newActor = f.makeActor(100, { blocked: true }), original = structuredClone(newActor.state);
  await f.complete(); f.tick(); f.tick();
  assert.deepEqual(newActor.state, original);
  assert.equal(f.suppression.references.state.entries.size, 0);
  assert.equal(f.calls.length, 2);
});

test('native adapter waits for dialogue and refuses a replaced base identity', async () => {
  const f = serviceFixture({ dialogue: true }); f.tick(); f.tick(); f.tick();
  assert.equal(f.calls.length, 0);
  f.actor.state.dialogue = false; f.actor.state.target = f.player; f.tick();
  assert.equal(f.calls.length, 0);
  f.actor.state.target = null; f.tick(); await f.complete(); f.tick();
  f.setConnected(false); f.event('connectionDisconnect');
  f.actor.state.baseId = 456; f.tick();
  assert.match(f.suppression.references.error(100), /identity changed/);
  assert.equal(f.jobs.length, 0);
  assert.equal(f.errors.length, 1);
});

function fixture(count = 1, options = {}) {
  const forms = new Map(Array.from({ length: count }, (_, i) => [i + 100, {
    baseId: 10, disabled: false, activationBlocked: false, inDialogue: false,
    position: [1, 2, 3], health: 50, deleted: false, ...options,
  }]));
  const calls = [], jobs = [], errors = [];
  const adapter = {
    read: id => forms.get(id) && { ...forms.get(id) },
    blockActivation(id, baseId, blocked) {
      assert.equal(forms.get(id).baseId, baseId); calls.push(['block', id, blocked]); forms.get(id).activationBlocked = blocked;
    },
    setDisabled(id, baseId, disabled) {
      const form = forms.get(id); assert.equal(form.baseId, baseId);
      calls.push(['disabled', id, disabled]);
      return new Promise((resolve, reject) => jobs.push({ form, disabled, resolve, reject }));
    },
    report: (id, error) => errors.push({ id, error }),
  };
  const ledger = new ReferenceSuppression(adapter);
  return { ledger, adapter, forms, calls, jobs, errors,
    async complete({ error, apply = true } = {}) {
      assert.ok(jobs.length); const job = jobs.shift();
      if (apply) job.form.disabled = job.disabled;
      if (error) job.reject(error); else job.resolve();
      await Promise.resolve();
    },
  };
}

test('overlapping suppression owners restore the exact original flags only after the last release', async () => {
  const f = fixture(), original = structuredClone(f.forms.get(100));
  f.ledger.acquire('cleaner', 100); f.ledger.acquire('phase', 100); f.ledger.update();
  assert.equal(f.ledger.isSettled([100]), false);
  await f.complete(); f.ledger.update();
  assert.equal(f.ledger.isSettled([100]), true);
  f.ledger.releaseOwner('phase'); f.ledger.update();
  assert.equal(f.forms.get(100).disabled, true);
  assert.equal(f.jobs.length, 0);
  f.ledger.releaseOwner('cleaner'); f.ledger.update();
  assert.equal(f.forms.get(100).activationBlocked, true, 'Do not unblock before native enable finishes');
  await f.complete(); f.ledger.update();
  assert.deepEqual(f.forms.get(100), original);
  assert.equal(f.ledger.state.entries.size, 0);
  assert.equal(f.ledger.isSettled([100]), true);
});

for (const disabled of [false, true]) for (const activationBlocked of [false, true]) {
  test(`original flags survive suppression: disabled=${disabled}, blocked=${activationBlocked}`, async () => {
    const f = fixture(1, { disabled, activationBlocked });
    f.ledger.acquire('phase', 100); f.ledger.update();
    if (f.jobs.length) { await f.complete(); f.ledger.update(); }
    f.ledger.releaseAll(); f.ledger.update();
    if (f.jobs.length) { await f.complete(); f.ledger.update(); }
    assert.equal(f.forms.get(100).disabled, disabled);
    assert.equal(f.forms.get(100).activationBlocked, activationBlocked);
    assert.equal(f.ledger.state.entries.size, 0);
    if (disabled) assert.equal(f.calls.some(c => c[0] === 'disabled'), false);
  });
}

test('release and reacquire during disable never launch a competing enable', async () => {
  const f = fixture(); f.ledger.acquire('phase', 100); f.ledger.update();
  f.ledger.releaseAll(); f.ledger.update();
  assert.equal(f.jobs.length, 1);
  f.ledger.acquire('nextPhase', 100); f.ledger.update();
  await f.complete(); f.ledger.update();
  assert.deepEqual(f.calls.filter(c => c[0] === 'disabled'), [['disabled', 100, true]]);
  assert.equal(f.ledger.isSettled([100]), true);
});

test('a new hide requested during restoration is applied after the enable completes', async () => {
  const f = fixture(); f.ledger.acquire('phase', 100); f.ledger.update(); await f.complete(); f.ledger.update();
  f.ledger.releaseAll(); f.ledger.update();
  f.ledger.acquire('nextPhase', 100); f.ledger.update();
  assert.equal(f.jobs.length, 1);
  await f.complete(); f.ledger.update();
  assert.equal(f.jobs.length, 1);
  await f.complete(); f.ledger.update();
  assert.deepEqual(f.calls.filter(c => c[0] === 'disabled').map(c => c[2]), [true, false, true]);
  assert.equal(f.forms.get(100).disabled, true);
});

test('hot reload adopts pending native work and retains the pre-suppression flags', async () => {
  const f = fixture(); f.ledger.acquire('phase', 100); f.ledger.update();
  const reloaded = new ReferenceSuppression(f.adapter, f.ledger.state);
  reloaded.releaseAll(); reloaded.update(); assert.equal(f.jobs.length, 1);
  await f.complete(); reloaded.update(); await f.complete(); reloaded.update();
  assert.equal(f.forms.get(100).disabled, false);
  assert.equal(f.forms.get(100).activationBlocked, false);
  assert.equal(reloaded.state.entries.size, 0);
});

test('a loaded save cannot receive restoration from the previous world', async () => {
  const f = fixture(); f.ledger.acquire('phase', 100); f.ledger.update();
  f.ledger.releaseAll();
  f.ledger.forgetWorld();
  const newForm = { ...f.forms.get(100), baseId: 20, disabled: false, activationBlocked: false };
  f.forms.set(100, newForm);
  await f.complete(); f.ledger.update();
  assert.equal(newForm.disabled, false);
  assert.equal(newForm.activationBlocked, false);
  assert.equal(f.calls.length, 2);
  assert.equal(f.ledger.state.entries.size, 0);
});

test('dialogue suppression waits without moving, killing or changing the actor', async () => {
  const f = fixture(1, { inDialogue: true }), original = structuredClone(f.forms.get(100));
  f.ledger.acquire('phase', 100);
  for (let i = 0; i < 30; ++i) f.ledger.update();
  assert.deepEqual(f.forms.get(100), original); assert.deepEqual(f.calls, []);
  assert.equal(f.ledger.isSettled([100]), false);
  f.forms.get(100).inDialogue = false; f.ledger.update(); await f.complete(); f.ledger.update();
  assert.equal(f.ledger.isSettled([100]), true);
});

test('authored corpses can be protected without being hidden', () => {
  const f = fixture(); f.ledger.acquire('cleaner', 100, 'activation'); f.ledger.update();
  assert.deepEqual(f.calls, [['block', 100, true]]);
  f.ledger.releaseAll(); f.ledger.update();
  assert.deepEqual(f.calls, [['block', 100, true], ['block', 100, false]]);
  assert.equal(f.forms.get(100).disabled, false);
});

test('failed native work remains unacknowledged and can be restored on release', async () => {
  const f = fixture(); f.ledger.acquire('phase', 100); f.ledger.update();
  await f.complete({ error: Error('native failure') }); f.ledger.update();
  assert.equal(f.ledger.isSettled([100]), false);
  assert.match(f.ledger.error(100), /native failure/);
  assert.equal(f.errors.length, 1);
  f.ledger.releaseAll(); f.ledger.update(); await f.complete(); f.ledger.update();
  assert.equal(f.forms.get(100).disabled, false); assert.equal(f.forms.get(100).activationBlocked, false);
});

test('a canceled disable that rejects still restores its original interaction state', async () => {
  const f = fixture(); f.ledger.acquire('phase', 100); f.ledger.update(); f.ledger.releaseAll();
  await f.complete({ error: Error('canceled hide'), apply: false }); f.ledger.update();
  assert.equal(f.forms.get(100).activationBlocked, false); assert.equal(f.ledger.state.entries.size, 0);
});

test('a native no-op is reported instead of being retried indefinitely or acknowledged', async () => {
  const f = fixture(); f.ledger.acquire('phase', 100); f.ledger.update();
  await f.complete({ apply: false }); f.ledger.update();
  for (let i = 0; i < 20; ++i) f.ledger.update();
  assert.equal(f.jobs.length, 0); assert.equal(f.errors.length, 1);
  assert.equal(f.ledger.isSettled([100]), false);
});

test('per-update budgets and duplicate requests bound native work', () => {
  const f = fixture(40);
  for (const id of f.forms.keys()) { f.ledger.acquire('phase', id); f.ledger.acquire('phase', id); }
  f.ledger.update(8); assert.equal(f.jobs.length, 8);
  f.ledger.update(8); assert.equal(f.jobs.length, 16);
  assert.throws(() => f.ledger.update(100));
});

test('a reset during native interaction blocking cannot start an old-world disable', () => {
  const f = fixture();
  const block = f.adapter.blockActivation;
  f.adapter.blockActivation = (...args) => { block(...args); f.ledger.forgetWorld(); };
  f.ledger.acquire('phase', 100); f.ledger.update();
  assert.equal(f.jobs.length, 0); assert.equal(f.ledger.state.entries.size, 0);
});

test('identity changes and capacity failures preserve existing restoration records', () => {
  const f = fixture(2), bounded = new ReferenceSuppression(f.adapter, undefined, 1);
  bounded.acquire('phase', 100);
  assert.throws(() => bounded.acquire('phase', 101), /limit/);
  assert.equal(bounded.state.entries.size, 1);
  f.forms.get(100).baseId = 20; bounded.update();
  assert.match(bounded.error(100), /identity changed/); assert.equal(f.calls.length, 0);
  assert.throws(() => bounded.acquire('phase', 0x14), /Invalid/);
});
