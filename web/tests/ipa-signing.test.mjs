// Feature suite: ipa-signing. Original test sections retain independent lexical scopes.
import * as nodeTestModule from "node:test";
import * as strictModule from "node:assert/strict";
import * as nodeEventsModule from "node:events";
import * as downloadPreparationModule from "../download-preparation.mjs";
import * as ipaSigningClientModule from "../ipa-signing-client.mjs";
import * as ipaSigningContractModule from "../ipa-signing-contract.mjs";
import * as ipaSigningFactoryModule from "../sealed-runtime/ipa-signing-factory.mjs";
import * as ipaSigningZipModule from "../ipa-signing-zip.mjs";
import * as ipaSigningFsModule from "../ipa-signing-fs.mjs";
import * as ipaSigningWorkerCoreModule from "../ipa-signing-worker-core.mjs";
import * as zipWriterModule from "../zip-writer.mjs";
import * as promisesModule from "node:fs/promises";
import * as appCustomizationModule from "../app-customization.mjs";
import * as appCustomizationPlistModule from "../app-customization-plist.mjs";
import * as packageModule from "../runtime-build/package.mjs";

// ipa-signing-client
{
  // 使用可转移合成凭据测试边界；不读取真实身份或伪称验证苹果签名。
  // Test boundaries with transferable synthetic credentials, not real identities or claimed Apple verification.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { getEventListeners } = nodeEventsModule;
  const { createDownloadPreparation } = downloadPreparationModule;
  const { prepareIpaSigningClient } = ipaSigningClientModule;
  const { validateSigningCredentials, validateSigningProgress } = ipaSigningContractModule;
  class WorkerDouble extends EventTarget {
    terminated = 0;
    messages = [];
    postMessage(message, transfers) {
      const copy = structuredClone(message, { transfer: transfers });
      this.messages.push(copy);
      this.handle?.(copy);
    }
    reply(data) {
      this.dispatchEvent(new MessageEvent("message", { data }));
    }
    terminate() {
      this.terminated++;
    }
  }
  const identity = () => ({
    kind: "p12",
    identity: new Uint8Array([1, 2, 3]).buffer,
    profile: new Uint8Array([4, 5, 6]).buffer,
    password: "synthetic-only",
  });
  const ipa = () => new Blob([new Uint8Array(30)]);
  async function ready(handler, options = {}) {
    const worker = new WorkerDouble();
    worker.handle = ({ id }) => {
      if (id === 0)
        queueMicrotask(() => worker.reply({ id: 0, ok: true, result: { initialized: true } }));
      else handler?.(worker);
    };
    const wasm = new Uint8Array(8).buffer;
    const client = await prepareIpaSigningClient({
      worker,
      assets: [["zsign.wasm", wasm]],
      ...options,
    });
    assert.equal(wasm.byteLength, 0);
    return { client, worker };
  }
  test("success consumes buffers, preserves unsigned Blob and terminates one-shot worker", async () => {
    const output = ipa(),
      input = ipa(),
      credentials = identity();
    const { client, worker } = await ready((w) =>
      queueMicrotask(() => w.reply({ id: 1, ok: true, result: { blob: output } })),
    );
    const result = await client.sign({ ipa: input, credentials });
    assert.equal(result.fileName, "D2RMobile-signed.ipa");
    assert.equal(result.blob, output);
    assert.equal(input.size, 30);
    assert.equal(credentials.identity.byteLength, 0);
    assert.equal(credentials.profile.byteLength, 0);
    assert.equal(worker.terminated, 1);
    await assert.rejects(client.sign({ ipa: input, credentials: identity() }), /SIGNING_CLOSED/);
    client.close();
    assert.equal(worker.terminated, 1);
  });
  test("cancel interrupts nonresponsive native worker and ignores late success", async () => {
    const { client, worker } = await ready();
    const controller = new AbortController();
    const pending = client.sign({ ipa: ipa(), credentials: identity(), signal: controller.signal });
    controller.abort(new Error("private reason"));
    await assert.rejects(pending, (e) => e.name === "AbortError" && !e.message.includes("private"));
    worker.reply({ id: 1, ok: true, result: { blob: ipa() } });
    assert.equal(worker.terminated, 1);
  });
  test("invalid inputs zero untransferred buffers and never send a private operation", async () => {
    const { client, worker } = await ready();
    const credentials = identity();
    await assert.rejects(client.sign({ ipa: new Blob([]), credentials }), /SIGNING_INPUT_INVALID/);
    assert.deepEqual([...new Uint8Array(credentials.identity)], [0, 0, 0]);
    assert.deepEqual([...new Uint8Array(credentials.profile)], [0, 0, 0]);
    assert.equal(worker.messages.length, 1);
    assert.equal(worker.terminated, 1);
  });
  test("already cancelled request transfers no credentials and clears owned buffers", async () => {
    const { client, worker } = await ready();
    const credentials = identity();
    const controller = new AbortController();
    controller.abort();
    await assert.rejects(client.sign({ ipa: ipa(), credentials, signal: controller.signal }), {
      name: "AbortError",
    });
    assert.deepEqual([...new Uint8Array(credentials.identity)], [0, 0, 0]);
    assert.equal(worker.messages.length, 1);
    assert.equal(worker.terminated, 1);
  });
  test("raw worker errors are redacted and output extra fields are rejected", async () => {
    for (const reply of [
      { id: 1, ok: false, error: { code: "password: private" } },
      { id: 1, ok: true, result: { blob: ipa(), password: "private" } },
      { id: 1, ok: true, result: { blob: new Blob([]) } },
    ]) {
      const { client, worker } = await ready((w) => queueMicrotask(() => w.reply(reply)));
      await assert.rejects(
        client.sign({ ipa: ipa(), credentials: identity() }),
        (e) => /^SIGNING_/.test(e.message) && !e.message.includes("private"),
      );
      assert.equal(worker.terminated, 1);
    }
  });
  test("progress only exposes closed stages and bounded counts; observer errors are harmless", async () => {
    const events = [];
    const { client } = await ready((w) =>
      queueMicrotask(() => {
        w.reply({ id: 1, type: "progress", stage: "sign", completed: 1, total: 2 });
        w.reply({ id: 1, ok: true, result: { blob: ipa() } });
      }),
    );
    await client.sign({
      ipa: ipa(),
      credentials: identity(),
      onProgress: (e) => {
        events.push(e);
        throw Error("observer");
      },
    });
    assert.deepEqual(events, [{ stage: "sign", completed: 1, total: 2 }]);
    for (const extra of [{ stage: "private" }, { completed: 3 }, { path: "private" }]) {
      assert.equal(
        validateSigningProgress({
          id: 1,
          type: "progress",
          stage: "sign",
          completed: 1,
          total: 2,
          ...extra,
        }),
        false,
      );
    }
  });
  test("concurrent invocation does not terminate active operation or clear its inputs", async () => {
    const { client, worker } = await ready();
    const first = client.sign({ ipa: ipa(), credentials: identity() });
    const secondCredentials = identity();
    await assert.rejects(
      client.sign({ ipa: ipa(), credentials: secondCredentials }),
      /SIGNING_CLOSED/,
    );
    assert.equal(worker.terminated, 0);
    assert.equal(secondCredentials.identity.byteLength, 3);
    worker.reply({ id: 1, ok: true, result: { blob: ipa() } });
    await first;
  });
  test("postMessage failure clears credentials and masks raw error", async () => {
    const { client, worker } = await ready();
    const credentials = identity();
    worker.postMessage = () => {
      throw Error("private error");
    };
    await assert.rejects(client.sign({ ipa: ipa(), credentials }), /SIGNING_WORKER_FAILED/);
    assert.deepEqual([...new Uint8Array(credentials.identity)], [0, 0, 0]);
    assert.equal(worker.terminated, 1);
  });
  test("initialization cancellation owns and terminates worker", async () => {
    const worker = new WorkerDouble(),
      controller = new AbortController();
    const result = prepareIpaSigningClient({
      worker,
      assets: [["zsign.wasm", new ArrayBuffer(8)]],
      signal: controller.signal,
    });
    controller.abort();
    await assert.rejects(result, { name: "AbortError" });
    assert.equal(worker.terminated, 1);
  });
  test("credentials reject shared buffers, unexpected fields and oversized passwords", () => {
    const same = new ArrayBuffer(2);
    assert.equal(
      validateSigningCredentials({ ...identity(), identity: same, profile: same }),
      false,
    );
    assert.equal(validateSigningCredentials({ ...identity(), path: "private" }), false);
    assert.equal(validateSigningCredentials({ ...identity(), password: "x".repeat(4097) }), false);
  });

  test("resizable credential and bootstrap buffers are rejected", async () => {
    const growing = new ArrayBuffer(8, { maxByteLength: 16 });
    assert.equal(growing.resizable, true);
    assert.equal(validateSigningCredentials({ ...identity(), identity: growing }), false);
    assert.equal(validateSigningCredentials({ ...identity(), profile: growing }), false);
    const worker = new WorkerDouble();
    await assert.rejects(
      prepareIpaSigningClient({ worker, assets: [["zsign.wasm", growing]] }),
      /SIGNING_INITIALIZATION_FAILED/,
    );
    assert.equal(worker.messages.length, 0);
    assert.equal(worker.terminated, 1);
  });

  test("embedded NUL password is rejected before native C strings", () => {
    assert.equal(validateSigningCredentials({ ...identity(), password: "before\0after" }), false);
  });

  test("silent initialization expires, terminates and ignores late success", async () => {
    const worker = new WorkerDouble();
    await assert.rejects(
      prepareIpaSigningClient({
        worker,
        assets: [["zsign.wasm", new ArrayBuffer(8)]],
        initializeTimeoutMs: 5,
      }),
      /SIGNING_TIMEOUT/,
    );
    worker.reply({ id: 0, ok: true, result: { initialized: true } });
    assert.equal(worker.terminated, 1);
  });
  test("silent signing expires and late result cannot replace the unsigned input", async () => {
    const { client, worker } = await ready(undefined, { signingTimeoutMs: 5 });
    const original = ipa(),
      credentials = identity();
    await assert.rejects(client.sign({ ipa: original, credentials }), /SIGNING_TIMEOUT/);
    worker.reply({ id: 1, ok: true, result: { blob: ipa() } });
    assert.equal(worker.terminated, 1);
    assert.equal(original.size, 30);
    assert.equal(credentials.identity.byteLength, 0);
  });
  test("rejected async progress observer is observed without changing signing", async () => {
    const { client } = await ready((w) =>
      queueMicrotask(() => {
        w.reply({ id: 1, type: "progress", stage: "sign", completed: 1, total: 1 });
        w.reply({ id: 1, ok: true, result: { blob: ipa() } });
      }),
    );
    await client.sign({
      ipa: ipa(),
      credentials: identity(),
      onProgress: async () => {
        throw Error("private observer");
      },
    });
    await new Promise((resolve) => setTimeout(resolve, 0));
  });
  test("deadline overrides cannot disable or exceed bounded production defaults", async () => {
    for (const options of [
      { initializeTimeoutMs: 0 },
      { initializeTimeoutMs: 120001 },
      { signingTimeoutMs: Infinity },
      { signingTimeoutMs: 600001 },
    ]) {
      const worker = new WorkerDouble();
      await assert.rejects(
        prepareIpaSigningClient({
          worker,
          assets: [["zsign.wasm", new ArrayBuffer(8)]],
          ...options,
        }),
        /SIGNING_INITIALIZATION_FAILED/,
      );
      assert.equal(worker.messages.length, 0);
      assert.equal(worker.terminated, 1);
    }
  });

  for (const errorCode of ["SIGNING_PROFILE_MISMATCH", "SIGNING_APPLICATION_ID_TOO_LONG"])
    test(`${errorCode} keeps the committed unsigned download available`, async () => {
      const original = ipa();
      let retainedBlob;
      const fallback = createDownloadPreparation({
        createObjectURL: (blob) => {
          retainedBlob = blob;
          return "blob:unsigned";
        },
        revokeObjectURL() {},
      });
      fallback.retain({ blob: original, fileName: "D2RMobile-unsigned.ipa" });
      const before = fallback.prepare();
      const { client, worker } = await ready((w) =>
        queueMicrotask(() => w.reply({ id: 1, ok: false, error: { code: errorCode } })),
      );
      await assert.rejects(client.sign({ ipa: original, credentials: identity() }), {
        message: errorCode,
      });
      assert.deepEqual(fallback.prepare(), before);
      assert.equal(retainedBlob, original);
      assert.equal(worker.terminated, 1);
      fallback.dispose();
    });

  for (const event of ["error", "messageerror"])
    test(`lifetime ${event} handling preserves active failure and releases listeners`, async () => {
      const { client, worker } = await ready();
      assert.equal(client.ready, true);
      const pending = client.sign({ ipa: ipa(), credentials: identity() });
      assert.equal(client.ready, false);
      worker.dispatchEvent(new Event(event));
      await assert.rejects(pending, /^Error: SIGNING_WORKER_FAILED$/);
      assert.equal(worker.terminated, 1);
      for (const type of ["message", "error", "messageerror"])
        assert.equal(getEventListeners(worker, type).length, 0);
      worker.dispatchEvent(new Event(event));
      assert.equal(worker.terminated, 1);
    });

  test("invalid workers and failed lifetime-listener cleanup preserve safe initialization errors", async () => {
    for (const worker of [
      undefined,
      {},
      {
        removeEventListener() {
          throw new Error("PRIVATE-CANARY");
        },
      },
    ]) {
      let terminated = false;
      if (worker) worker.terminate = () => (terminated = true);
      await assert.rejects(prepareIpaSigningClient({ worker, assets: [] }), {
        message: "SIGNING_INITIALIZATION_FAILED",
      });
      if (worker) assert.equal(terminated, true);
    }
  });

  test("failed lifetime-listener removal still terminates an explicitly closed warm worker", async (t) => {
    const { client, worker } = await ready();
    const removed = [];
    t.mock.method(worker, "removeEventListener", (type) => {
      removed.push(type);
      throw new Error("PRIVATE-CANARY");
    });
    assert.doesNotThrow(() => client.close());
    assert.equal(client.ready, false);
    assert.equal(worker.terminated, 1);
    assert.deepEqual(removed, ["error", "messageerror"]);
    client.close();
    assert.equal(worker.terminated, 1);
  });
}

// ipa-signing-factory
{
  // 仅使用合成工作线程验证生命周期，不读取真实凭据、不签名或执行浏览器代码。
  // Synthetic worker lifecycle only: no real credentials, signing, network or browser execution.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { getEventListeners } = nodeEventsModule;
  const { prepareVerifiedIpaSigningFactory } = ipaSigningFactoryModule;
  function setup(t) {
    const state = { workers: [], created: [], revoked: [], initialize: true };
    t.mock.method(URL, "createObjectURL", () => {
      const url = `blob:synthetic-${state.created.length}`;
      state.created.push(url);
      return url;
    });
    t.mock.method(URL, "revokeObjectURL", (url) => state.revoked.push(url));
    t.mock.method(globalThis, "fetch", () => assert.fail("signing retries must not fetch"));
    const previousWorker = Object.getOwnPropertyDescriptor(globalThis, "Worker");
    t.after(() => {
      if (previousWorker) Object.defineProperty(globalThis, "Worker", previousWorker);
      else delete globalThis.Worker;
    });
    globalThis.Worker = class extends EventTarget {
      terminations = 0;
      requests = [];
      constructor(url) {
        super();
        this.url = url;
        state.workers.push(this);
      }
      postMessage(message, transfers) {
        const data = structuredClone(message, { transfer: transfers });
        this.requests.push(data);
        if (data.type === "initialize" && state.initialize)
          queueMicrotask(() => this.reply({ id: 0, ok: true, result: { initialized: true } }));
      }
      reply(data) {
        this.dispatchEvent(new MessageEvent("message", { data }));
      }
      terminate() {
        this.terminations++;
      }
    };
    return state;
  }
  const create = () =>
    prepareVerifiedIpaSigningFactory({
      workerBlob: new Blob(["verified worker"]),
      wasmBytes: new Uint8Array(8),
    });
  const options = () => ({
    ipa: new Blob([new Uint8Array(22)]),
    credentials: {
      kind: "p12",
      identity: new ArrayBuffer(2),
      profile: new ArrayBuffer(2),
      password: "",
    },
  });
  const success = (worker) =>
    worker.reply({ id: 1, ok: true, result: { blob: new Blob([new Uint8Array(23)]) } });
  test("initial worker is reused, failed signing retries locally in a fresh worker", async (t) => {
    const state = setup(t),
      factory = await create();
    t.after(() => factory.close());
    assert.equal(state.workers.length, 1);
    const first = await factory.prepare();
    assert.equal(state.workers.length, 1);
    const failed = first.sign(options());
    state.workers[0].reply({ id: 1, ok: false, error: { code: "SIGNING_PROFILE_MISMATCH" } });
    await assert.rejects(failed, /SIGNING_PROFILE_MISMATCH/);
    assert.ok(state.workers[0].terminations > 0);
    const second = await factory.prepare();
    assert.equal(state.workers.length, 2);
    const result = second.sign(options());
    success(state.workers[1]);
    assert.equal((await result).blob.size, 23);
    assert.ok(state.workers[1].terminations > 0);
    assert.deepEqual(state.revoked, state.created);
    factory.close();
    await assert.rejects(factory.prepare(), /SIGNING_CLOSED/);
  });
  test("abandoning prepared credentials frees the worker and allows another preparation", async (t) => {
    const state = setup(t),
      factory = await create();
    t.after(() => factory.close());
    const first = await factory.prepare();
    await assert.rejects(factory.prepare(), /SIGNING_CLOSED/);
    first.close();
    first.close();
    assert.equal(state.workers[0].terminations, 1);
    const second = await factory.prepare();
    assert.equal(state.workers.length, 2);
    await assert.rejects(first.sign(options()), /SIGNING_CLOSED/);
    second.close();
  });
  test("cancelled signing destroys only its attempt and preserves retry factory", async (t) => {
    const state = setup(t),
      factory = await create();
    t.after(() => factory.close());
    const attempt = await factory.prepare(),
      controller = new AbortController();
    const signing = attempt.sign({ ...options(), signal: controller.signal });
    controller.abort();
    await assert.rejects(signing, { name: "AbortError" });
    const retry = await factory.prepare();
    assert.equal(state.workers.length, 2);
    retry.close();
  });
  test("close interrupts retry initialization, revokes its URL and prevents revival", async (t) => {
    const state = setup(t),
      factory = await create();
    (await factory.prepare()).close();
    state.initialize = false;
    const preparing = factory.prepare();
    assert.equal(state.workers.length, 2);
    await assert.rejects(factory.prepare(), /SIGNING_CLOSED/);
    factory.close();
    await assert.rejects(preparing, { name: "AbortError" });
    state.workers[1].reply({ id: 0, ok: true, result: { initialized: true } });
    await assert.rejects(factory.prepare(), /SIGNING_CLOSED/);
    assert.ok(state.workers.every((worker) => worker.terminations > 0));
    assert.deepEqual(state.revoked, state.created);
  });
  test("cancelled retry initialization remains retryable and accepts no credentials", async (t) => {
    const state = setup(t),
      factory = await create();
    t.after(() => factory.close());
    (await factory.prepare()).close();
    state.initialize = false;
    const controller = new AbortController(),
      preparing = factory.prepare({ signal: controller.signal });
    controller.abort();
    await assert.rejects(preparing, { name: "AbortError" });
    assert.ok(
      state.workers.every((worker) =>
        worker.requests.every((request) => request.type === "initialize"),
      ),
    );
    state.initialize = true;
    (await factory.prepare()).close();
    assert.equal(state.workers.length, 3);
  });
  test("explicit close terminates active signing and leaves no usable attempt", async (t) => {
    const state = setup(t),
      factory = await create();
    const attempt = await factory.prepare();
    const signing = attempt.sign(options());
    factory.close();
    await assert.rejects(signing, /SIGNING_CLOSED/);
    await assert.rejects(attempt.sign(options()), /SIGNING_CLOSED/);
    assert.ok(state.workers[0].terminations > 0);
  });

  test("failed fresh initialization is retryable without touching credentials", async (t) => {
    const state = setup(t),
      factory = await create();
    t.after(() => factory.close());
    (await factory.prepare()).close();
    state.initialize = false;
    const preparing = factory.prepare();
    state.workers[1].reply({ id: 0, ok: false, error: { code: "SIGNING_INITIALIZATION_FAILED" } });
    await assert.rejects(preparing, /SIGNING_INITIALIZATION_FAILED/);
    assert.ok(state.workers[1].terminations > 0);
    state.initialize = true;
    (await factory.prepare()).close();
    assert.equal(state.workers.length, 3);
    assert.ok(
      state.workers.every((worker) =>
        worker.requests.every((request) => request.type === "initialize"),
      ),
    );
  });

  test("direct signing cannot lazily initialize and concurrent signing leaves the first attempt intact", async (t) => {
    const state = setup(t),
      factory = await create();
    t.after(() => factory.close());
    await assert.rejects(factory.sign(options()), /SIGNING_CLOSED/);
    assert.equal(state.workers.length, 1);
    const attempt = await factory.prepare();
    const signing = attempt.sign(options());
    await assert.rejects(attempt.sign(options()), /SIGNING_CLOSED/);
    await assert.rejects(factory.prepare(), /SIGNING_CLOSED/);
    assert.equal(state.workers[0].terminations, 0);
    success(state.workers[0]);
    await signing;
    await assert.rejects(factory.sign(options()), /SIGNING_CLOSED/);
    assert.equal(state.workers.length, 1);
  });

  test("factory initialization requires no AbortSignal.any and releases forwarding listeners", async (t) => {
    setup(t);
    const previousAny = AbortSignal.any;
    AbortSignal.any = undefined;
    t.after(() => {
      AbortSignal.any = previousAny;
    });
    const controller = new AbortController();
    const factory = await prepareVerifiedIpaSigningFactory({
      workerBlob: new Blob(["verified worker"]),
      wasmBytes: new Uint8Array(8),
      signal: controller.signal,
    });
    t.after(() => factory.close());
    assert.equal(getEventListeners(controller.signal, "abort").length, 0);
    (await factory.prepare()).close();
    const retryController = new AbortController();
    (await factory.prepare({ signal: retryController.signal })).close();
    assert.equal(getEventListeners(retryController.signal, "abort").length, 0);
  });

  for (const event of ["error", "messageerror"])
    test(`failed idle signer (${event}) is replaced before a private attempt is returned`, async (t) => {
      const state = setup(t),
        factory = await create();
      t.after(() => factory.close());
      const stale = state.workers[0];
      stale.dispatchEvent(new Event(event));
      assert.equal(stale.terminations, 1);
      for (const type of ["message", "error", "messageerror"])
        assert.equal(getEventListeners(stale, type).length, 0);
      state.initialize = false;
      let prepared = false;
      const pending = factory.prepare().then((attempt) => {
        prepared = true;
        return attempt;
      });
      assert.equal(state.workers.length, 2);
      await new Promise(setImmediate);
      assert.equal(prepared, false, "credentials must wait for fresh initialization proof");
      const replacement = state.workers[1];
      assert.deepEqual(
        replacement.requests.map((request) => request.type),
        ["initialize"],
      );
      replacement.reply({ id: 0, ok: true, result: { initialized: true } });
      const attempt = await pending;
      const signing = attempt.sign(options());
      success(replacement);
      assert.equal((await signing).blob.size, 23);
      assert.equal(stale.requests.length, 1);
      assert.equal(replacement.terminations, 1);
      assert.deepEqual(state.revoked, state.created);
    });

  test("a signer failing after prepare rejects promptly and clears the attempt's credentials", async (t) => {
    const state = setup(t),
      factory = await create();
    t.after(() => factory.close());
    const attempt = await factory.prepare();
    state.workers[0].dispatchEvent(new Event("error"));
    const input = options();
    new Uint8Array(input.credentials.identity).fill(7);
    new Uint8Array(input.credentials.profile).fill(8);
    await assert.rejects(attempt.sign(input), /^Error: SIGNING_CLOSED$/);
    assert.deepEqual([...new Uint8Array(input.credentials.identity)], [0, 0]);
    assert.deepEqual([...new Uint8Array(input.credentials.profile)], [0, 0]);
    assert.equal(state.workers[0].requests.length, 1);
    assert.equal(state.workers[0].terminations, 1);
    (await factory.prepare()).close();
    assert.equal(state.workers.length, 2);
  });

  test("dormant signer owns verified bytes without creating a worker until prepare", async (t) => {
    const state = setup(t);
    const factory = await prepareVerifiedIpaSigningFactory({
      workerBlob: new Blob(["verified worker"]),
      wasmBytes: new Uint8Array(8),
      warmup: false,
    });
    t.after(() => factory.close());
    assert.equal(state.workers.length, 0);
    await assert.rejects(factory.sign(options()), /SIGNING_CLOSED/);
    assert.equal(state.workers.length, 0);
    const attempt = await factory.prepare();
    assert.equal(state.workers.length, 1);
    attempt.close();
    assert.ok(state.workers[0].terminations > 0);
    (await factory.prepare()).close();
    assert.equal(state.workers.length, 2);
  });
}

// ipa-signing-storage
{
  // 验证 ipa-signing-storage 的约定与边界；依据 ../ipa-signing-zip.mjs、../ipa-signing-fs.mjs 及本文件的测试断言。
  // Verify ipa-signing-storage contracts and boundaries against ../ipa-signing-zip.mjs、../ipa-signing-fs.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { inspectUnsignedSigningArchive, createSignedSigningArchive } = ipaSigningZipModule;
  const { createSigningFileStore, SIGNING_FS_LIMITS } = ipaSigningFsModule;
  const root = "Payload/Test.app/";
  const entry = (path, bytes, mode = 0o644) => ({
    path: root + path,
    size: bytes.length,
    mode,
    blob: new Blob([bytes]),
  });
  const entries = () => [
    entry("Info.plist", new Uint8Array([1, 2, 3])),
    entry("Test", new Uint8Array([0xcf, 0xfa, 0xed, 0xfe, 1, 2, 3, 4]), 0o755),
    entry("resource", new Uint8Array([9, 8, 7])),
  ];

  test("canonical unsigned archive is independently checked and repackages unchanged payloads", async () => {
    const blob = await createSignedSigningArchive(entries());
    const archive = await inspectUnsignedSigningArchive(blob);
    assert.equal(archive.appRoot, root);
    assert.equal(archive.entries.length, 3);
    const repacked = await createSignedSigningArchive(archive.entries);
    assert.deepEqual(await repacked.arrayBuffer(), await blob.arrayBuffer());
    const bytes = new Uint8Array(await blob.arrayBuffer());
    bytes[30 + new TextEncoder().encode(root + "Info.plist").length] ^= 1;
    await assert.rejects(inspectUnsignedSigningArchive(new Blob([bytes])), /SIGNING_INPUT_INVALID/);
  });

  test("canonical input rejects signing files, extensions, other roots and noncanonical trailers", async () => {
    for (const extra of [
      "embedded.mobileprovision",
      "_CodeSignature/CodeResources",
      "PlugIns/Extra.appex/Info.plist",
    ]) {
      const blob = await createSignedSigningArchive([
        ...entries(),
        entry(extra, new Uint8Array([1])),
      ]);
      await assert.rejects(inspectUnsignedSigningArchive(blob), /SIGNING_INPUT_INVALID/);
    }
    const blob = await createSignedSigningArchive(entries());
    await assert.rejects(
      inspectUnsignedSigningArchive(new Blob([blob, new Uint8Array([0])])),
      /SIGNING_INPUT_INVALID/,
    );
    const other = { ...entry("other", new Uint8Array([1])), path: "Payload/Other.app/Info.plist" };
    await assert.rejects(
      inspectUnsignedSigningArchive(await createSignedSigningArchive([...entries(), other])),
      /SIGNING_INPUT_INVALID/,
    );
  });

  test("signed writer rejects conflicting paths and honours cancellation", async () => {
    await assert.rejects(
      createSignedSigningArchive([...entries(), entries()[0]]),
      /SIGNING_OUTPUT_INVALID/,
    );
    const controller = new AbortController();
    controller.abort();
    await assert.rejects(createSignedSigningArchive(entries(), { signal: controller.signal }), {
      name: "AbortError",
    });
    await assert.rejects(
      inspectUnsignedSigningArchive(await createSignedSigningArchive(entries()), {
        signal: controller.signal,
      }),
      { name: "AbortError" },
    );
  });

  class SyncBlob extends Blob {
    constructor(bytes) {
      super([bytes]);
      this.bytes = bytes.slice();
    }
    slice(start = 0, end = this.size) {
      return new SyncBlob(this.bytes.slice(start, end));
    }
  }
  function storeFixture() {
    const input = entries().map((e) => ({
      ...e,
      blob: new SyncBlob(
        e.path.endsWith("/Test")
          ? new Uint8Array([0xcf, 0xfa, 0xed, 0xfe, 1, 2, 3, 4])
          : new Uint8Array(e.size).fill(7),
      ),
    }));
    let maxRead = 0;
    const store = createSigningFileStore(
      { appRoot: root, entries: input },
      {
        readBlobSync(blob) {
          maxRead = Math.max(maxRead, blob.size);
          return blob.bytes;
        },
      },
    );
    return {
      store,
      input,
      get maxRead() {
        return maxRead;
      },
    };
  }

  test("Blob store keeps resources immutable, snapshots changed outputs and wipes on disposal", async () => {
    const fixture = storeFixture(),
      { store, input } = fixture;
    assert.equal(store.allocatedBytes, 0);
    assert.throws(
      () => store.write(root + "resource", 0, new Uint8Array([1])),
      /SIGNING_INPUT_INVALID/,
    );
    store.write(root + "Test", 4, new Uint8Array([42]));
    assert.deepEqual(
      store.read(root + "Test", 0, 8),
      new Uint8Array([0xcf, 0xfa, 0xed, 0xfe, 42, 2, 3, 4]),
    );
    assert.equal(input[1].blob.bytes[4], 1);
    const output = store.collectEntries();
    store.write(root + "Test", 4, new Uint8Array([43]));
    assert.equal(
      new Uint8Array(await output.find((e) => e.path === root + "Test").blob.arrayBuffer())[4],
      42,
    );
    assert.ok(fixture.maxRead <= 65536);
    store.dispose();
    store.dispose();
    assert.equal(store.allocatedBytes, 0);
    assert.throws(() => store.read(root + "Test", 0, 1), /SIGNING_INPUT_INVALID/);
  });

  test("store allows closed signing metadata and executable temporary replacement only", () => {
    const { store } = storeFixture();
    store.write(root + "_CodeSignature/CodeResources", 0, new Uint8Array([1, 2]));
    store.write(root + "embedded.mobileprovision", 0, new Uint8Array([3]));
    assert.throws(
      () => store.write(root + "unexpected", 0, new Uint8Array([1])),
      /SIGNING_INPUT_INVALID/,
    );
    store.write(root + "Test.archo.0", 0, new Uint8Array([1, 2, 3]));
    assert.throws(() => store.collectEntries(), /SIGNING_INPUT_INVALID/);
    store.rename(root + "Test.archo.0", root + "Test");
    assert.equal(store.collectEntries().find((e) => e.path === root + "Test").mode, 0o755);
    store.remove(root + "Info.plist");
    assert.throws(() => store.collectEntries(), /SIGNING_INPUT_INVALID/);
    store.dispose();
  });

  test("store validates sizes, sparse ranges, metadata bounds and zeroes truncated bytes", () => {
    const { store } = storeFixture();
    assert.throws(
      () => store.write(root + "Test", SIGNING_FS_LIMITS.mutableFileBytes, new Uint8Array([1])),
      /SIGNING_SIZE_LIMIT/,
    );
    assert.throws(
      () => store.truncate(root + "Info.plist", SIGNING_FS_LIMITS.metadataBytes + 1),
      /SIGNING_SIZE_LIMIT/,
    );
    assert.throws(() => store.read(root + "Test", 0, 65537), /SIGNING_INPUT_INVALID/);
    assert.throws(
      () => store.write(root + "Test", Number.MAX_SAFE_INTEGER, new Uint8Array([1])),
      /SIGNING_INPUT_INVALID/,
    );
    store.truncate(root + "Test", 2);
    store.truncate(root + "Test", 8);
    assert.deepEqual(store.read(root + "Test", 2, 6), new Uint8Array(6));
    store.dispose();
  });

  test("custom mount remaps app root, reads in bounded slices and accepts signed WASM heap writes", async () => {
    const { mountSigningArchive } = await import("../ipa-signing-mount.mjs");
    let rootNode,
      unmounted = false,
      id = 0;
    class ErrnoError extends Error {
      constructor(code) {
        super(String(code));
        this.errno = code;
      }
    }
    const FS = {
      ErrnoError,
      createNode(parent, name, mode) {
        return { id: ++id, parent, name, mode, mount: parent?.mount };
      },
      mkdir(path) {
        assert.equal(path, "/work");
      },
      mount(type, opts, path) {
        assert.equal(path, "/work");
        const mountInfo = { mountpoint: path };
        rootNode = type.mount(mountInfo);
        rootNode.mount = mountInfo;
      },
      unmount(path) {
        assert.equal(path, "/work");
        unmounted = true;
      },
    };
    const archive = {
      appRoot: root,
      entries: entries().map((e) => ({
        ...e,
        blob: new SyncBlob(
          e.path.endsWith("/Test")
            ? new Uint8Array([0xcf, 0xfa, 0xed, 0xfe, 1, 2, 3, 4])
            : new Uint8Array(e.size).fill(7),
        ),
      })),
    };
    const mount = mountSigningArchive({ FS }, archive, { readBlobSync: (blob) => blob.bytes });
    const app = rootNode.contents.Payload.contents["App.app"],
      node = app.contents.Test;
    const assertOwnership = (node) => {
      assert.equal(node.mount, rootNode.mount);
      for (const child of Object.values(node.contents ?? {})) assertOwnership(child);
    };
    assertOwnership(rootNode);
    const buffer = new Int8Array(8);
    assert.equal(node.stream_ops.read({ node }, buffer, 0, 8, 0), 8);
    assert.equal(buffer[0], -49);
    assert.equal(node.stream_ops.write({ node }, new Int8Array([-1]), 0, 1, 4), 1);
    const signature = app.node_ops.mknod(app, "_CodeSignature", 0o040755);
    // 模拟 Emscripten 4.0.20 open 的 mknod(mode|0777) → chmod(mode) 顺序。
    // Model Emscripten 4.0.20 open's mknod(mode|0777) → chmod(mode) sequence.
    const resources = signature.node_ops.mknod(signature, "CodeResources", 0o100777);
    for (const mode of [0o100755, 0o104644, 0o040644])
      assert.throws(() => resources.node_ops.setattr(resources, { mode }), ErrnoError);
    resources.node_ops.setattr(resources, { mode: 0o100666 });
    assert.equal(resources.mode, 0o100644);
    assert.throws(() => resources.node_ops.setattr(resources, { mode: 0o100666 }), ErrnoError);
    const profile = app.node_ops.mknod(app, "embedded.mobileprovision", 0o100777);
    profile.node_ops.setattr(profile, { mode: 0o100644 });
    assert.throws(() => profile.node_ops.setattr(profile, { mode: 0o100666 }), ErrnoError);
    assert.throws(() => node.node_ops.setattr(node, { mode: 0o100666 }), ErrnoError);
    resources.stream_ops.write({ node: resources }, new Int8Array([1]), 0, 1, 0);
    assertOwnership(rootNode);
    assert.throws(() => app.node_ops.mknod(app, "escape", 0o100644), ErrnoError);
    const output = mount.collectEntries();
    assert.equal(
      new Uint8Array(await output.find((e) => e.path === root + "Test").blob.arrayBuffer())[4],
      255,
    );
    const replacement = app.node_ops.mknod(app, "Test.archo.0", 0o100644);
    replacement.stream_ops.write({ node: replacement }, new Int8Array([4, 5, 6]), 0, 3, 0);
    assert.equal(replacement.node_ops.getattr(replacement).mode & 0o777, 0o644);
    app.node_ops.unlink(app, "Test");
    replacement.node_ops.rename(replacement, app, "Test");
    assert.equal(replacement.node_ops.getattr(replacement).mode & 0o777, 0o755);
    assert.equal(mount.collectEntries().find((e) => e.path === root + "Test").mode, 0o755);
    assert.equal(
      new Uint8Array(await output.find((e) => e.path === root + "Test").blob.arrayBuffer())[4],
      255,
    );
    mount.dispose();
    assert.equal(unmounted, true);
    assert.throws(() => mount.collectEntries(), /SIGNING_CLOSED/);
  });

  test("signing ZIP reuses only immutable payload slices and matches the canonical byte writer", async () => {
    const { writeZip } = await import("../zip-writer.mjs");
    const { validateStagedZip } = await import("../staged-zip-validator.mjs");
    const { fileSource } = await import("../preflight.mjs");
    const files = [
      ...entries().filter((file) => !file.path.endsWith("/resource")),
      entry("empty", new Uint8Array()),
      entry(
        "resource",
        Uint8Array.from({ length: 65543 }, (_, i) => i & 255),
      ),
    ];
    const baselineParts = [];
    await writeZip(
      files.map((file) => ({
        ...file,
        async *chunks() {
          for (let offset = 0; offset < file.size; offset += 65536)
            yield new Uint8Array(await file.blob.slice(offset, offset + 65536).arrayBuffer());
        },
      })),
      {
        async write(bytes) {
          baselineParts.push(new Blob([bytes]));
        },
        async close() {},
        async abort() {
          baselineParts.length = 0;
        },
      },
    );
    const baseline = new Blob(baselineParts);
    const NativeBlob = globalThis.Blob,
      originalSlice = NativeBlob.prototype.slice,
      originalRead = NativeBlob.prototype.arrayBuffer;
    const readBuffers = new WeakSet(),
      sourceSlices = new WeakSet();
    let payloadRecopies = 0,
      metadataSnapshots = 0,
      finalParts;
    let output;
    try {
      NativeBlob.prototype.slice = function (...args) {
        const slice = originalSlice.apply(this, args);
        sourceSlices.add(slice);
        return slice;
      };
      NativeBlob.prototype.arrayBuffer = async function () {
        const buffer = await originalRead.call(this);
        readBuffers.add(buffer);
        return buffer;
      };
      globalThis.Blob = new Proxy(NativeBlob, {
        construct(target, args) {
          const parts = args[0] ?? [];
          if (args[1]?.type === "application/octet-stream") finalParts = [...parts];
          if (parts.length === 1 && ArrayBuffer.isView(parts[0])) {
            if (readBuffers.has(parts[0].buffer)) payloadRecopies++;
            else metadataSnapshots++;
          }
          return Reflect.construct(target, args, target);
        },
      });
      output = await createSignedSigningArchive(files);
      assert.equal(payloadRecopies, 0);
      assert.equal(metadataSnapshots, files.length * 3 + 1);
      assert.equal(finalParts.filter((part) => sourceSlices.has(part)).length, 4);
      assert.equal(
        finalParts
          .filter((part) => sourceSlices.has(part))
          .reduce((sum, part) => sum + part.size, 0),
        files.reduce((sum, file) => sum + file.size, 0),
      );
    } finally {
      globalThis.Blob = NativeBlob;
      NativeBlob.prototype.slice = originalSlice;
      NativeBlob.prototype.arrayBuffer = originalRead;
    }
    assert.deepEqual(await output.arrayBuffer(), await baseline.arrayBuffer());
    const receipt = await validateStagedZip(fileSource(output), files, {
      archiveFormat: "zip32",
      maxOutputBytes: 512 * 1024 * 1024,
    });
    assert.equal(receipt.valid, true);
    assert.equal(receipt.entries, files.length);
  });

  test("signing ZIP rejects interrupted or inconsistent source reads without stale slice reuse", async () => {
    const originalRead = Blob.prototype.arrayBuffer;
    for (const failure of ["throw", "short", "different", "abort"]) {
      const controller = new AbortController();
      let reads = 0;
      try {
        Blob.prototype.arrayBuffer = async function () {
          const bytes = await originalRead.call(this);
          if (++reads === 2) {
            if (failure === "throw") throw Error("synthetic read failure");
            if (failure === "short") return bytes.slice(1);
            if (failure === "different") {
              new Uint8Array(bytes)[0] ^= 1;
              return bytes;
            }
            controller.abort();
          }
          return bytes;
        };
        await assert.rejects(
          createSignedSigningArchive(entries(), { signal: controller.signal }),
          failure === "abort" ? { name: "AbortError" } : /SIGNING_OUTPUT_INVALID/,
        );
      } finally {
        Blob.prototype.arrayBuffer = originalRead;
      }
    }
    const output = await createSignedSigningArchive(entries());
    assert.equal((await inspectUnsignedSigningArchive(output)).entries.length, 3);
  });

  function trackedPayload(bytes, onRead = () => {}) {
    const file = entry("Info.plist", bytes),
      reads = [];
    file.blob.slice = (start, end) => {
      const slice = Blob.prototype.slice.call(file.blob, start, end);
      slice.arrayBuffer = async () => {
        const buffer = await Blob.prototype.arrayBuffer.call(slice),
          owned = new Uint8Array(buffer);
        reads.push(owned);
        onRead(owned);
        return buffer;
      };
      return slice;
    };
    return { file, reads };
  }

  for (const mode of ["abort", "short", "consumer"])
    test(`signing ZIP wipes private payload reads after ${mode} failure`, async () => {
      const controller = new AbortController();
      const { file, reads } = trackedPayload(Uint8Array.of(41, 42), () => {
        if (mode === "abort") controller.abort();
      });
      if (mode === "short") file.size++;
      if (mode === "short") {
        const slice = file.blob.slice;
        file.blob.slice = (start, end) => {
          const value = slice(start, end);
          Object.defineProperty(value, "size", { value: value.size + 1 });
          return value;
        };
        Object.defineProperty(file.blob, "size", { value: file.size });
      }
      const prototype = Object.getPrototypeOf(Uint8Array.prototype),
        iterate = prototype[Symbol.iterator];
      if (mode === "consumer") {
        prototype[Symbol.iterator] = function () {
          if (reads.some((bytes) => bytes.buffer === this.buffer))
            throw new Error("synthetic ZIP consumer failure");
          return iterate.call(this);
        };
      }
      try {
        await assert.rejects(
          createSignedSigningArchive([file], { signal: controller.signal }),
          mode === "abort" ? { name: "AbortError" } : /SIGNING_OUTPUT_INVALID/,
        );
      } finally {
        prototype[Symbol.iterator] = iterate;
      }
      assert.equal(reads.length, 1);
      assert.ok(reads[0].every((byte) => byte === 0));
    });

  test("signing ZIP wipes consumed payload buffers without changing immutable output", async () => {
    const input = Uint8Array.from({ length: 65543 }, (_, i) => (i % 251) + 1),
      { file, reads } = trackedPayload(input);
    const output = await createSignedSigningArchive([file]);
    assert.equal(reads.length, 2);
    assert.ok(reads.every((bytes) => bytes.every((byte) => byte === 0)));
    const archive = await inspectUnsignedSigningArchive(output);
    assert.deepEqual(new Uint8Array(await archive.entries[0].blob.arrayBuffer()), input);
    assert.deepEqual(new Uint8Array(await file.blob.arrayBuffer()), input);
  });
}

// ipa-signing-worker
{
  // 合成依赖只测试生命周期与 ABI，不声称合成字节具有有效签名。
  // Synthetic dependencies test lifecycle and ABI only, not signature validity of synthetic bytes.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createSigningFileStore } = ipaSigningFsModule;
  const { createSignedSigningArchive } = ipaSigningZipModule;
  const { createIpaSigningWorkerHandler, invokeSigningNative, SIGNING_NATIVE_PATHS } =
    ipaSigningWorkerCoreModule;
  const makeCredentials = () => ({
    kind: "p12",
    identity: new Uint8Array([1, 2]).buffer,
    profile: new Uint8Array([3, 4]).buffer,
    password: "synthetic",
  });
  const input = () => new Blob([new Uint8Array(30)]);
  function fakeNative(code = 0) {
    const files = new Map(),
      freed = [],
      calls = [];
    let at = 16;
    const native = {
      HEAPU8: new Uint8Array(4096),
      _malloc(size) {
        const p = at;
        at += size;
        return p;
      },
      _free(pointer) {
        freed.push(pointer);
      },
      _zsign_sign_bundle(...args) {
        const strings = args.slice(0, 9).map((p) => {
          const end = native.HEAPU8.indexOf(0, p);
          return new TextDecoder().decode(native.HEAPU8.slice(p, end));
        });
        calls.push({ strings, flags: args.slice(9) });
        return code;
      },
      FS: {
        mkdirTree() {},
        writeFile(path, bytes) {
          files.set(path, { contents: bytes.slice() });
        },
        lookupPath(path) {
          if (!files.has(path)) throw Error("missing");
          return { node: files.get(path) };
        },
        unlink(path) {
          files.delete(path);
        },
      },
    };
    return { native, files, freed, calls };
  }
  function harness(overrides = {}) {
    const mock = fakeNative(),
      messages = [],
      events = [];
    let closed = 0;
    const archive = { appRoot: "Payload/Original.app/", entries: [] };
    const entries = ["embedded.mobileprovision", "_CodeSignature/CodeResources"].map((p) => ({
      path: archive.appRoot + p,
      size: 1,
      mode: 0o644,
      blob: new Blob(["x"]),
    }));
    const dependencies = {
      createNative: async () => mock.native,
      inspectArchive: async () => {
        events.push("inspect");
        return archive;
      },
      mountArchive: () => ({
        collectEntries() {
          events.push("collect");
          return entries;
        },
        dispose() {
          events.push("dispose");
        },
      }),
      createArchive: async () => {
        events.push("package");
        return input();
      },
      postMessage: (message) => {
        if (message.ok && message.id === 1) {
          assert.equal(mock.files.size, 0);
          assert.ok(mock.native.HEAPU8.every((v) => v === 0));
          events.push("reply");
        }
        messages.push(message);
      },
      closeWorker: () => {
        closed++;
      },
      ...overrides,
    };
    const handle = createIpaSigningWorkerHandler(dependencies);
    return {
      ...mock,
      messages,
      events,
      handle,
      get closed() {
        return closed;
      },
    };
  }
  const initialize = (h) =>
    h.handle({ id: 0, type: "initialize", assets: [["zsign.wasm", new ArrayBuffer(8)]] });
  test("one-shot worker composes stages then removes credentials and wipes heap before success", async () => {
    const h = harness();
    await initialize(h);
    const credentials = makeCredentials();
    await h.handle({ id: 1, type: "sign", ipa: input(), credentials });
    assert.deepEqual(h.events, ["inspect", "collect", "dispose", "package", "reply"]);
    assert.equal(h.messages.at(-1).ok, true);
    assert.equal(h.closed, 1);
    assert.equal(credentials.password, "");
    assert.ok(new Uint8Array(credentials.identity).every((v) => v === 0));
    assert.deepEqual(h.calls[0].strings, [
      SIGNING_NATIVE_PATHS.app,
      "",
      SIGNING_NATIVE_PATHS.identity,
      SIGNING_NATIVE_PATHS.profile,
      "synthetic",
      "",
      "",
      "",
      "",
    ]);
    assert.deepEqual(h.calls[0].flags, [0, 1, 1, 0, 0]);
  });
  test("native string allocations are zeroed and freed, including non-ASCII password", () => {
    const h = fakeNative();
    invokeSigningNative(h.native, "测试-password");
    assert.equal(h.calls[0].strings[4], "测试-password");
    assert.equal(h.freed.length, 9);
    assert.ok(h.native.HEAPU8.every((v) => v === 0));
  });
  test("native failure still wipes strings and reports a closed error", () => {
    const h = fakeNative(-203);
    assert.throws(() => invokeSigningNative(h.native, "private"), /SIGNING_IDENTITY_INVALID/);
    assert.equal(h.freed.length, 9);
    assert.ok(h.native.HEAPU8.every((v) => v === 0));
  });
  test("raw packaging failure is sanitized and no partial output is published", async () => {
    const h = harness({
      createArchive: async () => {
        throw Error("private certificate path");
      },
    });
    await initialize(h);
    await h.handle({ id: 1, type: "sign", ipa: input(), credentials: makeCredentials() });
    assert.deepEqual(h.messages.at(-1), { id: 1, ok: false, error: { code: "SIGNING_FAILED" } });
    assert.equal(h.files.size, 0);
    assert.ok(h.native.HEAPU8.every((v) => v === 0));
    assert.equal(h.closed, 1);
  });
  test("missing signature resources never reach packaging", async () => {
    let packaged = false;
    const h = harness({
      mountArchive: () => ({ collectEntries: () => [], dispose() {} }),
      createArchive: async () => {
        packaged = true;
        return input();
      },
    });
    await initialize(h);
    await h.handle({ id: 1, type: "sign", ipa: input(), credentials: makeCredentials() });
    assert.equal(packaged, false);
    assert.equal(h.messages.at(-1).error.code, "SIGNING_OUTPUT_INVALID");
  });
  test("bad inputs and embedded NUL password never reach native", async () => {
    const h = harness();
    await initialize(h);
    const credentials = makeCredentials();
    credentials.password = "a\0b";
    await h.handle({ id: 1, type: "sign", ipa: input(), credentials });
    assert.equal(h.calls.length, 0);
    assert.equal(h.messages.at(-1).error.code, "SIGNING_INPUT_INVALID");
    assert.ok(new Uint8Array(credentials.profile).every((v) => v === 0));
  });
  test("a second request during inspection closes protocol and prevents native continuation", async () => {
    let resume;
    const archive = { appRoot: "Payload/Original.app/", entries: [] };
    const h = harness({
      inspectArchive: () =>
        new Promise((resolve) => {
          resume = () => resolve(archive);
        }),
    });
    await initialize(h);
    const first = h.handle({ id: 1, type: "sign", ipa: input(), credentials: makeCredentials() });
    const credentials = makeCredentials();
    await h.handle({ id: 1, type: "sign", ipa: input(), credentials });
    resume();
    await first;
    assert.equal(h.calls.length, 0);
    assert.equal(h.closed, 1);
    assert.equal(h.messages.at(-1).error.code, "SIGNING_PROTOCOL_INVALID");
    assert.ok(h.native.HEAPU8.every((v) => v === 0));
  });
  test("initialization failure is sanitized; no private work can run after close", async () => {
    const h = harness({
      createNative: async () => {
        throw Error("native private detail");
      },
    });
    await initialize(h);
    assert.equal(h.messages.at(-1).error.code, "SIGNING_INITIALIZATION_FAILED");
    const credentials = makeCredentials();
    await h.handle({ id: 1, type: "sign", ipa: input(), credentials });
    assert.ok(new Uint8Array(credentials.identity).every((v) => v === 0));
    assert.equal(h.closed, 1);
  });

  for (const [nativeCode, errorCode] of [
    [-206, "SIGNING_PROFILE_MISMATCH"],
    [-207, "SIGNING_APPLICATION_ID_TOO_LONG"],
    [-208, "SIGNING_FAILED"],
  ])
    test(`native failure ${nativeCode} wipes credentials and never packages output`, async () => {
      const h = harness();
      h.native._zsign_sign_bundle = () => nativeCode;
      await initialize(h);
      const credentials = makeCredentials();
      await h.handle({ id: 1, type: "sign", ipa: input(), credentials });
      assert.deepEqual(h.messages.at(-1), {
        id: 1,
        ok: false,
        error: { code: errorCode },
      });
      assert.deepEqual(h.events, ["inspect", "dispose"]);
      assert.equal(h.files.size, 0);
      assert.equal(h.freed.length, 9);
      assert.ok(h.native.HEAPU8.every((value) => value === 0));
      assert.equal(credentials.password, "");
      assert.equal(h.closed, 1);
    });

  test("real store snapshots package after mutable storage, credentials and native heap are cleared", async () => {
    class SyncBlob extends Blob {
      constructor(bytes) {
        super([bytes]);
        this.bytes = bytes.slice();
      }
      slice(start = 0, end = this.size) {
        return new SyncBlob(this.bytes.slice(start, end));
      }
    }
    const appRoot = "Payload/Original.app/";
    const original = new Uint8Array([0xcf, 0xfa, 0xed, 0xfe, 1, 2, 3, 4]);
    const archive = {
      appRoot,
      entries: [
        { path: appRoot + "Original", size: 8, mode: 0o755, blob: new SyncBlob(original) },
        {
          path: appRoot + "Info.plist",
          size: 1,
          mode: 0o644,
          blob: new SyncBlob(new Uint8Array([1])),
        },
      ],
    };
    let store,
      disposed = 0;
    const credentials = makeCredentials();
    const h = harness({
      inspectArchive: async () => archive,
      mountArchive: () => {
        store = createSigningFileStore(archive, { readBlobSync: (blob) => blob.bytes });
        store.write(appRoot + "Original", 4, new Uint8Array([42]));
        store.write(appRoot + "embedded.mobileprovision", 0, new Uint8Array([3, 4]));
        store.write(appRoot + "_CodeSignature/CodeResources", 0, new Uint8Array([7]));
        return {
          collectEntries: () => store.collectEntries(),
          dispose() {
            disposed++;
            store.dispose();
          },
        };
      },
      createArchive: async (entries) => {
        assert.equal(disposed, 1);
        assert.equal(store.allocatedBytes, 0);
        assert.equal(h.files.size, 0);
        assert.equal(h.freed.length, 9);
        assert.ok(h.native.HEAPU8.every((value) => value === 0));
        assert.ok(new Uint8Array(credentials.identity).every((value) => value === 0));
        assert.ok(new Uint8Array(credentials.profile).every((value) => value === 0));
        assert.equal(credentials.password, "");
        assert.deepEqual(
          new Uint8Array(
            await entries.find((e) => e.path === appRoot + "Original").blob.arrayBuffer(),
          ),
          new Uint8Array([0xcf, 0xfa, 0xed, 0xfe, 42, 2, 3, 4]),
        );
        return createSignedSigningArchive(entries);
      },
    });
    await initialize(h);
    await h.handle({ id: 1, type: "sign", ipa: input(), credentials });
    assert.equal(disposed, 1);
    assert.equal(h.messages.at(-1).ok, true);
    assert.ok(h.messages.at(-1).result.blob.size > 22);
    assert.equal(original[4], 1);
  });

  test("packaging rejection and closure after early cleanup never publish partial output", async () => {
    for (const closeDuringPackaging of [false, true]) {
      let resume, entered;
      const started = new Promise((resolve) => {
        entered = resolve;
      });
      const h = harness({
        createArchive: () => {
          assert.deepEqual(h.events, ["inspect", "collect", "dispose"]);
          assert.equal(h.files.size, 0);
          assert.ok(h.native.HEAPU8.every((value) => value === 0));
          entered();
          return new Promise((resolve, reject) => {
            resume = () =>
              closeDuringPackaging ? resolve(input()) : reject(Error("private detail"));
          });
        },
      });
      await initialize(h);
      const pending = h.handle({
        id: 1,
        type: "sign",
        ipa: input(),
        credentials: makeCredentials(),
      });
      await started;
      if (closeDuringPackaging)
        await h.handle({ id: 1, type: "sign", ipa: input(), credentials: makeCredentials() });
      resume();
      await pending;
      assert.equal(
        h.messages.some((message) => message.id === 1 && message.ok),
        false,
      );
      assert.equal(
        h.messages.at(-1).error.code,
        closeDuringPackaging ? "SIGNING_PROTOCOL_INVALID" : "SIGNING_FAILED",
      );
      assert.equal(h.events.filter((event) => event === "dispose").length, 1);
      assert.equal(h.closed, 1);
    }
  });

  for (const [nativeCode, errorCode] of [
    [-206, "SIGNING_PROFILE_MISMATCH"],
    [-207, "SIGNING_APPLICATION_ID_TOO_LONG"],
  ])
    test(`${errorCode} survives cleanup failure without publishing an archive`, async () => {
      const h = harness({
        mountArchive: () => ({
          collectEntries() {
            assert.fail("Rejected profile cannot collect signed output");
          },
          dispose() {
            throw Error("unmount failure");
          },
        }),
      });
      h.native._zsign_sign_bundle = () => nativeCode;
      await initialize(h);
      const credentials = makeCredentials();
      await h.handle({ id: 1, type: "sign", ipa: input(), credentials });
      assert.deepEqual(h.messages.at(-1), {
        id: 1,
        ok: false,
        error: { code: errorCode },
      });
      assert.ok(!h.events.includes("package"));
      assert.equal(h.files.size, 0);
      assert.ok(h.native.HEAPU8.every((v) => v === 0));
      assert.equal(credentials.password, "");
      assert.equal(h.closed, 1);
    });

  test("cleanup failure alone remains fatal and never publishes an archive", async () => {
    const h = harness({
      mountArchive: () => ({
        collectEntries: () =>
          ["embedded.mobileprovision", "_CodeSignature/CodeResources"].map((p) => ({
            path: "Payload/Original.app/" + p,
            size: 1,
          })),
        dispose() {
          throw Error("unmount failure");
        },
      }),
    });
    await initialize(h);
    await h.handle({ id: 1, type: "sign", ipa: input(), credentials: makeCredentials() });
    assert.equal(h.messages.at(-1).error.code, "SIGNING_FAILED");
    assert.ok(!h.events.includes("package"));
    assert.equal(h.files.size, 0);
    assert.ok(h.native.HEAPU8.every((v) => v === 0));
    assert.equal(h.closed, 1);
  });
}

// ipa-signing-zip-boundaries
{
  // 用规范 ZIP 的路径字节变更验证上传签名归档的目录大小写边界。
  // Verify uploaded signing archive directory-case boundaries by changing canonical ZIP path bytes.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { writeZip } = zipWriterModule;
  const { inspectUnsignedSigningArchive } = ipaSigningZipModule;

  async function archiveBytes(paths) {
    const parts = [];
    await writeZip(
      paths.map((path) => ({ path, size: 0, async *chunks() {} })),
      {
        async write(bytes) {
          parts.push(bytes.slice());
        },
        async close() {},
        async abort() {},
      },
    );
    return Buffer.concat(parts);
  }

  test("uploaded canonical unsigned IPA rejects differently cased implicit directories", async () => {
    const paths = [
      "Payload/Test.app/Info.plist",
      "Payload/Test.app/Resources/a",
      "Payload/Test.app/Resources/b",
    ];
    const bytes = await archiveBytes(paths);
    const archive = await inspectUnsignedSigningArchive(new Blob([bytes]));
    assert.deepEqual(
      archive.entries.map(({ path }) => path),
      paths,
    );

    // 同时修改本地头和中央目录名称，保持长度、CRC、偏移和其余规范布局不变。
    // Change both local and central names, preserving lengths, CRCs, offsets and the rest of the canonical layout.
    const name = Buffer.from("/Resources/b");
    let changed = 0;
    for (let at = bytes.indexOf(name); at !== -1; at = bytes.indexOf(name, at + name.length)) {
      bytes[at + 1] = "r".charCodeAt(0);
      changed++;
    }
    assert.equal(changed, 2);
    await assert.rejects(inspectUnsignedSigningArchive(new Blob([bytes])), /SIGNING_INPUT_INVALID/);
  });

  test("uploaded canonical unsigned IPA rejects deep implicit directory expansion", async () => {
    const path = "Payload/Test.app/" + "x_".repeat(30) + "file";
    const bytes = await archiveBytes(["Payload/Test.app/Info.plist", path]);
    assert.equal((await inspectUnsignedSigningArchive(new Blob([bytes]))).entries.length, 2);
    const original = Buffer.from(path),
      deep = Buffer.from(path.replaceAll("_", "/"));
    assert.equal(deep.length, original.length);
    assert.equal(deep.toString().split("/").length, 33);
    let changed = 0;
    for (
      let at = bytes.indexOf(original);
      at !== -1;
      at = bytes.indexOf(original, at + deep.length)
    ) {
      bytes.set(deep, at);
      changed++;
    }
    assert.equal(changed, 2);
    await assert.rejects(inspectUnsignedSigningArchive(new Blob([bytes])), /SIGNING_INPUT_INVALID/);
  });
}

// signing-application-id-limit
{
  // 合成归档与源码契约，不执行原生签名或声称验证设备安装。
  // Authored archives and source contracts, not native signing or device-install proof.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { prepareAppCustomization, customizeUnsignedIpa } = appCustomizationModule;
  const { readCustomizationPlist, writeCustomizationPlist } = appCustomizationPlistModule;
  const { createSignedSigningArchive, inspectUnsignedSigningArchive } = ipaSigningZipModule;

  const root = "Payload/Authored.app/";
  test("signing length diagnostics do not truncate or reject supported unsigned bundle IDs", async () => {
    const plist = Object.assign(Object.create(null), {
      CFBundleIdentifier: "local.authored.original",
      CFBundlePackageType: "APPL",
      CFBundleExecutable: "Authored",
    });
    const original = await createSignedSigningArchive(
      [
        ["Info.plist", new Blob([writeCustomizationPlist(plist)]), 0o644],
        ["Authored", new Blob(["inert authored executable"]), 0o755],
      ].map(([path, blob, mode]) => ({ path: root + path, size: blob.size, blob, mode })),
    );
    // 保留 64 字符和 10 字符前缀对应的现有边界。
    // Existing prefix-qualified boundaries for 64-character and 10-character prefixes.
    for (const length of [190, 191, 244, 245, 255]) {
      const bundleIdentifier = "com." + "a".repeat(length - 4);
      const customized = await customizeUnsignedIpa(
        original,
        await prepareAppCustomization({ bundleIdentifier }),
      );
      const archive = await inspectUnsignedSigningArchive(customized.blob);
      const info = archive.entries.find(({ path }) => path === root + "Info.plist");
      const actual = readCustomizationPlist(new Uint8Array(await info.blob.arrayBuffer()));
      assert.equal(actual.CFBundleIdentifier, bundleIdentifier);
    }
  });

  test("native source separates application-ID overflow without widening exact/wildcard matching", async () => {
    const source = await readFile(
      new URL("../toolchain/zsign-wasm/profile-compatibility.inc", import.meta.url),
      "utf8",
    );
    assert.match(source, /value\.size\(\) > \(dotted \? 255U : 64U\)/);
    assert.match(
      source,
      /!D2RIdentifier\(concrete, true\) \|\| pattern\.empty\(\) \|\| pattern\.size\(\) > 255/,
    );
    assert.match(source, /return D2RIdentifier\(pattern, true\) && pattern == concrete/);
    assert.match(
      source,
      /application = prefix \+ "\." \+ bundle;[\s\S]*?if \(application\.size\(\) > 255\) \{\s*if \(applicationIdentifierTooLong\)\s*\*applicationIdentifierTooLong = true;\s*return false;\s*\}\s*if \(!D2RMatchApplication\(pattern, application\)\)/,
    );
    assert.match(
      source,
      /if \(applicationIdentifierTooLong\)\s*\*applicationIdentifierTooLong = false;\s*if \(!D2RIdentifier\(bundle, true\)\)/,
    );
    const bridge = await readFile(
      new URL("../toolchain/zsign-wasm/prepare-source.py", import.meta.url),
      "utf8",
    );
    assert.match(bridge, /bool applicationIdentifierTooLong = false;/);
    assert.match(
      bridge,
      /!zsa\.ValidateProfileForBundle\(infoPlist\["CFBundleIdentifier"\]\.as_string\(\),\s*&applicationIdentifierTooLong\)\)\s*return applicationIdentifierTooLong \? -207 : -206;/,
    );
    const authoredNativeTest = await readFile(
      new URL("../toolchain/zsign-wasm/profile-compatibility-test.cpp", import.meta.url),
      "utf8",
    );
    assert.match(authoredNativeTest, /string\("LEGACY1234"\), string\(64, 'A'\)/);
    assert.match(authoredNativeTest, /supported, supported \+ 1, size_t\(255\)/);
  });
}

// signing-build-receipt
{
  // 合成收据检查不能证明浏览器签名或设备安装成功。
  // Synthetic receipt checks are not browser signing or device installation proof.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { validateSigningBuildReceipt, validateSigningPatchHashes } = packageModule;
  const pins = JSON.parse(
    await readFile(new URL("../toolchain/zsign-wasm/pins.json", import.meta.url), "utf8"),
  );
  function fixture() {
    return {
      pins: structuredClone(pins),
      compiler: "emcc (Emscripten gcc/clang-like replacement) 4.0.20\n",
      report: {
        sourcePins: structuredClone(pins),
        browserExecutionVerified: false,
        deviceInstallVerified: false,
        files: Object.fromEntries(
          [
            "zsign.mjs",
            "zsign.wasm",
            "ZSIGN-LICENSE.txt",
            "PORT-LICENSE.txt",
            "OPENSSL-LICENSE.txt",
            "OPENSSL-NOTICE.txt",
            "pins.json",
            "patch-report.json",
            "compiler-version.txt",
          ].map((name) => [name, { bytes: 16, sha256: "a".repeat(64) }]),
        ),
      },
    };
  }
  test("signer receipt requires the exact native output closure and source pins", () => {
    const value = fixture();
    assert.equal(validateSigningBuildReceipt(value), value.report.files);
  });
  test("missing, changed, oversized or unproven signing outputs cannot be packaged", () => {
    for (const mutate of [
      (v) => delete v.report.files["zsign.wasm"],
      (v) => (v.report.files["unexpected.wasm"] = { bytes: 16, sha256: "a".repeat(64) }),
      (v) => (v.report.files["zsign.wasm"].bytes = 0),
      (v) => (v.report.files["zsign.wasm"].bytes = 64 * 1024 * 1024 + 1),
      (v) => (v.report.files["zsign.wasm"].sha256 = "fake"),
      (v) => (v.report.sourcePins.zsign.commit = "0".repeat(40)),
      (v) => (v.pins.port.commit = "0".repeat(40)),
      (v) => (v.compiler = v.compiler.replace("4.0.20", "4.0.21")),
      (v) => (v.report.browserExecutionVerified = true),
      (v) => (v.report.deviceInstallVerified = true),
    ]) {
      const value = fixture();
      mutate(value);
      assert.throws(() => validateSigningBuildReceipt(value), /Invalid signing build receipt/);
    }
  });

  test("signing local receipts bind all four production patches including profile compatibility", () => {
    const localHashes = Object.fromEntries(
      ["prepare-source.py", "stream-sha.inc", "bounded-map.inc", "profile-compatibility.inc"].map(
        (name, index) => [name, String(index).repeat(64)],
      ),
    );
    const report = { patches: { ...localHashes } };
    assert.doesNotThrow(() => validateSigningPatchHashes(report, localHashes));
    for (const name of Object.keys(localHashes)) {
      const missing = structuredClone(report);
      delete missing.patches[name];
      assert.throws(
        () => validateSigningPatchHashes(missing, localHashes),
        /local source receipt mismatch/,
      );
      const stale = structuredClone(report);
      stale.patches[name] = "f".repeat(64);
      assert.throws(
        () => validateSigningPatchHashes(stale, localHashes),
        /local source receipt mismatch/,
      );
    }
    const missingLocalHelper = { ...localHashes };
    delete missingLocalHelper["profile-compatibility.inc"];
    assert.throws(
      () => validateSigningPatchHashes(report, missingLocalHelper),
      /local source receipt mismatch/,
    );
    assert.throws(() => validateSigningPatchHashes({}, {}), /local source receipt mismatch/);
  });
}
