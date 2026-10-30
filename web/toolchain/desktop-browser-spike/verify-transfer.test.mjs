// 验证 verify-transfer 的约定与边界；依据 web/toolchain/desktop-browser-spike 及本文件的测试断言。
// Verify verify-transfer contracts and boundaries against web/toolchain/desktop-browser-spike and this file's assertions.
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import { mkdtemp, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import test from "node:test";
const script = fileURLToPath(new URL("verify-transfer.mjs", import.meta.url));
test("public runtime transfer fails closed on changed, missing, extra files or wrong digest", async () => {
  const root = await mkdtemp(path.join(tmpdir(), "desktop-transfer-"));
  const run = (...args) =>
    spawnSync(process.execPath, [script, ...args], {
      encoding: "utf8",
      env: { ...process.env, GITHUB_OUTPUT: "" },
    });
  try {
    await mkdir(path.join(root, "licenses"));
    await writeFile(path.join(root, "texture.wasm"), "synthetic runtime");
    await writeFile(path.join(root, "licenses", "notice.txt"), "synthetic notice");
    assert.equal(run("seal", root).status, 0);
    const manifest = await readFile(path.join(root, "transfer-manifest.json"));
    const digest = createHash("sha256").update(manifest).digest("hex");
    assert.equal(run("verify", root, digest).status, 0);
    assert.notEqual(run("verify", root, "0".repeat(64)).status, 0);
    await writeFile(path.join(root, "texture.wasm"), "modified runtime");
    assert.notEqual(run("verify", root, digest).status, 0);
    await writeFile(path.join(root, "texture.wasm"), "synthetic runtime");
    await writeFile(path.join(root, "unexpected.txt"), "extra");
    assert.notEqual(run("verify", root, digest).status, 0);
    await rm(path.join(root, "unexpected.txt"));
    await rm(path.join(root, "texture.wasm"));
    assert.notEqual(run("verify", root, digest).status, 0);
    await writeFile(path.join(root, "texture.wasm"), "synthetic runtime");
    await writeFile(path.join(root, "transfer-manifest.json"), "{}");
    assert.notEqual(run("verify", root, digest).status, 0);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});
