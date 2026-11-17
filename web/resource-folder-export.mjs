// 依据 dm/host/romfs_validate.c 验收表及 File System Access 句柄语义导出独立资源。
// Export separate resources using the dm/host/romfs_validate.c acceptance table and File System Access handle semantics.
import { createResourceWriteBuffer } from "./resource-write-buffer.mjs";
import { indexFiles, safeRelativePath } from "./preflight.mjs";
import { ROMFS_LIMITS } from "./romfs-tree.mjs";
import { verifyResourceData } from "./resource-data-reuse.mjs";
import { withResourceOutputLock } from "./resource-output-lock.mjs";

/**
 * 原始已认证 RomFS 导出，与未签名 IPA 分离。不使用 ZIP、上传、整游戏缓冲区、原生资源更改或调用方提供的有效标志。romfs 必须来自 readAuthenticatedAssets(extracted, selection).romfs，且该提取会话仍有效。本模块不会认证调用方虚构的读取器；编排适配器负责强制验证来源。
 * Raw authenticated RomFS export, separate from the unsigned IPA. No ZIP, upload,
 * whole-game buffer, native resource changes or caller-provided validity flags.
 * romfs MUST come from readAuthenticatedAssets(extracted, selection).romfs while
 * that extraction session is still live. This module does not authenticate a
 * caller-invented reader; provenance is enforced by the orchestration adapter.
 */
export const RESOURCE_EXPORT_LIMITS = Object.freeze({
  chunkBytes: 64 * 1024,
  files: ROMFS_LIMITS.members,
  pathBytes: ROMFS_LIMITS.pathBytes,
  totalPathBytes: ROMFS_LIMITS.totalPathBytes,
  maxFileBytes: BigInt(Number.MAX_SAFE_INTEGER),
  maxTotalBytes: BigInt(Number.MAX_SAFE_INTEGER),
});
export const RESOURCE_EXPORT_MARKER = ".d2rm-resource-export.json";
// 原生独立资源验收表：dm/host/romfs_validate.c 的 need[]/size[]。
// 这些是完整原始归档大小，而非稀疏或去零的 .holes 变体。
// Native separate-resource acceptance table, dm/host/romfs_validate.c need[]/size[].
// These are full raw archive sizes, not sparse/zero-stripped .holes variants.
const REQUIRED_NATIVE_ARCHIVES = Object.freeze(
  [
    ["00-00000001.data", 1073741824n],
    ["00-00000002.data", 1073741821n],
    ["00-00000003.data", 1026342790n],
    ["00-00000004.data", 3664822n],
    ["00-00000005.data", 391990840n],
    ["00-00000006.data", 3794407n],
    ["01-00000001.data", 344377972n],
    ["01-00000002.data", 37189352n],
    ["02-00000001.data", 368762692n],
    ["02-00000002.data", 23206186n],
    ["03-00000001.data", 251620752n],
    ["03-00000002.data", 55071917n],
    ["04-00000001.data", 118831405n],
    ["04-00000002.data", 8915855n],
    ["05-00000001.data", 478931036n],
    ["05-00000002.data", 92048235n],
    ["17-00000001.data", 204495617n],
    ["17-00000002.data", 4496514n],
  ].map(Object.freeze),
);
const encoder = new TextEncoder();
const fail = (code) => {
  throw new Error(code);
};
const requireValue = (value, code) => {
  if (!value) fail(code);
};

export class ResourceExportError extends Error {
  constructor(cause, report) {
    super("Local resource export did not complete.", { cause });
    this.name = "ResourceExportError";
    this.report = Object.freeze(report);
  }
}

function inventory(romfs) {
  requireValue(
    romfs && Array.isArray(romfs.files) && typeof romfs.getFile === "function",
    "RESOURCE_READER_REQUIRED",
  );
  requireValue(
    romfs.files.length > 0 && romfs.files.length <= RESOURCE_EXPORT_LIMITS.files,
    "RESOURCE_FILE_LIMIT",
  );
  let totalBytes = 0n,
    pathBytes = 0;
  const directories = new Set();
  const entries = romfs.files.map((file) => {
    requireValue(
      file && typeof file.path === "string" && file.path.length <= RESOURCE_EXPORT_LIMITS.pathBytes,
      "RESOURCE_PATH_LIMIT",
    );
    const path = safeRelativePath(file.path),
      parts = path.split("/");
    requireValue(parts.length <= ROMFS_LIMITS.depth + 1, "RESOURCE_DEPTH_LIMIT");
    requireValue(parts[0].toLowerCase() !== RESOURCE_EXPORT_MARKER, "RESOURCE_MARKER_COLLISION");
    requireValue(!/[<>"|?*\u0080-\u009f]/u.test(path), "RESOURCE_UNSAFE_PATH");
    const encoded = encoder.encode(path);
    requireValue(
      new TextDecoder("utf-8", { fatal: true }).decode(encoded) === path,
      "RESOURCE_PATH_ENCODING",
    );
    requireValue(
      encoded.length <= RESOURCE_EXPORT_LIMITS.pathBytes &&
        parts.every((part) => encoder.encode(part).length <= ROMFS_LIMITS.nameBytes),
      "RESOURCE_PATH_LIMIT",
    );
    pathBytes += encoded.length;
    requireValue(pathBytes <= RESOURCE_EXPORT_LIMITS.totalPathBytes, "RESOURCE_METADATA_LIMIT");
    requireValue(
      typeof file.size === "bigint" &&
        file.size >= 0n &&
        file.size <= RESOURCE_EXPORT_LIMITS.maxFileBytes,
      "RESOURCE_FILE_SIZE_LIMIT",
    );
    totalBytes += file.size;
    requireValue(totalBytes <= RESOURCE_EXPORT_LIMITS.maxTotalBytes, "RESOURCE_TOTAL_SIZE_LIMIT");
    for (let i = 1; i < parts.length; i++) directories.add(parts.slice(0, i).join("/"));
    requireValue(
      directories.size + romfs.files.length <= ROMFS_LIMITS.members,
      "RESOURCE_MEMBER_LIMIT",
    );
    return Object.freeze({ path, parts, size: file.size });
  });
  // 共享的重复项、大小写折叠别名及文件/目录冲突策略。
  // Shared duplicate, case-folded alias and file/directory conflict policy.
  indexFiles(entries.map(({ path, size }) => ({ path, size: Number(size) })));
  const sizes = new Map(entries.map(({ path, size }) => [path, size]));
  requireValue(
    REQUIRED_NATIVE_ARCHIVES.every(([name, size]) => sizes.get(`data/${name}`) === size),
    "RESOURCE_NATIVE_ARCHIVE_MISMATCH",
  );
  entries.sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));
  return { entries, totalBytes };
}

// 只验证资源布局；不读取载荷或创建输出。 | Validate resource layout without payload reads or output.
export function validateResourceInventory(romfs) {
  const { entries, totalBytes } = inventory(romfs);
  return Object.freeze({ files: entries.length, totalBytes });
}

/**
 * 目录选择器及权限属于此前明确的用户手势。只创建 data 子目录；现有目录仅逐字节校验而不写入。失败时只逐项删除已跟踪且句柄仍匹配的自有条目，绝不递归删除。Web Lock 排除同源导出；不支持并发外部写入者。保留未知新增项及替换项。
 * The directory picker/permission belongs to a prior explicit user gesture.
 * Create only data; an existing data directory is compared read-only. On failure, removal is limited
 * to individually tracked entries whose handles still match, never recursive.
 * A Web Lock excludes same-origin exports. A concurrent external writer is not supported (File System Access has no
 * exclusive-create primitive). Unknown additions/replacements are preserved.
 *
 * onProgress is advisory and receives BigInt counters, at most every 100ms plus
 * file/stage transitions. Do not JSON.stringify it without encoding BigInts.
 */
export async function exportRomfsResources(options = {}) {
  try {
    return await withResourceOutputLock(
      options.locks ?? globalThis.navigator?.locks,
      options.signal,
      () => exportResourceData(options),
    );
  } catch (cause) {
    if (cause instanceof ResourceExportError) throw cause;
    throw new ResourceExportError(cause, {
      status: "incomplete",
      directoryName: null,
      cleanupComplete: true,
      incompleteMarkerPresent: false,
      cleanupCodes: [],
      reason: ["RESOURCE_EXPORT_BUSY", "RESOURCE_EXPORT_LOCK_UNAVAILABLE"].includes(cause?.message)
        ? cause.message
        : undefined,
    });
  }
}

async function exportResourceData({ romfs, outputDirectoryHandle, signal, onProgress } = {}) {
  let directoryName = null,
    folder,
    marker,
    activeWriter,
    writeBuffer;
  let filesWritten = 0,
    bytesWritten = 0n,
    totalBytes = 0n,
    totalFiles = 0;
  let markerPresent = false,
    lastProgress = 0,
    existingDataPreserved = false,
    verifying = false,
    filesVerified = 0,
    bytesVerified = 0n;
  const owned = [],
    directories = new Map();
  const check = () => signal?.throwIfAborted();
  const emit = (stage, force = false) => {
    const now = Date.now();
    if (!force && now - lastProgress < 100) return;
    lastProgress = now;
    try {
      const result = onProgress?.(
        Object.freeze({
          stage,
          state: stage === "complete" ? "complete" : "running",
          directoryName,
          operation: verifying ? "verify" : "write",
          filesWritten: verifying ? filesVerified : filesWritten,
          totalFiles,
          bytesWritten: verifying ? bytesVerified : bytesWritten,
          totalBytes,
        }),
      );
      // 进度观察者不得阻塞导出或泄漏异步拒绝。
      // Progress observers must not block export or leak async rejections.
      Promise.resolve(result).catch(() => {});
    } catch {
      /* 观察者绝不会更改已认证导出。 | Observers never alter an authenticated export. */
    }
  };
  writeBuffer = createResourceWriteBuffer({
    check,
    write: (bytes) => activeWriter.write(bytes),
    written: (count) => {
      bytesWritten += BigInt(count);
      emit("resources");
    },
  });
  async function absent(parent, name, kind) {
    check();
    try {
      if (kind === "directory") await parent.getDirectoryHandle(name);
      else await parent.getFileHandle(name);
    } catch (cause) {
      if (cause?.name === "NotFoundError") return;
      throw cause; // 包括同名但类型错误的条目。 | Includes a same-name entry of the wrong kind.
    }
    fail("RESOURCE_DESTINATION_COLLISION");
  }
  async function create(parent, name, kind) {
    await absent(parent, name, kind);
    check();
    const handle =
      kind === "directory"
        ? await parent.getDirectoryHandle(name, { create: true })
        : await parent.getFileHandle(name, { create: true });
    const item = { parent, name, kind, handle, removed: false };
    owned.push(item); // 在观察取消之前记录所有权。 | Record ownership before observing cancellation.
    requireValue(typeof handle.isSameEntry === "function", "RESOURCE_OWNERSHIP_UNAVAILABLE");
    check();
    return item;
  }
  async function removeOwned(item) {
    if (item.removed) return;
    let current;
    try {
      current =
        item.kind === "directory"
          ? await item.parent.getDirectoryHandle(item.name)
          : await item.parent.getFileHandle(item.name);
    } catch (cause) {
      if (cause?.name === "NotFoundError") {
        item.removed = true;
        return;
      }
      throw cause;
    }
    requireValue(await item.handle.isSameEntry(current), "RESOURCE_ENTRY_REPLACED");
    // 特意不递归：即使在我们的文件夹内，也保留用户新增项。
    // Deliberately nonrecursive: preserve user additions, even inside our folder.
    await item.parent.removeEntry(item.name);
    item.removed = true;
  }
  async function writeMarker(status) {
    check();
    const record = {
      schemaVersion: 2,
      status,
      resourceLayout: "raw-data-directory",
      files: totalFiles,
      bytes: totalBytes.toString(),
      filesWritten,
      bytesWritten: bytesWritten.toString(),
      ipaStatus: "separate-output-not-tracked",
      notice:
        status === "complete"
          ? "Resource export complete. This does not establish IPA completion, installation or device compatibility."
          : "Incomplete resource export. Do not use this folder as a finished game resource set.",
    };
    activeWriter = await marker.handle.createWritable({ keepExistingData: false });
    check();
    await activeWriter.write(encoder.encode(JSON.stringify(record, null, 2) + "\n"));
    check();
    await activeWriter.close();
    activeWriter = undefined;
    markerPresent = true;
    // 尤其不能将已完成的最终标记关闭重新归类为失败。
    // In particular, do not reclassify a completed final marker close as failure.
  }
  try {
    check();
    const entries = inventory(romfs)
      .entries.filter((entry) => entry.parts[0] === "data")
      .map((entry) => ({
        ...entry,
        sourcePath: entry.path,
        path: entry.parts.slice(1).join("/"),
        parts: entry.parts.slice(1),
      }));
    requireValue(
      entries.every(
        (entry) => entry.parts.length && entry.parts[0].toLowerCase() !== RESOURCE_EXPORT_MARKER,
      ),
      "RESOURCE_MARKER_COLLISION",
    );
    totalBytes = entries.reduce((sum, entry) => sum + entry.size, 0n);
    totalFiles = entries.length;
    requireValue(globalThis.isSecureContext === true, "RESOURCE_SECURE_CONTEXT_REQUIRED");
    requireValue(
      outputDirectoryHandle?.kind === "directory" &&
        typeof outputDirectoryHandle.queryPermission === "function" &&
        typeof outputDirectoryHandle.getDirectoryHandle === "function" &&
        typeof outputDirectoryHandle.removeEntry === "function",
      "RESOURCE_DIRECTORY_REQUIRED",
    );
    requireValue(
      (await outputDirectoryHandle.queryPermission({ mode: "readwrite" })) === "granted",
      "RESOURCE_WRITE_PERMISSION_REQUIRED",
    );
    check();
    directoryName = "data";
    let existing;
    try {
      existing = await outputDirectoryHandle.getDirectoryHandle(directoryName);
    } catch (cause) {
      if (cause?.name !== "NotFoundError") {
        existingDataPreserved = true;
        fail("RESOURCE_EXISTING_DATA_UNVERIFIED");
      }
    }
    if (existing) {
      existingDataPreserved = verifying = true;
      emit("resources", true);
      try {
        await verifyResourceData({
          directory: existing,
          entries,
          romfs,
          markerName: RESOURCE_EXPORT_MARKER,
          signal,
          onProgress: (counts) => {
            filesVerified = counts.filesVerified;
            bytesVerified = counts.bytesVerified;
            emit("resources");
          },
        });
        requireValue(
          await existing.isSameEntry(await outputDirectoryHandle.getDirectoryHandle(directoryName)),
          "RESOURCE_EXISTING_DATA_UNVERIFIED",
        );
        check();
      } catch (cause) {
        if (signal?.aborted) throw cause;
        throw new Error("RESOURCE_EXISTING_DATA_UNVERIFIED", { cause });
      }
      writeBuffer.dispose();
      emit("complete", true);
      return Object.freeze({
        status: "complete",
        directoryName,
        resourceFolder: directoryName,
        reused: true,
        filesWritten,
        bytesWritten,
        filesVerified,
        bytesVerified,
        totalBytes,
      });
    }
    folder = await create(outputDirectoryHandle, directoryName, "directory");
    directories.set("", folder.handle);
    marker = await create(folder.handle, RESOURCE_EXPORT_MARKER, "file");
    await writeMarker("incomplete");
    check();
    emit("resources", true);
    for (const entry of entries) {
      check();
      const source = romfs.getFile(entry.sourcePath);
      requireValue(
        source && source.size === entry.size && typeof source.readAt === "function",
        "RESOURCE_READER_CHANGED",
      );
      let parent = folder.handle;
      for (let i = 0; i < entry.parts.length - 1; i++) {
        const path = entry.parts.slice(0, i + 1).join("/");
        let next = directories.get(path);
        if (!next) {
          next = (await create(parent, entry.parts[i], "directory")).handle;
          directories.set(path, next);
        }
        parent = next;
      }
      const output = await create(parent, entry.parts.at(-1), "file");
      activeWriter = await output.handle.createWritable({ keepExistingData: false });
      check();
      for (let offset = 0n; offset < entry.size;) {
        check();
        const count = Number(
          entry.size - offset < BigInt(RESOURCE_EXPORT_LIMITS.chunkBytes)
            ? entry.size - offset
            : BigInt(RESOURCE_EXPORT_LIMITS.chunkBytes),
        );
        const bytes = await source.readAt(offset, count, { signal });
        try {
          check();
          requireValue(
            bytes instanceof Uint8Array && bytes.byteLength === count,
            "RESOURCE_SHORT_READ",
          );
          await writeBuffer.append(bytes);
          offset += BigInt(count);
          check();
        } finally {
          // 写缓冲已复制来源；已认证读取器返回独立副本，而非共享缓存视图。
          // The write buffer copied the source; authenticated readers return owned copies, not shared cache views.
          if (bytes instanceof Uint8Array) bytes.fill(0);
        }
      }
      await writeBuffer.flush();
      check();
      await activeWriter.close();
      activeWriter = undefined;
      check();
      const writtenFile = await output.handle.getFile();
      requireValue(
        Number.isSafeInteger(writtenFile.size) && BigInt(writtenFile.size) === entry.size,
        "RESOURCE_WRITTEN_SIZE_MISMATCH",
      );
      filesWritten++;
      emit("resources", true);
    }
    requireValue(
      bytesWritten === totalBytes && filesWritten === totalFiles,
      "RESOURCE_EXPORT_INCOMPLETE",
    );
    await writeMarker("complete");
    writeBuffer.dispose();
    emit("complete", true);
    return Object.freeze({
      status: "complete",
      directoryName,
      resourceFolder: directoryName,
      markerName: RESOURCE_EXPORT_MARKER,
      filesWritten,
      bytesWritten,
      totalBytes,
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
        cleanupCodes.push("RESOURCE_WRITER_ABORT_FAILED");
      }
      activeWriter = undefined;
    }
    // 保留未完成标记，直到所有负载文件及子目录
    // 均被删除。如果清理失败，它仍是明确警告。
    // Keep the incomplete marker until all payload files and child directories
    // have been removed. If cleanup fails it remains an explicit warning.
    // abort 失败时无法证明写入已停止；保留全部自有输出及未完成标记。
    // A failed abort cannot prove writes stopped; retain all owned output and the incomplete marker.
    for (const item of writerStopped ? [...owned].reverse() : []) {
      if (item === folder || item === marker) continue;
      try {
        await removeOwned(item);
      } catch {
        cleanupCodes.push("RESOURCE_ENTRY_CLEANUP_FAILED");
      }
    }
    if (!cleanupCodes.length) {
      if (marker) {
        try {
          await removeOwned(marker);
          markerPresent = false;
        } catch {
          cleanupCodes.push("RESOURCE_MARKER_CLEANUP_FAILED");
        }
      }
      if (!cleanupCodes.length && folder) {
        try {
          await removeOwned(folder);
        } catch {
          cleanupCodes.push("RESOURCE_FOLDER_CLEANUP_FAILED");
        }
      }
    }
    throw new ResourceExportError(cause, {
      status: "incomplete",
      ...(existingDataPreserved
        ? { existingDataPreserved: true, reason: "RESOURCE_EXISTING_DATA_UNVERIFIED" }
        : {}),
      code: signal?.aborted ? "CANCELLED" : "RESOURCE_EXPORT_FAILED",
      directoryName,
      filesWritten,
      bytesWritten: bytesWritten.toString(),
      totalBytes: totalBytes.toString(),
      cleanupComplete: owned.every((item) => item.removed),
      incompleteMarkerPresent: markerPresent,
      cleanupCodes: Object.freeze([...new Set(cleanupCodes)]),
    });
  }
}
