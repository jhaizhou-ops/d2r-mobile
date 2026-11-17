// 认证父级持有来源令牌、范围受限 RPC 与 OPFS 所有权；初始化前不接受私有输入。
// Authenticated parent owns source tokens, range-restricted RPC and OPFS ownership; no private inputs before initialization.
import {
  enumerateAuthenticatedAstcRecords,
  openAuthenticatedAstcRecord,
  readAuthenticatedAssets,
} from "./extraction-job-adapter.mjs";
import { readAuthenticatedCascVfs, reserveCascPackLocations } from "./casc-overlay-writer.mjs";
import { createArchiveReadRpcHost } from "./archive-read-rpc.mjs";
import { createAstcJobDirectory } from "./astc-job-directory.mjs";
const fail = (code) => {
  throw new Error(`ASTC_PRODUCER_${code}`);
};
const integer = (n, max) => Number.isSafeInteger(n) && n >= 0 && n <= max;
const diagnosticCodes = new Set([
  "STORAGE_QUOTA",
  "SIZE_LIMIT",
  "INTEGRITY_FAILED",
  "CODEC_FAILED",
]);
const abort = () => new DOMException("Cancelled", "AbortError");
// 有界文件分片等待可取消，底层 Blob I/O 不被误报为强制停止。
// Bounded File slice waits are abortable without claiming forced cancellation of underlying Blob I/O.
async function readFileSlice(file, at, count, signal) {
  let timer, cancel, pending;
  let delivered = false;
  try {
    signal?.throwIfAborted();
    pending = file.slice(at, at + count).arrayBuffer();
    const waits = [
      pending,
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error("ASTC_PRODUCER_READ_TIMEOUT")), 30000);
      }),
    ];
    if (signal)
      waits.push(
        new Promise((_, reject) => {
          cancel = () => reject(abort());
          signal.addEventListener("abort", cancel, { once: true });
          if (signal.aborted) cancel();
        }),
      );
    const buffer = await Promise.race(waits);
    delivered = true;
    return buffer;
  } catch (error) {
    if (error?.name === "AbortError") throw abort();
    fail("READ_FAILED");
  } finally {
    clearTimeout(timer);
    if (cancel) signal.removeEventListener("abort", cancel);
    // 失败的等待不转移所有权；擦除迟到的文件缓冲区并观察迟到的拒绝。
    // A failed wait transfers no ownership; wipe late File buffers and observe late rejections.
    if (!delivered && pending)
      Promise.resolve(pending)
        .then((buffer) => new Uint8Array(buffer).fill(0))
        .catch(() => {});
  }
}

const fields = (v, names) =>
  v &&
  typeof v === "object" &&
  !Array.isArray(v) &&
  Object.keys(v).sort().join() === [...names].sort().join();
/**
 * 范围包装不是认证；仅供固定父级把已认证记录绑定到 RPC。
 * Range wrapping is not authentication; the fixed parent binds already authenticated records to RPC.
 */
export function bindAstcRecordSource({ source, record }) {
  const offset = record?.offset,
    length = record?.encodedSize;
  if (
    !source ||
    typeof source.size !== "bigint" ||
    typeof source.readAt !== "function" ||
    typeof offset !== "bigint" ||
    offset < 0n ||
    !integer(length, 0xffffffff) ||
    offset + BigInt(length) > source.size
  )
    fail("RECORD");
  const end = offset + BigInt(length),
    read = source.readAt.bind(source),
    size = source.size;
  return Object.freeze({
    size,
    async readAt(at, count, { signal } = {}) {
      signal?.throwIfAborted();
      if (
        typeof at !== "bigint" ||
        at < offset ||
        at > end ||
        !integer(count, 65536) ||
        BigInt(count) > end - at
      )
        fail("READ_BOUNDS");
      const bytes = await read(at, count, { signal });
      signal?.throwIfAborted();
      return bytes;
    },
  });
}
/**
 * 只检查内部已初始化工作线程的收据结构；外部文件和哈希不能借此获得认证。
 * Check only an initialized internal worker receipt; external files and hashes gain no authentication here.
 */
export function validateAstcProducerReceipt(
  value,
  { paths, packName, maxPackBytes, preset = "thorough" },
) {
  if (
    !fields(value, ["type", "file", "pack", "records", "index", "count", "preset"]) ||
    value.type !== "complete" ||
    (preset !== "thorough" && preset !== "fast") ||
    value.preset !== preset ||
    !(value.file instanceof File) ||
    !fields(value.pack, ["name", "size", "sha256"]) ||
    value.pack.name !== packName ||
    !integer(value.pack.size, maxPackBytes) ||
    value.file.size !== value.pack.size ||
    !/^[a-f0-9]{64}$/.test(value.pack.sha256) ||
    !Array.isArray(value.records) ||
    !integer(value.count, 32768) ||
    value.count < 1 ||
    value.records.length !== value.count ||
    typeof value.index !== "string" ||
    value.index.length > 16 * 1024 * 1024
  )
    fail("RECEIPT");
  const seen = new Set(),
    records = [],
    lines = [`# pack\t${packName}\n`];
  let end = 0,
    metadataBytes = 0;
  const [part, archive] = packName.slice(0, -5).split("-").map(Number);
  const locator = part.toString(16).padStart(2, "0") + archive.toString(16).padStart(2, "0");
  for (const row of value.records) {
    if (
      !fields(row, ["path", "newEkey", "encodedSize", "decodedSize"]) ||
      typeof row.path !== "string" ||
      !paths.has(row.path) ||
      seen.has(row.path) ||
      !/^[a-f0-9]{32}$/.test(row.newEkey) ||
      row.newEkey.slice(18, 22) !== locator ||
      BigInt(`0x${row.newEkey.slice(22)}`) !== BigInt(end) ||
      !integer(row.encodedSize, 0xffffffff) ||
      !integer(row.decodedSize, 0xffffffff) ||
      row.encodedSize > value.pack.size - end
    )
      fail("RECEIPT");
    metadataBytes += new TextEncoder().encode(row.path).length;
    if (metadataBytes > 8 * 1024 * 1024) fail("RECEIPT");
    seen.add(row.path);
    end += row.encodedSize;
    records.push(Object.freeze({ ...row }));
    lines.push(`${row.path}\t${row.newEkey}\t${row.encodedSize}\t${row.decodedSize}\n`);
  }
  if (
    end !== value.pack.size ||
    records.at(-1).path !== "data/hd/texture_desc_cache.json" ||
    lines.join("") !== value.index
  )
    fail("RECEIPT");
  return Object.freeze({
    encoding: Object.freeze({ format: "astc", preset }),
    file: value.file,
    pack: Object.freeze({ ...value.pack }),
    records: Object.freeze(records),
    index: value.index,
    count: value.count,
  });
}
/**
 * 仅密封加载器调用：worker 与四个公开资产必须已验证，实际编码器初始化后才返回。
 * Sealed-loader-only: worker and four public assets must be verified; return only after actual codec initialization.
 */
export async function createAstcProducerClient({ worker, assets, signal } = {}) {
  const names = ["astc-encoder.mjs", "astc-encoder.wasm", "png-codec.wasm", "sha256.wasm"];
  if (
    !worker ||
    ["postMessage", "terminate", "addEventListener", "removeEventListener"].some(
      (k) => typeof worker[k] !== "function",
    ) ||
    (signal !== undefined && !(signal instanceof AbortSignal)) ||
    !Array.isArray(assets) ||
    assets.length !== 4
  )
    fail("INITIALIZE");
  const map = new Map(assets);
  if (
    map.size !== 4 ||
    names.some(
      (name) =>
        !(map.get(name) instanceof ArrayBuffer) ||
        map.get(name).resizable ||
        !map.get(name).byteLength ||
        map.get(name).byteLength > 32 * 1024 * 1024,
    )
  )
    fail("INITIALIZE");
  let stopped = false,
    used = false,
    lease,
    rpc,
    waiting,
    outputDisposed = false;
  const jobController = new AbortController();
  function stop(cause = new Error("ASTC_PRODUCER_CLOSED")) {
    if (stopped) return;
    stopped = true;
    jobController.abort();
    rpc?.close();
    worker.removeEventListener("message", message);
    worker.removeEventListener("error", bad);
    worker.removeEventListener("messageerror", bad);
    signal?.removeEventListener("abort", cancel);
    worker.terminate();
    if (waiting) {
      const pending = waiting;
      waiting = undefined;
      pending.release();
      pending.reject(cause);
    }
  }
  async function dispose() {
    outputDisposed = true;
    stop();
    if (lease) await lease.dispose();
  }
  function message({ data }) {
    if (stopped) return;
    if (!waiting) {
      stop(new Error("ASTC_PRODUCER_PROTOCOL"));
      return;
    }
    if (data?.type === "progress") {
      const p = waiting;
      if (
        p.type !== "complete" ||
        !fields(data, ["type", "sequence", "bytes", "blocks"]) ||
        data.sequence !== p.sequence + 1 ||
        !integer(data.bytes, p.maxBytes) ||
        !integer(data.blocks, p.maxBlocks) ||
        data.bytes < p.bytes ||
        data.blocks < p.blocks ||
        (data.bytes === p.bytes && data.blocks === p.blocks)
      ) {
        stop(new Error("ASTC_PRODUCER_PROTOCOL"));
        return;
      }
      p.sequence = data.sequence;
      p.bytes = data.bytes;
      p.blocks = data.blocks;
      p.renew();
      p.onProgress?.(data);
      return;
    }
    const pending = waiting;
    waiting = undefined;
    pending.release();
    if (data?.type === "failure") {
      const valid = fields(data, ["type", "code"]) && diagnosticCodes.has(data.code);
      const error = new Error(valid ? "ASTC_PRODUCER_FAILED" : "ASTC_PRODUCER_PROTOCOL");
      if (valid) error.detailCode = data.code;
      pending.reject(error);
      if (!valid) stop();
    } else if (data?.type !== pending.type) {
      pending.reject(new Error("ASTC_PRODUCER_PROTOCOL"));
      stop();
    } else pending.resolve(data);
  }
  function bad() {
    stop(new Error("ASTC_PRODUCER_WORKER_FAILED"));
  }
  function cancel() {
    stop(abort());
    void dispose().catch(() => {});
  }
  function request(data, transfers, type, timeoutMs, limits = {}) {
    if (stopped || waiting) return Promise.reject(new Error("ASTC_PRODUCER_CLOSED"));
    return new Promise((resolve, reject) => {
      let timer;
      const renew = () => {
        clearTimeout(timer);
        timer = setTimeout(() => stop(new Error("ASTC_PRODUCER_TIMEOUT")), timeoutMs);
      };
      const overall =
        limits.overallTimeoutMs === undefined
          ? undefined
          : setTimeout(() => stop(new Error("ASTC_PRODUCER_TIMEOUT")), limits.overallTimeoutMs);
      waiting = {
        resolve,
        reject,
        type,
        sequence: 0,
        bytes: 0,
        blocks: 0,
        maxBytes: limits.maxBytes,
        maxBlocks: limits.maxBlocks,
        onProgress: limits.onProgress,
        renew,
        release: () => {
          clearTimeout(timer);
          clearTimeout(overall);
        },
      };
      renew();
      try {
        worker.postMessage(data, transfers);
      } catch {
        stop(new Error("ASTC_PRODUCER_TRANSPORT"));
      }
    });
  }
  worker.addEventListener("message", message);
  worker.addEventListener("error", bad);
  worker.addEventListener("messageerror", bad);
  signal?.addEventListener("abort", cancel, { once: true });
  try {
    if (signal?.aborted) throw abort();
    const buffers = names.map((name) => map.get(name));
    const proof = await request(
      { type: "initialize", assets: names.map((name) => [name, map.get(name)]) },
      buffers,
      "initialized",
      60000,
    );
    if (
      !fields(proof, ["type", "version", "astc", "png", "sha"]) ||
      proof.version !== 1 ||
      proof.astc !== true ||
      proof.png !== true ||
      proof.sha !== true ||
      buffers.some((b) => b.byteLength !== 0)
    )
      fail("INITIALIZE");
    map.clear();
  } catch (error) {
    stop();
    throw error;
  }
  return Object.freeze({
    initialized: true,
    get ready() {
      return !stopped && !used;
    },
    close: dispose,
    async produce(extracted, selection, options = {}) {
      if (stopped || used) fail("SINGLE_USE");
      used = true;
      const {
        root,
        maxEncodedBytes,
        maxDecodedBytes,
        maxPackBytes,
        batchBlocks = 65536,
        preset = "thorough",
        signal: operationSignal,
        inactivityTimeoutMs = 300000,
        overallTimeoutMs,
        onProgress,
      } = options;
      if (
        (preset !== "thorough" && preset !== "fast") ||
        (onProgress !== undefined && typeof onProgress !== "function") ||
        !root ||
        !integer(maxEncodedBytes, 0xffffffff) ||
        !maxEncodedBytes ||
        !integer(maxDecodedBytes, 0xffffffff) ||
        !maxDecodedBytes ||
        !integer(maxPackBytes, 2 ** 40) ||
        !maxPackBytes ||
        !integer(batchBlocks, 65536) ||
        !batchBlocks ||
        !integer(inactivityTimeoutMs, 7200000) ||
        inactivityTimeoutMs < 1 ||
        (overallTimeoutMs !== undefined &&
          (!integer(overallTimeoutMs, Number.MAX_SAFE_INTEGER) ||
            overallTimeoutMs < 1 ||
            overallTimeoutMs > 2147483647)) ||
        (operationSignal !== undefined && !(operationSignal instanceof AbortSignal))
      ) {
        stop();
        fail("OPTIONS");
      }
      const combined = operationSignal
        ? AbortSignal.any([jobController.signal, operationSignal])
        : jobController.signal;
      const onAbort = () => stop(abort());
      operationSignal?.addEventListener("abort", onAbort, { once: true });
      let vfs,
        lastProgress = -Infinity,
        progressCounts = { bytes: 0, blocks: 0 };
      const report = (phase, counts = {}, force = false) => {
        const now = performance.now();
        if (!onProgress || (!force && now - lastProgress < 100)) return;
        lastProgress = now;
        const value = Object.freeze({
          phase,
          processedBytes: counts.bytes ?? 0,
          completedBlocks: counts.blocks ?? 0,
          ...(counts.records === undefined ? {} : { completedRecords: counts.records }),
        });
        try {
          Promise.resolve(onProgress(value)).catch(() => {});
        } catch {
          /* 观察者不影响所有权。 | Observers do not affect ownership. */
        }
      };
      try {
        report("preparing", {}, true);
        combined.throwIfAborted();
        const selected = await enumerateAuthenticatedAstcRecords(extracted, selection, {
          signal: combined,
        });
        if (!selected.descriptor) fail("DESCRIPTOR_MISSING");
        const authenticated = readAuthenticatedAssets(extracted, selection, { signal: combined });
        const reservation = reserveCascPackLocations(authenticated.romfs);
        vfs = (await readAuthenticatedCascVfs(extracted, selection, { signal: combined })).slice();
        if (vfs.byteLength > 16 * 1024 * 1024) fail("VFS");
        lease = await createAstcJobDirectory({
          root,
          signal: combined,
          stopAndDrain: () => stop(),
        });
        combined.throwIfAborted();
        const ports = new MessageChannel();
        rpc = createArchiveReadRpcHost(ports.port1, { signal: combined });
        const tokens = [...selected.textures, selected.descriptor],
          paths = new Set(tokens.map((token) => token.path));
        const records = tokens.map((token) => {
          const opened = openAuthenticatedAstcRecord(token, { signal: combined });
          return {
            path: token.path,
            record: { ...opened.record },
            source: rpc.register(bindAstcRecordSource(opened)),
          };
        });
        const data = await request(
          {
            type: "produce",
            records,
            vfs: vfs.buffer,
            root: lease.root,
            port: ports.port2,
            location: reservation.packLocation,
            packName: reservation.packName,
            maxEncodedBytes,
            maxDecodedBytes,
            maxPackBytes,
            batchBlocks,
            preset,
          },
          [vfs.buffer, ports.port2],
          "complete",
          inactivityTimeoutMs,
          {
            overallTimeoutMs,
            onProgress: (data) => {
              progressCounts = data;
              report("converting", data);
            },
            maxBytes:
              records.reduce((n, r) => n + r.record.decodedSize * 2, 0) +
              maxPackBytes * 3 +
              16 * 1024 * 1024,
            maxBlocks:
              records.reduce((n, r) => n + Math.ceil(r.record.decodedSize / 16), 0) + 32768 * 65536,
          },
        );
        combined.throwIfAborted();
        const receipt = validateAstcProducerReceipt(data, {
          paths,
          preset,
          packName: reservation.packName,
          maxPackBytes,
        });
        // 成功后先终止计算与来源读取，但不能取消仍由导出方持有的目录租约。
        // Stop computation/source reads after success without cancelling the directory lease retained for export.
        rpc.close();
        worker.removeEventListener("message", message);
        worker.removeEventListener("error", bad);
        worker.removeEventListener("messageerror", bad);
        signal?.removeEventListener("abort", cancel);
        worker.terminate();
        stopped = true;
        const source = Object.freeze({
          file: receipt.file,
          size: receipt.file.size,
          async readAt(at, count, { signal: readSignal } = {}) {
            if (outputDisposed) fail("DISPOSED");
            if (readSignal !== undefined && !(readSignal instanceof AbortSignal)) fail("OPTIONS");
            readSignal?.throwIfAborted();
            if (
              !integer(at, receipt.file.size) ||
              !integer(count, 65536) ||
              count > receipt.file.size - at
            )
              fail("READ_BOUNDS");
            const bytes = new Uint8Array(await readFileSlice(receipt.file, at, count, readSignal));
            try {
              readSignal?.throwIfAborted();
              if (outputDisposed) fail("DISPOSED");
              if (bytes.length !== count) fail("SHORT_READ");
              return bytes;
            } catch (error) {
              bytes.fill(0);
              throw error;
            }
          },
        });
        report("complete", { ...progressCounts, records: receipt.count }, true);
        return Object.freeze({
          source,
          pack: receipt.pack,
          encoding: receipt.encoding,
          records: receipt.records,
          index: receipt.index,
          count: receipt.count,
          dispose,
        });
      } catch (cause) {
        stop();
        if (vfs?.buffer.byteLength) vfs.fill(0);
        if (cause?.lease && !lease) lease = cause.lease;
        const safe = cause?.name === "AbortError" ? abort() : new Error("ASTC_PRODUCER_FAILED");
        if (diagnosticCodes.has(cause?.detailCode)) safe.detailCode = cause.detailCode;
        try {
          await dispose();
        } catch (cleanup) {
          const error = new Error(
            lease?.status.cleanupPending
              ? "ASTC_PRODUCER_CLEANUP_PENDING"
              : "ASTC_PRODUCER_CLEANUP_FAILED",
          );
          error.cause = safe;
          if (safe.detailCode) error.detailCode = safe.detailCode;
          error.lease = lease;
          error.cleanupCause = cleanup;
          throw error;
        }
        throw safe;
      } finally {
        operationSignal?.removeEventListener("abort", onAbort);
      }
    },
  });
}
