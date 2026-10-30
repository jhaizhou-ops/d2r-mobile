// 验证 SPIRV-Cross 桥接的固定输入行为；依据 ./wwwroot/convert.mjs、./wwwroot/replay-reflection.mjs 的模块与调用约定。
// Verify fixed-input behavior of the SPIRV-Cross bridge using the module and calling contracts in ./wwwroot/convert.mjs、./wwwroot/replay-reflection.mjs.
import assert from "node:assert/strict";
import test from "node:test";
import { readFileSync, readdirSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { validateSpirv, convertSpirv, MSL_ARGUMENTS, CROSS_COMMIT } from "./wwwroot/convert.mjs";
import { replayFields } from "./wwwroot/replay-reflection.mjs";
import { cases } from "./reflection-cases.mjs";
const valid = new Uint8Array(new Uint32Array([0x07230203, 0x10000, 0, 1, 0]).buffer);
test("malformed and non-word SPIR-V rejected before module loading", () => {
  for (const value of [null, new Uint8Array(), new Uint8Array(21), new Uint8Array(20)])
    assert.throws(() => validateSpirv(value));
  const bad = new Uint8Array(24);
  bad.set(valid);
  assert.throws(() => validateSpirv(bad));
  assert.notEqual(validateSpirv(valid), valid);
});
test("byte API invokes exact native flags, isolates calls, preserves hashes and failure", async () => {
  const calls = [];
  let instances = 0;
  const factory = async () => {
    instances++;
    const files = new Map();
    return {
      FS: {
        writeFile: (p, v) => files.set(p, v),
        readFile: (p) => files.get(p),
        unlink: (p) => files.delete(p),
      },
      callMain(args) {
        calls.push(args);
        files.set(
          args.at(-1),
          args.includes("--msl") ? "kernel void main0() {}\r\n" : '{"entryPoints":[]}\n',
        );
        return 0;
      },
    };
  };
  const result = await convertSpirv(valid, {
    factory,
    replayContext: { stage: "cs", resources: { sbufs: [] } },
  });
  assert.equal(instances, 2);
  assert.deepEqual(calls[0].slice(0, -3), MSL_ARGUMENTS);
  assert.deepEqual(calls[1], ["--reflect", "json", "/input.spv", "--output", "/reflection.json"]);
  assert.equal(result.msl, "kernel void main0() {}\n");
  assert.equal(result.replay.entry, "main0");
  assert.match(result.provenance.inputSha256, /^[a-f0-9]{64}$/);
  assert.equal(result.provenance.nativeChainClaimed, false);
  await assert.rejects(
    convertSpirv(valid, {
      factory: async () => ({ FS: { writeFile() {}, unlink() {} }, callMain: () => 1 }),
    }),
    /exited 1/,
  );
});
test("replay parser equals unchanged native Python functions on authored branch corpus", () => {
  const native = spawnSync("python3", [new URL("./native-oracle.py", import.meta.url).pathname], {
    input: JSON.stringify(cases),
    encoding: "utf8",
  });
  assert.equal(native.status, 0, native.stderr);
  assert.deepEqual(
    cases.map((x) => replayFields(x.stage, x.msl, x.resources)),
    JSON.parse(native.stdout),
  );
});

test("native/WASM proof recipes and single browser CI share the verified release compiler pin", () => {
  const lock = JSON.parse(readFileSync(new URL("./toolchain-lock.json", import.meta.url), "utf8"));
  assert.equal(lock.spirvCross.tag, "vulkan-sdk-1.4.363.0");
  assert.equal(lock.spirvCross.commit, "f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7");
  assert.equal(CROSS_COMMIT, lock.spirvCross.commit);
  for (const file of ["./build.sh", "../../runtime-build/build.mjs"]) {
    const source = readFileSync(new URL(file, import.meta.url), "utf8");
    assert.ok(source.includes(lock.spirvCross.commit), `${file}: commit drift`);
  }
  const recipes = readFileSync(
    new URL("../../verification/recipes/shader-proofs.sh", import.meta.url),
    "utf8",
  );
  for (const profile of ["spirv-cross", "shader-pipeline"]) {
    const begin = `# BEGIN PROFILE ${profile}\n`;
    const end = `# END PROFILE ${profile}\n`;
    assert.equal(recipes.split(begin).length, 2, `${profile}: unique start marker`);
    assert.equal(recipes.split(end).length, 2, `${profile}: unique end marker`);
    const start = recipes.indexOf(begin) + begin.length;
    const finish = recipes.indexOf(end);
    assert.ok(finish > start, `${profile}: ordered profile boundaries`);
    const source = recipes.slice(start, finish);
    assert.ok(source.includes(lock.spirvCross.commit), `${profile}: commit drift`);
    assert.ok(source.includes(lock.spirvCross.tag), `${profile}: tag drift`);
    assert.match(source, /build\.sh native /);
    assert.match(source, /build\.sh wasm /);
    assert.match(source, /--msl-version 20300/);
    assert.match(source, /--reflect json/);
    assert.match(source, /node capture-browser\.mjs/);
  }
  // 重型原生/浏览器比较保留在本地证明配方；唯一 CI 工作流构建固定的公开浏览器运行时。
  // Heavy native/browser comparisons remain in local proof recipes; the sole CI workflow builds pinned public browser runtimes.
  const workflows = new URL("../../../.github/workflows/", import.meta.url);
  assert.deepEqual(
    readdirSync(workflows).filter((name) => /\.ya?ml$/.test(name)),
    ["verify.yml"],
  );
  const workflow = readFileSync(new URL("verify.yml", workflows), "utf8");
  assert.deepEqual(
    [...workflow.slice(workflow.indexOf("\njobs:")).matchAll(/^ {2}([a-z-]+):/gm)].map(
      (match) => match[1],
    ),
    ["quick", "deploy-pages"],
  );
  assert.match(workflow, /\n {2}deploy-pages:\n {4}needs: quick\n/);
  assert.match(workflow, /run: node web\/verification\/verify\.mjs quick/);
  assert.match(workflow, /node web\/runtime-build\/build\.mjs/);
  assert.match(workflow, /node web\/runtime-build\/stage-preview\.mjs/);
});
