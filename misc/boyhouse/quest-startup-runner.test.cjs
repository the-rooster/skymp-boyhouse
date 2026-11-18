"use strict";

const assert = require("node:assert/strict");
const { spawnSync } = require("node:child_process");
const { createHash } = require("node:crypto");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const test = require("node:test");
const { selectQuests, verifySources } = require("./quest-startup-runner.cjs");

const masters = ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"];
const file = path.join(__dirname, "quest-startup-runner.cjs");
function catalog() {
  const quests = [
    { id: "Skyrim.esm:000001", winningPlugin: "Update.esm", editorId: "First", status: "untested" },
    { id: "Dawnguard.esm:000002", winningPlugin: "Dawnguard.esm", editorId: "Second", status: "untested" },
    { id: "Skyrim.esm:000003", editorId: "Deleted", status: "excluded-deleted" },
  ];
  const source = name => ({ file: name, sha256: createHash("sha256").update(name).digest("hex") });
  return { schemaVersion: 1, summary: { questRecords: 3, deletedQuestRecords: 1 }, quests,
    sources: masters.map(source), scripts: { sources: [source("Scripts.bsa")] } };
}

function fixture(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "skymp-startup-test-"));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  for (const name of [...masters, "Scripts.bsa"]) fs.writeFileSync(path.join(dir, name), name);
  fs.writeFileSync(path.join(dir, "catalog.json"), JSON.stringify(catalog()));
  return dir;
}

test("catalog form IDs use the originating master and count every excluded record", () => {
  const parsed = selectQuests(catalog());
  assert.deepEqual(parsed.quests.map(row => row.formId), [1, 0x02000002]);
  assert.deepEqual(parsed.deleted, ["Skyrim.esm:000003"]);
  for (const modify of [
    c => c.quests.pop(), c => { c.quests[1].id = c.quests[0].id; },
    c => { c.quests[1].winningPlugin = "Skyrim.esm"; }, c => { c.quests[0].id = "Skyrim.esm:000000"; },
    c => { c.quests[0].status = "excluded-deleted"; }, c => { c.quests[0].id = "Unknown.esm:000001"; },
  ]) {
    const invalid = catalog(); modify(invalid);
    assert.throws(() => selectQuests(invalid));
  }
});

test("the driver reads the same winning masters and script archives as the catalog", async t => {
  const dir = fixture(t);
  const current = catalog();
  assert.equal((await verifySources(current, dir)).archives[0], path.join(dir, "Scripts.bsa"));
  fs.writeFileSync(path.join(dir, "Scripts.bsa"), "replacement");
  await assert.rejects(verifySources(current, dir), /Game data differs/);
  current.sources.reverse();
  await assert.rejects(verifySources(current, dir), /master load order/);
});

// Run the real coordinator and child lifecycle with a tiny instrumented addon.
// The owned-data verification invokes the same entry point with scam_native.node.
function addon(dir, body) {
  fs.writeFileSync(path.join(dir, "addon.cjs"), `
    const assert = require("node:assert/strict");
    exports.ScampServer = class {
      constructor(settings) {
        this.settings = JSON.parse(settings);
        assert.equal(this.settings.listenHost, "127.0.0.1");
        assert.equal(this.settings.dataDir, process.cwd());
      }
      _setSelf() {}
      getEspmLoadOrder() { return ${JSON.stringify(masters)}; }
      lookupEspmRecordById(id) { return {fileIndex: id === 1 ? 1 : 2,
        record: {type:"QUST", flags:0, editorId:id === 1 ? "First" : "Second"}}; }
      createActor(actor) { assert.equal(this.actor, undefined); this.actor = actor; }
      set(actor, key, value) {
        assert.equal(actor, this.actor);
        if (key === "worldInstance") assert.equal(value, "campaign:" + actor + ":startup");
        else if (key === "private.boyhouse.questAdapters") this.adapters = value;
        else throw Error("Unexpected runtime write");
      }
      getDescFromId(id) { return String(id); }
      get() { return null; }
      destroyActor(actor) { assert.equal(actor, this.actor); this.actor = undefined; }
      ${body}
    };`);
}

function run(dir, ...extra) {
  return spawnSync(process.execPath, [file, "--catalog", "catalog.json", "--output", "out.json",
    "--markdown", "out.md", "--data-dir", dir, "--addon", "addon.cjs", ...extra],
  { cwd: dir, encoding: "utf8", timeout: 30000 });
}

test("bulk CLI exercises every active winner, keeps errors separate and compares two runs", t => {
  const dir = fixture(t);
  addon(dir, `probeQuestStartup(actor, id) {
    assert.equal(actor, this.actor); assert.deepEqual(this.adapters, [String(id)]);
    if (id === 1) return {status:"blocked",step:"aliases",steps:[],stages:[],
      blocker:{code:"alias.fill",detail:"requires an alias"}};
    return {status:"startup-exercised",step:"done",steps:["aliases"],stages:[0],blocker:null};
  }`);
  const first = run(dir);
  assert.equal(first.status, 0, first.stderr);
  const read = () => JSON.parse(fs.readFileSync(path.join(dir, "out.json"), "utf8"));
  assert.deepEqual(read().summary, { total: 2, started: 1, blocked: 1, error: 0 });
  assert.deepEqual(read().provenance.excludedDeleted, ["Skyrim.esm:000003"]);
  fs.copyFileSync(path.join(dir, "out.json"), path.join(dir, "baseline.json"));
  const collision = run(dir, "--compare", "out.json");
  assert.equal(collision.status, 1);
  assert.match(collision.stderr, /baseline/);
  assert.deepEqual(read().summary, { total: 2, started: 1, blocked: 1, error: 0 });
  addon(dir, `probeQuestStartup(actor, id) {
    if (id === 1) process.exit(42);
    throw Error("unclassified runtime failure");
  }`);
  const second = run(dir, "--compare", "baseline.json");
  assert.equal(second.status, 1, second.stderr);
  const report = read();
  assert.deepEqual(report.summary, { total: 2, started: 0, blocked: 0, error: 2 });
  assert.deepEqual(report.errors.map(group => group.code), ["probe.crash", "probe.exception"]);
  assert.equal(report.comparison.counts.errorTransition, 2);
});

test("missing probe support aborts the run before publishing a report", t => {
  const dir = fixture(t);
  addon(dir, "");
  const result = run(dir);
  assert.equal(result.status, 1);
  assert.match(result.stderr, /Rebuild addon/);
  assert.equal(fs.existsSync(path.join(dir, "out.json")), false);
});
