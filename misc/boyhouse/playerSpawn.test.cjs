const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const { EventEmitter } = require('node:events');
const root = path.resolve(__dirname, '../../skymp5-client');
const ts = require(require.resolve('typescript', { paths: [root, path.resolve(root, '../skymp5-server')] }));
const compiled = ts.transpileModule(fs.readFileSync(path.join(root, 'src/services/services/playerSpawnService.ts'), 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
}).outputText;
const spawn = (idx = 1, pos = [1000, 2000, 3000], worldOrCell = 60) => ({
  t: 33, isMe: true, idx, refrId: 0xff000000 + idx, transform: { pos, rot: [0, 0, 72], worldOrCell },
  appearance: { name: `Player ${idx}`, isFemale: true, raceId: 0x13746, headpartIds: [], presets: [] },
  props: { learnedSpells: [idx], health: 100, healthPercentage: 0.75 }, customPropsJsonDumps: [],
});

function fixture(storage = {}) {
  const events = new EventEmitter(), emitter = new EventEmitter(), calls = [], loads = [], errors = [], ready = [], guards = [];
  let now = 10000, frame = 0, present = true, world = 60, pos = [0, 0, 0], loaded3D = true;
  let connected = true, mainMenu = false, loadingMenu = false, loadoutGeneration = 0, inTick = false;
  let appearanceHook = () => {}, removeSpellsHook = () => {}, moveFailure;
  const player = () => {
    const capturedFrame = frame;
    const valid = () => assert.equal(capturedFrame, frame, 'Player wrapper escaped its update');
    return {
      getWorldSpace() { valid(); return world ? { getFormID: () => world } : null; }, getParentCell: () => null,
      getPositionX() { valid(); return pos[0]; }, getPositionY() { valid(); return pos[1]; }, getPositionZ() { valid(); return pos[2]; },
      is3DLoaded() { valid(); return loaded3D; }, setActorValue(name, value) { valid(); calls.push(['value', name, value]); },
    };
  };
  class LoadGameService {} class PlayerLoadoutService {} class TimeService {} class NetworkingService {}
  const dependencies = {
    './clientListener': { ClientListener: class {} }, './loadGameService': { LoadGameService },
    './playerLoadoutService': { PlayerLoadoutService }, './timeService': { TimeService }, './networkingService': { NetworkingService },
    '../../logging': { logError: (...args) => errors.push(args), logTrace() {} },
    '../../sync/appearance': { applyAppearanceToPlayer(appearance, guard) {
      calls.push(['appearance', appearance.name]); guards.push(guard); appearanceHook();
    } },
    '../../sync/spell': {
      removeAllSpells: () => { calls.push(['remove-spells']); removeSpellsHook(); },
      learnSpells: (_player, spells) => calls.push(['spells', [...spells]]),
    },
    '../../sync/actorvalues': { setActorValuePercentage: (_player, name, value) => calls.push(['percentage', name, value]) },
  };
  const module = { exports: {} };
  vm.runInNewContext(compiled, { module, exports: module.exports, Date: { now: () => now }, require: name => {
    assert.ok(name in dependencies, `Unexpected dependency ${name}`); return dependencies[name];
  } });
  const loadout = {
    shouldApplySpawnAppearance: () => true,
    beginSpawn(message) { calls.push(['begin', message.idx]); return ++loadoutGeneration; },
    cancelSpawn(generation) { calls.push(['cancel', generation]); if (generation === loadoutGeneration) ++loadoutGeneration; },
    markSpawnReady(generation) { assert.equal(generation, loadoutGeneration); ready.push(generation); },
  };
  const sp = { storage, Ui: { isMenuOpen: name => {
    assert.equal(inTick, false, 'Papyrus Ui calls are forbidden in tick context');
    return name === 'Main Menu' ? mainMenu : name === 'Loading Menu' ? loadingMenu : false;
  } },
    Game: { getPlayer: () => present ? player() : null, getModCount: () => 2, getModName: i => ['Skyrim.esm', 'Update.esm'][i],
      getFormEx: id => ({ id }) }, Cell: { from: value => value }, WorldSpace: { from: value => value },
    TESModPlatform: { moveRefrToPosition(_player, _cell, _world, ...transform) {
      if (moveFailure) throw moveFailure;
      calls.push(['move', ...transform]);
    } },
  };
  const service = new module.exports.PlayerSpawnService(sp, {
    on: (...args) => events.on(...args), emitter, lookupListener(type) {
      if (type === LoadGameService) return { loadGame: (...args) => loads.push(args) };
      if (type === PlayerLoadoutService) return loadout;
      if (type === TimeService) return { getTime: () => ({ newGameHourValue: 12.5 }) };
      if (type === NetworkingService) return { isConnected: () => connected };
      throw Error('Unexpected service lookup');
    },
  });
  return { service, calls, loads, errors, ready, guards, storage, emitter, loadout,
    update(ms = 0) { now += ms; ++frame; events.emit('update'); },
    tick(ms = 0) { now += ms; inTick = true; try { events.emit('tick'); } finally { inTick = false; } },
    native: (...args) => events.emit(...args), message: (...args) => emitter.emit(...args),
    move(position, worldId = 60) { pos = [...position]; world = worldId; },
    present(value) { present = value; }, loaded3D(value) { loaded3D = value; },
    connected(value) { connected = value; }, mainMenu(value) { mainMenu = value; },
    loadingMenu(value) { loadingMenu = value; events.emit(value ? 'menuOpen' : 'menuClose', { name: 'Loading Menu' }); },
    appearanceHook(value) { appearanceHook = value; }, removeSpellsHook(value) { removeSpellsHook = value; },
    moveFailure(value) { moveFailure = value; },
    arrive(message = spawn()) { pos = [...message.transform.pos]; world = message.transform.worldOrCell;
      for (let i = 0; i < 5; ++i) { ++frame; events.emit('update'); } },
  };
}

test('elapsed time cannot declare an unpositioned player ready', () => {
  const f = fixture(), generation = f.service.request(spawn());
  f.update(); f.tick(5000); f.update();
  assert.equal(f.service.isReady(generation), false); assert.equal(f.ready.length, 0);
  assert.equal(f.calls.some(c => c[0] === 'appearance'), false);
  f.arrive(); assert.equal(f.service.isReady(generation), true); assert.equal(f.ready.length, 1);
  f.update(5000); assert.equal(f.ready.length, 1);
});

for (const condition of ['z', 'world', '3d']) test(`readiness requires the correct ${condition}, not matching horizontal coordinates`, () => {
  const f = fixture(), message = spawn(), generation = f.service.request(message);
  f.move(condition === 'z' ? [1000, 2000, 5000] : message.transform.pos, condition === 'world' ? 61 : 60);
  if (condition === '3d') f.loaded3D(false);
  for (let i = 0; i < 4; ++i) f.update(500);
  assert.equal(f.service.isReady(generation), false); assert.equal(f.ready.length, 0);
  f.loaded3D(true); f.arrive(message); assert.equal(f.service.isReady(generation), true);
});

test('an already nearby player still receives the authoritative rotation before readiness', () => {
  const f = fixture(), message = spawn(); f.move(message.transform.pos);
  f.service.request(message); f.arrive(message);
  assert.deepEqual(f.calls.find(c => c[0] === 'move'), ['move', 1000, 2000, 3000, 0, 0, 72]);
  assert.equal(f.calls.filter(c => c[0] === 'move').length, 1);
});

test('a superseding self snapshot owns positioning, spells, appearance and loadout readiness', () => {
  const f = fixture(), first = f.service.request(spawn(1)); f.update();
  const message = spawn(2, [4000, 5000, 6000], 62), next = f.service.request(message);
  f.arrive(message);
  assert.equal(f.service.isReady(first), false); assert.equal(f.service.isReady(next), true);
  assert.deepEqual(f.calls.filter(c => c[0] === 'appearance'), [['appearance', 'Player 2']]);
  assert.deepEqual(f.calls.filter(c => c[0] === 'spells'), [['spells', [2]]]);
  assert.equal(f.ready.length, 1);
});

test('a same-player creation draft prevents baseline presentation from overwriting its face', () => {
  const f = fixture(); f.loadout.shouldApplySpawnAppearance = () => false;
  const message = spawn(); const generation = f.service.request(message);
  f.arrive(message);
  assert.equal(f.service.isReady(generation), true);
  assert.equal(f.calls.some(call => call[0] === 'appearance'), false);
  assert.ok(f.calls.some(call => call[0] === 'spells'));
});

for (const during of ['appearance', 'removeSpells']) test(`a replacement during ${during} prevents later old-snapshot effects`, () => {
  const f = fixture(); f.service.request(spawn());
  f[`${during}Hook`](() => { f[`${during}Hook`](() => {}); f.service.request(spawn(2)); });
  f.arrive(spawn(1));
  assert.equal(f.calls.some(c => c[0] === 'spells' && c[1][0] === 1), false);
  assert.equal(f.ready.length, 0);
  f.arrive(spawn(2)); assert.equal(f.ready.length, 1);
  assert.equal(f.guards[0](), false);
});

test('bootstrap loads one generated world and waits for native load and position observations', () => {
  const f = fixture(); f.present(false); const message = spawn(), generation = f.service.request(message);
  f.tick(499); assert.equal(f.loads.length, 0); f.tick(1); f.tick(5000); assert.equal(f.loads.length, 1);
  assert.equal(f.loads[0][3].isFemale, true); assert.deepEqual([...f.loads[0][4]], ['Skyrim.esm', 'Update.esm']);
  f.native('preLoadGame'); f.update(); assert.equal(f.ready.length, 0);
  f.present(true); f.message('gameLoad', { isCausedBySkyrimPlatform: true });
  f.tick(1000); assert.equal(f.loads.length, 1, 'A post-load tick must not generate another world before update');
  f.arrive(message);
  assert.equal(f.service.isReady(generation), true);
});

test('a replacement arriving during generated loading positions the new snapshot without starting a second load', () => {
  const f = fixture(); f.present(false); f.service.request(spawn()); f.tick(500); f.native('preLoadGame');
  const message = spawn(2, [7000, 8000, 9000]), generation = f.service.request(message); f.tick(2000);
  assert.equal(f.loads.length, 1);
  f.present(true); f.message('gameLoad', { isCausedBySkyrimPlatform: true }); f.arrive(message);
  assert.equal(f.service.isReady(generation), true);
  assert.deepEqual(f.calls.filter(c => c[0] === 'appearance'), [['appearance', 'Player 2']]);
});

test('personal-save pre-load cancels old player work before the loaded event', () => {
  const f = fixture(); const generation = f.service.request(spawn()); f.update();
  f.native('preLoadGame'); f.move([9000, 9000, 9000]); f.update(5000);
  f.message('gameLoad', { isCausedBySkyrimPlatform: false }); f.update();
  assert.equal(f.service.isReady(generation), false); assert.equal(f.ready.length, 0);
  assert.equal(f.calls.filter(c => c[0] === 'move').length, 1);
});

test('disconnect during loading cannot apply old state when the generated world arrives', () => {
  const f = fixture(); f.present(false); f.service.request(spawn()); f.tick(500);
  f.message('connectionDisconnect'); f.present(true); f.message('gameLoad', { isCausedBySkyrimPlatform: true }); f.arrive();
  assert.equal(f.ready.length, 0); assert.equal(f.calls.some(c => c[0] === 'appearance'), false);
});

test('direct socket closure cancels a pending spawn even without a disconnect event', () => {
  const f = fixture(); f.service.request(spawn()); f.update(); f.connected(false); f.arrive();
  assert.equal(f.ready.length, 0); assert.equal(f.service.status, undefined);
});

test('hot reload invalidates previously scheduled appearance callbacks and native work', () => {
  const f = fixture(); f.service.request(spawn()); f.arrive(); assert.equal(f.guards[0](), true);
  fixture(f.storage); assert.equal(f.guards[0](), false);
});

test('failed movement and deadlines remain failed instead of applying presentation or releasing equipment', () => {
  const f = fixture(); f.service.request(spawn());
  for (let i = 0; i < 10; ++i) f.update(500);
  assert.equal(f.service.status.phase, 'failed'); assert.equal(f.calls.filter(c => c[0] === 'move').length, 8);
  assert.equal(f.ready.length, 0); assert.equal(f.errors.length, 1);
  const timeout = fixture(); timeout.present(false); timeout.service.request(spawn()); timeout.tick(60001);
  assert.equal(timeout.service.status.phase, 'failed'); assert.equal(timeout.loads.length, 0);
});

test('native movement exceptions cancel the queued loadout and preserve the failure', () => {
  const f = fixture(); f.moveFailure(Error('native move rejected')); f.service.request(spawn()); f.update(); f.arrive();
  assert.equal(f.ready.length, 0); assert.match(f.service.status.error, /native move rejected/);
});

test('loading screens do not trigger another generated load or repeated teleports', () => {
  const f = fixture(); f.service.request(spawn()); f.loadingMenu(true); f.update(); f.tick(1000);
  assert.equal(f.loads.length, 0); assert.equal(f.calls.some(c => c[0] === 'move'), false);
  f.loadingMenu(false); f.arrive(); assert.equal(f.ready.length, 1);
});

test('loading-menu events gate main-menu bootstrap without calling Papyrus from tick', () => {
  const f = fixture(); f.present(false); f.mainMenu(true); f.service.request(spawn());
  f.loadingMenu(true); f.tick(1000); assert.equal(f.loads.length, 0);
  f.loadingMenu(false); f.tick(); assert.equal(f.loads.length, 1);
});

test('invalid transforms leave the current valid spawn intact', () => {
  const f = fixture(), generation = f.service.request(spawn());
  assert.throws(() => f.service.request(spawn(2, [NaN, 0, 0])), /transform/);
  assert.throws(() => f.service.request(spawn(2, [0, 0, 0], 0x100000000)), /transform/);
  f.arrive(); assert.equal(f.service.isReady(generation), true);
});
