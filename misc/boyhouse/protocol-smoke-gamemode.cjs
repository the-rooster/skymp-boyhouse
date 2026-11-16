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
  await command(stranger.bot, { action: "kick", actor: owner.actor, target: guest.actor });
  assert.equal(scope(guest), scope(owner));
  await command(owner.bot, { action: "kick", target: guest.actor });
  assert.equal(scope(guest), `campaign:${guest.actor}:fixture`);
  await command(owner.bot, { action: "invite", target: guest.actor });
  await command(guest.bot, { action: "accept", leader: owner.actor });
  await command(guest.bot, { action: "enter", owner: owner.actor, area: "fixture" });
  assert.equal(scope(guest), scope(owner));
  owner.bot.destroy();
  await sleep(250);
  assert.equal(scope(guest), `campaign:${guest.actor}:fixture`, "Disconnect must revoke the departed owner's guest access");
  guest.bot.destroy();
  await sleep(250);
  const returning = await login(1002);
  assert.equal(returning.actor, guest.actor);
  assert.equal(scope(returning), `campaign:${guest.actor}:fixture`);
  // Existing console output is routed to native logs; stdout here stays visible.
  require("node:fs").writeSync(1, "Full server protocol smoke passed: login, forged sender, invite, join, kick, disconnect, reconnect\n");
  process.exit(0);
})().catch(error => {
  require("node:fs").writeSync(2, `${error.stack}\n`);
  process.exit(1);
});
