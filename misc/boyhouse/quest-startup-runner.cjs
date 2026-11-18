"use strict";

// Each worker opens the native addon in its own temporary directory. A new
// campaign owns each attempt. Recycling the process also releases native
// caches and contains a failed or non-returning probe.
const assert = require("node:assert/strict");
const { fork } = require("node:child_process");
const { createHash, randomUUID } = require("node:crypto");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { parseArgs } = require("node:util");
const { buildStartupReport, renderStartupMarkdown } = require("./quest-startup-report.cjs");

const masters = ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"];
const batchSize = 64;
const defaultAddon = path.resolve(__dirname, "../../build/dist/server/scam_native.node");
const journal = "private.boyhouse.questJournal";
const banneredMareCell = 0x1605e;
const banneredMareLocation = 0x1f86d;
const dialogueNpcs = [{original: 0x1a66e, base: 0x13ba3}, {original: 0x1a671, base: 0x1a670}]; // Hulda, Mikael
const dialogueIdentities = {actor1Original: dialogueNpcs[0].original, actor2Original: dialogueNpcs[1].original,
  actor1Base: dialogueNpcs[0].base, actor2Base: dialogueNpcs[1].base};

function selectQuests(catalog) {
  assert.equal(catalog.schemaVersion, 1, "Expected version 1 of the dependency catalog");
  assert.ok(Array.isArray(catalog.quests) && catalog.quests.length > 0, "Empty quest catalog");
  assert.equal(catalog.summary.questRecords, catalog.quests.length, "Incomplete quest catalog");
  const quests = [], deleted = [], seen = new Set();
  for (const entry of catalog.quests) {
    const { id, editorId, winningPlugin, status } = entry;
    const match = typeof id === "string" && /^([^:]+):([0-9A-F]{6})$/.exec(id);
    if (!match || !masters.includes(match[1]) || typeof editorId !== "string")
      throw new Error(`Invalid catalog quest: ${id}`);
    if (seen.has(id)) throw new Error(`Duplicate catalog quest: ${id}`);
    seen.add(id);
    const formId = masters.indexOf(match[1]) * 0x1000000 + parseInt(match[2], 16);
    if (!formId) throw new Error("Null catalog quest ID");
    if (status === "excluded-deleted") {
      deleted.push(id);
      continue;
    }
    if (status !== "untested" || masters.indexOf(winningPlugin) < masters.indexOf(match[1]))
      throw new Error(`Invalid winning quest: ${id}`);
    quests.push({ id, editorId, winningPlugin, formId });
  }
  assert.equal(deleted.length, catalog.summary.deletedQuestRecords, "Deleted quest count mismatch");
  return { quests, deleted: deleted.sort() };
}

async function hashFile(filename) {
  const hash = createHash("sha256");
  for await (const chunk of fs.createReadStream(filename)) hash.update(chunk);
  return hash.digest("hex");
}

async function verifySources(catalog, dataDir) {
  assert.deepEqual(catalog.sources.map(source => source.file), masters, "Unexpected master load order");
  const scripts = catalog.scripts.sources;
  assert.ok(Array.isArray(scripts) && scripts.length, "No script archives in dependency catalog");
  const sources = [], paths = [];
  for (const source of [...catalog.sources, ...scripts]) {
    const { file, sha256 } = source;
    if (typeof file !== "string" || /[\\/:]/.test(file) ||
        !(masters.includes(file) || /\.bsa$/i.test(file)) || source.kind === "loose" ||
        !/^[a-f0-9]{64}$/.test(sha256))
      throw new Error(`Expected a fingerprinted master or archive in the data directory: ${file}`);
    const filename = path.join(dataDir, file);
    if (paths.includes(filename)) throw new Error(`Duplicate game-data source: ${file}`);
    assert.equal(await hashFile(filename), sha256, `Game data differs from the catalog: ${file}`);
    sources.push({ file, sha256 });
    paths.push(filename);
  }
  return { sources, loadOrder: paths.slice(0, masters.length), archives: paths.slice(masters.length) };
}

function errorRow(quest, code, detail) {
  return { id: quest.id, editorId: quest.editorId, winningPlugin: quest.winningPlugin,
    status: "error", step: "probe", code, detail };
}

function selectFixture(server, quest, scenario) {
  const { record, fileIndex } = server.lookupEspmRecordById(quest.formId);
  if (record?.type !== "QUST" || (record.flags & 0x20) || record.editorId !== quest.editorId ||
      masters[fileIndex] !== quest.winningPlugin)
    throw new Error(`Native winning record differs from the catalog: ${quest.id}`);
  if (scenario === "forced-start") return "forced-start";
  assert.equal(scenario, "actor-dialogue", "Unknown startup scenario");
  assert.ok(Array.isArray(record.fields), "Missing winning QUST fields");
  const events = record.fields.filter(field => field.type === "ENAM");
  if (!events.length) return "forced-start";
  if (events.length !== 1 || !(events[0].data instanceof Uint8Array) || events[0].data.length !== 4)
    throw new Error(`Invalid QUST ENAM for ${quest.id}`);
  return Buffer.from(events[0].data).equals(Buffer.from("ADIA")) ? "actor-dialogue" : "forced-start";
}

function normalizeStoryRoute(route) {
  const uint32 = value => Number.isInteger(value) && value >= 0 && value <= 0xffffffff;
  assert.ok(route && uint32(route.candidateCount), "Invalid story route candidate count");
  const {candidateCount, nodeIds, questIndex} = route;
  if (candidateCount !== 1) {
    assert.ok(nodeIds === undefined && questIndex === undefined, "Unexpected selected story route");
    return {candidateCount};
  }
  assert.ok(Array.isArray(nodeIds) && nodeIds.length >= 2 &&
    nodeIds.every(id => uint32(id) && id !== 0) && new Set(nodeIds).size === nodeIds.length &&
    uint32(questIndex), "Invalid selected story route");
  return {candidateCount, nodeIds, questIndex};
}

function probe(server, quest, actor, fixture) {
  const actors = [];
  const dialogue = fixture === "actor-dialogue";
  const participants = [];
  let manifest;
  try {
    server.createActor(actor, dialogue ? [0, 0, 0] : [10000, 10000, 10000], 0, dialogue ? banneredMareCell : 0x3c);
    actors.push(actor);
    const instance = `campaign:${actor}:startup`;
    server.set(actor, "worldInstance", instance);
    if (dialogue) {
      server.set(actor, "campaignReferences", {references: dialogueNpcs.map(npc => npc.original), encounterLevel: 1});
      manifest = server.get(actor, "campaignReferences");
      assert.ok(manifest?.schemaVersion === 1 && manifest.actor === actor && manifest.campaignOwner === actor &&
        manifest.instance === instance && /^[0-9a-f]{32}$/.test(manifest.identity) &&
        Array.isArray(manifest.references) && manifest.references.length === 2, "Invalid dialogue campaign manifest");
      for (const npc of dialogueNpcs) {
        const binding = manifest.references.find(ref => ref.original === npc.original);
        assert.ok(binding?.baseId === npc.base && binding.kind === "actor" &&
          binding.deleted === false && binding.disabled === false && Number.isInteger(binding.copy) &&
          binding.copy >= 0xff000000 && binding.copy <= 0xffffffff && !actors.includes(binding.copy),
          "Invalid dialogue NPC copy receipt");
        actors.push(binding.copy);
        participants.push(binding.copy);
        assert.equal(server.get(binding.copy, "worldInstance"), instance, "Dialogue copy is outside the owner instance");
        assert.equal(server.getActorCellOrWorld(binding.copy), banneredMareCell, "Expected authored Bannered Mare placement");
      }
    }
    server.set(actor, "private.boyhouse.questAdapters", [server.getDescFromId(quest.formId)]);
    const before = actors.map(id => server.get(id, journal));
    assert.ok(before.every(value => value == null), "Expected fresh campaign journals");
    let result;
    try {
      result = server.probeQuestStartup(actor, quest.formId, ...participants);
    } catch (error) {
      return errorRow(quest, "probe.exception", error.message);
    } finally {
      for (const [i, id] of actors.entries())
        assert.deepEqual(server.get(id, journal), before[i], "Startup probe changed a disposable journal");
      if (dialogue) assert.deepEqual(server.get(actor, "campaignReferences"), manifest, "Startup probe changed dialogue copy receipts");
    }
    const blocked = result?.status === "blocked";
    if (dialogue) {
      const event = result?.storyEvent;
      assert.ok(event, "Expected native ADIA evidence for both fixture actors and their location");
      const {actor1Original, actor2Original, ...bound} = event;
      assert.deepEqual(bound, {
        type: "ADIA", actor1: participants[0], actor2: participants[1], location: banneredMareLocation,
      }, "Expected native ADIA evidence for both fixture actors and their location");
      if (actor1Original !== undefined || actor2Original !== undefined)
        assert.deepEqual([actor1Original, actor2Original], dialogueNpcs.map(npc => npc.original),
          "Native ADIA event disagrees with the campaign copy receipts");
    }
    if ((!blocked && result?.status !== "startup-exercised") ||
        typeof result.step !== "string" || !result.step ||
        !Array.isArray(result.steps) || !result.steps.every(value => typeof value === "string" && value) ||
        !Array.isArray(result.stages) || !result.stages.every(Number.isInteger) ||
        (blocked ? !result.blocker?.code || typeof result.blocker.detail !== "string" : result.blocker !== null))
      throw new Error(`Invalid native startup result for ${quest.id}`);
    return { id: quest.id, editorId: quest.editorId, winningPlugin: quest.winningPlugin,
      status: blocked ? "blocked" : "started", step: result.step, steps: result.steps, stages: result.stages,
      ...(dialogue ? { storyEvent: { type: "ADIA", ...dialogueIdentities,
        cell: banneredMareCell, location: result.storyEvent.location } } : {}),
      ...(dialogue && result.storyRoute !== undefined ? {storyRoute: normalizeStoryRoute(result.storyRoute)} : {}),
      code: result.blocker?.code ?? "", detail: result.blocker?.detail ?? "" };
  } finally {
    for (const id of actors.reverse()) server.destroyActor(id);
  }
}

function attempt(server, quest, actor) {
  return probe(server, quest, actor, selectFixture(server, quest, "forced-start"));
}

const send = message => new Promise((resolve, reject) =>
  process.send(message, error => error ? reject(error) : resolve()));

async function work({ quests, addon, loadOrder, archives, scenario }) {
  const { ScampServer } = require(addon);
  fs.mkdirSync("scripts");
  const server = new ScampServer(JSON.stringify({
    port: 0, maxPlayers: 1, listenHost: "127.0.0.1", dataDir: process.cwd(),
    loadOrder, archives, npcEnabled: false, offlineMode: true, logLevel: "error",
  }));
  server._setSelf(server);
  assert.equal(typeof server.probeQuestStartup, "function", "Rebuild addon with probeQuestStartup support");
  assert.deepEqual(server.getEspmLoadOrder(), masters, "Unexpected native load order");
  for (const [i, quest] of quests.entries()) {
    const fixture = selectFixture(server, quest, scenario);
    await send({ start: quest.id, fixture });
    await send({ row: probe(server, quest, 0xff001000 + i, fixture) });
  }
}

async function runBatch(quests, config) {
  const state = fs.mkdtempSync(path.join(os.tmpdir(), "skymp-startup-"));
  let child, timer;
  try {
    child = fork(__filename, ["--worker"], {
      cwd: state, execArgv: [], stdio: ["ignore", "pipe", "pipe", "ipc"],
    });
    const rows = [];
    let active = null, failure = "", logs = "", timedOut = false;
    const pushRow = row => {
      rows.push(config.scenario === "forced-start" ? row : { ...row, fixture: active });
      active = null;
    };
    const watchdog = () => {
      clearTimeout(timer);
      timer = setTimeout(() => { timedOut = true; child.kill("SIGKILL"); }, 60000);
    };
    const tail = chunk => { logs = (logs + chunk.toString()).slice(-4096); };
    child.stdout.on("data", tail);
    child.stderr.on("data", tail);
    child.on("message", msg => {
      watchdog();
      const expected = quests[rows.length]?.id;
      if (msg.fatal) failure = msg.fatal;
      else if (!active && expected && msg.start === expected &&
          (msg.fixture === "forced-start" || msg.fixture === config.scenario)) active = msg.fixture;
      else if (active && expected && msg.row?.id === expected) {
        pushRow(msg.row);
      } else {
        failure = "Out-of-order startup worker response";
        child.kill("SIGKILL");
      }
    });
    const closed = new Promise((resolve, reject) => {
      child.once("error", reject);
      child.once("close", (code, signal) => resolve({ code, signal }));
    });
    watchdog();
    child.send({ quests, ...config });
    const { code, signal } = await closed;
    if (failure) throw new Error(failure);
    if (code === 0 && rows.length === quests.length && !active) return rows;
    if (active && (signal || timedOut || code !== 0)) {
      pushRow(errorRow(quests[rows.length], timedOut ? "probe.timeout" : "probe.crash",
        timedOut ? "Worker exceeded 60 seconds during this attempt" : `Worker terminated with ${signal ?? code}`));
      return rows;
    }
    throw new Error(`Startup worker stopped before completing its batch (${signal ?? code}). ${logs}`);
  } finally {
    clearTimeout(timer);
    if (child && child.exitCode === null && child.signalCode === null) child.kill("SIGKILL");
    fs.rmSync(state, { recursive: true, force: true });
  }
}

function writeReport(filename, contents) {
  fs.mkdirSync(path.dirname(filename), { recursive: true });
  const tmp = filename + "." + randomUUID() + ".tmp";
  try {
    fs.writeFileSync(tmp, contents, { flag: "wx" });
    fs.renameSync(tmp, filename);
  } finally {
    fs.rmSync(tmp, { force: true });
  }
}

async function main(args) {
  const options = Object.fromEntries(["catalog", "output", "markdown", "compare", "contrast", "scenario", "data-dir", "addon"]
    .map(key => [key, { type: "string" }]));
  const { values } = parseArgs({ args, options });
  if (!values.catalog || !values.output || !values.markdown)
    throw new Error("Usage: node quest-startup-runner.cjs --catalog FILE --output FILE --markdown FILE [--scenario forced-start|actor-dialogue] [--compare FILE] [--contrast FORCED_BASELINE] [--data-dir DIR] [--addon FILE]");
  const scenario = values.scenario ?? "forced-start";
  if (!["forced-start", "actor-dialogue"].includes(scenario)) throw new Error("Unknown startup scenario");
  if (values.contrast && scenario !== "actor-dialogue") throw new Error("--contrast requires --scenario actor-dialogue");
  const read = filename => JSON.parse(fs.readFileSync(filename, "utf8"));
  const catalog = read(values.catalog);
  const previous = values.compare ? read(values.compare) : null;
  const contrast = values.contrast ? read(values.contrast) : null;
  const { quests, deleted } = selectQuests(catalog);
  const { sources, loadOrder, archives } = await verifySources(catalog, path.resolve(values["data-dir"] || "/game-data"));
  const addon = fs.realpathSync(values.addon || defaultAddon);
  const output = path.resolve(values.output), markdown = path.resolve(values.markdown);
  const inputs = [path.resolve(values.catalog), addon, ...loadOrder, ...archives];
  if (values.compare) inputs.push(path.resolve(values.compare));
  if (values.contrast) inputs.push(path.resolve(values.contrast));
  if (output === markdown || inputs.includes(output) || inputs.includes(markdown))
    throw new Error("Choose output paths separate from each other, the baseline and the input files");
  const rows = [];
  while (rows.length < quests.length) {
    const batch = quests.slice(rows.length, rows.length + batchSize);
    rows.push(...await runBatch(batch, { addon, loadOrder, archives, scenario }));
  }
  assert.equal(rows.length + deleted.length, catalog.summary.questRecords);
  const provenance = { mode: scenario, sources, addonSha256: await hashFile(addon),
    catalogRecords: catalog.summary.questRecords, excludedDeleted: deleted, discardedCampaigns: true };
  if (scenario === "actor-dialogue") {
    provenance.actorDialogue = {winningQuestEvent: "ADIA", fallback: "forced-start", ...dialogueIdentities,
      placement: "authored", encounterLevel: 1,
      cell: banneredMareCell, location: banneredMareLocation};
  }
  const report = buildStartupReport(rows, { provenance, previous, contrast });
  writeReport(output, JSON.stringify(report, null, 2) + "\n");
  writeReport(markdown, renderStartupMarkdown(report));
  console.log(JSON.stringify(report.summary));
  if (report.summary.error) process.exitCode = 1;
}

module.exports = { selectQuests, verifySources, attempt };
if (require.main === module) {
  if (process.send && process.argv[2] === "--worker") {
    process.once("message", config => work(config).then(() => process.exit(0)).catch(async error => {
      await send({ fatal: error.message });
      process.exit(1);
    }));
  } else main(process.argv.slice(2)).catch(error => { console.error(error.message); process.exitCode = 1; });
}
