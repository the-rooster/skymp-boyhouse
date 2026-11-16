const { test } = require("node:test");
const assert = require("node:assert/strict");
const { ProjectionFeed } = require("../../skymp5-server/ts/campaign/projectionFeed.js");
const { WorldProjectionReplica } = require("../../skymp5-client/src/campaign/worldProjection.ts");

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
  const connected = new Set([0, 1]), blocked = new Map(), packets = [], closed = [];
  const adapter = {
    connected: user => connected.has(user),
    actor: user => data.get(user)?.actor ?? 0,
    instance: id => [...data.values()].find(m => m.actor === id).instance,
    read: id => { ++reads; return structuredClone([...data.values()].find(m => m.actor === id)); },
    block: (user, value) => blocked.set(user, value),
    send: (user, packet) => packets.push({ user, packet: structuredClone(packet) }),
    close: (user, error) => { closed.push({ user, error }); connected.delete(user); },
  };
  const feed = new ProjectionFeed(adapter, () => now, () => (++nonce).toString(16).padStart(32, "0"));
  const replica = new WorldProjectionReplica();
  feed.connect(0); replica.begin("session");
  const deliver = () => {
    while (packets.length) {
      const { user, packet } = packets.shift();
      if (user === 0) assert.equal(replica.accept(packet), true, JSON.stringify(packet));
    }
  };
  const hello = () => feed.packet(0, { action: "hello", sessionId: "session" });
  return { feed, replica, data, adapter, blocked, packets, closed, connected, deliver, hello,
    advance: ms => now += ms, reads: () => reads };
}

test("4096 hidden and visible originals arrive atomically in bounded chunks", () => {
  const f = fixture(4096); f.hello();
  assert.equal(f.packets.length, 33);
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
  f.feed.packet(1, { action: "hello", sessionId: "session" });
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
  f.feed.packet(0, { action: "hello", sessionId: "new" });
  f.feed.packet(0, oldAck);
  assert.equal(f.blocked.get(0), true);
  for (const packet of oldPackets) assert.equal(f.replica.accept(packet), false);
  f.deliver();
  f.feed.packet(0, f.replica.applied(f.replica.version)); f.deliver();
  assert.equal(f.replica.ready, true);
});

test("duplicate references and malformed chunks cannot partially replace a projection", () => {
  const f = fixture(129); f.hello();
  const [begin, first, last] = f.packets.map(p => p.packet);
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
