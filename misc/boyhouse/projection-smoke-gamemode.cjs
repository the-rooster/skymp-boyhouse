"use strict";
// Disposable native server/control-plane integration. Bots consume serialized
// server packets; they do not emulate Skyrim's engine application.
const assert = require("node:assert/strict");
const { WorldProjectionReplica } = require("../../skymp5-client/src/campaign/worldProjection.ts");
const server = globalThis.mp;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const packet = (bot, content) => bot.send({ t: 1, contentJsonDump: JSON.stringify(content) });
const scope = p => server.get(p.actor, "worldInstance");
function receive(p) {
  for (const dump of p.bot.pollCustomPackets()) {
    const value = JSON.parse(dump);
    if (value.customPacketType === "boyhouse.projection") assert.equal(p.replica.accept(value), true);
  }
}
async function prepared(p) {
  for (let i = 0; i < 80; ++i) {
    receive(p);
    if (p.replica.prepared()) {
      const baseline = p.replica.preparedBaseline();
      assert.equal(baseline.packetEpoch, server.getPacketEpoch(p.user));
      assert.equal(baseline.actor, p.actor);
      assert.equal(baseline.messages[1].isMe, true);
      assert.equal(baseline.messages[1].refrId, p.actor);
      assert.equal(JSON.stringify(baseline).includes('private.boyhouse'), false);
      return p.replica.prepared();
    }
    await sleep(25);
  }
  throw Error("No projection manifest arrived");
}
async function acknowledge(p) {
  await prepared(p);
  const ack = p.replica.applied(p.replica.version);
  packet(p.bot, ack);
  for (let i = 0; i < 80; ++i) {
    await sleep(25); receive(p);
    if (p.replica.ready) {
      assert.equal(server.getGameplayBlocked(p.user), false);
      return ack;
    }
  }
  throw Error("Projection acknowledgement was not released");
}
async function login(profile, session) {
  const bot = server.createBot(true); await sleep(50);
  const user = bot.getUserId();
  assert.equal(server.getGameplayBlocked(user), true, "Native gameplay gate starts before login");
  const replica = new WorldProjectionReplica(); replica.begin(session);
  packet(bot, { customPacketType: "boyhouse.projection", protocolVersion: 2, action: "hello", sessionId: session });
  packet(bot, { customPacketType: "loginWithSkympIo", gameData: { profileId: profile } });
  await sleep(300);
  const actor = server.getUserActor(user); assert.ok(actor);
  const player = { bot, user, actor, replica };
  await prepared(player);
  assert.equal(server.getGameplayBlocked(user), true);
  return player;
}
async function command(p, body) {
  p.bot.send({ t: 34, epoch: p.replica.packetEpoch, message: {
    t: 1, contentJsonDump: JSON.stringify({ customPacketType: "boyhouse.instance", ...body }),
  } }); await sleep(250);
}
async function finishCreation(p) {
  if (server.get(p.actor, "appearance")) return;
  const data = {
    isFemale: false, raceId: 0x13746, weight: 50, skinColor: 0, hairColor: 0,
    headpartIds: [], headTextureSetId: 0, options: Array(19).fill(0),
    presets: Array(4).fill(0), tints: [], name: `Projection ${p.actor}`,
  };
  p.bot.send({ t: 34, epoch: p.replica.packetEpoch, message: {
    t: 4, idx: server.get(p.actor, "idx"), data,
  } });
  await sleep(100);
  assert.deepEqual(server.get(p.actor, "appearance"), data,
    "Character creation must complete in the released projection epoch");
}

(async () => {
  const owner = await login(2001, "owner");
  const first = owner.replica.prepared();
  assert.equal(first.instance, "");
  await command(owner, { action: "enter", area: "fixture" });
  assert.equal(scope(owner), "", "Custom gameplay commands are blocked before acknowledgement");
  await acknowledge(owner);
  await command(owner, { action: "enter", area: "fixture" });
  assert.equal(scope(owner), "", "Character creation still blocks gameplay after projection release");
  await finishCreation(owner);
  packet(owner.bot, { customPacketType: "boyhouse.instance", action: "enter", area: "fixture" });
  await sleep(250);
  assert.equal(scope(owner), "", "Untagged input cannot bypass the released epoch");
  const guest = await login(2002, "guest"); await acknowledge(guest); await finishCreation(guest);
  const stranger = await login(2003, "stranger"); await acknowledge(stranger); await finishCreation(stranger);
  const key = "4e50d:Skyrim.esm";
  for (const [p, index] of [[owner, 10], [guest, 5]]) {
    server.set(p.actor, "private.boyhouse.questAdapters", [key]);
    server.set(p.actor, "questCommand", { quest: 0x4e50d, op: "setStage", index });
  }
  server.set(owner.actor, "campaignReferences", {
    instanceArea: "fixture", references: [0x1a677, 0x8a986], encounterLevel: 5,
  });
  await command(owner, { action: "enter", area: "fixture" });
  const hostProjection = await prepared(owner);
  assert.equal(hostProjection.references.length, 2);
  assert.equal(server.getGameplayBlocked(owner.user), true);
  const hostAck = owner.replica.applied(owner.replica.version);
  packet(stranger.bot, hostAck); await sleep(50);
  assert.equal(server.getGameplayBlocked(owner.user), true, "Another connection cannot acknowledge the host");
  packet(owner.bot, hostAck); await sleep(50); receive(owner);
  assert.equal(owner.replica.ready, true);
  assert.equal(server.getGameplayBlocked(owner.user), false);
  await command(owner, { action: "invite", target: guest.actor });
  await command(guest, { action: "accept", leader: owner.actor });
  await command(guest, { action: "enter", owner: owner.actor, area: "fixture" });
  const visiting = await prepared(guest);
  assert.equal(visiting.campaignOwner, owner.actor);
  assert.equal(visiting.identity, hostProjection.identity);
  assert.deepEqual(visiting.references, hostProjection.references);
  const oldGuestAck = await acknowledge(guest);
  const location = server.get(owner.actor, "locationalData");
  server.set(owner.actor, "locationalData", { ...location,
    pos: [location.pos[0] + 1024, location.pos[1], location.pos[2]] });
  await sleep(500);
  assert.equal(scope(owner), "");
  assert.equal(scope(guest), `campaign:${guest.actor}:fixture`);
  assert.equal(server.getGameplayBlocked(owner.user), true);
  assert.equal(server.getGameplayBlocked(guest.user), true);
  const personal = await prepared(guest);
  assert.equal(personal.campaignOwner, guest.actor);
  assert.notEqual(personal.identity, visiting.identity);
  packet(guest.bot, oldGuestAck); await sleep(50);
  assert.equal(server.getGameplayBlocked(guest.user), true);
  await acknowledge(guest); await acknowledge(owner);
  assert.equal(server.get(guest.actor, "questJournal").quests[key].stage, 5);
  guest.bot.destroy(); await sleep(100);
  const returning = await login(2002, "returning");
  assert.equal(returning.actor, guest.actor);
  packet(returning.bot, oldGuestAck); await sleep(50);
  assert.equal(server.getGameplayBlocked(returning.user), true);
  await acknowledge(returning);
  assert.throws(() => server.setGameplayBlocked(65535, false));
  assert.throws(() => server.setGameplayBlocked(owner.user, "false"));
  require("node:fs").writeSync(1, "Projection protocol smoke passed: gated login, native manifests, forged/stale acknowledgements, host/guest handoff, boundary recovery and reconnect\n");
  process.exit(0);
})().catch(error => {
  require("node:fs").writeSync(2, `${error.stack}\n`); process.exit(1);
});
