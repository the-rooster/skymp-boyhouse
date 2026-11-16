"use strict";
// Loaded as the gamemode of a disposable full TS server, never production.
const assert = require("node:assert/strict");
const server = globalThis.mp;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const packet = (bot, body) => bot.send({ t: 1, contentJsonDump: JSON.stringify(body) });
async function command(bot, body) {
  packet(bot, { customPacketType: "boyhouse.instance", ...body });
  await sleep(250);
}
async function login(profileId) {
  const bot = server.createBot();
  await sleep(50);
  packet(bot, { customPacketType: "loginWithSkympIo", gameData: { profileId } });
  await sleep(250);
  const actor = server.getUserActor(bot.getUserId());
  assert.ok(actor, "Full Login/Spawn pipeline must assign a character");
  return { bot, actor };
}
(async () => {
  const owner = await login(1001), guest = await login(1002), stranger = await login(1003);
  const quest = 0x4e50d, key = "4e50d:Skyrim.esm";
  const journal = player => server.get(player.actor, "questJournal");
  for (const player of [owner, guest]) server.set(player.actor, "private.boyhouse.questAdapters", [key]);
  server.set(guest.actor, "questCommand", { quest, op: "setStage", index: 5 });
  const personal = journal(guest).quests;
  const scope = player => server.get(player.actor, "worldInstance");
  await command(owner.bot, { action: "enter", area: "fixture" });
  assert.equal(scope(owner), `campaign:${owner.actor}:fixture`);
  // A supplied sender ID cannot impersonate the campaign owner.
  await command(stranger.bot, { action: "enter", actor: owner.actor, owner: owner.actor, area: "fixture" });
  assert.equal(scope(stranger), "");
  await command(owner.bot, { action: "invite", target: guest.actor });
  await command(guest.bot, { action: "accept", leader: owner.actor });
  await command(guest.bot, { action: "enter", owner: owner.actor, area: "fixture" });
  assert.equal(scope(guest), scope(owner));
  const beforeForgery = journal(owner);
  packet(stranger.bot, { customPacketType: "boyhouse.questJournal", action: "setStage", actor: owner.actor, quest, index: 200 });
  packet(guest.bot, { customPacketType: "boyhouse.questJournal", action: "setStage", quest, index: 200 });
  await sleep(250);
  assert.deepEqual(journal(owner), beforeForgery, "Clients cannot mutate quests even while admitted as a guest");
  server.set(guest.actor, "questCommand", { quest, op: "setStage", index: 10 });
  assert.equal(journal(owner).quests[key].stage, 10);
  assert.equal(journal(guest).campaignOwner, owner.actor);
  await command(stranger.bot, { action: "kick", actor: owner.actor, target: guest.actor });
  assert.equal(scope(guest), scope(owner));
  await command(owner.bot, { action: "kick", target: guest.actor });
  assert.equal(scope(guest), `campaign:${guest.actor}:fixture`);
  assert.deepEqual(journal(guest).quests, personal, "Kick restores the guest's own story");
  await command(owner.bot, { action: "invite", target: guest.actor });
  await command(guest.bot, { action: "accept", leader: owner.actor });
  await command(guest.bot, { action: "enter", owner: owner.actor, area: "fixture" });
  assert.equal(scope(guest), scope(owner));
  const ownerLocation = server.get(owner.actor, "locationalData");
  server.set(owner.actor, "locationalData", {
    ...ownerLocation, pos: [ownerLocation.pos[0] + 1024, ownerLocation.pos[1], ownerLocation.pos[2]],
  });
  await sleep(500);
  assert.equal(scope(owner), "", "Boundary polling returns departed owners to shared space");
  assert.equal(scope(guest), `campaign:${guest.actor}:fixture`, "Remaining visitors return to personal history when the host leaves");
  assert.deepEqual(journal(guest).quests, personal);
  server.set(owner.actor, "locationalData", ownerLocation);
  await command(owner.bot, { action: "enter", area: "fixture" });
  await command(guest.bot, { action: "enter", owner: owner.actor, area: "fixture" });
  assert.equal(scope(guest), scope(owner), "Host departure from an area does not dissolve the party");
  owner.bot.destroy();
  await sleep(250);
  assert.equal(scope(guest), `campaign:${guest.actor}:fixture`, "Disconnect must revoke the departed owner's guest access");
  guest.bot.destroy();
  await sleep(250);
  const returning = await login(1002);
  assert.equal(returning.actor, guest.actor);
  assert.equal(scope(returning), `campaign:${guest.actor}:fixture`);
  assert.deepEqual(journal(returning).quests, personal, "Reconnect preserves personal quest state");
  const returningUser = returning.bot.getUserId();
  server.set(returning.actor, "worldInstance", ""); // Deliberately invalidate the native visit.
  await sleep(500);
  assert.equal(server.getUserByActor(returning.actor), 65535, "Quarantine detaches native actor authority");
  assert.equal(server.isConnected(returningUser), false, "Quarantine closes the connection");
  // Existing console output is routed to native logs; stdout here stays visible.
  require("node:fs").writeSync(1, "Full server protocol smoke passed: login, forged sender/quest writes, prepared party entry, kick, boundary departure, reconnect, quarantine\n");
  process.exit(0);
})().catch(error => {
  require("node:fs").writeSync(2, `${error.stack}\n`);
  process.exit(1);
});
