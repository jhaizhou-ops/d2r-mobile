// 仅用于构建的流程；复用现有 CI 的 emsdk 4.0.20，不创建新 CI 作业。
// Build-only recipe. Uses the existing CI emsdk 4.0.20; creates no new CI job.
import process from "node:process";
import { readPinnedZlibSource } from "./source-download.mjs";
import { execFileSync } from "node:child_process";
import { readFileSync, writeFileSync, mkdirSync, cpSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { join } from "node:path";
import { createHash } from "node:crypto";
import { gzipSync } from "node:zlib";
const root = fileURLToPath(new URL(".", import.meta.url));
const hash = (b) => createHash("sha256").update(b).digest("hex");
const vendor = join(root, "vendor");
mkdirSync(vendor, { recursive: true });
async function source(pinName) {
  const pin = JSON.parse(readFileSync(new URL(pinName, import.meta.url)));
  const tar = join(vendor, `zlib-${pin.version}.tar.gz`);
  await readPinnedZlibSource(tar, pin);
  const names = execFileSync("tar", ["-tzf", tar], { encoding: "utf8" }).trim().split("\n");
  if (
    names.some(
      (n) => !n.startsWith(pin.root + "/") || n.startsWith("/") || n.split("/").includes(".."),
    )
  )
    throw new Error("Unsafe official zlib archive paths");
  execFileSync("tar", ["-xzf", tar, "-C", vendor]);
  return join(vendor, pin.root);
}
const zlib = await source("zlib-source.json");
const legacy = await source("zlib-legacy-source.json");
const emcc = process.env.EMCC || "emcc";
const version = execFileSync(emcc, ["--version"], { encoding: "utf8" });
if (!/^emcc .*4\.0\.20\b/m.test(version)) throw new Error("Expected existing emsdk 4.0.20");
mkdirSync(join(root, "dist"), { recursive: true });
const exports = [
  "png_input",
  "png_output",
  "png_width",
  "png_height",
  "png_error",
  "png_zlib_version",
  "png_encode",
  "png_decode",
  "zlib_begin",
  "zlib_push",
  "zlib_abort",
  "generic_zlib_version",
];
const legacyObjects = [];
for (const [name, path] of [
  ["generic_zlib", join(root, "src/generic_zlib.c")],
  ...["adler32", "crc32", "deflate", "trees", "zutil"].map((n) => [n, join(legacy, n + ".c")]),
]) {
  const object = join(root, "dist/legacy-" + name + ".o");
  execFileSync(
    emcc,
    [
      "-std=c11",
      "-Oz",
      "-flto",
      "-DZ_PREFIX",
      "-Dz_errmsg=legacy_z_errmsg",
      "-Wno-deprecated-non-prototype",
      "-I" + legacy,
      "-c",
      path,
      "-o",
      object,
    ],
    { stdio: "inherit" },
  );
  legacyObjects.push(object);
}
execFileSync(
  emcc,
  [
    "-std=c11",
    "-Oz",
    "-flto",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-I" + zlib,
    join(root, "src/png_codec.c"),
    ...legacyObjects,
    ...["adler32", "crc32", "deflate", "trees", "zutil", "inflate", "inftrees", "inffast"].map(
      (n) => join(zlib, n + ".c"),
    ),
    "--no-entry",
    "-sSTANDALONE_WASM=1",
    "-sFILESYSTEM=0",
    "-sMALLOC=emmalloc",
    "-sABORTING_MALLOC=0",
    "-sALLOW_MEMORY_GROWTH=0",
    "-sINITIAL_MEMORY=67108864",
    "-sSTACK_SIZE=65536",
    `-sEXPORTED_FUNCTIONS=${JSON.stringify(exports.map((n) => "_" + n))}`,
    "-o",
    join(root, "dist/png-codec.wasm"),
  ],
  { stdio: "inherit" },
);
const bytes = readFileSync(join(root, "dist/png-codec.wasm"));
const module = await WebAssembly.compile(bytes);
const imports = WebAssembly.Module.imports(module);
if (imports.length) throw new Error("Unexpected codec imports: " + JSON.stringify(imports));
cpSync(join(root, "reference/LICENSE"), join(root, "dist/LICENSE-Pillow"));
cpSync(join(root, "reference/LICENSE-zlib"), join(root, "dist/LICENSE-zlib"));
cpSync(join(root, "reference/LICENSE-zlib-legacy"), join(root, "dist/LICENSE-zlib-legacy"));
const report = {
  zlib: "1.3.1",
  genericZlib: "1.2.13",
  emsdk: "4.0.20",
  bytes: bytes.length,
  gzipBytes: gzipSync(bytes, { level: 9 }).length,
  sha256: hash(bytes),
  hostImports: 0,
  memoryBytes: 67108864,
};
writeFileSync(join(root, "dist/build-report.json"), JSON.stringify(report, null, 2) + "\n");
console.log(JSON.stringify(report));
