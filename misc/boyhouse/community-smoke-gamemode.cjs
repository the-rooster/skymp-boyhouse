"use strict";
// Opt-in native in-process bots. Reuses the ordinary projection smoke helpers;
// acknowledgements do not emulate Skyrim's engine application.
const assert = require("node:assert/strict");
module.exports = async function community({ server, sleep, packet, scope,
  prepared, acknowledge, login, command, finishCreation }) {
  const started = Date.now(), checkpoints = [];
  const players = await Promise.all(Array.from({ length: 32 }, (_, index) => login(32001 + index, `bot32-${index}`)));
  function connected(label) {
    assert.equal(players.length, 32);
    assert.equal(new Set(players.map(p => p.user)).size, 32);
    assert.equal(new Set(players.map(p => p.actor)).size, 32);
    for (const p of players) {
      assert.equal(server.isConnected(p.user), true, `${label}: disconnected user ${p.user}`);
      assert.equal(server.getUserActor(p.user), p.actor);
    }
    checkpoints.push({ label, simultaneouslyConnected: players.length, elapsedMs: Date.now() - started });
    require('node:fs').writeSync(1, JSON.stringify(checkpoints[checkpoints.length - 1]) + '\n');
  }
  connected('all logged in, projection still gated');
  await Promise.all(players.map(acknowledge));
  await Promise.all(players.map(finishCreation));
  connected('all32 acknowledged and appearance accepted');
  for (const p of players) {
    assert.equal(server.getGameplayBlocked(p.user), false);
    assert.equal(scope(p), '');
  }
  const [owner, guest, stranger] = players;
  const key = '4e50d:Skyrim.esm';
  for (const [p, index] of [[owner, 10], [guest, 5], [stranger, 10]]) {
    server.set(p.actor, 'private.boyhouse.questAdapters', [key]);
    server.set(p.actor, 'questCommand', { quest: 0x4e50d, op: 'setStage', index });
  }
  server.set(owner.actor, 'campaignReferences', { instanceArea: 'fixture', references: [0x1a677, 0x8a986], encounterLevel: 5 });
  await command(owner, { action: 'enter', area: 'fixture' });
  const hostProjection = await prepared(owner);
  await acknowledge(owner);
  await command(owner, { action: 'invite', target: guest.actor });
  await command(guest, { action: 'accept', leader: owner.actor });
  await command(guest, { action: 'enter', owner: owner.actor, area: 'fixture' });
  const visiting = await prepared(guest);
  const guestHostAck = await acknowledge(guest);
  assert.equal(scope(owner), `campaign:${owner.actor}:fixture`);
  assert.equal(scope(guest), scope(owner));
  assert.equal(visiting.campaignOwner, owner.actor);
  assert.equal(visiting.identity, hostProjection.identity);
  assert.deepEqual(visiting.references, hostProjection.references);
  for (const p of players.slice(2)) assert.equal(scope(p), '');
  assert.equal(server.get(guest.actor, 'questJournal').quests[key].stage, 10);
  server.set(guest.actor, 'questExecuteStage', { quest: 0x4e50d, stage: 100 });
  assert.equal(server.get(owner.actor, 'questJournal').quests[key].stage, 100);
  assert.equal(server.get(guest.actor, 'questJournal').quests[key].stage, 100);
  assert.equal(server.get(stranger.actor, 'questJournal').quests[key].stage, 10);
  connected('host and guest share phase+journal;30 remain shared');
  await command(owner, { action: 'kick', target: guest.actor });
  const personal = await prepared(guest);
  assert.equal(scope(guest), `campaign:${guest.actor}:fixture`);
  assert.equal(personal.campaignOwner, guest.actor);
  assert.notEqual(personal.identity, visiting.identity);
  packet(guest.bot, guestHostAck);
  await sleep(50);
  assert.equal(server.getGameplayBlocked(guest.user), true, 'Host-phase ACK cannot release personal projection');
  await acknowledge(guest);
  assert.equal(server.get(guest.actor, 'questJournal').quests[key].stage, 5);
  assert.equal(server.get(owner.actor, 'questJournal').quests[key].stage, 100);
  for (const p of players.slice(2)) assert.equal(scope(p), '');
  connected('guest restored to personal phase and journal');
  await command(owner, { action: 'invite', target: guest.actor });
  guest.bot.destroy();
  await sleep(200);
  const returning = await login(32002, 'bot32-returning');
  assert.equal(returning.actor, guest.actor);
  await acknowledge(returning);
  players[1] = returning;
  assert.equal(server.get(returning.actor, 'questJournal').quests[key].stage, 5);
  const personalPhase = `campaign:${guest.actor}:fixture`;
  assert.equal(scope(returning), personalPhase);
  // An unaccepted invitation from the previous connection must have expired.
  await command(returning, { action: 'accept', leader: owner.actor });
  await command(returning, { action: 'enter', owner: owner.actor, area: 'fixture' });
  assert.equal(scope(returning), personalPhase);
  await command(owner, { action: 'invite', target: returning.actor });
  await command(returning, { action: 'accept', leader: owner.actor });
  await command(returning, { action: 'enter', owner: owner.actor, area: 'fixture' });
  assert.equal((await prepared(returning)).campaignOwner, owner.actor);
  await acknowledge(returning);
  assert.equal(server.get(returning.actor, 'questJournal').quests[key].stage, 100);
  connected('returning guest rejoined host after a fresh invitation');
  await command(owner, { action: 'kick', target: returning.actor });
  assert.equal((await prepared(returning)).campaignOwner, returning.actor);
  await acknowledge(returning);
  assert.equal(scope(returning), personalPhase);
  assert.equal(server.get(returning.actor, 'questJournal').quests[key].stage, 5);
  for (const p of players) {
    assert.ok(p.replica.ready);
    assert.equal(server.getGameplayBlocked(p.user), false);
    assert.equal(server.getPacketEpoch(p.user), p.replica.packetEpoch);
  }
  for (const p of players.slice(2)) assert.equal(scope(p), '');
  connected('guest reconnected with all 32 current projections acknowledged');
  const result = {
    passed: true, kind: '32 simultaneous in-process native bots, not real Skyrim clients',
    elapsedMs: Date.now() - started, checkpoints,
    host: { actor: owner.actor, phase: scope(owner), stage: server.get(owner.actor, 'questJournal').quests[key].stage },
    guest: { actor: returning.actor, phase: scope(returning), stage: server.get(returning.actor, 'questJournal').quests[key].stage },
    sharedOthers: players.slice(2).map(p => ({ actor: p.actor, phase: scope(p), connected: server.isConnected(p.user) })),
    connections: players.map(p => ({ user: p.user, actor: p.actor, blocked: server.getGameplayBlocked(p.user), epoch: server.getPacketEpoch(p.user) })),
    limitations: ['No Skyrim engine or rendered clients', 'No sustained traffic/load benchmark', 'No server restart in this probe', 'Trusted quest fixture/fragment call is not a quest playthrough', 'No enrolled primitive movement/resume production scenario exercised'],
  };
  require("node:fs").writeSync(1, JSON.stringify(result) + "\n");
  return result;
};