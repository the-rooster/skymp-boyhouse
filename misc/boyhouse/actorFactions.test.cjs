const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const { EventEmitter } = require('node:events');
const root = path.resolve(__dirname, '../../skymp5-client');
const ts = require(require.resolve('typescript', { paths: [root, path.resolve(root, '../skymp5-server')] }));
const load = (name, dependencies = {}) => {
  const module = { exports: {} };
  const source = ts.transpileModule(fs.readFileSync(path.join(root, 'src', name), 'utf8'), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
  }).outputText;
  vm.runInNewContext(source, { module, exports: module.exports, require(name) {
    assert.ok(name in dependencies, `Unexpected dependency ${name}`); return dependencies[name];
  } });
  return module.exports;
};
const parser = load('sync/campaignFactions.ts');
const leaseModule = load('sync/actorFactionLease.ts', { './campaignFactions': parser });
const { ActorFactionLeases } = leaseModule;
const target = (rank = 1, isMember = rank >= 0) => [{ factionId: 0x2bf9b, rank, isMember }];
const plain = value => JSON.parse(JSON.stringify(value));

function native() {
  const calls = [], jobs = new Map();
  let sequence = 0, responseHook = value => value;
  const response = token => responseHook({ ...jobs.get(token) });
  const api = {
    actorFactionsApiVersion: 1,
    beginActorFactions(actorId) {
      assert.ok(![...jobs.values()].some(job => job.actorId === actorId), 'Previous actor lease was not released');
      const token = `native-${++sequence}`;
      jobs.set(token, { token, actorId, status: 'idle', mutationPending: false, uncertain: false });
      calls.push(['begin', actorId, token]); return token;
    },
    applyActorFactions(token, revision, entries) {
      calls.push(['apply', token, revision, plain(entries)]);
      Object.assign(jobs.get(token), { revision, status: 'observing', mutationPending: true });
      return response(token);
    },
    getActorFactionsStatus(token) { calls.push(['observe', token]); return response(token); },
    restoreActorFactions(token) {
      calls.push(['restore', token]);
      Object.assign(jobs.get(token), { status: 'restoring', mutationPending: true }); return response(token);
    },
    releaseActorFactions(token) {
      const job = jobs.get(token);
      assert.ok(['restored', 'superseded'].includes(job.status) && !job.mutationPending);
      calls.push(['release', token]); jobs.delete(token);
    },
  };
  return { api, calls, jobs,
    token: () => [...jobs.keys()][0],
    settle(token = [...jobs.keys()][0]) { Object.assign(jobs.get(token), { status: 'settled', mutationPending: false }); },
    restore(token = [...jobs.keys()][0]) { Object.assign(jobs.get(token), { status: 'restored', mutationPending: false }); },
    hook(callback) { responseHook = callback; },
  };
}

test('faction targets apply only on update and require a later exact observed settlement', () => {
  const n = native(), leases = new ActorFactionLeases(n.api), entries = target();
  leases.request('player', 0x14, entries);
  entries[0].rank = 99;
  assert.equal(n.calls.length, 0);
  assert.equal(leases.readiness().ready, false);
  leases.update();
  assert.deepEqual(n.calls[1], ['apply', n.token(), 1, target()]);
  assert.equal(leases.readiness('player').ready, false);
  n.settle(); leases.update();
  assert.equal(leases.readiness('player').ready, true);
  const count = n.calls.length;
  for (let i = 0; i < 100; ++i) assert.equal(leases.readiness().ready, true);
  assert.equal(n.calls.length, count, 'Reading readiness must not dispatch engine work');
  leases.request('player', 0x14, target()); leases.update();
  assert.equal(n.calls.filter(call => call[0] === 'apply').length, 1);
});

test('an immediate settled apply response cannot acknowledge its own dispatch', () => {
  const n = native(), leases = new ActorFactionLeases(n.api);
  n.hook(value => ({ ...value, status: 'settled', mutationPending: false }));
  leases.request('player', 0x14, target()); leases.update();
  assert.equal(leases.readiness().ready, false);
  leases.update();
  assert.equal(leases.readiness().ready, true);
});

test('replacement faction revisions never accept obsolete token, actor or rank acknowledgements', () => {
  for (const wrong of [
    value => ({ ...value, revision: value.revision - 1 }),
    value => ({ ...value, token: 'previous-world' }),
    value => ({ ...value, actorId: 0xff000001 }),
    value => ({ ...value, uncertain: true }),
    value => ({ ...value, status: 'superseded' }),
  ]) {
    const n = native(), leases = new ActorFactionLeases(n.api);
    leases.request('player', 0x14, target()); leases.update(); n.settle(); leases.update();
    leases.request('player', 0x14, target(-1));
    assert.equal(leases.readiness().ready, false);
    leases.update();
    n.settle(); n.hook(wrong); leases.update();
    assert.equal(leases.readiness().ready, false);
    assert.ok(leases.readiness().error);
  }
});

test('retirement preserves the original state until observed restoration and release', () => {
  const n = native(), leases = new ActorFactionLeases(n.api);
  leases.request('old', 0x14, target()); leases.update();
  const old = n.token();
  leases.retire('old'); leases.request('new', 0x14, target(5));
  assert.equal(leases.drained().ready, false);
  leases.update();
  assert.deepEqual(n.calls.filter(call => call[0] === 'begin'), [['begin', 0x14, old]]);
  assert.equal(leases.drained(0x14).ready, false);
  n.restore(old); leases.update();
  assert.ok(n.calls.some(call => call[0] === 'release' && call[1] === old));
  assert.equal(n.calls.filter(call => call[0] === 'begin').length, 2);
  assert.equal(leases.readiness('new').ready, false);
  n.settle(); leases.update();
  assert.equal(leases.readiness('new').ready, true);
});

test('JS reload retains outstanding leases and invalidates the previous coordinator', () => {
  const n = native(), state = ActorFactionLeases.createState(), old = new ActorFactionLeases(n.api, state);
  old.request('player', 0x14, target()); old.update();
  const token = n.token(), resumed = new ActorFactionLeases(n.api, state);
  assert.equal(resumed.drained().ready, false);
  const count = n.calls.length; old.update(); old.retireAll();
  assert.equal(n.calls.length, count);
  assert.throws(() => old.request('player', 0x14, target(9)));
  resumed.request('fresh', 0x14, target(2)); resumed.update();
  assert.ok(n.calls.some(call => call[0] === 'restore' && call[1] === token));
  n.restore(token); resumed.update(); n.settle(); resumed.update();
  assert.equal(resumed.readiness('fresh').ready, true);
});

test('restoration failure retains the native lease and prevents disposal', () => {
  const n = native(), leases = new ActorFactionLeases(n.api);
  leases.request('npc', 0xff000001, target()); leases.update();
  leases.retireActor(0xff000001);
  n.hook(value => ({ ...value, status: 'failed', uncertain: true, error: 'restore uncertain' }));
  leases.update();
  assert.equal(leases.drained(0xff000001).ready, false);
  assert.match(leases.drained().error, /restore uncertain/);
  assert.equal(n.calls.filter(call => call[0] === 'release').length, 0);
  assert.equal(n.jobs.size, 1);
});

test('failed cleanup keeps polling through reload and releases only after confirmed world supersession', () => {
  for (const reload of [false, true]) {
    const n = native(), state = ActorFactionLeases.createState();
    let leases = new ActorFactionLeases(n.api, state);
    leases.request('npc', 0xff000001, target()); leases.update(); leases.retireAll();
    const token = n.token();
    n.hook(value => ({ ...value, status: 'failed', uncertain: true, error: 'restore uncertain' }));
    leases.update();
    if (reload) leases = new ActorFactionLeases(n.api, state);
    leases.update(); leases.update();
    assert.equal(leases.drained().ready, false);
    assert.equal(n.calls.filter(call => call[0] === 'restore').length, 1, 'Do not retry a failed mutation');
    assert.equal(n.calls.filter(call => call[0] === 'observe').length, 2);
    n.hook(value => value);
    Object.assign(n.jobs.get(token), { status: 'superseded', uncertain: false, mutationPending: true });
    leases.update(); assert.equal(leases.drained().ready, false);
    n.jobs.get(token).mutationPending = false;
    leases.update();
    assert.equal(leases.drained().ready, true);
    assert.equal(leases.drained().error, undefined);
    assert.equal(n.calls.filter(call => call[0] === 'apply').length, 1);
    assert.equal(n.calls.filter(call => call[0] === 'restore').length, 1);
    assert.equal(n.calls.filter(call => call[0] === 'release').length, 1);
  }
});

test('a terminal release exception can recover by observing without restoring again', () => {
  for (const releasedBeforeThrow of [false, true]) {
    const n = native(), leases = new ActorFactionLeases(n.api);
    leases.request('player', 0x14, target()); leases.update(); leases.retireAll(); leases.update();
    const token = n.token(), getStatus = n.api.getActorFactionsStatus;
    n.restore(token);
    let releases = 0;
    n.api.releaseActorFactions = value => {
      ++releases;
      if (releases === 1) {
        if (releasedBeforeThrow) n.jobs.delete(value);
        throw Error('release interrupted');
      }
      n.jobs.delete(value);
    };
    n.api.getActorFactionsStatus = value => n.jobs.has(value) ? getStatus(value) : {
      token: value, actorId: 0, status: 'superseded', mutationPending: false, uncertain: false,
    };
    leases.update(); assert.equal(leases.drained().ready, false);
    assert.match(leases.drained().error, /release interrupted/);
    leases.update();
    assert.equal(leases.drained().ready, true);
    assert.equal(releases, 2);
    assert.equal(n.calls.filter(call => call[0] === 'restore').length, 1);
  }
});

test('a confirmed world replacement releases old tokens without touching replacement actors', () => {
  const n = native(), leases = new ActorFactionLeases(n.api);
  leases.request('npc', 0xff000001, target()); leases.update(); leases.retireAll();
  n.hook(value => ({ ...value, status: 'superseded', mutationPending: false }));
  const release = n.api.releaseActorFactions;
  n.api.releaseActorFactions = token => {
    Object.assign(n.jobs.get(token), { status: 'superseded', mutationPending: false }); release(token);
  };
  leases.update();
  assert.equal(leases.drained().ready, true);
  assert.equal(n.calls.filter(call => call[0] === 'apply').length, 1);
});

test('an already retired native token with no actor can only finish cleanup', () => {
  const n = native(), leases = new ActorFactionLeases(n.api);
  leases.request('npc', 0xff000001, target()); leases.update(); leases.retireAll();
  n.hook(value => ({ ...value, actorId: 0, status: 'superseded', mutationPending: false }));
  n.api.releaseActorFactions = token => { n.jobs.delete(token); };
  leases.update();
  assert.equal(leases.drained().ready, true);
  const active = new ActorFactionLeases(n.api);
  active.request('npc', 0xff000001, target()); active.update();
  assert.equal(active.readiness().ready, false);
  assert.ok(active.readiness().error);
});

test('a failed application may restore while normal pending restoration remains observed', () => {
  const n = native(), leases = new ActorFactionLeases(n.api);
  leases.request('player', 0x14, target()); leases.update();
  n.hook(value => ({ ...value, status: 'failed', uncertain: true, error: 'application drift' }));
  leases.update(); assert.match(leases.readiness().error, /application drift/);
  leases.retireAll(); n.hook(value => value); leases.update();
  assert.equal(leases.drained().ready, false);
  assert.equal(leases.drained().error, undefined);
  leases.update(); assert.equal(leases.drained().ready, false);
  n.restore(); leases.update(); assert.equal(leases.drained().ready, true);
});

test('unsupported API and invalid targets cannot silently acknowledge managed state', () => {
  const leases = new ActorFactionLeases({});
  assert.equal(leases.readiness().ready, true);
  leases.request('player', 0x14, []); leases.update();
  assert.equal(leases.readiness().ready, false);
  assert.match(leases.readiness().error, /API version 1/);
  leases.retireAll(); leases.update(); assert.equal(leases.drained().ready, true);
  assert.throws(() => leases.request('original', 0x1347b, target()));
  assert.throws(() => leases.request('bad', 0x14, [{ factionId: 1, rank: 0 }]));
  const n = native(), active = new ActorFactionLeases(n.api);
  active.request('owner', 0x14, target());
  assert.throws(() => active.request('other', 0x14, target()));
  assert.equal(active.state.leases.size, 1);
});

test('retiring an old copy cannot retire a newer actor owned by the same view', () => {
  const n = native(), leases = new ActorFactionLeases(n.api);
  leases.request('view', 0xff000001, target()); leases.update();
  leases.request('view', 0xff000002, target(3));
  leases.retireActor(0xff000001); leases.update();
  const next = [...n.jobs.values()].find(job => job.actorId === 0xff000002);
  assert.ok(next);
  n.restore([...n.jobs.values()].find(job => job.actorId === 0xff000001).token);
  n.settle(next.token); leases.update();
  assert.equal(leases.readiness('view').ready, true);
});

test('timeouts report stalled work without dispatching native calls or forgetting cleanup', () => {
  let now = 0;
  const n = native(), leases = new ActorFactionLeases(n.api, ActorFactionLeases.createState(), () => now);
  leases.request('player', 0x14, target()); leases.update();
  const count = n.calls.length;
  now = 30001; leases.checkTimeouts();
  assert.equal(n.calls.length, count);
  assert.match(leases.readiness().error, /application timed out/);
  leases.retireAll(); leases.update();
  now += 30001; leases.checkTimeouts();
  assert.match(leases.drained().error, /restoration timed out/);
  assert.equal(n.jobs.size, 1);
  n.restore(); leases.update();
  assert.equal(leases.drained().ready, true, 'Timed-out cleanup must still resume on native updates');
});

test('reentrant cancellation during begin retains its token without applying the retired target', () => {
  const n = native(), leases = new ActorFactionLeases(n.api), begin = n.api.beginActorFactions;
  n.api.beginActorFactions = actor => { const token = begin(actor); leases.retireAll(); return token; };
  leases.request('player', 0x14, target()); leases.update();
  assert.equal(n.calls.filter(call => call[0] === 'apply').length, 0);
  assert.equal(n.jobs.size, 1);
  leases.update(); n.restore(); leases.update();
  assert.equal(leases.drained().ready, true);
});

test('a reentrant newer revision cannot inherit settlement from the dispatched revision', () => {
  const n = native(), leases = new ActorFactionLeases(n.api), apply = n.api.applyActorFactions;
  let replace = true;
  n.api.applyActorFactions = (...args) => {
    const observed = apply(...args);
    if (replace) { replace = false; leases.request('player', 0x14, target(5)); }
    return observed;
  };
  leases.request('player', 0x14, target()); leases.update();
  assert.equal(leases.readiness().ready, false);
  assert.equal(leases.readiness().error, undefined);
  leases.update(); n.settle(); leases.update();
  assert.deepEqual(n.calls.filter(call => call[0] === 'apply').map(call => call[2]), [1, 2]);
  assert.equal(leases.readiness().ready, true);
});

function serviceFixture(storage = {}) {
  const n = native(), events = new EventEmitter(), emitter = new EventEmitter();
  let closed = 0;
  class NetworkingService {}
  const { ActorFactionsService } = load('services/services/actorFactionsService.ts', {
    '../../sync/actorFactionLease': leaseModule, '../../sync/campaignFactions': parser,
    './clientListener': { ClientListener: class {} }, './networkingService': { NetworkingService },
    '../../logging': { logError() {} },
  });
  const service = new ActorFactionsService({ ...n.api, storage }, {
    on: (...args) => events.on(...args), once: (...args) => events.once(...args), emitter,
    lookupListener(type) { assert.equal(type, NetworkingService); return { isConnected: () => true, close() { ++closed; } }; },
  });
  return { ...n, service, events, emitter, storage, closed: () => closed,
    update: () => events.emit('update'), tick: () => events.emit('tick') };
}
const spawn = entries => ({ idx: 7, isMe: true, customPropsJsonDumps: entries === undefined ? [] : [
  { propName: 'campaignFactions', propValueJsonDump: JSON.stringify(entries) },
] });

test('self faction readiness waits for the spawned world and the latest authoritative property', () => {
  const f = serviceFixture(), generation = f.service.beginSpawn(spawn(target()));
  f.update(); assert.equal(f.calls.length, 0);
  assert.equal(f.service.blocksGameplayInput, true);
  f.service.markSpawnAvailable(generation); f.update();
  assert.equal(f.service.spawnReadiness(generation).ready, false);
  f.service.updateSelf(999, target(7));
  assert.equal(f.calls.filter(call => call[0] === 'apply').length, 1);
  f.service.updateSelf(7, target(2)); f.update();
  assert.equal(f.calls.filter(call => call[0] === 'apply').at(-1)[2], 2);
  f.settle(); f.update();
  assert.equal(f.service.spawnReadiness(generation).ready, true);
  assert.equal(f.service.blocksGameplayInput, false);
  f.service.cancelSpawn(generation); f.update();
  assert.equal(f.service.drained.ready, false);
  f.restore(); f.update();
  assert.equal(f.service.drained.ready, true);
  assert.equal(f.service.spawnReadiness(generation).ready, false);
});

test('disconnect and failed load attempts drain leases without starting new native targets', () => {
  const f = serviceFixture(), generation = f.service.beginSpawn(spawn(target()));
  f.service.markSpawnAvailable(generation); f.update();
  f.events.emit('preLoadGame'); f.update();
  assert.equal(f.calls.filter(call => call[0] === 'restore').length, 0);
  f.events.emit('postLoadGame'); f.update();
  assert.equal(f.calls.filter(call => call[0] === 'restore').length, 1);
  assert.equal(f.service.prepareDisposal(0x14), false);
  f.emitter.emit('connectionDisconnect', {}); f.restore(); f.update();
  assert.equal(f.service.drained.ready, true);
  assert.equal(f.service.blocksGameplayInput, false);
  assert.equal(f.service.prepareDisposal(0x14), true);
  assert.equal(f.calls.filter(call => call[0] === 'apply').length, 1);
});

test('unmanaged self baselines do not require the native API', () => {
  const f = serviceFixture(), generation = f.service.beginSpawn(spawn(undefined));
  f.service.markSpawnAvailable(generation); f.update();
  assert.equal(f.service.spawnReadiness(generation).ready, true);
  assert.equal(f.calls.length, 0);
});
