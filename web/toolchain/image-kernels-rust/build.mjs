// 构建 Rust 图像处理内核的 WASM 模块；依据 web/toolchain/image-kernels-rust 的模块与调用约定。
// Build the Rust image-kernel WASM module using the module and calling contracts in web/toolchain/image-kernels-rust.
import process from "node:process";
import { execFileSync } from "node:child_process";
import { mkdirSync, copyFileSync, readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { createHash } from "node:crypto";
import { gzipSync } from "node:zlib";
const cwd = fileURLToPath(new URL(".", import.meta.url));
const env = { ...process.env, RUSTUP_TOOLCHAIN: "1.99.1" };
const run = (args, extra = {}) =>
  execFileSync("cargo", args, { cwd, env: { ...env, ...extra }, stdio: "inherit" });
run(["fmt", "--", "--check"]);
run(["clippy", "--release", "--target", "wasm32-unknown-unknown", "--", "-D", "warnings"]);
run(["build", "--release", "--locked", "--target", "wasm32-unknown-unknown"], {
  RUSTFLAGS: "-C link-arg=--max-memory=67108864 -C link-arg=--stack-first",
});
mkdirSync(new URL("dist/", import.meta.url), { recursive: true });
const bytes = readFileSync(
  new URL("target/wasm32-unknown-unknown/release/d2r_image_kernels.wasm", import.meta.url),
);
const module = await WebAssembly.compile(bytes);
if (WebAssembly.Module.imports(module).length) throw new Error("Unexpected host imports");
copyFileSync(
  new URL("target/wasm32-unknown-unknown/release/d2r_image_kernels.wasm", import.meta.url),
  new URL("dist/image-kernels.wasm", import.meta.url),
);
const report = {
  bytes: bytes.length,
  gzipBytes: gzipSync(bytes, { level: 9 }).length,
  sha256: createHash("sha256").update(bytes).digest("hex"),
  dependencies: 0,
  hostImports: 0,
  maxMemoryBytes: 67_108_864,
};
writeFileSync(
  new URL("dist/build-report.json", import.meta.url),
  JSON.stringify(report, null, 2) + "\n",
);
console.log(JSON.stringify(report));
