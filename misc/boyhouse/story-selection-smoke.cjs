"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const {spawnSync} = require("node:child_process");

const masters = ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"];
const gameData = "/game-data";
const pluginName = "StorySelectionTest.esp";
const cell = 0x1605e, location = 0x1f86d;
const hulda = 0x1a66e, mikael = 0x1a671;
const target = id => masters.length * 0x1000000 + id;
const firstQuest = target(0x803), fallbackQuest = target(0x804);
const banneredMareQuest = 0x17f32;

function u32(n) {
  const bytes = Buffer.alloc(4);
  bytes.writeUInt32LE(n);
  return bytes;
}

function field(tag, bytes) {
  const size = Buffer.alloc(2);
  size.writeUInt16LE(bytes.length);
  return Buffer.concat([Buffer.from(tag), size, bytes]);
}

function record(type, id, fields, flags = 0) {
  const body = Buffer.concat(fields);
  return Buffer.concat([Buffer.from(type), u32(body.length), u32(flags), u32(id),
    Buffer.alloc(8), body]);
}

function group(type, records) {
  const body = Buffer.concat(records);
  return Buffer.concat([Buffer.from("GRUP"), u32(24 + body.length), Buffer.from(type),
    Buffer.alloc(12), body]);
}

// The fixtures override only routing unless the test also needs a controlled
// quest. All modes use the same NPCs, placements, campaign allocator and masters.
function writeFixture(mode) {
  const local = id => 0x01000000 + id; // the plugin has one master
  const event = local(0x800), first = local(0x801), fallback = local(0x802);
  const number = (tag, n) => field(tag, u32(n));
  const node = (parent, previous, count) => [number("PNAM", parent), number("SNAM", previous),
    number("CITC", count), number("DNAM", 0), number("XNAM", 0)];
  const condition = Buffer.alloc(32); // GetIsID(Mikael), run on ADIA actor 1
  condition.writeFloatLE(1, 4);
  condition.writeUInt16LE(72, 8);
  condition.writeUInt32LE(0x1a670, 12);
  condition.writeUInt32LE(7, 20);
  condition.write("R1", 28, "ascii");
  const eventAlias = (kind, id, member, requiredBase) => {
    const fields = [number(kind, id), field("ALID", Buffer.from("Event" + member + "\0")),
      number("FNAM", kind === "ALST" ? 0x200 : 0), field("ALFE", Buffer.from("ADIA")),
      field("ALFD", Buffer.from(member + "\0\0"))];
    if (requiredBase !== undefined) {
      const filter = Buffer.alloc(32); // Subject.GetIsID on this alias's event member
      filter.writeFloatLE(1, 4);
      filter.writeUInt16LE(72, 8);
      filter.writeUInt32LE(requiredBase, 12);
      filter.writeInt32LE(-1, 28);
      fields.push(field("CTDA", filter));
    }
    if (kind === "ALST") fields.push(number("VTCK", 0));
    return [...fields, field("ALED", Buffer.alloc(0))];
  };
  const makeQuest = (id, name, actor1Base, actor2Base) => {
    const denyReuse = mode === "reuse-denied-fixture";
    const data = Buffer.alloc(12);
    data.writeUInt16LE(0x100); // Run Once
    return record("QUST", local(id), [field("EDID", Buffer.from(name + "\0")),
      field("DNAM", data), field("ENAM", Buffer.from("ADIA")),
      field("NEXT", Buffer.alloc(0)), number("ANAM", denyReuse ? 4 : 3),
      ...eventAlias("ALST", 0, "R1", actor1Base), ...eventAlias("ALST", 1, "R2", actor2Base),
      ...eventAlias("ALLS", 2, "L1"),
      ...(denyReuse ? [number("ALST", 3), field("ALID", Buffer.from("NamedMikael\0")),
        number("FNAM", 0x200), number("ALUA", 0x1a670), number("VTCK", 0), field("ALED", Buffer.alloc(0))] : [])]);
  };
  const header = Buffer.alloc(12);
  header.writeFloatLE(1.7);
  header.writeUInt32LE(mode === "vanilla-entry-fixture" ? 3 : 6, 4);
  header.writeUInt32LE(0x805, 8);
  const records = [record("TES4", 0, [field("HEDR", header),
    field("MAST", Buffer.from("Skyrim.esm\0")), field("DATA", Buffer.alloc(8))]),
    group("SMEN", [record("SMEN", 0x15fce, [], 0x20),
      record("SMEN", event, [...node(0, 0, 0), field("ENAM", Buffer.from("ADIA"))])])];
  if (mode === "vanilla-entry-fixture") {
    // Only the routing changes. The QUST, its aliases, child scenes and scripts
    // are read from the five masters and the original archive.
    records.push(group("SMQN", [record("SMQN", first, [...node(event, 0, 0),
      number("QNAM", 1), number("NNAM", banneredMareQuest)])]));
  } else {
    records.push(group("SMQN", [
      record("SMQN", first, [...node(event, 0, 1), field("CTDA", condition),
        number("QNAM", 1), number("NNAM", local(0x803))]),
      record("SMQN", fallback, [...node(event, first, 0),
        number("QNAM", 1), number("NNAM", local(0x804))])]));
    const firstBase = mode === "alias-fallback-fixture" ? 0x13ba3 : 0x1a670;
    records.push(group("QUST", [makeQuest(0x803, "StoryMikaelFirst", firstBase),
      makeQuest(0x804, "StoryFallback")]));
  }
  fs.writeFileSync(pluginName, Buffer.concat(records));
}

function probe(mode) {
  const loadOrder = masters.map(name => path.join(gameData, name));
  if (mode !== "owned-masters") {
    writeFixture(mode);
    loadOrder.push(path.resolve(pluginName));
  }
  fs.mkdirSync("scripts");
  const {ScampServer} = require(path.resolve(__dirname, "../../build/dist/server/scam_native.node"));
  const server = new ScampServer(JSON.stringify({port:0, maxPlayers:1, listenHost:"127.0.0.1",
    dataDir:process.cwd(), loadOrder, archives:[path.join(gameData, "Skyrim - Misc.bsa")],
    npcEnabled:false, offlineMode:true, logLevel:"error"}));
  server._setSelf(server);
  const owner = 0xff001000;
  server.createActor(owner, [0, 0, 0], 0, cell);
  server.set(owner, "worldInstance", `campaign:${owner}:startup`);
  server.set(owner, "campaignReferences", {references:[hulda, mikael], encounterLevel:1});
  const manifest = server.get(owner, "campaignReferences");
  const copy = id => manifest.references.find(ref => ref.original === id).copy;
  const ordered = [copy(hulda), copy(mikael)];
  const observations = [];
  for (const [i, participants] of [ordered, [...ordered].reverse()].entries()) {
    const select = () => server.probeActorDialogueSelection(owner, ...participants);
    const event = select(); // no quest ID and no enrolled quest
    assert.deepEqual(event.storyEvent, {type:"ADIA", actor1:participants[0], actor2:participants[1], location});
    assert.deepEqual(select(), event);
    let startup;
    if (mode === "owned-masters") {
      assert.equal(event.storySelection.outcome, "blocked");
      assert.equal(event.storySelection.nodeId, 0x16fa1);
      assert.equal(event.storySelection.questId, 0x16fa6);
    } else if (mode === "vanilla-entry-fixture") {
      const {fileIndex, record} = server.lookupEspmRecordById(banneredMareQuest);
      assert.equal(fileIndex, 0);
      assert.equal(record.editorId, "DialogueWhiterunBanneredMareScene1");
      assert.equal(event.storySelection.outcome, "attempt");
      assert.equal(event.storySelection.questId, banneredMareQuest);
      server.set(owner, "private.boyhouse.questAdapters", [server.getDescFromId(banneredMareQuest)]);
      const result = server.probeQuestStartup(owner, banneredMareQuest, ...participants);
      assert.deepEqual(result.storySelection, event.storySelection);
      assert.deepEqual(result.storyAliasBindings, [
        {id:3, member:"R1", formId:i === 0 ? hulda : mikael},
        {id:4, member:"R2", formId:i === 0 ? mikael : hulda}
      ]);
      assert.deepEqual(result.storyUniqueActorBindings, [
        {id:1, baseId:0x13ba3, formId:hulda, reused:true},
        {id:2, baseId:0x1a670, formId:mikael, reused:true}
      ]);
      assert.equal(result.status, "blocked");
      assert.equal(result.blocker.code, "scene.dialogue-playback");
      const topics = [0x17f2b, 0x17f2a, 0x17f29, 0x17f28, 0x17f27, 0x17f1f];
      assert.deepEqual(result.scenePlan, { questId: banneredMareQuest, sceneId: 0x17f33,
        lines: topics.map((topicId, phase) => ({ index: phase + 1, phase, speakerAliasId: phase % 2 + 1,
          speakerFormId: [hulda, mikael][phase % 2], speakerCopyId: ordered[phase % 2], topicId })) });
      assert.equal(result.blocker.detail, "17f33:Skyrim.esm");
      startup = {quest:record.editorId, status:result.status, step:result.step, blocker:result.blocker,
        stages:result.stages, eventAliases:result.storyAliasBindings, uniqueAliases:result.storyUniqueActorBindings, scenePlan:result.scenePlan};
      server.set(owner, "private.boyhouse.questAdapters", []);
    } else {
      const expected = i === 1 && mode !== "alias-fallback-fixture" ? firstQuest : fallbackQuest;
      assert.equal(event.storySelection.outcome, "attempt");
      assert.equal(event.storySelection.questId, expected);
      if (mode === "alias-fallback-fixture" && i === 1) {
        server.set(owner, "private.boyhouse.questAdapters", [server.getDescFromId(firstQuest)]);
        const rejected = server.probeQuestStartup(owner, firstQuest, ...participants);
        assert.equal(rejected.blocker.code, "alias.event-condition-false");
        assert.deepEqual(rejected.storySelection, event.storySelection);
      }
      if (expected === firstQuest) {
        server.set(owner, "private.boyhouse.questAdapters", [server.getDescFromId(fallbackQuest)]);
        const later = server.probeQuestStartup(owner, fallbackQuest, ...participants);
        assert.equal(later.blocker.code, "story.preceding-quest");
        assert.deepEqual(later.storySelection, event.storySelection);
      }
      server.set(owner, "private.boyhouse.questAdapters", [server.getDescFromId(expected)]);
      const started = server.probeQuestStartup(owner, expected, ...participants);
      if (mode === "reuse-denied-fixture") {
        assert.equal(started.status, "blocked");
        assert.equal(started.blocker.code, "alias.reuse");
      } else {
        assert.equal(started.status, "startup-exercised", JSON.stringify(started.blocker));
      }
      assert.deepEqual(started.storySelection, event.storySelection);
      assert.deepEqual(started.storyAliasBindings, [
        {id:0, member:"R1", formId:i === 0 ? hulda : mikael},
        {id:1, member:"R2", formId:i === 0 ? mikael : hulda},
        {id:2, member:"L1", formId:location}
      ]);
      startup = {quest:expected, status:started.status, step:started.step, blocker:started.blocker, stages:started.stages,
        eventAliases:started.storyAliasBindings};
      server.set(owner, "private.boyhouse.questAdapters", []);
    }
    for (const id of [owner, ...ordered]) assert.equal(server.get(id, "private.boyhouse.questJournal"), null);
    assert.deepEqual(server.get(owner, "campaignReferences"), manifest);
    observations.push({actor1Original:i === 0 ? hulda : mikael, actor2Original:i === 0 ? mikael : hulda,
      selection:event.storySelection, ...(startup ? {startup} : {})});
  }
  console.log(JSON.stringify({mode, observations}));
}

if (process.argv[2]) {
  probe(process.argv[2]);
} else {
  for (const mode of ["owned-masters", "ordered-fixture", "alias-fallback-fixture", "reuse-denied-fixture", "vanilla-entry-fixture"]) {
    const cwd = fs.mkdtempSync(path.join(os.tmpdir(), "skymp-story-event-"));
    try {
      const run = spawnSync(process.execPath, [__filename, mode], {cwd, stdio:"inherit", timeout:120000});
      if (run.error) throw run.error;
      assert.equal(run.status, 0, `Actor dialogue smoke failed for ${mode}`);
    } finally {
      fs.rmSync(cwd, {recursive:true, force:true});
    }
  }
}
