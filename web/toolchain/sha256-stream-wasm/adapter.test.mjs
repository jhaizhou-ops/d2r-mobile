// 模拟 ABI 仅测试有界读取与生命周期；密码正确性由独立原生/WASM 测试验证。
// Mock ABI tests cover bounded reads and lifecycle only; separate native/WASM checks establish hash correctness.
import assert from "node:assert/strict";
import test from "node:test";
import { createHash } from "node:crypto";
import {
  createSha256FileHasher,
  createSha256FileHasherFromAbi,
  SHA256_CHUNK_BYTES,
} from "./adapter.mjs";
const expected = (bytes) => createHash("sha256").update(bytes).digest("hex");
function fixture() {
  const memory = new WebAssembly.Memory({ initial: 4, maximum: 4 });
  let active = 0,
    sequence = 0,
    hash;
  const calls = [];
  const abi = {
    memory,
    sha_input: () => 0,
    sha_output: () => SHA256_CHUNK_BYTES,
    sha_begin() {
      if (active) return 0;
      active = ++sequence;
      hash = createHash("sha256");
      calls.push(["begin", active]);
      return active;
    },
    sha_update(owner, length) {
      if (owner !== active) throw new Error("wrong owner update");
      calls.push(["update", owner, length]);
      hash.update(new Uint8Array(memory.buffer, 0, length));
      new Uint8Array(memory.buffer, 0, SHA256_CHUNK_BYTES).fill(0);
      return 0;
    },
    sha_finish(owner) {
      if (owner !== active) throw new Error("wrong owner finish");
      new Uint8Array(memory.buffer, SHA256_CHUNK_BYTES, 32).set(hash.digest());
      calls.push(["finish", owner]);
      active = 0;
      return 0;
    },
    sha_abort(owner) {
      calls.push(["abort", owner]);
      if (owner !== active) throw new Error("wrong owner abort");
      active = 0;
      hash = undefined;
      new Uint8Array(memory.buffer).fill(0);
      return 0;
    },
  };
  return {
    abi,
    calls,
    hasher: createSha256FileHasherFromAbi(abi),
    get active() {
      return active;
    },
  };
}

test("mock ABI: bounded File slices, exact byte count, empty input and reuse", async () => {
  const f = fixture(),
    reads = [],
    readBuffers = [],
    bytes = new Uint8Array(2 * SHA256_CHUNK_BYTES + 19).fill(31);
  class LocalFile extends Blob {
    arrayBuffer() {
      throw new Error("whole file buffering forbidden");
    }
    slice(start, end) {
      reads.push([start, end]);
      assert.ok(end - start <= SHA256_CHUNK_BYTES);
      const slice = super.slice(start, end),
        read = slice.arrayBuffer.bind(slice);
      slice.arrayBuffer = async () => {
        const buffer = await read();
        readBuffers.push(new Uint8Array(buffer));
        return buffer;
      };
      return slice;
    }
  }
  const result = await f.hasher.hashFile(new LocalFile([bytes]));
  assert.deepEqual(result, { sha256: expected(bytes), bytesHashed: bytes.length });
  assert.ok(Object.isFrozen(result));
  assert.ok(readBuffers.every((buffer) => buffer.every((byte) => byte === 0)));
  assert.ok(bytes.every((byte) => byte === 31));
  assert.ok(
    new Uint8Array(f.abi.memory.buffer, SHA256_CHUNK_BYTES, 32).every((byte) => byte === 0),
  );
  assert.deepEqual(
    reads.map(([a, b]) => b - a),
    [65536, 65536, 19],
  );
  assert.equal(f.active, 0);
  assert.deepEqual(await f.hasher.hashFile(new Blob()), {
    sha256: expected(new Uint8Array()),
    bytesHashed: 0,
  });
});

test("mock ABI: pending-read cancellation frees owner, late completion cannot touch a new hash", async () => {
  const f = fixture(),
    controller = new AbortController();
  let resolveOld,
    requested = false;
  class Pending extends Blob {
    slice() {
      return {
        arrayBuffer() {
          requested = true;
          return new Promise((resolve) => {
            resolveOld = resolve;
          });
        },
      };
    }
  }
  const old = f.hasher.hashFile(new Pending([new Uint8Array(4)]), { signal: controller.signal });
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(requested, true);
  await assert.rejects(f.hasher.hashFile(new Blob()), /busy/);
  controller.abort();
  assert.equal(f.active, 0);
  const next = f.hasher.hashFile(new Blob([new Uint8Array([7])]));
  await assert.rejects(old, { name: "AbortError" });
  const oldBytes = new Uint8Array(4).fill(41);
  resolveOld(oldBytes.buffer);
  assert.deepEqual(await next, { sha256: expected(new Uint8Array([7])), bytesHashed: 1 });
  await new Promise((resolve) => setImmediate(resolve));
  assert.ok(oldBytes.every((byte) => byte === 0));
  assert.deepEqual(
    f.calls.filter(([kind]) => kind === "abort"),
    [["abort", 1]],
  );
  assert.deepEqual(
    f.calls.filter(([kind]) => kind === "update"),
    [["update", 2, 1]],
  );
});

test("mock ABI: read errors, short slices and update errors release state and preserve failure", async () => {
  for (const kind of ["read", "short", "update"]) {
    const f = fixture();
    let captured;
    class BadFile extends Blob {
      slice() {
        return {
          async arrayBuffer() {
            if (kind === "read") throw new Error("primary read");
            captured = new Uint8Array(kind === "short" ? 2 : 1).fill(41);
            return captured.buffer;
          },
        };
      }
    }
    if (kind === "update") f.abi.sha_update = () => -1;
    await assert.rejects(
      f.hasher.hashFile(new BadFile([new Uint8Array(1)])),
      kind === "read" ? /primary read/ : kind === "short" ? /short/ : /update failed/,
    );
    assert.equal(f.active, 0);
    assert.equal(f.calls.at(-1)[0], "abort");
    assert.ok(!captured || captured.every((byte) => byte === 0));
  }
});

test("mock ABI: pre-aborted/invalid inputs never acquire context", async () => {
  const f = fixture(),
    controller = new AbortController();
  controller.abort();
  await assert.rejects(f.hasher.hashFile(new Blob(), { signal: controller.signal }), {
    name: "AbortError",
  });
  await assert.rejects(f.hasher.hashFile({ size: 1, slice() {} }), /File or Blob/);
  await assert.rejects(f.hasher.hashFile(new Blob(), { signal: {} }), /AbortSignal/);
  assert.deepEqual(f.calls, []);
});

// 手写模块仅声明可增长内存，不含任何密码实现，用于验证工厂拒绝路径。
// The tiny test module declares growable memory only, with no crypto, to exercise factory rejection.
test("module factory rejects growable memory before accepting an ABI", async () => {
  const module = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0, 5, 4, 1, 1, 4, 5, 7, 10, 1, 6, 109, 101, 109, 111, 114, 121, 2, 0,
  ]);
  await assert.rejects(createSha256FileHasher(module), /memory must not grow/);
});

// 进度只包含完成散列的单调计数；同步及异步观察者失败不能破坏结果或泄漏所有者。
// Progress contains only monotonic completed-hash counts; sync/async observer failures cannot corrupt results or leak owners.
test("mock ABI: completed hash progress is monotonic and observer failures are isolated", async () => {
  const f = fixture(),
    bytes = new Uint8Array(65537),
    events = [],
    evidence = [];
  const result = await f.hasher.hashFile(new Blob([bytes]), {
    onProgress: (value) => {
      evidence.push([Object.isFrozen(value), f.calls.at(-1)[0]]);
      events.push(value);
      if (events.length === 1) throw new Error("observer");
      return Promise.reject(new Error("observer async"));
    },
  });
  assert.deepEqual(events, [
    { bytesHashed: 65536, totalBytes: 65537 },
    { bytesHashed: 65537, totalBytes: 65537 },
  ]);
  assert.deepEqual(evidence, [
    [true, "update"],
    [true, "update"],
  ]);
  assert.equal(result.sha256, expected(bytes));
  assert.equal(f.active, 0);
  await assert.rejects(f.hasher.hashFile(new Blob(), { onProgress: true }), /observer/);
});

test("mock ABI: cancellation during File fulfillment wipes owned bytes and preserves reason", async () => {
  const f = fixture(),
    controller = new AbortController(),
    reason = new Error("synthetic cancellation"),
    captured = Uint8Array.of(41, 42);
  class CancelledFile extends Blob {
    slice() {
      return {
        async arrayBuffer() {
          controller.abort(reason);
          return captured.buffer;
        },
      };
    }
  }
  await assert.rejects(
    f.hasher.hashFile(new CancelledFile([Uint8Array.of(41, 42)]), { signal: controller.signal }),
    (error) => error === reason,
  );
  await new Promise((resolve) => setImmediate(resolve));
  assert.ok(captured.every((byte) => byte === 0));
  assert.equal(f.active, 0);
  assert.equal(
    f.calls.some(([kind]) => kind === "update"),
    false,
  );
});

test("mock ABI: cancelled late File rejection is observed without touching the next hash", async () => {
  const f = fixture(),
    controller = new AbortController();
  let rejectRead;
  class PendingFile extends Blob {
    slice() {
      return {
        arrayBuffer: () =>
          new Promise((_, reject) => {
            rejectRead = reject;
          }),
      };
    }
  }
  const reading = f.hasher.hashFile(new PendingFile([Uint8Array.of(41)]), {
    signal: controller.signal,
  });
  await new Promise((resolve) => setImmediate(resolve));
  controller.abort();
  await assert.rejects(reading, { name: "AbortError" });
  rejectRead(new Error("synthetic late read failure"));
  const bytes = Uint8Array.of(42);
  assert.deepEqual(await f.hasher.hashFile(new Blob([bytes])), {
    sha256: expected(bytes),
    bytesHashed: 1,
  });
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(
    f.calls.filter(([kind]) => kind === "abort"),
    [["abort", 1]],
  );
});
