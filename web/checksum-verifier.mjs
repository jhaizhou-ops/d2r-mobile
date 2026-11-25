// 仅校验旧归档格式的校验和，不用于认证或游戏身份识别。
// 依据归档读取器注入约定与上游分块 MD5 接口。
// Verifies legacy archive-format checksums only, not authentication or game identity.
// Based on the archive-reader injection contract and upstream chunked MD5 interface.

/**
 * 仅用于旧归档格式校验和，绝不用于认证或游戏身份识别。
 * Legacy archive-format checksum only, never authentication or game identity.
 */
import { createMD5 } from "hash-wasm";

export const MD5_CHUNK_BYTES = 64 * 1024;
const yieldTask = () => new Promise((resolve) => setTimeout(resolve, 0));

/**
 * 归档读取器注入接口：verifyMd5(bytes, expectedHex, { signal, kind, index })。
 * 仅哈希精确的 Uint8Array 视图；调用方在操作结束前不得修改它。
 * 内存开销受块大小限制，而非总输入大小。
 * Archive-reader injection: verifyMd5(bytes, expectedHex, { signal, kind, index }).
 * Hash only the exact Uint8Array view. The caller must not mutate it until settled.
 * Memory overhead is bounded by the chunk size, not the total input size.
 */
export async function verifyMd5(bytes, expectedHex, { signal } = {}) {
  signal?.throwIfAborted();
  if (!(bytes instanceof Uint8Array)) throw new TypeError("Expected Uint8Array bytes");
  if (
    typeof expectedHex !== "string" ||
    expectedHex.length !== 32 ||
    !/^[0-9a-f]{32}$/i.test(expectedHex)
  )
    throw new TypeError("Expected a 32-character MD5 hex digest");
  let hash = await createMD5();
  try {
    for (let offset = 0; offset < bytes.byteLength; offset += MD5_CHUNK_BYTES) {
      signal?.throwIfAborted();
      // 传递视图本身而非其底层缓冲区。上游将此有界块复制到工作缓冲区，
      // 工作内存固定，不随输入总量增长。
      // Pass the view itself, not its backing buffer. Upstream copies this bounded
      // chunk into fixed-size WASM working memory, independent of total input size.
      hash.update(bytes.subarray(offset, offset + MD5_CHUNK_BYTES));
      // 任务（而不只是微任务）让 UI 取消操作能在块之间运行。
      // A task (not just a microtask) lets UI cancellation run between chunks.
      await yieldTask();
      signal?.throwIfAborted();
    }
    signal?.throwIfAborted();
    return hash.digest() === expectedHex.toLowerCase();
  } finally {
    hash = null;
  }
}
