// Builds only a diagnostic artifact. Install separately with INI/save backups.
const fs = require('node:fs'), path = require('node:path');
const root = path.resolve(__dirname, '../../skymp5-client');
const ts = require(require.resolve('typescript', { paths: [root, path.resolve(root, '../skymp5-server')] }));
const output = process.argv[2];
if (!output) throw Error('Usage: node misc/boyhouse/build-lifetime-smoke.cjs <output.js>');
const modules = [
  ['../../campaign/ownedReferenceLifetime', 'campaign/ownedReferenceLifetime.ts'],
  ['service', 'services/services/ownedReferenceService.ts'],
  ['./formTypeEx', 'extensions/formTypeEx.ts'],
  ['../extensions/objectReferenceEx', 'extensions/objectReferenceEx.ts'],
  ['spawn', 'view/spawnProcess.ts'],
];
let source = `const sp = skyrimPlatform;
const log = (event, value) => sp.writeLogs('boyhouseLifetimeSmoke', event, JSON.stringify(value));
const dependencies = {
  skyrimPlatform: sp,
  './clientListener': { ClientListener: class {} },
  './worldCleanerService': { WorldCleanerService: class {} },
  './nativeReferenceOperationService': { NativeReferenceOperationService: class {} },
  '../services/services/nativeReferenceOperationService': { NativeReferenceOperationService: class {} },
  '../services/spApiInteractor': { SpApiInteractor: {} },
  '../../logging': { logError: (...args) => log('service-error', args.map(String)) },
  '../sync/appearance': { applyTints: () => { throw Error('This fixture uses unmodified NPC appearance'); } },
};
`;
for (const [id, file] of modules) {
  const compiled = ts.transpileModule(fs.readFileSync(path.join(root, 'src', file), 'utf8'), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2019 },
  }).outputText;
  source += `{
const module = { exports: {} };
(function(module, exports, require) {\n${compiled}\n})(module, module.exports, id => {
 if (!(id in dependencies)) throw Error('Unexpected fixture dependency: ' + id);
 return dependencies[id];
});
dependencies[${JSON.stringify(id)}] = module.exports;
}
`;
}

function fixture() {
  const listeners = new Map(), protection = new Map();
  const emitter = {
    on(event, callback) { const list = listeners.get(event) || []; list.push(callback); listeners.set(event, list); },
    emit(event) { for (const callback of listeners.get(event) || []) callback(); },
  };
  const controller = { emitter, on: sp.on, once: sp.once, lookupListener: () => ({
    operations: { isBusy: () => false }, // This isolated fixture submits no borrowed mutations.
    modWcProtection(id, amount) { protection.set(id, (protection.get(id) || 0) + amount); },
  }) };
  const service = new dependencies.service.OwnedReferenceService(sp, controller);
  const lifetime = service.references;
  let phase = 'load', loaded = false, requested = false, baseId, reference, ready = 0, canceledReady = 0, result;
  let cleanupAt = 0, originalAI;
  let nextSample = 0;
  const started = Date.now();
  const check = (condition, message) => { if (!condition) throw Error(message); };
  const finish = (passed, reason) => {
    if (result) return;
    result = { passed, reason, elapsedMs: Date.now() - started, ready, canceledReady };
    log('result', result); lifetime.retireAll(); cleanupAt = Date.now(); phase = 'cleanup';
  };
  const create = (canceled, dead = false, object = false) => {
    const player = sp.Game.getPlayer();
    const placed = player.placeAtMe(sp.Game.getFormEx(object ? 0xf : baseId), 1, true, true);
    check(!!placed, 'Failed to create a native temporary reference');
    const actor = sp.Actor.from(placed); originalAI = actor?.isAIEnabled();
    check(object ? !actor : !!actor, 'Spawn base has the wrong reference type');
    reference = lifetime.claim(placed.getFormID()); protection.set(reference.id, 1);
    check(reference.id >= 0xff000000 && reference.handle > 0, 'Invalid temporary reference identity');
    log('claimed', { id: reference.id, handle: reference.handle, canceled, dead, object, originalAI });
    new dependencies.spawn.SpawnProcess(null, [133857, -61130, 14662], reference, lifetime, () => {
      if (canceled) ++canceledReady; else ++ready;
    }, [0, 0, 72], dead);
    if (canceled) { lifetime.update(); lifetime.retire(reference); }
  };
  sp.once('tick', () => sp.blockPapyrusEvents(true));
  sp.once('update', () => sp.TESModPlatform.blockPapyrusEvents(true));
  sp.on('loadGame', () => { emitter.emit('gameLoad'); loaded = true; });
  sp.on('tick', () => {
    if (!requested && Date.now() - started > 1500) {
      requested = true;
      sp.loadGame([133857, -61130, 14662], [0, 0, 72], 0x3c, { name: 'Boyhouse Lifetime Test' },
        ['Skyrim.esm', 'Update.esm', 'Dawnguard.esm', 'HearthFires.esm', 'Dragonborn.esm']);
    }
    if (!result && Date.now() - started > 90000) finish(false, 'Native fixture deadline exceeded in ' + phase);
    if (result && Date.now() - cleanupAt > 10000) { log('cleanup-timeout', lifetime.state.references.size); sp.win32.exitProcess(); }
  });
  sp.on('update', () => {
    try {
      if (phase === 'cleanup') {
        if (lifetime.state.references.size === 0) { log('cleanup-complete', true); phase = 'done'; sp.win32.exitProcess(); }
        return;
      }
      if (phase === 'done' || !loaded) return;
      if (reference?.error) throw Error(reference.error);
      if (reference && Date.now() >= nextSample && (phase === 'spawn' || phase === 'corpse')) {
        nextSample = Date.now() + 1000;
        const actor = sp.Actor.from(sp.Game.getFormEx(reference.id));
        if (actor) log('spawn-sample', { phase, id: reference.id, disabled: actor.isDisabled(),
          dead: actor.isDead(), loaded: actor.is3DLoaded(), position: [actor.getPositionX(), actor.getPositionY(), actor.getPositionZ()] });
      }
      if (phase === 'load') {
        const original = sp.Actor.from(sp.Game.getFormEx(1080569));
        if (!original || !original.is3DLoaded()) return;
        baseId = original.getBaseObject().getFormID();
        check(sp.getReferenceHandle(0xffffffff) === 0, 'Absent reference must have zero identity');
        check(sp.getReferenceHandle(0x14) > 0, 'Player reference has no identity');
        create(false); phase = 'spawn';
      } else if (phase === 'spawn' && ready === 1) {
        const actor = sp.Actor.from(sp.Game.getFormEx(reference.id));
        check(actor && !actor.isDisabled() && !actor.isDead() && actor.is3DLoaded(), 'Spawn did not become a loaded live actor');
        check(actor.isAIEnabled() === originalAI, 'Live spawn failed to restore its AI state');
        check(sp.getReferenceHandle(reference.id) === reference.handle, 'Engine identity changed during spawn');
        log('ready', { id: reference.id, handle: reference.handle });
        lifetime.retire(reference); phase = 'delete';
      } else if (phase === 'delete' && lifetime.state.references.size === 0) {
        check(sp.getReferenceHandle(reference.id) === 0, 'Deletion did not invalidate identity');
        check(protection.get(reference.id) === 0, 'Protection was not released exactly once');
        log('deleted', reference.id); create(false, true); phase = 'corpse';
      } else if (phase === 'corpse' && ready === 2) {
        const actor = sp.Actor.from(sp.Game.getFormEx(reference.id));
        check(actor && !actor.isDisabled() && actor.isDead() && actor.is3DLoaded(), 'Dead spawn did not become a loaded corpse');
        check(actor.isAIEnabled() === originalAI, 'Dead spawn failed to restore its AI state');
        log('corpse-ready', { id: reference.id, handle: reference.handle, dead: actor.isDead(), loaded: actor.is3DLoaded() });
        lifetime.retire(reference); phase = 'corpse-delete';
      } else if (phase === 'corpse-delete' && lifetime.state.references.size === 0) {
        check(sp.getReferenceHandle(reference.id) === 0 && protection.get(reference.id) === 0, 'Corpse cleanup incomplete');
        create(false, false, true); phase = 'object';
      } else if (phase === 'object' && ready === 3) {
        const object = sp.ObjectReference.from(sp.Game.getFormEx(reference.id));
        check(object && !object.isDisabled() && object.is3DLoaded() && !sp.Actor.from(object), 'Object did not become a loaded non-actor');
        log('object-ready', { id: reference.id, handle: reference.handle, loaded: object.is3DLoaded() });
        lifetime.retire(reference); phase = 'object-delete';
      } else if (phase === 'object-delete' && lifetime.state.references.size === 0) {
        check(sp.getReferenceHandle(reference.id) === 0 && protection.get(reference.id) === 0, 'Object cleanup incomplete');
        create(true); phase = 'cancel';
      } else if (phase === 'cancel' && lifetime.state.references.size === 0) {
        check(canceledReady === 0 && ready === 3, 'Canceled spawn reported ready');
        check(sp.getReferenceHandle(reference.id) === 0, 'Canceled actor was not deleted');
        check(protection.get(reference.id) === 0, 'Canceled actor protection was not released');
        finish(true, 'Observed live/dead/object spawns, AI restoration, identity, cancellation, drain, and deletion passed');
      }
    } catch (error) { finish(false, String(error && error.stack || error)); }
  });
  log('loaded', { platform: sp.getPlatformVersion(), fixture: 'owned-reference-lifetime' });
}
fs.writeFileSync(output, source + `(${fixture.toString()})();\n`);
console.log(output);
