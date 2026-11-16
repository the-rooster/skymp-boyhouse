const { test } = require("node:test");
const assert = require("node:assert/strict");
const { QuestJournalReplica } = require("../../skymp5-client/src/campaign/questJournalReplica.ts");
const journal = (revision = 1, owner = 1, instance = "") => ({ schemaVersion: 1, revision,
  actor: 2, campaignOwner: owner, instance, contextEpoch: "0:0", quests: {
    "4e50d:Skyrim.esm": { stage: 10, stages: [10], started: true, running: true, completed: false,
      active: true, objectives: { 10: { displayed: true, completed: false, failed: false } } },
  } });
const packet = (sequence, state = journal(), subscriptionId = "s") => ({ customPacketType: "boyhouse.questJournal",
  subscriptionId, sequence, ok: true, journal: state });

test("late packets from a previous party or connection cannot restore old progress", () => {
  const replica = new QuestJournalReplica(); replica.begin("s");
  assert.equal(replica.accept(packet(1, journal(40, 1, "campaign:1:area"))), true);
  assert.equal(replica.accept(packet(3, journal(2, 2, "campaign:2:area"))), true);
  assert.equal(replica.accept(packet(2, journal(41, 1, "campaign:1:area"))), false);
  assert.equal(replica.snapshot().campaignOwner, 2);
  replica.begin("new");
  assert.equal(replica.snapshot(), undefined);
  assert.equal(replica.accept(packet(100)), false);
  assert.equal(replica.accept(packet(1, journal(), "new")), true);
  replica.clear();
  assert.equal(replica.accept(packet(2, journal(), "new")), false);
});

test("a stale revision cannot rewind a campaign but a changed context can start lower", () => {
  const replica = new QuestJournalReplica(); replica.begin("s");
  replica.accept(packet(1, journal(10)));
  assert.equal(replica.accept(packet(2, journal(9))), false);
  assert.equal(replica.accept(packet(2, { ...journal(1), contextEpoch: "2:2" })), true);
});

test("malformed snapshots are rejected atomically without consuming their sequence", () => {
  const replica = new QuestJournalReplica(); replica.begin("s");
  replica.accept(packet(1));
  const good = replica.snapshot();
  const malformed = [ { ...journal(), schemaVersion: 2 }, { ...journal(), revision: -1 },
    { ...journal(), quests: null }, { ...journal(), campaignOwner: 1.5 }, { ...journal(), contextEpoch: 123 } ];
  const badStage = journal(); badStage.quests["4e50d:Skyrim.esm"].stage = 20; malformed.push(badStage);
  const badObjective = journal(); Object.assign(badObjective.quests["4e50d:Skyrim.esm"].objectives[10], { completed: true, failed: true }); malformed.push(badObjective);
  for (const state of malformed) {
    assert.equal(replica.accept(packet(2, state)), false);
    assert.deepEqual(replica.snapshot(), good);
  }
  assert.equal(replica.accept(packet(2, journal(2))), true);
});

test("unavailable campaigns clear visible progress and snapshots cannot mutate the replica", () => {
  const replica = new QuestJournalReplica(); replica.begin("s");
  const source = packet(1); replica.accept(source);
  source.journal.quests = {}; replica.snapshot().quests = {};
  assert.equal(Object.keys(replica.snapshot().quests).length, 1);
  assert.equal(replica.accept({ customPacketType: "boyhouse.questJournal", subscriptionId: "s",
    sequence: 2, ok: false, error: "Owner context expired" }), true);
  assert.equal(replica.snapshot(), undefined);
  assert.equal(replica.error, "Owner context expired");
});
