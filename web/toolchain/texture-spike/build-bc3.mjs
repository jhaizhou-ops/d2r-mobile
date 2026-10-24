// 构建 BC3 纹理编解码模块；依据 web/toolchain/texture-spike 的模块与调用约定。
// Build the BC3 texture codec module using the module and calling contracts in web/toolchain/texture-spike.
import { execFileSync } from "node:child_process";
import { mkdirSync, copyFileSync, statSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { join } from "node:path";
import { createHash } from "node:crypto";
const root = fileURLToPath(new URL(".", import.meta.url));
const crate = join(root, "rust-bc3");
const source = readFileSync(new URL("../../../dm/ios/tools/gen_widestash_art.py", import.meta.url));
if (
  createHash("sha256").update(source).digest("hex") !==
  "f58435e650cc12e1472eb229a7eb5d4e08f7477f37f5d91376df5ad5af952c79"
)
  throw new Error("Original BC3 source changed; review Rust encoder parity before rebuilding");
const rustEnv = { ...process.env, RUSTUP_TOOLCHAIN: "1.99.1" };
execFileSync("cargo", ["fmt", "--", "--check"], { cwd: crate, stdio: "inherit", env: rustEnv });
execFileSync(
  "cargo",
  ["clippy", "--release", "--target", "wasm32-unknown-unknown", "--", "-D", "warnings"],
  { cwd: crate, stdio: "inherit", env: rustEnv },
);
execFileSync("cargo", ["build", "--release", "--locked", "--target", "wasm32-unknown-unknown"], {
  cwd: crate,
  stdio: "inherit",
  env: { ...rustEnv, RUSTFLAGS: "-C link-arg=--max-memory=33554432 -C link-arg=--stack-first" },
});
mkdirSync(join(root, "dist"), { recursive: true });
const target = join(root, "dist", "bc3.wasm");
copyFileSync(join(crate, "target/wasm32-unknown-unknown/release/d2r_bc3.wasm"), target);
console.log(`Rust BC3 encoder: ${statSync(target).size} bytes`);
