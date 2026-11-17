// 验证合成签名 NCA 的认证提取路径；依据 ../../content-selection.mjs、../../romfs-tree.mjs 的模块与调用约定。
// Verify the authenticated extraction path using synthetic signed NCAs using the module and calling contracts in ../../content-selection.mjs、../../romfs-tree.mjs.
import process from "node:process";
import { Buffer } from "node:buffer";
/** 分别使用生产根与隔离测试根，对真实编译的生产边界进行两次测试。 */
/**
 * 分别使用生产根及隔离测试根，两次测试真实编译的生产边界。
 * Tests real compiled production boundary twice, with production vs isolated test root. */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
import { createHash } from "node:crypto";
import { parseCnmt } from "../../content-selection.mjs";
import { indexRomfs } from "../../romfs-tree.mjs";
const [prodPath, testPath, dir] = process.argv.slice(2);
const fixture = (name) => readFileSync(`${dir}/${name}`);
const manifest = JSON.parse(fixture("manifest.json"));
const sha = (bytes) => createHash("sha256").update(bytes).digest("hex");
async function load(path) {
  const { default: factory } = await import(pathToFileURL(path));
  const m = await factory({ wasmBinary: readFileSync(path.replace(/\.mjs$/, ".wasm")) });
  m.FS.mkdir("/secret");
  m.FS.writeFile("/secret/prod.keys", "");
  m.FS.writeFile("/secret/title.keys", "");
  assert.equal(m._nsp_load_keys(), 0);
  m.FS.unlink("/secret/prod.keys");
  m.FS.unlink("/secret/title.keys");
  return m;
}
function open(m, method, bytes) {
  m.FS.writeFile("/fixture.nca", bytes);
  return m.ccall(method, "number", ["string"], ["/fixture.nca"]);
}
function buffer(m, size, fn) {
  const ptr = m._malloc(size);
  assert.ok(ptr);
  try {
    return fn(ptr);
  } finally {
    m._nsp_zero(ptr, size);
    m._free(ptr);
  }
}
function readEntry(m, i) {
  return buffer(m, 512, (ptr) => {
    assert.ok(m._nsp_entry_info(i, ptr, 512) >= 17);
    const size = Number(new DataView(m.HEAPU8.buffer).getBigUint64(ptr + 8, true));
    const name = Buffer.from(m.HEAPU8.subarray(ptr + 16, ptr + 512))
      .toString()
      .split("\0")[0];
    assert.ok(size < 65536);
    const bytes = buffer(m, size, (p) => {
      assert.equal(m._nsp_read_entry(i, 0, 0, p, size), size);
      return Buffer.from(m.HEAPU8.subarray(p, p + size));
    });
    const hash = buffer(m, 32, (p) => {
      assert.equal(m._nsp_hash_entry(i, p), 0);
      return Buffer.from(m.HEAPU8.subarray(p, p + 32)).toString("hex");
    });
    assert.equal(hash, sha(bytes));
    return { name, bytes };
  });
}
const prod = await load(prodPath),
  test = await load(testPath);
for (const [name, method] of [
  ["meta.nca", "nsp_open_meta"],
  ["program.nca", "nsp_open_exefs"],
]) {
  assert.equal(
    open(prod, method, fixture(name)),
    -3,
    `Production must reject synthetic signer: ${name}`,
  );
  assert.equal(prod._nsp_entry_count(), -1);
}
assert.equal(
  buffer(prod, 8, (p) => {
    prod.FS.writeFile("/fixture.nca", fixture("program.nca"));
    return prod.ccall("nsp_open_romfs", "number", ["string", "number"], ["/fixture.nca", p]);
  }),
  -3,
);
assert.equal(
  buffer(prod, 8, (p) =>
    prod.ccall("nsp_open_romfs_hash_verified", "number", ["string", "number"], ["/fixture.nca", p]),
  ),
  -3,
);
// 从外层 NSP 开始，通过同一个有界 PFS 读取器读取其中实际的 NCA 字节。
// Start at an outer NSP and read its actual NCA bytes through the same bounded PFS reader.
assert.equal(open(test, "nsp_open_pfs0", fixture("synthetic.nsp")), 0);
assert.equal(test._nsp_entry_count(), 2);
const outer = [readEntry(test, 0), readEntry(test, 1)];
assert.equal(outer[0].name, manifest.metaName);
assert.equal(outer[1].name, manifest.programName);
assert.equal(sha(outer[0].bytes), manifest.metaHash);
assert.equal(sha(outer[1].bytes), manifest.programHash);
assert.equal(open(test, "nsp_open_meta", outer[0].bytes), 0);
assert.equal(test._nsp_entry_count(), 1);
const cnmt = readEntry(test, 0).bytes;
assert.deepEqual(cnmt, fixture("expected.cnmt"));
const parsed = parseCnmt(cnmt);
assert.equal(parsed.contents[0].sha256, sha(outer[1].bytes));
assert.equal(parsed.contents[0].size, BigInt(outer[1].bytes.length));
assert.equal(parsed.cnmtDigest, sha(cnmt.subarray(0, -32)));
assert.equal(open(test, "nsp_open_exefs", outer[1].bytes), 0);
assert.equal(test._nsp_entry_count(), 2);
assert.deepEqual(readEntry(test, 0), { name: "main", bytes: fixture("expected.main") });
assert.deepEqual(readEntry(test, 1), { name: "main.npdm", bytes: fixture("expected.npdm") });
function openRomfs(bytes, method = "nsp_open_romfs") {
  test.FS.writeFile("/fixture.nca", bytes);
  return buffer(test, 8, (p) => {
    const rc = test.ccall(method, "number", ["string", "number"], ["/fixture.nca", p]);
    return { rc, size: new DataView(test.HEAPU8.buffer).getBigUint64(p, true) };
  });
}
const rom = openRomfs(outer[1].bytes);
assert.equal(rom.rc, 0);
assert.deepEqual(openRomfs(outer[1].bytes, "nsp_open_romfs_hash_verified"), rom);
const source = {
  size: rom.size,
  async readAt(offset, count) {
    return buffer(test, count, (p) => {
      assert.equal(
        test._nsp_read_romfs(Number(offset & 0xffffffffn), Number(offset >> 32n), p, count),
        count,
      );
      return Uint8Array.from(test.HEAPU8.subarray(p, p + count));
    });
  },
};
assert.deepEqual(Buffer.from(await source.readAt(0n, Number(rom.size))), fixture("expected.romfs"));
const index = await indexRomfs(source);
assert.equal(index.files.length, 1);
const hello = index.getFile("hello.txt");
assert.ok(hello);
assert.equal(
  Buffer.from(await hello.readAt(0n, Number(hello.size))).toString(),
  "Synthetic authenticated RomFS greeting.\n",
);
// 分别破坏每个已认证层，而不只是外层 NSP 或签名。
// Individually corrupt each authenticated layer, not merely the outer NSP or signature.
for (const [label, offset] of Object.entries(manifest.mutations)) {
  const bytes = Buffer.from(outer[1].bytes);
  bytes[offset] ^= 1;
  const rc = label === "romfsPayload" ? openRomfs(bytes).rc : open(test, "nsp_open_exefs", bytes);
  assert.equal(rc, -3, `Must reject tampered ${label}`);
  assert.equal(test._nsp_entry_count(), -1);
}
const metaTampered = Buffer.from(outer[0].bytes);
metaTampered[0xc00] ^= 1;
assert.equal(open(test, "nsp_open_meta", metaTampered), -3);
prod._nsp_reset();
test._nsp_reset();
console.log(
  "PASS: production rejects synthetic signer; isolated test root authenticates NSP → Meta/CNMT → Program ExeFS and IVFC/RomFS; tampering rejected.",
);
console.log(
  "Synthetic test-root proof only. No Nintendo signatures, complete NSO/NPDM validity, game compatibility, encrypted NCA or BKTR update proof.",
);
