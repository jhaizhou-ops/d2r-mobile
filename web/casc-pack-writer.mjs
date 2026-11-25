// 依据原生 CASC 规则串行暂存补充包；由生产 ASTC 工作线程调用，保留原样或压缩 BLTE 规格。
// Serial supplemental-pack staging from native CASC rules, used by the production ASTC worker; preserves raw or compressed BLTE specs.
import { createRawFileStage, RAW_FILE_STAGE_CHUNK_BYTES } from "./raw-file-stage.mjs";
import { createMD5, md5 } from "hash-wasm";
import { CASC_PACK_METADATA_LIMITS } from "./toolchain/casc-overlay-js/overlay.mjs";
const BLOCK = 256 * 1024,
  U32 = 0xffffffff,
  OFFSET_LIMIT = 2 ** 40;
const text = new TextEncoder();
const fail = (code) => {
  throw new Error(`CASC_PACK_${code}`);
};
const integer = (value, max) => Number.isSafeInteger(value) && value >= 0 && value <= max;
const hex = (bytes) => Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
const fromHex = (value) => Uint8Array.from(value.match(/../g), (b) => parseInt(b, 16));
const put32 = (bytes, offset, value) => new DataView(bytes.buffer).setUint32(offset, value, false);
function chunk(bytes) {
  if (
    !(bytes instanceof Uint8Array) ||
    !(bytes.buffer instanceof ArrayBuffer) ||
    bytes.buffer.resizable === true ||
    bytes.buffer.byteLength > RAW_FILE_STAGE_CHUNK_BYTES
  )
    fail("INVALID_CHUNK");
  return bytes;
}
function pathValue(path) {
  if (
    typeof path !== "string" ||
    !path ||
    path.length > 4096 ||
    path.startsWith("#") ||
    path.startsWith("+") ||
    path !== path.toLowerCase() ||
    path !== path.trim() ||
    path.includes(":") ||
    path.includes("\\") ||
    Array.from(path).some(
      (c) =>
        c.codePointAt(0) < 32 ||
        c.codePointAt(0) === 127 ||
        (c.codePointAt(0) >= 0xd800 && c.codePointAt(0) <= 0xdfff),
    ) ||
    path.split("/").some((p) => !p || p === "." || p === "..") ||
    text.encode(path).length > 4096
  )
    fail("INVALID_PATH");
  return path;
}

// 取消只停止等待，不伪称能强制中断宿主 I/O；未完成的清理通过错误上的 cleanup Promise 暴露。
// Cancellation stops waiting, not host I/O by force; unfinished cleanup is exposed as a cleanup Promise on the error.
export const CASC_PACK_CLEANUP_WAIT_MS = 1000;
function boundedIO(signal) {
  const active = new Set(),
    closing = [];
  const wait = async (operation) => {
    signal?.throwIfAborted();
    const work = Promise.resolve().then(() => {
      signal?.throwIfAborted();
      return operation();
    });
    if (!signal) return work;
    let onAbort;
    try {
      return await Promise.race([
        work,
        new Promise((_, reject) => {
          onAbort = () => reject(signal.reason);
          signal.addEventListener("abort", onAbort, { once: true });
          if (signal.aborted) onAbort();
        }),
      ]);
    } finally {
      signal.removeEventListener("abort", onAbort);
    }
  };
  const schedule = (operation) => {
    const work = Promise.resolve().then(operation);
    work.catch(() => {});
    closing.push(work);
  };
  const close = (iterator) => {
    if (!active.delete(iterator)) return;
    schedule(() => iterator.return?.());
  };
  return {
    wait,
    async *iterate(source) {
      if (typeof source?.[Symbol.asyncIterator] !== "function") fail("ASYNC_SOURCE_REQUIRED");
      const iterator = source[Symbol.asyncIterator]();
      active.add(iterator);
      try {
        while (true) {
          const item = await wait(() => iterator.next());
          if (!item || typeof item !== "object") fail("INVALID_ITERATOR_RESULT");
          if (item.done) {
            active.delete(iterator);
            return;
          }
          yield item.value;
        }
      } finally {
        close(iterator);
      }
    },
    async cleanup(cause, stage) {
      for (const iterator of active) close(iterator);
      schedule(() => stage.dispose());
      const completion = Promise.allSettled(closing);
      let timer;
      const pending = Symbol("pending");
      const result = await Promise.race([
        completion,
        new Promise((resolve) => {
          timer = setTimeout(() => resolve(pending), CASC_PACK_CLEANUP_WAIT_MS);
        }),
      ]).finally(() => clearTimeout(timer));
      if (result === pending) {
        const error = new AggregateError(
          [cause, new Error("CASC_PACK_CLEANUP_PENDING")],
          "CASC_PACK_OPERATION_AND_CLEANUP_FAILED",
          { cause },
        );
        error.cleanup = completion;
        throw error;
      }
      const failures = result
        .filter((item) => item.status === "rejected")
        .map((item) => item.reason);
      if (failures.length)
        throw new AggregateError([cause, ...failures], "CASC_PACK_OPERATION_AND_CLEANUP_FAILED", {
          cause,
        });
      throw cause;
    },
  };
}

async function* exactSource(source, size, signal, state, io) {
  if (typeof source?.[Symbol.asyncIterator] !== "function") fail("ASYNC_SOURCE_REQUIRED");
  let count = 0;
  for await (const value of io.iterate(source)) {
    signal?.throwIfAborted();
    const bytes = chunk(value);
    if (bytes.length > size - count) fail("DECODED_SIZE");
    count += bytes.length;
    if (bytes.length) yield bytes;
  }
  signal?.throwIfAborted();
  if (count !== size) fail("DECODED_SIZE");
  state.complete = true;
}
async function* blocks(input, io, blockSize = BLOCK) {
  let buffer = new Uint8Array(blockSize),
    used = 0;
  for await (const bytes of io.iterate(input)) {
    for (let at = 0; at < bytes.length;) {
      const length = Math.min(blockSize - used, bytes.length - at);
      buffer.set(bytes.subarray(at, at + length), used);
      used += length;
      at += length;
      if (used === blockSize) {
        yield buffer;
        buffer = new Uint8Array(blockSize);
        used = 0;
      }
    }
  }
  if (used) yield buffer.subarray(0, used);
}
async function* one(bytes) {
  yield bytes;
}
// 原生 ekey_for 按魔数识别 BLTE，即使条目规格为 n；最多保留前八字节再决定哈希范围。
// Native ekey_for recognizes BLTE by magic even for n records; retain at most eight bytes before choosing the hash extent.
async function rawHasher() {
  let hash = await createMD5();
  const prefix = new Uint8Array(8);
  let used = 0,
    decided = false,
    limit = Infinity,
    hashed = 0;
  const feed = (bytes) => {
    const length = Math.min(bytes.length, limit - hashed);
    if (length > 0) hash.update(bytes.subarray(0, length));
    hashed += length;
  };
  const decide = () => {
    if (used >= 4 && hex(prefix.subarray(0, 4)) === "424c5445") {
      if (used < 8) fail("SHORT_BLTE_PREFIX");
      limit = new DataView(prefix.buffer).getUint32(4, false);
    }
    decided = true;
    feed(prefix.subarray(0, used));
  };
  return {
    update(bytes) {
      if (decided) return feed(bytes);
      const length = Math.min(8 - used, bytes.length);
      prefix.set(bytes.subarray(0, length), used);
      used += length;
      if (used === 8) {
        decide();
        feed(bytes.subarray(length));
      }
    },
    digest() {
      if (!decided) decide();
      return hash.digest();
    },
    dispose() {
      hash = null;
    },
  };
}

/**
 * jobs 为已解析现有路径的异步序列 {path,spec,decodedSize,source}；此处不认证路径关联。
 * jobs is an async sequence of resolved existing paths {path,spec,decodedSize,source}; this does not authenticate path associations.
 * locationAvailable 必须确认原始目录及计划输出中均无同名档；它不是原子预留或安全边界。
 * locationAvailable must confirm no same-name archive in originals or planned outputs; it is not an atomic reservation or security boundary.
 * hashFile(file,{signal,chunkBytes}) 必须有界读取并返回 {sha256,bytesHashed}；哈希仅描述本次生成内容。
 * hashFile(file,{signal,chunkBytes}) must read bounded chunks and return {sha256,bytesHashed}; the hash describes generated content only.
 */
export async function writeCascPack({
  jobs,
  location,
  locationAvailable,
  root,
  maxPackBytes,
  expectedEntries,
  maxEntries,
  codec,
  hashFile,
  signal,
  createStage = createRawFileStage,
} = {}) {
  if (
    !Array.isArray(location) ||
    location.length !== 2 ||
    location.some((n) => !integer(n, 255)) ||
    !integer(maxPackBytes, OFFSET_LIMIT) ||
    (expectedEntries === undefined && maxEntries === undefined) ||
    (expectedEntries !== undefined &&
      !integer(expectedEntries, CASC_PACK_METADATA_LIMITS.entries)) ||
    (maxEntries !== undefined && !integer(maxEntries, CASC_PACK_METADATA_LIMITS.entries)) ||
    (expectedEntries !== undefined && maxEntries !== undefined && expectedEntries > maxEntries) ||
    typeof jobs?.[Symbol.asyncIterator] !== "function" ||
    typeof locationAvailable !== "function" ||
    typeof hashFile !== "function" ||
    typeof createStage !== "function" ||
    typeof codec?.compressZlibStream !== "function" ||
    codec.genericZlibVersion !== "1.2.13"
  )
    fail("INVALID_OPTIONS");
  // 精确计数仍是更严格的上限；仅上限模式不证明枚举完整性。
  // An exact count remains the tighter bound; maximum-only mode does not prove enumeration completeness.
  const entryLimit = expectedEntries ?? maxEntries;
  if (signal !== undefined && !(signal instanceof AbortSignal)) fail("INVALID_SIGNAL");
  signal?.throwIfAborted();
  const [part, archive] = location;
  const name = `${String(part).padStart(2, "0")}-${String(archive).padStart(8, "0")}.data`;
  const io = boundedIO(signal);
  if ((await io.wait(() => locationAvailable(name, { signal }))) !== true)
    fail("LOCATION_COLLISION");
  signal?.throwIfAborted();
  const acquiring = Promise.resolve().then(() =>
    createStage({ root, maxBytes: maxPackBytes, signal }),
  );
  let stage;
  try {
    stage = await io.wait(() => acquiring);
  } catch (cause) {
    // 迟到的获取仍归本次操作所有；不因取消而丢失其清理句柄。
    // A late acquisition remains owned by this operation; cancellation must not lose its cleanup handle.
    return io.cleanup(cause, {
      async dispose() {
        let late;
        try {
          late = await acquiring;
        } catch (failure) {
          if (failure !== cause) throw failure;
          return;
        }
        await late.dispose();
      },
    });
  }
  let total = 0,
    pathsSize = 0;
  const records = [],
    paths = new Set(),
    index = [`# pack\t${name}\n`];
  const append = async (bytes) => {
    signal?.throwIfAborted();
    chunk(bytes);
    if (bytes.length > maxPackBytes - total) fail("PACK_SIZE");
    await io.wait(() => stage.append(bytes));
    total += bytes.length;
    signal?.throwIfAborted();
  };
  try {
    for await (const item of io.iterate(jobs)) {
      const job = item && {
        path: item.path,
        spec: item.spec,
        decodedSize: item.decodedSize,
        source: item.source,
      };
      signal?.throwIfAborted();
      if (
        records.length >= entryLimit ||
        !job ||
        !integer(job.decodedSize, U32) ||
        !["n", "z", "b:64K*=n", "b:256K*=z"].includes(job.spec)
      )
        fail("INVALID_JOB");
      const path = pathValue(job.path);
      pathsSize += text.encode(path).length;
      if (paths.has(path) || pathsSize > CASC_PACK_METADATA_LIMITS.pathBytes)
        fail("METADATA_LIMIT");
      paths.add(path);
      const offset = total,
        state = { complete: false };
      if (offset >= OFFSET_LIMIT) fail("OFFSET_RANGE");
      const input = exactSource(job.source, job.decodedSize, signal, state, io);
      let encodedSize = 0,
        digest;
      const emit = async (bytes) => {
        chunk(bytes);
        if (bytes.length > U32 - encodedSize) fail("ENCODED_SIZE");
        await append(bytes);
        encodedSize += bytes.length;
      };
      const compress = (source, size) =>
        codec.compressZlibStream(source, {
          maxInputBytes: size,
          maxOutputBytes: U32 - encodedSize,
          level: 9,
          signal,
        });
      if (job.spec === "b:256K*=z" || job.spec === "b:64K*=n") {
        const rawBlocks = job.spec === "b:64K*=n",
          blockSize = rawBlocks ? 64 * 1024 : BLOCK,
          count = Math.max(1, Math.ceil(job.decodedSize / blockSize));
        if (rawBlocks) {
          const size = 12 + 25 * count + job.decodedSize;
          if (size > U32) fail("ENCODED_SIZE");
          if (size > maxPackBytes - total) fail("PACK_SIZE");
        }
        const header = new Uint8Array(12 + 24 * count);
        header.set([66, 76, 84, 69]);
        put32(header, 4, header.length);
        header[8] = 15;
        header[9] = count >>> 16;
        header[10] = count >>> 8;
        header[11] = count;
        // 原样块的最大头部超过暂存单块上限；预留和回填都分段复制。
        // Raw-block headers can exceed one stage chunk; reserve and backpatch bounded copies.
        for (let at = 0; at < header.length; at += RAW_FILE_STAGE_CHUNK_BYTES)
          await emit(new Uint8Array(Math.min(RAW_FILE_STAGE_CHUNK_BYTES, header.length - at)));
        let blockIndex = 0;
        for await (const block of blocks(input, io, blockSize)) {
          let hash = await createMD5();
          const start = encodedSize,
            blockState = { complete: false };
          try {
            const mode = new Uint8Array([rawBlocks ? 78 : 90]);
            hash.update(mode);
            await emit(mode);
            if (rawBlocks) {
              hash.update(block);
              await emit(block);
            } else {
              for await (const bytes of io.iterate(
                compress(
                  exactSource(one(block), block.length, signal, blockState, io),
                  block.length,
                ),
              )) {
                chunk(bytes);
                hash.update(bytes);
                await emit(bytes);
              }
              if (!blockState.complete || encodedSize - start < 2) fail("CODEC_INCOMPLETE");
            }
            put32(header, 12 + 24 * blockIndex, encodedSize - start);
            put32(header, 16 + 24 * blockIndex, block.length);
            header.set(fromHex(hash.digest()), 20 + 24 * blockIndex);
          } finally {
            hash = null;
          }
          blockIndex++;
        }
        if (job.decodedSize === 0) {
          const mode = new Uint8Array([78]);
          await emit(mode);
          put32(header, 12, 1);
          header.set(fromHex(await md5(mode)), 20);
          blockIndex++;
        }
        if (blockIndex !== count) fail("BLOCK_COUNT");
        for (let at = 0; at < header.length; at += RAW_FILE_STAGE_CHUNK_BYTES)
          await io.wait(() =>
            stage.backpatch(offset + at, header.slice(at, at + RAW_FILE_STAGE_CHUNK_BYTES)),
          );
        signal?.throwIfAborted();
        digest = await md5(header);
      } else {
        const hash = await rawHasher();
        try {
          const output = job.spec === "n" ? input : compress(input, job.decodedSize);
          for await (const bytes of io.iterate(output)) {
            chunk(bytes);
            hash.update(bytes);
            await emit(bytes);
          }
          digest = hash.digest();
        } finally {
          hash.dispose();
        }
      }
      if (!state.complete) fail("SOURCE_NOT_CONSUMED");
      if (job.spec === "z" && encodedSize === 0) fail("CODEC_INCOMPLETE");
      const ekey =
        digest.slice(0, 18) +
        hex(new Uint8Array([part, archive])) +
        offset.toString(16).padStart(10, "0");
      const record = Object.freeze({
        path,
        newEkey: ekey,
        encodedSize,
        decodedSize: job.decodedSize,
      });
      records.push(record);
      index.push(`${path}\t${ekey}\t${encodedSize}\t${job.decodedSize}\n`);
    }
    if (expectedEntries !== undefined && records.length !== expectedEntries) fail("ENTRY_COUNT");
    signal?.throwIfAborted();
    const source = await io.wait(() => stage.finalize());
    if (source.size !== total || source.file?.size !== total || typeof source.readAt !== "function")
      fail("FINAL_SIZE");
    const result = await io.wait(() =>
      hashFile(source.file, { signal, chunkBytes: RAW_FILE_STAGE_CHUNK_BYTES }),
    );
    signal?.throwIfAborted();
    if (result?.bytesHashed !== total || !/^[0-9a-f]{64}$/.test(result?.sha256 ?? ""))
      fail("FINAL_HASH");
    return Object.freeze({
      source,
      count: records.length,
      index: index.join(""),
      records: Object.freeze(records),
      pack: Object.freeze({ name, size: total, sha256: result.sha256 }),
      dispose: () => stage.dispose(),
    });
  } catch (cause) {
    return io.cleanup(cause, stage);
  }
}
