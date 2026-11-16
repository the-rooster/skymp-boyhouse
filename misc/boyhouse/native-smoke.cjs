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
  npcEnabled: false, offlineMode: true, logLevel: "error",
}));
server._setSelf(server);
const ids = [0xff000a01, 0xff000a02, 0xff000a03];
for (const id of ids) server.createActor(id, [10000, 10000, 10000], 0, 0x3c);
const authority = new PartyInstances({
  exists: id => server.get(id, "type") === "MpActor",
  getInstance: id => server.get(id, "worldInstance"),
  setInstance: (id, key) => server.set(id, "worldInstance", key),
  disable: id => server.setEnabled(id, false),
  isInside: () => true,
}, [{ id: "fixture", worldOrCell: 0x3c, center: [10000, 10000, 10000], radius: 100 }]);
const [owner, guest, stranger] = ids;
authority.enter(owner, owner, "fixture");
assert.throws(() => authority.enter(stranger, owner, "fixture"), /Party membership/);
authority.invite(owner, guest); authority.accept(guest, owner);
authority.enter(guest, owner, "fixture");
assert.equal(server.get(owner, "worldInstance"), server.get(guest, "worldInstance"));
assert.notEqual(server.get(stranger, "worldInstance"), server.get(guest, "worldInstance"));
authority.kick(owner, guest);
assert.notEqual(server.get(owner, "worldInstance"), server.get(guest, "worldInstance"));
assert.throws(() => server.set(owner, "worldInstance", 42));
console.log("Native server + party authority smoke test passed (owned game masters loaded)");
process.exit(0);
