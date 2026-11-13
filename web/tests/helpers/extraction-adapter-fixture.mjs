// 验证 extraction-adapter-fixture 的约定与边界；依据 ../../extraction-job-adapter.mjs 及本文件的测试断言。
// Verify extraction-adapter-fixture contracts and boundaries against ../../extraction-job-adapter.mjs and this file's assertions.
import assert from "node:assert/strict";
import { createExtractionJobAdapter } from "../../extraction-job-adapter.mjs";
import {
  D2R_APPLICATION_ID as app,
  D2R_PATCH_ID as patchId,
  D2R_WARLOCK_ID as warlock,
  resolveD2rExpansionSelection,
} from "../../d2r-expansion-policy.mjs";
// 一个根目录，以及仅含合成 CASC 清单的 data 目录。
// One root directory and a data directory containing only a synthetic CASC manifest.
function romfsFixture() {
  const name = new TextEncoder().encode("build_manifest.psv");
  const manifest = new TextEncoder().encode("Synthetic manifest header\n");
  const dirStart = 84,
    dirSize = 52,
    hashStart = dirStart + dirSize;
  const fileStart = hashStart + 4,
    fileSize = 32 + Math.ceil(name.length / 4) * 4;
  const bytes = new Uint8Array(256 + manifest.length),
    v = new DataView(bytes.buffer);
  const u32 = (p, x) => v.setUint32(p, x, true),
    u64 = (p, x) => v.setBigUint64(p, BigInt(x), true);
  [80, 80, 4, dirStart, dirSize, hashStart, 4, fileStart, fileSize, 256].forEach((x, i) =>
    u64(i * 8, x),
  );
  u32(80, 0);
  u32(hashStart, 0);
  [0, 0xffffffff, 24, 0xffffffff, 0xffffffff, 0].forEach((x, i) => u32(dirStart + i * 4, x));
  [0, 0xffffffff, 0xffffffff, 0, 0xffffffff, 4].forEach((x, i) => u32(dirStart + 24 + i * 4, x));
  bytes.set(new TextEncoder().encode("data"), dirStart + 48);
  u32(fileStart, 24);
  u32(fileStart + 4, 0xffffffff);
  u64(fileStart + 8, 0);
  u64(fileStart + 16, manifest.length);
  u32(fileStart + 24, 0xffffffff);
  u32(fileStart + 28, name.length);
  bytes.set(name, fileStart + 32);
  bytes.set(manifest, 256);
  return bytes;
}
export function inspectedFixture() {
  const content = (n, type = 1) => ({
    contentId: n.toString(16).padStart(32, "0"),
    sha256: n.toString(16).padStart(64, "0"),
    size: 100n,
    type,
    idOffset: 0,
  });
  const records = [
    {
      type: "Application",
      titleId: app,
      applicationId: app,
      patchId,
      version: 0n,
      contents: [content(1)],
    },
    {
      type: "Patch",
      titleId: patchId,
      applicationId: app,
      version: 65536n,
      contents: [content(2)],
    },
    {
      type: "AddOnContent",
      titleId: warlock,
      applicationId: app,
      version: 0n,
      requiredApplicationVersion: 65536n,
      contents: [content(3, 2)],
    },
  ];
  return {
    records,
    inventory: records.flatMap((r) => r.contents.map((c) => ({ ...c, hashVerified: true }))),
  };
}
const initial = inspectedFixture();
export const selection = resolveD2rExpansionSelection(initial.records, initial.inventory);
export function fixture({
  entries,
  selected = selection,
  maxProgramBytes,
  contentMode,
  verifyRomfs,
  failRead = false,
  inspected = inspectedFixture(),
  romfs = romfsFixture(),
} = {}) {
  let generation = "exefs",
    disposed = false;
  const worker = new MockWorker(({ operation, payload }) => {
    if (operation === "initialize") return inspected;
    if (operation === "select")
      return {
        selection: selected,
        token: generation,
        entries: entries ?? [
          { index: 0, name: "main", size: 70000n, offset: 0n },
          { index: 1, name: "sdk", size: 8n, offset: 70000n },
        ],
      };
    if (operation === "readExefs") {
      assert.equal(payload.token, generation);
      assert.equal(generation, "exefs");
      if (failRead) throw new Error("PRIVATE-CANARY");
      return new Uint8Array(payload.count).fill(payload.index + 1);
    }
    if (operation === "openRomfs") {
      assert.equal(payload.token, generation);
      generation = "romfs";
      return { token: generation, size: BigInt(romfs.length) };
    }
    if (operation === "readRomfs") {
      assert.equal(payload.token, generation);
      return romfs.slice(Number(payload.offset), Number(payload.offset) + payload.count);
    }
    if (operation === "dispose") {
      disposed = true;
      return { disposed: true };
    }
    throw new Error("unknown request");
  });
  const adapter = createExtractionJobAdapter({
    createWorker: () => worker,
    maxProgramBytes,
    contentMode,
    verifyRomfs,
  });
  return {
    adapter,
    worker,
    get disposed() {
      return disposed;
    },
  };
}
export async function open(f, signal) {
  const session = await f.adapter.openSecretSession({
    inputs: { nsps: [], prodKeys: new Blob(["synthetic"]), titleKeys: new Blob(["synthetic"]) },
    signal,
  });
  return session;
}

// 验证 extraction-worker 的约定与边界；依据 web/tests/helpers 及本文件的测试断言。
// Verify extraction-worker contracts and boundaries against web/tests/helpers and this file's assertions.
export class MockWorker {
  listeners = new Map();
  requests = [];
  terminated = false;
  constructor(handler) {
    this.handler = handler;
  }
  addEventListener(type, callback) {
    this.listeners.set(type, callback);
  }
  removeEventListener(type) {
    this.listeners.delete(type);
  }
  terminate() {
    this.terminated = true;
  }
  emit(data) {
    this.listeners.get("message")?.({ data });
  }
  postMessage(data) {
    this.requests.push(structuredClone(data));
    if (this.handler)
      queueMicrotask(async () => {
        try {
          const result = await this.handler(data);
          this.emit({
            id: data.id,
            ok: true,
            result: structuredClone(
              result,
              result instanceof Uint8Array ? { transfer: [result.buffer] } : undefined,
            ),
          });
        } catch {
          this.emit({ id: data.id, ok: false, error: "EXTRACTION_FAILED" });
        }
      });
  }
}
