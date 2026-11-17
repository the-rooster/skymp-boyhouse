const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const { EventEmitter } = require('node:events');
const root = path.resolve(__dirname, '../../skymp5-client');
const ts = require(require.resolve('typescript', { paths: [root, path.resolve(root, '../skymp5-server')] }));
const compiled = ts.transpileModule(fs.readFileSync(path.join(root, 'src/services/services/loadGameService.ts'), 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
}).outputText;

function fixture(integratePersonalMode = false) {
  const events = new EventEmitter(), emitter = new EventEmitter(), loaded = [], nativeCalls = [];
  let connected = true, closes = 0, inUpdate = false;
  class NetworkingService {}
  const dependencies = { './clientListener': { ClientListener: class {} }, './networkingService': { NetworkingService } };
  const module = { exports: {} };
  vm.runInNewContext(compiled, { module, exports: module.exports, require(name) {
    assert.ok(name in dependencies, `Unexpected dependency ${name}`); return dependencies[name];
  } });
  const sp = { loadGame() {}, Game: { setInChargen(...flags) {
    assert.equal(inUpdate, true, 'Papyrus call escaped update'); nativeCalls.push(flags);
  } }, Debug: { messageBox() { assert.equal(inUpdate, true, 'Papyrus call escaped update'); } } };
  const controller = {
    on: (...args) => events.on(...args), once: (...args) => events.once(...args), emitter,
    lookupListener(type) {
      assert.equal(type, NetworkingService);
      return { isConnected: () => connected, close() { ++closes; connected = false; emitter.emit('connectionDisconnect'); } };
    },
  };
  const service = new module.exports.LoadGameService(sp, controller);
  let personal;
  if (integratePersonalMode) {
    for (const [file, exported] of [['singlePlayerService.ts', 'SinglePlayerService'],
      ['enforceLimitationsService.ts', 'EnforceLimitationsService']]) {
      const source = ts.transpileModule(fs.readFileSync(path.join(root, 'src/services/services', file), 'utf8'), {
        compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
      }).outputText;
      const target = { exports: {} };
      vm.runInNewContext(source, { module: target, exports: target.exports, require(name) {
        assert.ok(name in dependencies, `Unexpected dependency ${name}`); return dependencies[name];
      } });
      const instance = new target.exports[exported](sp, controller);
      if (exported === 'SinglePlayerService') personal = instance;
    }
  }
  emitter.on('gameLoad', event => loaded.push(event));
  return { service, events, emitter, loaded, nativeCalls, personal, get closes() { return closes; },
    update() { inUpdate = true; try { events.emit('update'); } finally { inUpdate = false; } },
    reconnect() { connected = true; emitter.emit('connectionAccepted'); },
  };
}

for (const bootstrap of [false, true]) test(`failed ${bootstrap ? 'generated bootstrap' : 'personal load'} ends its invalidated campaign after event delivery`, () => {
  const f = fixture();
  if (bootstrap) f.service.loadGame([0, 0, 0], [0, 0, 0], 60);
  assert.equal(f.service.isLoadingPlatformSave, bootstrap);
  f.events.emit('preLoadGame'); f.events.emit('postLoadGame', { success: false });
  assert.equal(f.service.isLoadingPlatformSave, false); assert.equal(f.closes, 0);
  assert.equal(f.loaded.length, 0, 'Failure must not announce world replacement');
  f.events.emit('tick'); assert.equal(f.closes, 1);
  f.events.emit('tick'); assert.equal(f.closes, 1);
});

test('newGame immediately uses the personal replacement lifecycle even during a generated bootstrap', () => {
  const f = fixture(true); f.update(); f.nativeCalls.length = 0;
  f.service.loadGame([0, 0, 0], [0, 0, 0], 60);
  f.events.emit('newGame');
  assert.equal(f.service.isLoadingPlatformSave, false); assert.equal(f.closes, 1);
  assert.equal(f.personal.isSinglePlayer, true);
  assert.equal(f.loaded.length, 1); assert.equal(f.loaded[0].isCausedBySkyrimPlatform, false);
  assert.equal(f.loaded[0].isNewGame, true); assert.equal(f.nativeCalls.length, 0);
  f.update(); assert.deepEqual(f.nativeCalls, [[false, false, false]]);
});

test('deferred new-game presentation cannot change a newer multiplayer session', () => {
  const f = fixture(true); f.update(); f.nativeCalls.length = 0;
  f.events.emit('newGame'); f.reconnect(); f.update();
  assert.equal(f.nativeCalls.length, 0);
});

test('a second new game after reconnect invalidates that session even when personal mode was already selected', () => {
  const f = fixture(true); f.update(); f.nativeCalls.length = 0;
  f.events.emit('newGame'); assert.equal(f.closes, 1);
  f.reconnect(); f.events.emit('newGame'); assert.equal(f.closes, 2);
  assert.equal(f.loaded.length, 2); assert.equal(f.nativeCalls.length, 0);
  f.update(); assert.deepEqual(f.nativeCalls, [[false, false, false]], 'Only the latest world may run its deferred work');
});

test('a stale failed-load close cannot disconnect a newly accepted session', () => {
  const f = fixture(); f.events.emit('postLoadGame', { success: false });
  f.reconnect(); f.events.emit('tick'); assert.equal(f.closes, 0);
});

test('missing results and successful post-load events do not fabricate replacement or failure', () => {
  const f = fixture(); f.service.loadGame([0, 0, 0], [0, 0, 0], 60);
  f.events.emit('postLoadGame'); f.events.emit('postLoadGame', { success: true }); f.events.emit('tick');
  assert.equal(f.closes, 0); assert.equal(f.loaded.length, 0); assert.equal(f.service.isLoadingPlatformSave, true);
  f.events.emit('loadGame');
  assert.equal(f.loaded.length, 1); assert.equal(f.loaded[0].isCausedBySkyrimPlatform, true);
  f.events.emit('tick'); assert.equal(f.service.isLoadingPlatformSave, false);
});
