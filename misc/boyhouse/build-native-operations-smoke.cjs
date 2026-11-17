// Builds a generated-save diagnostic using the actual client services. Back up
// INIs/saves and install separately; this script only writes the requested file.
const fs = require('node:fs'), path = require('node:path');
const root = path.resolve(__dirname, '../../skymp5-client');
const ts = require(require.resolve('typescript', { paths: [root, path.resolve(root, '../skymp5-server')] }));
const output = process.argv[2];
if (!output) throw Error('Usage: node misc/boyhouse/build-native-operations-smoke.cjs <output.js>');
const modules = [
  ['../../campaign/nativeReferenceOperations', 'campaign/nativeReferenceOperations.ts'],
  ['../../campaign/ownedReferenceLifetime', 'campaign/ownedReferenceLifetime.ts'],
  ['./nativeReferenceOperationService', 'services/services/nativeReferenceOperationService.ts'],
  ['owned', 'services/services/ownedReferenceService.ts'],
  ['ragdoll', 'services/services/ragdollService.ts'],
];
let source = `const sp = skyrimPlatform;
const log = (event, value) => sp.writeLogs('boyhouseNativeOperationsSmoke', event, JSON.stringify(value));
const dependencies = {
  './clientListener': { ClientListener: class {} },
  './ownedReferenceService': { OwnedReferenceService: class {} },
  './worldCleanerService': { WorldCleanerService: class {} },
  './actorFactionsService': { ActorFactionsService: class {} },
  '../../logging': { logError: (...args) => log('service-error', args.map(String)) },
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
  let native, owned;
  const controller = { emitter, on: sp.on, once: sp.once, lookupListener(type) {
    if (type === dependencies['./nativeReferenceOperationService'].NativeReferenceOperationService) return native;
    if (type === dependencies['./ownedReferenceService'].OwnedReferenceService) return owned;
    if (type === dependencies['./actorFactionsService'].ActorFactionsService) return { prepareDisposal: () => true };
    if (type === dependencies['./worldCleanerService'].WorldCleanerService) return {
      modWcProtection(id, amount) { protection.set(id, (protection.get(id) || 0) + amount); },
    };
    throw Error('Unexpected service lookup');
  } };
  native = new dependencies['./nativeReferenceOperationService'].NativeReferenceOperationService(sp, controller);
  owned = new dependencies.owned.OwnedReferenceService(sp, controller);
  const ragdoll = new dependencies.ragdoll.RagdollService(sp, controller);
  const lifetime = owned.references, operations = native.operations;
  const settings = ['fDiffMultHPToPCE', 'fDiffMultHPToPCH', 'fDiffMultHPToPCL',
    'fDiffMultHPToPCN', 'fDiffMultHPToPCVE', 'fDiffMultHPToPCVH'];
  const damage = () => settings.map(name => sp.Game.getGameSettingFloat(name));
  const check = (condition, message) => { if (!condition) throw Error(message); };
  let phase = 'load', requested = false, loaded = false, lease, job, baseId, before;
  let callbackCount = 0, lateSteps = 0, result, cleanupAt = 0;
  const started = Date.now();
  const actor = () => sp.Actor.from(sp.Game.getFormEx(lease.id));
  const create = () => {
    const value = sp.Actor.from(sp.Game.getPlayer().placeAtMe(sp.Game.getFormEx(baseId), 1, true, true));
    check(!!value, 'Temporary actor creation failed');
    lease = lifetime.claim(value.getFormID()); protection.set(lease.id, 1);
    lifetime.enqueue(lease, () => actor().enable(false));
    log('claimed', { id: lease.id, handle: lease.handle });
  };
  const finish = (passed, reason) => {
    if (result) return;
    result = { passed, reason, elapsedMs: Date.now() - started, callbackCount, lateSteps, before, after: damage() };
    log('result', result); operations.cancelEpoch(); lifetime.retireAll(); cleanupAt = Date.now(); phase = 'cleanup';
  };
  sp.once('tick', () => sp.blockPapyrusEvents(true));
  sp.once('update', () => sp.TESModPlatform.blockPapyrusEvents(true));
  sp.on('loadGame', () => { emitter.emit('gameLoad'); loaded = true; });
  sp.on('tick', () => {
    if (!requested && Date.now() - started > 1500) {
      requested = true;
      sp.loadGame([133857, -61130, 14662], [0, 0, 72], 0x3c, { name: 'Boyhouse Native Operations Test' },
        ['Skyrim.esm', 'Update.esm', 'Dawnguard.esm', 'HearthFires.esm', 'Dragonborn.esm']);
    }
    if (!result && Date.now() - started > 90000) {
      // No game-setting reads from tick: leave reporting to update.
      phase = 'timeout';
    }
    if (result && Date.now() - cleanupAt > 10000) {
      log('cleanup-timeout', { owned: lifetime.state.references.size, busy: operations.isBusy() }); sp.win32.exitProcess();
    }
  });
  sp.on('update', () => {
    try {
      if (phase === 'cleanup') {
        if (!lifetime.state.references.size && !operations.isBusy()) {
          check(JSON.stringify(damage()) === JSON.stringify(before), 'Cleanup left difficulty settings changed');
          log('cleanup-complete', true); phase = 'done'; sp.win32.exitProcess();
        }
        return;
      }
      if (phase === 'done' || !loaded) return;
      if (phase === 'timeout') throw Error('Native operation fixture deadline exceeded');
      if (lease?.error) throw Error(lease.error);
      if (operations.state.errors.length) throw Error(JSON.stringify(operations.state.errors));
      if (phase === 'load') {
        const original = sp.Actor.from(sp.Game.getFormEx(1080569));
        if (!original || !original.is3DLoaded()) return;
        baseId = original.getBaseObject().getFormID(); before = damage(); create(); phase = 'enable';
      } else if (phase === 'enable' && !lease.operation && !lease.work.length && actor()?.is3DLoaded()) {
        check(!actor().isDisabled(), 'Owned enable failed');
        job = ragdoll.safeRemoveRagdollFromWorld(actor(), () => ++callbackCount);
        operations.update(); check(job.status === 'running', 'Ragdoll did not start');
        check(damage().every(value => value === 0), 'Ragdoll damage override did not apply');
        emitter.emit('worldModelReset'); phase = 'ragdoll-cancel';
      } else if (phase === 'ragdoll-cancel' && job.status === 'cancelled') {
        check(callbackCount === 0, 'Cancelled ragdoll callback ran');
        check(JSON.stringify(damage()) === JSON.stringify(before), 'Cancelled ragdoll did not restore settings');
        log('ragdoll-cancelled', { before, after: damage() });
        job = ragdoll.safeRemoveRagdollFromWorld(actor(), () => ++callbackCount); phase = 'ragdoll-complete';
      } else if (phase === 'ragdoll-complete' && job.status === 'complete') {
        check(callbackCount === 1, 'Ragdoll completion callback count differs');
        check(JSON.stringify(damage()) === JSON.stringify(before), 'Completed ragdoll did not restore settings');
        log('ragdoll-complete', { before, after: damage() });
        lifetime.enqueue(lease, () => actor().disable(false)); lifetime.update();
        check(lifetime.hasNativeOperation(lease.id), 'Owned disable did not start');
        job = native.submit(lease.id, 'visibility', () => true, [reference => {
          check(!lifetime.hasNativeOperation(lease.id), 'Borrowed work raced owned native operation');
          check(reference.isDisabled(), 'Borrowed work started before owned disable took effect');
          return reference.enable(false);
        }, reference => {
          check(!reference.isDisabled(), 'Native enable did not take effect');
          check(!lease.operation?.disposal, 'Deletion began before sequence drained');
          return reference.disable(false);
        }, reference => {
          check(reference.isDisabled(), 'Native disable did not take effect');
          check(!lease.operation?.disposal, 'Deletion raced sequence follow-up');
        }]);
        operations.update(); check(job.status === 'queued', 'Borrowed operation bypassed active owned operation');
        phase = 'borrowed';
      } else if (phase === 'borrowed' && job.status === 'running') {
        lifetime.retire(lease); lifetime.update();
        check(!lease.operation?.disposal, 'Deletion started with native operation pending');
        log('deletion-deferred', lease.id); phase = 'delete';
      } else if (phase === 'delete' && !lifetime.state.references.size) {
        check(job.status === 'complete', 'Deleted before borrowed sequence completed');
        check(sp.getReferenceHandle(lease.id) === 0 && protection.get(lease.id) === 0, 'Owned cleanup incomplete');
        log('sequence-drained-deleted', lease.id); create(); phase = 'enable-cancel';
      } else if (phase === 'enable-cancel' && !lease.operation && !lease.work.length && actor()?.is3DLoaded()) {
        job = native.submit(lease.id, 'cancelled-visibility', () => true,
          [reference => reference.disable(false), () => { ++lateSteps; }], () => ++lateSteps);
        operations.update(); check(job.status === 'running', 'Cancellation had no active native operation');
        emitter.emit('worldModelReset'); lifetime.retire(lease); lifetime.update();
        check(!lease.operation?.disposal, 'Cancelled operation did not retain deletion barrier'); phase = 'cancel-delete';
      } else if (phase === 'cancel-delete' && !lifetime.state.references.size) {
        check(job.status === 'cancelled' && lateSteps === 0, 'Cancelled operation ran a late step or callback');
        check(sp.getReferenceHandle(lease.id) === 0 && protection.get(lease.id) === 0, 'Cancelled owned cleanup incomplete');
        finish(true, 'Native serialization, cancellation, deletion barriers, and ragdoll damage restoration passed');
      }
    } catch (error) { finish(false, String(error && error.stack || error)); }
  });
  log('loaded', { platform: sp.getPlatformVersion(), fixture: 'native-reference-operations' });
}
fs.writeFileSync(output, source + `(${fixture.toString()})();\n`);
console.log(output);
