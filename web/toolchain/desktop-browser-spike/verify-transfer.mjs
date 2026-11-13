// 仅用于 CI：将确切的公开运行时目录树绑定到构建作业的受信摘要。
// CI only: bind the exact public runtime tree to the build job's trusted digest.
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { appendFile, readFile, readdir, writeFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";
const [mode, directory, expected] = process.argv.slice(2);
assert.ok(
  ["seal", "verify"].includes(mode) && directory,
  "Expected seal|verify DIRECTORY [SHA256]",
);
const digest = (bytes) => createHash("sha256").update(bytes).digest("hex");
const manifestName = "transfer-manifest.json";
async function inventory(relative = "") {
  const entries = await readdir(path.join(directory, relative), { withFileTypes: true });
  const result = [];
  for (const entry of entries.sort((a, b) => a.name.localeCompare(b.name, "en"))) {
    const name = relative ? `${relative}/${entry.name}` : entry.name;
    if (name === manifestName) continue;
    assert.ok(/^[a-zA-Z0-9./_-]+$/.test(name), "Unexpected public asset filename");
    assert.ok(!entry.isSymbolicLink(), "No symlinks in runtime transfer");
    if (entry.isDirectory()) result.push(...(await inventory(name)));
    else {
      assert.ok(entry.isFile(), "Only regular runtime assets allowed");
      const bytes = await readFile(path.join(directory, name));
      assert.ok(bytes.length > 0 && bytes.length <= 16 * 1024 * 1024, "Asset cap");
      result.push({ path: name, bytes: bytes.length, sha256: digest(bytes) });
    }
  }
  return result;
}
const files = await inventory();
assert.ok(files.length > 0 && files.length <= 32, "File-count cap");
assert.ok(files.reduce((total, file) => total + file.bytes, 0) <= 32 * 1024 * 1024, "Bundle cap");
const actual = JSON.stringify({ version: 1, files });
if (mode === "seal") {
  await writeFile(path.join(directory, manifestName), actual);
  if (process.env.GITHUB_OUTPUT)
    await appendFile(process.env.GITHUB_OUTPUT, `digest=${digest(actual)}\n`);
  console.log(`Public runtime transfer SHA256: ${digest(actual)}`);
} else {
  assert.match(expected ?? "", /^[a-f0-9]{64}$/);
  const manifest = await readFile(path.join(directory, manifestName));
  assert.equal(digest(manifest), expected, "Transfer manifest differs from build-job digest");
  assert.equal(
    manifest.toString("utf8"),
    actual,
    "Transferred runtime tree differs from build output",
  );
  console.log(`Verified ${files.length} exact public runtime files against build-job SHA256`);
}
