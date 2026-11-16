const { test } = require("node:test");
const assert = require("node:assert/strict");
const { QuestJournalFeed } = require("../../skymp5-server/ts/campaign/questJournalFeed.js");

function fixture() {
  const sent = [], connected = new Set([7]), actors = new Map([[7, 101]]);
  let read = actor => ({ actor, campaignOwner: actor, revision: 0, quests: {} });
  const feed = new QuestJournalFeed({ connected: user => connected.has(user), actor: user => actors.get(user),
    read: actor => read(actor), send: (user, packet) => sent.push({ user, packet }) });
  return { feed, sent, connected, actors, read: fn => { read = fn; },
    subscribe: (extra = {}) => feed.subscribe(7, { action: "subscribe", subscriptionId: "session_a", ...extra }) };
}

test("journal subscriptions resolve the recipient's character, ignoring forged ownership", () => {
  const f = fixture();
  assert.equal(f.subscribe({ actor: 999, owner: 888 }), true);
  assert.equal(f.sent[0].user, 7);
  assert.equal(f.sent[0].packet.journal.campaignOwner, 101);
  assert.equal(f.feed.subscribe(7, { action: "setStage", quest: 123, index: 200 }), false);
  assert.equal(f.subscribe({ subscriptionId: "a".repeat(97) }), false);
});

test("unchanged journals are quiet; changes are ordered and subscription retries are idempotent", () => {
  const f = fixture();
  f.subscribe(); f.feed.poll(); f.subscribe();
  assert.equal(f.sent.length, 1);
  f.read(actor => ({ actor, campaignOwner: actor, revision: 1, quests: { example: {} } }));
  f.feed.poll();
  assert.equal(f.sent[1].packet.sequence, 2);
  assert.equal(f.sent[1].packet.journal.revision, 1);
});

test("login and owner departure produce explicit invalidation before a later snapshot", () => {
  const f = fixture();
  f.actors.clear(); f.subscribe();
  assert.equal(f.sent[0].packet.ok, false);
  f.actors.set(7, 101); f.feed.poll();
  assert.equal(f.sent[1].packet.ok, true);
  f.read(() => { throw new Error("owner gone"); }); f.feed.poll();
  assert.equal(f.sent[2].packet.ok, false);
  assert.equal(f.sent[2].packet.journal, undefined);
});

test("reusing a connection slot cannot carry another subscription across reconnect", () => {
  const f = fixture();
  f.subscribe(); f.connected.clear(); f.feed.poll();
  f.connected.add(7); f.actors.set(7, 202); f.feed.poll();
  assert.equal(f.sent.length, 1);
  f.subscribe({ subscriptionId: "session_b" });
  assert.equal(f.sent[1].packet.subscriptionId, "session_b");
  assert.equal(f.sent[1].packet.journal.actor, 202);
  assert.equal(f.sent[1].packet.sequence, 1);
  f.feed.disconnect(7); f.feed.poll();
  assert.equal(f.sent.length, 2);
});

test("oversized snapshots fail visibly and a failed send can retry", () => {
  const f = fixture();
  f.read(() => ({ oversized: "x".repeat(1024 * 1024) })); f.subscribe();
  assert.equal(f.sent[0].packet.ok, false);
  let attempts = 0;
  const feed = new QuestJournalFeed({ connected: () => true, actor: () => 1, read: () => ({}),
    send: (_user, packet) => { if (++attempts === 1) throw new Error("send failed"); assert.equal(packet.sequence, 1); } });
  assert.throws(() => feed.subscribe(7, { action: "subscribe", subscriptionId: "s" }));
  feed.poll();
  assert.equal(attempts, 2);
});
