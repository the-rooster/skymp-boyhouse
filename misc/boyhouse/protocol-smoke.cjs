"use strict";
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
const root = path.resolve(__dirname, "../..");
const state = fs.mkdtempSync(path.join(os.tmpdir(), "boyhouse-protocol-"));
fs.mkdirSync(path.join(state, "data/scripts"), { recursive: true });
for (const file of ["scam_native.node", "dist_back"]) fs.symlinkSync(path.join(root, "build/dist/server", file), path.join(state, file));
fs.writeFileSync(path.join(state, "gamemode.js"), `require(${JSON.stringify(path.join(__dirname, "protocol-smoke-gamemode.cjs"))});`);
fs.writeFileSync(path.join(state, "server-settings.json"), JSON.stringify({
  name: "Boyhouse protocol smoke", port: 17781, maxPlayers: 4, listenHost: "127.0.0.1", uiListenHost: "127.0.0.1",
  offlineMode: true, master: "http://127.0.0.1", dataDir: "./data", npcEnabled: false, logLevel: "error",
  databaseDriver: "sqlite", databaseName: path.join(state, "world.sqlite3"),
  loadOrder: ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"].map(name => `/game-data/${name}`),
  startPoints: [{ pos: [133857, -61130, 14662], worldOrCell: "0x3c", angleZ: 72 }],
  boyhouseInstances: { experimental: true, areas: [{ id: "fixture", worldOrCell: 60, center: [133857, -61130, 14662], radius: 512, encounterLevel: 5 }] }
}));
const result = spawnSync(process.execPath, [path.join(state, "dist_back/skymp5-server.js")], { cwd: state, stdio: "inherit", timeout: 20000 });
if (result.error) console.error(result.error);
process.exit(result.status === 0 ? 0 : 1);
