"use strict";
const fs = require("node:fs");
const path = require("node:path");
const state = "/state";
const masters = ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"];
for (const master of masters) fs.accessSync(path.join("/game-data", master));
for (const dir of ["data/scripts", "logs"]) fs.mkdirSync(path.join(state, dir), { recursive: true });
for (const file of ["scam_native.node", "dist_back"]) {
  const source = path.join("/src/build/dist/server", file), dest = path.join(state, file);
  fs.accessSync(source);
  if (!fs.existsSync(dest)) fs.symlinkSync(source, dest);
}
function createOnce(file, content) {
  try { fs.writeFileSync(path.join(state, file), content, { flag: "wx", mode: 0o600 }); }
  catch (e) { if (e.code !== "EEXIST") throw e; }
}
createOnce("gamemode.js", '// Instance boundary test realm. No quest progression is enabled.\n');
createOnce("server-settings.json", JSON.stringify({
  name: "Boyhouse instance laboratory", port: 17777, maxPlayers: 4,
  listenHost: "0.0.0.0", uiListenHost: "0.0.0.0", offlineMode: true,
  master: "http://127.0.0.1", dataDir: "./data",
  loadOrder: masters.map(name => `/game-data/${name}`),
  npcEnabled: false, logLevel: "info", gamemodePath: "./gamemode.js",
  databaseDriver: "sqlite", databaseName: "./world.sqlite3",
  startPoints: [{ pos: [133857, -61130, 14662], worldOrCell: "0x3c", angleZ: 72 }],
  boyhouseInstances: { experimental: true, areas: [{
    id: "boundary_test", worldOrCell: 60, center: [133857, -61130, 14662], radius: 512, encounterLevel: 5
  }] }
}, null, 2) + "\n");
console.log("Staging initialized; existing settings and saves preserved. Ports publish only to host loopback.");
