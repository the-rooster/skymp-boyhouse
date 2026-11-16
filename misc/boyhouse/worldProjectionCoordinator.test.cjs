const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const { EventEmitter } = require('node:events');
const { ProjectionFeed } = require('../../skymp5-server/ts/campaign/projectionFeed.js');
const { WorldProjectionReplica } = require('../../skymp5-client/src/campaign/worldProjection.ts');
const { CampaignPacketChannel } = require('../../skymp5-client/src/campaign/campaignPacketChannel.ts');
const { NativeReferenceOperations } = require('../../skymp5-client/src/campaign/nativeReferenceOperations.ts');
const { OwnedReferenceLifetime } = require('../../skymp5-client/src/campaign/ownedReferenceLifetime.ts');
const { ReferenceSuppression } = require('../../skymp5-client/src/campaign/referenceSuppression.ts');
const { QuestJournalReplica } = require('../../skymp5-client/src/campaign/questJournalReplica.ts');
const root = path.resolve(__dirname, '../../skymp5-client');
const ts = require(require.resolve('typescript', { paths: [root, path.resolve(root, '../skymp5-server')] }));
function load(file, dependencies) {
  const source = ts.transpileModule(fs.readFileSync(path.join(root, 'src', file), 'utf8'), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
  }).outputText;
  const module = { exports: {} };
  vm.runInNewContext(source, { module, exports: module.exports, require: id => {
    assert.ok(id in dependencies, `Unexpected import ${id}`); return dependencies[id];
  } }, { filename: file });
  return module.exports;
}
const { WorldProjectionCoordinator } = load('campaign/worldProjectionCoordinator.ts', {
  './worldProjection': { WorldProjectionReplica },
});
const actor = 0xff000001;
const manifest = () => ({ schemaVersion: 1, actor, campaignOwner: actor, instance: '', contextEpoch: '1:1', identity: '', references: [] });
const baseline = epoch => ({ schemaVersion: 1, actor, packetEpoch: epoch, messages: [
  { t: 32, eventSources: [], updateOwnerFunctions: [], updateNeighborFunctions: [] },
  { t: 33, idx: 1, refrId: actor, isMe: true, transform: { pos: [0, 0, 0], rot: [0, 0, 0], worldOrCell: 60 },
    props: { inventory: { entries: [] }, isRaceMenuOpen: false }, customPropsJsonDumps: [] },
] });

function bindingFixture(enabled = true, initiallyConnected = false) {
  const emitter = new EventEmitter(), events = new EventEmitter(), channel = new CampaignPacketChannel();
  const packets = [], sent = [], calls = [], storage = {};
  let connected = initiallyConnected, nonce = 0, epoch = '', data = manifest(), engineError;
  const feed = new ProjectionFeed({ connected: () => connected, actor: () => actor,
    instance: () => data.instance, read: () => structuredClone(data), block() {},
    beginEpoch: () => (epoch = (++nonce).toString(16).padStart(32, '0')), epoch: () => epoch,
    baseline: (_user, token) => baseline(token), send: (_user, packet) => packets.push(packet),
    close() { assert.fail('Unexpected server close'); },
  });
  class NetworkingService {}
  const network = {
    get packetEpochRequired() { return channel.enabled; },
    requirePacketEpoch: () => channel.requireEpoch(), selectPacketEpoch: value => channel.select(value),
    drainPacketEpoch: value => channel.drain(value, () => {}), releasePacketEpoch: value => channel.release(value, () => {}),
    isConnected: () => connected,
    close() { calls.push('close'); connected = false; emitter.emit('connectionDisconnect', {}); },
    reconnect() { calls.push('reconnect'); },
  };
  const controller = { emitter, on: (...args) => events.on(...args), once: (...args) => events.once(...args),
    lookupListener: type => { assert.equal(type, NetworkingService); return network; } };
  const engine = { cancel() {}, disconnect() {}, apply() { calls.push('apply'); }, release() { calls.push('release'); },
    drained: () => ({ ready: true }), observe: () => ({ ready: !engineError, error: engineError }) };
  emitter.on('sendMessage', e => {
    const packet = channel.outgoing(e.message);
    if (!packet) return;
    const content = JSON.parse((packet.t === 34 ? packet.message : packet).contentJsonDump);
    sent.push(content);
    if (content.customPacketType === 'boyhouse.projection') feed.packet(0, content);
  });
  const dependencies = {
    '../../campaign/worldProjectionCoordinator': { WorldProjectionCoordinator },
    '../../campaign/questJournalReplica': { QuestJournalReplica },
    '../../messages': { MsgType: { CustomPacket: 1 } }, '../../logging': { logError() {} },
    './clientListener': { ClientListener: class {} }, './networkingService': { NetworkingService },
  };
  const { CampaignProjectionService } = load('services/services/campaignProjectionService.ts', dependencies);
  const { CampaignQuestService } = load('services/services/campaignQuestService.ts', dependencies);
  const sp = { storage, settings: { 'skymp5-client': { 'campaign-projection': enabled } } };
  const projection = new CampaignProjectionService(sp, controller, engine);
  const journal = new CampaignQuestService(sp, controller);
  function deliver() {
    while (packets.length) emitter.emit('customPacketMessage', { message: { contentJsonDump: JSON.stringify(packets.shift()) } });
  }
  return { emitter, events, sent, calls, channel, projection, journal, storage,
    connect() { connected = true; feed.connect(0); emitter.emit('connectionAccepted', {}); },
    deliver, error(value) { engineError = value; },
    release() { deliver(); events.emit('tick'); events.emit('update'); deliver(); events.emit('update'); },
    replace() { feed.pauseActor(actor); data = { ...data, contextEpoch: '1:2' }; feed.poll(); deliver(); },
  };
}

test('service bindings subscribe the journal only after real coordinator release and replace its subscription per phase', () => {
  const f = bindingFixture(); f.events.emit('tick'); f.connect();
  assert.equal(f.sent.filter(p => p.customPacketType === 'boyhouse.questJournal').length, 0);
  f.deliver(); f.events.emit('tick'); f.events.emit('update');
  assert.equal(f.projection.coordinator.status.phase, 'awaiting-ready');
  assert.equal(f.sent.filter(p => p.customPacketType === 'boyhouse.questJournal').length, 0);
  f.deliver(); f.events.emit('update');
  const first = f.sent.at(-1); assert.equal(first.action, 'subscribe');
  const packet = { customPacketType: 'boyhouse.questJournal', subscriptionId: first.subscriptionId,
    sequence: 1, ok: true, journal: { schemaVersion: 1, revision: 1, actor, campaignOwner: actor,
      instance: '', contextEpoch: '1:1', quests: {} } };
  f.emitter.emit('customPacketMessage', { message: { contentJsonDump: JSON.stringify(packet) } });
  assert.ok(f.journal.replica.snapshot());
  f.replace(); assert.equal(f.journal.replica.snapshot(), undefined);
  f.emitter.emit('customPacketMessage', { message: { contentJsonDump: JSON.stringify(packet) } });
  assert.equal(f.journal.replica.snapshot(), undefined);
  f.release(); const second = f.sent.at(-1);
  assert.equal(second.action, 'subscribe'); assert.notEqual(second.subscriptionId, first.subscriptionId);
  assert.equal(f.storage.boyhouseProjectionCoordinator, f.projection.coordinator);
  assert.equal(f.calls.filter(c => c === 'release').length, 2);
});

test('a deferred projection failure cannot close a newer connection', () => {
  const f = bindingFixture(); f.events.emit('tick'); f.connect(); f.deliver(); f.events.emit('tick');
  f.error('Native application failed'); f.events.emit('update');
  assert.equal(f.projection.coordinator.status.phase, 'failed');
  assert.equal(f.calls.includes('close'), false, 'Do not destroy native client from a packet callback');
  f.emitter.emit('connectionDisconnect', {}); f.error(undefined); f.connect();
  f.events.emit('tick'); assert.equal(f.calls.includes('close'), false);
  f.release(); assert.equal(f.projection.coordinator.status.phase, 'ready');
});

test('disabled projection preserves legacy login and hot reload reconnects an enabled existing connection', () => {
  const disabled = bindingFixture(false); disabled.events.emit('tick'); disabled.connect();
  assert.equal(disabled.sent.length, 1); assert.equal(disabled.sent[0].action, 'subscribe');
  assert.equal(disabled.channel.enabled, false); assert.equal(disabled.calls.length, 0);
  const reload = bindingFixture(true, true); reload.events.emit('tick');
  assert.deepEqual(reload.calls, ['close', 'reconnect']);
});

function fixture() {
  const channel = new CampaignPacketChannel(), packets = [], sent = [], calls = [], closed = [], delivered = [];
  let now = 0, nonce = 0, epoch = '', blocked = true, connected = true, data = manifest();
  let drained = false, ready = false, error, onApply = () => {}, onObserve = () => {}, onDeliver = () => {};
  const feed = new ProjectionFeed({ connected: () => connected, actor: () => actor, instance: () => data.instance,
    read: () => structuredClone(data), block: (_user, value) => { blocked = value; },
    beginEpoch: () => { epoch = (++nonce).toString(16).padStart(32, '0'); return epoch; }, epoch: () => epoch,
    baseline: (_user, token) => baseline(token), send: (_user, packet) => packets.push(structuredClone(packet)),
    close: (_user, reason) => { connected = false; closed.push(String(reason)); },
  }, () => now);
  const deliver = message => { delivered.push(message); onDeliver(message); };
  const engine = {
    cancel() { calls.push('cancel'); ready = false; },
    drained: () => ({ ready: drained, error }),
    apply(projection, world) { calls.push(['apply', projection, world]); onApply(); },
    observe() { onObserve(); return { ready, error }; },
    release() { calls.push('release'); }, disconnect() { calls.push('disconnect'); },
  };
  const coordinator = new WorldProjectionCoordinator(engine, {
    block: token => token ? channel.select(token) : channel.requireEpoch(),
    send: message => { sent.push(message); feed.packet(0, message); },
    drain: token => channel.drain(token, deliver), release: token => channel.release(token, deliver),
    close: reason => closed.push(reason),
  }, () => now);
  feed.connect(0); coordinator.connect('one');
  return { coordinator, feed, channel, packets, sent, calls, closed, delivered, engine,
    get blocked() { return blocked; }, get epoch() { return epoch; },
    deliver() { while (packets.length) coordinator.receive(packets.shift()); },
    drained(value = true) { drained = value; }, ready(value = true) { ready = value; }, error(value) { error = value; },
    advance(ms) { now += ms; }, onApply(fn) { onApply = fn; }, onObserve(fn) { onObserve = fn; }, onDeliver(fn) { onDeliver = fn; },
    replace() { feed.pauseActor(actor); data = { ...data, contextEpoch: '1:2' }; feed.poll(); this.deliver(); },
    bootstrap() { this.deliver(); this.drained(); coordinator.advance(false); },
  };
}

test('a baseline waits for old work, then engine observation, then server release before input opens', () => {
  const f = fixture(); f.deliver(); f.coordinator.advance(false);
  assert.equal(f.coordinator.status.phase, 'draining'); assert.equal(f.calls.some(c => Array.isArray(c)), false);
  f.drained(); f.coordinator.advance(false); assert.equal(f.coordinator.status.phase, 'applying');
  assert.equal(f.calls.filter(c => Array.isArray(c)).length, 1);
  f.ready(); f.coordinator.advance(false); assert.equal(f.sent.length, 1, 'Tick cannot certify engine work');
  f.coordinator.advance(true); assert.equal(f.coordinator.status.phase, 'awaiting-ready');
  assert.equal(f.channel.ready, false); assert.equal(f.calls.includes('release'), false);
  f.deliver(); f.coordinator.advance(true);
  assert.equal(f.coordinator.status.phase, 'ready'); assert.equal(f.channel.ready, true);
  assert.equal(f.calls.filter(c => c === 'release').length, 1); assert.equal(f.blocked, false);
});

test('replication received during loading is applied and observed before gameplay release', () => {
  const f = fixture(); f.bootstrap();
  f.channel.incoming({ t: 34, epoch: f.epoch, message: { t: 33, idx: 5 } }, () => assert.fail());
  f.ready(); f.coordinator.advance(true); f.deliver();
  f.onDeliver(() => f.ready(false)); f.coordinator.advance(true);
  assert.equal(f.delivered.length, 1); assert.equal(f.coordinator.status.phase, 'releasing');
  assert.equal(f.channel.outgoing({ t: 2 }), undefined); assert.equal(f.calls.includes('release'), false);
  f.ready(); f.coordinator.advance(true); assert.equal(f.channel.ready, true);
});

test('superseding a locally applied transition discards its old acknowledgement and pending input', () => {
  const f = fixture(); f.bootstrap(); f.ready(); f.coordinator.advance(true);
  const old = f.sent.at(-1), oldReady = f.packets.shift();
  f.replace(); assert.equal(f.coordinator.receive(oldReady), false);
  f.coordinator.advance(false); f.ready(); f.coordinator.advance(true);
  assert.notEqual(f.sent.at(-1).token, old.token); assert.equal(f.calls.includes('release'), false);
  f.deliver(); f.coordinator.advance(true); assert.equal(f.coordinator.status.phase, 'ready');
});

test('an engine observation that triggers another transition cannot acknowledge its old result', () => {
  const f = fixture(); f.bootstrap(); f.ready();
  f.onObserve(() => { f.onObserve(() => {}); f.replace(); f.ready(); });
  f.coordinator.advance(true);
  assert.equal(f.sent.filter(packet => packet.action === 'ack').length, 0);
  assert.equal(f.channel.ready, false); assert.equal(f.coordinator.status.phase, 'draining');
});

test('a reentrant epoch replacement while draining replication cannot reopen the old world', () => {
  const f = fixture(); f.bootstrap();
  f.channel.incoming({ t: 34, epoch: f.epoch, message: { t: 2 } }, () => assert.fail());
  f.ready(); f.coordinator.advance(true); f.deliver(); f.onDeliver(() => f.replace());
  f.coordinator.advance(true);
  assert.equal(f.channel.ready, false); assert.equal(f.calls.includes('release'), false);
  assert.equal(f.coordinator.status.phase, 'draining');
});

for (const where of ['drain', 'apply', 'observe']) test(`${where} failure keeps input blocked and reports failure once`, () => {
  const f = fixture(); f.deliver();
  if (where === 'drain') f.error('old native call failed');
  else {
    f.drained();
    if (where === 'apply') f.onApply(() => { throw Error('baseline dispatch failed'); });
    f.coordinator.advance(false);
    if (where === 'observe') f.error('new native call failed');
  }
  f.coordinator.advance(true); f.coordinator.advance(true);
  assert.equal(f.coordinator.status.phase, 'failed'); assert.equal(f.channel.ready, false);
  assert.equal(f.sent.filter(packet => packet.action === 'failed').length, 1);
  assert.equal(f.calls.includes('release'), false); assert.ok(f.closed.length > 0);
});

test('partial transfers and repeated replacement manifests cannot extend the transition deadline', () => {
  const f = fixture(); f.coordinator.receive(f.packets.shift()); f.advance(29000); f.replace();
  f.advance(1001); f.coordinator.advance(false);
  assert.equal(f.coordinator.status.phase, 'failed'); assert.match(f.coordinator.status.error, /timed out/);
  assert.equal(f.channel.ready, false);
});

test('disconnect and a new session reject all previous control packets', () => {
  const f = fixture(), old = [...f.packets]; f.coordinator.disconnect();
  old.forEach(packet => assert.equal(f.coordinator.receive(packet), false));
  f.feed.connect(0); f.coordinator.connect('two');
  old.forEach(packet => assert.equal(f.coordinator.receive(packet), false));
  assert.equal(f.coordinator.status.phase, 'receiving'); assert.equal(f.channel.ready, false);
});

function engineFixture(options = {}) {
  const events = new EventEmitter(), emitter = new EventEmitter(), pending = [], errors = [], calls = [];
  const forms = new Map([[0x100, { handle: 10, base: 123, actor: true, disabled: false, blocked: false }],
    [0x14, { handle: 20, base: 7, actor: true, disabled: false, blocked: false }]]);
  const flags = options.flags || [true, true, true, true, false, true, true, false];
  let frame = 0, native, owned, suppression, engine, playerReady = false, spawn, viewsReady = false, nativeNoOp = false;
  let world = { forms: [], playerCharacterFormIdx: -1 }, originalReady = true;
  const reference = id => {
    const state = forms.get(id); if (!state) return null;
    const captured = frame, valid = () => assert.equal(frame, captured, 'Wrapper escaped native callback');
    const job = (name, effect) => { valid(); calls.push(name); return new Promise(resolve => pending.push({ name,
      finish() { effect(); resolve(); } })); };
    return { getFormID() { valid(); return id; }, getBaseObject() { valid(); return { getFormID: () => state.base }; },
      isDeleted() { valid(); return !!state.deleted; }, isDisabled() { valid(); return state.disabled; },
      isActivationBlocked() { valid(); return state.blocked; }, blockActivation(value) { valid(); state.blocked = value; },
      isInDialogueWithPlayer: () => false, getDialogueTarget: () => null, actor: state.actor,
      disable: () => job('disable', () => { state.disabled = true; }),
      enable: () => job('enable', () => { state.disabled = false; }),
      delete: () => job('delete', () => { state.deleted = true; }),
    };
  };
  const sp = { storage: options.storage || {}, ObjectReference: { from: value => value },
    Actor: { from: value => value?.actor ? value : null }, getReferenceHandle: id => forms.get(id)?.deleted ? 0 : forms.get(id)?.handle || 0,
    TESModPlatform: { setWeaponDrawnMode() {} }, Game: { getPlayer: () => reference(0x14), getFormEx: reference,
      disablePlayerControls(...mask) { if (!nativeNoOp) mask.slice(0, 8).forEach((change, i) => { if (change) flags[i] = false; }); },
      enablePlayerControls(...mask) { if (!nativeNoOp) mask.slice(0, 8).forEach((change, i) => { if (change) flags[i] = true; }); },
    } };
  ['Movement', 'Fighting', 'CamSwitch', 'Looking', 'Sneaking', 'Menu', 'Activate', 'Journal'].forEach((name, i) => {
    sp.Game[`is${name}ControlsEnabled`] = () => flags[i];
  });
  class OwnedToken {} class WorldCleanerService {} class WorldView {} class RemoteServer {} class PlayerSpawnService {} class NetworkingService {}
  const dependencies = {
    './clientListener': { ClientListener: class {} }, '../../logging': { logError: (...args) => errors.push(args) },
    '../../campaign/nativeReferenceOperations': { NativeReferenceOperations },
    '../../campaign/ownedReferenceLifetime': { OwnedReferenceLifetime },
    '../../campaign/referenceSuppression': { ReferenceSuppression }, './ownedReferenceService': { OwnedReferenceService: OwnedToken },
    './worldCleanerService': { WorldCleanerService }, '../../view/worldView': { WorldView },
    './remoteServer': { RemoteServer }, './playerSpawnService': { PlayerSpawnService }, './networkingService': { NetworkingService },
  };
  const { NativeReferenceOperationService } = load('services/services/nativeReferenceOperationService.ts', dependencies);
  dependencies['./nativeReferenceOperationService'] = { NativeReferenceOperationService };
  const { OwnedReferenceService } = load('services/services/ownedReferenceService.ts', dependencies);
  const { ReferenceSuppressionService } = load('services/services/referenceSuppressionService.ts', dependencies);
  dependencies['./referenceSuppressionService'] = { ReferenceSuppressionService };
  const { NativeWorldProjectionService } = load('services/services/nativeWorldProjectionService.ts', dependencies);
  const view = { allowed: true, aliases: [], setFormViewUpdateAllowed(value) { this.allowed = value; },
    setProjectionReferences(refs) { this.aliases = structuredClone(refs); },
    getSpawnReadiness: () => ({ ready: viewsReady, failed: [] }) };
  const remote = { resetWorldModel() { world = { forms: [], playerCharacterFormIdx: -1 }; view.aliases = []; emitter.emit('worldModelReset'); },
    getWorldModel: () => world, getOriginalApplicationProgress: () => ({ ready: originalReady }) };
  const network = { applyCampaignBaseline(value) {
    calls.push('baseline'); world = { forms: value.messages.filter(message => message.t === 33), playerCharacterFormIdx: 0 };
    spawn = { generation: (spawn?.generation || 0) + 1, phase: 'pending' }; return true;
  } };
  const controller = { emitter, on: (...args) => events.on(...args), once: (...args) => events.once(...args), lookupListener(type) {
    if (type === NativeReferenceOperationService) return native;
    if (type === OwnedToken) return owned;
    if (type === ReferenceSuppressionService) return suppression;
    if (type === WorldView) return view;
    if (type === RemoteServer) return remote;
    if (type === NetworkingService) return network;
    if (type === PlayerSpawnService) return { get status() { return spawn; }, isReady: generation => playerReady && spawn?.generation === generation };
    if (type === WorldCleanerService) return { modWcProtection() {}, setProjectionMode(value) { calls.push(['cleaner', value]); } };
    throw Error('Unexpected service');
  } };
  // Match production ordering: engine adapter's native update precedes queues.
  engine = new NativeWorldProjectionService(sp, controller);
  native = new NativeReferenceOperationService(sp, controller); owned = new OwnedReferenceService(sp, controller);
  suppression = new ReferenceSuppressionService(sp, controller);
  const projection = { ...manifest(), instance: `campaign:${actor}:room`, identity: 'a'.repeat(32),
    references: [{ original: 0x100, copy: 0xff001000, baseId: 123, kind: 'actor', disabled: false, deleted: false }] };
  return { engine, native, owned, suppression, view, projection, pending, forms, flags, errors, calls, sp,
    update() { ++frame; events.emit('update'); },
    async drain(limit = 24) { for (let i = 0; i < limit; ++i) {
      if (pending.length) pending.shift().finish(); await Promise.resolve(); await Promise.resolve(); this.update();
    } },
    begin() { engine.cancel(); engine.apply(projection, baseline('a'.repeat(32))); },
    playerReady(value = true) { playerReady = value; }, viewsReady(value = true) { viewsReady = value; },
    originalsReady(value) { originalReady = value; }, noOpControls(value) { nativeNoOp = value; },
  };
}

test('native engine waits for self, suppression, original work, and copies while preserving existing control restrictions', async () => {
  const f = engineFixture(), before = [...f.flags]; f.begin(); f.update();
  assert.equal(f.engine.observe().ready, false); assert.equal(f.forms.get(0x100).disabled, false);
  assert.deepEqual(f.flags, [false, false, true, true, false, false, false, false]);
  f.playerReady(); await f.drain();
  assert.equal(f.forms.get(0x100).disabled, true); assert.equal(f.forms.get(0x100).blocked, true);
  assert.equal(f.view.aliases[0].copy, 0xff001000); assert.equal(f.view.allowed, true);
  assert.equal(f.engine.observe().ready, false); f.viewsReady(); f.originalsReady(false);
  assert.equal(f.engine.observe().ready, false); f.originalsReady(true); assert.equal(f.engine.observe().ready, true);
  f.engine.release(); assert.deepEqual(f.flags, before); assert.equal(f.errors.length, 0);
});

test('cancelling while native suppression is pending drains disable and restoration before another baseline', async () => {
  const f = engineFixture(); f.begin(); f.playerReady(); f.update(); f.update();
  assert.equal(f.pending[0].name, 'disable'); f.engine.cancel();
  assert.equal(f.engine.drained().ready, false); assert.throws(() => f.engine.apply(f.projection, baseline('b'.repeat(32))), /not drained/);
  await f.drain(); assert.equal(f.forms.get(0x100).disabled, false); assert.equal(f.forms.get(0x100).blocked, false);
  assert.equal(f.engine.drained().ready, true); assert.deepEqual(f.view.aliases, []);
});

test('projection content mismatch and control no-ops fail instead of acknowledging engine application', async () => {
  const f = engineFixture(); f.begin(); f.forms.get(0x100).base = 999; f.playerReady(); f.update();
  assert.match(f.engine.observe().error, /installed content/); assert.equal(f.view.allowed, false);
  f.engine.disconnect(); await f.drain(); assert.equal(f.flags[0], true);
  const g = engineFixture(); g.begin(); g.noOpControls(true); g.update();
  assert.match(g.engine.observe().error, /controls did not become blocked/);
});

test('disabled/deleted copies cannot appear in the visible baseline', () => {
  const f = engineFixture(); f.engine.cancel(); f.projection.references[0].deleted = true;
  const world = baseline('a'.repeat(32)); world.messages.push({ t: 33, isMe: false, refrId: 0xff001000, baseId: 123 });
  assert.throws(() => f.engine.apply(f.projection, world), /conflicts/);
  assert.equal(f.calls.includes('baseline'), false);
});

test('ordinary authored NPC copies also hide and remap their originals', async () => {
  const f = engineFixture(); f.engine.cancel(); const world = baseline('a'.repeat(32));
  world.messages.push({ t: 33, isMe: false, refrId: 0x100000100, baseId: 123 });
  f.engine.apply(manifest(), world); f.playerReady(); await f.drain();
  assert.equal(f.forms.get(0x100).disabled, true); assert.equal(f.view.aliases[0].copy, 0x100000100);
  f.engine.disconnect(); await f.drain(); assert.equal(f.forms.get(0x100).disabled, false);
});

test('a reloaded engine adapter restores its retained control lease before doing other work', () => {
  const f = engineFixture({ flags: [false, false, true, true, false, false, false, false], storage: {
    boyhouseProjectionControls: { version: 1, enabled: [true, true, false, false, false, true, true, false] },
  } });
  f.update(); assert.deepEqual(f.flags, [true, true, true, true, false, true, true, false]);
});
