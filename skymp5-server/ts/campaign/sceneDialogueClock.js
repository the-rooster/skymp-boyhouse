"use strict";

const realtime = { now: () => performance.now(), setTimeout, clearTimeout };
const maxTimeout = 0x7fffffff; // Node treats larger delays as a 1 ms timeout.
const topicKey = /^[1-9a-f][a-f0-9]{0,5}:[^/\\:\x00-\x1f]+\.[eE][sS][mMpPlL]$/;

function topicDurations(index, ids, descFromId) {
  if (index?.schemaVersion !== 1 || index.language !== "en" ||
      !Array.isArray(index.sources) || !index.sources.length ||
      !index.topics || typeof index.topics !== "object" || Array.isArray(index.topics))
    throw Error("Invalid English scene timing index");
  const names = new Set();
  for (const source of index.sources) {
    if (!source || typeof source.file !== "string" ||
        !/^[^/\\:\x00-\x1f]+$/.test(source.file) ||
        typeof source.sha256 !== "string" || !/^[a-f0-9]{64}$/.test(source.sha256) ||
        names.has(source.file.toLowerCase()))
      throw Error("Invalid scene timing source");
    names.add(source.file.toLowerCase());
  }
  for (const [key, value] of Object.entries(index.topics)) {
    if (!topicKey.test(key) || !Number.isSafeInteger(value) || value <= 0)
      throw Error("Invalid scene topic duration for " + key);
  }
  return ids.map(id => {
    if (!Number.isSafeInteger(id) || id <= 0 || id >= 0xff000000)
      throw Error("Invalid planned scene topic");
    const key = descFromId(id);
    if (typeof key !== "string" || !topicKey.test(key) || !Object.hasOwn(index.topics, key))
      throw Error("Missing measured scene duration for " + key);
    return index.topics[key];
  });
}

// The caller has already verified the index hashes against its loaded content.
// The cursor retains the projection's scene lock until the final line ends.
function playTimedScene(cursor, timingIndex, descFromId, clock = realtime) {
  let timer;
  let generation = 0;
  let stopped = false;
  let resolveFinished, rejectFinished;
  const finished = new Promise((resolve, reject) => {
    resolveFinished = resolve;
    rejectFinished = reject;
  });

  function release() {
    stopped = true;
    ++generation;
    try {
      if (timer !== undefined) clock.clearTimeout(timer);
    } finally {
      cursor.cancel();
    }
  }
  function finish(outcome, error) {
    if (stopped) return;
    try {
      release();
    } catch (cleanupError) {
      error ??= cleanupError;
    }
    if (error) rejectFinished(error);
    else resolveFinished(outcome);
  }
  function cancel() { finish("cancelled"); }

  try {
    if (typeof clock?.now !== "function" || typeof clock.setTimeout !== "function" ||
        typeof clock.clearTimeout !== "function" || typeof cursor.assertCurrent !== "function")
      throw Error("Invalid scene clock or cursor");
    const ids = Array.from(cursor.topicIds);
    if (!ids.length) throw Error("Scene has no topics to play");
    const durations = topicDurations(timingIndex, ids, descFromId);
    let next = 0;
    let lastTime = -Infinity;
    let began;
    let expected = cursor.nextActionIndex;
    function time() {
      const now = clock.now();
      if (!Number.isFinite(now) || now < lastTime)
        throw Error("Invalid monotonic scene clock");
      lastTime = now;
      return now;
    }

    function schedule(delay) {
      const token = ++generation;
      timer = clock.setTimeout(() => {
        if (stopped || generation !== token) return;
        ++generation;
        timer = undefined;
        try {
          cursor.assertCurrent();
          const remaining = durations[next - 1] - (time() - began);
          if (remaining > 0) {
            schedule(remaining);
          } else if (next === ids.length) {
            if (cursor.nextActionIndex !== null || cursor.nextTopicId !== null)
              throw Error("Unexpected pending dialogue action");
            finish("completed");
          } else {
            dispatch();
          }
        } catch (error) {
          finish(undefined, error);
        }
      }, Math.min(maxTimeout, Math.ceil(delay)));
    }
    function dispatch() {
      if (!Number.isSafeInteger(expected) || expected < 0 ||
          cursor.nextActionIndex !== expected || cursor.nextTopicId !== ids[next])
        throw Error("Timed dialogue action changed");
      const duration = durations[next];
      const delivered = cursor.advance(expected, duration);
      if (stopped) return;
      if (!Number.isSafeInteger(delivered) || delivered <= 0)
        throw Error("Dialogue cue reached no participant");
      expected = cursor.nextActionIndex;
      // A delayed callback sends only this cue. Give it its full measured
      // duration from actual delivery before considering the next action.
      began = time();
      ++next;
      schedule(duration);
    }

    cursor.assertCurrent();
    time();
    dispatch();
    return { cancel, finished };
  } catch (error) {
    release();
    throw error;
  }
}

module.exports = { playTimedScene };
