const { test } = require("node:test");
const assert = require("node:assert/strict");
const { ProjectionFeed } = require("../../skymp5-server/ts/campaign/projectionFeed.js");
const { WorldProjectionReplica } = require("../../skymp5-client/src/campaign/worldProjection.ts");
const { CampaignPacketChannel } = require("../../skymp5-client/src/campaign/campaignPacketChannel.ts");

const actor = 0xff000001;
function manifest(count = 1, owner = actor) {
  return { schemaVersion: 1, actor, campaignOwner: owner, instance: `campaign:${owner}:area`,
    contextEpoch: "1:1", identity: "a".repeat(32), references: Array.from({ length: count }, (_, index) => ({
      original: index + 0x100, copy: index + 0xff010000, baseId: 0x1000,
      kind: index % 2 ? "actor" : "object", disabled: index % 3 === 0, deleted: index % 7 === 0,
    })) };
}

function fixture(count = 1) {
  let now = 0, nonce = 0, reads = 0;
  const data = new Map([[0, manifest(count)], [1, { ...manifest(count, actor + 1), actor: actor + 1 }]]);
  const connected = new Set([0, 1]), blocked = new Map(), packets = [], closed = [], epochs = new Map();
  const adapter = {
    connected: user => connected.has(user),
    actor: user => data.get(user)?.actor ?? 0,
    instance: id => [...data.values()].find(m => m.actor === id).instance,
    read: id => { ++reads; return structuredClone([...data.values()].find(m => m.actor === id)); },
    block: (user, value) => blocked.set(user, value),
    beginEpoch: user => {
      assert.equal(blocked.get(user), true);
      const epoch = (++nonce).toString(16).padStart(32, "0"); epochs.set(user, epoch); return epoch;
    },
    epoch: user => epochs.get(user),
    baseline: (user, packetEpoch) => ({ schemaVersion: 1, actor: data.get(user).actor, packetEpoch, messages: [
      { t: 32, eventSources: [], updateOwnerFunctions: [], updateNeighborFunctions: [] },
      { t: 33, idx: 1, refrId: data.get(user).actor, isMe: true,
        transform: { pos: [0, 0, 0], rot: [0, 0, 0], worldOrCell: 60 },
        props: { inventory: { entries: [] }, isRaceMenuOpen: false }, customPropsJsonDumps: [] },
    ] }),
    send: (user, packet) => packets.push({ user, packet: structuredClone(packet) }),
    close: (user, error) => { closed.push({ user, error }); connected.delete(user); },
  };
  const feed = new ProjectionFeed(adapter, () => now);
  const replica = new WorldProjectionReplica();
  feed.connect(0); replica.begin("session");
  const deliver = () => {
    while (packets.length) {
      const { user, packet } = packets.shift();
      if (user === 0) assert.equal(replica.accept(packet), true, JSON.stringify(packet));
    }
  };
  const hello = () => feed.packet(0, { action: "hello", protocolVersion: 2, sessionId: "session" });
  return { feed, replica, data, adapter, blocked, packets, closed, connected, deliver, hello,
    advance: ms => now += ms, reads: () => reads };
}

test("4096 hidden and visible originals arrive atomically in bounded chunks", () => {
  const f = fixture(4096); f.hello();
  assert.equal(f.packets.length, 34);
  assert.ok(f.packets.every(p => JSON.stringify(p.packet).length < 32768));
  const begin = f.packets.shift().packet;
  assert.equal(f.replica.accept(begin), true);
  assert.equal(f.replica.prepared(), undefined);
  assert.equal(f.replica.snapshot(), undefined);
  assert.equal(f.blocked.get(0), true);
  f.packets.reverse(); f.deliver();
  assert.deepEqual(f.replica.prepared(), f.data.get(0));
  assert.equal(f.replica.ready, false);
  const ack = f.replica.applied(f.replica.version);
  assert.equal(f.replica.ready, false, "Applying locally must still wait for the server release");
  f.feed.packet(0, ack); f.deliver();
  assert.equal(f.replica.ready, true);
  assert.equal(f.blocked.get(0), false);
  assert.deepEqual(f.replica.snapshot(), f.data.get(0));
});

test("receipt and an unsolicited ready message never acknowledge engine work", () => {
  const f = fixture(); f.hello();
  const begin = f.packets[0].packet; f.deliver();
  assert.equal(f.replica.accept({ ...begin, kind: "ready" }), false);
  assert.equal(f.blocked.get(0), true);
  assert.equal(f.replica.ready, false);
  f.advance(30000); f.feed.poll();
  assert.equal(f.closed.length, 1);
  assert.equal(f.blocked.get(0), true);
});

test("late engine completion cannot acknowledge a superseding transition", () => {
  const f = fixture(); f.hello(); f.deliver();
  const oldVersion = f.replica.version;
  f.feed.pauseActor(actor);
  f.data.set(0, { ...manifest(2, actor + 1), contextEpoch: "2:8" });
  f.feed.poll(); f.deliver();
  assert.equal(f.replica.applied(oldVersion), undefined);
  assert.equal(f.blocked.get(0), true);
  assert.equal(f.replica.prepared().campaignOwner, actor + 1);
  f.feed.packet(0, f.replica.applied(f.replica.version)); f.deliver();
  assert.equal(f.replica.ready, true);
});

test("acknowledgements are tied to the connection, token, sequence and actor assignment", () => {
  const f = fixture(); f.hello(); f.deliver();
  const ack = f.replica.applied(f.replica.version);
  f.feed.connect(1);
  f.feed.packet(1, { action: "hello", protocolVersion: 2, sessionId: "session" });
  f.feed.packet(1, { ...ack, actor: actor + 1 });
  assert.equal(f.blocked.get(1), true);
  for (const replacement of [{ token: "f".repeat(32) }, { sequence: 999 }, { sessionId: "old" }])
    f.feed.packet(0, { ...ack, ...replacement });
  assert.equal(f.blocked.get(0), true);
  f.data.get(0).actor = actor + 2;
  f.feed.packet(0, ack);
  assert.equal(f.blocked.get(0), true);
});

test("a state change while applying causes a replacement manifest, never early release", () => {
  const f = fixture(); f.hello(); f.deliver();
  const old = f.replica.applied(f.replica.version);
  f.data.get(0).references[0].disabled = false;
  f.feed.packet(0, old); f.deliver();
  assert.equal(f.blocked.get(0), true);
  assert.equal(f.replica.ready, false);
  assert.equal(f.replica.prepared().references[0].disabled, false);
  f.feed.packet(0, old);
  assert.equal(f.blocked.get(0), true);
  f.feed.packet(0, f.replica.applied(f.replica.version)); f.deliver();
  assert.equal(f.replica.ready, true);
});

test("idle sessions do not repeatedly parse whole campaign ledgers", () => {
  const f = fixture(); f.hello(); f.deliver();
  f.feed.packet(0, f.replica.applied(f.replica.version)); f.deliver();
  assert.equal(f.reads(), 2);
  for (let i = 0; i < 1000; ++i) { f.advance(250); f.feed.poll(); }
  assert.equal(f.reads(), 2);
  assert.equal(f.packets.length, 0);
  assert.equal(f.closed.length, 0);
});

test("visibility commits gate every participant and publish only the committed view", () => {
  const f = fixture(2);
  const guest = new WorldProjectionReplica(); guest.begin("guest");
  f.data.get(1).instance = f.data.get(0).instance;
  f.data.get(1).campaignOwner = actor;
  const deliver = () => {
    while (f.packets.length) {
      const { user, packet } = f.packets.shift();
      assert.equal((user === 0 ? f.replica : guest).accept(packet), true);
    }
  };
  f.hello(); f.feed.connect(1);
  f.feed.packet(1, { action: "hello", protocolVersion: 2, sessionId: "guest" });
  deliver();
  const previous = [f.replica.applied(f.replica.version), guest.applied(guest.version)];
  for (const user of [0, 1]) f.feed.packet(user, previous[user]);
  deliver();
  const reads = f.reads();
  const send = f.adapter.send;
  f.adapter.send = (user, packet) => {
    send(user, packet);
    if (packet.kind === "pause") {
      // Reentry before the second pause must not release the second player.
      f.feed.packet(1, previous[1]);
      f.feed.poll();
      assert.equal(f.blocked.get(1), true);
    }
  };
  assert.equal(f.feed.prepareVisibility([0, 1]), true);
  assert.equal(f.reads(), reads);
  assert.ok(f.packets.every(({ packet }) => packet.kind === "pause"));
  deliver();
  for (const data of f.data.values()) data.references[1].disabled = true;
  assert.equal(f.feed.completeVisibility([0, 1], true), true);
  deliver();
  assert.equal(f.replica.prepared().references[1].disabled, true);
  assert.equal(guest.prepared().references[1].disabled, true);
  for (const user of [0, 1]) {
    f.feed.packet(user, previous[user]);
    assert.equal(f.blocked.get(user), true);
  }
  f.feed.packet(0, f.replica.applied(f.replica.version)); deliver();
  assert.equal(f.blocked.get(0), false);
  assert.equal(f.blocked.get(1), true);
  f.feed.packet(1, guest.applied(guest.version)); deliver();
  assert.equal(f.blocked.get(1), false);
});

test("visibility preparation refuses missing or duplicate participants before pausing", () => {
  for (const users of [[0, 0], [0, 1], [0, -1], [0, 0.5], [0, 0x100000000]]) {
    const f = fixture(); f.hello(); f.deliver();
    f.feed.packet(0, f.replica.applied(f.replica.version)); f.deliver();
    const epoch = f.adapter.epoch(0);
    assert.throws(() => f.feed.prepareVisibility(users));
    assert.equal(f.adapter.epoch(0), epoch);
    assert.equal(f.blocked.get(0), false);
    assert.equal(f.packets.length, 0);
  }
});

test("hello during a visibility hold waits for publication without extending timeout", () => {
  const f = fixture();
  f.feed.prepareVisibility([0]);
  f.advance(29000); f.hello(); f.feed.poll();
  assert.equal(f.packets.length, 0);
  assert.equal(f.reads(), 0);
  f.feed.completeVisibility([0], true); f.deliver();
  assert.ok(f.replica.prepared());
  f.advance(1001); f.feed.poll();
  assert.equal(f.closed.length, 1);
  assert.equal(f.blocked.get(0), true);
});

test("visibility abort or publication failure retains every participant's gate", () => {
  for (const mode of ["abort", "publish", "closeFailure"]) {
    const f = fixture(); f.hello(); f.feed.connect(1);
    f.feed.packet(1, { action: "hello", protocolVersion: 2, sessionId: "guest" });
    f.feed.prepareVisibility([0, 1]);
    if (mode === "publish") f.adapter.baseline = () => { throw Error("baseline failed"); };
    if (mode === "closeFailure") {
      const close = f.adapter.close;
      f.adapter.close = (user, error) => {
        if (user === 0) throw Error("native fallback must close this socket");
        close(user, error);
      };
    }
    if (mode === "abort") assert.equal(f.feed.completeVisibility([0, 1], false), true);
    else assert.throws(() => f.feed.completeVisibility([0, 1], mode !== "closeFailure"));
    for (const user of [0, 1]) assert.equal(f.blocked.get(user), true);
    assert.equal(f.feed.users.size, 0);
    assert.ok(f.closed.some(({ user }) => user === 1), "cleanup must continue after the first failure");
  }
});

test("stale visibility cleanup preserves a replacement connection in the same slot", () => {
  for (const synchronized of [false, true]) for (const differentActor of [false, true]) {
    const f = fixture(); f.hello(); f.feed.connect(1);
    f.feed.packet(1, { action: "hello", protocolVersion: 2, sessionId: "guest" });
    f.feed.prepareVisibility([0, 1]);
    f.feed.disconnect(1);
    if (differentActor) f.data.get(1).actor += 1;
    f.feed.connect(1);
    const replacement = f.feed.users.get(1), epoch = f.adapter.epoch(1);
    f.blocked.set(1, false);
    f.packets.length = 0;
    if (synchronized) assert.throws(() => f.feed.completeVisibility([0, 1], true));
    else assert.equal(f.feed.completeVisibility([0, 1], false), true);
    assert.deepEqual(f.closed.map(({ user }) => user), [0]);
    assert.equal(f.feed.users.get(1), replacement);
    assert.equal(f.connected.has(1), true);
    assert.equal(f.blocked.get(1), false);
    assert.equal(f.adapter.epoch(1), epoch);
    assert.equal(f.packets.length, 0);
    assert.equal(f.feed.visibilityParticipants.size, 0);

    // The replacement's own projection handshake still works after cleanup.
    const replica = new WorldProjectionReplica(); replica.begin("replacement");
    f.feed.packet(1, { action: "hello", protocolVersion: 2, sessionId: "replacement" });
    while (f.packets.length) assert.equal(replica.accept(f.packets.shift().packet), true);
    f.feed.packet(1, replica.applied(replica.version));
    while (f.packets.length) assert.equal(replica.accept(f.packets.shift().packet), true);
    assert.equal(replica.ready, true);
    assert.equal(f.blocked.get(1), false);
  }
});

test("visibility cleanup preserves a new actor assignment on the existing connection", () => {
  for (const synchronized of [false, true]) {
    const f = fixture(); f.hello(); f.feed.connect(1);
    f.feed.packet(1, { action: "hello", protocolVersion: 2, sessionId: "guest" });
    f.feed.prepareVisibility([0, 1]);
    const state = f.feed.users.get(1);
    f.data.get(1).actor += 1;
    f.packets.length = 0;
    if (synchronized) assert.throws(() => f.feed.completeVisibility([0, 1], true));
    else assert.equal(f.feed.completeVisibility([0, 1], false), true);
    assert.deepEqual(f.closed.map(({ user }) => user), [0]);
    assert.equal(f.feed.users.get(1), state);
    assert.equal(state.visibilityHeld, false);
    assert.equal(f.feed.visibilityParticipants.size, 0);
    f.feed.poll();
    assert.equal(state.pending.actor, f.data.get(1).actor);
    assert.equal(f.blocked.get(1), true, "The replacement actor still needs its own acknowledgement");
  }
});

test("visibility cleanup rechecks each captured participant after reentrant callbacks", () => {
  for (const phase of ["prepare", "publish", "close"]) {
    const f = fixture(); f.hello(); f.feed.connect(1);
    f.feed.packet(1, { action: "hello", protocolVersion: 2, sessionId: "guest" });
    let replacement;
    const replaceGuest = () => {
      f.feed.disconnect(1); f.feed.connect(1);
      replacement = f.feed.users.get(1);
      f.blocked.set(1, false);
    };
    if (phase === "prepare") {
      const send = f.adapter.send;
      f.adapter.send = (user, packet) => {
        if (user === 0 && packet.kind === "pause") {
          replaceGuest(); throw Error("Preparation failed after slot reuse");
        }
        send(user, packet);
      };
      assert.throws(() => f.feed.prepareVisibility([0, 1]));
      assert.equal(f.feed.completeVisibility([0, 1], false), true);
    } else {
      f.feed.prepareVisibility([0, 1]);
      if (phase === "publish") {
        f.adapter.baseline = () => {
          replaceGuest(); throw Error("Publication failed after slot reuse");
        };
        assert.throws(() => f.feed.completeVisibility([0, 1], true));
      } else {
        const close = f.adapter.close;
        f.adapter.close = (user, error) => {
          close(user, error);
          if (user === 0) replaceGuest();
        };
        assert.equal(f.feed.completeVisibility([0, 1], false), true);
      }
    }
    assert.deepEqual(f.closed.map(({ user }) => user), [0], phase);
    assert.equal(f.feed.users.get(1), replacement, phase);
    assert.equal(f.connected.has(1), true, phase);
    assert.equal(f.blocked.get(1), false, phase);
    assert.equal(f.feed.visibilityParticipants.size, 0, phase);
  }
});

test("a failed pause cannot close the replacement of the connection being paused", () => {
  const f = fixture(); f.hello();
  let replacement;
  f.adapter.send = (_user, packet) => {
    if (packet.kind === "pause") {
      f.feed.disconnect(0); f.feed.connect(0);
      replacement = f.feed.users.get(0);
      throw Error("Pause failed after its own slot was reused");
    }
  };
  assert.throws(() => f.feed.prepareVisibility([0]));
  assert.equal(f.feed.completeVisibility([0], false), true);
  assert.equal(f.feed.users.get(0), replacement);
  assert.equal(f.connected.has(0), true);
  assert.equal(f.closed.length, 0);
  assert.equal(f.feed.visibilityParticipants.size, 0);
});

test("repeated hello, invalid ack and changed manifests cannot keep a transition alive forever", () => {
  const f = fixture(); f.hello(); f.deliver();
  const ack = f.replica.applied(f.replica.version);
  f.advance(29000);
  f.hello();
  f.feed.packet(0, { ...ack, token: "invalid" });
  f.data.get(0).references[0].deleted = false;
  f.feed.packet(0, ack);
  f.advance(1001); f.feed.poll();
  assert.equal(f.closed.length, 1);
  assert.equal(f.blocked.get(0), true);
});

test("bad manifests, failed sends and engine failures keep gameplay blocked", () => {
  for (const mode of ["size", "transport", "engine", "reader"]) {
    const f = fixture(mode === "size" ? 4097 : 1);
    if (mode === "transport") f.adapter.send = () => { throw Error("transport failed"); };
    if (mode === "reader") f.adapter.read = () => { throw Error("missing allocation"); };
    f.hello();
    if (mode === "engine") {
      f.deliver();
      f.feed.packet(0, f.replica.failed(f.replica.version));
    }
    assert.equal(f.closed.length, 1, mode);
    assert.equal(f.blocked.get(0), true, mode);
  }
});

test("reused connections cannot restore old manifests or acknowledgement authority", () => {
  const f = fixture(); f.hello();
  const oldPackets = f.packets.map(p => p.packet); f.deliver();
  const oldAck = f.replica.applied(f.replica.version);
  f.feed.disconnect(0); f.feed.connect(0); f.replica.begin("new");
  f.feed.packet(0, { action: "hello", protocolVersion: 2, sessionId: "new" });
  f.feed.packet(0, oldAck);
  assert.equal(f.blocked.get(0), true);
  for (const packet of oldPackets) assert.equal(f.replica.accept(packet), false);
  f.deliver();
  f.feed.packet(0, f.replica.applied(f.replica.version)); f.deliver();
  assert.equal(f.replica.ready, true);
});

test("duplicate references and malformed chunks cannot partially replace a projection", () => {
  const f = fixture(129); f.hello();
  const [begin, first, last, baseline] = f.packets.map(p => p.packet);
  assert.equal(f.replica.accept(begin), true);
  assert.equal(f.replica.accept(first), true);
  for (const replacement of [
    { references: [{ ...last.references[0], original: first.references[0].original }] },
    { references: [{ ...last.references[0], copy: first.references[0].copy }] },
    { references: [{ ...last.references[0], copy: actor }] },
    { references: [{ ...last.references[0], disabled: "false" }] },
    { references: [{ ...last.references[0], extra: true }] },
    { references: [] }, { index: 99 }, { sequence: 100 }, { token: "0".repeat(32) },
  ]) assert.equal(f.replica.accept({ ...last, ...replacement }), false);
  assert.equal(f.replica.prepared(), undefined);
  assert.equal(f.replica.snapshot(), undefined);
  assert.equal(f.replica.accept(first), true, "An identical retransmission is harmless");
  assert.equal(f.replica.accept(last), true);
  assert.equal(f.replica.prepared(), undefined, "Reference bindings alone cannot acknowledge a baseline");
  assert.equal(f.replica.accept(baseline), true);
  const prepared = f.replica.prepared(); prepared.references.length = 0;
  assert.equal(f.replica.prepared().references.length, 129);
});

test("invalid metadata does not consume a future valid sequence", () => {
  const f = fixture(); f.hello();
  const packet = f.packets[0].packet;
  for (const metadata of [
    { ...packet.metadata, actor: 0x14 }, { ...packet.metadata, instance: "campaign:123:area" },
    { ...packet.metadata, identity: "bad" }, { ...packet.metadata, contextEpoch: "-1:1" },
    { ...packet.metadata, references: [] },
  ]) assert.equal(f.replica.accept({ ...packet, metadata }), false);
  assert.equal(f.replica.version, 0);
  assert.equal(f.replica.accept(packet), true);
});

test("empty shared-space manifests still require engine application and server release", () => {
  const f = fixture(0);
  f.data.set(0, { ...manifest(0), instance: "", identity: "" });
  f.hello(); f.deliver();
  assert.deepEqual(f.replica.prepared().references, []);
  assert.equal(f.replica.ready, false);
  f.feed.packet(0, f.replica.applied(f.replica.version)); f.deliver();
  assert.equal(f.replica.ready, true);
});

test("missing, malformed and cross-epoch baselines cannot release a projection", () => {
  const f = fixture(); f.hello();
  const [begin, refs, packet] = f.packets.map(p => p.packet);
  f.replica.accept(begin); f.replica.accept(refs);
  assert.equal(f.replica.prepared(), undefined);
  assert.equal(f.replica.applied(f.replica.version), undefined);
  assert.equal(f.replica.accept({ ...packet, token: 'f'.repeat(32) }), false);
  assert.equal(f.replica.accept({ ...packet, data: ' '.repeat(packet.data.length) }), false);
  assert.equal(f.replica.accept(packet), true);
  const baseline = f.replica.preparedBaseline();
  assert.equal(baseline.packetEpoch, f.adapter.epoch(0));
  baseline.messages.length = 0;
  assert.equal(f.replica.preparedBaseline().messages.length, 2);
});

test("baseline assembly rejects input, snippets, private state, duplicate identities and a different self", () => {
  for (const mutate of [
    b => b.messages.push({ t: 30 }), b => b.messages.push({ t: 1, contentJsonDump: '{}' }),
    b => b.messages[1].customPropsJsonDumps.push({ propName: 'private.boyhouse.questJournal', propValueJsonDump: '{}' }),
    b => b.messages.push({ ...b.messages[1], isMe: false }),
    b => b.messages[1].refrId++, b => b.messages[1].isMe = false,
    b => delete b.messages[1].props.inventory, b => b.packetEpoch = 'f'.repeat(32),
  ]) {
    const f = fixture(); const read = f.adapter.baseline;
    f.adapter.baseline = (...args) => { const baseline = read(...args); mutate(baseline); return baseline; };
    f.hello();
    if (f.closed.length) continue; // Native envelope mismatch is rejected server-side too.
    const packets = f.packets.map(p => p.packet);
    for (const packet of packets.slice(0, -1)) assert.equal(f.replica.accept(packet), true);
    assert.equal(f.replica.accept(packets.at(-1)), false);
    assert.equal(f.replica.applied(f.replica.version), undefined);
    assert.equal(f.blocked.get(0), true);
  }
});

test("native epoch rotation invalidates an otherwise matching acknowledgement", () => {
  const f = fixture(); f.hello(); f.deliver();
  const ack = f.replica.applied(f.replica.version), old = f.replica.packetEpoch;
  f.adapter.beginEpoch(0); f.feed.packet(0, ack); f.deliver();
  assert.notEqual(f.replica.packetEpoch, old);
  assert.equal(f.replica.ready, false); assert.equal(f.blocked.get(0), true);
});

test("large Unicode baselines assemble out of order with bounded chunks", () => {
  const f = fixture(), read = f.adapter.baseline;
  f.adapter.baseline = (...args) => {
    const b = read(...args);
    b.messages[1].customPropsJsonDumps.push({ propName: 'visible', propValueJsonDump: JSON.stringify('🐉'.repeat(20000)) });
    return b;
  };
  f.hello();
  const begin = f.packets.shift().packet;
  assert.ok(begin.baselineChunks > 2);
  assert.equal(f.replica.accept(begin), true);
  assert.ok(f.packets.every(p => Buffer.byteLength(JSON.stringify(p.packet)) < 64 * 1024));
  f.packets.reverse(); f.deliver();
  assert.equal(JSON.parse(f.replica.preparedBaseline().messages[1].customPropsJsonDumps[0].propValueJsonDump), '🐉'.repeat(20000));
});

test("oversized baseline data fails before any begin packet is published", () => {
  const f = fixture(), read = f.adapter.baseline;
  f.adapter.baseline = (...args) => { const b = read(...args); b.messages.push({ data: 'x'.repeat(2 * 1024 * 1024) }); return b; };
  f.hello();
  assert.equal(f.packets.length, 0);
  assert.equal(f.closed.length, 1);
  assert.equal(f.blocked.get(0), true);
});

test("manifest-only protocol clients cannot release the native gameplay gate", () => {
  const legacy = fixture();
  legacy.feed.packet(0, { action: "hello", sessionId: "session" });
  assert.equal(legacy.closed.length, 1); assert.equal(legacy.blocked.get(0), true);
  const f = fixture(); f.hello(); f.deliver();
  const ack = f.replica.applied(f.replica.version);
  f.feed.packet(0, { ...ack, protocolVersion: undefined });
  assert.equal(f.blocked.get(0), true);
  f.feed.packet(0, ack); f.deliver();
  assert.equal(f.replica.ready, true);
});

test("the baseline epoch selects replication without replaying gameplay from the previous phase", () => {
  const f = fixture(), channel = new CampaignPacketChannel(), delivered = [];
  channel.requireEpoch(); f.hello(); f.deliver();
  const old = f.replica.packetEpoch;
  channel.select(old);
  channel.incoming({ t: 34, epoch: old, message: { t: 2, idx: 1 } }, m => delivered.push(m));
  assert.equal(channel.outgoing({ t: 2 }), undefined);
  f.feed.pauseActor(actor); f.feed.poll(); f.deliver();
  const current = f.replica.packetEpoch; channel.select(current);
  channel.incoming({ t: 34, epoch: old, message: { t: 25, idx: 1 } }, m => delivered.push(m));
  channel.incoming({ t: 34, epoch: current, message: { t: 2, idx: 2 } }, m => delivered.push(m));
  assert.equal(channel.release(old, m => delivered.push(m)), false);
  f.feed.packet(0, f.replica.applied(f.replica.version)); f.deliver();
  assert.equal(channel.release(current, m => delivered.push(m)), true);
  assert.deepEqual(delivered, [{ t: 2, idx: 2 }]);
});
