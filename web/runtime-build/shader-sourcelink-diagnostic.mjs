// 只诊断固定构建输出中的 SourceLink 输入；不输出路径、URL、源码、环境或 PDB，也不改变载荷。
// Diagnose only fixed SourceLink build outputs; never report paths, URLs, source, environment or PDB contents, or change payloads.
import { Buffer } from "node:buffer";
import { createHash } from "node:crypto";
import { constants } from "node:fs";
import { lstat, open } from "node:fs/promises";
import { resolve, join } from "node:path";

const schema = "shader-sourcelink-diagnostic-v1";
const maximumBytes = 64 * 1024;
const candidates = [
  ["obj", "Release", "net10.0", "ShaderSpike.sourcelink.json"],
  ["obj", "Release", "net10.0", "browser-wasm", "ShaderSpike.sourcelink.json"],
];
const status = (value) => ({ schema, status: value });

async function candidatePath(root, segments) {
  let path = root;
  for (let index = -1; index < segments.length; index++) {
    if (index >= 0) path = join(path, segments[index]);
    let info;
    try {
      info = await lstat(path);
    } catch (error) {
      if (error.code === "ENOENT") return undefined;
      throw error;
    }
    if (
      info.isSymbolicLink() ||
      (index === segments.length - 1 ? !info.isFile() : !info.isDirectory())
    )
      throw new Error("Invalid diagnostic candidate");
  }
  return path;
}

async function boundedBytes(path) {
  const file = await open(path, constants.O_RDONLY | constants.O_NOFOLLOW);
  try {
    const info = await file.stat();
    if (!info.isFile() || info.size < 1 || info.size > maximumBytes)
      throw new Error("Diagnostic byte cap");
    const buffer = Buffer.alloc(maximumBytes + 1);
    let size = 0;
    while (size < buffer.length) {
      const { bytesRead } = await file.read(buffer, size, buffer.length - size, size);
      if (!bytesRead) break;
      size += bytesRead;
    }
    if (size !== info.size || size > maximumBytes) throw new Error("Diagnostic size changed");
    return buffer.subarray(0, size);
  } finally {
    await file.close();
  }
}

export async function inspectShaderSourceLink({ projectDirectory, sourceRevision } = {}) {
  try {
    if (typeof projectDirectory !== "string" || !/^[a-f0-9]{40}$/.test(sourceRevision))
      return status("invalid");
    const root = resolve(projectDirectory);
    const found = (await Promise.all(candidates.map((parts) => candidatePath(root, parts)))).filter(
      Boolean,
    );
    if (!found.length) return status("absent");
    if (found.length !== 1) return status("ambiguous");
    const bytes = await boundedBytes(found[0]);
    const value = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes));
    if (
      !value ||
      typeof value !== "object" ||
      Array.isArray(value) ||
      Object.keys(value).length !== 1 ||
      !Object.hasOwn(value, "documents") ||
      !value.documents ||
      typeof value.documents !== "object" ||
      Array.isArray(value.documents)
    )
      return status("invalid");
    const mappings = Object.entries(value.documents);
    if (mappings.length > 128) return status("invalid");
    let containsCurrentAppRevision = false;
    for (const [key, target] of mappings) {
      if (!key || key.length > 8192 || typeof target !== "string" || target.length > 8192)
        return status("invalid");
      const url = new URL(target);
      if (url.protocol !== "https:" || url.username || url.password || url.search || url.hash)
        return status("invalid");
      if (url.pathname.split("/").includes(sourceRevision)) containsCurrentAppRevision = true;
    }
    return {
      schema,
      status: "present",
      bytes: bytes.length,
      sha256: createHash("sha256").update(bytes).digest("hex"),
      documentMappings: mappings.length,
      containsCurrentAppRevision,
    };
  } catch {
    // 缺失或无效诊断不能使有效构建失败，也不能泄漏底层错误中的路径。
    // Missing/invalid diagnostics cannot fail a valid build or expose paths from underlying errors.
    return status("invalid");
  }
}
