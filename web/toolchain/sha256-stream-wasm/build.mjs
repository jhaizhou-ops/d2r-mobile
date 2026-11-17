// 用已有 emsdk 和显式固定源码构建独立 SHA-256；不安装、不联网、不修改提取器。
// Build standalone SHA-256 with existing emsdk and explicit pinned sources; no installation, network or extractor changes.
import process from "node:process";
import { execFileSync } from "node:child_process";
import { readFileSync, writeFileSync, mkdirSync, copyFileSync } from "node:fs";
import { resolve } from "node:path";
import { gzipSync } from "node:zlib";
import { directory, verifySource, sha256 } from "./verify-source.mjs";
const args = process.argv.slice(2);
if (args.length !== 2 || args[0] !== "--source")
  throw new Error("Usage: node build.mjs --source /explicit/hactool-checkout");
const { root, pin } = verifySource(args[1]);
const compiler = process.env.EMCC || "emcc";
const version = execFileSync(compiler, ["--version"], { encoding: "utf8" });
if (!/^emcc .*4\.0\.20\b/m.test(version)) throw new Error("Expected existing emsdk 4.0.20");
const out = resolve(directory, "dist");
mkdirSync(out, { recursive: true });
const flags = [
  "-std=c11",
  "-Oz",
  "-flto",
  "-Wall",
  "-Wextra",
  "-Werror",
  `-I${resolve(root, "mbedtls/include")}`,
  `-I${directory}`,
  '-DMBEDTLS_CONFIG_FILE="sha256_config.h"',
  resolve(directory, "bridge.c"),
  resolve(root, "mbedtls/library/sha256.c"),
  "--no-entry",
  "-sSTANDALONE_WASM=1",
  "-sFILESYSTEM=0",
  "-sALLOW_MEMORY_GROWTH=0",
  "-sINITIAL_MEMORY=262144",
  "-sSTACK_SIZE=16384",
  '-sEXPORTED_FUNCTIONS=["_sha_input","_sha_output","_sha_begin","_sha_update","_sha_finish","_sha_abort"]',
  "-o",
  resolve(out, "sha256.wasm"),
];
execFileSync(compiler, flags, { stdio: "inherit" });
const bytes = readFileSync(resolve(out, "sha256.wasm"));
const module = await WebAssembly.compile(bytes);
if (WebAssembly.Module.imports(module).length) throw new Error("Unexpected SHA-256 host imports");
const instance = await WebAssembly.instantiate(module);
if (instance.exports.memory.buffer.byteLength !== 262144)
  throw new Error("Unexpected SHA-256 memory size");
let fixed = false;
try {
  instance.exports.memory.grow(1);
} catch {
  fixed = true;
}
if (!fixed) throw new Error("SHA-256 memory must not grow");
for (const name of ["LICENSE", "apache-2.0.txt"])
  copyFileSync(resolve(root, "mbedtls", name), resolve(out, name));
const local = Object.fromEntries(
  ["bridge.c", "sha256_config.h", "adapter.mjs", "build.mjs", "source-pin.json"].map((path) => [
    path,
    sha256(readFileSync(resolve(directory, path))),
  ]),
);
const report = {
  schemaVersion: 1,
  source: pin,
  localSha256: local,
  compilerVersion: version.trim(),
  emsdk: "4.0.20",
  memoryBytes: 262144,
  stackBytes: 16384,
  chunkBytes: 65536,
  hostImports: 0,
  memoryGrowth: false,
  bytes: bytes.length,
  gzipBytes: gzipSync(bytes, { level: 9 }).length,
  sha256: sha256(bytes),
  flags: flags.map((flag) => flag.replaceAll(root, "$SOURCE").replaceAll(directory, "$TOOL")),
  productionEnabled: false,
};
writeFileSync(resolve(out, "build-report.json"), JSON.stringify(report, null, 2) + "\n");
console.log(JSON.stringify(report));
