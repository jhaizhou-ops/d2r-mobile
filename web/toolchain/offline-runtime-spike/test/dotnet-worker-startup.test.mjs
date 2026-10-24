// 验证 dotnet-worker-startup 的约定与边界；依据 web/toolchain/offline-runtime-spike/test 及本文件的测试断言。
// Verify dotnet-worker-startup contracts and boundaries against web/toolchain/offline-runtime-spike/test and this file's assertions.
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import vm from "node:vm";
import test from "node:test";
const source = await readFile(new URL("../dotnet-worker.mjs", import.meta.url), "utf8");
const sourceEvidence = JSON.parse(
  await readFile(new URL("./dotnet-source-evidence.json", import.meta.url), "utf8"),
);
const upstream = sourceEvidence.sidecar;
// 使用 MIT 许可的 .NET 10.0.13 精确源码片段，完整源码 SHA-256 记录在样例中。仅移除 TS 的 export/as-any 语法并暴露声明供 VM 观察。
// Exact MIT-licensed .NET10.0.13 source excerpts, full source SHA-256 recorded in fixture.
// Strip only TS export/as-any syntax and expose declarations for VM observation.
const classification = upstream.classification
  .replaceAll("(globalThis as any)", "globalThis")
  .replaceAll("export const ", "globalThis.");
function context() {
  const listeners = new Map(),
    replies = [];
  const sandbox = {
    importScripts() {},
    onmessage: null,
    addEventListener(type, handler) {
      listeners.set(type, handler);
    },
    postMessage(value) {
      replies.push(value);
    },
  };
  sandbox.self = sandbox;
  vm.createContext(sandbox);
  return { sandbox, listeners, replies };
}
function classifyAndComplete(sandbox) {
  let completed = false;
  sandbox.runtimeHelpers = {
    coreAssetsInMemory: {
      promise_control: {
        resolve() {
          completed = true;
        },
      },
    },
  };
  vm.runInContext(classification, sandbox);
  vm.runInContext(upstream.coreReadiness, sandbox);
  return {
    sidecar: sandbox.ENVIRONMENT_IS_SIDECAR,
    worker: sandbox.ENVIRONMENT_IS_WORKER,
    completed,
  };
}
test("old onmessage assignment reproduces blocked core readiness branch", () => {
  const { sandbox } = context();
  sandbox.onmessage = () => {};
  assert.deepEqual(classifyAndComplete(sandbox), {
    sidecar: false,
    worker: true,
    completed: false,
  });
});
test("actual sealed worker listener permits upstream sidecar detection and core completion", () => {
  const { sandbox, listeners } = context();
  vm.runInContext(source, sandbox);
  assert.equal(sandbox.onmessage, null);
  assert.equal(typeof listeners.get("message"), "function");
  assert.deepEqual(classifyAndComplete(sandbox), { sidecar: true, worker: false, completed: true });
});
test("early requests still reject synchronously without starting runtime or accepting payload", () => {
  const { sandbox, listeners, replies } = context();
  vm.runInContext(source, sandbox);
  listeners.get("message")({ data: { id: 7, type: "run" } });
  assert.equal(replies.length, 1);
  assert.equal(replies[0].id, 7);
  assert.equal(replies[0].ok, false);
  assert.equal(replies[0].error, "Runtime not initialized");
  assert.equal(sandbox.onmessage, null);
});

const nativeSource = sourceEvidence.nativeUrl;
const nativeResolution = `${nativeSource.locateFile}\n${nativeSource.wasmBinaryFile}`.replaceAll(
  "import.meta.url",
  "nativeModuleUrl",
);
function resolveNativeFile(moduleConfig) {
  const sandbox = {
    Module: moduleConfig,
    URL,
    nativeModuleUrl: "blob:https://example.test/verified-native-module",
    scriptDirectory: "",
    ENVIRONMENT_IS_SHELL: false,
    isDataURI: () => false,
  };
  vm.createContext(sandbox);
  vm.runInContext(nativeResolution, sandbox);
  return sandbox.wasmBinaryFile;
}

test("packaged Emscripten glue reproduces the blob-relative native URL failure", () => {
  assert.throws(() => resolveNativeFile({}), { code: "ERR_INVALID_URL" });
});

for (const worker of ["../dotnet-worker.mjs", "../../../sealed-runtime/dotnet-worker.mjs"]) {
  test(`${worker} supplies native alias resolution before Emscripten startup`, async () => {
    const workerSource = await readFile(new URL(worker, import.meta.url), "utf8");
    const { sandbox } = context();
    vm.runInContext(workerSource, sandbox);
    const resources = {
      wasmNative: [
        {
          name: "dotnet.native.verified.wasm",
          resolvedUrl: "https://offline.invalid/dotnet.native.verified.wasm",
        },
      ],
    };
    const moduleConfig = sandbox.nativeModuleConfig(resources);
    assert.equal(resolveNativeFile(moduleConfig), resources.wasmNative[0].resolvedUrl);
    assert.throws(() => moduleConfig.locateFile("undeclared.wasm"), /Unknown native runtime file/);
    assert.throws(() => sandbox.nativeModuleConfig({ wasmNative: [] }), /Expected one resolved/);
    assert.throws(
      () => sandbox.nativeModuleConfig({ wasmNative: [{ name: "unresolved.wasm" }] }),
      /Expected one resolved/,
    );
    assert.match(workerSource, /\.withModuleConfig\(nativeModuleConfig\(config\.resources\)\)/);
  });
}
