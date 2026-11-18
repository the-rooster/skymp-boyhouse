"use strict";

const fs = require("node:fs");
const path = require("node:path");
const { createHash } = require("node:crypto");

// The server needs the metadata index and its own matching masters. The voice
// archive used to measure those timings can remain on the generating machine.
async function loadSceneTimingIndex(file, settings, nativeOrder) {
  if (typeof file !== "string" || !file || typeof settings?.dataDir !== "string")
    throw Error("Expected a dialogue timing index file and game data directory");
  const index = JSON.parse(await fs.promises.readFile(file, "utf8"));
  if (index?.schemaVersion !== 1 || !Array.isArray(index.sources) ||
      index.sources.some(s => typeof s?.file !== "string" || !/^[a-f0-9]{64}$/.test(s.sha256)))
    throw Error("Invalid dialogue timing source manifest");
  const masters = index.sources.filter(s => /\.(esm|esp|esl)$/i.test(s.file));
  const files = settings.loadOrder ?? nativeOrder;
  if (!Array.isArray(files) || files.length !== masters.length || nativeOrder.length !== masters.length ||
      !masters.length || !index.sources.some(s => /\.bsa$/i.test(s.file)))
    throw Error("Dialogue timing load order differs from the server");
  for (const [i, source] of masters.entries()) {
    if (source.file !== nativeOrder[i] || typeof files[i] !== "string" || path.basename(files[i]) !== source.file)
      throw Error("Dialogue timing load order differs from the server");
    const hash = createHash("sha256");
    for await (const chunk of fs.createReadStream(path.resolve(settings.dataDir, files[i]))) hash.update(chunk);
    if (hash.digest("hex") !== source.sha256) throw Error("Dialogue timing source differs: " + source.file);
  }
  return index;
}

module.exports = { loadSceneTimingIndex };
