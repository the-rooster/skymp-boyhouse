"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { createHash } = require("node:crypto");
const test = require("node:test");
const { loadSceneTimingIndex } = require("../../skymp5-server/ts/campaign/sceneTimingIndex");

test("dialogue timing metadata must match the actual winning plugin load order and bytes", async t => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "scene-timing-"));
  t.after(() => fs.rmSync(dir, { recursive:true, force:true }));
  const names = ["First.esm", "Override.esp"];
  const hash = s => createHash("sha256").update(s).digest("hex");
  for (const name of names) fs.writeFileSync(path.join(dir, name), name);
  const index = { schemaVersion:1, language:"en", topics:{"1:First.esm":1200},
    sources:[...names.map(file => ({file, sha256:hash(file)})), {file:"Voices_en0.bsa", sha256:hash("voices")}] };
  const file = path.join(dir, "timing.json");
  fs.writeFileSync(file, JSON.stringify(index));
  const settings = { dataDir:dir, loadOrder:[names[0], path.join(dir, names[1])] };
  assert.deepEqual(await loadSceneTimingIndex(file, settings, names), index);
  await assert.rejects(loadSceneTimingIndex(file, settings, [...names].reverse()), /load order/);
  fs.writeFileSync(path.join(dir, names[1]), "different winning data");
  await assert.rejects(loadSceneTimingIndex(file, settings, names), /source differs/);
});
