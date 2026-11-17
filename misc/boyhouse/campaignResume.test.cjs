const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs"), path = require("node:path"), vm = require("node:vm");
const { EventEmitter } = require("node:events");
const { ProjectionFeed } = require("../../skymp5-server/ts/campaign/projectionFeed.js");
const { PartyInstances } = require("../../skymp5-server/ts/campaign/partyInstances.js");
const { WorldProjectionReplica } = require("../../skymp5-client/src/campaign/worldProjection.ts");

const actor = 0xff000001;
const phase = `campaign:${actor}:town`;
const area = { id: "town", worldOrCell: 60, center: [0, 0, 0], radius: 50, encounterLevel: 5 };
const manifest = () => ({ schemaVersion: 1, actor, campaignOwner: actor, instance: phase,
  contextEpoch: "1:1", identity: "a".repeat(32), references: [] });
const baseline = packetEpoch => ({ schemaVersion: 1, actor, packetEpoch, messages: [
  { t: 32, eventSources: [], updateOwnerFunctions: [], updateNeighborFunctions: [] },
  { t: 33, idx: 1, refrId: actor, isMe: true,
    transform: { pos: [0, 0, 0], rot: [0, 0, 0], worldOrCell: 60 },
    props: { inventory: { entries: [] }, isRaceMenuOpen: false }, customPropsJsonDumps: [] },
] });

function projectionFixture(initial = "Pending") {
  let time = 0, nonce = 0, epoch = "", proof = 0, connected = true, blocked = false;
  let resume = initial ? { attempt: "attempt-1", status: initial } : null;
  const packets = [], closes = [], calls = [];
  const adapter = {
    connected: () => connected, actor: () => actor, instance: () => phase,
    resume: () => resume,
    read: () => { calls.push("ordinary-manifest"); return manifest(); },
    baseline: (_user, token) => { calls.push("ordinary-baseline"); return baseline(token); },
    block: (_user, value) => { calls.push(`block:${value}`); blocked = value; },
    beginEpoch: () => { calls.push("epoch"); return epoch = (++nonce).toString(16).padStart(32, "0"); },
    epoch: () => epoch,
    captureResume: (_user, token) => {
      calls.push("capture");
      assert.equal(resume.status, "ProjectionReady");
      assert.equal(blocked, true);
      return { attempt: resume.attempt, proof: `proof-${++proof}`, manifest: manifest(), baseline: baseline(token) };
    },
    completeResume: (_user, attempt, token, capturedProof, ready) => {
      calls.push("complete");
      assert.equal(attempt, resume.attempt);
      assert.equal(token, epoch);
      assert.equal(capturedProof, `proof-${proof}`);
      assert.equal(blocked, true);
      ready();
      // Model the native post-callback check: replacement attempts cannot be
      // released by the old callback even when the numeric user slot is reused.
      if (resume?.attempt !== attempt || epoch !== token) return false;
      resume = null;
      blocked = false;
      calls.push("native-release");
      return true;
    },
    send: (_user, packet) => {
      if (packet.kind === "ready") assert.equal(blocked, true);
      packets.push(structuredClone(packet));
    },
    close: (_user, error) => { closes.push(error); connected = false; },
  };
  const feed = new ProjectionFeed(adapter, () => time);
  const replica = new WorldProjectionReplica();
  feed.connect(0); replica.begin("session");
  const hello = () => feed.packet(0, { action: "hello", protocolVersion: 2, sessionId: "session" });
  const deliver = () => { while (packets.length) assert.equal(replica.accept(packets.shift()), true); };
  return { feed, adapter, replica, packets, calls, closes, hello, deliver,
    ready: () => { resume.status = "ProjectionReady"; },
    setResume: value => { resume = value; },
    blocked: () => blocked, epochCount: () => nonce, advance: ms => time += ms };
}

test("pending resume waits for durability without rotating epochs, then releases through native proof", () => {
  const f = projectionFixture(); f.hello();
  for (let i = 0; i < 40; ++i) { f.advance(250); f.feed.poll(); }
  assert.equal(f.epochCount(), 1);
  assert.deepEqual(f.packets, []);
  assert.equal(f.calls.includes("capture"), false);
  f.ready(); f.feed.poll(); f.deliver();
  assert.equal(f.epochCount(), 2);
  assert.equal(f.blocked(), true);
  assert.equal(f.replica.ready, false);
  f.feed.packet(0, f.replica.applied(f.replica.version)); f.deliver();
  assert.equal(f.replica.ready, true);
  assert.equal(f.blocked(), false);
  assert.equal(f.calls.filter(x => x === "capture").length, 1);
  assert.equal(f.calls.filter(x => x === "native-release").length, 1);
  assert.equal(f.calls.includes("block:false"), false);
  assert.equal(f.calls.includes("ordinary-manifest"), false);
  assert.equal(f.calls.includes("ordinary-baseline"), false);
  f.feed.poll();
  assert.equal(f.epochCount(), 2, "completed attempt must not publish again");
});

test("invalidated native proof publishes a fresh generation and ignores its old acknowledgement", () => {
  const f = projectionFixture("ProjectionReady"); f.hello(); f.deliver();
  const oldAck = f.replica.applied(f.replica.version);
  const complete = f.adapter.completeResume;
  let attempts = 0;
  f.adapter.completeResume = (...args) => ++attempts === 1 ? false : complete(...args);
  f.feed.packet(0, oldAck);
  assert.equal(f.blocked(), true);
  assert.equal(f.packets.some(p => p.kind === "ready"), false);
  f.deliver();
  f.feed.packet(0, oldAck);
  assert.equal(attempts, 1);
  f.feed.packet(0, f.replica.applied(f.replica.version)); f.deliver();
  assert.equal(f.replica.ready, true);
  assert.equal(attempts, 2);
  assert.equal(f.calls.filter(x => x === "capture").length, 2);
});

test("ready callback replacement keeps the new connection blocked and alive", () => {
  const f = projectionFixture("ProjectionReady"); f.hello(); f.deliver();
  const oldAck = f.replica.applied(f.replica.version);
  const send = f.adapter.send;
  f.adapter.send = (user, packet) => {
    send(user, packet);
    if (packet.kind === "ready") {
      f.feed.disconnect(user);
      f.setResume({ attempt: "replacement", status: "Pending" });
      f.feed.connect(user);
    }
  };
  f.feed.packet(0, oldAck);
  assert.equal(f.blocked(), true);
  assert.equal(f.closes.length, 0);
  assert.equal(f.calls.includes("native-release"), false);
  assert.equal(f.calls.includes("block:false"), false);
  assert.equal(f.feed.users.get(0).session, "");
  f.hello(); f.feed.packet(0, oldAck);
  assert.equal(f.feed.users.get(0).session, "session");
  assert.equal(f.closes.length, 0);
});

test("resume failure and a failing ready send keep native input closed", () => {
  for (const mode of ["failed", "capture", "ready"]) {
    const f = projectionFixture("ProjectionReady");
    if (mode === "failed") f.setResume({ attempt: "attempt-1", status: "Failed", error: "missing copy" });
    if (mode === "capture") f.adapter.captureResume = () => { throw Error("stale owner"); };
    f.hello();
    if (mode === "ready") {
      f.deliver();
      f.adapter.send = () => { throw Error("transport failure"); };
      f.feed.packet(0, f.replica.applied(f.replica.version));
    }
    assert.equal(f.closes.length, 1, mode);
    assert.equal(f.blocked(), true, mode);
    assert.equal(f.calls.includes("native-release"), false, mode);
    assert.equal(f.calls.includes("block:false"), false, mode);
  }
});

test("an ordinary projection acknowledgement cannot bypass a newly acquired resume hold", () => {
  const f = projectionFixture(null); f.hello(); f.deliver();
  const oldAck = f.replica.applied(f.replica.version);
  f.setResume({ attempt: "new-resume", status: "Pending" });
  f.feed.packet(0, oldAck);
  assert.equal(f.blocked(), true);
  assert.equal(f.packets.length, 0);
  assert.equal(f.epochCount(), 2);
  assert.equal(f.calls.includes("block:false"), false);
  f.ready(); f.feed.poll(); f.deliver();
  f.feed.packet(0, oldAck);
  assert.equal(f.calls.includes("native-release"), false);
  f.feed.packet(0, f.replica.applied(f.replica.version)); f.deliver();
  assert.equal(f.replica.ready, true);
});

test("repeated hello and pending polling cannot extend the resume deadline", () => {
  const f = projectionFixture(); f.hello();
  f.advance(29999); f.hello(); f.feed.poll();
  assert.equal(f.closes.length, 0);
  f.advance(1); f.feed.poll();
  assert.equal(f.closes.length, 1);
  assert.equal(f.blocked(), true);
  assert.equal(f.epochCount(), 1);
});

const serverRoot = path.resolve(__dirname, "../../skymp5-server");
const ts = require(require.resolve("typescript", {
  paths: [serverRoot, path.resolve(serverRoot, "../skymp5-client")],
}));
function loadSystem(name, dependencies) {
  const code = ts.transpileModule(fs.readFileSync(path.join(serverRoot, "ts/systems", name), "utf8"), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 },
  }).outputText;
  const module = { exports: {} };
  vm.runInNewContext(code, { module, exports: module.exports, console: { log() {}, error() {} },
    require: id => { assert.ok(id in dependencies, `Unexpected import ${id}`); return dependencies[id]; },
  }, { filename: name });
  return module.exports;
}

async function systemsFixture(resumeResult = { attempt: "attempt-1", status: "Pending" }) {
  const calls = [], gm = new EventEmitter();
  let assigned = 0, connected = true;
  const settings = { allSettings: { boyhouseInstances: { experimental: true, projection: true, areas: [area] } },
    startPoints: [{ pos: [0, 0, 0], angleZ: 0, worldOrCell: 60 }] };
  const dependencies = { "../settings": { Settings: { get: async () => settings } },
    "../campaign/partyInstances": { PartyInstances }, "../campaign/projectionFeed": { ProjectionFeed } };
  const { CampaignInstances } = loadSystem("campaignInstances.ts", dependencies);
  const { Spawn } = loadSystem("spawn.ts", dependencies);
  const svr = {
    get: (_id, key) => ({ type: "MpActor", worldInstance: phase, campaignReferences: manifest() })[key],
    set: (_id, key, value) => calls.push(["set", key, value]),
    getActorsByProfileId: () => [actor],
    getUserActor: () => assigned,
    getUserByActor: () => assigned ? 0 : 0xffffffff,
    isConnected: user => user === 0 && connected,
    getGameplayBlocked: () => true,
    getActorPos: () => [0, 0, 0], getActorCellOrWorld: () => 60,
    setEnabled: (_id, enabled) => calls.push(["enabled", enabled]),
    setUserActor: (_user, id) => { calls.push(["assign", id]); assigned = id; },
    resumeCampaignActor: (user, id, expected, definition) => {
      calls.push(["resume", user, id, expected, structuredClone(definition)]);
      if (resumeResult instanceof Error) throw resumeResult;
      if (resumeResult) assigned = id;
      return resumeResult;
    },
    detachCampaignActor: () => { calls.push(["detach"]); return resumeResult !== null; },
    setCampaignVisibilityCallbacks() {},
    sendCustomPacket: () => calls.push(["send"]),
    kick: user => { calls.push(["kick", user]); connected = false; },
  };
  const ctx = { svr, gm };
  const instances = new CampaignInstances(), spawn = new Spawn(() => {});
  await instances.initAsync(ctx); await spawn.initAsync(ctx);
  return { calls, ctx, instances, spawn, fire: () => svr._onSpawnAllowed(0, 1, ["role"]) };
}

test("real spawn hook adopts the native resume without enable, rebind, or allocation", async () => {
  const f = await systemsFixture(); f.fire();
  assert.deepEqual(f.calls, [
    ["resume", 0, actor, phase, { id: "town", worldOrCell: 60, center: [0, 0, 0], radius: 50 }],
    ["set", "private.discordRoles", ["role"]],
  ]);
  assert.deepEqual(f.instances.authority.snapshot(actor).visit, { owner: actor, area: "town" });
});

test("unenrolled spawn retains preparation, enabling, assignment, and ordinary disconnect", async () => {
  const f = await systemsFixture(null); f.fire();
  assert.deepEqual(f.calls.map(c => c[0] === "set" ? c[1] : c[0]), [
    "resume", "campaignReferences", "worldInstance", "enabled", "assign", "private.discordRoles",
  ]);
  f.calls.length = 0;
  f.spawn.disconnect(0, f.ctx);
  assert.deepEqual(f.calls, [["detach"], ["enabled", false]]);
});

test("failed resume before assignment closes the requesting user without enabling or rebinding", async () => {
  const f = await systemsFixture(Error("missing saved allocation"));
  assert.throws(f.fire, /missing saved allocation/);
  assert.equal(f.calls.some(c => c[0] === "assign" || (c[0] === "enabled" && c[1] === true)), false);
  assert.equal(f.calls.some(c => c[0] === "set"), false);
  assert.ok(f.calls.some(c => c[0] === "kick" && c[1] === 0));
});

test("disconnect preserves party cleanup order and routes enrolled actors through native detach", async () => {
  const f = await systemsFixture(); f.fire(); f.calls.length = 0;
  const leave = f.instances.authority.leave.bind(f.instances.authority);
  f.instances.authority.leave = id => { f.calls.push(["party-leave"]); leave(id); };
  f.instances.disconnect(0, f.ctx);
  f.spawn.disconnect(0, f.ctx);
  assert.deepEqual(f.calls, [["party-leave"], ["detach"]]);
});
