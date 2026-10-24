// 基于固定 Emscripten 的 wasmBinary 接口，在 Node 测试线程中运行真实 WASM 编码。
// Run actual WASM encoding in a Node test thread through pinned Emscripten's wasmBinary interface.
// 测试专用全局垫片不代表浏览器、网络加载、解码保真或生产工作线程验证。
// Test-only global shims do not establish browser, network-loading, decode-fidelity or production-worker validation.
import assert from "node:assert/strict";
import process from "node:process";
import { createHash } from "node:crypto";
import { readFile, stat } from "node:fs/promises";
import { isMainThread, parentPort, Worker } from "node:worker_threads";
import { encodeWithModule } from "./contract.mjs";
const DEADLINE_MS = 30000;
const MAX_ARTIFACT_BYTES = 32 * 1024 * 1024;
const ARM_COMMIT = "2ff200e08c98a61775351687ea2b5d38e455ad8b";
const digest = (bytes) => createHash("sha256").update(bytes).digest("hex");
async function checkedFile(name) {
  const url = new URL(`./dist/${name}`, import.meta.url);
  const info = await stat(url);
  assert.ok(
    info.isFile() && info.size > 0 && info.size <= MAX_ARTIFACT_BYTES,
    `Invalid artifact size: ${name}`,
  );
  const bytes = await readFile(url);
  assert.equal(bytes.length, info.size, `Artifact changed while reading: ${name}`);
  return bytes;
}
async function runWorker() {
  assert.ok(Number(process.versions.node.split(".")[0]) >= 24, "Node 24 or newer required");
  const [wasm, javascript, commit, compiler] = await Promise.all([
    checkedFile("astc-encoder.wasm"),
    checkedFile("astc-encoder.mjs"),
    checkedFile("ARM-COMMIT.txt"),
    checkedFile("compiler-version.txt"),
  ]);
  assert.equal(commit.toString().trim(), ARM_COMMIT, "Unexpected Arm source receipt");
  assert.match(compiler.toString().split("\n")[0], /^emcc .* 4\.0\.20(?:\s|$)/);
  assert.ok(WebAssembly.validate(wasm), "Invalid WASM artifact");
  // 仅在此 Node 测试线程内提供 Emscripten worker 环境所需的显式垫片。
  // Supply explicit Emscripten worker-environment shims only inside this Node test thread.
  globalThis.WorkerGlobalScope = class TestOnlyWorkerGlobalScope {};
  globalThis.self = globalThis;
  globalThis.fetch = () => {
    throw new Error("Network fetch forbidden in local WASM smoke");
  };
  globalThis.XMLHttpRequest = class {
    constructor() {
      throw new Error("XHR forbidden in local WASM smoke");
    }
  };
  // 不透明 data URL 模拟 Blob 的不可解析相对基址，但不宣称真实浏览器验证。
  // An opaque data URL exercises Blob-like relative-base rejection, without claiming browser verification.
  const { default: createModule } = await import(
    `data:text/javascript;base64,${javascript.toString("base64")}`
  );
  assert.equal(typeof createModule, "function");
  let instantiations = 0;
  const module = await createModule({
    wasmBinary: new Uint8Array(wasm.buffer, wasm.byteOffset, wasm.byteLength),
    locateFile(name) {
      assert.equal(name, "astc-encoder.wasm");
      return "data:application/wasm;base64,";
    },
    instantiateWasm(imports, receive) {
      const compiled = new WebAssembly.Module(wasm),
        instance = new WebAssembly.Instance(compiled, imports);
      instantiations++;
      receive(instance, compiled);
      return instance.exports;
    },
  });
  assert.equal(instantiations, 1);
  for (const name of [
    "_malloc",
    "_free",
    "_astc_output_size",
    "_astc_encode_rgba",
    "_astc_encoder_abi_version",
  ])
    assert.equal(typeof module[name], "function", `Missing ABI export ${name}`);
  assert.equal(module._astc_encoder_abi_version(), 2);
  assert.equal(module._astc_output_size(0, 4), 0);
  assert.equal(module._astc_output_size(4097, 1), 0);
  assert.equal(module._astc_output_size(2048, 2048), 0);
  assert.equal(module._astc_output_size(0xffffffff, 0xffffffff), 0);
  let src = 0,
    dst = 0;
  try {
    src = module._malloc(64);
    dst = module._malloc(16);
    assert.ok(src && dst, "Tiny validation allocations failed");
    assert.equal(module._astc_encode_rgba(src, 64, 4, 4, 0, dst, 16, 2), -1);
    assert.equal(module._astc_encode_rgba(src, 63, 4, 4, 0, dst, 16, 0), -1);
    assert.equal(module._astc_encode_rgba(src, 64, 4, 4, 2, dst, 16, 0), -1);
    assert.equal(module._astc_encode_rgba(src, 64, 4, 4, 0, dst, 15, 0), -1);
    assert.equal(module._astc_encode_rgba(0, 64, 4, 4, 0, dst, 16, 0), -1);
    assert.equal(module._astc_encode_rgba(src, 64, 4, 4, 0, 0, 16, 0), -1);
  } finally {
    if (dst) module._free(dst);
    if (src) module._free(src);
  }
  const fixtures = [];
  let observedHeapMax = 0;
  for (const preset of ["thorough", "fast"])
    for (const profile of ["linear", "srgb"])
      for (const [width, height] of [
        [1, 1],
        [4, 4],
        [5, 7],
        [17, 3],
      ]) {
        const outputs = [];
        for (const gradient of [false, true]) {
          const rgba = new Uint8Array(width * height * 4);
          for (let y = 0; y < height; ++y)
            for (let x = 0; x < width; ++x) {
              const offset = (y * width + x) * 4;
              rgba[offset] = gradient ? Math.floor((x * 255) / Math.max(width - 1, 1)) : 255;
              rgba[offset + 1] = gradient ? Math.floor((y * 255) / Math.max(height - 1, 1)) : 0;
              rgba[offset + 2] = 0;
              rgba[offset + 3] = gradient
                ? Math.floor(((x + y) * 255) / (width + height - 1))
                : 255;
            }
          const image = { rgba: rgba.buffer, width, height, profile, preset };
          const output = new Uint8Array(encodeWithModule(module, image));
          const repeat = new Uint8Array(encodeWithModule(module, image));
          if (preset === "thorough")
            assert.deepEqual(
              output,
              new Uint8Array(encodeWithModule(module, { ...image, preset: undefined })),
              "Omitted preset changed thorough bytes",
            );
          const expected = Math.ceil(width / 4) * Math.ceil(height / 4) * 16;
          assert.equal(output.length, expected);
          assert.equal(module._astc_output_size(width, height), expected);
          assert.ok(
            output.some((byte) => byte !== 0),
            "Trivial all-zero output",
          );
          assert.deepEqual(output, repeat, "Repeated identical input was nondeterministic");
          assert.ok(
            module.HEAPU8.byteLength >= 32 * 1024 * 1024 &&
              module.HEAPU8.byteLength <= 128 * 1024 * 1024,
            "Observed heap outside build configuration",
          );
          observedHeapMax = Math.max(observedHeapMax, module.HEAPU8.byteLength);
          outputs.push(output);
          fixtures.push({
            profile,
            preset,
            width,
            height,
            gradient,
            bytes: output.length,
            sha256: digest(output),
          });
        }
        assert.notDeepEqual(outputs[0], outputs[1], "Different fixtures produced identical blocks");
      }
  return {
    kind: "actual-wasm-encoding-node-worker-test-shims",
    browserVerified: false,
    opaqueModuleUrlVerified: true,
    decodeVerified: false,
    fixtureCount: fixtures.length,
    encodeCalls:
      fixtures.length * 2 + fixtures.filter(({ preset }) => preset === "thorough").length,
    observedHeapMax,
    wasmSha256: digest(wasm),
    javascriptSha256: digest(javascript),
    fixtures,
  };
}
if (isMainThread) {
  let worker, timer;
  try {
    const result = await new Promise((resolve, reject) => {
      worker = new Worker(new URL(import.meta.url), {
        resourceLimits: { maxOldGenerationSizeMb: 64 },
      });
      timer = setTimeout(
        () => reject(new Error(`WASM smoke exceeded ${DEADLINE_MS} ms`)),
        DEADLINE_MS,
      );
      worker.once("message", (message) =>
        message.ok ? resolve(message.result) : reject(new Error(message.error)),
      );
      worker.once("error", reject);
      worker.once("exit", (code) =>
        reject(new Error(`WASM smoke worker exited without result (${code})`)),
      );
    });
    console.log(JSON.stringify(result));
  } catch (error) {
    console.error(`ASTC WASM smoke failed: ${error.message}`);
    process.exitCode = 1;
  } finally {
    clearTimeout(timer);
    await worker?.terminate();
  }
} else {
  try {
    parentPort.postMessage({ ok: true, result: await runWorker() });
  } catch (error) {
    parentPort.postMessage({ ok: false, error: String(error.stack || error) });
  } finally {
    parentPort.close();
  }
}
