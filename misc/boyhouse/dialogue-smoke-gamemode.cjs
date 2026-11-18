"use strict";

const assert = require("node:assert/strict");
const questId = 0x17f32, sceneId = 0x17f33;
const originals = [0x1a66e, 0x1a671];
const topics = [0x17f2b, 0x17f2a, 0x17f29, 0x17f28, 0x17f27, 0x17f1f];
const type = "boyhouse.sceneDialogue";

module.exports = async ({ server, sleep, packet, prepared, acknowledge, login, command, finishCreation, scope, community }) => {
  const players = [];
  for (const [index, name] of ["innkeeper", "visitor", "other-campaign"].entries()) {
    const player = await login(4100 + index, name);
    await acknowledge(player);
    await finishCreation(player);
    players.push(player);
  }
  const [host, guest, outsider] = players;
  for (const p of [host, outsider]) {
    server.set(p.actor, "campaignReferences", { instanceArea: "fixture", references: originals, encounterLevel: 5 });
    await command(p, { action: "enter", area: "fixture" });
    await acknowledge(p);
  }
  assert.notEqual(scope(host), scope(outsider));
  await command(host, { action: "invite", target: guest.actor });
  await command(guest, { action: "accept", leader: host.actor });
  await command(guest, { action: "enter", owner: host.actor, area: "fixture" });
  await prepared(guest);
  assert.equal(scope(guest), scope(host));
  // Keep the remaining community members in shared space throughout playback.
  const bystanders = community ? await Promise.all(Array.from({length:29}, async (_, i) => {
    const p = await login(4200 + i, "listener-" + i);
    await acknowledge(p);
    await finishCreation(p);
    assert.equal(scope(p), "");
    return p;
  })) : [];
  const nonlisteners = [outsider, ...bystanders];
  const copy = (player, original) => server.get(player.actor, "campaignReferences").references
    .find(ref => ref.original === original).copy;
  assert.notEqual(copy(host, originals[0]), copy(outsider, originals[0]));

  // The separate startup smoke fills the authored aliases. Here the existing
  // quest-save format lets an ordinary assigned player resume at its scene.
  const key = "17f32:Skyrim.esm";
  server.set(host.actor, "private.boyhouse.questAdapters", [key]);
  server.set(host.actor, "questCommand", { quest: questId, op: "start" });
  assert.throws(() => server.beginCampaignSceneDialogue(host.actor, questId, sceneId), /scene.speaker-binding/);
  const ledger = server.get(host.actor, "private.boyhouse.questJournal");
  ledger.vm = { schemaVersion: 6, scripts: {}, arrays: {}, globals: {}, continuations: {},
    aliases: { [key + "/alias:1"]: "1a66e:Skyrim.esm", [key + "/alias:2"]: "1a671:Skyrim.esm" } };
  server.set(host.actor, "private.boyhouse.questJournal", ledger);

  // Only the trusted gamemode can request a cue.
  const drain = p => p.bot.pollCustomPackets(p.replica.packetEpoch).map(dump => JSON.parse(dump)).filter(m => m.customPacketType === type);
  const forged = { customPacketType: type, owner: host.actor, sceneId, topicId: topics[0], actionIndex: 1 };
  packet(outsider.bot, forged);
  outsider.bot.send({ t: 34, epoch: outsider.replica.packetEpoch, message: { t: 1, contentJsonDump: JSON.stringify(forged) } });
  await sleep(100);
  for (const p of [...players, ...bystanders]) assert.deepEqual(drain(p), []);

  const run = server.beginCampaignSceneDialogue(host.actor, questId, sceneId);
  assert.throws(() => server.beginCampaignSceneDialogue(host.actor, questId, sceneId), /already active/);
  assert.throws(() => run.advance(2), /no longer pending/);
  assert.equal(run.advance(1), 1, "Guest has not acknowledged its projection yet");
  await sleep(100);
  assert.equal(drain(host).length, 1);
  assert.deepEqual(drain(guest), []);
  for (const p of nonlisteners) assert.deepEqual(drain(p), []);
  run.cancel();
  assert.throws(() => run.advance(2), /no longer pending/);
  await acknowledge(guest);

  const scene = server.beginCampaignSceneDialogue(host.actor, questId, sceneId);
  for (let i = 0; i < topics.length; i++) {
    assert.equal(scene.nextActionIndex, i + 1);
    assert.equal(scene.advance(i + 1), 2);
    assert.throws(() => scene.advance(i + 1), /no longer pending/);
  }
  await sleep(200);
  assert.equal(scene.nextActionIndex, null);
  for (const p of [host, guest]) {
    const cues = drain(p);
    assert.equal(cues.length, 6);
    for (const [i, cue] of cues.entries()) {
      assert.deepEqual(cue, { customPacketType: type, protocolVersion: 1, packetEpoch: p.replica.packetEpoch,
        sequence: i + (p === host ? 2 : 1), sceneId, actionIndex: i + 1,
        speaker: copy(host, originals[i % 2]), topicId: topics[i] });
    }
  }
  for (const p of nonlisteners) assert.deepEqual(drain(p), []);
  const timingFile = process.env.BOYHOUSE_DIALOGUE_TIMINGS;
  let timed;
  if (timingFile) {
    const timing = JSON.parse(require("node:fs").readFileSync(timingFile, "utf8"));
    const durations = topics.map(topic => timing.topics[server.getDescFromId(topic)]);
    assert.ok(durations.every(ms => Number.isSafeInteger(ms) && ms > 0));
    const began = performance.now();
    const playback = server.playCampaignSceneDialogue(host.actor, questId, sceneId);
    const heard = [[], []];
    while (heard[0].length < topics.length || heard[1].length < topics.length) {
      assert.ok(performance.now() - began < durations.reduce((a, b) => a + b, 0) + 5000, "Timed cues did not arrive");
      await sleep(25);
      for (const [index, listener] of [host, guest].entries()) {
        for (const cue of drain(listener)) {
          assert.equal(cue.slotMs, durations[heard[index].length]);
          heard[index].push({topic:cue.topicId, elapsedMs:Math.round(performance.now() - began)});
        }
      }
      for (const p of nonlisteners) assert.deepEqual(drain(p), []);
    }
    for (const observed of heard) {
      assert.deepEqual(observed.map(line => line.topic), topics);
      let earliest = 0;
      for (const [i, line] of observed.entries()) {
        assert.ok(line.elapsedMs >= earliest - 25, "A dialogue cue preceded its measured audio slot");
        earliest += durations[i];
      }
    }
    assert.throws(() => server.beginCampaignSceneDialogue(host.actor, questId, sceneId), /already active/);
    assert.equal(await playback.finished, "completed");
    const elapsedMs = Math.round(performance.now() - began);
    assert.ok(elapsedMs >= durations.reduce((a, b) => a + b, 0) - 10, "Final line was cut short");
    timed = {durations, host:heard[0], guest:heard[1], elapsedMs};
  }
  const interrupted = server.beginCampaignSceneDialogue(host.actor, questId, sceneId);
  await command(host, { action: "kick", target: guest.actor });
  assert.equal(interrupted.advance(1), 1);
  await sleep(100);
  assert.equal(drain(host).length, 1);
  assert.deepEqual(drain(guest), []);
  for (const p of nonlisteners) assert.deepEqual(drain(p), []);
  server.set(host.actor, "questCommand", { quest: questId, op: "stop" });
  assert.throws(() => interrupted.advance(2), /scene.quest-not-running/);
  require("node:fs").writeSync(1, JSON.stringify({ passed: true, scene: sceneId, lines: topics.length,
    connections: players.length + bystanders.length, sharedBystanders: bystanders.length, hostAndGuest: true, timed, pendingGuestCues: 0, removedGuestCues: 0, otherCampaignCues: 0 }) + "\n");
};
