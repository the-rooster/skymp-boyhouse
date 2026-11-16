const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { EventEmitter } = require('node:events');
const { CampaignPacketChannel } = require('../../skymp5-client/src/campaign/campaignPacketChannel.ts');
const a = 'a'.repeat(32), b = 'b'.repeat(32);
const move = { t: 2, idx: 7, data: { pos: [1, 2, 3] } };
const frame = (epoch, message = move) => ({ t: 34, epoch, message });
const custom = type => ({ t: 1, contentJsonDump: JSON.stringify({ customPacketType: type }) });

test('pending input is dropped, control survives, and a completed release tags all input', () => {
  const channel = new CampaignPacketChannel();
  assert.equal(channel.outgoing(move), move);
  channel.requireEpoch();
  assert.equal(channel.outgoing(move), undefined);
  assert.equal(channel.outgoing(custom('boyhouse.instance')), undefined);
  assert.equal(channel.outgoing(custom('boyhouse.projection')).t, 1);
  assert.equal(channel.outgoing(custom('loginWithSkympIo')).t, 1);
  channel.select(a);
  assert.equal(channel.release(b, () => {}), false);
  assert.equal(channel.release(a, () => {}), true);
  assert.deepEqual(channel.outgoing(move), frame(a));
  assert.deepEqual(channel.outgoingRaw(Uint8Array.from([134, 2, 5]).buffer), { t: 34, epoch: a, raw: [134, 2, 5] });
  assert.throws(() => channel.outgoing(frame(a)), /Invalid/);
  assert.throws(() => channel.outgoingRaw(Uint8Array.from([134, 34]).buffer), /Invalid/);
  channel.select(b);
  assert.equal(channel.outgoing(move), undefined);
});

test('only selected-epoch replication is buffered and old epochs stay rejected after release', () => {
  const channel = new CampaignPacketChannel(), seen = [];
  const deliver = m => seen.push(m);
  channel.incoming(frame(a), deliver);
  channel.select(a);
  channel.incoming(move, deliver);
  channel.incoming(frame(b), deliver);
  channel.incoming(frame(a, frame(a)), deliver);
  const mutable = frame(a, { t: 2, idx: 8 });
  channel.incoming(mutable, deliver);
  mutable.message.idx = 100;
  assert.deepEqual(seen, []);
  assert.equal(channel.release(a, deliver), true);
  assert.deepEqual(seen, [{ t: 2, idx: 8 }]);
  channel.select(b);
  channel.incoming(frame(a), deliver);
  channel.incoming(frame(b), deliver);
  channel.release(b, deliver);
  channel.incoming(frame(a), deliver);
  assert.deepEqual(seen, [{ t: 2, idx: 8 }, move]);
});

test('a new epoch during replay cancels the old batch and cannot open input', () => {
  const channel = new CampaignPacketChannel(), seen = [];
  channel.select(a);
  channel.incoming(frame(a, { t: 2, idx: 1 }), () => assert.fail());
  channel.incoming(frame(a, { t: 2, idx: 2 }), () => assert.fail());
  assert.equal(channel.release(a, message => {
    seen.push(message.idx);
    assert.equal(channel.outgoing(move), undefined);
    channel.select(b);
  }), false);
  assert.deepEqual(seen, [1]);
  assert.equal(channel.ready, false);
  assert.equal(channel.currentEpoch, b);
});

test('reentrant replay is refused and newly arriving packets require another observed drain', () => {
  const channel = new CampaignPacketChannel(), seen = [];
  channel.select(a);
  channel.incoming(frame(a, { t: 2, idx: 1 }), () => assert.fail());
  assert.equal(channel.release(a, message => {
    seen.push(message.idx);
    assert.equal(channel.release(a, () => assert.fail()), false);
    assert.equal(channel.outgoing(move), undefined);
    if (message.idx === 1) channel.incoming(frame(a, { t: 2, idx: 2 }), () => assert.fail());
  }), false);
  assert.deepEqual(seen, [1]);
  assert.equal(channel.ready, false);
  assert.equal(channel.drain(a, message => seen.push(message.idx)), true);
  assert.deepEqual(seen, [1, 2]);
  assert.equal(channel.ready, false);
  assert.equal(channel.release(a, () => assert.fail()), true);
  assert.equal(channel.ready, true);
});

test('replication can drain repeatedly while all gameplay input remains blocked', () => {
  const channel = new CampaignPacketChannel(), seen = [];
  channel.select(a); channel.incoming(frame(a, { t: 2, idx: 1 }), () => assert.fail());
  assert.equal(channel.drain(a, message => seen.push(message.idx)), true);
  assert.deepEqual(seen, [1]); assert.equal(channel.ready, false); assert.equal(channel.outgoing(move), undefined);
  channel.incoming(frame(a, { t: 2, idx: 2 }), () => assert.fail());
  assert.equal(channel.drain(a, message => seen.push(message.idx)), true);
  assert.deepEqual(seen, [1, 2]); assert.equal(channel.ready, false);
  assert.equal(channel.release(a, () => assert.fail()), true);
});

test('count overflow, byte overflow and application exceptions all leave input closed', () => {
  const channel = new CampaignPacketChannel();
  channel.select(a);
  for (let i = 0; i < 4096; ++i) channel.incoming(frame(a), () => assert.fail());
  assert.throws(() => channel.incoming(frame(a), () => assert.fail()), /limit/);
  assert.equal(channel.release(a, () => assert.fail()), false);
  assert.equal(channel.ready, false);
  channel.select(b);
  assert.throws(() => channel.incoming(frame(b, { t: 1, contentJsonDump: 'x'.repeat(3 * 1024 * 1024) }), () => assert.fail()), /limit/);
  assert.equal(channel.currentEpoch, '');
  channel.select(a);
  channel.incoming(frame(a), () => assert.fail());
  assert.throws(() => channel.release(a, () => { throw Error('application failed'); }), /application/);
  assert.equal(channel.ready, false);
  assert.equal(channel.currentEpoch, '');
});

test('only projection control bypasses the receive gate, and reconnect clears old queues', () => {
  const channel = new CampaignPacketChannel(), seen = [];
  const deliver = m => seen.push(m);
  channel.requireEpoch();
  channel.incoming(custom('loginWithSkympIo'), deliver);
  channel.incoming(custom('boyhouse.instance'), deliver);
  channel.incoming(custom('boyhouse.projection'), deliver);
  assert.equal(seen.length, 1);
  channel.select(a);
  channel.incoming(frame(a), deliver);
  channel.clear();
  channel.select(b);
  channel.release(b, deliver);
  assert.equal(seen.length, 1);
  assert.throws(() => channel.select('bad'), /Invalid/);
  assert.equal(channel.currentEpoch, b);
});

// Exercise the actual NetworkingService event wiring with a native API double.
// Engine work and native serialization remain covered separately.
function harness(version = '2.1.0') {
  const ts = require(require.resolve('typescript', { paths: [
    path.resolve(__dirname, '../../skymp5-client'), path.resolve(__dirname, '../../skymp5-server'),
  ] }));
  const root = path.resolve(__dirname, '../../skymp5-client/src');
  const load = (file, dependencies) => {
    const source = fs.readFileSync(path.join(root, file), 'utf8');
    const compiled = ts.transpileModule(source, { compilerOptions: {
      module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020,
    } }).outputText;
    const module = { exports: {} };
    vm.runInNewContext(compiled, { module, exports: module.exports, require: id => {
      if (!(id in dependencies)) throw Error(`Unmocked dependency ${id}`);
      return dependencies[id];
    }, Uint8Array, ArrayBuffer }, { filename: file });
    return module.exports;
  };
  const native = new EventEmitter(), emitter = new EventEmitter();
  const packets = [], sent = [], errors = [];
  let destroyed = 0, created = 0;
  const model = { forms: [{ idx: 42, refrId: 0xff000001 }], playerCharacterFormIdx: 0 };
  class RemoteServer {} class PlayerLoadoutService {}
  const loadout = { blocksGameplayInput: false, generation: 1, projectionReadiness: { ready: true } };
  const controller = { on: (...args) => native.on(...args), once: (...args) => native.once(...args), emitter,
    lookupListener: type => {
      if (type === PlayerLoadoutService) return loadout;
      assert.equal(type, RemoteServer); return { getWorldModel: () => model };
    } };
  const sp = { storage: {}, decodeUtf8: bytes => new TextDecoder().decode(bytes), mpClientPlugin: {
    getVersion: () => version, send: (message, reliable) => sent.push({ message: JSON.parse(message), reliable }),
    sendRaw: (bytes, length, reliable) => sent.push({ raw: Array.from(new Uint8Array(bytes)), length, reliable }),
    createClient: () => ++created, destroyClient: () => ++destroyed, isConnected: () => true,
    tick: callback => { for (const packet of packets.splice(0)) callback(...packet); },
  } };
  const { NetworkingService } = load('services/services/networkingService.ts', {
    '../../logging': { logTrace() {}, logError: (...args) => errors.push(args) },
    '../../lib/errors': { NeverError: Error }, '../../messages': load('messages.ts', {}),
    './clientListener': { ClientListener: class {} }, './remoteServer': { RemoteServer },
    './playerLoadoutService': { PlayerLoadoutService },
    '../../campaign/campaignPacketChannel': { CampaignPacketChannel },
  });
  const service = new NetworkingService(sp, controller);
  return { service, emitter, sent, errors, packets, native, loadout,
    get destroyed() { return destroyed; }, get created() { return created; },
    message: message => packets.push(['message', new TextEncoder().encode(JSON.stringify(message)).buffer, '']),
    tick: () => native.emit('tick'),
  };
}

test('NetworkingService gates every outgoing API and unwraps frames before ordinary dispatch', () => {
  const h = harness(), received = [];
  h.emitter.on('updateMovementMessage', event => received.push(event.message));
  h.service.selectPacketEpoch(a);
  h.emitter.emit('sendMessage', { message: move, reliability: 'unreliable' });
  h.emitter.emit('sendMessageWithRefrId', { message: { t: 2, _refrId: 0xff000001 }, reliability: 'unreliable' });
  h.emitter.emit('sendRawMessage', { rawMessage: Uint8Array.from([134, 2, 5]).buffer, reliability: 'unreliable' });
  assert.deepEqual(h.sent, []);
  h.message(frame(b)); h.message(move); h.message(frame(a)); h.tick();
  assert.deepEqual(received, []);
  assert.equal(h.service.releasePacketEpoch(a), true);
  assert.deepEqual(JSON.parse(JSON.stringify(received)), [move]);
  h.emitter.emit('sendMessage', { message: move, reliability: 'unreliable' });
  h.emitter.emit('sendMessageWithRefrId', { message: { t: 2, _refrId: 0xff000001 }, reliability: 'reliable' });
  h.emitter.emit('sendRawMessage', { rawMessage: Uint8Array.from([134, 2, 5]).buffer, reliability: 'unreliable' });
  assert.deepEqual(h.sent.map(x => x.message.t), [34, 34, 34]);
  assert.deepEqual(h.sent.map(x => x.reliable), [false, true, false]);
  assert.equal(h.sent[1].message.message.idx, 42);
  assert.deepEqual(h.sent[2].message.raw, [134, 2, 5]);
  assert.deepEqual(h.errors, []);
});

test('NetworkingService applies a selected baseline through normal dispatch and preserves input gating', () => {
  const h = harness(), seen = [];
  h.emitter.on('anyMessage', event => seen.push(event.message.t));
  const baseline = { schemaVersion: 1, actor: 0xff000001, packetEpoch: a,
    messages: [{ t: 32 }, { t: 33 }, { t: 26 }] };
  assert.equal(h.service.applyCampaignBaseline(baseline), false);
  h.service.selectPacketEpoch(a); assert.equal(h.service.applyCampaignBaseline(baseline), true);
  assert.deepEqual(seen, [32, 33, 26]);
  h.emitter.emit('sendMessage', { message: move, reliability: 'reliable' }); assert.equal(h.sent.length, 0);
  h.message(frame(a)); h.tick(); assert.equal(h.service.drainPacketEpoch(a), true);
  assert.deepEqual(seen, [32, 33, 26, 2]);
  h.emitter.emit('sendMessage', { message: move, reliability: 'reliable' }); assert.equal(h.sent.length, 0);
});

test('a reentrant epoch change interrupts baseline dispatch before later messages', () => {
  const h = harness(), seen = []; h.service.selectPacketEpoch(a);
  h.emitter.on('anyMessage', event => { seen.push(event.message.t); h.service.selectPacketEpoch(b); });
  assert.equal(h.service.applyCampaignBaseline({ schemaVersion: 1, actor: 0xff000001, packetEpoch: a,
    messages: [{ t: 32 }, { t: 33 }] }), false);
  assert.deepEqual(seen, [32]);
});

test('explicit closure notifies world cleanup after destroying transport without reconnecting', () => {
  const h = harness(); let notifications = 0;
  h.emitter.on('connectionDisconnect', () => { ++notifications; assert.equal(h.destroyed, 1); });
  h.service.close();
  assert.equal(notifications, 1); assert.equal(h.created, 0);
});

test('NetworkingService preserves legacy dispatch and requires the epoch-capable native codec', () => {
  const h = harness('2.0.0'), received = [];
  h.emitter.on('updateMovementMessage', e => received.push(e.message));
  h.message(move); h.tick();
  h.emitter.emit('sendMessage', { message: move, reliability: 'unreliable' });
  assert.equal(received.length, 1);
  assert.equal(h.sent[0].message.t, 2);
  assert.throws(() => h.service.requirePacketEpoch(), /2.1.0/);
  h.emitter.emit('sendMessage', { message: move, reliability: 'unreliable' });
  assert.equal(h.sent.length, 1); // A rejected codec cannot leave gameplay open.
});

test('NetworkingService defers failure cleanup and does not close a newer connection', () => {
  const h = harness();
  h.service.selectPacketEpoch(a);
  h.message(frame(a, { t: 1, contentJsonDump: 'x'.repeat(3 * 1024 * 1024) }));
  h.tick();
  assert.equal(h.destroyed, 0);
  assert.equal(h.errors.length, 1);
  h.service.connect('localhost', 17777);
  h.tick();
  assert.equal(h.destroyed, 0);
  assert.equal(h.created, 1);

  h.service.selectPacketEpoch(b);
  h.message(frame(b, { t: 1, contentJsonDump: 'x'.repeat(3 * 1024 * 1024) }));
  h.tick(); h.tick();
  assert.equal(h.destroyed, 1);
});

test('NetworkingService closes epoch-aware input when native JSON is malformed', () => {
  const h = harness();
  h.service.selectPacketEpoch(a);
  h.service.releasePacketEpoch(a);
  h.packets.push(['message', new TextEncoder().encode('{broken').buffer, '']);
  h.tick();
  assert.equal(h.errors.length, 1);
  h.emitter.emit('sendMessage', { message: move, reliability: 'unreliable' });
  assert.deepEqual(h.sent, []);
  assert.equal(h.destroyed, 0);
  h.tick();
  assert.equal(h.destroyed, 1);
});

test('creation blocks every gameplay output while permitting only the retained appearance and projection control', () => {
  const h = harness(); h.service.selectPacketEpoch(a); h.service.releasePacketEpoch(a);
  h.loadout.blocksGameplayInput = true;
  const pending = { generation: 1, actorIdx: 42, data: { name: 'Finished character' } };
  h.loadout.pendingAppearance = pending;
  for (const message of [move, { t: 4, idx: 43, data: pending.data },
    { t: 4, idx: 42, data: { ...pending.data } },
    { t: 1, contentJsonDump: JSON.stringify({ customPacketType: 'boyhouse.questJournal', action: 'subscribe' }) }])
    h.emitter.emit('sendMessage', { message, reliability: 'reliable' });
  h.emitter.emit('sendMessageWithRefrId', { message: { t: 2, _refrId: 0xff000001 }, reliability: 'unreliable' });
  h.emitter.emit('sendRawMessage', { rawMessage: Uint8Array.from([134, 2, 5]).buffer, reliability: 'unreliable' });
  assert.equal(h.sent.length, 0);
  h.emitter.emit('sendMessageWithRefrId', { message: { t: 4, data: pending.data }, reliability: 'reliable' });
  assert.equal(h.sent.length, 1); assert.equal(h.sent[0].message.message.idx, 42);
  h.emitter.emit('sendMessage', { message: { t: 1,
    contentJsonDump: JSON.stringify({ customPacketType: 'boyhouse.projection', action: 'hello' }) }, reliability: 'reliable' });
  assert.equal(h.sent[1].message.t, 1);
  h.service.selectPacketEpoch(b);
  h.emitter.emit('sendMessageWithRefrId', { message: { t: 4, data: pending.data }, reliability: 'reliable' });
  assert.equal(h.sent.length, 2, 'Even final appearance waits for the replacement epoch to release');
  h.service.releasePacketEpoch(b);
  h.emitter.emit('sendMessageWithRefrId', { message: { t: 4, data: pending.data }, reliability: 'reliable' });
  assert.equal(h.sent[2].message.epoch, b);
});

test('creation failure cleanup is deferred and cannot close a newer loadout', () => {
  for (const replaced of [false, true]) {
    const h = harness(); h.loadout.projectionReadiness = { ready: false, error: 'Creator failed' };
    h.emitter.emit('playerLoadoutFailed', { generation: 1, error: 'Creator failed' });
    assert.equal(h.destroyed, 0);
    if (replaced) ++h.loadout.generation;
    h.tick(); assert.equal(h.destroyed, replaced ? 0 : 1);
  }
});
