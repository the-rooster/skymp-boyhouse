"use strict";
const assert = require("node:assert/strict");
const path = require("node:path");
const fs = require("node:fs");
const os = require("node:os");
const dataDir = fs.mkdtempSync(path.join(os.tmpdir(), "boyhouse-smoke-"));
fs.mkdirSync(path.join(dataDir, "scripts"));
const { PartyInstances } = require("../../skymp5-server/ts/campaign/partyInstances.js");
const { ScampServer } = require(path.resolve("build/dist/server/scam_native.node"));
const server = new ScampServer(JSON.stringify({
  port: 17779, maxPlayers: 8, listenHost: "127.0.0.1", dataDir,
  loadOrder: ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"].map(name => `/game-data/${name}`),
  archives: ["/game-data/Skyrim - Misc.bsa"],
  npcEnabled: false, offlineMode: true, logLevel: "error",
}));
server._setSelf(server);
const ids = [0xff000a01, 0xff000a02, 0xff000a03];
for (const id of ids) server.createActor(id, [10000, 10000, 10000], 0, 0x3c);
const authority = new PartyInstances({
  exists: id => server.get(id, "type") === "MpActor",
  getInstance: id => server.get(id, "worldInstance"),
  prepareInstance: (id, area) => server.set(id, "campaignReferences", {
    instanceArea: area.id, encounterLevel: area.encounterLevel,
    // Include known authored references to exercise preparation on entry and
    // revocation even though these bot players are in an empty test location.
    references: [0x1a677, 0x8a986],
  }),
  setInstance: (id, key) => server.set(id, "worldInstance", key),
  disable: id => server.setEnabled(id, false),
  isInside: () => true,
}, [{ id: "fixture", worldOrCell: 0x3c, center: [10000, 10000, 10000], radius: 100, encounterLevel: 5 }]);
const [owner, guest, stranger] = ids;
const quest = 0x4e50d, key = "4e50d:Skyrim.esm";
const progress = id => server.get(id, "questJournal");
const stage = (id, index) => server.set(id, "questCommand", { quest, op: "setStage", index });
assert.throws(() => stage(owner, 10), /adapter is not enabled/);
for (const id of ids) server.set(id, "private.boyhouse.questAdapters", [key]);
stage(owner, 10); stage(guest, 5);
const guestPersonal = progress(guest).quests;
assert.throws(() => server.set(owner, "questJournal", {}), /not implemented/);
authority.enter(owner, owner, "fixture");
assert.throws(() => authority.enter(stranger, owner, "fixture"), /Party membership/);
authority.invite(owner, guest); authority.accept(guest, owner);
authority.enter(guest, owner, "fixture");
assert.equal(server.get(owner, "worldInstance"), server.get(guest, "worldInstance"));
assert.notEqual(server.get(stranger, "worldInstance"), server.get(guest, "worldInstance"));
assert.equal(progress(guest).campaignOwner, owner);
const allocate = (id, level = 5) => server.set(id, "campaignReferences", {
  references: [0x1a677, 0x8a986], encounterLevel: level,
});
assert.throws(() => allocate(guest), /owning private phase/);
assert.throws(() => allocate(owner, 1.5), /Invalid encounter level/);
allocate(owner);
const allocations = server.get(owner, "private.boyhouse.referenceAllocations");
const npcCopy = 0xff000000 + parseInt(allocations.copies["1a677:Skyrim.esm"], 16);
assert.equal(server.get(npcCopy, "type"), "MpActor");
assert.equal(server.get(npcCopy, "worldInstance"), server.get(owner, "worldInstance"));
const allocatedInventory = server.get(npcCopy, "inventory");
allocate(owner, 50);
assert.deepEqual(server.get(owner, "private.boyhouse.referenceAllocations"), allocations);
assert.deepEqual(server.get(npcCopy, "inventory"), allocatedInventory);
const allocationArea = {
  worldOrCell: server.getActorCellOrWorld(npcCopy),
  center: server.getActorPos(npcCopy), radius: 1,
};
server.set(owner, "campaignReferences", { area: allocationArea, encounterLevel: 5 });
assert.equal(server.get(owner, "private.boyhouse.referenceAllocations").copies["1a677:Skyrim.esm"],
  allocations.copies["1a677:Skyrim.esm"]);
assert.throws(() => server.set(owner, "campaignReferences", {
  area: { ...allocationArea, radius: -1 }, encounterLevel: 5,
}), /Invalid campaign allocation area/);
stage(guest, 20);
assert.equal(progress(owner).quests[key].stage, 20);
assert.deepEqual(progress(stranger).quests, {});
const beforeFragment = progress(owner);
assert.throws(() => server.set(guest, "questExecuteStage", { quest, stage: 30 }), /Method not found - 'AddToMap'/);
assert.deepEqual(progress(owner), beforeFragment);
assert.throws(() => server.set(guest, "questExecuteStage", { quest, stage: 100, expectedRevision: 0 }), /Stale quest revision/);
assert.throws(() => server.set(guest, "questExecuteStage", { quest, stage: 10.5 }), /Invalid quest stage/);
server.set(guest, "questExecuteStage", { quest, stage: 100, expectedRevision: beforeFragment.revision });
assert.equal(progress(owner).quests[key].stage, 100);
assert.equal(Object.hasOwn(progress(owner), "vm"), false);
const afterFragment = progress(owner);
server.set(guest, "questExecuteStage", { quest, stage: 100 });
assert.deepEqual(progress(owner), afterFragment);
server.set(owner, "private.boyhouse.questAdapters", [key, "2d512:Skyrim.esm"]);
server.set(guest, "questExecuteStage", { quest: 0x2d512, stage: 5 });
assert.equal(progress(owner).quests["2d512:Skyrim.esm"].objectives["5"].displayed, true);
const { QuestJournalFeed } = require("../../skymp5-server/ts/campaign/questJournalFeed.js");
const packets = [];
const feed = new QuestJournalFeed({ connected: () => true, actor: user => ids[user], read: progress,
  send: (user, packet) => packets.push({ user, packet }) });
feed.subscribe(1, { action: "subscribe", subscriptionId: "native-smoke", actor: stranger });
assert.equal(packets[0].packet.journal.campaignOwner, owner);
authority.kick(owner, guest);
feed.poll();
assert.equal(packets[1].packet.journal.campaignOwner, guest);
assert.deepEqual(packets[1].packet.journal.quests, guestPersonal);
assert.notEqual(server.get(owner, "worldInstance"), server.get(guest, "worldInstance"));
const guestAllocations = server.get(guest, "private.boyhouse.referenceAllocations");
assert.notEqual(guestAllocations.copies["1a677:Skyrim.esm"], allocations.copies["1a677:Skyrim.esm"]);
const guestNpc = 0xff000000 + parseInt(guestAllocations.copies["1a677:Skyrim.esm"], 16);
assert.equal(server.get(guestNpc, "worldInstance"), server.get(guest, "worldInstance"));
assert.throws(() => server.set(owner, "worldInstance", 42));
console.log("Native server + party + private allocation + journal/feed + original MQ102/DA04 fragments passed (not quest playthroughs)");
process.exit(0);
