// 合成原生对照及拒绝路径；不读取游戏数据。 | Synthetic native oracle and rejection cases; no game data.
import assert from "node:assert/strict";
import test from "node:test";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { createCascPrebuiltOverlayTransform, createCascOverlayTransform } from "../overlay.mjs";
const fixture = spawnSync("python3", [new URL("prebuilt-fixture.py", import.meta.url).pathname], {
  encoding: "utf8",
});
assert.equal(fixture.status, 0, fixture.stderr);
const base = JSON.parse(fixture.stdout);
const dependencies = {
  md5: (bytes) => new Uint8Array(createHash("md5").update(bytes).digest()),
  compress(bytes) {
    const result = spawnSync(
      "python3",
      ["-c", "import sys,zlib;sys.stdout.buffer.write(zlib.compress(sys.stdin.buffer.read(),9))"],
      { input: bytes },
    );
    assert.equal(result.status, 0);
    return new Uint8Array(result.stdout);
  },
};
const generate = createCascPrebuiltOverlayTransform(dependencies);
function request(external = structuredClone(base.external), jobs = base.jobs) {
  const parts = base.parts.map((p) => Buffer.from(p, "base64"));
  parts.push(Buffer.from(JSON.stringify(external)));
  return [
    new Uint8Array(Buffer.concat(parts)),
    { parts: parts.map((p) => p.length), jobs, location: [0, 2] },
  ];
}
test("external references match unmodified native overlay bytes and compact receipt", async () => {
  const [payload, options] = request(),
    before = payload.slice();
  const result = await generate(payload, options);
  for (const [name, bytes] of Object.entries(base.expected))
    assert.deepEqual(Buffer.from(result.files[name]), Buffer.from(bytes, "base64"));
  assert.deepEqual(result.external, { count: 1, pack: base.external.pack });
  assert.equal(result.inventory.length, 2);
  assert.deepEqual(payload, before);
  await assert.rejects(createCascOverlayTransform(dependencies)(payload, options));
});
test("reject mixed locations, ranges, malformed metadata and ordinary overlap", async () => {
  for (const mutate of [
    (x) => {
      x.pack.name = "00-00000002.data";
    },
    (x) => {
      x.pack.size = 1;
    },
    (x) => {
      x.pack.sha256 = "wrong";
    },
    (x) => {
      x.records[0].newEkey = "00".repeat(16);
    },
    (x) => {
      x.records[0].decodedSize = 2 ** 32;
    },
    (x) => {
      x.records[0].oldEkey = base.jobs[0].oldEkey;
    },
    (x) => {
      x.records[0].path = base.jobs[0].path;
    },
    (x) => {
      x.records.push(x.records[0]);
    },
    (x) => {
      x.records = [];
    },
  ]) {
    const external = structuredClone(base.external);
    mutate(external);
    await assert.rejects(generate(...request(external)), /CASC_OVERLAY_INVALID/);
  }
  const [bytes, options] = request();
  bytes[bytes.length - 1] = 0;
  await assert.rejects(generate(bytes, options), /CASC_OVERLAY_INVALID/);
  const huge = new Uint8Array(16 * 1024 * 1024 + 1);
  await assert.rejects(
    generate(huge, { parts: [0, 0, 0, huge.length], jobs: [], location: [0, 2] }),
  );
});
test("prebuilt-only updates and external record count remain bounded", async () => {
  const external = structuredClone(base.external);
  const parts = base.parts.slice(0, 3).map((p) => Buffer.from(p, "base64"));
  parts.push(Buffer.from(JSON.stringify(external)));
  const result = await generate(new Uint8Array(Buffer.concat(parts)), {
    parts: parts.map((p) => p.length),
    jobs: [],
    location: [0, 2],
  });
  assert.equal(result.inventory.length, 0);
  assert.equal(result.external.count, 1);
  const tooMany = structuredClone(base.external);
  tooMany.records = Array(32769).fill(tooMany.records[0]);
  await assert.rejects(generate(...request(tooMany)), /CASC_OVERLAY_INVALID/);
});
