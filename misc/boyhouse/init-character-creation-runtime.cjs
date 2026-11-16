"use strict";
// Explicit fresh scratch state only. The regular login/spawn path must create
// the actor and request RaceSexMenu; this fixture never supplies an appearance.
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
if (process.argv.length !== 4 || process.argv[2] !== '--fresh-state' || !path.isAbsolute(process.argv[3]))
  throw Error('Usage: node init-character-creation-runtime.cjs --fresh-state /absolute/empty/scratch');
const state = path.resolve(process.argv[3]);
if (state === path.parse(state).root || (fs.existsSync(state) && fs.readdirSync(state).length))
  throw Error('Character creation smoke requires an empty dedicated scratch directory');
const masters = ['Skyrim.esm', 'Update.esm', 'Dawnguard.esm', 'HearthFires.esm', 'Dragonborn.esm'];
for (const master of masters) fs.accessSync(path.join('/game-data', master));
for (const name of ['scam_native.node', 'dist_back']) fs.accessSync(path.join('/src/build/dist/server', name));
fs.mkdirSync(state, { recursive: true });
for (const dir of ['data/scripts', 'logs']) fs.mkdirSync(path.join(state, dir), { recursive: true });
for (const name of ['scam_native.node', 'dist_back'])
  fs.symlinkSync(path.join('/src/build/dist/server', name), path.join(state, name));
const position = [132597, -63725, 14067];
fs.writeFileSync(path.join(state, 'server-settings.json'), JSON.stringify({
  name: 'Boyhouse disposable character creation', port: 17777, maxPlayers: 4,
  listenHost: '0.0.0.0', uiListenHost: '0.0.0.0', offlineMode: true,
  master: 'http://127.0.0.1', dataDir: './data', npcEnabled: false, logLevel: 'info',
  gamemodePath: './gamemode.js', databaseDriver: 'sqlite', databaseName: './world.sqlite3',
  loadOrder: masters.map(name => `/game-data/${name}`),
  startPoints: [{ pos: position, worldOrCell: '0x3c', angleZ: 72 }],
  boyhouseInstances: { experimental: true, projection: true, areas: [{
    id: 'projection_test', worldOrCell: 60, center: position, radius: 256, encounterLevel: 5,
  }] },
}, null, 2));
fs.writeFileSync(path.join(state, 'gamemode.js'), `
// No precreated actor, menu override, fake appearance or test-only packet gate.
console.log('Fresh character creation fixture: login with offline profile 2026111650');
`);
fs.writeFileSync(path.join(state, 'creation-smoke-metadata.json'), JSON.stringify({
  profileId: 2026111650, expectedName: 'Boyhouse Creation Test', position, worldOrCell: 60,
  requiresFreshCharacter: true, requiresCampaignProjection: true,
}, null, 2));
const child = spawn(process.execPath, ['dist_back/skymp5-server.js'], { cwd: state, stdio: 'inherit' });
const timeout = setTimeout(() => child.kill('SIGTERM'), 300000);
for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => child.kill(signal));
child.on('exit', code => { clearTimeout(timeout); process.exit(code ?? 1); });
child.on('error', error => { console.error(error); clearTimeout(timeout); process.exit(1); });
