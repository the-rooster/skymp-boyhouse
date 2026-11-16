const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { EventEmitter } = require('node:events');
const root = path.resolve(__dirname, '../../skymp5-client');
const ts = require(require.resolve('typescript', { paths: [root] }));

function load(file, dependencies) {
  const source = fs.readFileSync(path.join(root, 'src/services/services', file), 'utf8');
  const compiled = ts.transpileModule(source, { compilerOptions: {
    module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020,
  } }).outputText;
  const module = { exports: {} };
  vm.runInNewContext(compiled, { module, exports: module.exports, require: id => {
    if (!(id in dependencies)) throw Error(`Unexpected dependency: ${id}`);
    return dependencies[id];
  } }, { filename: file });
  return module.exports;
}

function equipmentHarness() {
  const events = new EventEmitter(), emitter = new EventEmitter();
  const player = { getFormID: () => 0x14 };
  let count = -1;
  const dependencies = {
    './clientListener': { ClientListener: class {} },
    './singlePlayerService': {}, '../../messages': { MsgType: { UpdateEquipment: 5 } },
    '../../sync/movementGet': {}, '../../view/worldViewMisc': {}, '../../sync/animation': {},
    skyrimPlatform: { FormType: { Book: 27, Potion: 46, Ingredient: 30 } },
    '../../sync/appearance': {}, '../../sync/actorvalues': {},
    '../../sync/equipment': { getEquipment: (actor, numChanges) => {
      assert.equal(actor, player);
      return { inv: { entries: [{ baseId: 0x12e46, count }] }, numChanges };
    } },
    '../../view/hostAttempts': {}, './remoteServer': {}, './deathService': {},
    '../../logging': { logTrace() {} },
  };
  const { SendInputsService } = load('sendInputsService.ts', dependencies);
  const service = new SendInputsService({ Game: { getPlayer: () => player } }, {
    on: (...args) => events.on(...args), emitter,
  });
  const changed = () => events.emit('equip', { actor: player, baseObj: { getType: () => 26, getFormID: () => 0x12e46 } });
  return { service, emitter, changed, setCount(value) { count = value; }, send: () => service.sendEquipment() };
}

test('equipment waits for a valid inventory and retries without another equip event', () => {
  const h = equipmentHarness(), sent = [];
  h.emitter.on('sendMessageWithRefrId', e => sent.push(e.message));
  h.changed();
  for (const invalid of [-1, 0, 0x100000000, 1.5, NaN]) { h.setCount(invalid); h.send(); }
  assert.equal(sent.length, 0);
  h.setCount(1); h.send(); h.send();
  assert.equal(sent.length, 1);
  assert.equal(sent[0].data.numChanges, 1);
  assert.equal(sent[0].data.inv.entries[0].count, 1);
});

test('native send failure retains the equipment change and sequence for retry', () => {
  const h = equipmentHarness(), sent = [];
  h.changed(); h.setCount(1);
  h.emitter.once('sendMessageWithRefrId', () => { throw Error('native serialization failed'); });
  assert.throws(h.send, /native serialization failed/);
  h.emitter.on('sendMessageWithRefrId', e => sent.push(e.message));
  h.send();
  assert.equal(sent.length, 1);
  assert.equal(sent[0].data.numChanges, 1);
});

test('an equip event raised during send remains pending', () => {
  const h = equipmentHarness(), sent = [];
  h.changed(); h.setCount(1);
  h.emitter.once('sendMessageWithRefrId', h.changed);
  h.emitter.on('sendMessageWithRefrId', e => sent.push(e.message));
  h.send(); h.send(); h.send();
  assert.deepEqual(sent.map(m => m.data.numChanges), [1, 2]);
});

for (const direct of [true, false]) test(`manifest uses ${direct ? 'explicit server HTTP URL' : 'existing master API'}`, async () => {
  const requests = [], mods = [{ filename: 'Skyrim.esm', size: 123, crc32: 45 }];
  class HttpClient {
    constructor(base) { this.base = base; }
    async get(route) {
      requests.push({ base: this.base, route });
      return { status: 200, body: JSON.stringify({ versionMajor: 1, mods }) };
    }
  }
  const { SettingsService } = load('settingsService.ts', {
    skyrimPlatform: { HttpClient, printConsole() {}, Utility: { wait: async () => {} } },
    './clientListener': { ClientListener: class {} }, './authService': {},
    './timersService': {}, '../../logging': { logTrace() {} },
  });
  const config = { master: 'http://master.test/', 'server-master-key': 'test-realm' };
  if (direct) config['server-http-url'] = 'http://127.0.0.1:17778/';
  const service = new SettingsService({ settings: { 'skymp5-client': config } }, {});
  assert.equal(JSON.stringify(await service.getServerMods()), JSON.stringify(mods));
  assert.deepEqual(requests, [{
    base: direct ? 'http://127.0.0.1:17778' : 'http://master.test',
    route: direct ? '/manifest.json' : '/api/servers/test-realm/manifest.json',
  }]);
});
