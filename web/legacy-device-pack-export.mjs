// 按 main.m 的改名目录查找及 astc_pack.py finalize 布局导出补充包，只删除已跟踪的自有条目。
// Export supplemental packs using main.m renamed-folder lookup and astc_pack.py finalize layout, removing only tracked owned entries.
import { createResourceWriteBuffer } from "./resource-write-buffer.mjs";
import { validateLegacyPackMetadata } from "./astc-output-validator.mjs";
import { unusedOutputDirectory, withResourceOutputLock } from "./resource-output-lock.mjs";
export const LEGACY_PACK_EXPORT_MARKER = ".d2rm-legacy-pack-export.json";
export const LEGACY_PACK_EXPORT_CHUNK_BYTES = 64 * 1024;
const encoder = new TextEncoder();
const fail = (code) => {
  throw new Error(code);
};
export class LegacyPackExportError extends Error {
  constructor(cause, report) {
    super("Local Legacy Device Pack export did not complete.", { cause });
    this.name = "LegacyPackExportError";
    this.report = Object.freeze(report);
  }
}
/**
 * finalizedPack 必须来自仍有效的内部最终暂存；哈希和位置契约不是认证。既不关闭源也不联网。
 * finalizedPack must come from a live internal finalized stage; hash/location contracts are not authentication. Never disposes the source or uses networking.
 */
export async function exportLegacyDevicePack(options = {}) {
  try {
    return await withResourceOutputLock(
      options.locks ?? globalThis.navigator?.locks,
      options.signal,
      () => exportLegacyPack(options),
    );
  } catch (cause) {
    if (cause instanceof LegacyPackExportError) throw cause;
    throw new LegacyPackExportError(cause, {
      status: "incomplete",
      code: options.signal?.aborted ? "CANCELLED" : "LEGACY_PACK_EXPORT_FAILED",
      directoryName: null,
      filesWritten: 0,
      bytesWritten: 0,
      totalBytes: 0,
      cleanupComplete: true,
      incompleteMarkerPresent: false,
      cleanupCodes: [],
    });
  }
}
async function exportLegacyPack({
  finalizedPack,
  outputDirectoryHandle,
  signal,
  onProgress,
  secureContext = globalThis.isSecureContext,
} = {}) {
  let directoryName = null,
    folder,
    marker,
    activeWriter,
    writeBuffer,
    markerPresent = false,
    bytesWritten = 0,
    filesWritten = 0,
    pack,
    encoding,
    source,
    lastProgress = 0;
  const owned = [];
  const check = () => signal?.throwIfAborted();
  const emit = (state, force = false) => {
    const now = Date.now();
    if (!force && now - lastProgress < 100) return;
    lastProgress = now;
    try {
      Promise.resolve(
        onProgress?.(
          Object.freeze({
            state,
            directoryName,
            bytesWritten,
            totalBytes: pack.size,
            filesWritten,
            totalFiles: 2,
          }),
        ),
      ).catch(() => {});
    } catch {
      /* 观察者不影响导出。 | Observers do not affect export. */
    }
  };
  writeBuffer = createResourceWriteBuffer({
    check,
    write: (bytes) => activeWriter.write(bytes),
    written: (count) => {
      bytesWritten += count;
      emit("running");
    },
  });
  async function create(parent, name, kind) {
    check();
    try {
      if (kind === "directory") await parent.getDirectoryHandle(name);
      else await parent.getFileHandle(name);
      fail("LEGACY_PACK_COLLISION");
    } catch (cause) {
      if (cause?.name !== "NotFoundError") throw cause;
    }
    check();
    const handle =
      kind === "directory"
        ? await parent.getDirectoryHandle(name, { create: true })
        : await parent.getFileHandle(name, { create: true });
    const item = { parent, name, kind, handle, removed: false };
    owned.push(item);
    if (typeof handle.isSameEntry !== "function") fail("LEGACY_PACK_OWNERSHIP_UNAVAILABLE");
    check();
    return item;
  }
  async function remove(item) {
    if (item.removed) return;
    let current;
    try {
      current =
        item.kind === "directory"
          ? await item.parent.getDirectoryHandle(item.name)
          : await item.parent.getFileHandle(item.name);
    } catch (cause) {
      if (cause?.name !== "NotFoundError") throw cause;
      item.removed = true;
      return;
    }
    if (!(await item.handle.isSameEntry(current))) fail("LEGACY_PACK_ENTRY_REPLACED");
    // 不递归删除，以保护用户后来加入或替换的文件。
    // Never remove recursively, preserving files subsequently added or replaced by the user.
    await item.parent.removeEntry(item.name);
    item.removed = true;
  }
  async function writeSmall(item, bytes) {
    check();
    activeWriter = await item.handle.createWritable({ keepExistingData: false });
    check();
    await activeWriter.write(bytes);
    check();
    await activeWriter.close();
    activeWriter = undefined;
  }
  async function writeMarker(status) {
    await writeSmall(
      marker,
      encoder.encode(
        JSON.stringify({
          schemaVersion: 1,
          status,
          pack,
          bytesWritten,
          filesWritten,
          ipaStatus: "separate-output-not-tracked",
        }) + "\n",
      ),
    );
    markerPresent = true;
  }
  try {
    if (signal !== undefined && !(signal instanceof AbortSignal))
      fail("LEGACY_PACK_INVALID_SIGNAL");
    check();
    pack = validateLegacyPackMetadata(finalizedPack?.pack);
    if (finalizedPack?.encoding !== undefined) {
      const value = finalizedPack.encoding;
      if (
        !value ||
        Object.keys(value).sort().join(",") !== "format,preset" ||
        value.format !== "astc" ||
        !["thorough", "fast"].includes(value.preset)
      )
        fail("LEGACY_PACK_INVALID_ENCODING");
      // 编码来源说明不是外部内容认证，原生 pack 字段保持不变。
      // Encoding provenance is not external-content authentication; native pack fields stay unchanged.
      encoding = Object.freeze({ format: "astc", preset: value.preset });
    }
    source = finalizedPack?.source;
    if (
      source?.size !== pack.size ||
      source.file?.size !== pack.size ||
      typeof source.readAt !== "function"
    )
      fail("LEGACY_PACK_SOURCE_REQUIRED");
    if (secureContext !== true) fail("LEGACY_PACK_SECURE_CONTEXT_REQUIRED");
    if (
      outputDirectoryHandle?.kind !== "directory" ||
      typeof outputDirectoryHandle.queryPermission !== "function" ||
      typeof outputDirectoryHandle.getDirectoryHandle !== "function" ||
      typeof outputDirectoryHandle.removeEntry !== "function"
    )
      fail("LEGACY_PACK_DIRECTORY_REQUIRED");
    if ((await outputDirectoryHandle.queryPermission({ mode: "readwrite" })) !== "granted")
      fail("LEGACY_PACK_PERMISSION_REQUIRED");
    check();
    directoryName = await unusedOutputDirectory(outputDirectoryHandle, "Legacy Device Pack", check);
    folder = await create(outputDirectoryHandle, directoryName, "directory");
    marker = await create(folder.handle, LEGACY_PACK_EXPORT_MARKER, "file");
    await writeMarker("incomplete");
    check();
    emit("running", true);
    const data = await create(folder.handle, "data", "directory");
    const archive = await create(data.handle, pack.name, "file");
    activeWriter = await archive.handle.createWritable({ keepExistingData: false });
    check();
    for (let offset = 0; offset < pack.size;) {
      check();
      const count = Math.min(LEGACY_PACK_EXPORT_CHUNK_BYTES, pack.size - offset);
      const bytes = await source.readAt(offset, count, { signal });
      const boundedBytes =
        bytes instanceof Uint8Array &&
        bytes.buffer instanceof ArrayBuffer &&
        !bytes.buffer.resizable &&
        bytes.buffer.byteLength <= LEGACY_PACK_EXPORT_CHUNK_BYTES;
      try {
        check();
        if (!boundedBytes || bytes.length !== count) fail("LEGACY_PACK_READ_RANGE");
        await writeBuffer.append(bytes);
        offset += count;
        check();
      } finally {
        // 读取返回后立即拥有有界副本；取消或短读失败也必须清理。
        // Own bounded read copies immediately; wipe them on cancellation or short-read failure too.
        if (boundedBytes) bytes.fill(0);
      }
    }
    await writeBuffer.flush();
    check();
    await activeWriter.close();
    activeWriter = undefined;
    check();
    if ((await archive.handle.getFile()).size !== pack.size) fail("LEGACY_PACK_WRITTEN_SIZE");
    filesWritten++;
    const manifest = await create(folder.handle, "manifest.json", "file");
    const manifestBytes = encoder.encode(
      JSON.stringify({ files: [pack], ...(encoding ? { encoding } : {}) }, null, 1) + "\n",
    );
    await writeSmall(manifest, manifestBytes);
    check();
    if ((await manifest.handle.getFile()).size !== manifestBytes.length)
      fail("LEGACY_PACK_WRITTEN_SIZE");
    filesWritten++;
    check();
    await writeMarker("complete");
    // 最终标记成功关闭是提交边界；之后发生的取消不会把完整输出误报为失败。
    // Successful final-marker close is the commit boundary; later cancellation does not misreport complete output as failure.
    writeBuffer.dispose();
    emit("complete", true);
    return Object.freeze({
      status: "complete",
      ...(encoding ? { encoding } : {}),
      directoryName,
      markerName: LEGACY_PACK_EXPORT_MARKER,
      pack,
      filesWritten,
      bytesWritten,
      totalBytes: pack.size,
    });
  } catch (cause) {
    writeBuffer.dispose();
    const cleanupCodes = [];
    let writerStopped = true;
    if (activeWriter) {
      try {
        await activeWriter.abort();
      } catch {
        writerStopped = false;
        cleanupCodes.push("LEGACY_PACK_WRITER_ABORT_FAILED");
      }
      activeWriter = undefined;
    }
    // abort 失败时无法证明写入已停止；保留全部自有输出及未完成标记。
    // A failed abort cannot prove writes stopped; retain all owned output and the incomplete marker.
    for (const item of writerStopped ? [...owned].reverse() : []) {
      if (item === folder || item === marker) continue;
      try {
        await remove(item);
      } catch {
        cleanupCodes.push("LEGACY_PACK_ENTRY_CLEANUP_FAILED");
      }
    }
    if (!cleanupCodes.length && marker) {
      try {
        await remove(marker);
        markerPresent = false;
      } catch {
        cleanupCodes.push("LEGACY_PACK_MARKER_CLEANUP_FAILED");
      }
    }
    if (!cleanupCodes.length && folder) {
      try {
        await remove(folder);
      } catch {
        cleanupCodes.push("LEGACY_PACK_FOLDER_CLEANUP_FAILED");
      }
    }
    throw new LegacyPackExportError(cause, {
      status: "incomplete",
      code: signal?.aborted ? "CANCELLED" : "LEGACY_PACK_EXPORT_FAILED",
      directoryName,
      filesWritten,
      bytesWritten,
      totalBytes: pack?.size ?? 0,
      cleanupComplete: owned.every((item) => item.removed),
      incompleteMarkerPresent: markerPresent,
      cleanupCodes: Object.freeze([...new Set(cleanupCodes)]),
    });
  }
}
