"use strict";

const {isDeepStrictEqual} = require("node:util");

const statuses = ["started", "blocked", "error"];
const byText = (left, right) => left < right ? -1 : left > right ? 1 : 0;

// Keep the order of load lists and runtime traces. Sort only object keys.
function orderedValue(value) {
  if (Array.isArray(value)) return value.map(orderedValue);
  if (value && typeof value === "object") {
    return Object.fromEntries(Object.keys(value).sort(byText)
      .map(key => [key, orderedValue(value[key])]));
  }
  return value;
}

function indexRows(rows) {
  if (!Array.isArray(rows)) throw new Error("Expected quest startup rows");
  const indexed = new Map();
  for (const row of rows) {
    if (!row || typeof row.id !== "string" || !row.id.trim())
      throw new Error("Missing quest ID");
    if (indexed.has(row.id)) throw new Error(`Duplicate quest ID: ${row.id}`);
    if (!statuses.includes(row.status)) throw new Error(`Unknown startup status for ${row.id}`);
    if (typeof row.editorId !== "string" || typeof row.winningPlugin !== "string" || !row.winningPlugin)
      throw new Error(`Missing catalog metadata for ${row.id}`);
    if (typeof row.step !== "string" || !row.step || typeof row.code !== "string" ||
        (row.status !== "started" && !row.code) || typeof row.detail !== "string")
      throw new Error(`Missing startup observation for ${row.id}`);
    if (row.steps !== undefined && (!Array.isArray(row.steps) ||
        row.steps.some(step => typeof step !== "string" || !step)))
      throw new Error(`Invalid completed startup phases for ${row.id}`);
    indexed.set(row.id, row);
  }
  return indexed;
}

function groupByCause(quests, status) {
  const groups = new Map();
  for (const quest of quests) {
    if (quest.status !== status) continue;
    if (!groups.has(quest.code))
      groups.set(quest.code, {code: quest.code, count: 0, steps: new Set(), examples: []});
    const group = groups.get(quest.code);
    group.count++;
    group.steps.add(quest.step);
    if (group.examples.length < 3) group.examples.push({id: quest.id, editorId: quest.editorId});
  }
  return [...groups.values()]
    .map(group => ({...group, steps: [...group.steps].sort(byText)}))
    .sort((a, b) => b.count - a.count || byText(a.code, b.code));
}

function observation(row) {
  return row ? {status: row.status, step: row.step, code: row.code} : null;
}

function compare(previous, report) {
  if (previous?.kind !== "quest-startup" || previous.schemaVersion !== 1)
    throw new Error("Expected a version 1 quest startup baseline");
  for (const key of ["mode", "sources", "actorDialogue"]) {
    if (JSON.stringify(orderedValue(previous.provenance?.[key])) !==
        JSON.stringify(report.provenance[key]))
      throw new Error(`Cannot compare startup runs with different ${key}`);
  }
  const before = indexRows(previous.quests);
  const after = indexRows(report.quests);
  const counts = {unchanged: 0, startedAfterBlock: 0, blockedAfterStart: 0, advancedPastStep: 0, changedBlocker: 0,
    errorTransition: 0, added: 0, removed: 0};
  const changes = [];
  for (const id of [...new Set([...before.keys(), ...after.keys()])].sort(byText)) {
    const old = before.get(id);
    const now = after.get(id);
    if (old && now && old.fixture !== now.fixture)
      throw new Error(`Cannot compare different fixtures for ${id}`);
    let kind;
    if (!old) kind = "added";
    else if (!now) kind = "removed";
    else if (old.status === now.status && old.step === now.step && old.code === now.code)
      kind = "unchanged";
    else if (now.status === "error" || old.status === "error") kind = "errorTransition";
    else if (old.status === "started" && now.status === "blocked") kind = "blockedAfterStart";
    else if (old.status === "blocked" && now.status === "started") kind = "startedAfterBlock";
    else if (now.status === "blocked" && now.step !== old.step && now.steps?.includes(old.step))
      kind = "advancedPastStep";
    else if (now.status === "blocked") kind = "changedBlocker";
    else kind = "unchanged"; // Both completed startup.
    counts[kind]++;
    if (kind !== "unchanged")
      changes.push({id, kind, before: observation(old), after: observation(now)});
  }
  return {counts, changes};
}

function contrastFixtures(previous, report) {
  if (previous?.kind !== "quest-startup" || previous.schemaVersion !== 1 ||
      previous.provenance?.mode !== "forced-start" || report.provenance.mode !== "actor-dialogue")
    throw new Error("Fixture contrast requires a forced-start baseline and an actor-dialogue run");
  for (const key of ["sources", "catalogRecords", "excludedDeleted"]) {
    if (report.provenance[key] === undefined || JSON.stringify(orderedValue(previous.provenance[key])) !==
        JSON.stringify(report.provenance[key])) throw new Error(`Fixture contrast differs in ${key}`);
  }
  const before = indexRows(previous.quests);
  const after = report.quests;
  if (before.size !== after.length || after.length + report.provenance.excludedDeleted.length !== report.provenance.catalogRecords)
    throw new Error("Fixture contrast requires the same full catalog");
  const observations = [];
  const forcedControls = {changed: 0, unchanged: 0, changedIds: []};
  for (const row of after) {
    const old = before.get(row.id);
    if (!old || row.editorId !== old.editorId || row.winningPlugin !== old.winningPlugin)
      throw new Error(`Fixture contrast differs in winning quest ${row.id}`);
    if (row.fixture === "actor-dialogue") {
      observations.push({id: row.id, forced: {...observation(old), detail: old.detail},
        actorDialogue: {...observation(row), detail: row.detail}});
    } else {
      // A mixed run labels its forced rows. Compare all other evidence,
      // including phase order, stages and exact diagnostics.
      const {fixture, ...evidence} = row;
      if (isDeepStrictEqual(old, evidence)) forcedControls.unchanged++;
      else {
        forcedControls.changed++;
        forcedControls.changedIds.push(row.id);
      }
    }
  }
  return {forcedAddonSha256: previous.provenance.addonSha256, observations, forcedControls};
}

function buildStartupReport(rows, {previous = null, provenance = {}, contrast = null} = {}) {
  const quests = [...indexRows(rows).values()]
    .sort((a, b) => byText(a.id, b.id)).map(orderedValue);
  const summary = {total: quests.length, started: 0, blocked: 0, error: 0};
  for (const quest of quests) summary[quest.status]++;
  const report = {schemaVersion: 1, kind: "quest-startup", provenance: orderedValue(provenance),
    summary, blockers: groupByCause(quests, "blocked"), errors: groupByCause(quests, "error"), quests};
  if (provenance.mode === "actor-dialogue") {
    report.fixtures = {};
    for (const name of ["actor-dialogue", "forced-start"])
      report.fixtures[name] = {total: 0, started: 0, blocked: 0, error: 0};
    for (const row of quests) {
      if (!Object.hasOwn(report.fixtures, row.fixture)) throw new Error(`Unknown fixture for ${row.id}`);
      report.fixtures[row.fixture].total++;
      report.fixtures[row.fixture][row.status]++;
    }
  }
  if (previous !== null) report.comparison = compare(previous, report);
  if (contrast !== null) report.fixtureContrast = contrastFixtures(contrast, report);
  return report;
}

const tableCell = value => String(value).replace(/[\r\n]+/g, " ").replaceAll("|", "\\|");

function renderStartupMarkdown(report) {
  const {summary} = report;
  const lines = ["# Quest startup results", "",
    `Attempted **${summary.total}** quest records. **${summary.started}** completed the explicit startup probe, ` +
    `**${summary.blocked}** reached a reported blocker, and **${summary.error}** returned a test error.`,
    "", "Each attempt requests a first start in a fresh campaign and rolls back its changes. Natural triggers, later quest branches and multiplayer " +
    "playthroughs require separate tests."];
  if (report.fixtures) {
    lines.push("", "## Startup fixtures", "",
      "Every winning QUST with an ADIA event uses Hulda and Mikael as ordered dialogue participants " +
      "in the campaign owner's private Bannered Mare instance. " +
      "All other winning quests use the ordinary forced-start probe.", "",
      "| Fixture | Attempts | Completed startup | Blocked | Error |", "| --- | ---: | ---: | ---: | ---: |");
    for (const [name, counts] of Object.entries(report.fixtures))
      lines.push(`| ${name} | ${counts.total} | ${counts.started} | ${counts.blocked} | ${counts.error} |`);
  }
  if (report.fixtureContrast) {
    const {forcedAddonSha256, observations, forcedControls} = report.fixtureContrast;
    lines.push("", "## Forced startup and explicit actor dialogue", "",
      `The same **${observations.length}** ADIA quests were attempted under both fixtures. ` +
      "These observations describe coverage under different inputs. " +
      "Measuring runtime progress requires rerunning the same fixture.", "");
    if (forcedControls)
      lines.push(`Among forced-start controls, **${forcedControls.unchanged}** had identical evidence ` +
        `and **${forcedControls.changed}** changed.`, "");
    if (forcedAddonSha256 !== report.provenance.addonSha256)
      lines.push("The two runs also used different addon builds.", "");
    const pairs = new Map();
    const outcome = row => row.code || row.status;
    for (const {forced, actorDialogue} of observations) {
      const pair = `| ${tableCell(outcome(forced))} | ${tableCell(outcome(actorDialogue))}`;
      pairs.set(pair, (pairs.get(pair) ?? 0) + 1);
    }
    lines.push("| Forced startup | Actor dialogue | Quests |", "| --- | --- | ---: |");
    for (const [pair, count] of [...pairs].sort((a, b) => b[1] - a[1] || byText(a[0], b[0])))
      lines.push(`${pair} | ${count} |`);
  }
  const completed = report.quests.filter(quest => quest.status === "started");
  if (completed.length && completed.every(quest => Array.isArray(quest.stages))) {
    const withStage = completed.filter(quest => quest.stages.length > 0).length;
    lines.push("", `Of the completed probes, **${withStage}** executed at least one authored startup stage. ` +
      `**${completed.length - withStage}** completed without executing a stage.`);
  }
  for (const [title, groups] of [["First startup blockers", report.blockers], ["Test errors", report.errors]]) {
    lines.push("", `## ${title}`, "");
    if (!groups.length) {
      lines.push("None observed.");
      continue;
    }
    lines.push("| Cause | Quests | Steps | Example quests |", "| --- | ---: | --- | --- |");
    for (const group of groups) {
      const examples = group.examples.map(example => example.editorId || example.id);
      lines.push(`| ${tableCell(group.code)} | ${group.count} | ${tableCell(group.steps.join(", "))} | ${tableCell(examples.join(", "))} |`);
    }
  }
  // Different Story Manager events need different inputs. Keep the common
  // admission blocker grouped above and show its actual triggers here.
  const events = new Map();
  for (const quest of report.quests) {
    if (quest.status === "blocked" && quest.code === "story.event")
      events.set(quest.detail, (events.get(quest.detail) ?? 0) + 1);
  }
  if (events.size) {
    lines.push("", "## Story event requests", "", "| Diagnostic | Quests |", "| --- | ---: |");
    for (const [detail, count] of [...events].sort((a, b) => b[1] - a[1] || byText(a[0], b[0])))
      lines.push(`| ${tableCell(detail)} | ${count} |`);
  }
  if (report.comparison) {
    const {counts} = report.comparison;
    lines.push("", "## Changes from the previous run", "",
      "| Outcome | Quests |", "| --- | ---: |",
      `| Started after previously blocking | ${counts.startedAfterBlock} |`,
      `| Blocked after previously starting | ${counts.blockedAfterStart} |`,
      `| Completed the previously blocked phase and reached another blocker | ${counts.advancedPastStep} |`,
      `| First blocker changed without evidence the old phase completed | ${counts.changedBlocker} |`,
      `| Changed result involving a test error | ${counts.errorTransition} |`,
      `| Added to the run | ${counts.added} |`, `| Omitted from the run | ${counts.removed} |`,
      `| Same outcome | ${counts.unchanged} |`, "",
      "A changed blocker can reflect a different path. The JSON records the previous and current observations for each change.");
  }
  return lines.join("\n") + "\n";
}

module.exports = {buildStartupReport, renderStartupMarkdown};
