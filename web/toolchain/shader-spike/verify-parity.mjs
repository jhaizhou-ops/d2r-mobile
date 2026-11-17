// 比较着色器转换实现的输出一致性；依据 ./wwwroot/synthetic-fixture.mjs 的模块与调用约定。
// Compare output parity between shader translation implementations using the module and calling contracts in ./wwwroot/synthetic-fixture.mjs.
import assert from "node:assert/strict";
import { readFile, writeFile, mkdir, stat } from "node:fs/promises";
import { resolve, join } from "node:path";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { code, job } from "./wwwroot/synthetic-fixture.mjs";

const root = fileURLToPath(new URL("../../../", import.meta.url));
const expectedHash = (
  await readFile(new URL("./upstream-source.sha256", import.meta.url), "utf8")
).split(/\s/)[0];
const source = await readFile(join(root, "dm/gfx/tools/dmsh_xlate/Program.cs"));
assert.equal(
  createHash("sha256").update(source).digest("hex"),
  expectedHash,
  "Native source changed: review/update bridge and parity provenance before testing.",
);
const [command, first, second] = process.argv.slice(2);
async function boundedRead(path, maximum = 256 * 1024) {
  const info = await stat(path);
  assert.ok(
    info.isFile() && info.size > 0 && info.size <= maximum,
    `Invalid synthetic artifact size: ${path}`,
  );
  const value = await readFile(path);
  assert.ok(value.length <= maximum, "Artifact grew beyond cap while reading");
  return value;
}
if (command === "prepare" && first) {
  const directory = resolve(first);
  await mkdir(directory, { recursive: true });
  const shaderPath = join(directory, "synthetic-exit.bin");
  await writeFile(shaderPath, code);
  await writeFile(
    join(directory, "jobs.json"),
    JSON.stringify([{ ...job, code: shaderPath }], null, 2),
  );
  console.log("Synthetic fixture prepared; native oracle has NOT been run.");
} else if (command === "compare" && first && second) {
  const nativeSpirv = await boundedRead(join(first, `${job.key}.spv`), 128 * 1024);
  const nativeResources = JSON.parse(
    (await boundedRead(join(first, `${job.key}.res.json`))).toString("utf8"),
  );
  const browser = JSON.parse((await boundedRead(second)).toString("utf8"));
  assert.ok(browser && typeof browser === "object" && !Array.isArray(browser));
  assert.deepEqual(Object.keys(browser).sort(), ["resources", "spirvBase64"]);
  assert.equal(typeof browser.spirvBase64, "string");
  const browserSpirv = Buffer.from(browser.spirvBase64, "base64");
  assert.equal(browserSpirv.toString("base64"), browser.spirvBase64, "Noncanonical base64");
  assert.ok(browserSpirv.length <= 128 * 1024, "Synthetic browser SPIR-V exceeds cap");
  assert.ok(nativeSpirv.length >= 20 && nativeSpirv.length % 4 === 0, "Invalid SPIR-V size");
  assert.equal(nativeSpirv.readUInt32LE(0), 0x07230203, "Invalid SPIR-V magic");
  assert.deepEqual(browserSpirv, nativeSpirv, "SPIR-V binary differs from native oracle");
  assert.deepEqual(
    browser.resources,
    nativeResources,
    "Resource sidecar differs from native oracle",
  );
  // 仅在完全一致后写入，便于独立查看验证器诊断。
  // Written only after full equality, for independently visible validator diagnostics.
  await writeFile(join(first, "browser-synthetic-exit.spv"), browserSpirv);
  console.log(
    JSON.stringify({
      fixture: job.key,
      spirvBytes: nativeSpirv.length,
      spirvSha256: createHash("sha256").update(nativeSpirv).digest("hex"),
      byteParity: true,
      completeSidecarParity: true,
      boundary: "Synthetic EXIT only; not game, graphics, GPU execution or IPA compatibility.",
    }),
  );
} else {
  throw new Error(
    "Usage: node verify-parity.mjs prepare <fixture-dir> | compare <native-output-dir> <browser-result.json>",
  );
}
