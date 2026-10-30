// 构建精确字体栅格化 WASM 模块；依据 web/toolchain/font-rasterizer-exact 的模块与调用约定。
// Build the exact font-rasterization WASM module using the module and calling contracts in web/toolchain/font-rasterizer-exact.
import process from "node:process";
import { Buffer } from "node:buffer";
// 仅用于构建：复用现有 CI 的 emsdk 4.0.20，不新增 CI 作业。
// Build-only: reuse the existing CI emsdk 4.0.20, never add a CI job.
import {
  readFileSync,
  writeFileSync,
  mkdirSync,
  existsSync,
  copyFileSync,
  mkdtempSync,
  renameSync,
  rmSync,
} from "node:fs";
import { fileURLToPath } from "node:url";
import { join } from "node:path";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { gzipSync } from "node:zlib";
const root = fileURLToPath(new URL(".", import.meta.url));
const pin = JSON.parse(readFileSync(join(root, "freetype-source.json")));
const hash = (bytes) => createHash("sha256").update(bytes).digest("hex");
const vendor = join(root, "vendor"),
  dist = join(root, "dist");
mkdirSync(vendor, { recursive: true });
mkdirSync(dist, { recursive: true });
const archive = join(vendor, "freetype.zip");
const validArchive = (bytes) =>
  bytes.length === pin.bytes &&
  hash(bytes) === pin.sha256 &&
  createHash("sha512").update(bytes).digest("hex") === pin.sha512;
// 缓存中断或过期时重新获取固定来源；只有完整验证的字节才能原子替换缓存。
// Refetch interrupted or stale caches from the pinned source; only fully verified bytes replace the cache atomically.
if (!existsSync(archive) || !validArchive(readFileSync(archive))) {
  const response = await fetch(pin.url);
  if (!response.ok) throw new Error(`FreeType source fetch ${response.status}`);
  const chunks = [];
  let size = 0;
  for await (const chunk of response.body) {
    size += chunk.length;
    if (size > pin.bytes) throw new Error("Oversized FreeType source");
    chunks.push(chunk);
  }
  const bytes = Buffer.concat(chunks);
  if (!validArchive(bytes)) throw new Error("FreeType source mismatch");
  const staging = mkdtempSync(join(vendor, ".freetype-"));
  try {
    const pending = join(staging, "freetype.zip");
    writeFileSync(pending, bytes);
    renameSync(pending, archive);
  } finally {
    try {
      rmSync(staging, { recursive: true, force: true });
    } catch {
      // 临时缓存清理失败不得掩盖原始写入或重命名错误。
      // Temporary cache cleanup failure must not mask the original write or rename error.
    }
  }
}
const config = readFileSync(join(root, "reference/ftconfig.h"));
if (hash(config) !== pin.configSha256) throw new Error("FreeType port configuration mismatch");
const names = execFileSync("unzip", ["-Z1", archive], { encoding: "utf8" }).trim().split("\n");
if (
  names.some(
    (name) =>
      !name.startsWith(pin.root + "/") || name.split("/").includes("..") || name.includes("\\"),
  )
)
  throw new Error("Unsafe pinned archive paths");
execFileSync("unzip", ["-oq", archive, "-d", vendor]);
const source = join(vendor, pin.root);
writeFileSync(join(source, "include/ftconfig.h"), config);
// 流标志仅移除文件 I/O；所有光栅化模块、微调、整数运算与默认值
// 均沿用固定版本 3.1.58 移植的原始列表。
// The stream flag removes file I/O only. All rasterizer modules, hinting,
// integer arithmetic and defaults are the pinned 3.1.58 port's unchanged list.
const common = [
  "-std=c11",
  "-O2",
  "-DFT2_BUILD_LIBRARY",
  "-DFT_CONFIG_OPTION_DISABLE_STREAM_SUPPORT",
  "-I" + join(source, "include"),
];
const files = [
  join(root, "src/font_rasterizer.c"),
  ...pin.sources.map((path) => join(source, path)),
];
if (process.argv.includes("--native")) {
  execFileSync(
    process.env.CC || "cc",
    [...common, "-shared", "-fPIC", ...files, "-lm", "-o", join(dist, "font-rasterizer.so")],
    { stdio: "inherit" },
  );
  console.log(
    "Built native semantic-check library; NOT a WASM artifact or production parity proof.",
  );
} else {
  const emcc = process.env.EMCC || "emcc";
  const version = execFileSync(emcc, ["--version"], { encoding: "utf8" });
  if (!/^emcc .*4\.0\.20\b/m.test(version)) throw new Error("Expected existing emsdk 4.0.20");
  const exports = [
    "font_input",
    "font_text",
    "font_mask",
    "font_canvas",
    "font_info",
    "font_error",
    "font_ft_error",
    "font_version",
    "font_open",
    "font_close",
    "font_measure",
    "font_render",
    "font_draw",
  ];
  execFileSync(
    emcc,
    [
      ...common,
      "-Oz",
      "-flto",
      "-fwasm-exceptions",
      ...files,
      "--no-entry",
      "-sSTANDALONE_WASM=1",
      "-sFILESYSTEM=0",
      "-sMALLOC=emmalloc",
      "-sABORTING_MALLOC=0",
      "-sSUPPORT_LONGJMP=wasm",
      "-sALLOW_MEMORY_GROWTH=0",
      "-sINITIAL_MEMORY=67108864",
      "-sSTACK_SIZE=262144",
      `-sEXPORTED_FUNCTIONS=${JSON.stringify(exports.map((name) => "_" + name))}`,
      "-o",
      join(dist, "font-rasterizer.wasm"),
    ],
    { stdio: "inherit" },
  );
  const wasm = readFileSync(join(dist, "font-rasterizer.wasm"));
  const module = await WebAssembly.compile(wasm);
  const imports = WebAssembly.Module.imports(module);
  if (imports.length)
    throw new Error("Unexpected font module host imports: " + JSON.stringify(imports));
  const report = {
    pillow: "10.2.0",
    freetype: "2.6.0",
    layout: "BASIC",
    emsdk: "4.0.20",
    bytes: wasm.length,
    gzipBytes: gzipSync(wasm, { level: 9 }).length,
    sha256: hash(wasm),
    hostImports: 0,
    memoryBytes: 67108864,
  };
  writeFileSync(join(dist, "build-report.json"), JSON.stringify(report, null, 2) + "\n");
  console.log(JSON.stringify(report));
}
for (const name of [
  "LICENSE",
  "LICENSE-FreeType",
  "LICENSE-FreeType-selection",
  "LICENSE-bdf",
  "LICENSE-pcf",
  "LICENSE-FreeType-zlib",
])
  copyFileSync(join(root, "reference", name), join(dist, name));
