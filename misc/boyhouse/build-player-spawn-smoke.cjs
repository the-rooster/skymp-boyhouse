// Append the observer to the real client: separate SP plugins have separate storage.
// This requires a reachable, offline-mode test server and matching client settings.
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '../..');
const output = process.argv[2];
if (!output) throw Error('Usage: node misc/boyhouse/build-player-spawn-smoke.cjs <output.js>');
const client = fs.readFileSync(path.join(root, 'build/dist/client/Data/Platform/Plugins/skymp5-client.js'), 'utf8');
const observer = String.raw`
;(() => {
  const sp = skyrimPlatform;
  const log = (name, value) => sp.writeLogs('boyhousePlayerSpawnTest', name, JSON.stringify(value === undefined ? null : value));
  const started = Date.now();
  let stopping = false, nextSample = 0, passedSamples = 0, target;
  const finish = (passed, detail) => {
    if (stopping) return;
    stopping = true;
    log('test-end', { passed, elapsedMs: Date.now() - started, detail });
    sp.win32.exitProcess();
  };
  sp.on('consoleMessage', e => {
    if (/error|failed|Create actor|logged in|spawn observed ready/i.test(e.message)) log('console', e.message);
  });
  sp.on('loadGame', e => log('loadGame', e));
  sp.on('tick', () => {
    if (!stopping && Date.now() - started > 90000) finish(false, 'Spawn/menu readiness deadline expired');
  });
  sp.on('update', () => {
    if (stopping || Date.now() < nextSample) return;
    nextSample = Date.now() + 1000;
    try {
      const model = sp.storage.worldModel;
      const self = model && model.forms[model.playerCharacterFormIdx];
      if (!target && self && self.movement) target = JSON.parse(JSON.stringify(self.movement));
      const player = sp.Game.getPlayer();
      const loadout = sp.storage.playerLoadoutState;
      const open = sp.Ui.isMenuOpen('RaceSex Menu');
      const world = player && (player.getWorldSpace() || player.getParentCell());
      const position = player && [player.getPositionX(), player.getPositionY(), player.getPositionZ()];
      const distanceSquared = target && position && position.reduce((sum, n, i) => sum + (n - target.pos[i]) ** 2, 0);
      const worn = [];
      if (player) for (let bit = 0; bit < 32; ++bit) {
        const form = player.getWornForm((2 ** bit) >>> 0);
        if (form) worn.push({ slot: bit + 30, id: form.getFormID() });
      }
      const sample = {
        connected: sp.mpClientPlugin.isConnected(), target, position,
        world: world && world.getFormID(), loaded: player && player.is3DLoaded(),
        distanceSquared, open, ready: !!(loadout && loadout.ready),
        creation: loadout && loadout.creation, worn,
        left: player && player.getEquippedWeapon(true) && player.getEquippedWeapon(true).getFormID(),
        right: player && player.getEquippedWeapon(false) && player.getEquippedWeapon(false).getFormID(),
        lifetime: sp.storage.boyhousePlayerSpawnLifetime,
      };
      log('sample', sample);
      const passed = sample.connected && sample.ready && sample.loaded && target &&
        sample.world === target.worldOrCell && Number.isFinite(distanceSquared) && distanceSquared <= 128 ** 2 &&
        open && sample.creation === 'editing' && !worn.length && !sample.left && !sample.right;
      passedSamples = passed ? passedSamples + 1 : 0;
      if (passedSamples >= 5) finish(true, 'Five consecutive positioned/loaded/ready and unequipped character-menu samples');
    } catch (error) { finish(false, String(error.stack || error)); }
  });
  log('loaded', { platform: sp.getPlatformVersion() });
})();
`;
fs.writeFileSync(output, client + '\n' + observer);
console.log(output);
