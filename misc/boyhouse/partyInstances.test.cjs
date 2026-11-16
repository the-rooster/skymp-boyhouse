const { test } = require("node:test");
const assert = require("node:assert/strict");
const { PartyInstances } = require("../../skymp5-server/ts/campaign/partyInstances.js");

function fixture() {
  const scopes = new Map([[1, ""], [2, ""], [3, ""]]);
  const outside = new Set();
  const disabled = new Set();
  let time = 0;
  const adapter = {
    exists: id => scopes.has(id),
    getInstance: id => scopes.get(id),
    setInstance: (id, key) => scopes.set(id, key),
    disable: id => disabled.add(id),
    isInside: id => !outside.has(id),
  };
  const areas = [{ id: "town", worldOrCell: 60, center: [0, 0, 0], radius: 50 }];
  const authority = new PartyInstances(adapter, areas, () => time);
  return { authority, scopes, outside, disabled, adapter, areas, advance: ms => time += ms };
}

test("a matching history never substitutes for party membership", () => {
  const { authority: a, scopes } = fixture();
  a.enter(1, 1, "town");
  assert.throws(() => a.enter(2, 1, "town"), /Party membership/);
  a.enter(2, 2, "town");
  assert.notEqual(scopes.get(1), scopes.get(2));
  assert.equal(scopes.get(3), "");
});
test("late party join requires an accepted invite and explicit entry", () => {
  const { authority: a, scopes } = fixture();
  a.enter(1, 1, "town");
  a.invite(1, 2);
  assert.throws(() => a.enter(2, 1, "town"));
  a.accept(2, 1);
  assert.equal(scopes.get(2), "");
  a.enter(2, 1, "town");
  assert.equal(scopes.get(1), scopes.get(2));
  assert.equal(scopes.get(3), "");
});
test("kicked guests immediately return to their own instance", () => {
  const { authority: a, scopes } = fixture();
  a.invite(1, 2); a.accept(2, 1);
  a.enter(1, 1, "town"); a.enter(2, 1, "town");
  a.kick(1, 2);
  assert.equal(scopes.get(2), "campaign:2:town");
  assert.throws(() => a.enter(2, 1, "town"), /Party membership/);
});
test("leader departure revokes guests and outstanding invitations", () => {
  const { authority: a, scopes } = fixture();
  a.invite(1, 2); a.accept(2, 1); a.invite(1, 3);
  a.enter(1, 1, "town"); a.enter(2, 1, "town");
  a.leave(1);
  assert.equal(scopes.get(2), "campaign:2:town");
  assert.throws(() => a.accept(3, 1));
});
test("an instance owner need not be the party leader", () => {
  const { authority: a, scopes } = fixture();
  a.invite(1, 2); a.accept(2, 1);
  a.enter(2, 2, "town"); a.enter(1, 2, "town");
  a.leave(2);
  assert.equal(scopes.get(1), "campaign:1:town");
});
test("restart cannot restore a guest into an unauthorized campaign", () => {
  const f = fixture();
  f.scopes.set(2, "campaign:1:town");
  const fresh = new PartyInstances(f.adapter, f.areas);
  fresh.recover(2);
  assert.equal(f.scopes.get(2), "campaign:2:town");
});
test("unknown saved scopes quarantine the character", () => {
  const { authority: a, scopes, disabled } = fixture();
  scopes.set(1, "foreign:location");
  assert.throws(() => a.recover(1), /administrator recovery/);
  assert.ok(disabled.has(1));
});
test("failed revocation disables the guest rather than preserving access", () => {
  const f = fixture(), a = f.authority;
  a.invite(1, 2); a.accept(2, 1);
  a.enter(1, 1, "town"); a.enter(2, 1, "town");
  f.adapter.setInstance = () => { throw Error("native failure"); };
  a.kick(1, 2);
  assert.ok(f.disabled.has(2));
  assert.equal(a.snapshot(2).leader, null);
});
test("entry validates location and guest owner presence", () => {
  const { authority: a, outside } = fixture();
  a.invite(1, 2); a.accept(2, 1);
  assert.throws(() => a.enter(2, 1, "town"), /owner is not/);
  outside.add(1);
  assert.throws(() => a.enter(1, 1, "town"), /outside/);
  assert.throws(() => a.enter(2, 2, "unknown"), /not enabled/);
});
test("exit requires crossing back outside the altered area", () => {
  const { authority: a, outside, scopes } = fixture();
  a.enter(1, 1, "town");
  assert.throws(() => a.exit(1), /Leave the altered area/);
  outside.add(1); a.exit(1);
  assert.equal(scopes.get(1), "");
});
test("invitations expire and can only be accepted by their recipient", () => {
  const { authority: a, advance } = fixture();
  a.invite(1, 2);
  assert.throws(() => a.accept(3, 1));
  advance(60001);
  assert.throws(() => a.accept(2, 1), /expired/);
});
test("members cannot invite or kick as the leader", () => {
  const { authority: a } = fixture();
  a.invite(1, 2); a.accept(2, 1);
  assert.throws(() => a.invite(2, 3), /leader/);
  assert.throws(() => a.kick(2, 1), /leader/);
});
test("invalid actor identities and invalid area config are rejected", () => {
  const f = fixture();
  for (const id of [0, -1, 1.5, "1", NaN, 2 ** 32, 4]) assert.throws(() => f.authority.enter(id, 1, "town"));
  assert.throws(() => new PartyInstances(f.adapter, [{ ...f.areas[0], radius: -1 }]));
  assert.throws(() => new PartyInstances(f.adapter, [f.areas[0], f.areas[0]]));
});
test("snapshot mutation does not change party state", () => {
  const { authority: a } = fixture();
  a.invite(1, 2); a.accept(2, 1); a.enter(1, 1, "town");
  const snapshot = a.snapshot(1);
  snapshot.members.length = 0; snapshot.visit.owner = 3;
  assert.equal(a.snapshot(1).members.length, 2);
  assert.equal(a.snapshot(1).visit.owner, 1);
});
