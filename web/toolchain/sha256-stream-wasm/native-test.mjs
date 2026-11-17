// 以已有本机编译器执行固定源的合成散列测试；临时产物只保留到测试结束。
// Run synthetic pinned-source hash checks with an installed native compiler; temporary outputs are removed afterward.
import process from "node:process";
import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { resolve } from "node:path";
import { directory, verifySource, sha256 } from "./verify-source.mjs";
const args = process.argv.slice(2);
if (args.length !== 2 || args[0] !== "--source")
  throw new Error("Usage: node native-test.mjs --source /explicit/hactool-checkout");
const { root, pin } = verifySource(args[1]);
const compiler = process.env.CC || "cc";
const version = execFileSync(compiler, ["--version"], { encoding: "utf8" }).trim();
const temporary = mkdtempSync(resolve(tmpdir(), "d2r-sha256-native-"));
try {
  const output = resolve(temporary, "sha256.so");
  execFileSync(
    compiler,
    [
      "-std=c11",
      "-O2",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-shared",
      "-fPIC",
      `-I${resolve(root, "mbedtls/include")}`,
      `-I${directory}`,
      '-DMBEDTLS_CONFIG_FILE="sha256_config.h"',
      resolve(directory, "bridge.c"),
      resolve(root, "mbedtls/library/sha256.c"),
      "-o",
      output,
    ],
    { stdio: "inherit" },
  );
  const result = JSON.parse(
    execFileSync("python3", [resolve(directory, "native-check.py"), output], {
      encoding: "utf8",
      timeout: 30000,
    }),
  );
  console.log(
    JSON.stringify(
      {
        ...result,
        compilerVersion: version,
        source: pin,
        bridgeSha256: sha256(readFileSync(resolve(directory, "bridge.c"))),
        binarySha256: sha256(readFileSync(output)),
        wasmExecuted: false,
      },
      null,
      2,
    ),
  );
} finally {
  rmSync(temporary, { recursive: true, force: true });
}
