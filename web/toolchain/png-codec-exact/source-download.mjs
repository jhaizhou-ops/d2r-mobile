// 仅对固定公开源码的瞬时下载错误重试；大小或摘要不符立即失败。
// Retry transient downloads of pinned public source only; size or digest mismatch fails immediately.
import { createHash } from "node:crypto";
import { readFile, writeFile, mkdtemp, rename, rm } from "node:fs/promises";
import { dirname, join } from "node:path";
import { setTimeout as delay } from "node:timers/promises";

const transientStatuses = new Set([408, 429, 500, 502, 503, 504]);
function sourceMismatch(pin, response, receivedBytes, receivedSha256 = null, complete = false) {
  // 仅记录有界元数据，不输出未通过验证的响应正文，也不放宽固定摘要。
  // Log bounded metadata, never an unverified response body or a relaxed digest.
  const details = {
    url: pin.url,
    status: response.status,
    contentType: response.headers.get("content-type")?.slice(0, 128) ?? null,
    contentEncoding: response.headers.get("content-encoding")?.slice(0, 128) ?? null,
    expectedBytes: pin.bytes,
    receivedBytes,
    complete,
    expectedSha256: pin.sha256,
    receivedSha256,
  };
  return new Error("Pinned zlib source mismatch: " + JSON.stringify(details));
}

export async function downloadPinnedZlib(pin, { fetch = globalThis.fetch, sleep = delay } = {}) {
  if (
    !Number.isSafeInteger(pin.bytes) ||
    pin.bytes < 1 ||
    pin.bytes > 16 * 1024 * 1024 ||
    !/^[a-f0-9]{64}$/.test(pin.sha256) ||
    !pin.url.startsWith("https://")
  )
    throw new Error("Invalid zlib source pin");
  for (let attempt = 0; attempt < 3; attempt++) {
    let retry = false,
      reader;
    try {
      let response;
      try {
        response = await fetch(pin.url, { signal: AbortSignal.timeout(120000) });
      } catch (error) {
        retry = error instanceof TypeError || ["AbortError", "TimeoutError"].includes(error?.name);
        throw error;
      }
      if (!response.ok) {
        retry = transientStatuses.has(response.status);
        await response.body?.cancel().catch(() => {});
        throw new Error(`zlib source fetch ${response.status}`);
      }
      if (!response.body) throw new Error("Missing zlib source body");
      reader = response.body.getReader();
      const raw = new Uint8Array(pin.bytes);
      let offset = 0;
      for (;;) {
        let part;
        try {
          part = await reader.read();
        } catch (error) {
          retry =
            error instanceof TypeError || ["AbortError", "TimeoutError"].includes(error?.name);
          throw error;
        }
        if (part.done) break;
        if (!(part.value instanceof Uint8Array)) throw sourceMismatch(pin, response, offset);
        if (part.value.length > raw.length - offset)
          throw sourceMismatch(pin, response, offset + part.value.length);
        raw.set(part.value, offset);
        offset += part.value.length;
      }
      const digest = createHash("sha256").update(raw.subarray(0, offset)).digest("hex");
      if (offset !== pin.bytes || digest !== pin.sha256)
        throw sourceMismatch(pin, response, offset, digest, true);
      return raw;
    } catch (error) {
      if (!retry || attempt === 2) throw error;
    } finally {
      // 不保留失败响应或部分文件；仅调用方在完整校验后写入缓存。
      // Retain no failed response or partial file; the caller caches only fully verified bytes.
      if (reader) {
        await reader.cancel().catch(() => {});
        reader.releaseLock();
      }
    }
    await sleep(1000 * (attempt + 1));
  }
}

// 缓存只有通过固定大小和摘要校验才可复用；完整替换通过同目录原子重命名发布。
// Reuse only size/digest-verified caches; publish complete replacements by same-directory atomic rename.
export async function readPinnedZlibSource(path, pin, options) {
  let cached;
  try {
    cached = await readFile(path);
  } catch (error) {
    if (error.code !== "ENOENT") throw error;
  }
  if (
    cached?.length === pin.bytes &&
    createHash("sha256").update(cached).digest("hex") === pin.sha256
  )
    return cached;
  const bytes = await downloadPinnedZlib(pin, options);
  const staging = await mkdtemp(join(dirname(path), ".zlib-source-"));
  try {
    const temporary = join(staging, "archive");
    await writeFile(temporary, bytes);
    await rename(temporary, path);
  } finally {
    await rm(staging, { recursive: true, force: true }).catch(() => {});
  }
  return bytes;
}
