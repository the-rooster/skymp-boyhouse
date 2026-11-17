const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const root = path.resolve(__dirname, '../../skymp5-client');
const ts = require(require.resolve('typescript', { paths: [root, path.resolve(root, '../skymp5-server')] }));
const moduleUnderTest = { exports: {} };
vm.runInNewContext(ts.transpileModule(fs.readFileSync(path.join(root, 'src/sync/campaignFactions.ts'), 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
}).outputText, { module: moduleUnderTest, exports: moduleUnderTest.exports });
const { campaignFactionRanks, campaignFactionsFromBaseline } = moduleUnderTest.exports;
const plain = value => JSON.parse(JSON.stringify(value));

test('faction transport keeps missing, empty, negative ranks and stable IDs distinct', () => {
  assert.equal(campaignFactionsFromBaseline({ customPropsJsonDumps: [] }), undefined);
  assert.deepEqual(plain(campaignFactionsFromBaseline({ customPropsJsonDumps: [
    { propName: 'campaignFactions', propValueJsonDump: '[]' },
  ] })), []);
  const entries = [{ factionId: 0x1000123, rank: -128, isMember: true }, { factionId: 0x456, rank: -2, isMember: true },
    { factionId: 0x123, rank: -1, isMember: false }, { factionId: 0x789, rank: 127, isMember: true }];
  assert.deepEqual(plain(campaignFactionRanks(entries)), [entries[2], entries[1], entries[3], entries[0]]);
  assert.deepEqual(entries.map(entry => entry.rank), [-128, -2, -1, 127]);
});

test('faction transport refuses malformed full snapshots before native planning', () => {
  const entry = overrides => ({ factionId: 1, rank: 0, isMember: true, ...overrides });
  for (const value of [undefined, null, {}, [entry({ factionId: 0 })], [entry({ rank: 128 })],
    [entry({ rank: -129 })], [entry({ rank: NaN })], [entry({ rank: 0.5 })],
    [entry({ extra: true })], [entry({}), entry({ rank: 1 })], [entry({ isMember: 1 })],
    [{ factionId: 1, rank: 0 }], Array(1), Array.from({ length: 4097 }, (_, index) => entry({ factionId: index + 1 }))]) {
    assert.throws(() => campaignFactionRanks(value));
  }
  assert.throws(() => campaignFactionsFromBaseline({ customPropsJsonDumps: [
    { propName: 'campaignFactions', propValueJsonDump: '[]' },
    { propName: 'campaignFactions', propValueJsonDump: '[]' },
  ] }));
  assert.throws(() => campaignFactionsFromBaseline({ customPropsJsonDumps: [
    { propName: 'campaignFactions', propValueJsonDump: 'broken' },
  ] }));
});
