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

function attempt(server, quest, actor) {
  const { record, fileIndex } = server.lookupEspmRecordById(quest.formId);
  if (record?.type !== "QUST" || (record.flags & 0x20) || record.editorId !== quest.editorId ||
      masters[fileIndex] !== quest.winningPlugin)
    throw new Error(`Native winning record differs from the catalog: ${quest.id}`);
  server.createActor(actor, [10000, 10000, 10000], 0, 0x3c);
  try {
    server.set(actor, "worldInstance", `campaign:${actor}:startup`);
    server.set(actor, "private.boyhouse.questAdapters", [server.getDescFromId(quest.formId)]);
    const before = server.get(actor, journal);
    assert.ok(before == null, "Expected a fresh campaign journal");
    let result;
    try {
      result = server.probeQuestStartup(actor, quest.formId);
    } catch (error) {
      return errorRow(quest, "probe.exception", error.message);
    } finally {
      assert.deepEqual(server.get(actor, journal), before, "Startup probe changed the disposable journal");
    }
    const blocked = result?.status === "blocked";
    if ((!blocked && result?.status !== "startup-exercised") ||
        typeof result.step !== "string" || !result.step ||
        !Array.isArray(result.steps) || !result.steps.every(value => typeof value === "string" && value) ||
        !Array.isArray(result.stages) || !result.stages.every(Number.isInteger) ||
        (blocked ? !result.blocker?.code || typeof result.blocker.detail !== "string" : result.blocker !== null))
      throw new Error(`Invalid native startup result for ${quest.id}`);
    return { id: quest.id, editorId: quest.editorId, winningPlugin: quest.winningPlugin,
      status: blocked ? "blocked" : "started", step: result.step, steps: result.steps, stages: result.stages,
      code: result.blocker?.code ?? "", detail: result.blocker?.detail ?? "" };
  } finally {
    server.destroyActor(actor);
  }
}

const send = message => new Promise((resolve, reject) =>
  process.send(message, error => error ? reject(error) : resolve()));

async function work({ quests, addon, loadOrder, archives }) {
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
    await send({ start: quest.id });
    await send({ row: attempt(server, quest, 0xff001000 + i) });
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
    let active = false, failure = "", logs = "", timedOut = false;
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
      else if (!active && expected && msg.start === expected) active = true;
      else if (active && expected && msg.row?.id === expected) {
        active = false;
        rows.push(msg.row);
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
      rows.push(errorRow(quests[rows.length], timedOut ? "probe.timeout" : "probe.crash",
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
  const options = Object.fromEntries(["catalog", "output", "markdown", "compare", "data-dir", "addon"]
    .map(key => [key, { type: "string" }]));
  const { values } = parseArgs({ args, options });
  if (!values.catalog || !values.output || !values.markdown)
    throw new Error("Usage: node quest-startup-runner.cjs --catalog FILE --output FILE --markdown FILE [--compare FILE] [--data-dir DIR] [--addon FILE]");
  const read = filename => JSON.parse(fs.readFileSync(filename, "utf8"));
  const catalog = read(values.catalog);
  const previous = values.compare ? read(values.compare) : null;
  const { quests, deleted } = selectQuests(catalog);
  const { sources, loadOrder, archives } = await verifySources(catalog, path.resolve(values["data-dir"] || "/game-data"));
  const addon = fs.realpathSync(values.addon || defaultAddon);
  const output = path.resolve(values.output), markdown = path.resolve(values.markdown);
  const inputs = [path.resolve(values.catalog), addon, ...loadOrder, ...archives];
  if (values.compare) inputs.push(path.resolve(values.compare));
  if (output === markdown || inputs.includes(output) || inputs.includes(markdown))
    throw new Error("Choose output paths separate from each other, the baseline and the input files");
  const rows = [];
  while (rows.length < quests.length) {
    const batch = quests.slice(rows.length, rows.length + batchSize);
    rows.push(...await runBatch(batch, { addon, loadOrder, archives }));
  }
  assert.equal(rows.length + deleted.length, catalog.summary.questRecords);
  const provenance = { mode: "forced-start", sources, addonSha256: await hashFile(addon),
    catalogRecords: catalog.summary.questRecords, excludedDeleted: deleted, discardedCampaigns: true };
  const report = buildStartupReport(rows, { provenance, previous });
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
