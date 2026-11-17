// 验证 generate 的约定与边界；依据 ../../content-selection.mjs、../../romfs-tree.mjs 及本文件的测试断言。
// Verify generate contracts and boundaries against ../../content-selection.mjs、../../romfs-tree.mjs and this file's assertions.
import process from "node:process";
import test from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, readdirSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { execFileSync } from "node:child_process";
import { createHash, createPublicKey, verify, constants } from "node:crypto";
import { fileURLToPath } from "node:url";
import { parseCnmt } from "../../content-selection.mjs";
import { indexRomfs } from "../../romfs-tree.mjs";
const sha = (x) => createHash("sha256").update(x).digest();
test("generator produces internally coherent authored fixtures, no persisted private key", async () => {
  const dir = mkdtempSync(join(tmpdir(), "toy-nca-unit-"));
  try {
    execFileSync(process.execPath, [
      fileURLToPath(new URL("./generate.mjs", import.meta.url)),
      dir,
    ]);
    const read = (name) => readFileSync(join(dir, name));
    assert.deepEqual(
      readdirSync(dir).sort(),
      [
        "expected.cnmt",
        "expected.main",
        "expected.npdm",
        "expected.romfs",
        "manifest.json",
        "meta.nca",
        "program.nca",
        "synthetic.nsp",
        "test-root.bin",
      ].sort(),
    );
    const root = createPublicKey({
      format: "jwk",
      key: { kty: "RSA", n: read("test-root.bin").toString("base64url"), e: "AQAB" },
    });
    for (const name of ["meta.nca", "program.nca"]) {
      const nca = read(name);
      assert.equal(nca.readBigUInt64LE(0x208), BigInt(nca.length));
      assert.ok(
        verify(
          "sha256",
          nca.subarray(0x200, 0x400),
          { key: root, padding: constants.RSA_PKCS1_PSS_PADDING, saltLength: 32 },
          nca.subarray(0, 256),
        ),
      );
      for (let i = 0; i < 4; i++)
        if (nca.readUInt32LE(0x240 + i * 16))
          assert.deepEqual(
            sha(nca.subarray(0x400 + i * 512, 0x600 + i * 512)),
            nca.subarray(0x280 + i * 32, 0x2a0 + i * 32),
          );
    }
    const cnmt = read("expected.cnmt"),
      parsed = parseCnmt(cnmt);
    assert.equal(parsed.contents[0].sha256, sha(read("program.nca")).toString("hex"));
    assert.equal(parsed.cnmtDigest, sha(cnmt.subarray(0, -32)).toString("hex"));
    const bytes = read("expected.romfs");
    const index = await indexRomfs({
      size: BigInt(bytes.length),
      async readAt(offset, count) {
        return Uint8Array.from(bytes.subarray(Number(offset), Number(offset) + count));
      },
    });
    assert.equal(index.files.length, 1);
    assert.equal(index.files[0].path, "hello.txt");
    // 对所有 IVFC 块分别按父层校验，零填充部分也包括在内。
    // All IVFC blocks independently checked against their parent, including zero padding.
    const nca = read("program.nca"),
      fs = nca.subarray(0x600, 0x800),
      start = nca.readUInt32LE(0x250) * 512;
    for (let i = 0; i < 6; i++) {
      const offset = Number(fs.readBigUInt64LE(24 + i * 24));
      const expected =
        i === 0
          ? fs.subarray(0xc8, 0xe8)
          : nca.subarray(start + (i - 1) * 512, start + (i - 1) * 512 + 32);
      assert.deepEqual(sha(nca.subarray(start + offset, start + offset + 512)), expected);
    }
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
