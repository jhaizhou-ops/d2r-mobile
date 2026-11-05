// 运行 AIR 编译器的 MSL 用例；依据 ./check-buffer-reflection.mjs 的模块与调用约定。
// Run MSL cases for the AIR compiler using the module and calling contracts in ./check-buffer-reflection.mjs.
import process from "node:process";
// 原生引导检查，不声称浏览器、iOS 或完整游戏已完成验收。
// Native bootstrap checks; not a browser/iOS/full-game completion claim.
import { spawnSync } from "node:child_process";
import { readFileSync, mkdirSync, writeFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { basename, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import assert from "node:assert/strict";
import { checkReflection } from "./check-buffer-reflection.mjs";
assert(
  process.argv[2] && process.argv[3],
  "Explicit external fork clang and header-directory paths are required",
);
const root = dirname(fileURLToPath(import.meta.url));
const clang = process.argv[2];
const headers = process.argv[3];
const out = process.env.AIR_TEST_OUTPUT_DIR || join(root, "msl-fixture-results");
mkdirSync(out, { recursive: true });
const sha = (b) => createHash("sha256").update(b).digest("hex");
const records = [];
function emitIR(path, input) {
  return spawnSync(
    clang,
    [
      "-cc1",
      "-triple",
      "air64-apple-ios18.0.0",
      "-x",
      "metal",
      "-std=metal3.2",
      "-Werror=unknown-attributes",
      "-I",
      headers,
      "-emit-llvm",
      "-o",
      "-",
      path,
    ],
    { input, encoding: "utf8", maxBuffer: 16 * 1024 * 1024 },
  );
}
for (const [stage, define, values] of [
  ["compute", "FACTOR", ["3", "7"]],
  ["vertex", "SCALE", ["2.0f", "4.0f"]],
  ["fragment", "THRESHOLD", ["0.25f", "0.75f"]],
]) {
  const path = join(root, "fixtures", `msl-${stage}.metal`);
  const source = readFileSync(path);
  const outputs = [];
  for (const value of values) {
    const variant = source.toString().replaceAll(define, value);
    const r = emitIR("-", variant);
    const name = `${stage}-${value}`;
    writeFileSync(join(out, name + ".stderr"), r.stderr || String(r.error || ""));
    assert.equal(r.status, 0, `${name} must compile`);
    assert.match(
      r.stdout,
      new RegExp(`!air\\.${stage === "compute" ? "kernel" : stage} =`),
      `${name} stage metadata`,
    );
    writeFileSync(join(out, name + ".ll"), r.stdout);
    records.push({
      stage,
      sourceSha256: sha(variant),
      fixtureTemplateSha256: sha(source),
      preludeSha256: sha(readFileSync(join(headers, "metal_stdlib"))),
      sourceSubstitution: `${define}=${value}`,
      irSha256: sha(r.stdout),
    });
    outputs.push(r.stdout);
  }
  assert.notEqual(
    outputs[0],
    outputs[1],
    `${stage} output must respond to source changes (same stdin filename)`,
  );
}

// 将存储/接口回归保留在现有原生 MSL 运行器中；运行器在匹配构建上实际执行前，这些检查仅算自编覆盖。
// Keep storage/interface regressions in the existing native MSL runner. These
// checks are authored coverage until this runner executes on a matching build.
const storageFixtures = JSON.parse(readFileSync(join(root, "fixtures/stage-storage.json"), "utf8"));
for (const fixture of storageFixtures) {
  let path = join(root, fixture.file);
  let source;
  if (fixture.expected === "rejected") {
    assert.match(fixture.file, /^fixtures\/stage-storage-negative\/[a-z0-9-]+\.metal$/);
    assert(
      Array.isArray(fixture.sourceLines) &&
        fixture.sourceLines.every((line) => typeof line === "string"),
    );
    source = fixture.sourceLines.join("\n");
    path = join(out, fixture.file);
  } else {
    assert.equal(fixture.sourceLines, undefined);
    source = readFileSync(path);
  }
  assert.equal(sha(source), fixture.sha256, `${fixture.file}: source identity`);
  if (fixture.expected === "rejected") {
    mkdirSync(dirname(path), { recursive: true });
    writeFileSync(path, source);
  }
  const result = emitIR(path);
  assert.ifError(result.error);
  assert.equal(result.signal, null, `${fixture.file}: unexpected compiler signal`);
  const name = `stage-storage-${basename(fixture.file, ".metal")}`;
  writeFileSync(join(out, `${name}.stderr`), result.stderr);
  if (fixture.expected === "rejected") {
    assert.notEqual(result.status, 0, `${fixture.file}: must reject`);
    assert(result.stderr.includes(fixture.diagnostic), `${fixture.file}: ${result.stderr}`);
  } else {
    assert.equal(result.status, 0, `${fixture.file}: ${result.stderr}`);
    const ir = result.stdout;
    assert.match(ir, /!air\.kernel =/);
    assert.match(ir, new RegExp(`define\\b[^\\n]*@${fixture.entry}\\(`));
    assert.equal((ir.match(/!"air\.stage_in"/g) ?? []).length, 1, "one stage input field");
    const bindings =
      fixture.entry === "stage_record_threadgroup"
        ? [
            ["scratch", 0, 3],
            ["output", 0, 1],
          ]
        : [
            ["source", 0, 2],
            ["output", 1, 1],
          ];
    for (const [argument, index, addressSpace] of bindings) {
      const lines = ir
        .split("\n")
        .filter((line) => line.includes(`!"air.arg_name", !"${argument}"`));
      assert.equal(lines.length, 1, `${fixture.file}: ${argument} metadata is unique`);
      assert(lines[0].includes('!"air.buffer"'));
      assert(lines[0].includes(`!"air.location_index", i32 ${index}`));
      assert(lines[0].includes(`!"air.address_space", i32 ${addressSpace}`));
      assert(!lines[0].includes('!"air.stage_in"'));
    }
    writeFileSync(join(out, `${name}.ll`), ir);
  }
  records.push({
    fixture: fixture.file,
    expected: fixture.expected,
    sourceSha256: sha(source),
    preludeSha256: sha(readFileSync(join(headers, "metal_stdlib"))),
    ...(fixture.expected === "compiled" ? { irSha256: sha(result.stdout) } : {}),
  });
}
const reflectionFixtures = JSON.parse(
  readFileSync(join(root, "fixtures/buffer-reflection.json"), "utf8"),
);
for (const fixture of reflectionFixtures) {
  const source = readFileSync(join(root, fixture.file));
  assert.equal(sha(source), fixture.sha256, `${fixture.file}: source identity`);
  const result = emitIR(join(root, fixture.file));
  assert.ifError(result.error);
  assert.equal(result.signal, null, `${fixture.file}: unexpected compiler signal`);
  const name = basename(fixture.file, ".metal");
  writeFileSync(join(out, `${name}.stderr`), result.stderr);
  assert.equal(result.status, 0, `${fixture.file}: ${result.stderr}`);
  checkReflection(result.stdout, fixture);
  writeFileSync(join(out, `${name}.ll`), result.stdout);
  records.push({
    fixture: fixture.file,
    expected: fixture.expected,
    sourceSha256: sha(source),
    preludeSha256: sha(readFileSync(join(headers, "metal_stdlib"))),
    irSha256: sha(result.stdout),
  });
}
writeFileSync(
  join(out, "results.json"),
  JSON.stringify({ scope: "native experimental subset only", records }, null, 2),
);
console.log(
  "Six authored MSL variants, two storage/interface inputs and four reflection inputs compiled; 15 storage/interface cases rejected as expected. Metadata checks are not memory-layout, browser, full-game or iOS acceptance proof.",
);
