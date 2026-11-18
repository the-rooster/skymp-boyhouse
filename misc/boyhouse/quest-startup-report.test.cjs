const { test } = require("node:test");
const assert = require("node:assert/strict");
const { buildStartupReport, renderStartupMarkdown } = require("./quest-startup-report.cjs");

function quest(n, status = "blocked", code = "alias.fill", fields = {}) {
  return {id: `Skyrim.esm:${String(n).padStart(6, "0")}`, editorId: `Quest${n}`,
    winningPlugin: "Update.esm", status, step: "aliases", code, detail: "Alias 7", ...fields};
}

test("first blockers use stable cause codes; test failures and completed startups stay separate", () => {
  const rows = [1, 2, 3, 4].map(n => quest(n, "blocked", "alias.fill", {detail: `Alias ${n}`}));
  rows[0].editorId = "";
  rows.push(quest(5, "started", ""), quest(6, "error", "alias.fill"),
    quest(7, "blocked", "stage.run", {step: "stage|fragment"}));
  const report = buildStartupReport(rows);
  assert.deepEqual(report.summary, {total: 7, started: 1, blocked: 5, error: 1});
  assert.deepEqual(report.blockers.map(g => [g.code, g.count]), [["alias.fill", 4], ["stage.run", 1]]);
  assert.equal(report.blockers[0].examples.length, 3);
  assert.equal(report.errors[0].count, 1);
  assert.equal(report.quests[3].detail, "Alias 4");
  const markdown = renderStartupMarkdown(report);
  assert.match(markdown, /Skyrim\.esm:000001/);
  assert.ok(markdown.includes("stage\\|fragment"));
  assert.match(markdown, /multiplayer playthroughs require separate tests/);
});

test("quest ordering, cause ties, nested row evidence and provenance serialize deterministically", () => {
  const a = quest(2, "blocked", "z", {evidence: {first: 1, second: [9, 2]}});
  const b = quest(1, "blocked", "a");
  const forward = buildStartupReport([a, b], {provenance: {z: 1, a: 2}});
  const reverse = buildStartupReport([b, {...a, evidence: {second: [9, 2], first: 1}}],
    {provenance: {a: 2, z: 1}});
  assert.equal(JSON.stringify(forward), JSON.stringify(reverse));
  assert.equal(renderStartupMarkdown(forward), renderStartupMarkdown(reverse));
  assert.deepEqual(forward.blockers.map(group => group.code), ["a", "z"]);
  assert.deepEqual(forward.quests[1].evidence.second, [9, 2]);
});

test("comparison distinguishes reaching startup from changing blockers, errors and coverage", () => {
  const previous = buildStartupReport([quest(1), quest(2, "started", ""), quest(3),
    quest(4, "error", "io"), quest(5), quest(6)]);
  const report = buildStartupReport([quest(1, "started", ""), quest(2), quest(3, "blocked", "script.event"),
    quest(4, "blocked", "alias.fill"), quest(5, "blocked", "alias.fill", {detail: "more information"}),
    quest(7)], {previous});
  assert.deepEqual(report.comparison.counts, {unchanged: 1, startedAfterBlock: 1, advancedPastStep: 0,
    blockedAfterStart: 1, changedBlocker: 1, errorTransition: 1, added: 1, removed: 1});
  assert.deepEqual(report.comparison.changes[2], {id: quest(3).id, kind: "changedBlocker",
    before: {status: "blocked", step: "aliases", code: "alias.fill"},
    after: {status: "blocked", step: "aliases", code: "script.event"}});
  assert.match(renderStartupMarkdown(report), /First blocker changed without evidence.* \| 1/);
});

test("advancement requires a completed phase trace and a different blocking phase", () => {
  const previous = buildStartupReport([quest(1), quest(2), quest(3)]);
  const report = buildStartupReport([
    quest(1, "blocked", "script.event", {step: "scripts", steps: ["record", "aliases"]}),
    quest(2, "blocked", "script.event", {step: "scripts", steps: ["record"]}),
    quest(3, "blocked", "alias.effect", {steps: ["record"]}),
  ], {previous});
  assert.equal(report.comparison.counts.advancedPastStep, 1);
  assert.equal(report.comparison.counts.changedBlocker, 2);
  assert.equal(report.comparison.changes[0].before.code, "alias.fill");
  assert.equal(report.comparison.changes[0].after.code, "script.event");
});

test("baseline comparisons require matching content fingerprints, load order, and probe mode", () => {
  const sources = [{file: "Skyrim.esm", sha256: "1"}, {file: "Update.esm", sha256: "2"}];
  const provenance = {mode: "forced-start", sources};
  const previous = buildStartupReport([quest(1)], {provenance});
  assert.equal(buildStartupReport([quest(1)], {previous, provenance}).comparison.counts.unchanged, 1);
  assert.throws(() => buildStartupReport([], {previous, provenance: {...provenance, sources: [...sources].reverse()}}), /sources/);
  assert.throws(() => buildStartupReport([], {previous, provenance: {...provenance, mode: "natural"}}), /mode/);
  assert.throws(() => buildStartupReport([], {previous: {kind: "quest-startup", schemaVersion: 0}}), /version/);
});

test("malformed rows and duplicates in either run fail before publishing totals", () => {
  assert.throws(() => buildStartupReport([quest(1), quest(1)]), /Duplicate quest ID/);
  assert.throws(() => buildStartupReport([quest(1, "playable")]), /status/);
  assert.throws(() => buildStartupReport([quest(1, "blocked", "")]), /observation/);
  const previous = buildStartupReport([quest(2)]);
  previous.quests.push(quest(2));
  assert.throws(() => buildStartupReport([quest(1)], {previous}), /Duplicate quest ID/);
});

test("stage evidence distinguishes active stage execution from stage-free startup", () => {
  const report = buildStartupReport([
    quest(1, "started", "", {stages: [0]}),
    quest(2, "started", "", {stages: []}),
    quest(3, "blocked", "scene.begin-on-quest-start", {stages: [10]}),
  ]);
  assert.match(renderStartupMarkdown(report), /\*\*1\*\* executed at least one authored startup stage. \*\*1\*\* completed without executing a stage/);
});

test("story event blockers show which event fixture will reach the most quests", () => {
  const rows = ["SCPT", "ADIA", "ADIA"].map((tag, i) => quest(i, "blocked", "story.event",
    {step: "admission", detail: `Quest requires story event ${tag}`}));
  const markdown = renderStartupMarkdown(buildStartupReport(rows));
  assert.ok(markdown.includes("| Quest requires story event ADIA | 2 |\n| Quest requires story event SCPT | 1 |"));
});
