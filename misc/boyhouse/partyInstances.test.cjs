const { test } = require("node:test");
const assert = require("node:assert/strict");
const { PartyInstances } = require("../../skymp5-server/ts/campaign/partyInstances.js");

function fixture() {
  const scopes = new Map([[1, ""], [2, ""], [3, ""]]);
  const outside = new Set();
  const disabled = new Set();
  const preparations = [];
  let time = 0;
  const adapter = {
    exists: id => scopes.has(id),
    getInstance: id => scopes.get(id),
    prepareInstance: (id, area) => preparations.push({ id, area: area.id, previous: scopes.get(id) }),
    setInstance: (id, key) => scopes.set(id, key),
    disable: id => disabled.add(id),
    isInside: id => !outside.has(id),
  };
  const areas = [{ id: "town", worldOrCell: 60, center: [0, 0, 0], radius: 50, encounterLevel: 5 }];
  const authority = new PartyInstances(adapter, areas, () => time);
  return { authority, scopes, outside, disabled, adapter, areas, preparations, advance: ms => time += ms };
}

test("owner recovery adopts only native-accepted exact-phase visits without preparation", () => {
  const f = fixture(); f.scopes.set(1, "campaign:1:town");
  const request = { userId: 7, handled: false };
  f.adapter.resumeOwner = (actor, area, user) => {
    assert.equal(actor, 1); assert.equal(area.id, "town"); assert.equal(user, 7);
    assert.equal(f.authority.snapshot(actor).visit, null, "visit must follow native acceptance");
    return { attempt: "resume-1", status: "Pending", actor: 1, phase: "campaign:1:town" };
  };
  f.adapter.setInstance = () => assert.fail("Resume cannot rebind the existing phase");
  f.authority.recover(1, request);
  assert.equal(request.handled, true);
  assert.deepEqual(f.authority.snapshot(1).visit, { owner: 1, area: "town" });
  assert.deepEqual(f.preparations, []);
  assert.deepEqual([...f.disabled], []);
});

test("unenrolled recovery explicitly falls back only when native admission returns null", () => {
  const f = fixture(); f.scopes.set(1, "campaign:1:town");
  const request = { userId: 7, handled: false };
  f.adapter.resumeOwner = () => null;
  f.authority.recover(1, request);
  assert.equal(request.handled, false);
  assert.equal(f.preparations.length, 1);
  assert.deepEqual(f.authority.snapshot(1).visit, { owner: 1, area: "town" });
});

test("invalid enrolled recovery and enrolled guest refusal never prepare a replacement history", () => {
  for (const saved of ["campaign:1:town", "campaign:2:town"]) {
    const f = fixture(); f.scopes.set(1, saved);
    const request = { userId: 7, handled: false };
    f.adapter.resumeOwner = () => { throw Error("enrolled context rejected"); };
    assert.throws(() => f.authority.recover(1, request), /enrolled context rejected/);
    assert.equal(request.handled, false);
    assert.equal(f.scopes.get(1), saved);
    assert.deepEqual(f.preparations, []);
    assert.equal(f.authority.snapshot(1).visit, null);
    assert.ok(f.disabled.has(1));
  }
});

test("malformed native resume acceptance cannot publish a visit or mark spawn handled", () => {
  const accepted = { actor: 1, phase: "campaign:1:town", attempt: "x", status: "Pending" };
  for (const resumed of [undefined, {}, { ...accepted, attempt: "" },
    { ...accepted, status: "Failed" }, { ...accepted, status: "unknown" },
    { ...accepted, actor: 2 }, { ...accepted, phase: "campaign:2:town" }]) {
    const f = fixture(); f.scopes.set(1, "campaign:1:town");
    const request = { userId: 7, handled: false };
    f.adapter.resumeOwner = () => resumed;
    assert.throws(() => f.authority.recover(1, request), /resume was not accepted/);
    assert.equal(request.handled, false);
    assert.deepEqual(f.preparations, []);
    assert.equal(f.authority.snapshot(1).visit, null);
    assert.ok(f.disabled.has(1));
  }
});

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

test("destination is prepared before admission, including guest revocation", () => {
  const f = fixture(), a = f.authority;
  f.adapter.setInstance = (id, key) => {
    if (key) assert.equal(f.preparations.at(-1).id, Number(key.split(":")[1]));
    f.scopes.set(id, key);
  };
  a.invite(1, 2); a.accept(2, 1);
  a.enter(1, 1, "town"); a.enter(2, 1, "town"); a.kick(1, 2);
  assert.deepEqual(f.preparations, [
    { id: 1, area: "town", previous: "" },
    { id: 1, area: "town", previous: "campaign:1:town" },
    { id: 2, area: "town", previous: "campaign:1:town" },
  ]);
  assert.equal(f.scopes.get(2), "campaign:2:town");
});

test("failed voluntary preparation leaves the previous visit active", () => {
  const f = fixture(), a = f.authority;
  a.invite(1, 2); a.accept(2, 1);
  a.enter(1, 1, "town"); a.enter(2, 2, "town");
  f.adapter.prepareInstance = () => { throw Error("incomplete destination"); };
  assert.throws(() => a.enter(2, 1, "town"), /incomplete destination/);
  assert.equal(f.scopes.get(2), "campaign:2:town");
  assert.equal(a.snapshot(2).visit.owner, 2);
  assert.equal(f.disabled.size, 0);
});

test("failed preparation during revocation or recovery quarantines the guest", () => {
  for (const recovering of [false, true]) {
    const f = fixture(), a = f.authority;
    a.invite(1, 2); a.accept(2, 1);
    a.enter(1, 1, "town"); a.enter(2, 1, "town");
    f.adapter.prepareInstance = () => { throw Error("incomplete destination"); };
    if (recovering) {
      const restarted = new PartyInstances(f.adapter, f.areas);
      assert.throws(() => restarted.recover(2), /incomplete destination/);
      assert.equal(restarted.snapshot(2).visit, null);
    } else {
      a.kick(1, 2);
      assert.equal(a.snapshot(2).visit, null);
    }
    assert.ok(f.disabled.has(2));
  }
});

test("a partially applied native transition is quarantined and has no visit", () => {
  const f = fixture(), a = f.authority;
  f.adapter.setInstance = (id, key) => { f.scopes.set(id, key); throw Error("subscription failure"); };
  assert.throws(() => a.enter(1, 1, "town"), /subscription failure/);
  assert.equal(f.scopes.get(1), "campaign:1:town");
  assert.ok(f.disabled.has(1));
  assert.equal(a.snapshot(1).visit, null);
});

test("leaving the area restores guests even while party membership remains", () => {
  const f = fixture(), a = f.authority;
  a.invite(1, 2); a.accept(2, 1);
  a.enter(1, 1, "town"); a.enter(2, 1, "town");
  f.outside.add(1);
  assert.deepEqual(a.reconcile(), [1, 2]);
  assert.equal(f.scopes.get(1), "");
  assert.equal(f.scopes.get(2), "campaign:2:town");
  assert.ok(a.sameParty(1, 2));
  f.outside.add(2);
  assert.deepEqual(a.reconcile(), [2]);
  assert.equal(f.scopes.get(2), "");
  assert.deepEqual(a.reconcile(), []);
});

test("an owner becoming a guest cannot leave visitors in the abandoned phase", () => {
  const f = fixture(), a = f.authority;
  a.invite(1, 2); a.accept(2, 1); a.invite(1, 3); a.accept(3, 1);
  a.enter(1, 1, "town"); a.enter(2, 1, "town"); a.enter(3, 3, "town");
  a.enter(1, 3, "town");
  assert.equal(f.scopes.get(1), "campaign:3:town");
  assert.equal(f.scopes.get(2), "campaign:2:town");
  assert.equal(a.snapshot(2).visit.owner, 2);
});

test("recovery outside the saved boundary returns to shared space without allocation", () => {
  const f = fixture();
  f.scopes.set(2, "campaign:1:town"); f.outside.add(2);
  f.authority.recover(2);
  assert.equal(f.scopes.get(2), "");
  assert.equal(f.authority.snapshot(2).visit, null);
  assert.deepEqual(f.preparations, []);
});

test("area limits and trusted encounter levels are validated at startup", () => {
  const f = fixture();
  for (const fields of [{ encounterLevel: undefined }, { encounterLevel: 0 },
    { encounterLevel: 1.5 }, { encounterLevel: 65536 }, { radius: 65537 },
    { worldOrCell: 0 }, { worldOrCell: 0xff000000 }, { center: [60000000, 0, 0] }]) {
    assert.throws(() => new PartyInstances(f.adapter, [{ ...f.areas[0], ...fields }]), /boundary/);
  }
  assert.throws(() => new PartyInstances({ ...f.adapter, prepareInstance: undefined }, f.areas), /preparation/);
});

test("a missing or unexpectedly moved host cannot strand authorized visitors", () => {
  for (const missing of [false, true]) {
    const f = fixture(), a = f.authority;
    a.invite(1, 2); a.accept(2, 1);
    a.enter(1, 1, "town"); a.enter(2, 1, "town");
    if (missing) f.scopes.delete(1);
    else f.scopes.set(1, "");
    a.reconcile();
    assert.equal(a.snapshot(1).visit, null);
    assert.equal(f.scopes.get(2), "campaign:2:town");
  }
});

test("invalid saved owner identities are quarantined, not normalized", () => {
  for (const owner of ["0", "01", "4294967296", "99999999999999999999999"]) {
    const f = fixture();
    f.authority.enter(1, 1, "town");
    f.scopes.set(1, `campaign:${owner}:town`);
    assert.throws(() => f.authority.recover(1), /administrator recovery/);
    assert.equal(f.authority.snapshot(1).visit, null);
    assert.ok(f.disabled.has(1));
  }
});

test("guests cannot enter a host whose client projection is pending", () => {
  const f = fixture(), a = f.authority;
  let ready = false;
  f.adapter.isReady = () => ready;
  a.invite(1, 2); a.accept(2, 1); a.enter(1, 1, "town");
  assert.throws(() => a.enter(2, 1, "town"), /owner is not/);
  ready = true; a.enter(2, 1, "town");
  ready = false;
  assert.deepEqual(a.reconcile(), [], "An existing guest is not revoked during a same-phase host refresh");
  assert.equal(f.scopes.get(2), f.scopes.get(1));
});

test("an invitation expires when its recipient leaves before accepting", () => {
  const { authority } = fixture();
  authority.enter(1, 1, "town");
  authority.invite(1, 2);
  authority.invite(1, 3);
  authority.leave(2); // Disconnect cleanup runs even for a prospective member.
  assert.equal(authority.snapshot(2).invitation, null);
  assert.throws(() => authority.accept(2, 1), /Invitation missing or expired/);
  assert.throws(() => authority.enter(2, 1, "town"), /Party membership required/);
  authority.accept(3, 1);
  authority.invite(1, 2);
  authority.accept(2, 1);
  authority.enter(2, 1, "town");
  assert.deepEqual(authority.snapshot(2).visit, { owner: 1, area: "town" });
});

test("a refused duplicate guest login leaves the active party visit intact", () => {
  const { authority, adapter, scopes, disabled } = fixture();
  authority.enter(1, 1, "town");
  authority.invite(1, 2); authority.accept(2, 1);
  authority.enter(2, 1, "town");
  adapter.assignedElsewhere = (actor, user) => actor === 2 && user === 8;
  adapter.resumeOwner = () => { throw Error("guest already assigned"); };
  assert.throws(() => authority.recover(2, { userId: 8, handled: false }), /already assigned/);
  assert.deepEqual(authority.snapshot(2).visit, { owner: 1, area: "town" });
  assert.equal(scopes.get(2), "campaign:1:town");
  assert.ok(authority.sameParty(1, 2));
  assert.equal(disabled.size, 0);
});

test("a guest cannot adopt a host snapshot as a personal recovery", () => {
  const { authority, scopes } = fixture();
  scopes.set(2, "campaign:1:town");
  assert.throws(() => authority.confirmPersonalResume(2, "campaign:1:town"));
  assert.throws(() => authority.confirmPersonalResume(2, "campaign:2:town"));
  assert.equal(authority.snapshot(2).visit, null);
});
