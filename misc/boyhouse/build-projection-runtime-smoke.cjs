// Observe the actual client services in their own SP runtime (storage is per plugin).
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '../..');
const output = process.argv[2];
if (!output) throw Error('Usage: node misc/boyhouse/build-projection-runtime-smoke.cjs <output.js>');
const client = fs.readFileSync(path.join(root, 'build/dist/client/Data/Platform/Plugins/skymp5-client.js'), 'utf8');
const observer = String.raw`
;(() => {
  const sp = skyrimPlatform, started = Date.now();
  const log = (name, value) => sp.writeLogs('boyhouseProjectionRuntimeTest', name, JSON.stringify(value));
  let stopping = false, phase = 'login', nextSample = 0, waitUntil = 0;
  let original, controls, enteredVersion, copies = [], sampleCount = 0, leaveSteps = 0;
  const finish = (passed, detail) => {
    if (stopping) return;
    stopping = true;
    log('test-end', { passed, phase, elapsedMs: Date.now() - started, detail });
    sp.win32.exitProcess();
  };
  const assert = (test, message) => { if (!test) throw Error(message); };
  const flags = () => [sp.Game.isMovementControlsEnabled(), sp.Game.isFightingControlsEnabled(),
    sp.Game.isCamSwitchControlsEnabled(), sp.Game.isLookingControlsEnabled(), sp.Game.isSneakingControlsEnabled(),
    sp.Game.isMenuControlsEnabled(), sp.Game.isActivateControlsEnabled(), sp.Game.isJournalControlsEnabled()];
  const inspect = id => {
    const ref = sp.ObjectReference.from(sp.Game.getFormEx(id));
    if (!ref) return null;
    const actor = sp.Actor.from(ref);
    return { id, handle: sp.getReferenceHandle(id), baseId: ref.getBaseObject().getFormID(),
      disabled: ref.isDisabled(), blocked: ref.isActivationBlocked(), deleted: ref.isDeleted(),
      loaded: ref.is3DLoaded(), dead: actor && actor.isDead(), health: actor && actor.getActorValue('Health') };
  };
  const command = (coordinator, action) => {
    sp.mpClientPlugin.send(JSON.stringify({ t: 34, epoch: coordinator.replica.packetEpoch,
      message: { t: 1, contentJsonDump: JSON.stringify({ customPacketType: 'boyhouse.instance', action, area: 'projection_test' }) }
    }), true);
    log('command', { action, version: coordinator.status.version });
  };
  sp.on('consoleMessage', e => {
    if (/error|failed|spawn observed|logged in/i.test(e.message)) log('console', e.message);
  });
  sp.on('tick', () => {
    if (!stopping && Date.now() - started > 120000) finish(false, 'Projection runtime deadline expired');
  });
  sp.on('update', () => {
    if (stopping) return;
    try {
      const coordinator = sp.storage.boyhouseProjectionCoordinator;
      if (!coordinator) return;
      const status = coordinator.status;
      const view = sp.storage.view, model = sp.storage.worldModel;
      const projection = coordinator.replica.snapshot();
      if (Date.now() >= nextSample) {
        nextSample = Date.now() + 1000;
        const player = sp.Game.getPlayer();
        log('sample', { phase, status, projection, player: sp.storage.boyhousePlayerSpawnLifetime,
          position: player && [player.getPositionX(), player.getPositionY(), player.getPositionZ()],
          world: player && (player.getWorldSpace() || player.getParentCell()).getFormID(),
          controls: flags(), view: view && model && view.getSpawnReadiness(model),
          owned: Array.from(sp.storage.boyhouseOwnedReferences.references.values()).map(r => ({
            id: r.id, retired: r.retired, error: r.error, work: r.work.length, operation: !!r.operation })),
          operations: { errors: sp.storage.boyhouseNativeReferenceOperations.errors,
            lanes: sp.storage.boyhouseNativeReferenceOperations.lanes.size },
          suppression: Array.from(sp.storage.boyhouseReferenceSuppression.entries).map(([id, r]) => ({
            id, settled: r.settled, error: r.error, owners: Array.from(r.owners.keys()) })),
        });
      }
      if (status.phase === 'failed') throw Error(status.error);
      if (phase !== 'login' && status.version === 0) throw Error('Connection reset during an instance transition');
      if (status.phase !== 'ready' || !projection || Date.now() < waitUntil) return;
      assert(!sp.Ui.isMenuOpen('RaceSex Menu'), 'Precreated test profile unexpectedly needs character creation');
      if (phase === 'login') {
        assert(projection.instance === '', 'Initial world is not shared');
        original = inspect(1080569); controls = flags();
        assert(original && !original.disabled && !original.dead, 'Fixture original is unavailable before entry');
        log('before-entry', { original, controls });
        command(coordinator, 'enter'); phase = 'entering'; waitUntil = Date.now() + 500;
      } else if (phase === 'entering' && projection.instance) {
        const mapping = projection.references.find(r => r.original === original.id);
        assert(mapping, 'Area scan did not project the fixture original');
        assert(mapping.baseId === original.baseId, 'Fixture content identity differs');
        const hidden = inspect(original.id);
        assert(hidden.disabled && hidden.blocked && hidden.dead === original.dead && hidden.health === original.health,
          'Original was not safely hidden');
        copies = projection.references.filter(r => !r.disabled && !r.deleted).map(r => {
          const local = view.getLocalRefrId(r.copy), copy = inspect(local);
          assert(copy && copy.handle && copy.loaded && !copy.disabled && copy.baseId === r.baseId,
            'Projected copy is not observed ready: ' + r.original);
          assert(view.getProjectedLocalRefrId(r.original) === local, 'Original alias does not resolve to its copy');
          return { ...r, local, handle: copy.handle };
        });
        assert(copies.length > 0, 'No visible projected copies');
        assert(JSON.stringify(flags()) === JSON.stringify(controls), 'Controls were not restored after entry');
        enteredVersion = status.version;
        log('entered', { hidden, copies, version: enteredVersion });
        // The authority deliberately refuses an exit command while still in
        // the altered area. Cross its boundary and let normal movement input
        // and server reconciliation initiate the shared-world transition.
        phase = 'leaving'; waitUntil = Date.now() + 1000;
      } else if (phase === 'leaving' && projection.instance) {
        // Incremental, observed movement also tolerates the server's initial
        // position correction after a new baseline. Do not assume a native
        // teleport call or a sent packet means the player reached its target.
        // Generated-save loading temporarily blocks MoveRefrToPosition in
        // native code. Bound the observed retry window, not just one call.
        assert(++leaveSteps <= 80, 'Player did not leave the altered area');
        const player = sp.Game.getPlayer();
        const before = [player.getPositionX(), player.getPositionY(), player.getPositionZ()];
        sp.TESModPlatform.moveRefrToPosition(player, null, player.getWorldSpace(),
          before[0] + 64, before[1], before[2], 0, 0, 72);
        log('cross-boundary-step', { before, step: leaveSteps });
        waitUntil = Date.now() + 250;
      } else if (phase === 'leaving' && !projection.instance && status.version > enteredVersion) {
        const restored = inspect(original.id);
        assert(restored && restored.disabled === original.disabled && restored.blocked === original.blocked &&
          restored.dead === original.dead && restored.health === original.health, 'Original state was not restored');
        for (const copy of copies) {
          assert(sp.getReferenceHandle(copy.local) !== copy.handle, 'Temporary copy is still alive');
          assert(view.getProjectedLocalRefrId(copy.original) === undefined, 'Projection alias survived exit');
        }
        assert(JSON.stringify(flags()) === JSON.stringify(controls), 'Controls were not restored after exit');
        if (++sampleCount >= 5) finish(true, { restored, copiesRemoved: copies.length, controls: flags(), version: status.version });
        else waitUntil = Date.now() + 200;
      }
    } catch (error) { finish(false, String(error.stack || error)); }
  });
  log('loaded', { platform: sp.getPlatformVersion() });
})();
`;
fs.writeFileSync(output, client + '\n' + observer);
console.log(output);
