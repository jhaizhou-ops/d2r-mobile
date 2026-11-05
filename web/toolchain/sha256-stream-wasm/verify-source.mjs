// 独立验证已有固定检出目录、源文件和许可证；不下载，也不选择任意缓存目录。
// Independently verify an explicit pinned checkout, source files and licenses; no downloads or cache guessing.
import { execFileSync } from "node:child_process";
import { readFileSync, realpathSync, lstatSync } from "node:fs";
import { createHash } from "node:crypto";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
export const directory = dirname(fileURLToPath(import.meta.url));
export const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");
export function verifySource(source) {
  if (typeof source !== "string" || !source)
    throw new Error("Explicit existing source checkout required");
  const root = realpathSync(source);
  const pin = JSON.parse(readFileSync(resolve(directory, "source-pin.json"), "utf8"));
  const revision = execFileSync("git", ["-C", root, "rev-parse", "HEAD"], {
    encoding: "utf8",
  }).trim();
  if (revision !== pin.commit) throw new Error("SHA-256 source revision mismatch");
  for (const [path, expected] of Object.entries(pin.files)) {
    const file = resolve(root, path),
      info = lstatSync(file);
    if (!info.isFile() || info.isSymbolicLink() || realpathSync(file) !== file)
      throw new Error("Unsafe SHA-256 source path");
    const bytes = readFileSync(file);
    if (bytes.length !== expected.bytes || sha256(bytes) !== expected.sha256)
      throw new Error(`SHA-256 source mismatch: ${path}`);
  }
  const config = readFileSync(resolve(directory, "sha256_config.h"));
  if (sha256(config) !== pin.configSha256) throw new Error("SHA-256 configuration mismatch");
  return { root, pin };
}
