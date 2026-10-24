// 依据 OPFS 暂存输出事务和浏览器 Blob 接口提供 IPA 下载目标。
// Provide IPA download destinations based on OPFS staged-output transactions and the browser Blob interface.
import { createBrowserStagedOutputAdapters } from "./browser-staged-output.mjs";
import { planStagedZip } from "./staged-zip-validator.mjs";
import { ZIP_LIMITS } from "./zip-writer.mjs";

export const IPA_DOWNLOAD_LIMITS = Object.freeze({
  maxOutputBytes: ZIP_LIMITS.maxTotalBytes,
  maxChunkBytes: ZIP_LIMITS.maxChunkBytes,
});
export const IPA_DOWNLOAD_NAME = "D2RMobile-unsigned.ipa";
const requireValue = (value, code) => {
  if (!value) throw new Error(code);
};

/**
 * 现有 OPFS 事务之后的有界、仅用于 IPA 的 Blob 目标。资源归档绝不经过此适配器。与文件输出一样，发布 Blob 前会重新检查并删除已验证的暂存数据。不可变 Blob 分块避免再创建一个整归档的 JavaScript ArrayBuffer；浏览器仍可能在内存中保留整个归档。调用方拥有其创建的对象 URL，在提供下载重试期间必须保留，替换或销毁视图时再撤销。
 * Bounded IPA-only Blob destination behind the existing OPFS transaction.
 * Resource archives never pass through this adapter. The validated stage is
 * rechecked and removed before the Blob is published, just as for file output.
 * Immutable Blob chunks avoid a second whole-archive JavaScript ArrayBuffer;
 * browsers may still retain up to the complete archive in memory.
 *
 * The caller owns any object URL it creates and must retain it while download
 * retries are offered, then revoke it when replaced or the view is disposed.
 */
export function createBrowserBlobOutputAdapters({
  archiveFormat = "zip32",
  maxOutputBytes = IPA_DOWNLOAD_LIMITS.maxOutputBytes,
  ...stagingOptions
} = {}) {
  requireValue(
    Number.isSafeInteger(maxOutputBytes) &&
      maxOutputBytes > 0 &&
      maxOutputBytes <= IPA_DOWNLOAD_LIMITS.maxOutputBytes,
    "OUTPUT_BLOB_BUDGET",
  );
  let blob,
    expectedBytes,
    started = false,
    opened = false;
  const destinationHandle = {
    kind: "file",
    // 这是内存接收端，并非访问用户文件系统目标。
    // This is an in-memory sink, not access to a user filesystem destination.
    async queryPermission() {
      return "granted";
    },
    async createWritable() {
      requireValue(!opened, "OUTPUT_BLOB_ALREADY_OPEN");
      opened = true;
      let parts = [],
        bytesWritten = 0,
        state = "writing";
      return {
        async write(bytes) {
          requireValue(state === "writing", "OUTPUT_BLOB_NOT_WRITABLE");
          requireValue(
            bytes instanceof Uint8Array &&
              bytes.byteLength <= IPA_DOWNLOAD_LIMITS.maxChunkBytes &&
              bytes.byteLength <= expectedBytes - bytesWritten &&
              bytes.byteLength <= maxOutputBytes - bytesWritten,
            "OUTPUT_BLOB_BYTE_LIMIT",
          );
          parts.push(new Blob([bytes]));
          bytesWritten += bytes.byteLength;
        },
        async close() {
          requireValue(state === "writing", "OUTPUT_BLOB_NOT_WRITABLE");
          requireValue(bytesWritten === expectedBytes, "OUTPUT_BLOB_SHORT_ARCHIVE");
          const completed = new Blob(parts, { type: "application/octet-stream" });
          requireValue(completed.size === expectedBytes, "OUTPUT_BLOB_SIZE_MISMATCH");
          parts = [];
          blob = completed;
          state = "committed";
        },
        async abort() {
          if (state === "committed") return;
          parts = [];
          state = "aborted";
        },
      };
    },
  };
  const staged = createBrowserStagedOutputAdapters({
    ...stagingOptions,
    destinationHandle,
    archiveFormat,
    maxOutputBytes,
  });
  return Object.freeze({
    async createOutput(context) {
      requireValue(!started, "OUTPUT_BLOB_ALREADY_STARTED");
      expectedBytes = planStagedZip(context.manifest, {
        archiveFormat,
        maxOutputBytes,
      }).totalBytes;
      started = true;
      return staged.createOutput(context);
    },
    validateArchive: (context) => staged.validateArchive(context),
    getDownload() {
      requireValue(blob instanceof Blob, "OUTPUT_BLOB_NOT_COMMITTED");
      return Object.freeze({ blob, fileName: IPA_DOWNLOAD_NAME });
    },
  });
}
