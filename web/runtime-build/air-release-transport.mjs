// 仅用于构建时传输，不执行编译器或安装到目标目录。
// Build-time transport only. No compiler execution or destination installation.
import { Buffer } from "node:buffer";
import { lstat, readFile } from "node:fs/promises";
export async function boundedReleaseRead(path, maximum) {
  const info = await lstat(path);
  if (!info.isFile() || info.isSymbolicLink() || info.size < 1 || info.size > maximum)
    throw new Error(`Invalid bounded AIR release input: ${path}`);
  const bytes = await readFile(path);
  if (bytes.length !== info.size) throw new Error("AIR release input changed while reading");
  return bytes;
}
function officialDownloadUrl(value) {
  const url = new URL(value);
  if (
    url.protocol !== "https:" ||
    url.username ||
    url.password ||
    url.port ||
    !["github.com", "release-assets.githubusercontent.com"].includes(url.hostname)
  )
    throw new Error("AIR release download redirected outside official GitHub asset hosts");
  return url;
}
export async function downloadAirReleaseBytes(url, maximum, fetcher = fetch) {
  let current = officialDownloadUrl(url);
  const signal = AbortSignal.timeout(10 * 60_000);
  for (let redirects = 0; redirects <= 3; redirects++) {
    const response = await fetcher(current, { redirect: "manual", credentials: "omit", signal });
    if ([301, 302, 303, 307, 308].includes(response.status)) {
      const target = response.headers.get("location");
      await response.body?.cancel();
      if (!target || redirects === 3) throw new Error("Invalid AIR release asset redirect");
      current = officialDownloadUrl(new URL(target, current));
      continue;
    }
    if (!response.ok || !response.body)
      throw new Error(`Pinned AIR release download failed: ${response.status}`);
    const length = response.headers.get("content-length");
    if (length !== null && (!/^[0-9]+$/.test(length) || Number(length) > maximum))
      throw new Error("AIR release declared byte length exceeds cap");
    const parts = [];
    let total = 0;
    for await (const part of response.body) {
      total += part.byteLength;
      if (total > maximum) throw new Error("AIR release download exceeded byte cap");
      parts.push(part);
    }
    if (!total || (length !== null && total !== Number(length)))
      throw new Error("AIR release download length mismatch");
    return Buffer.concat(parts, total);
  }
  throw new Error("AIR release redirect limit exceeded");
}
export async function downloadAirForkSourceBytes(url, maximum, fetcher = fetch) {
  const target = new URL(url);
  if (
    target.protocol !== "https:" ||
    target.hostname !== "raw.githubusercontent.com" ||
    target.username ||
    target.password ||
    target.port ||
    target.search ||
    target.hash ||
    !/^\/[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+\/[a-f0-9]{40}\//.test(target.pathname)
  )
    throw Error("AIR source URL must name an exact official GitHub commit");
  const response = await fetcher(target, {
    redirect: "error",
    credentials: "omit",
    signal: AbortSignal.timeout(120000),
  });
  if (!response.ok || !response.body) throw Error("Pinned AIR source download failed");
  const length = response.headers.get("content-length");
  if (length !== null && (!/^[0-9]+$/.test(length) || Number(length) > maximum))
    throw Error("AIR source declared bytes exceed cap");
  const chunks = [];
  let size = 0;
  for await (const chunk of response.body) {
    size += chunk.byteLength;
    if (size > maximum) throw Error("AIR source exceeded byte cap");
    chunks.push(chunk);
  }
  if (!size || (length !== null && size !== Number(length)))
    throw Error("AIR source length mismatch");
  return Buffer.concat(chunks, size);
}
