// Build an isolated native fixture from the actual client implementation.
// This only writes the requested plugin artifact; installation/INI backups and
// launching Skyrim are deliberately separate operations. Use a generated save.
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '../../skymp5-client');
const ts = require(require.resolve('typescript', { paths: [root, path.resolve(root, '../skymp5-server')] }));
const output = process.argv[2];
if (!output) throw Error('Usage: node misc/boyhouse/build-reference-smoke.cjs <output.js>');
function compile(file) {
  return ts.transpileModule(fs.readFileSync(path.join(root, 'src', file), 'utf8'), { compilerOptions: {
    module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2019,
  } }).outputText;
}
const modules = {
  '../../campaign/referenceSuppression': compile('campaign/referenceSuppression.ts'),
  service: compile('services/services/referenceSuppressionService.ts'),
};
// SP3 exposes this native module as a global (the client webpack build uses
// the same external); it is not a Node package resolvable through require().
const prelude = `const sp = skyrimPlatform;
const log = (event, value) => sp.writeLogs('boyhouseReferenceSmoke', event, JSON.stringify(value));
const dependencies = {
  './clientListener': { ClientListener: class {} },
  '../../logging': { logError: (...args) => log('service-error', args.map(String)) },
};
`;
const wrappers = Object.entries(modules).map(([id, source]) => `{
const module = { exports: {} };
(function(module, exports, require) {\n${source}\n})(module, module.exports, id => {
  if (!(id in dependencies)) throw Error('Unexpected fixture dependency: ' + id);
  return dependencies[id];
});
dependencies[${JSON.stringify(id)}] = module.exports;
}
`).join('');

function fixture() {
  const listeners = new Map();
  const emitter = {
    on(event, callback) {
      const callbacks = listeners.get(event) || []; callbacks.push(callback); listeners.set(event, callbacks);
    },
    emit(event, value) { for (const callback of listeners.get(event) || []) callback(value); },
  };
  const controller = { emitter, on: (event, callback) => sp.on(event, callback) };
  const service = new dependencies.service.ReferenceSuppressionService(sp, controller);
  const ledger = service.references;
  const started = Date.now();
  let phase = 'load', requested = false, loaded = false, original, releasedAt = 0, result;
  const snapshot = actor => ({
    id: actor.getFormID(), baseId: actor.getBaseObject().getFormID(), disabled: actor.isDisabled(),
    blocked: actor.isActivationBlocked(), deleted: actor.isDeleted(), dead: actor.isDead(),
    health: actor.getActorValue('Health'), position: [actor.getPositionX(), actor.getPositionY(), actor.getPositionZ()],
  });
  const check = (condition, message) => { if (!condition) throw Error(message); };
  const finish = (passed, reason) => {
    if (result) return;
    result = { passed, reason, elapsedMs: Date.now() - started, original };
    log('result', result);
    ledger.releaseAll(); releasedAt = Date.now(); phase = 'cleanup';
  };
  sp.once('tick', () => sp.blockPapyrusEvents(true));
  sp.once('update', () => sp.TESModPlatform.blockPapyrusEvents(true));
  sp.on('loadGame', () => { emitter.emit('gameLoad'); loaded = true; log('world-loaded', true); });
  sp.on('tick', () => {
    if (!requested && Date.now() - started > 1500) {
      requested = true;
      log('load-generated-world', true);
      sp.loadGame([133857, -61130, 14662], [0, 0, 72], 0x3c, { name: 'Boyhouse Reference Test' },
        ['Skyrim.esm', 'Update.esm', 'Dawnguard.esm', 'HearthFires.esm', 'Dragonborn.esm']);
    }
    if (!result && Date.now() - started > 90000) finish(false, 'Native fixture deadline exceeded in ' + phase);
    if (result && Date.now() - releasedAt > 10000) {
      log('cleanup-timeout', { remaining: ledger.state.entries.size }); sp.win32.exitProcess();
    }
  });
  sp.on('update', () => {
    try {
      if (phase === 'cleanup') {
        if (ledger.state.entries.size === 0) { log('cleanup-complete', true); phase = 'done'; sp.win32.exitProcess(); }
        return;
      }
      if (phase === 'done' || !loaded) return;
      // SP3's native wrappers are valid only for the current callback. Keep
      // numeric identity/snapshots between frames, just as the real adapter does.
      let reference = original ? sp.Actor.from(sp.Game.getFormEx(original.id)) : undefined;
      if (original && !reference) throw Error('Captured actor disappeared');
      if (reference) {
        const error = ledger.error(original.id);
        if (error) throw Error(error);
      }
      if (phase === 'load') {
        // Authored ambient creature markers near the generated world's spawn,
        // taken from the user's owned Skyrim.esm (no third-party game assets).
        for (const id of [1080569, 1080566, 1080583, 1080568]) {
          const actor = sp.Actor.from(sp.Game.getFormEx(id));
          if (!actor || actor.isDeleted() || actor.isDisabled() || actor.isDead() ||
              !actor.is3DLoaded() || actor.isInDialogueWithPlayer() || actor.getDialogueTarget()) continue;
          reference = actor; original = snapshot(actor); break;
        }
        if (!reference) return;
        log('captured', original);
        ledger.acquire('cleaner', original.id); ledger.acquire('quest', original.id); phase = 'hide';
      } else if (phase === 'hide' && ledger.isSettled([original.id])) {
        const state = snapshot(reference); log('hidden', state);
        check(state.disabled && state.blocked && !state.dead && !state.deleted, 'Actor was not safely hidden');
        ledger.releaseOwner('cleaner'); phase = 'shared';
      } else if (phase === 'shared' && ledger.isSettled([original.id])) {
        check(reference.isDisabled() && reference.isActivationBlocked(), 'Remaining quest claim did not retain suppression');
        log('remaining-owner-retained', true); ledger.releaseOwner('quest'); phase = 'restore';
      } else if (phase === 'restore' && ledger.isSettled([original.id])) {
        const restored = snapshot(reference); log('restored', restored);
        check(restored.baseId === original.baseId && restored.disabled === original.disabled &&
          restored.blocked === original.blocked && restored.dead === original.dead && restored.deleted === original.deleted,
          'Restored actor flags differ from captured state');
        check(ledger.state.entries.size === 0, 'Restoration record not released');
        // Cancel while the native disable is in flight, then wait for a real
        // enable and restoration of activation before accepting the result.
        ledger.acquire('canceledPhase', original.id); ledger.update();
        ledger.releaseOwner('canceledPhase'); phase = 'cancel';
      } else if (phase === 'cancel' && ledger.isSettled([original.id])) {
        const restored = snapshot(reference); log('canceled-phase-restored', restored);
        check(!restored.disabled && restored.blocked === original.blocked && !restored.dead && !restored.deleted,
          'Canceled transition failed to restore the actor');
        finish(true, 'Native hide, overlapping ownership, restoration, and canceled transition passed');
      }
    } catch (error) { finish(false, String(error && error.stack || error)); }
  });
  log('loaded', { platform: sp.getPlatformVersion(), fixture: 'reference-suppression' });
}
fs.writeFileSync(output, prelude + wrappers + `(${fixture.toString()})();\n`);
console.log(output);
