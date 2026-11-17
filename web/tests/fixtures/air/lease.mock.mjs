// 测试真实客户端与租约之间的异步所有权交接；清单、编译及线程均为合成替身。
// Test asynchronous ownership handoff between the real client and lease; inventory, compilation and workers are synthetic.
import test, { mock } from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { setImmediate as nextTurn } from "node:timers/promises";

const encode = (text) => new TextEncoder().encode(text);
const sha = (bytes) => createHash("sha256").update(bytes).digest("hex");
const shell = encode("synthetic shell");
const profile = {
  id: "synthetic-profile",
  archivePath: "profile/shell.zip",
  evidence: { archiveBytes: shell.length, sha256: sha(shell) },
};
const factory = new Uint8Array([1, 2, 3]);
const wasm = new Uint8Array([4, 5, 6]);
const component = {
  entry: "compiler/worker.mjs",
  factory: "compiler/factory.mjs",
  wasm: "compiler/compiler.wasm",
  receipt: "compiler/receipt.json",
  factorySha256: sha(factory),
  wasmSha256: sha(wasm),
  headerSha256: "1".repeat(64),
};
mock.module("../../../shell-profiles.mjs", {
  namedExports: { SHELL_PROFILE_IDS: [profile.id], resolveShellProfile: () => profile },
});
mock.module("../../../air-compiler-runtime-contract.mjs", {
  namedExports: {
    validateAirCompilerRuntimeComponent: () => component,
    assertAirCompilerReceiptMatchesComponent() {},
    runtimeAssetMaximumBytes: () => 1024 * 1024,
    runtimeTransportMaximumBytes: () => 1024 * 1024,
  },
});
globalThis.isSecureContext = true;
globalThis.location = { href: "https://example.test/", origin: "https://example.test" };
Object.defineProperty(globalThis, "navigator", { value: {}, configurable: true });
let serial = 0;

async function setup(t) {
  t.mock.method(WebAssembly, "compile", async () => ({ synthetic: true }));
  const workers = [];
  globalThis.Worker = class extends EventTarget {
    requests = [];
    terminations = 0;
    constructor() {
      super();
      workers.push(this);
    }
    postMessage(message, transfers = []) {
      const data = structuredClone(message, { transfer: transfers });
      this.requests.push(data);
      if (data.type === "initialize")
        queueMicrotask(() => this.reply({ type: "initialized", id: data.id, heapBytes: 4096 }));
    }
    reply(data) {
      this.dispatchEvent(new MessageEvent("message", { data }));
    }
    terminate() {
      this.terminations++;
    }
  };
  const files = new Map([
    [profile.archivePath, shell],
    [component.entry, encode("synthetic worker")],
    [component.factory, factory],
    [component.wasm, wasm],
    [component.receipt, encode("{}")],
  ]);
  const assets = [...files].map(([path, bytes]) => ({
    path,
    bytes: bytes.length,
    sha256: sha(bytes),
  }));
  const manifest = encode(
    JSON.stringify({ version: 1, assets, totalBytes: assets.reduce((sum, a) => sum + a.bytes, 0) }),
  );
  files.set("runtime-manifest.json", manifest);
  t.mock.method(globalThis, "fetch", async (url) => {
    const bytes = files.get(new URL(url).pathname.slice("/runtime/".length));
    assert.ok(bytes, "only synthetic public inventory is read");
    return new Response(bytes);
  });
  globalThis.caches = { open: () => Promise.reject(new Error("Synthetic cache unavailable")) };
  const { prepareSealedRuntime } = await import(
    `../../../sealed-runtime/clients.mjs?lease=${++serial}`
  );
  const runtime = await prepareSealedRuntime({
    baseUrl: "https://example.test/runtime/",
    manifestSha256: sha(manifest),
    profileId: profile.id,
    components: ["airCompiler"],
  });
  t.after(() => runtime.close());
  return { runtime, workers };
}

for (const mode of ["lease", "runtime", "signal", "none"])
  test(`compiled output handoff after queued ${mode} cancellation`, async (t) => {
    const { runtime, workers } = await setup(t);
    const lease = runtime.createAirCompilerClient();
    const controller = new AbortController();
    t.after(() => lease.dispose());
    const pending = lease.compile("kernel void main0() {}", {
      entry: "main0",
      stage: "kernel",
      signal: controller.signal,
      onProgress({ phase }) {
        if (phase !== "complete") return;
        queueMicrotask(() => {
          if (mode === "lease") lease.dispose();
          if (mode === "runtime") runtime.close();
          if (mode === "signal") controller.abort();
        });
      },
    });
    await nextTurn();
    const worker = workers[0];
    const output = new Uint8Array(88).fill(7);
    worker.reply({
      type: "compiled",
      id: worker.requests.at(-1).id,
      bytes: output.buffer,
      recycle: false,
      heapBytes: 4096,
      compilationCount: 1,
      inspection: { validation: "container-and-integrity-only", outputSize: output.length },
    });
    if (mode === "none") {
      assert.equal((await pending).bytes.buffer, output.buffer);
      assert.ok(output.every((value) => value === 7));
      lease.dispose();
      await runtime.close();
      assert.ok(
        output.every((value) => value === 7),
        "delivered output belongs to the caller",
      );
    } else {
      await assert.rejects(pending, mode === "signal" ? { name: "AbortError" } : /lease closed/);
      assert.ok(
        output.every((value) => value === 0),
        "undelivered output must be wiped",
      );
      if (mode !== "signal") {
        assert.equal(lease.ready, false);
        assert.equal(worker.terminations, 1);
      }
    }
  });

// 一个租约可占满固定池，关闭活跃租约必须立即停止全部实例。
// One lease may fill the fixed pool; closing an active lease must stop every instance immediately.
test("sealed AIR lease accepts four concurrent calls, bounds overload and cancels every worker", async (t) => {
  Object.assign(navigator, { hardwareConcurrency: 8, deviceMemory: 8 });
  t.after(() => {
    delete navigator.hardwareConcurrency;
    delete navigator.deviceMemory;
  });
  const { runtime, workers } = await setup(t);
  const lease = runtime.createAirCompilerClient();
  assert.equal(lease.concurrency, 4);
  assert.equal(workers.length, 4);
  const pending = Array.from({ length: 4 }, () =>
    lease.compile("kernel void main0() {}", {
      entry: "main0",
      stage: "kernel",
    }),
  );
  const settlement = Promise.allSettled(pending);
  await nextTurn();
  await assert.rejects(
    lease.compile("kernel void main0() {}", { entry: "main0", stage: "kernel" }),
    /busy/,
  );
  assert.equal(runtime.retireShaderWorkers(), false);
  lease.dispose();
  const results = await settlement;
  assert.ok(results.every((result) => result.status === "rejected"));
  assert.ok(workers.every((worker) => worker.terminations === 1));
  assert.equal(lease.ready, false);
});
