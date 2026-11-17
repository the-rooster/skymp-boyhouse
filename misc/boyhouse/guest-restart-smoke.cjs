"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

const root = path.resolve(__dirname, "../..");
const read = name => JSON.parse(fs.readFileSync(name, "utf8"));
const baselineField = "private.boyhouse.movementBaseline";
const journalField = "private.boyhouse.questJournal";

async function loginGuest({ server, sleep, packet, prepared, acknowledge, login, boot }) {
  const seed = read("visitor-seed.json");
  const hostJournal = server.get(seed.host, journalField);
  const guestJournal = server.get(seed.actor, journalField);
  const before = server.get(seed.actor, baselineField);
  assert.equal(before.instance, boot === "first" ? seed.oldPhase : seed.personalPhase);
  const player = await login(seed.profile, "visitor-restart");
  const manifest = await prepared(player);
  assert.equal(player.actor, seed.actor);
  assert.equal(manifest.campaignOwner, seed.actor);
  assert.equal(manifest.instance, seed.personalPhase);
  if (boot === "second") {
    // Reuse the session label and user slot so the previous packet really
    // exercises the fresh server epoch rather than just a different player.
    packet(player.bot, read("previous-ack.json"));
    await sleep(100);
    assert.equal(server.getGameplayBlocked(player.user), true);
  }
  const ack = await acknowledge(player);
  const epoch = player.replica.packetEpoch;
  const subscribe = subscriptionId => player.bot.send({ t: 34, epoch, message: {
    t: 1, contentJsonDump: JSON.stringify({
      customPacketType: "boyhouse.questJournal", action: "subscribe", subscriptionId,
    }),
  } });
  const wrongEpoch = (epoch[0] === "a" ? "b" : "a") + epoch.slice(1);
  player.bot.pollCustomPackets(wrongEpoch);
  subscribe("wrong-epoch");
  // The journal feed accepts at most one subscription per 200 ms.
  await sleep(300);
  assert.deepEqual(player.bot.pollCustomPackets(wrongEpoch), []);
  player.bot.pollCustomPackets(epoch);
  const subscriptionId = "guest-personal";
  subscribe(subscriptionId);
  let journal;
  for (let poll = 0; poll < 100 && !journal; poll++) {
    await sleep(25);
    for (const json of player.bot.pollCustomPackets(epoch)) {
      const message = JSON.parse(json);
      if (message.customPacketType === "boyhouse.questJournal" && message.subscriptionId === subscriptionId) {
        assert.ok(message.ok, message.error);
        journal = message.journal;
      }
    }
  }
  assert.ok(journal, "Guest did not receive the subscribed journal");
  assert.equal(journal.campaignOwner, seed.actor);
  assert.equal(journal.quests[seed.questKey].stage, seed.guestStage);
  assert.equal(server.get(seed.host, "questJournal").quests[seed.questKey].stage, seed.hostStage);
  const gold = id => server.get(id, "inventory").entries
    .filter(entry => entry.baseId === 15).reduce((sum, entry) => sum + entry.count, 0);
  assert.equal(gold(player.actor), seed.guestGold);
  assert.equal(gold(seed.host), seed.hostGold);
  assert.deepEqual(server.get(seed.host, journalField), hostJournal);
  assert.deepEqual(server.get(player.actor, journalField), guestJournal);
  const after = server.get(player.actor, baselineField);
  assert.equal(after.instance, seed.personalPhase);
  assert.equal(after.sequence, seed.sequence);
  assert.deepEqual(after.position, before.position);
  assert.deepEqual(after.angle, before.angle);
  if (boot === "first") {
    assert.notEqual(after.campaignIdentity, before.campaignIdentity);
    fs.writeFileSync("previous-ack.json", JSON.stringify(ack));
  } else {
    assert.deepEqual(after, before);
  }
}

function run(executable, args, cwd, extraEnv) {
  const result = spawnSync(executable, args, {
    cwd, env: { ...process.env, ...extraEnv }, stdio: "inherit", timeout: 60000,
  });
  if (result.error) throw result.error;
  assert.equal(result.status, 0, `Process exited with ${result.status ?? result.signal}`);
}

function main() {
  const state = fs.mkdtempSync(path.join(os.tmpdir(), "skymp-visitor-restart-"));
  try {
    const seedDir = path.join(state, "seed");
    run(path.join(root, "build/unit/unit"), ["[GuestRestartSeed]"], root,
      { BOYHOUSE_VISITOR_SEED_DIR: seedDir });
    const seed = read(path.join(seedDir, "visitor-seed.json"));
    fs.mkdirSync(path.join(seedDir, "data/scripts"), { recursive: true });
    const entry = path.join(root, "build/dist/server");
    for (const file of ["scam_native.node", "dist_back"])
      fs.symlinkSync(path.join(entry, file), path.join(seedDir, file));
    const gamemode = path.join(__dirname, "projection-smoke-gamemode.cjs");
    fs.writeFileSync(path.join(seedDir, "gamemode.js"), `require(${JSON.stringify(gamemode)});`);
    fs.writeFileSync(path.join(seedDir, "server-settings.json"), JSON.stringify({
      name: "Cold visitor restart smoke", port: 17799, maxPlayers: 4,
      listenHost: "127.0.0.1", uiListenHost: "127.0.0.1", offlineMode: true,
      master: "http://127.0.0.1", dataDir: "./data", npcEnabled: false, logLevel: "error",
      databaseDriver: "sqlite", databaseName: path.join(seedDir, "world.sqlite3"),
      loadOrder: ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"]
        .map(name => path.join("/game-data", name)),
      archives: ["/game-data/Skyrim - Misc.bsa"],
      startPoints: [{ pos: seed.area.center, worldOrCell: String(seed.area.worldOrCell), angleZ: 0 }],
      boyhouseInstances: { experimental: true, projection: true, areas: [seed.area] },
    }));
    for (const boot of ["first", "second"]) {
      run(process.execPath, ["--experimental-strip-types", path.join(seedDir, "dist_back/skymp5-server.js")],
        seedDir, { BOYHOUSE_GUEST_RESTART: boot, BOYHOUSE_PROTOCOL_COMMUNITY: "0" });
    }
    console.log("Visitor restart passed: personal journal, inventory, movement baseline, and fresh acknowledgement across two server boots");
    fs.rmSync(state, { recursive: true });
  } catch (error) {
    console.error(`Visitor restart fixture retained at ${state}`);
    throw error;
  }
}

module.exports = { loginGuest };
if (require.main === module) main();
