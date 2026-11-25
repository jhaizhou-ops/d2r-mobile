// 只缓存已验证生成器的编码输出；输入仍在本次作业中读取和验证，绝不持久化原始输入或像素。
// Cache only validated generator outputs; fresh inputs are still read/checked, never persisted as raw bytes or pixels.
import { cacheKeyFor } from "./local-output-cache.mjs";

const INDEX_BYTES = 256 * 1024;
const MAX_FILES = 512;
const SHA256 = /^[a-f0-9]{64}$/;
const OPERATIONS = new Set(["native-art", "hud-v4", "map-goal"]);
const METADATA = Object.freeze({
  "native-art": ["assetCount"],
  "hud-v4": ["mode", "backend"],
  "map-goal": ["backend"],
});
const utf8 = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: true });
const digest = async (bytes) =>
  Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)), (byte) =>
    byte.toString(16).padStart(2, "0"),
  ).join("");
const view = (value) =>
  value instanceof Uint8Array
    ? value
    : value instanceof ArrayBuffer
      ? new Uint8Array(value)
      : undefined;
const validPath = (path) =>
  typeof path === "string" &&
  path.length > 0 &&
  path.length <= 512 &&
  !/[\\:]/u.test(path) &&
  [...path].every(
    (character) => character.charCodeAt(0) >= 32 && character.charCodeAt(0) !== 127,
  ) &&
  path.split("/").every((part) => part && part !== "." && part !== "..");
const wipe = (files) => {
  for (const value of Object.values(files ?? {})) view(value)?.fill(0);
};

/**
 * outputCache 是可信应用基础设施，使用独立锚定的摘要账本。不能仅凭目录清单授予信任；缓存命中和新结果接受相同校验。只保留单个生成器已限定大小的输出集，不另建合并归档或 base64 副本。
 *
 * outputCache is trusted application plumbing with an independently anchored digest ledger.
 * Never accept a folder manifest alone as authority. Validation is identical for hits and fresh results.
 * Only one generator's already-bounded output set is held, without a combined archive/base64 copy.
 */
export async function runCachedAssetOperation({
  outputCache,
  cacheContext,
  operation,
  payload,
  options = {},
  signal,
  checkCancellation = () => signal?.throwIfAborted(),
  maxFileBytes = 16 * 1024 * 1024,
  maxTotalBytes = 64 * 1024 * 1024,
  run,
  validate,
}) {
  const check = checkCancellation;
  check(signal);
  let key;
  if (
    OPERATIONS.has(operation) &&
    typeof outputCache?.read === "function" &&
    typeof outputCache?.write === "function" &&
    SHA256.test(cacheContext?.runtimeManifestSha256 ?? "") &&
    typeof cacheContext?.profileId === "string"
  ) {
    try {
      // 在工作线程转移输入之前计算哈希；载荷包括本次操作使用的每张图像、字体、字符串、布局及先前生成的依赖。
      // Hash before a worker can transfer the input. The payload includes every image, font,
      // string, layout and preceding generated dependency used by this exact operation.
      const inputSha256 = await digest(payload);
      key = await cacheKeyFor({
        schemaVersion: 1,
        boundary: "validated-encoded-art-v1",
        operation,
        context: cacheContext,
        options,
        inputSha256,
      });
    } catch {
      // 可选缓存的身份或存储失败不能阻止正常生成。
      // Optional cache identity/storage failures cannot disable normal generation.
      check(signal);
    }
  }
  check(signal);

  if (key) {
    let indexBytes;
    const files = Object.create(null);
    try {
      indexBytes = await outputCache.read({
        kind: "asset-index",
        key,
        maxBytes: INDEX_BYTES,
        signal,
      });
      check(signal);
      if (indexBytes) {
        const index = JSON.parse(decoder.decode(indexBytes));
        if (
          index?.schemaVersion !== 1 ||
          index.operation !== operation ||
          !Array.isArray(index.files) ||
          !index.files.length ||
          index.files.length > MAX_FILES ||
          !index.metadata ||
          Array.isArray(index.metadata) ||
          typeof index.metadata !== "object" ||
          Object.keys(index.metadata).some((name) => !METADATA[operation].includes(name))
        )
          throw new Error("ASSET_CACHE_INDEX");
        let total = 0;
        for (const record of index.files) {
          check(signal);
          if (
            !validPath(record?.path) ||
            Object.hasOwn(files, record.path) ||
            !Number.isSafeInteger(record.size) ||
            !SHA256.test(record.sha256 ?? "") ||
            record.size < 0 ||
            record.size > maxFileBytes ||
            (total += record.size) > maxTotalBytes
          )
            throw new Error("ASSET_CACHE_FILE");
          const fileKey = await cacheKeyFor({ key, ...record });
          const bytes = await outputCache.read({
            kind: "asset-file",
            key: fileKey,
            maxBytes: maxFileBytes,
            signal,
          });
          if (bytes) files[record.path] = bytes;
          check(signal);
          if (!(bytes instanceof Uint8Array) || bytes.length !== record.size)
            throw new Error("ASSET_CACHE_FILE");
        }
        const result = { ...index.metadata, files };
        await validate(result);
        check(signal);
        return result;
      }
    } catch {
      wipe(files);
      check(signal);
      // 缺失、格式错误或过期的缓存只算未命中，不能降低输出边界要求。
      // Missing, malformed or stale cache output is a miss, never a weaker output boundary.
    } finally {
      indexBytes?.fill(0);
    }
  }

  check(signal);
  const result = await run();
  try {
    check(signal);
    await validate(result);
    check(signal);
    if (key) {
      let indexBytes;
      try {
        const { files, ...metadata } = result;
        // 将来新增的线程诊断或原始中间产物不能静默加入持久存储。
        // Future worker diagnostics/raw intermediates are not silently added to persistent storage.
        if (Object.keys(metadata).some((name) => !METADATA[operation].includes(name)))
          return result;
        const entries = Object.entries(files ?? {});
        if (!entries.length || entries.length > MAX_FILES) return result;
        const records = [];
        let total = 0;
        for (const [path, value] of entries) {
          const bytes = view(value);
          if (
            !validPath(path) ||
            !bytes ||
            bytes.length > maxFileBytes ||
            (total += bytes.length) > maxTotalBytes
          )
            return result;
          records.push({ path, size: bytes.length, sha256: await digest(bytes) });
          check(signal);
        }
        // 只序列化有界元数据；编码后的输出文件独立保存并依次写入。
        // Serialize bounded metadata only. Encoded output files stay separate and are written serially.
        indexBytes = utf8.encode(
          JSON.stringify({ schemaVersion: 1, operation, metadata, files: records }),
        );
        if (indexBytes.length > INDEX_BYTES) return result;
        for (const record of records) {
          check(signal);
          // 绑定内容的文件键让部分刷新期间先前完整的索引仍然有效。
          // Content-bound file keys keep an earlier complete index valid during partial refreshes.
          const fileKey = await cacheKeyFor({ key, ...record });
          const written = await outputCache.write({
            kind: "asset-file",
            key: fileKey,
            bytes: view(files[record.path]),
            signal,
          });
          check(signal);
          if (!written) return result;
        }
        // 只有全部已接受输出文件成功关闭后，才发布完整索引。
        // Publish the complete index only after every accepted output file has closed successfully.
        await outputCache.write({ kind: "asset-index", key, bytes: indexBytes, signal });
        check(signal);
      } catch {
        check(signal);
      } finally {
        indexBytes?.fill(0);
      }
    }
    return result;
  } catch (error) {
    // 返回之前，本操作仍拥有晚到或已取消的线程结果。
    // Until return, the operation owns even late/cancelled worker results.
    wipe(result?.files);
    throw error;
  }
}
