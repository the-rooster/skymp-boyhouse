"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const { playTimedScene } = require("../../skymp5-server/ts/campaign/sceneDialogueClock");

const desc = id => id.toString(16) + ":Skyrim.esm";
function setup() {
  const ids = [0x17f2b, 0x17f2a, 0x17f29];
  const durations = [1700, 2800, 350];
  const timing = {
    schemaVersion: 1, language: "en",
    sources: [{ file: "Skyrim.esm", sha256: "a".repeat(64) }],
    topics: Object.fromEntries(ids.map((id, i) => [desc(id), durations[i]])),
  };
  const timers = new Map(), sent = [];
  let seq = 0, now = 0;
  const clock = {
    now: () => now,
    setTimeout(fn, ms) {
      timers.set(++seq, { fn, deadline: now + ms });
      return seq;
    },
    clearTimeout(handle) { timers.delete(handle); },
  };
  // Dispatch one queued timer as the event loop would, with a chosen offset.
  // An early dispatch simulates clock granularity or a stale host wakeup.
  function wake(elapsed) {
    assert.equal(timers.size, 1);
    const [id, timer] = timers.entries().next().value;
    timers.delete(id);
    now += elapsed;
    timer.fn();
  }
  let next = 0;
  const cursor = {
    active: true, valid: true, cancels: 0, topicIds: ids,
    get nextActionIndex() { return next < ids.length ? next + 1 : null; },
    get nextTopicId() { return ids[next] ?? null; },
    assertCurrent() { if (!this.active || !this.valid) throw Error("Quest or epoch changed"); },
    cancel() { this.active = false; ++this.cancels; },
    advance(action, durationMs) {
      this.assertCurrent();
      assert.equal(action, this.nextActionIndex);
      sent.push({ action, time: now, durationMs });
      ++next;
      return 1;
    },
  };
  return { cursor, timing, clock, timers, sent, wake,
    play: () => playTimedScene(cursor, timing, desc, clock) };
}

test("late timer sends one line and starts a fresh measured interval", async () => {
  const f = setup();
  const run = f.play();
  assert.deepEqual(f.sent, [{ action: 1, time: 0, durationMs: 1700 }]);
  f.wake(0);
  f.wake(1699);
  assert.equal(f.sent.length, 1);
  f.wake(8000); // Plenty of time has elapsed for every line in the scene.
  assert.deepEqual(f.sent.at(-1), { action: 2, time: 9699, durationMs: 2800 });
  assert.equal(f.sent.length, 2);
  f.wake(2799);
  assert.equal(f.sent.length, 2);
  f.wake(1);
  assert.deepEqual(f.sent.at(-1), { action: 3, time: 12499, durationMs: 350 });
  assert.equal(f.cursor.active, true);
  f.wake(349);
  assert.equal(f.cursor.active, true);
  f.wake(1);
  assert.equal(await run.finished, "completed");
  assert.equal(f.cursor.cancels, 1);
  assert.equal(f.timers.size, 0);
  run.cancel();
  assert.equal(f.cursor.cancels, 1);
});

test("index and every planned duration are checked before publishing", () => {
  for (const breakIndex of [
    x => { delete x.topics[desc(0x17f29)]; },
    x => { x.topics[desc(0x17f29)] = 0; },
    x => { x.topics[desc(0x17f29)] = 1.9; },
    x => { x.topics[desc(0x17f29)] = Number.MAX_SAFE_INTEGER + 1; },
    x => { x.topics["017f29:Skyrim.esm"] = 100; },
    x => { x.language = "fr"; },
    x => { x.schemaVersion = 2; },
    x => { x.sources = []; },
    x => { x.sources[0].sha256 = "short"; },
    x => { x.sources.push({ ...x.sources[0], file: "skyrim.esm" }); },
  ]) {
    const f = setup();
    breakIndex(f.timing);
    assert.throws(() => f.play(), /timing|duration/i);
    assert.equal(f.sent.length, 0);
    assert.equal(f.timers.size, 0);
    assert.equal(f.cursor.cancels, 1);
  }
});

test("a changed quest or projection aborts pending and trailing audio intervals", async () => {
  for (const beforeFinalEnd of [false, true]) {
    const f = setup();
    const run = f.play();
    if (beforeFinalEnd) { f.wake(1700); f.wake(2800); }
    f.cursor.valid = false;
    f.wake(2000);
    await assert.rejects(run.finished, /quest or epoch changed/i);
    assert.equal(f.sent.length, beforeFinalEnd ? 3 : 1);
    assert.equal(f.timers.size, 0);
    assert.equal(f.cursor.cancels, 1);
  }
});

test("cancellation releases the cursor once and ignores already-queued callbacks", async () => {
  const f = setup();
  const run = f.play();
  const stale = [...f.timers.values()][0].fn;
  run.cancel();
  run.cancel();
  stale();
  assert.equal(await run.finished, "cancelled");
  assert.equal(f.timers.size, 0);
  assert.equal(f.cursor.cancels, 1);
  assert.equal(f.sent.length, 1);
});

test("failed dispatch, external cursor advances and backwards time never replay a cue", async () => {
  for (const failure of ["transport", "empty", "external", "clock"]) {
    const f = setup();
    const run = f.play();
    if (failure === "transport") f.cursor.advance = () => { throw Error("partial delivery"); };
    if (failure === "empty") f.cursor.advance = () => 0;
    if (failure === "external") f.cursor.advance(f.cursor.nextActionIndex);
    f.wake(failure === "clock" ? -1 : 1700);
    await assert.rejects(run.finished, /partial delivery|no participant|action changed|monotonic/);
    assert.equal(f.sent.length, failure === "external" ? 2 : 1);
    assert.equal(f.timers.size, 0);
    assert.equal(f.cursor.cancels, 1);
  }
});

test("first dispatch failure is synchronous and releases the cursor", () => {
  const f = setup();
  f.cursor.advance = () => { throw Error("closed transport"); };
  assert.throws(f.play, /closed transport/);
  assert.equal(f.timers.size, 0);
  assert.equal(f.cursor.cancels, 1);
});

test("large measured durations are chunked without early delivery", async () => {
  const f = setup();
  const max = 0x7fffffff;
  f.timing.topics[desc(f.cursor.topicIds[0])] = max + 10;
  const run = f.play();
  assert.equal([...f.timers.values()][0].deadline, max);
  f.wake(max);
  assert.equal(f.sent.length, 1);
  f.wake(10);
  assert.equal(f.sent.length, 2);
  run.cancel();
  assert.equal(await run.finished, "cancelled");
});
