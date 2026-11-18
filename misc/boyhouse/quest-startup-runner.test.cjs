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
        this.actors = new Map();
        this.settings = JSON.parse(settings);
        assert.equal(this.settings.listenHost, "127.0.0.1");
        assert.equal(this.settings.dataDir, process.cwd());
      }
      _setSelf() {}
      getEspmLoadOrder() { return ${JSON.stringify(masters)}; }
      lookupEspmRecordById(id) { return {fileIndex: id === 1 ? 1 : 2,
        record: {type:"QUST", flags:0, editorId:id === 1 ? "First" : "Second",
          fields: [{type:"ENAM",data:Buffer.from(id === 1 ? "ADIA" : "SCPT")}]}}; }
      createActor(actor, pos, angle, cell) {
        assert.equal(this.actors.size, 0);
        if (this.actors.size === 0) this.actor = actor;
        this.actors.set(actor, {cell});
      }
      set(actor, key, value) {
        const record = this.actors.get(actor);
        if (key === "worldInstance") {
          assert.equal(value, "campaign:" + this.actor + ":startup");
          record.phase = value;
        } else if (key === "campaignReferences") {
          assert.equal(actor, this.actor);
          assert.deepEqual(value, {references:[0x1a66e,0x1a671],encounterLevel:1});
          this.manifest = {schemaVersion:1,actor,campaignOwner:actor,identity:'a'.repeat(32),
            instance:record.phase,references:[]};
          for (const [index, original] of value.references.entries()) {
            const base = [0x13ba3,0x1a670][index], copy = actor + index + 1;
            this.actors.set(copy, {base,cell:0x1605e,phase:record.phase});
            this.manifest.references.unshift({original,baseId:base,kind:"actor",copy,deleted:false,disabled:false});
          }
        } else if (key === "private.boyhouse.questAdapters") {
          assert.equal(actor, this.actor); this.adapters = value;
        }
        else throw Error("Unexpected runtime write");
      }
      getDescFromId(id) { return String(id); }
      get(id,key) {
        if (key === "worldInstance") return this.actors.get(id).phase;
        if (key === "campaignReferences") return structuredClone(this.manifest);
        return null;
      }
      getActorCellOrWorld(id) { return this.actors.get(id).cell; }
      destroyActor(actor) { assert.ok(this.actors.delete(actor)); if (!this.actors.size) this.actor = undefined; }
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

test("actor-dialogue CLI selects winners by ENAM, validates both actors and contrasts the same catalog", t => {
  const dir = fixture(t);
  addon(dir, `probeQuestStartup(actor, id, first, second) {
    assert.deepEqual(this.adapters, [String(id)]);
    if (first === undefined) {
      assert.equal(this.actors.size, 1);
      assert.equal(this.actors.get(actor).cell, 0x3c);
      return {status:"blocked",step:"admission",steps:[],stages:[],blocker:{
        code:"story.event",detail:"Quest requires story event " + (id === 1 ? "ADIA" : "SCPT")}};
    }
    assert.equal(id, 1);
    assert.equal(this.actors.size, 3);
    assert.equal(new Set([actor, first, second]).size, 3);
    assert.equal(this.actors.get(actor).cell, 0x1605e);
    assert.deepEqual(this.actors.get(first), {...this.actors.get(actor),base:0x13ba3});
    assert.deepEqual(this.actors.get(second), {...this.actors.get(actor),base:0x1a670});
    return {status:"blocked",step:"aliases",steps:["admission"],stages:[],blocker:{code:"alias.fill",detail:"Alias 2"},
      storyEvent:{type:"ADIA",actor1:first,actor2:second,location:0x1f86d},
      storyRoute:{candidateCount:1,nodeIds:[0x123,0x456],questIndex:0},
        storySelection:{outcome:"attempt",nodeIds:[0x123,0x456],nodeId:0x456,questId:id,questIndex:0,detail:""},
        storyAliasBindings:[{id:3,member:"R1",formId:0x1a66e}],
        storyUniqueActorBindings:[{id:4,baseId:0x13ba3,formId:0x1a66e,reused:true}]};
  }`);
  const forced = run(dir, "--output", "forced.json");
  assert.equal(forced.status, 0, forced.stderr);
  const mixed = run(dir, "--scenario", "actor-dialogue", "--contrast", "forced.json");
  assert.equal(mixed.status, 0, mixed.stderr);
  const report = JSON.parse(fs.readFileSync(path.join(dir, "out.json")));
  assert.equal(report.provenance.mode, "actor-dialogue");
  assert.equal(report.provenance.actorDialogue.winningQuestEvent, "ADIA");
  assert.deepEqual(report.fixtures, {
    "actor-dialogue": {total: 1, started: 0, blocked: 1, error: 0},
    "forced-start": {total: 1, started: 0, blocked: 1, error: 0},
  });
  const story = report.quests.find(q => q.fixture === "actor-dialogue");
  assert.deepEqual(story.storyEvent, {type:"ADIA",actor1Base:0x13ba3,actor2Base:0x1a670,
    actor1Original:0x1a66e,actor2Original:0x1a671,cell:0x1605e,location:0x1f86d});
  assert.equal(report.provenance.actorDialogue.placement, "authored");
  assert.deepEqual(story.storyRoute, {candidateCount:1,nodeIds:[0x123,0x456],questIndex:0});
  assert.deepEqual(story.storyAliasBindings, [{id:3,member:"R1",formId:0x1a66e}]);
  assert.deepEqual(story.storyUniqueActorBindings, [{id:4,baseId:0x13ba3,formId:0x1a66e,reused:true}]);
  assert.deepEqual(story.storySelection, {outcome:"attempt",nodeIds:[0x123,0x456],nodeId:0x456,
    questId:1,questIndex:0,detail:""});
  assert.deepEqual(report.fixtureContrast.forcedControls, {unchanged:1,changed:0,changedIds:[]});
  assert.equal(report.fixtureContrast.observations.length, 1);
  const pair = report.fixtureContrast.observations[0];
  assert.equal(pair.id, story.id);
  assert.equal(pair.forced.code, "story.event");
  assert.equal(pair.actorDialogue.code, "alias.fill");
  assert.equal(report.comparison, undefined);
  assert.match(fs.readFileSync(path.join(dir, "out.md"), "utf8"), /\| story\.event \| alias\.fill \| 1 \|/);
  const incompatible = run(dir, "--scenario", "actor-dialogue", "--compare", "forced.json");
  assert.equal(incompatible.status, 1);
  assert.match(incompatible.stderr, /different mode/);
  const baseline = JSON.parse(fs.readFileSync(path.join(dir, "forced.json")));
  baseline.quests.pop();
  fs.writeFileSync(path.join(dir, "partial.json"), JSON.stringify(baseline));
  const partial = run(dir, "--scenario", "actor-dialogue", "--contrast", "partial.json");
  assert.equal(partial.status, 1);
  assert.match(partial.stderr, /same full catalog/);
});

test("older addons silently ignoring the event argument cannot publish actor-dialogue evidence", t => {
  const dir = fixture(t);
  addon(dir, `probeQuestStartup() {
    return {status:"blocked",step:"admission",steps:[],stages:[],
      blocker:{code:"story.event",detail:"Quest requires story event ADIA"}};
  }`);
  const result = run(dir, "--scenario", "actor-dialogue");
  assert.equal(result.status, 1);
  assert.match(result.stderr, /Expected native ADIA evidence/);
  assert.equal(fs.existsSync(path.join(dir, "out.json")), false);
});

test("forged or changed authored actor receipts abort the report", t => {
  const dir = fixture(t);
  for (const [members, diagnostic] of [
    [`get(id,key) {
      if (key === "campaignReferences") {
        const snapshot = structuredClone(this.manifest);
        snapshot.references[0].original = snapshot.references[1].original;
        return snapshot;
      }
      return key === "worldInstance" ? this.actors.get(id).phase : null;
    }`, /Invalid dialogue NPC copy receipt/],
    [`probeQuestStartup() {
      this.manifest.references[0].disabled = true;
      throw Error("Native probe failure");
    }`, /changed dialogue copy receipts/],
    [`probeQuestStartup(owner, id, first, second) {
      return {status:"blocked",step:"admission",steps:[],stages:[],blocker:{code:"story.event",detail:"test"},
        storyEvent:{type:"ADIA",actor1:first,actor2:second,location:0x1f86d,actor1Original:0x1a671,actor2Original:0x1a66e}};
    }`, /disagrees with the campaign copy receipts/],
  ]) {
    addon(dir, "probeQuestStartup() {}\n" + members);
    const result = run(dir, "--scenario", "actor-dialogue");
    assert.equal(result.status, 1);
    assert.match(result.stderr, diagnostic);
    assert.equal(fs.existsSync(path.join(dir, "out.json")), false);
  }
});

test("successful dialogue startup must match the candidate chosen by the event", t => {
  const dir = fixture(t);
  addon(dir, `probeQuestStartup(owner, id, first, second) {
    const result = {status:"startup-exercised",step:"done",steps:["quest-admission"],stages:[],blocker:null};
    if (first !== undefined) {
      result.storyEvent = {type:"ADIA",actor1:first,actor2:second,location:0x1f86d};
      result.storySelection = {outcome:"attempt",nodeIds:[5],nodeId:5,questIndex:0,questId:id + 1,detail:""};
    }
    return result;
  }`);
  const result = run(dir, "--scenario", "actor-dialogue");
  assert.equal(result.status, 1);
  assert.match(result.stderr, /ordered selection of the requested quest/);
});

test("worker crashes retain their attempted fixture and the mixed run resumes with the next winner", t => {
  const dir = fixture(t);
  addon(dir, `probeQuestStartup(actor, id, partner) {
    if (partner !== undefined) process.exit(42);
    return {status:"startup-exercised",step:"done",steps:[],stages:[],blocker:null};
  }`);
  const result = run(dir, "--scenario", "actor-dialogue");
  assert.equal(result.status, 1);
  const report = JSON.parse(fs.readFileSync(path.join(dir, "out.json")));
  assert.equal(report.fixtures["actor-dialogue"].error, 1);
  assert.equal(report.fixtures["forced-start"].started, 1);
  assert.equal(report.quests.find(q => q.code === "probe.crash").storyEvent, undefined);
});

test("ADIA route evidence with no selected route has only a candidate count", t => {
  const dir = fixture(t);
  for (const [route, valid] of [
    [{candidateCount:0}, true], [{candidateCount:2}, true],
    [{candidateCount:1}, false], [{candidateCount:0,nodeIds:[1,2]}, false],
    [{candidateCount:1,nodeIds:[1,-2],questIndex:0}, false],
  ]) {
    addon(dir, `probeQuestStartup(actor,id,first,second) {
      const result = {status:"blocked",step:"story-route",steps:["admission"],stages:[],
        blocker:{code:"story.route-missing",detail:"No route"}};
      if (first !== undefined) {
        result.storyEvent = {type:"ADIA",actor1:first,actor2:second,location:0x1f86d};
        result.storyRoute = ${JSON.stringify(route)};
      }
      return result;
    }`);
    const result = run(dir, "--scenario", "actor-dialogue");
    assert.equal(result.status, valid ? 0 : 1, result.stderr);
    if (valid) {
      const report = JSON.parse(fs.readFileSync(path.join(dir, "out.json")));
      assert.deepEqual(report.quests.find(row => row.fixture === "actor-dialogue").storyRoute, route);
    } else assert.match(result.stderr, /story route/);
  }
});
