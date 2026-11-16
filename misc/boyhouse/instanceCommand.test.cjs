const { test } = require("node:test");
const assert = require("node:assert/strict");
const { parseInstanceCommand: parse } = require("../../skymp5-client/src/campaign/instanceCommand.ts");
test("commands encode intent, never an authenticated sender", () => {
  assert.deepEqual(parse("bh invite 0xff000002"), { customPacketType: "boyhouse.instance", action: "invite", target: 0xff000002 });
  assert.equal(parse("bh accept 123").leader, 123);
  assert.deepEqual(parse("bh enter test 123"), { customPacketType: "boyhouse.instance", action: "enter", area: "test", owner: 123 });
  assert.equal(parse("bh enter test").owner, undefined);
});
test("malformed and ambiguous commands cannot become requests", () => {
  for (const input of ["bh invite -1", "bh invite 12junk", "bh invite ff00", "bh invite 0", "bh invite 4294967296", "bh status extra", "bh enter ../x", "bh setOwner 2", "bh accept", "wrong status"]) assert.throws(() => parse(input));
});
