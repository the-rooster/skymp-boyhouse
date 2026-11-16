const { test } = require('node:test');
const assert = require('node:assert/strict');
const { NativeReferenceOperations } = require('../../skymp5-client/src/campaign/nativeReferenceOperations.ts');
const { OwnedReferenceLifetime } = require('../../skymp5-client/src/campaign/ownedReferenceLifetime.ts');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const { EventEmitter } = require('node:events');
const root = path.resolve(__dirname, '../../skymp5-client');
const ts = require(require.resolve('typescript', { paths: [root, path.resolve(root, '../skymp5-server')] }));
function load(file, dependencies) {
  const module = { exports: {} };
  const compiled = ts.transpileModule(fs.readFileSync(path.join(root, 'src', file), 'utf8'), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
  }).outputText;
  vm.runInNewContext(compiled, { module, exports: module.exports, require: name => {
    assert.ok(name in dependencies, `Unexpected dependency ${name}`); return dependencies[name];
  } });
  return module.exports;
}

function fixture() {
  const calls = [], pending = [], errors = [], identities = new Map([[100, 10], [101, 11]]);
  let blocked = false;
  const adapter = { identity: id => identities.get(id) || 0, canStart: () => !blocked, report: job => errors.push(job.error) };
  const queue = new NativeReferenceOperations(adapter);
  const step = name => () => { calls.push(name); return new Promise((resolve, reject) => pending.push({ resolve, reject })); };
  return { queue, calls, pending, errors, identities, step, adapter,
    submit(key = 'visibility', run = step(key), current = () => true, persistent = false, complete) {
      return queue.submit(100, key, current, run, complete, persistent);
    },
    block(value) { blocked = value; },
    async finish(error) { const next = pending.shift(); assert.ok(next); error ? next.reject(error) : next.resolve(); await Promise.resolve(); },
  };
}

test('native property sequences serialize every step and finish only on update', async () => {
  const f = fixture(); let completed = 0;
  const job = f.submit('harvest', [f.step('disable'), f.step('enable')], undefined, false, () => ++completed);
  f.submit('motion'); f.queue.update(); assert.deepEqual(f.calls, ['disable']);
  await f.finish(); assert.deepEqual(f.calls, ['disable']); assert.equal(job.status, 'running');
  f.queue.update(); assert.deepEqual(f.calls, ['disable', 'enable']); assert.equal(completed, 0);
  await f.finish(); f.queue.update(); assert.equal(completed, 1); assert.equal(job.status, 'complete');
  assert.deepEqual(f.calls, ['disable', 'enable', 'motion']);
});

test('coalescing replaces queued property updates without replacing an active native call', async () => {
  const f = fixture(), first = f.submit(); f.queue.update();
  const second = f.submit('visibility', f.step('second')), third = f.submit('visibility', f.step('third'));
  assert.equal(first.status, 'running'); assert.equal(second.status, 'cancelled'); assert.equal(third.status, 'queued');
  await f.finish(); f.queue.update(); assert.deepEqual(f.calls, ['visibility', 'third']);
});

test('cancellation drains active calls and discards their remaining steps and callbacks', async () => {
  const f = fixture(); let completed = 0;
  const old = f.submit('old', [f.step('old-step'), f.step('must-not-run')], undefined, false, () => ++completed);
  f.queue.update(); f.queue.cancelEpoch(); const next = f.submit('new');
  f.queue.update(); assert.equal(f.queue.isBusy(100), true); assert.equal(old.status, 'running');
  assert.equal(next.status, 'queued');
  await f.finish(); f.queue.update();
  assert.equal(old.status, 'cancelled'); assert.equal(completed, 0); assert.deepEqual(f.calls, ['old-step', 'new']);
});

test('persistent restoration survives a campaign reset but never a world replacement', async () => {
  const f = fixture(), job = f.submit('restoration', undefined, undefined, true);
  f.queue.cancelEpoch(); f.queue.update(); assert.equal(job.status, 'running');
  let terminal = 0; job.onTerminal = () => ++terminal;
  f.queue.forgetWorld(); assert.equal(job.status, 'cancelled'); assert.equal(terminal, 1);
  f.submit('new-world'); f.queue.update(); await f.finish(); f.queue.update();
  assert.equal(f.queue.isBusy(), true); assert.equal(terminal, 1); // Only the new call remains.
});

test('reused identities cannot run a queued operation or pass an unsettled call', async () => {
  const f = fixture(), job = f.submit(); f.queue.update(); f.identities.set(100, 12);
  assert.throws(() => f.submit('replacement'), /still busy/);
  await f.finish(); f.queue.update(); assert.equal(job.status, 'cancelled');
  const next = f.submit('replacement'); f.queue.update(); assert.equal(next.handle, 12);
});

test('a caller guard cancels queued and follow-up work when its view is replaced', async () => {
  const f = fixture(); let current = true;
  const job = f.submit('steps', [f.step('first'), f.step('late')], () => current);
  f.queue.update(); current = false; await f.finish(); f.queue.update();
  assert.equal(job.status, 'cancelled'); assert.deepEqual(f.calls, ['first']); assert.equal(f.queue.isBusy(), false);
});

test('native rejection records failure instead of acknowledging the operation', async () => {
  const f = fixture(); let completed = 0;
  const job = f.submit('fail', f.step('fail'), undefined, false, () => ++completed);
  f.queue.update(); await f.finish(Error('native rejected')); f.queue.update();
  assert.equal(job.status, 'failed'); assert.equal(completed, 0); assert.match(f.queue.state.errors[0].error, /native rejected/);
  f.queue.cancelEpoch(); assert.equal(f.queue.state.errors.length, 1);
  f.queue.forgetWorld(); assert.equal(f.queue.state.errors.length, 0);
});

test('synchronous native failure and completion failure settle once', async () => {
  const f = fixture(); let terminal = 0;
  const job = f.submit('throw', () => { throw Error('sync'); }); job.onTerminal = () => ++terminal;
  f.queue.update(); f.queue.update(); assert.equal(job.status, 'failed'); assert.equal(terminal, 1);
  const after = f.submit('after', f.step('native'), undefined, false, () => { throw Error('after'); });
  f.queue.update(); await f.finish(); f.queue.update(); assert.equal(after.status, 'failed'); assert.equal(f.errors.length, 2);
});

test('a peer native operation blocks starting work without discarding the request', () => {
  const f = fixture(), job = f.submit(); f.block(true); f.queue.update(); f.queue.update();
  assert.equal(job.status, 'queued'); assert.equal(f.calls.length, 0);
  f.block(false); f.queue.update(); assert.equal(job.status, 'running');
});

test('a failing caller guard is reported without throwing from the update loop', () => {
  const f = fixture(), job = f.submit('guard', undefined, () => { throw Error('guard failed'); });
  f.queue.update(); assert.equal(job.status, 'failed'); assert.equal(f.calls.length, 0);
  assert.equal(f.queue.state.errors.length, 1); assert.equal(f.queue.isBusy(), false);
});

test('updates and queue growth are bounded and invalid sequences leave no empty lanes', () => {
  const f = fixture();
  assert.throws(() => f.submit('empty', [])); assert.equal(f.queue.isBusy(), false);
  for (let i = 0; i < 32; ++i) {
    const id = 200 + i; f.identities.set(id, 1000 + i);
    f.queue.submit(id, 'work', () => true, f.step(String(id)));
  }
  f.queue.update(); assert.equal(f.calls.length, 16);
  for (let i = 0; i < 16; ++i) f.submit(String(i));
  assert.throws(() => f.submit('overflow'), /limit/);
  assert.throws(() => f.queue.update(65), /budget/);
});

test('reload adoption retains pending native work until it settles', async () => {
  const f = fixture(); const old = f.submit(); f.queue.update();
  const adopted = new NativeReferenceOperations(f.adapter, f.queue.state); adopted.cancelEpoch();
  adopted.update(); assert.equal(adopted.isBusy(), true);
  await f.finish(); adopted.update(); assert.equal(old.status, 'cancelled'); assert.equal(adopted.isBusy(), false);
});

function servicesFixture() {
  const events = new EventEmitter(), emitter = new EventEmitter(), calls = [], pending = [], changes = [], errors = [];
  const forms = new Map([[0xff001000, { handle: 100, disabled: false }], [0xff001001, { handle: 101, disabled: false }]]);
  const values = new Map(['fDiffMultHPToPCE', 'fDiffMultHPToPCH', 'fDiffMultHPToPCL', 'fDiffMultHPToPCN',
    'fDiffMultHPToPCVE', 'fDiffMultHPToPCVH'].map((key, i) => [key, (i + 1) / 4]));
  let frame = 0;
  const reference = id => {
    const state = forms.get(id); if (!state) return null;
    const captured = frame, valid = () => assert.equal(frame, captured, 'Native wrapper crossed update callbacks');
    const operation = (name, change = () => {}) => {
      valid(); calls.push([name, id]);
      return new Promise((resolve, reject) => pending.push({ name, id, reject, resolve() { change(); resolve(); } }));
    };
    return { getFormID() { valid(); return id; }, isDeleted: () => !forms.has(id), isDisabled: () => state.disabled,
      disable: () => operation('disable', () => { state.disabled = true; }),
      enable: () => operation('enable', () => { state.disabled = false; }),
      delete: () => operation('delete', () => forms.delete(id)), forceRemoveRagdollFromWorld: () => operation('ragdoll'),
    };
  };
  class OwnedReferenceServiceToken {} class WorldCleanerService {}
  const dependencies = {
    './clientListener': { ClientListener: class {} }, '../../logging': { logError: (...args) => errors.push(args) },
    '../../campaign/nativeReferenceOperations': { NativeReferenceOperations },
    '../../campaign/ownedReferenceLifetime': { OwnedReferenceLifetime },
    './ownedReferenceService': { OwnedReferenceService: OwnedReferenceServiceToken }, './worldCleanerService': { WorldCleanerService },
  };
  const { NativeReferenceOperationService } = load('services/services/nativeReferenceOperationService.ts', dependencies);
  dependencies['./nativeReferenceOperationService'] = { NativeReferenceOperationService };
  const { OwnedReferenceService } = load('services/services/ownedReferenceService.ts', dependencies);
  const { RagdollService } = load('services/services/ragdollService.ts', dependencies);
  const sp = { storage: {}, getReferenceHandle: id => forms.get(id)?.handle || 0,
    Game: { getFormEx: reference, getGameSettingFloat: key => values.get(key), setGameSettingFloat: (key, value) => values.set(key, value) },
    ObjectReference: { from: value => value }, Actor: { from: value => value }, TESModPlatform: { setWeaponDrawnMode() {} },
  };
  let native, owned;
  const controller = { emitter, on: (...args) => events.on(...args), once: (...args) => events.once(...args),
    lookupListener(type) {
      if (type === NativeReferenceOperationService) return native;
      if (type === OwnedReferenceServiceToken) return owned;
      if (type === WorldCleanerService) return { modWcProtection: (...args) => changes.push(args) };
      throw Error('Unexpected service lookup');
    },
  };
  native = new NativeReferenceOperationService(sp, controller); owned = new OwnedReferenceService(sp, controller);
  const ragdoll = new RagdollService(sp, controller);
  return { native, owned, ragdoll, values, calls, forms, changes, errors, emitter, events, pending, reference, sp,
    update() { ++frame; events.emit('update'); },
    async finish(name) { const i = name ? pending.findIndex(job => job.name === name) : 0; assert.ok(i >= 0 && pending[i]);
      pending.splice(i, 1)[0].resolve(); await Promise.resolve(); },
  };
}

test('owned deletion waits for borrowed native work and borrowed work waits for an owned spawn', async () => {
  const f = servicesFixture(), id = 0xff001000, lease = f.owned.references.claim(id);
  f.owned.references.enqueue(lease, () => f.reference(id).enable()); f.owned.references.update();
  const job = f.native.submit(id, 'visibility', () => true, ref => ref.disable());
  f.update(); assert.deepEqual(f.calls.map(c => c[0]), ['enable']);
  await f.finish(); f.update(); assert.equal(job.status, 'running');
  f.owned.references.retire(lease); f.update(); assert.equal(f.calls.some(c => c[0] === 'delete'), false);
  await f.finish(); f.update(); assert.deepEqual(f.calls.map(c => c[0]), ['enable', 'disable', 'delete']);
  await f.finish(); f.update(); assert.equal(f.forms.has(id), false); assert.equal(f.native.operations.isBusy(), false);
});

test('persistent suppression observes native completion across an epoch cancellation', async () => {
  const f = servicesFixture(); let result = 'pending';
  f.native.submitPersistent(0xff001000, 'suppression', () => true, ref => ref.disable()).then(() => { result = 'done'; });
  f.emitter.emit('worldModelReset'); f.update(); await f.finish(); assert.equal(result, 'pending');
  f.update(); await Promise.resolve(); assert.equal(result, 'done');
});

test('ragdoll damage leases overlap and restore the exact original difficulty settings', async () => {
  const f = servicesFixture(), before = [...f.values.values()]; let completed = 0;
  f.ragdoll.safeRemoveRagdollFromWorld(f.reference(0xff001000), () => ++completed);
  f.ragdoll.safeRemoveRagdollFromWorld(f.reference(0xff001001), () => ++completed); f.update();
  assert.ok([...f.values.values()].every(v => v === 0));
  await f.finish(); f.update(); assert.equal(completed, 1); assert.ok([...f.values.values()].every(v => v === 0));
  await f.finish(); f.update(); assert.equal(completed, 2); assert.deepEqual([...f.values.values()], before);
});

test('cancelled ragdoll waits for native completion, suppresses its callback and preserves external settings', async () => {
  const f = servicesFixture(), before = [...f.values.values()]; let completed = 0;
  const job = f.ragdoll.safeRemoveRagdollFromWorld(f.reference(0xff001000), () => ++completed); f.update();
  f.emitter.emit('worldModelReset'); f.update(); assert.ok([...f.values.values()].every(v => v === 0));
  f.values.set('fDiffMultHPToPCE', 3); await f.finish(); f.update();
  assert.equal(job.status, 'cancelled'); assert.equal(completed, 0);
  assert.deepEqual([...f.values.values()], [3, ...before.slice(1)]);
});

test('new save discards old operations and restores the ragdoll global override on update', async () => {
  const f = servicesFixture(), before = [...f.values.values()];
  const job = f.ragdoll.safeRemoveRagdollFromWorld(f.reference(0xff001000), () => assert.fail('Old callback ran'));
  f.update(); f.events.emit('preLoadGame'); f.update();
  assert.equal(job.status, 'cancelled'); assert.deepEqual([...f.values.values()], before);
  f.emitter.emit('gameLoad', {}); await f.finish(); f.update(); assert.equal(f.native.operations.isBusy(), false);
});
