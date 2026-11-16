"use strict";
// Run only with a fresh disposable /state mount. No staging saves are reused.
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const state = '/state';
if (fs.existsSync(path.join(state, 'server-settings.json')) || fs.existsSync(path.join(state, 'world.sqlite3')))
  throw Error('Projection runtime smoke requires a fresh state directory');
const masters = ['Skyrim.esm', 'Update.esm', 'Dawnguard.esm', 'HearthFires.esm', 'Dragonborn.esm'];
for (const master of masters) fs.accessSync(path.join('/game-data', master));
for (const dir of ['data/scripts', 'logs']) fs.mkdirSync(path.join(state, dir), { recursive: true });
for (const name of ['scam_native.node', 'dist_back'])
  fs.symlinkSync(path.join('/src/build/dist/server', name), path.join(state, name));
const position = [132597, -63725, 14067];
fs.writeFileSync(path.join(state, 'server-settings.json'), JSON.stringify({
  name: 'Boyhouse disposable projection runtime', port: 17777, maxPlayers: 4,
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
const server = globalThis.mp;
const actor = server.createActor(0, ${JSON.stringify(position)}, 72, 60, 2026111620);
server.setRaceMenuOpen(actor, false);
console.log('Projection runtime profile created', actor);
`);
const child = spawn(process.execPath, ['dist_back/skymp5-server.js'], { cwd: state, stdio: 'inherit' });
const timeout = setTimeout(() => child.kill('SIGTERM'), 300000);
for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => child.kill(signal));
child.on('exit', code => { clearTimeout(timeout); process.exit(code ?? 1); });
child.on('error', error => { console.error(error); clearTimeout(timeout); process.exit(1); });
