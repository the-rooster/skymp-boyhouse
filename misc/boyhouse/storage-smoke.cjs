"use strict";
// Uses disposable server characters, not Skyrim clients or quest playthroughs.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { fork, spawnSync } = require("node:child_process");
const { once } = require("node:events");
const { setTimeout: delay } = require("node:timers/promises");
const root = path.resolve(__dirname, "../..");
const ids = Array.from({ length: 32 }, (_, index) => 0xff000b00 + index);
const [owner, guest, stranger] = ids;
const quest = 0x4e50d, questKey = "4e50d:Skyrim.esm";
const privateJournal = "private.boyhouse.questJournal";

function worker(mode, state) {
  const { ScampServer } = require(path.join(root, "build/dist/server/scam_native.node"));
  const { PartyInstances } = require("../../skymp5-server/ts/campaign/partyInstances.js");
  const server = new ScampServer(JSON.stringify({
    port: 17783, maxPlayers: 32, listenHost: "127.0.0.1",
    dataDir: path.join(state, "data"), databaseDriver: "sqlite", databaseName: path.join(state, "world.sqlite3"),
    loadOrder: ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"].map(name => `/game-data/${name}`),
    archives: ["/game-data/Skyrim - Misc.bsa"], npcEnabled: false, offlineMode: true, logLevel: "error",
  }));
  server._setSelf(server);
  server.attachSaveStorage();
  const authority = new PartyInstances({
    exists: id => server.get(id, "type") === "MpActor",
    getInstance: id => server.get(id, "worldInstance"),
    prepareInstance: (id, area) => server.set(id, "campaignReferences", {
      instanceArea: area.id, encounterLevel: area.encounterLevel, references: [0x1a677, 0x8a986],
    }),
    setInstance: (id, key) => server.set(id, "worldInstance", key),
    disable: id => server.setEnabled(id, false), isInside: () => true,
  }, [{ id: "fixture", worldOrCell: 0x3c, center: [10000, 10000, 10000], radius: 100, encounterLevel: 5 }]);
  const snapshot = id => {
    const allocations = server.get(id, "private.boyhouse.referenceAllocations") ?? null;
    return {
      id, instance: server.get(id, "worldInstance"), inventory: server.get(id, "inventory"),
      journal: server.get(id, privateJournal), allocations,
      copies: Object.entries(allocations?.copies ?? {}).map(([original, shortId]) => {
        const copy = 0xff000000 + parseInt(shortId, 16);
        return { original, copy, instance: server.get(copy, "worldInstance"), inventory: server.get(copy, "inventory") };
      }),
    };
  };
  if (mode === "--write") {
    for (const [index, id] of ids.entries()) {
      server.createActor(id, [10000, 10000, 10000], 0, 0x3c, 1000 + index);
      server.set(id, "private.boyhouse.questAdapters", [questKey]);
      server.set(id, "questCommand", { quest, op: "setStage", index: id === guest ? 5 : 10 });
      server.set(id, "inventory", { entries: [{ baseId: 0xf, count: 100 + index }] });
    }
    authority.enter(owner, owner, "fixture");
    authority.invite(owner, guest); authority.accept(guest, owner);
    authority.enter(guest, owner, "fixture");
    // Original fragment execution persists its VM state with the host journal.
    server.set(guest, "questExecuteStage", { quest, stage: 100 });
    assert.equal(server.get(owner, "questJournal").quests[questKey].stage, 100);
    process.send(ids.map(snapshot));
    setInterval(() => server.tick(), 10);
  } else {
    const expected = JSON.parse(fs.readFileSync(path.join(state, "expected.json"), "utf8"));
    assert.deepEqual(ids.map(snapshot), expected);
    for (const id of ids) authority.recover(id);
    assert.equal(server.get(owner, "questJournal").quests[questKey].stage, 100);
    assert.equal(server.get(guest, "questJournal").quests[questKey].stage, 5);
    assert.equal(server.get(stranger, "questJournal").quests[questKey].stage, 10);
    assert.notEqual(server.get(owner, "worldInstance"), server.get(guest, "worldInstance"));
    assert.deepEqual(ids.map(id => server.get(id, privateJournal)), expected.map(form => form.journal));
    assert.deepEqual(snapshot(owner), expected[0], "Owner references and loot survive recovery unchanged");
    const recovered = snapshot(guest);
    assert.equal(recovered.copies.length, 2, "Guest recovery prepares a personal destination before admission");
    assert.ok(recovered.copies.every(copy => copy.instance === recovered.instance));
    assert.ok(recovered.copies.every(copy => !expected[0].copies.some(host => host.copy === copy.copy)));
    console.log("SQLite restart restored 32 journals/inventories, preserved allocated loot, and prepared personal guest recovery");
    process.exit(0);
  }
}

async function main() {
  const { DatabaseSync } = require("node:sqlite");
  const state = fs.mkdtempSync(path.join(os.tmpdir(), "boyhouse-storage-"));
  fs.mkdirSync(path.join(state, "data/scripts"), { recursive: true });
  const writer = fork(__filename, ["--write", state], { silent: true });
  let output = "";
  for (const stream of [writer.stdout, writer.stderr]) stream.on("data", chunk => { output = (output + chunk).slice(-64000); });
  const watchdog = setTimeout(() => writer.kill("SIGKILL"), 30000);
  let reader;
  try {
    const expected = await Promise.race([
      once(writer, "message").then(([message]) => message),
      once(writer, "exit").then(([code, signal]) => { throw new Error(`Writer exited before saving: ${code}/${signal}\n${output}`); }),
    ]);
    fs.writeFileSync(path.join(state, "expected.json"), JSON.stringify(expected));
    reader = new DatabaseSync(path.join(state, "world.sqlite3"), { readOnly: true });
    const deadline = Date.now() + 10000;
    let saved = false;
    while (Date.now() < deadline) {
      const forms = new Map(reader.prepare("SELECT form_desc,body FROM change_forms").all().map(row => [row.form_desc, JSON.parse(row.body)]));
      saved = expected.every(form => {
        const stored = forms.get((form.id & 0xffffff).toString(16));
        return stored && stored.worldInstance === form.instance &&
          JSON.stringify(stored.inv) === JSON.stringify(form.inventory) &&
          JSON.stringify(stored.dynamicFields[privateJournal]) === JSON.stringify(form.journal);
      });
      if (saved) break;
      if (writer.exitCode !== null || writer.signalCode !== null) throw new Error(`Writer died during save\n${output}`);
      await delay(20);
    }
    assert.ok(saved, `Server did not durably save all 32 characters\n${output}`);
    reader.close(); reader = undefined;
    // Abruptly end the native saver thread; then load through the real server.
    const exited = once(writer, "exit");
    writer.kill("SIGKILL");
    const [, signal] = await exited;
    assert.equal(signal, "SIGKILL");
    clearTimeout(watchdog);
    const result = spawnSync(process.execPath, [__filename, "--read", state], { encoding: "utf8", timeout: 30000 });
    assert.equal(result.status, 0, `${result.error || ""}\n${result.stdout}\n${result.stderr}`);
    console.log(result.stdout.trim());
    fs.rmSync(state, { recursive: true });
  } catch (error) {
    console.error(`Storage smoke artifacts retained at ${state}`);
    throw error;
  } finally {
    clearTimeout(watchdog);
    reader?.close();
    if (writer.exitCode === null && writer.signalCode === null) writer.kill("SIGKILL");
  }
}

if (["--write", "--read"].includes(process.argv[2])) worker(process.argv[2], process.argv[3]);
else main().catch(error => { console.error(error); process.exitCode = 1; });
