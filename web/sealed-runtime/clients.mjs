// 依据已验证公开清单、完整性摘要及各工作线程协议管理密封运行时。
// Manage sealed runtimes using verified public inventories, integrity digests and worker protocols.
import { resolveShellProfile, SHELL_PROFILE_IDS } from "../shell-profiles.mjs";
import { nativeArtAtlasDescriptor } from "../native-art-texture.mjs";
import { validateEncodeRequest } from "../toolchain/texture-spike/browser-texture-encoder.mjs";
import {
  ASSET_RUNTIME_DIRECTORY,
  validateAssetRuntimeInventory,
} from "../toolchain/asset-runtime-js/payload-policy.mjs";
import { OPERATION_LIMITS, validateRequest } from "../toolchain/asset-runtime-js/contract.mjs";
import {
  selectRuntimeAssetGroups,
  validateRuntimeGroupSelection,
  validateRuntimeTransportGroups,
} from "../runtime-asset-groups.mjs";
import { createAstcProducerClient as initializeAstcProducerClient } from "../astc-producer-client.mjs";
import { prepareVerifiedIpaSigningFactory } from "./ipa-signing-factory.mjs";
import { createAirCompilerWorkerPool } from "../air-compiler-worker-client.mjs";
import { shaderFailureDiagnostic } from "../shader-diagnostics.mjs";
import {
  validateAirCompilerRuntimeComponent,
  assertAirCompilerReceiptMatchesComponent,
  runtimeAssetMaximumBytes,
  runtimeTransportMaximumBytes,
} from "../air-compiler-runtime-contract.mjs";
const OFFLINE = "toolchain/offline-runtime-spike";
const TEXTURE = "toolchain/texture-spike";
const LIMIT = Object.freeze({
  file: 64 * 1024 * 1024,
  total: 512 * 1024 * 1024,
  manifest: 2 * 1024 * 1024,
  files: 4096,
});
let attempted = false;
const sealedRuntimes = new WeakSet();
/**
 * 仅接受本模块在已验证预加载及初始化后创建的仍有效会话。
 * Accept only live sessions created by this module after verified preload and initialization.
 */
export function assertSealedRuntime(value) {
  if (!sealedRuntimes.has(value)) fail("A live verified sealed runtime is required");
  return value;
}
const hash = async (bytes) =>
  Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)), (b) =>
    b.toString(16).padStart(2, "0"),
  ).join("");
function fail(message) {
  throw new Error(message);
}
function safePath(path) {
  return (
    typeof path === "string" &&
    path.length <= 512 &&
    /^[A-Za-z0-9_.+/-]+$/.test(path) &&
    path.split("/").every((p) => p && p !== "." && p !== "..")
  );
}
/**
 * 纯清单验证；调用方另行验证受信任的顶层摘要。
 * Pure inventory validation; the caller separately verifies the trusted top digest.
 */
export function validateRuntimeInventory(manifest) {
  return validateRuntimeRecords(manifest, (asset) => runtimeAssetMaximumBytes(asset.path));
}
function validateRuntimeRecords(manifest, maximumBytes) {
  if (
    manifest?.version !== 1 ||
    !Array.isArray(manifest.assets) ||
    !manifest.assets.length ||
    manifest.assets.length > LIMIT.files
  )
    fail("Invalid runtime manifest");
  const names = new Set();
  let total = 0;
  for (const asset of manifest.assets) {
    if (
      !safePath(asset.path) ||
      names.has(asset.path.toLowerCase()) ||
      !Number.isSafeInteger(asset.bytes) ||
      asset.bytes < 1 ||
      asset.bytes > maximumBytes(asset) ||
      !/^[a-f0-9]{64}$/.test(asset.sha256)
    )
      fail("Invalid runtime inventory");
    names.add(asset.path.toLowerCase());
    total += asset.bytes;
  }
  if (total > LIMIT.total || manifest.totalBytes !== total) fail("Runtime byte budget mismatch");
  return manifest.assets;
}
/**
 * 传输可选：旧清单继续通过逻辑路径获取内容。
 * Transport is optional: old manifests continue to fetch their logical paths.
 */
function runtimeTransport(manifest, inventory) {
  if (manifest.transport === undefined)
    return { files: inventory, totalBytes: manifest.totalBytes, entries: undefined };
  const transport = manifest.transport;
  if (
    transport?.version !== 1 ||
    !Array.isArray(transport.entries) ||
    !transport.entries.length ||
    transport.entries.length > inventory.length
  )
    fail("Invalid runtime transport");
  const files = validateRuntimeRecords(
    { version: 1, assets: transport.files, totalBytes: transport.totalBytes },
    (file) => runtimeTransportMaximumBytes(file, manifest),
  );
  let decodedTotal = 0;
  for (const file of files) {
    if (
      !["identity", "gzip"].includes(file.encoding) ||
      !Number.isSafeInteger(file.decodedBytes) ||
      file.decodedBytes < 1 ||
      file.decodedBytes > runtimeTransportMaximumBytes(file, manifest) ||
      (file.encoding === "identity" && file.decodedBytes !== file.bytes)
    )
      fail("Invalid runtime transport encoding");
    decodedTotal += file.decodedBytes;
  }
  if (decodedTotal > LIMIT.total) fail("Decoded runtime byte budget exceeded");
  const physical = new Map(files.map((file) => [file.path, file]));
  const expected = new Set(inventory.map((asset) => `${asset.sha256}-${asset.bytes}`));
  const entries = new Map();
  const ranges = new Map(files.map((file) => [file.path, []]));
  for (const entry of transport.entries) {
    const key = `${entry.sha256}-${entry.bytes}`;
    const file = physical.get(entry.file);
    if (
      !expected.has(key) ||
      entries.has(key) ||
      !file ||
      !Number.isSafeInteger(entry.offset) ||
      entry.offset < 0 ||
      !Number.isSafeInteger(entry.bytes) ||
      entry.bytes < 1 ||
      entry.offset + entry.bytes > file.decodedBytes
    )
      fail("Invalid runtime transport entry");
    entries.set(key, entry);
    ranges.get(entry.file).push(entry);
  }
  if (entries.size !== expected.size) fail("Incomplete runtime transport");
  for (const [path, segments] of ranges) {
    let end = 0;
    for (const entry of segments.sort((a, b) => a.offset - b.offset)) {
      if (entry.offset !== end) fail("Runtime transport overlap or gap");
      end += entry.bytes;
    }
    if (end !== physical.get(path).decodedBytes) fail("Unused runtime transport bytes");
  }
  return { files, totalBytes: transport.totalBytes, entries };
}
/**
 * 验证完整清单后，省略未预载的内置外壳及未选可选闭包；绝不省略通用资产。
 * Validate the complete manifest before omitting unpreloaded shells and optional closures, never common assets.
 */
export function validateRuntimeProfileSelection(
  manifest,
  profileId,
  { optionalGroups = [], preloadProfileIds = [] } = {},
) {
  const profile = resolveShellProfile(profileId);
  if (!Array.isArray(preloadProfileIds)) fail("Invalid runtime profile preload selection");
  const selectedShells = new Set(
    [profile.id, ...preloadProfileIds].map((id) => resolveShellProfile(id).archivePath),
  );
  const inventory = validateRuntimeInventory(manifest);
  validateAirCompilerRuntimeComponent(manifest);
  const transport = runtimeTransport(manifest, inventory);
  const groupInventory = selectRuntimeAssetGroups(inventory, optionalGroups);
  if (transport.entries) validateRuntimeTransportGroups(inventory, transport.entries.values());
  const shellPaths = new Set();
  for (const id of SHELL_PROFILE_IDS) {
    const known = resolveShellProfile(id);
    shellPaths.add(known.archivePath);
    const record = inventory.find((asset) => asset.path === known.archivePath);
    if (
      record &&
      (record.bytes !== known.evidence.archiveBytes || record.sha256 !== known.evidence.sha256)
    )
      fail("Runtime shell profile integrity mismatch");
  }
  for (const path of selectedShells)
    if (!inventory.some((asset) => asset.path === path))
      fail("Selected runtime shell is unavailable");
  const selectedInventory = groupInventory.filter(
    (asset) => !shellPaths.has(asset.path) || selectedShells.has(asset.path),
  );
  let downloadFiles;
  if (transport.entries) {
    const keys = new Set(selectedInventory.map((asset) => `${asset.sha256}-${asset.bytes}`));
    for (const asset of inventory.filter(
      (asset) => shellPaths.has(asset.path) && !selectedShells.has(asset.path),
    )) {
      const key = `${asset.sha256}-${asset.bytes}`;
      const entry = transport.entries.get(key);
      // 被省略的外壳必须独占载荷，不能与任何保留资产共享内容或物理文件。
      // An omitted shell must own its payload, without sharing content or a physical file with retained assets.
      if (
        keys.has(key) ||
        [...transport.entries.values()].filter((item) => item.file === entry.file).length !== 1
      )
        fail("Optional runtime shell payload is shared");
    }
    const requiredFiles = new Set([...keys].map((key) => transport.entries.get(key).file));
    downloadFiles = transport.files.filter((file) => requiredFiles.has(file.path));
  } else {
    downloadFiles = selectedInventory;
  }
  return {
    inventory,
    selectedInventory,
    transport,
    downloadFiles,
    downloadBytes: downloadFiles.reduce((total, file) => total + file.bytes, 0),
  };
}
// 私有输入打开之前，只有已验证的公开清单资源能进入此缓存。
// Only verified public inventory assets enter this cache, before private inputs are opened.
const PUBLIC_CACHE = "d2r-public-runtime-sha256-v1";
const DOWNLOAD_CONCURRENCY = 4;
async function responseBytes(response, cap, onChunk, exactSize = false, signal) {
  if (!response.ok || !response.body) {
    try {
      // 拒绝的响应不读取正文；取消失败或挂起都不能阻塞原始错误。
      // Never read a rejected response body; failed or stalled cancellation must not delay the original error.
      void Promise.resolve(response.body?.cancel()).catch(() => {});
    } catch {
      // 同步取消失败不替换缺失资源错误。 | Synchronous cancellation failure does not replace the missing-asset error.
    }
    fail("Missing public runtime asset");
  }
  const reader = response.body.getReader();
  let cancelled = false;
  const cancel = () => {
    if (cancelled) return;
    cancelled = true;
    try {
      // 清理失败或永不完成都不能掩盖原始错误或扣留输出缓冲区。
      // Failed or nonsettling cleanup must not mask the primary error or retain the output buffer.
      void Promise.resolve(reader.cancel()).catch(() => {});
    } catch {
      // 同步清理错误同样只是尽力而为。 | Synchronous cleanup failure is also best effort.
    }
  };
  try {
    signal?.addEventListener("abort", cancel, { once: true });
    signal?.throwIfAborted();
    // 已知大小的资源直接写入最终缓冲区，避免同时保留完整分块和合并副本。
    // Write known-size assets into the final buffer instead of retaining chunks plus a full copy.
    const output = exactSize ? new Uint8Array(cap) : undefined;
    const chunks = [];
    let size = 0;
    for (;;) {
      const { done, value } = await reader.read();
      signal?.throwIfAborted();
      if (done) break;
      size += value.length;
      if (size > cap) fail("Public runtime byte cap");
      if (output) output.set(value, size - value.length);
      else chunks.push(value);
      onChunk?.(value.length);
    }
    if (output) return size === cap ? output : output.subarray(0, size);
    const bytes = new Uint8Array(size);
    let offset = 0;
    for (const chunk of chunks) {
      bytes.set(chunk, offset);
      offset += chunk.length;
    }
    return bytes;
  } catch (error) {
    cancel();
    throw error;
  } finally {
    signal?.removeEventListener("abort", cancel);
    try {
      reader.releaseLock();
    } catch {
      // 锁释放也不得替换读取或容量错误。 | Lock cleanup must not replace read or byte-cap errors.
    }
  }
}
async function read(url, cap, signal, onChunk, exactSize = false) {
  const controller = new AbortController();
  const abort = () => controller.abort(signal?.reason);
  signal?.addEventListener("abort", abort, { once: true });
  if (signal?.aborted) abort();
  const timeout = setTimeout(
    () => controller.abort(new Error("Application download timed out. Reload to retry.")),
    90000,
  );
  try {
    const response = await fetch(url, {
      // 为所有者私有 Sites 保留托管会话；URL 均同源。
      // Preserve the hosting session for owner-private Sites; URLs are same-origin.
      credentials: "same-origin",
      redirect: "error",
      cache: "no-store",
      signal: controller.signal,
    });
    return await responseBytes(response, cap, onChunk, exactSize, controller.signal);
  } finally {
    clearTimeout(timeout);
    signal?.removeEventListener("abort", abort);
  }
}
// 浏览器存储失败不得阻碍不使用缓存的公开下载路径。
// A browser storage failure must not prevent the uncached public-download path.
async function optionalStorage(operation) {
  let timer;
  try {
    return await Promise.race([
      operation(),
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error("Public cache unavailable")), 3000);
      }),
    ]);
  } catch {
    return undefined;
  } finally {
    clearTimeout(timer);
  }
}
function progressReporter(onProgress) {
  const state = {
    phase: "manifest",
    loadedBytes: 0,
    totalBytes: 0,
    downloadedBytes: 0,
    cachedBytes: 0,
    completedFiles: 0,
    totalFiles: 0,
    label: "Reading application file list…",
  };
  let last = 0;
  return (change, force = false) => {
    Object.assign(state, change);
    const now = performance.now();
    if (force || now - last >= 100) {
      last = now;
      try {
        // 启动进度仅供观察；同步异常及异步拒绝不能改变运行时生命周期。
        // Startup progress is advisory; synchronous throws and async rejection cannot change runtime lifetime.
        Promise.resolve(onProgress?.({ ...state })).catch(() => {});
      } catch {
        // 包括调用方 thenable 属性访问异常，保留原始进度和初始化结果。
        // Preserve progress and initialization results even when caller thenable access throws.
      }
    }
  };
}
function jsonOptions(value, cap = 65536) {
  const text = JSON.stringify(value);
  if (typeof text !== "string" || new TextEncoder().encode(text).length > cap)
    fail("Runtime metadata cap");
  return JSON.parse(text);
}
function input(bytes, cap) {
  if (
    !(bytes instanceof Uint8Array) ||
    !(bytes.buffer instanceof ArrayBuffer) ||
    bytes.length > cap
  )
    fail("Runtime input cap");
  return bytes.slice();
}
function channel(worker, onFatal, onStage, shaderSubstep) {
  let next = 1,
    closed = false,
    taken = false,
    entry;
  const stop = (reason = new Error("Sealed runtime closed")) => {
    if (closed) return;
    closed = true;
    // 原生终止不发出事件。先通知已获取的提取
    // 传输，使其拒绝队列请求并清除请求截止计时器。
    // Native termination emits no event. Notify the acquired extraction
    // transport first so it can reject its queue and clear request deadlines.
    if (taken) worker.dispatchEvent(new Event("error", { cancelable: true }));
    worker.terminate();
    if (entry) {
      clearTimeout(entry.timer);
      entry.signal?.removeEventListener("abort", entry.abort);
      entry.reject(reason);
      entry = undefined;
    }
  };
  worker.onmessage = ({ data }) => {
    if (closed) return;
    if (entry?.id === 0 && data?.type === "initialization-stage") {
      onStage?.(data.stage);
      return;
    }
    if (!entry || data?.id !== entry.id) return;
    // 必须是明确的协议布尔值；程序工作线程仍可返回顶层结果字段。
    // Require an explicit protocol boolean; program workers may still return top-level result fields.
    if (typeof data.ok !== "boolean") {
      stop(new Error("Runtime worker protocol failed"));
      onFatal();
      return;
    }
    const p = entry;
    entry = undefined;
    clearTimeout(p.timer);
    p.signal?.removeEventListener("abort", p.abort);
    if (data.ok) {
      p.resolve(data.result === undefined ? data : data.result);
    } else {
      const error = new Error(
        p.id === 0
          ? String(data.error ?? "Runtime initialization failed").slice(0, 1200)
          : "Runtime operation failed",
      );
      // 只为两个着色器组件传递白名单诊断；其他组件仍使用原有密封错误。
      // Carry allowlisted diagnostics only for the two shader components; other components retain sealed errors.
      if (shaderSubstep)
        error.shaderDiagnostic = shaderFailureDiagnostic(
          { message: data.error, shaderDiagnostic: data.shaderDiagnostic },
          { substep: p.id === 0 ? "pipeline-initialization" : shaderSubstep },
        );
      p.reject(error);
    }
  };
  worker.onerror = (event) => {
    if (closed) return;
    stop(
      new Error(
        entry?.id === 0
          ? String(event.message || "Worker initialization failed").slice(0, 1200)
          : "Runtime worker failed",
      ),
    );
    onFatal();
  };
  worker.onmessageerror = () => {
    if (closed) return;
    stop();
    onFatal();
  };
  return {
    stop,
    takeWorker() {
      if (closed || entry) fail("Runtime unavailable");
      taken = true;
      worker.onmessage = null;
      return worker;
    },
    get busy() {
      return Boolean(entry);
    },
    request(message, transfers = [], signal, initialize = false) {
      if (closed || entry || signal?.aborted)
        return Promise.reject(new Error("Runtime unavailable, busy or cancelled"));
      const id = initialize ? 0 : next++;
      return new Promise((resolve, reject) => {
        const abort = () => {
          stop(new DOMException("Runtime cancelled", "AbortError"));
          onFatal();
        };
        const timer = setTimeout(
          () => {
            stop(new Error("Runtime deadline exceeded"));
            onFatal();
          },
          initialize ? 60000 : 120000,
        );
        entry = { id, resolve, reject, timer, signal, abort };
        signal?.addEventListener("abort", abort, { once: true });
        try {
          worker.postMessage({ ...message, id }, transfers);
        } catch (error) {
          stop(error);
          onFatal();
        }
      });
    },
  };
}
/**
 * 仅用于受信任的应用引导。在请求私有文件或密钥之前调用。manifestSha256 必须来自构建生成的 runtime-integrity.mjs，绝不能来自用户输入。所有请求的公开字节均先通过验证；核心运行时立即初始化，可选运行时可延迟到使用时从内存初始化。
 * Trusted application bootstrap only. Call before asking for private files/keys.
 * manifestSha256 MUST come from the built runtime-integrity.mjs, never user input.
 * Verifies all requested public bytes and initializes core runtimes before resolving.
 * Optional runtimes may defer initialization until use, exclusively from retained verified bytes.
 */
export async function prepareSealedRuntime({
  baseUrl,
  manifestSha256,
  profileId,
  components = ["assets", "texture", "translator", "extraction", "program", "spirvCross"],
  optionalGroups = [],
  preloadProfileIds = [],
  deferOptionalInitialization = false,
  signal,
  onProgress,
} = {}) {
  const selectedProfile = resolveShellProfile(profileId);
  const selectedGroups = validateRuntimeGroupSelection(optionalGroups);
  if (attempted) fail("Runtime bootstrap is one-shot per page");
  attempted = true;
  if (
    !isSecureContext ||
    !crypto?.subtle ||
    typeof Worker !== "function" ||
    typeof WebAssembly !== "object" ||
    navigator.serviceWorker?.controller
  )
    fail("Required clean browser capabilities unavailable");
  if (
    typeof deferOptionalInitialization !== "boolean" ||
    !/^[a-f0-9]{64}$/.test(manifestSha256) ||
    !Array.isArray(components) ||
    !components.length ||
    new Set(components).size !== components.length ||
    components.some(
      (x) =>
        ![
          "assets",
          "texture",
          "translator",
          "extraction",
          "program",
          "spirvCross",
          "airCompiler",
        ].includes(x),
    )
  )
    fail("Invalid trusted runtime configuration");
  const base = new URL(baseUrl, location.href);
  if (
    base.origin !== location.origin ||
    !["http:", "https:"].includes(base.protocol) ||
    base.username ||
    base.password ||
    base.search ||
    base.hash ||
    !base.pathname.endsWith("/")
  )
    fail("Runtime base must be same-origin directory");
  const report = progressReporter(onProgress);
  report({}, true);
  const publicBytes = await read(new URL("runtime-manifest.json", base), LIMIT.manifest, signal);
  if ((await hash(publicBytes)) !== manifestSha256) fail("Runtime manifest integrity mismatch");
  const manifest = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(publicBytes));
  const airCompilerComponent = validateAirCompilerRuntimeComponent(manifest, {
    required: components.includes("airCompiler"),
  });
  const { inventory, selectedInventory, transport, downloadFiles, downloadBytes } =
    validateRuntimeProfileSelection(manifest, selectedProfile.id, {
      optionalGroups: selectedGroups,
      preloadProfileIds,
    });
  const assets = new Map();
  const downloadedAssets = new Map();
  let cache = await optionalStorage(() => caches.open(PUBLIC_CACHE));
  const cacheKey = (record) =>
    new URL(`__public-runtime-cache__/${record.sha256}-${record.bytes}`, base).href;
  let downloadedBytes = 0,
    cachedBytes = 0,
    completedFiles = 0,
    cursor = 0;
  const update = (force = false) =>
    report(
      {
        phase: "download",
        loadedBytes: downloadedBytes + cachedBytes,
        downloadedBytes,
        cachedBytes,
        completedFiles,
        totalFiles: downloadFiles.length,
        totalBytes: downloadBytes,
        label: "Loading application files…",
      },
      force,
    );
  update(true);
  const downloads = new AbortController();
  const abortDownloads = () => downloads.abort(signal?.reason);
  signal?.addEventListener("abort", abortDownloads, { once: true });
  if (signal?.aborted) abortDownloads();
  const deadline = setTimeout(
    () => downloads.abort(new Error("Application loading timed out. Reload to retry.")),
    300000,
  );
  function checkDownloadCancellation() {
    if (downloads.signal.aborted) throw downloads.signal.reason;
  }
  async function download() {
    while (cursor < downloadFiles.length) {
      checkDownloadCancellation();
      const record = downloadFiles[cursor++];
      const key = cacheKey(record);
      let bytes;
      if (cache) {
        const publicCache = cache;
        const cacheRead = new AbortController();
        const cancelCacheRead = () => cacheRead.abort(downloads.signal.reason);
        downloads.signal.addEventListener("abort", cancelCacheRead, { once: true });
        if (downloads.signal.aborted) cancelCacheRead();
        const checkCacheRead = () => {
          checkDownloadCancellation();
          cacheRead.signal.throwIfAborted();
        };
        let cached;
        try {
          cached = await optionalStorage(async () => {
            const response = await publicCache.match(key);
            if (!response) {
              checkCacheRead();
              return { value: undefined };
            }
            try {
              const value = await responseBytes(
                response,
                record.bytes,
                undefined,
                true,
                cacheRead.signal,
              );
              checkCacheRead();
              const digest = await hash(value);
              checkCacheRead();
              if (value.length === record.bytes && digest === record.sha256) return { value };
            } catch {
              checkCacheRead();
              // 截断或超大的缓存记录绝不会被信任或保留。
              // A truncated or oversized cache record is never trusted or retained.
            }
            await publicCache.delete(key);
            return { value: undefined };
          });
        } finally {
          downloads.signal.removeEventListener("abort", cancelCacheRead);
          // 超时后取消读取，并拒绝晚到的响应或哈希；不回滚已经开始的缓存写入。
          // Cancel abandoned reads and reject late responses/hashes; never roll back already-started cache writes.
          cacheRead.abort();
        }
        checkDownloadCancellation();
        if (!cached) cache = undefined;
        if (cached?.value) {
          bytes = cached.value;
          cachedBytes += bytes.length;
          update();
        }
      }
      if (!bytes) {
        bytes = await read(
          new URL(record.path, base),
          record.bytes,
          downloads.signal,
          (size) => {
            checkDownloadCancellation();
            downloadedBytes += size;
            update();
          },
          true,
        );
        checkDownloadCancellation();
        if (bytes.length !== record.bytes || (await hash(bytes)) !== record.sha256)
          fail("Runtime asset integrity mismatch. Reload to retry.");
        checkDownloadCancellation();
        if (cache) {
          const publicCache = cache;
          // 不在取消后启动写入；已开始的 Cache.put 无法回滚，仍只包含验证过的公开字节。
          // Do not start writes after cancellation; an already-started Cache.put may finish with verified public bytes.
          const stored = await optionalStorage(async () => {
            await publicCache.put(key, new Response(bytes));
            return true;
          });
          if (!stored) cache = undefined;
        }
      }
      checkDownloadCancellation();
      downloadedAssets.set(record.path, bytes);
      completedFiles++;
      update();
    }
  }
  try {
    await Promise.all(
      Array.from({ length: Math.min(DOWNLOAD_CONCURRENCY, downloadFiles.length) }, download),
    );
    if (downloads.signal.aborted) throw downloads.signal.reason;
    if (cache) {
      // 仅逐出我们公开缓存中的过期条目，绝不处理任何私有浏览器存储。
      // Evict only obsolete entries from our public cache, never any private browser storage.
      const activeKeys = new Set(transport.files.map(cacheKey));
      await optionalStorage(async () => {
        for (const request of await cache.keys())
          if (!activeKeys.has(request.url)) await cache.delete(request);
      });
    }
    checkDownloadCancellation();
  } catch (error) {
    downloads.abort(error);
    throw error;
  } finally {
    clearTimeout(deadline);
    signal?.removeEventListener("abort", abortDownloads);
  }
  update(true);
  report({ phase: "initializing", label: "Preparing application…" }, true);
  if (transport.entries) {
    for (const file of downloadFiles) {
      if (signal?.aborted) throw new DOMException("Runtime cancelled", "AbortError");
      if (file.encoding === "gzip") {
        if (typeof DecompressionStream !== "function")
          fail(
            "This browser cannot decompress application files. Use current desktop Chrome or Edge.",
          );
        const stream = new Blob([downloadedAssets.get(file.path)])
          .stream()
          .pipeThrough(new DecompressionStream("gzip"));
        const decoded = await responseBytes(
          new Response(stream),
          file.decodedBytes,
          undefined,
          true,
          signal,
        );
        signal?.throwIfAborted();
        if (decoded.length !== file.decodedBytes) fail("Decoded runtime size mismatch");
        downloadedAssets.set(file.path, decoded);
      }
    }
  }
  const verifiedContents = new Map();
  for (const record of selectedInventory) {
    if (signal?.aborted) throw new DOMException("Runtime cancelled", "AbortError");
    if (!transport.entries) {
      assets.set(record.path, downloadedAssets.get(record.path));
      continue;
    }
    const key = `${record.sha256}-${record.bytes}`;
    let value = verifiedContents.get(key);
    if (!value) {
      const entry = transport.entries.get(key);
      value = downloadedAssets.get(entry.file).subarray(entry.offset, entry.offset + entry.bytes);
      if (value.length !== record.bytes || (await hash(value)) !== record.sha256)
        fail("Packed runtime asset integrity mismatch");
      verifiedContents.set(key, value);
    }
    assets.set(record.path, value);
  }
  downloadedAssets.clear();
  verifiedContents.clear();
  // 允许私有输入前，先验证可执行代码的完整依赖闭包。
  // Validate executable closure before allowing private inputs.
  function bytes(path) {
    const value = assets.get(path);
    if (!value) fail(`Missing verified runtime asset: ${path}`);
    return value;
  }
  function closure(directory, validate) {
    const sub = JSON.parse(new TextDecoder().decode(bytes(`${directory}/manifest.json`)));
    if (sub.version !== 1 || !Array.isArray(sub.assets) || sub.assets.length > 1024)
      fail("Invalid component manifest");
    validate?.(sub);
    const seen = new Set();
    return sub.assets
      .filter((a) => a.path !== "worker.mjs")
      .map((a) => {
        if (!safePath(a.path) || a.path.includes("/") || seen.has(a.path))
          fail("Invalid component asset");
        seen.add(a.path);
        const path = `${directory}/${a.path}`,
          record = inventory.find((x) => x.path === path);
        if (!record || record.bytes !== a.bytes || record.sha256 !== a.sha256)
          fail("Component manifest mismatch");
        return [a.path, bytes(path)];
      });
  }
  const plans = {};
  if (components.includes("assets"))
    plans.assets = {
      entry: `${ASSET_RUNTIME_DIRECTORY}/worker.mjs`,
      assets: closure(ASSET_RUNTIME_DIRECTORY, validateAssetRuntimeInventory),
    };
  if (components.includes("translator"))
    plans.translator = {
      entry: "sealed-runtime/dotnet-worker.mjs",
      assets: closure(`${OFFLINE}/dotnet-dist`),
    };
  if (components.includes("texture"))
    plans.texture = {
      entry: "sealed-runtime/texture-worker.mjs",
      assets: [
        ["texture.mjs", bytes(`${TEXTURE}/dist/texture.mjs`)],
        ["texture.wasm", bytes(`${TEXTURE}/dist/texture.wasm`)],
        ...(assets.has(`${TEXTURE}/dist/bc3.wasm`)
          ? [["bc3.wasm", bytes(`${TEXTURE}/dist/bc3.wasm`)]]
          : []),
        ["browser-texture.mjs", bytes(`${TEXTURE}/browser-texture.mjs`)],
        ["browser-texture-encoder.mjs", bytes(`${TEXTURE}/browser-texture-encoder.mjs`)],
      ],
    };
  if (components.includes("extraction"))
    plans.extraction = {
      entry: "sealed-workers/nsp-worker.mjs",
      assets: [
        ["hactool-boundary.wasm", bytes("toolchain/nsp-extract-spike/dist/hactool-boundary.wasm")],
      ],
    };
  if (components.includes("spirvCross"))
    plans.spirvCross = {
      entry: "sealed-workers/cross-worker.mjs",
      assets: [["spirv-cross.wasm", bytes("toolchain/spirv-cross-spike/wwwroot/spirv-cross.wasm")]],
    };
  if (components.includes("program"))
    plans.program = {
      entry: "sealed-workers/program-worker.mjs",
      assets: [
        ["capstone-lz4.wasm", bytes("toolchain/program-rewrite-spike/dist/capstone-lz4.wasm")],
      ],
    };
  // 旧清单仍能打开，但不虚构编译能力；新着色器运行时在私有输入前启动真实 AIR 编译器。
  // Legacy inventories remain readable without invented compiler capability; new shader runtimes initialize actual AIR before private inputs.
  if (
    airCompilerComponent &&
    components.some((name) => ["translator", "spirvCross", "airCompiler"].includes(name))
  ) {
    const receipt = JSON.parse(
      new TextDecoder("utf-8", { fatal: true }).decode(bytes(airCompilerComponent.receipt)),
    );
    assertAirCompilerReceiptMatchesComponent(receipt, airCompilerComponent);
    plans.airCompiler = {
      entry: airCompilerComponent.entry,
      assets: [
        ["factory", bytes(airCompilerComponent.factory)],
        ["wasm", bytes(airCompilerComponent.wasm)],
      ],
    };
  }
  if (selectedGroups.includes("legacy-astc"))
    plans.astcProducer = {
      entry: "sealed-workers/astc-producer-worker.mjs",
      assets: [
        ["astc-encoder.mjs", bytes("toolchain/astc-encoder-wasm/dist/astc-encoder.mjs")],
        ["astc-encoder.wasm", bytes("toolchain/astc-encoder-wasm/dist/astc-encoder.wasm")],
        ["png-codec.wasm", bytes(`${ASSET_RUNTIME_DIRECTORY}/png-codec.wasm`)],
        ["sha256.wasm", bytes("toolchain/sha256-stream-wasm/dist/sha256.wasm")],
      ],
    };
  if (selectedGroups.includes("ipa-signing"))
    plans.ipaSigning = {
      entry: "sealed-workers/signing-worker.mjs",
      assets: [["zsign.wasm", bytes("toolchain/zsign-wasm/dist/zsign.wasm")]],
    };
  for (const plan of Object.values(plans)) bytes(plan.entry);
  if (signal?.aborted) throw new DOMException("Runtime cancelled", "AbortError");
  // 原型：没有页面级 CSP 锁定。输入处理保持在本地。
  // Prototype: no page-level CSP lockdown. Input handling remains local.
  const channels = {},
    urls = [];
  let closed = false,
    recyclingAssets = false,
    assetWorkerUrl,
    producerClient,
    producerWorker,
    producerInitialization,
    producerTaken = false,
    signingFactory,
    signingInitialization,
    signingTaken = false,
    compilerClient,
    compilerInitialization,
    compilerLeases = 0,
    compilerActive = 0,
    compilerRetired = false,
    closing;
  let session;
  function close() {
    if (closed) return closing;
    closed = true;
    // 初始化客户端尚未返回时，原生 terminate 不会拒绝其等待；仅取消这段未完成的初始化。
    // Native terminate cannot reject an initializer that has not returned its client; cancel only that pending initialization.
    producerInitialization?.abort();
    signingInitialization?.abort();
    compilerInitialization?.abort();
    compilerClient?.close();
    if (!signingTaken) {
      signingFactory?.close();
    }
    if (session) sealedRuntimes.delete(session);
    for (const c of Object.values(channels)) c.stop();
    try {
      closing = Promise.resolve(producerClient?.close());
    } catch (error) {
      closing = Promise.reject(error);
    }
    producerWorker?.terminate();
    // 同步关闭调用方可能忽略返回值；保留可等待的错误，同时观察拒绝以避免未处理异常。
    // Synchronous callers may ignore the return value; retain awaitable cleanup errors while observing rejection.
    closing.catch(() => {});
    for (const url of urls) URL.revokeObjectURL(url);
    for (const plan of Object.values(plans)) plan.assets.length = 0;
    assets.clear();
    return closing;
  }
  // 可编辑界面只预载可选字节；使用时才从内存初始化，不启动任何网络读取。
  // Editable UI preloads optional bytes only; initialize from memory on use, without network reads.
  async function prepareAstcProducer({ signal: operationSignal } = {}) {
    if (closed) fail("Runtime session closed");
    operationSignal?.throwIfAborted();
    if (producerClient?.ready) return;
    const plan = plans.astcProducer;
    if (!plan || producerInitialization || producerClient || producerTaken)
      fail("ASTC producer unavailable or already acquired");
    const transferred = plan.assets.map(([local, value]) => [local, value.slice().buffer]),
      buffers = transferred.map(([, buffer]) => buffer);
    plan.assets.length = 0;
    const url = URL.createObjectURL(new Blob([bytes(plan.entry)], { type: "text/javascript" }));
    urls.push(url);
    producerWorker = new Worker(url, { type: "module" });
    producerInitialization = new AbortController();
    try {
      producerClient = await initializeAstcProducerClient({
        worker: producerWorker,
        assets: transferred,
        signal: operationSignal
          ? AbortSignal.any([operationSignal, producerInitialization.signal])
          : producerInitialization.signal,
      });
      if (closed || operationSignal?.aborted) {
        await producerClient?.close();
        throw new DOMException("Runtime cancelled", "AbortError");
      }
      if (
        producerClient?.initialized !== true ||
        producerClient.ready !== true ||
        buffers.some((buffer) => buffer.byteLength !== 0)
      )
        fail("Actual ASTC producer initialization/transfer failed");
    } catch (error) {
      try {
        await close();
      } catch {
        // 保留初始化错误，不让清理失败掩盖它。 | Preserve the initialization failure.
      }
      throw error;
    } finally {
      producerInitialization = undefined;
    }
  }
  try {
    const initialized = {};
    for (const [name, plan] of Object.entries(plans)) {
      if (name === "astcProducer" && deferOptionalInitialization) continue;
      const url =
        name === "ipaSigning" || name === "astcProducer"
          ? undefined
          : URL.createObjectURL(new Blob([bytes(plan.entry)], { type: "text/javascript" }));
      if (url !== undefined) urls.push(url);
      if (name === "assets") assetWorkerUrl = url;
      const labels = {
        assets: "game resources",
        texture: "textures",
        translator: "shaders",
        extraction: "package reader",
        program: "application compiler",
        spirvCross: "shader converter",
        astcProducer: "legacy texture producer",
        ipaSigning: "local IPA signer",
        airCompiler: "Metal shader compiler",
      };
      const label = `Preparing ${labels[name] ?? "application"}…`;
      report({ phase: "initializing", label }, true);
      if (name === "airCompiler") {
        compilerInitialization = new AbortController();
        try {
          compilerClient = await createAirCompilerWorkerPool({
            createWorker: () => {
              if (closed || compilerRetired) fail("AIR compiler runtime closed");
              return new Worker(url, { type: "module" });
            },
            assets: {
              factoryBytes: plan.assets[0][1],
              wasmBytes: plan.assets[1][1],
              factorySha256: airCompilerComponent.factorySha256,
              wasmSha256: airCompilerComponent.wasmSha256,
              headerSha256: airCompilerComponent.headerSha256,
            },
            signal: signal
              ? AbortSignal.any([signal, compilerInitialization.signal])
              : compilerInitialization.signal,
            onProgress: () => report({ phase: "initializing", label }, true),
          });
        } finally {
          compilerInitialization = undefined;
          plan.assets.length = 0;
        }
        if (closed || signal?.aborted) throw new DOMException("Runtime cancelled", "AbortError");
        if (compilerClient?.initialized !== true || compilerClient.ready !== true)
          fail("Actual AIR compiler initialization failed");
        initialized.airCompiler = {
          initialized: true,
          compiler: compilerClient.compiler,
          options: compilerClient.options,
        };
        continue;
      }
      if (name === "ipaSigning") {
        // 工厂拥有独立公开副本，不依赖转换运行时的生命周期。
        // The factory owns independent public copies, never this conversion runtime's lifetime.
        signingInitialization = new AbortController();
        try {
          signingFactory = await prepareVerifiedIpaSigningFactory({
            workerBlob: new Blob([bytes(plan.entry)], { type: "text/javascript" }),
            wasmBytes: plan.assets[0][1],
            warmup: !deferOptionalInitialization,
            signal: signal
              ? AbortSignal.any([signal, signingInitialization.signal])
              : signingInitialization.signal,
          });
        } finally {
          signingInitialization = undefined;
          plan.assets.length = 0;
        }
        if (closed || signal?.aborted) throw new DOMException("Runtime cancelled", "AbortError");
        if (!deferOptionalInitialization) initialized.ipaSigning = { initialized: true };
        continue;
      }
      if (name === "astcProducer") {
        await prepareAstcProducer({ signal });
        initialized.astcProducer = { initialized: true };
        continue;
      }
      let initializationStage = "starting";
      const shaderSubstep = { translator: "translation", spirvCross: "spirv-cross" }[name];
      const c = (channels[name] = channel(
        new Worker(url, { type: "module" }),
        close,
        (stage) => {
          initializationStage = String(stage).slice(0, 80);
          const stages = {
            "probe-before": "starting runtime",
            "config-module": "reading configuration",
            "loader-module": "loading engine",
            "runtime-create": "starting engine",
            "assembly-exports": "loading functions",
            "probe-after": "finishing startup",
          };
          report(
            { phase: "initializing", label: stages[stage] ? `${label} ${stages[stage]}` : label },
            true,
          );
        },
        shaderSubstep,
      ));
      const transferred = plan.assets.map(([local, value]) => [local, value.slice().buffer]);
      // 验证后的公开原件由 assets 保留；传输副本准备好后即可释放计划中的重复引用。
      // assets retains verified public originals; drop duplicate plan references once copies are ready.
      plan.assets.length = 0;
      const buffers = transferred.map(([, buffer]) => buffer);
      let result;
      try {
        result = await c.request(
          { type: "initialize", assets: transferred },
          buffers,
          signal,
          true,
        );
      } catch (error) {
        const failure = new Error(
          `${name} initialization (${initializationStage}): ${String(error?.message ?? error).slice(0, 1200)}`,
        );
        if (shaderSubstep)
          failure.shaderDiagnostic = shaderFailureDiagnostic(error, {
            substep: "pipeline-initialization",
          });
        throw failure;
      }
      if (signal?.aborted) throw new DOMException("Runtime cancelled", "AbortError");
      if (result?.initialized !== true || buffers.some((buffer) => buffer.byteLength !== 0))
        fail("Actual runtime initialization/transfer failed");
      initialized[name] = result;
    }
    if (closed || signal?.aborted) throw new DOMException("Runtime cancelled", "AbortError");
    // 后续可选初始化期间，先完成的 ASTC 线程可能已失败；历史证明不等于当前可用。
    // An earlier ASTC worker may fail during later optional startup; historical proof is not current readiness.
    if (producerClient && !producerClient.ready) fail("ASTC producer unavailable");
    if (compilerClient && !compilerClient.ready) fail("AIR compiler unavailable");
    // 公开清单字节仍可通过副本访问，绝不通过加载器或 fetch 回调提供。
    // 它们不包含私有任务输入，并在此会话关闭时丢弃。
    // Public inventory bytes remain available by copy, never via a loader/fetch callback.
    // They contain no private job inputs and are discarded when this session closes.
    // 生产流程可明确选择在着色器退休后提前丢弃独占 WASM 原件。
    // Production may explicitly opt to discard dedicated WASM originals after shader retirement.
    function readPublicAsset(path) {
      if (closed) fail("Runtime session closed");
      if (!safePath(path)) fail("Invalid public asset path");
      return bytes(path).slice();
    }
    const leaseCounts = new Map(),
      retiredComponents = new Set();
    function lease(name) {
      if (retiredComponents.has(name)) fail("Runtime component retired");
      if (closed || !channels[name]) fail("Runtime component unavailable");
      leaseCounts.set(name, (leaseCounts.get(name) ?? 0) + 1);
      let released = false,
        active = false;
      return {
        request(message, transfers, signal) {
          if (released || closed || active || (name === "assets" && recyclingAssets))
            return Promise.reject(new Error("Runtime lease closed or busy"));
          active = true;
          return channels[name].request(message, transfers, signal).finally(() => {
            active = false;
          });
        },
        close() {
          if (released) return;
          released = true;
          leaseCounts.set(name, leaseCounts.get(name) - 1);
          if (active) close();
        },
      };
    }
    function createAssetWorker() {
      const l = lease("assets");
      return {
        run(operation, value, options = {}) {
          const limits = Object.hasOwn(OPERATION_LIMITS, operation)
            ? OPERATION_LIMITS[operation]
            : undefined;
          if (!limits) fail("Unsupported asset operation");
          const cap = limits[0] * 1024 * 1024;
          const metadata = jsonOptions(options, limits[1]);
          const payload = input(value, cap).buffer;
          if (operation === "casc-prebuilt-overlay")
            validateRequest({ id: 1, operation, payload, options: metadata });
          return l.request(
            {
              type: "run",
              operation,
              payload,
              options: metadata,
            },
            [payload],
          );
        },
        close: l.close,
      };
    }
    async function recycleAssetWorker({ signal } = {}) {
      if (
        closed ||
        retiredComponents.has("assets") ||
        !plans.assets ||
        recyclingAssets ||
        channels.assets?.busy ||
        signal?.aborted
      )
        fail("Asset runtime unavailable or busy");
      recyclingAssets = true;
      channels.assets.stop();
      try {
        const next = (channels.assets = channel(
          new Worker(assetWorkerUrl, { type: "module" }),
          close,
        ));
        const files = closure(ASSET_RUNTIME_DIRECTORY, validateAssetRuntimeInventory).map(
          ([name, value]) => [name, value.slice().buffer],
        );
        const buffers = files.map(([, buffer]) => buffer);
        const result = await next.request(
          { type: "initialize", assets: files },
          buffers,
          signal,
          true,
        );
        if (closed || signal?.aborted) throw new DOMException("Runtime cancelled", "AbortError");
        if (result?.initialized !== true || buffers.some((buffer) => buffer.byteLength !== 0))
          fail("Asset runtime restart failed");
      } catch (error) {
        close();
        throw error;
      } finally {
        recyclingAssets = false;
      }
    }
    function createTextureClient() {
      const l = lease("texture");
      return {
        layout(descriptor, options = {}) {
          return l.request({
            operation: "layout",
            descriptor: jsonOptions(descriptor),
            options: jsonOptions(options),
          });
        },
        decode(descriptor, value, options = {}) {
          const source = input(value, 64 * 1024 * 1024);
          return l.request(
            {
              operation: "decode",
              descriptor: jsonOptions(descriptor),
              source,
              options: jsonOptions(options),
            },
            [source.buffer],
          );
        },
        decodeV61(value) {
          const source = input(value, 64 * 1024 * 1024 + 40);
          return l.request({ operation: "decodeV61", source }, [source.buffer]);
        },
        async decodeAtlas(value, { signal } = {}) {
          const { descriptor, ...geometry } = nativeArtAtlasDescriptor(value);
          const source = value.slice(40);
          const result = await l.request(
            { operation: "decodePillowBc3", descriptor, source },
            [source.buffer],
            signal,
          );
          return { ...result, ...geometry };
        },
        encodeAndPack({ templateHeader, rgba, width, height }, { signal } = {}) {
          if (signal?.aborted) throw new DOMException("Encoding cancelled", "AbortError");
          validateEncodeRequest(templateHeader, width, height);
          if (
            ![width, height].every((n) => Number.isSafeInteger(n) && n > 0 && n <= 8192) ||
            width * height > 4194304 ||
            rgba?.byteLength !== width * height * 4
          )
            fail("RGBA dimensions mismatch");
          const header = input(templateHeader, 40),
            pixels = input(rgba, 16 * 1024 * 1024);
          return l.request(
            { operation: "encodeAndPack", templateHeader: header, rgba: pixels, width, height },
            [header.buffer, pixels.buffer],
            signal,
          );
        },
        packLinearBC3({ templateHeader, linearBC3, width, height }, { signal } = {}) {
          const header = input(templateHeader, 40),
            linear = input(linearBC3, 16 * 1024 * 1024);
          return l.request(
            {
              operation: "packLinearBC3",
              templateHeader: header,
              linearBC3: linear,
              width,
              height,
            },
            [header.buffer, linear.buffer],
            signal,
          );
        },
        close: l.close,
      };
    }
    function createTextureEncoder() {
      const texture = createTextureClient();
      // 已验证 Rust 二进制为必需项，不再尝试旧 Python 编码分派。 | The verified Rust binary is required; never attempt the old Python encoder dispatch.
      if (!assets.has(`${TEXTURE}/dist/bc3.wasm`)) {
        texture.close();
        fail("Verified Rust BC3 encoder is required");
      }
      return {
        packLinearBC3: texture.packLinearBC3,
        encodeAndPack: texture.encodeAndPack,
        close: texture.close,
      };
    }
    function createTranslator() {
      const l = lease("translator");
      return {
        translate({ job, code, signal }) {
          const payload = input(code, 1024 * 1024).buffer;
          return l.request({ type: "run", job: jsonOptions(job), payload }, [payload], signal);
        },
        close: l.close,
      };
    }
    function createAirCompilerClient() {
      if (closed || compilerRetired || !compilerClient?.ready)
        fail(
          "Verified AIR compiler is unavailable. Rebuild the public runtime with compiled AIR assets; source-only shader output is not supported.",
        );
      compilerLeases++;
      let released = false,
        active = 0;
      const release = () => {
        if (released) return;
        released = true;
        compilerLeases--;
        if (active) close();
      };
      return Object.freeze({
        initialized: true,
        get ready() {
          return !released && !closed && !compilerRetired && compilerClient.ready;
        },
        compiler: compilerClient.compiler,
        options: compilerClient.options,
        limits: compilerClient.limits,
        concurrency: compilerClient.concurrency,
        async compile(source, options = {}) {
          if (released || closed || compilerRetired || active >= compilerClient.concurrency)
            fail("AIR compiler lease closed or busy");
          active++;
          compilerActive++;
          try {
            const result = await compilerClient.compile(source, options);
            try {
              // 底层输出已完成后，租约交接前仍可能取消或关闭。
              // Cancellation or closure can occur after compilation but before lease handoff.
              if (released || closed || compilerRetired) fail("AIR compiler lease closed or busy");
              options.signal?.throwIfAborted();
            } catch (error) {
              result.bytes.fill(0);
              throw error;
            }
            return result;
          } finally {
            active--;
            compilerActive--;
          }
        },
        dispose: release,
        close: release,
      });
    }
    function wipeShaderInput(value) {
      const buffer = ArrayBuffer.isView(value) ? value.buffer : value;
      if (!(buffer instanceof ArrayBuffer) || buffer.byteLength === 0) return;
      new Uint8Array(buffer).fill(0);
    }
    function ownedShaderInput(bytes, cap) {
      if (
        !(bytes instanceof Uint8Array) ||
        !(bytes.buffer instanceof ArrayBuffer) ||
        bytes.length > cap
      )
        fail("Runtime input cap");
      return bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength
        ? bytes.buffer
        : bytes.slice().buffer;
    }
    function workerFacade(client, invoke, inputField) {
      let stopped = false;
      const substep = inputField === "code" ? "translation" : "spirv-cross";
      const events = new Map();
      const bridge = {
        onmessage: null,
        onerror: null,
        addEventListener(type, listener) {
          if (!events.has(type)) events.set(type, new Set());
          events.get(type).add(listener);
        },
        removeEventListener(type, listener) {
          events.get(type)?.delete(listener);
        },
        postMessage(data, transfer = []) {
          if (stopped) fail("Runtime worker closed");
          // 同步捕获元数据和输入所有权，保持真实 Worker 的复制/转移语义。
          // Snapshot metadata and input ownership synchronously, matching real Worker semantics.
          let message;
          let correlation = {};
          try {
            const idField = inputField === "code" ? "requestId" : "id";
            const id = data?.[idField];
            // 关联标识只允许有界标量，绝不复制或回显任意对象。
            // Correlation IDs are bounded scalars; never clone or echo arbitrary objects.
            if (
              !(
                id === undefined ||
                id === null ||
                typeof id === "boolean" ||
                (typeof id === "number" && Number.isFinite(id)) ||
                (typeof id === "string" && id.length <= 256)
              )
            )
              fail("Invalid shader correlation ID");
            correlation = { [idField]: id };
            if (inputField === "code") {
              const code = data?.code;
              if (
                !(code instanceof Uint8Array) ||
                !(code.buffer instanceof ArrayBuffer) ||
                code.length > 1024 * 1024
              )
                fail("Runtime input cap");
              message = { ...correlation, code, job: jsonOptions(data.job) };
            } else {
              if (
                data?.type !== "convert" ||
                !(data.spirv instanceof ArrayBuffer) ||
                data.spirv.byteLength > 16 * 1024 * 1024
              )
                fail("Invalid Cross request");
              message = {
                ...correlation,
                type: "convert",
                spirv: data.spirv,
                replayContext: jsonOptions(data.replayContext),
              };
            }
          } catch (error) {
            const reply = {
              ...correlation,
              ok: false,
              error: "Sealed operation failed",
              shaderDiagnostic: shaderFailureDiagnostic(error, { substep }),
            };
            Promise.resolve().then(() => emit(reply));
            return;
          }
          const transfers = Array.from(transfer);
          let privateSlice;
          let snapshot;
          try {
            // 借用子视图只复制有效范围，不克隆其可能很大的整个底层缓冲区。
            // Copy only a borrowed subview's range, not its potentially much larger backing buffer.
            if (
              inputField === "code" &&
              !transfers.includes(message.code.buffer) &&
              (message.code.byteOffset !== 0 ||
                message.code.byteLength !== message.code.buffer.byteLength)
            ) {
              privateSlice = message.code.slice();
              message.code = privateSlice;
              transfers.push(privateSlice.buffer);
            }
            snapshot = structuredClone(message, { transfer: transfers });
          } finally {
            wipeShaderInput(privateSlice);
          }
          Promise.resolve()
            .then(async () => {
              try {
                if (stopped) return;
                return await invoke(snapshot);
              } finally {
                // 未交给真实工作线程的私有副本仍须清零，包括排队取消和忙碌拒绝。
                // Wipe private copies not handed to the real worker, including queued cancellation and busy rejection.
                wipeShaderInput(snapshot?.[inputField]);
              }
            })
            .then(
              (reply) => emit(reply),
              (error) =>
                emit({
                  id: snapshot?.id,
                  requestId: snapshot?.requestId,
                  ok: false,
                  error: "Sealed operation failed",
                  shaderDiagnostic: shaderFailureDiagnostic(error, { substep }),
                }),
            );
        },
        terminate() {
          if (stopped) return;
          stopped = true;
          events.clear();
          client.close();
        },
      };
      function emit(data) {
        if (stopped) return;
        const event = { data };
        bridge.onmessage?.(event);
        for (const listener of events.get("message") ?? []) listener(event);
      }
      return bridge;
    }
    function createTranslatorWorker() {
      const l = lease("translator");
      return workerFacade(
        l,
        async (data) => {
          const payload = ownedShaderInput(data.code, 1024 * 1024);
          try {
            return {
              requestId: data.requestId,
              result: await l.request({ type: "run", job: data.job, payload }, [payload]),
            };
          } finally {
            wipeShaderInput(payload);
          }
        },
        "code",
      );
    }
    function createCrossWorker() {
      const l = lease("spirvCross");
      return workerFacade(
        l,
        async (data) => {
          if (data?.type !== "convert" || !(data.spirv instanceof ArrayBuffer))
            fail("Invalid Cross request");
          const spirv = ownedShaderInput(new Uint8Array(data.spirv), 16 * 1024 * 1024);
          try {
            const result = await l.request(
              { type: "convert", spirv, replayContext: data.replayContext },
              [spirv],
            );
            return { id: data.id, ok: true, result };
          } finally {
            wipeShaderInput(spirv);
          }
        },
        "spirv",
      );
    }
    function createProgramWorker() {
      if (retiredComponents.has("program")) fail("Program worker retired");
      const l = lease("program");
      return {
        run({ bytes, moduleName, releaseId, caseName }) {
          if (caseName !== undefined)
            fail("Synthetic program profiles are not accepted by sealed application client");
          const copy = input(bytes, 32 * 1024 * 1024);
          return l.request({ bytes: copy, moduleName, releaseId }, [copy.buffer]);
        },
        close: l.close,
      };
    }
    // 完成阶段后只释放指定计算线程；整组空闲且没有共享客户端时才退休。
    // Retire only completed compute stages, atomically checking the whole group's idle leases.
    function retireComponents(names) {
      if (names.every((name) => retiredComponents.has(name))) return true;
      if (
        closed ||
        names.some(
          (name) =>
            !channels[name] ||
            leaseCounts.get(name) ||
            channels[name].busy ||
            (name === "assets" && recyclingAssets),
        )
      )
        return false;
      for (const name of names) {
        retiredComponents.add(name);
        channels[name].stop();
      }
      return true;
    }
    const retireProgramWorker = () => retireComponents(["program"]);
    const retireAssetWorkers = () => retireComponents(["assets", "texture"]);
    const retireShaderWorkers = ({ releasePublicWasm = false } = {}) => {
      if (typeof releasePublicWasm !== "boolean") fail("Invalid shader retirement option");
      if (compilerLeases || compilerActive) return false;
      if (!retireComponents(["translator", "spirvCross"])) return false;
      compilerRetired = true;
      compilerClient?.close();
      if (releasePublicWasm) {
        // 仅删除已认证且独占完整底层缓冲区的两个 WASM 原件引用；不清零、转移或触碰共享载荷。
        // Drop only the two authenticated WASM originals owning a whole unaliased buffer; never wipe, transfer or touch shared payloads.
        for (const path of [
          airCompilerComponent?.wasm,
          "toolchain/spirv-cross-spike/wwwroot/spirv-cross.wasm",
        ]) {
          const value = assets.get(path);
          if (!value || value.byteOffset !== 0 || value.byteLength !== value.buffer.byteLength)
            continue;
          if (
            [...assets].some(
              ([otherPath, other]) => otherPath !== path && other.buffer === value.buffer,
            )
          )
            continue;
          assets.delete(path);
        }
      }
      return true;
    };
    let extractionTaken = false;
    function createExtractionWorker() {
      if (closed || !channels.extraction || extractionTaken)
        fail("Extraction worker unavailable or already acquired");
      extractionTaken = true;
      return channels.extraction.takeWorker();
    }
    function createAstcProducerClient() {
      if (closed || !producerClient?.ready || producerTaken)
        fail("ASTC producer unavailable or already acquired");
      producerTaken = true;
      return producerClient;
    }
    function takeIpaSigningFactory() {
      if (closed || !signingFactory || signingTaken)
        fail("IPA signer unavailable or already acquired");
      signingTaken = true;
      return signingFactory;
    }
    const workerUrls = Object.freeze(
      Object.fromEntries(
        ["translator", "spirvCross"]
          .filter((name) => plans[name])
          .map((name) => [name, new URL(plans[name].entry, base).href]),
      ),
    );
    function createShaderWorkerFactory() {
      return (url) => {
        if (url === workerUrls.translator) return createTranslatorWorker();
        if (url === workerUrls.spirvCross) return createCrossWorker();
        fail("Unknown sealed shader worker URL");
      };
    }
    session = Object.freeze({
      initialized: Object.freeze(initialized),
      readPublicAsset,
      createAssetWorker,
      recycleAssetWorker,
      createTextureClient,
      createTextureEncoder,
      createTranslator,
      createAirCompilerClient,
      createAirCompiler: createAirCompilerClient,
      createTranslatorWorker,
      createCrossWorker,
      createProgramWorker,
      retireProgramWorker,
      retireAssetWorkers,
      retireShaderWorkers,
      createExtractionWorker,
      createAstcProducerClient,
      prepareAstcProducer,
      takeIpaSigningFactory,
      createShaderWorkerFactory,
      workerUrls,
      close,
    });
    sealedRuntimes.add(session);
    report({ phase: "initializing", label: "Preparing application profile…" }, true);
    if (closed || signal?.aborted) throw new DOMException("Runtime cancelled", "AbortError");
    return session;
  } catch (error) {
    try {
      await close();
    } catch {
      // 保留初始化失败；公开准备未完成时没有可交付的私有输出。
      // Preserve the initialization failure; incomplete public bootstrap cannot have deliverable private output.
    }
    throw error;
  }
}
