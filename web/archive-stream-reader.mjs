// 依据 archive-reader 的 N/Z/BLTE 校验规则与 raw-file-stage，增量解码到私有暂存文件。
// Incrementally decode into a private stage using archive-reader N/Z/BLTE checks and raw-file-stage.
import { ARCHIVE_LIMITS } from "./archive-reader.mjs";
import { createRawFileStage } from "./raw-file-stage.mjs";
import { createMD5, md5 } from "hash-wasm";
export const ARCHIVE_STREAM_READ_BYTES = 65536;
export const ARCHIVE_STREAM_CACHE_BYTES = 1024 * 1024;
export const ARCHIVE_STREAM_CLEANUP_WAIT_MS = 1000;
const U32 = 0xffffffff;
const fail = (code) => {
  throw new Error(`ARCHIVE_STREAM_${code}`);
};
const integer = (v, max = U32) => Number.isSafeInteger(v) && v >= 0 && v <= max;
const hex = (bytes) => Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
const u32 = (bytes, at) =>
  new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(at, false);
const abort = (signal) => {
  if (signal?.aborted) throw new DOMException("Cancelled", "AbortError");
};
async function waiting(operation, signal) {
  abort(signal);
  const pending = Promise.resolve().then(() => {
    abort(signal);
    return operation();
  });
  if (!signal) return pending;
  let stop;
  try {
    return await Promise.race([
      pending,
      new Promise((_resolve, reject) => {
        stop = () => reject(new DOMException("Cancelled", "AbortError"));
        signal.addEventListener("abort", stop, { once: true });
        if (signal.aborted) stop();
      }),
    ]);
  } finally {
    signal.removeEventListener("abort", stop);
  }
}
async function cleanup(stagePromise, cause, primary) {
  const completion = Promise.resolve(stagePromise).then(
    (stage) => stage?.dispose(),
    (failure) => {
      // 不重复主错误或正常取消；独立的迟到获取失败仍须通过清理句柄报告。
      // Do not duplicate the primary error or normal cancellation; retain distinct late acquisition failures.
      if (failure !== primary && !(failure?.name === "AbortError" && cause?.name === "AbortError"))
        throw failure;
    },
  );
  completion.catch(() => {});
  let timer;
  const pending = Symbol("pending");
  const result = await Promise.race([
    completion.then(
      () => null,
      () => "failed",
    ),
    new Promise((resolve) => {
      timer = setTimeout(() => resolve(pending), ARCHIVE_STREAM_CLEANUP_WAIT_MS);
    }),
  ]).finally(() => clearTimeout(timer));
  if (result !== null) {
    const error = new Error(
      result === pending ? "ARCHIVE_STREAM_CLEANUP_PENDING" : "ARCHIVE_STREAM_CLEANUP_FAILED",
    );
    error.cleanup = completion;
    error.cause = cause;
    throw error;
  }
  throw cause;
}
// 1 KiB 压缩输入按背压推进；输出立即切成独占 64 KiB 块，不能绕过声明的解码大小。
// Feed compressed input in 1 KiB backpressured slices; split output into owned 64 KiB chunks within declared size.
async function inflate(input, expected, emit, signal) {
  if (typeof DecompressionStream !== "function") fail("DEFLATE_UNAVAILABLE");
  const stream = new DecompressionStream("deflate"),
    reader = stream.readable.getReader(),
    writer = stream.writable.getWriter();
  let failed, disposal;
  const stop = (error) => {
    if (!failed) {
      failed = error;
      disposal = Promise.allSettled([reader.cancel(error), writer.abort(error)]);
    }
  };
  const check = () => {
    abort(signal);
    if (failed) throw failed;
  };
  const producer = (async () => {
    try {
      for await (const bytes of input) {
        check();
        await waiting(() => writer.write(bytes), signal);
      }
      await waiting(() => writer.close(), signal);
    } catch (error) {
      stop(error);
      throw error;
    }
  })();
  const consumer = (async () => {
    let written = 0;
    try {
      while (true) {
        check();
        const { done, value } = await waiting(() => reader.read(), signal);
        check();
        if (done) break;
        if (!(value instanceof Uint8Array) || value.length > expected - written)
          fail("DECODED_SIZE");
        for (let at = 0; at < value.length; at += ARCHIVE_STREAM_READ_BYTES) {
          check();
          await emit(value.slice(at, at + ARCHIVE_STREAM_READ_BYTES));
        }
        written += value.length;
      }
      if (written !== expected) fail("DECODED_SIZE");
    } catch (error) {
      stop(error);
      throw error;
    }
  })();
  try {
    await Promise.all([producer, consumer]);
  } finally {
    await Promise.allSettled([producer, consumer]);
    if (disposal) await disposal;
    reader.releaseLock();
    writer.releaseLock();
  }
}
/**
 * 来源必须是同一个不可变归档的 BigInt readAt 能力；路径到键的认证归调用方可信层。
 * 只返回完成 EKey、各块 MD5、大小及完整 CKey 验证后的暂存能力，不发布部分输出。
 * Source must be a BigInt readAt capability for one immutable archive; trusted callers authenticate path-to-key association.
 * Return staged access only after EKey, block MD5, sizes and whole-content CKey pass; never publish partial output.
 */
export async function stageArchiveRecord(
  source,
  record,
  { root, maxDecodedBytes, maxEncodedBytes, signal, createStage = createRawFileStage } = {},
) {
  if (signal !== undefined && !(signal instanceof AbortSignal)) fail("SIGNAL");
  abort(signal);
  if (
    !source ||
    typeof source.size !== "bigint" ||
    source.size < 0n ||
    typeof source.readAt !== "function" ||
    !integer(maxDecodedBytes) ||
    !integer(maxEncodedBytes) ||
    typeof createStage !== "function"
  )
    fail("OPTIONS");
  const { offset, encodedSize, decodedSize, encoding, ckey, ekey } = record ?? {};
  if (
    typeof offset !== "bigint" ||
    offset < 0n ||
    !integer(encodedSize, maxEncodedBytes) ||
    !integer(decodedSize, maxDecodedBytes) ||
    offset > source.size ||
    BigInt(encodedSize) > source.size - offset ||
    typeof ckey !== "string" ||
    !/^[a-f0-9]{32}$/i.test(ckey) ||
    typeof ekey !== "string" ||
    !/^[a-f0-9]{32}$/i.test(ekey) ||
    BigInt(`0x${ekey.slice(22)}`) !== offset
  )
    fail("RECORD");
  let acquiring, stage;
  let contentHash = await createMD5();
  const read = async (position, count) => {
    if (
      !integer(position, encodedSize) ||
      !integer(count, ARCHIVE_STREAM_READ_BYTES) ||
      count > encodedSize - position
    )
      fail("READ_BOUNDS");
    const bytes = await waiting(
      () => source.readAt(offset + BigInt(position), count, { signal }),
      signal,
    );
    abort(signal);
    if (
      !(bytes instanceof Uint8Array) ||
      !(bytes.buffer instanceof ArrayBuffer) ||
      bytes.buffer.resizable ||
      bytes.length !== count
    )
      fail("SHORT_READ");
    // 使用基类构造器复制，避免 Buffer 等子类的 slice 保留来源别名。
    // Copy through the base constructor; subclass slice methods such as Buffer.slice may alias.
    return new Uint8Array(bytes);
  };
  let total = 0;
  try {
    const prefix = await read(0, Math.min(12, encodedSize));
    const blte = prefix.length >= 4 && hex(prefix.subarray(0, 4)) === "424c5445";
    const blocks = [];
    if (blte) {
      if (prefix.length < 12) fail("BLTE_HEADER");
      const headerSize = u32(prefix, 4),
        count = prefix[9] * 65536 + prefix[10] * 256 + prefix[11];
      if (
        prefix[8] !== 15 ||
        !count ||
        count > ARCHIVE_LIMITS.blocks ||
        headerSize !== 12 + count * 24 ||
        headerSize > encodedSize
      )
        fail("BLTE_HEADER");
      const header = new Uint8Array(headerSize);
      for (let at = 0; at < headerSize; at += ARCHIVE_STREAM_READ_BYTES)
        header.set(await read(at, Math.min(ARCHIVE_STREAM_READ_BYTES, headerSize - at)), at);
      if ((await md5(header)).slice(0, 18) !== ekey.slice(0, 18).toLowerCase()) fail("EKEY");
      let position = headerSize,
        decodedTotal = 0;
      for (let i = 0; i < count; ++i) {
        const at = 12 + i * 24,
          encoded = u32(header, at),
          decoded = u32(header, at + 4);
        if (
          !encoded ||
          encoded > ARCHIVE_LIMITS.encodedBlock ||
          decoded > ARCHIVE_LIMITS.decodedBlock ||
          encoded > encodedSize - position ||
          decoded > decodedSize - decodedTotal
        )
          fail("BLOCK_BOUNDS");
        blocks.push({
          position,
          encoded,
          decoded,
          checksum: hex(header.subarray(at + 8, at + 24)),
        });
        position += encoded;
        decodedTotal += decoded;
      }
      if (position !== encodedSize || decodedTotal !== decodedSize) fail("TOTALS");
    } else {
      if (
        !["n", "z"].includes(encoding) ||
        encodedSize > ARCHIVE_LIMITS.encodedBlock ||
        decodedSize > ARCHIVE_LIMITS.decodedBlock ||
        (encoding === "n" && encodedSize !== decodedSize)
      )
        fail("RAW_BOUNDS");
      blocks.push({ position: 0, encoded: encodedSize, decoded: decodedSize });
    }
    acquiring = Promise.resolve().then(() => createStage({ root, maxBytes: decodedSize, signal }));
    stage = await waiting(() => acquiring, signal);
    if (
      !stage ||
      ["append", "finalize", "dispose"].some((name) => typeof stage[name] !== "function")
    )
      fail("STAGE");
    const emit = async (bytes) => {
      if (bytes.length > decodedSize - total || bytes.length > ARCHIVE_STREAM_READ_BYTES)
        fail("DECODED_SIZE");
      await waiting(() => stage.append(bytes), signal);
      abort(signal);
      contentHash.update(bytes);
      total += bytes.length;
    };
    for (const block of blocks) {
      // 小块最多缓存 1 MiB，验证后复用；较大块从同一不可变来源读取两遍。
      // Cache at most 1 MiB for verified small-block reuse; read larger blocks twice from the same immutable source.
      const cached =
        block.encoded <= ARCHIVE_STREAM_CACHE_BYTES ? new Uint8Array(block.encoded) : null;
      let verified = await createMD5();
      let verifiedDigest;
      try {
        for (let at = 0; at < block.encoded; at += ARCHIVE_STREAM_READ_BYTES) {
          const bytes = await read(
            block.position + at,
            Math.min(ARCHIVE_STREAM_READ_BYTES, block.encoded - at),
          );
          verified.update(bytes);
          cached?.set(bytes, at);
        }
        const digest = verified.digest();
        verifiedDigest = digest;
        if (
          blte ? digest !== block.checksum : digest.slice(0, 18) !== ekey.slice(0, 18).toLowerCase()
        )
          fail(blte ? "BLOCK_MD5" : "EKEY");
      } finally {
        verified = null;
      }
      const payloadRead = (at, count) =>
        cached
          ? Promise.resolve(cached.slice(at - block.position, at - block.position + count))
          : read(at, count);
      let encodedHash = await createMD5();
      try {
        let mode = encoding,
          payloadAt = block.position,
          payloadSize = block.encoded;
        if (blte) {
          const marker = await payloadRead(payloadAt, 1);
          encodedHash.update(marker);
          if (marker[0] !== 78 && marker[0] !== 90) fail("BLOCK_MODE");
          mode = marker[0] === 78 ? "n" : "z";
          payloadAt++;
          payloadSize--;
        }
        async function* input() {
          const slice = mode === "z" ? ARCHIVE_LIMITS.compressedSlice : ARCHIVE_STREAM_READ_BYTES;
          // 压缩块（包括缓存）按 64 KiB 独立快照读取，仍逐个馈送 1 KiB 并等待解压背压。
          // Read compressed blocks (including cached blocks) as independent 64 KiB snapshots, still feeding 1 KiB with decoder backpressure.
          const readSize = ARCHIVE_STREAM_READ_BYTES;
          for (let at = 0; at < payloadSize; at += readSize) {
            const bytes = await payloadRead(payloadAt + at, Math.min(readSize, payloadSize - at));
            for (let part = 0; part < bytes.length; part += slice) {
              abort(signal);
              const piece = bytes.subarray(part, part + slice);
              encodedHash.update(piece);
              yield piece;
            }
          }
        }
        if (mode === "n") {
          if (payloadSize !== block.decoded) fail("RAW_LENGTH");
          for await (const bytes of input()) await emit(bytes);
        } else await inflate(input(), block.decoded, emit, signal);
        const actual = encodedHash.digest();
        // 第二遍必须与已验证的完整摘要一致，即使非 BLTE 的 EKey 只保存摘要前缀。
        // Require the full verified digest on the second pass even though non-BLTE EKeys store only a prefix.
        if (actual !== verifiedDigest) fail("SOURCE_CHANGED");
        if (
          blte ? actual !== block.checksum : actual.slice(0, 18) !== ekey.slice(0, 18).toLowerCase()
        )
          fail(blte ? "BLOCK_MD5" : "EKEY");
      } finally {
        encodedHash = null;
      }
    }
    if (total !== decodedSize || contentHash.digest() !== ckey.toLowerCase()) fail("CKEY");
    const file = await waiting(() => stage.finalize(), signal);
    abort(signal);
    if (file?.size !== decodedSize || typeof file.readAt !== "function") fail("FINAL_SIZE");
    let disposed = false;
    return Object.freeze({
      size: decodedSize,
      async readAt(at, count, options = {}) {
        if (disposed) fail("DISPOSED");
        if (options.signal !== undefined && !(options.signal instanceof AbortSignal))
          fail("SIGNAL");
        const combined =
          signal && options.signal
            ? AbortSignal.any([signal, options.signal])
            : (signal ?? options.signal);
        abort(combined);
        if (!integer(at, decodedSize) || !integer(count, 1024 * 1024) || count > decodedSize - at)
          fail("READ_BOUNDS");
        const bytes = await waiting(() => {
          if (disposed) fail("DISPOSED");
          return file.readAt(at, count);
        }, combined);
        if (disposed) fail("DISPOSED");
        abort(combined);
        if (!(bytes instanceof Uint8Array) || bytes.length !== count) fail("SHORT_READ");
        return bytes;
      },
      dispose() {
        disposed = true;
        return stage.dispose();
      },
    });
  } catch (error) {
    const safe =
      signal?.aborted || error?.name === "AbortError"
        ? new DOMException("Cancelled", "AbortError")
        : new Error(
            /^ARCHIVE_STREAM_[A-Z_]+$/.test(error?.message ?? "")
              ? error.message
              : "ARCHIVE_STREAM_FAILED",
          );
    if (acquiring) return cleanup(acquiring, safe, error);
    throw safe;
  } finally {
    contentHash = null;
  }
}
