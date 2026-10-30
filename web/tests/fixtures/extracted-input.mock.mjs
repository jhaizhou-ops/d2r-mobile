// 仅测试替换受信目录，生产 API 不提供目录覆盖入口。
// Replace the trusted catalog only in isolated tests; no production catalog override exists.
import test, { mock } from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
const main = new Uint8Array(70000).fill(0x42),
  sdk = new Uint8Array(100).fill(0x53);
const hash = (bytes) => createHash("sha256").update(bytes).digest("hex");
const release = {
  id: "authored-synthetic",
  titleId: "0100726014352000",
  gameVersion: "synthetic",
  modules: [
    { name: "main", inputSha256: hash(main) },
    { name: "sdk", inputSha256: hash(sdk) },
  ],
};
mock.module("../../program-release-catalog.mjs", {
  namedExports: {
    PROGRAM_REWRITE_LIMITS: {
      moduleInputBytes: 32 * 1024 * 1024,
      totalInputBytes: 64 * 1024 * 1024,
    },
    diagnoseRegisteredProgramDigests(digests) {
      return {
        status: release.modules.every((m) => digests[m.name] === m.inputSha256)
          ? "registered"
          : "unsupported-version",
        releaseId: release.id,
      };
    },
    findRegisteredProgramRelease: () => release,
  },
});
const { openExtractedInput } = await import("../../extracted-input.mjs");
const {
  createExtractionJobAdapter,
  readAuthenticatedProgramModules,
  readAuthenticatedAssets,
  readInputContentPolicy,
} = await import("../../extraction-job-adapter.mjs");
const exportSource = await readFile(
  new URL("../../resource-folder-export.mjs", import.meta.url),
  "utf8",
);
const archiveSizes = [...exportSource.matchAll(/\["([\d-]+\.data)", (\d+)n\]/g)].map(
  ([, name, size]) => ({ name, size: Number(size) }),
);
assert.equal(archiveSizes.length, 18);
class SparseFile extends Blob {
  constructor(size, reads) {
    super([]);
    this.length = size;
    this.reads = reads;
  }
  get size() {
    return this.length;
  }
  slice(start, end) {
    assert(end - start <= 65536);
    this.reads.push([start, end]);
    return new Blob([new Uint8Array(end - start)]);
  }
  arrayBuffer() {
    throw Error("whole archive read forbidden");
  }
}
class BoundedBlob extends Blob {
  constructor(parts, reads) {
    super(parts);
    this.reads = reads;
  }
  slice(start, end) {
    assert(end - start <= 65536);
    this.reads.push([start, end]);
    return super.slice(start, end);
  }
  arrayBuffer() {
    throw Error("whole module read forbidden");
  }
}
function input() {
  const reads = [];
  return {
    reads,
    inputMode: "extracted",
    exefs: [
      { path: "main", file: new BoundedBlob([main], reads) },
      { path: "sdk", file: new BoundedBlob([sdk], reads) },
    ],
    romfs: [
      { path: "data/build_manifest.psv", file: new BoundedBlob(["Synthetic header\n"], reads) },
      ...archiveSizes.map(({ name, size }) => ({
        path: `data/${name}`,
        file: new SparseFile(size, reads),
      })),
    ],
  };
}
test("local folder source requires measured tuple, fixed resource layout, bounded reads and explicit DLC uncertainty", async () => {
  const files = input();
  files.romfs.push({
    path: "data/hd/global/warlock/0100726014353001.bin",
    file: new Blob(["synthetic"]),
  });
  const opened = await openExtractedInput(files);
  assert.equal(opened.selection.inputKind, "extracted");
  assert.equal(opened.selection.dlcMetadata, "not-authenticated");
  assert.equal(opened.selection.contentMode, "base-update-only");
  assert.equal(Object.hasOwn(opened.selection, "base"), false);
  assert.equal(Object.hasOwn(opened.selection, "patch"), false);
  assert.deepEqual(opened.selection.dlc, []);
  assert.equal(files.reads.length, 3, "only modules are read before resource use");
  const archive = opened.romfs.getFile("data/00-00000001.data");
  assert.equal((await archive.readAt(1000000n, 7)).length, 7);
  await assert.rejects(archive.readAt(0n, 65537), { code: "EXTRACTION_READ_BOUNDS" });
  await assert.rejects(archive.readAt(archive.size, 1), { code: "EXTRACTION_READ_BOUNDS" });
  opened.dispose();
  assert(opened.programModules.main.every((b) => b === 0));
  await assert.rejects(archive.readAt(0n, 1), { code: "EXTRACTION_DISPOSED" });
  const bad = input();
  bad.romfs.pop();
  await assert.rejects(openExtractedInput(bad), { code: "EXTRACTED_RESOURCE_LAYOUT" });
  const wrong = input();
  wrong.exefs[0].file = new Blob(["modified"]);
  await assert.rejects(openExtractedInput(wrong), { code: "PROGRAM_UNSUPPORTED_VERSION" });
});
test("extracted adapter creates no worker/keys and binds the exact issued result and selection", async () => {
  let workers = 0;
  const adapter = createExtractionJobAdapter({
    contentMode: "optional-warlock",
    createWorker() {
      workers++;
      throw Error("no worker for local folders");
    },
  });
  const session = await adapter.openSecretSession({ inputs: input() });
  const inspected = await adapter.inspectContents({ session });
  assert.equal(inspected.inputKind, "extracted");
  const selection = await adapter.selectContents({ session });
  const extracted = await adapter.extractSelected({ session, selection });
  assert.equal(workers, 0);
  assert.deepEqual(readInputContentPolicy(extracted, selection), {
    contentMode: "base-update-only",
    inputProvenance: "local-extracted",
    dlcMetadata: "not-authenticated",
  });
  assert.throws(
    () => readInputContentPolicy(extracted, { ...selection, inputKind: "packages" }),
    /PROVENANCE/,
  );
  assert.deepEqual(readAuthenticatedProgramModules(extracted, selection).main, main);
  const assets = readAuthenticatedAssets(extracted, selection);
  assert.equal(assets.romfs.files.length, 19);
  assert.throws(() => readAuthenticatedProgramModules({ ...extracted }, selection), /PROVENANCE/);
  assert.throws(
    () => readAuthenticatedAssets(extracted, { ...selection, dlc: [{}] }),
    /PROVENANCE/,
  );
  await session.dispose();
  assert.throws(() => readAuthenticatedAssets(extracted, selection), /PROVENANCE/);
  assert(extracted.programModules.main.every((b) => b === 0));
  await adapter.cleanup();
});
test("cancellation wipes a late bounded read and revoked capabilities", async () => {
  const controller = new AbortController(),
    files = input(),
    opened = await openExtractedInput(files, { signal: controller.signal });
  const archive = opened.romfs.getFile("data/00-00000001.data");
  controller.abort();
  await assert.rejects(archive.readAt(0n, 5), { name: "AbortError" });
  opened.dispose();
});

test("disposal cancels a stalled local resource read and wipes its late buffer", async () => {
  const files = input();
  const waiting = Promise.withResolvers(),
    started = Promise.withResolvers();
  const late = new Uint8Array(8).fill(9);
  class StalledFile extends SparseFile {
    slice() {
      started.resolve();
      return { arrayBuffer: () => waiting.promise };
    }
  }
  const target = files.romfs.find((row) => row.path === "data/00-00000001.data");
  target.file = new StalledFile(target.file.size, files.reads);
  const opened = await openExtractedInput(files);
  const read = opened.romfs.getFile(target.path).readAt(0n, 8);
  await started.promise;
  opened.dispose();
  await assert.rejects(read, { name: "AbortError" });
  waiting.resolve(late.buffer);
  await new Promise((resolve) => setImmediate(resolve));
  assert(late.every((byte) => byte === 0));
});

for (const phase of ["diagnostic", "async-index"])
  test(`caller mutation during ${phase} cannot promote local provenance`, async () => {
    const files = input(),
      waiting = Promise.withResolvers(),
      started = Promise.withResolvers();
    if (phase === "async-index") {
      class StalledManifest extends Blob {
        slice(start, end) {
          started.resolve();
          return {
            arrayBuffer: async () => {
              await waiting.promise;
              return super.slice(start, end).arrayBuffer();
            },
          };
        }
      }
      files.romfs[0].file = new StalledManifest(["Synthetic header\n"]);
    }
    const adapter = createExtractionJobAdapter({
      contentMode: "optional-warlock",
      createWorker() {
        assert.fail("local route has no worker");
      },
    });
    const session = await adapter.openSecretSession({ inputs: files });
    const selection = await adapter.selectContents({ session });
    const original = structuredClone(selection);
    const mutate = () => {
      selection.inputKind = "packages";
      selection.dlc = [{}];
      selection.dlcMetadata = "authenticated";
    };
    const pending = adapter.extractSelected({
      session,
      selection,
      onDiagnostic: phase === "diagnostic" ? mutate : undefined,
    });
    if (phase === "async-index") {
      await started.promise;
      mutate();
      waiting.resolve();
    }
    const extracted = await pending;
    for (const read of [
      readAuthenticatedProgramModules,
      readAuthenticatedAssets,
      readInputContentPolicy,
    ])
      assert.throws(() => read(extracted, selection), /PROVENANCE/);
    assert.equal(readInputContentPolicy(extracted, original).inputProvenance, "local-extracted");
    const spoofed = { ...selection, toJSON: () => original };
    assert.throws(() => readInputContentPolicy(extracted, spoofed), /PROVENANCE/);
    let claims = 0;
    const getter = { ...original };
    Object.defineProperty(getter, "inputKind", {
      enumerable: true,
      get() {
        return claims++ === 0 ? "extracted" : "packages";
      },
    });
    assert.equal(readInputContentPolicy(extracted, getter).inputProvenance, "local-extracted");
    assert.equal(
      claims,
      1,
      "caller fields are snapshotted only for equality, never trusted for policy",
    );
    await adapter.cleanup();
  });
test("cleanup cancels opening with stalled module read and wipes late bytes", async () => {
  const files = input(),
    waiting = Promise.withResolvers(),
    started = Promise.withResolvers();
  const late = new Uint8Array(65536).fill(0x42);
  class StalledModule extends Blob {
    slice() {
      started.resolve();
      return { arrayBuffer: () => waiting.promise };
    }
  }
  files.exefs[0].file = new StalledModule([main]);
  const adapter = createExtractionJobAdapter({
    contentMode: "optional-warlock",
    createWorker() {
      assert.fail("local route has no worker");
    },
  });
  const opening = adapter.openSecretSession({ inputs: files });
  const rejected = assert.rejects(opening);
  await started.promise;
  await adapter.cleanup();
  await rejected;
  waiting.resolve(late.buffer);
  await new Promise((resolve) => setImmediate(resolve));
  assert(late.every((byte) => byte === 0));
});

for (const sharedSignal of [false, true])
  test(`extraction-stage cancellation revokes local provenance and borrowed bytes (${sharedSignal ? "shared" : "separate"} signal)`, async () => {
    const controller = new AbortController();
    const adapter = createExtractionJobAdapter({
      contentMode: "optional-warlock",
      createWorker() {
        assert.fail("local route has no worker");
      },
    });
    const session = await adapter.openSecretSession({
      inputs: input(),
      signal: sharedSignal ? controller.signal : undefined,
    });
    const selection = await adapter.selectContents({ session });
    const extracted = await adapter.extractSelected({
      session,
      selection,
      signal: controller.signal,
    });
    const borrowed = readAuthenticatedProgramModules(extracted, selection);
    const resource = readAuthenticatedAssets(extracted, selection).romfs.getFile(
      "data/build_manifest.psv",
    );
    controller.abort();
    for (const read of [
      readAuthenticatedProgramModules,
      readAuthenticatedAssets,
      readInputContentPolicy,
    ])
      assert.throws(() => read(extracted, selection), /PROVENANCE/);
    await assert.rejects(resource.readAt(0n, 1));
    assert.throws(() => extracted.logicalAssets.lookup("anything"));
    assert(borrowed.main.every((byte) => byte === 0));
    assert(borrowed.sdk.every((byte) => byte === 0));
    await adapter.cleanup();
  });
