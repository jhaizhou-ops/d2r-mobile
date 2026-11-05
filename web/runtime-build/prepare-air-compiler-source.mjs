// 镜像经过认证的完整 fork 源码及已安装 SDK 运行时源码；此处不重建输入、不打补丁、不重新编译，也不执行这些输入。
// Mirror the exact authenticated full fork source and installed SDK runtime sources.
// Never reconstruct, patch, rebuild or execute compiler inputs here.
import { Buffer } from "node:buffer";
import process from "node:process";
import { createHash } from "node:crypto";
import { isDeepStrictEqual } from "node:util";
import { appendFile, lstat, mkdir, readFile, writeFile, rm } from "node:fs/promises";
import { spawnSync } from "node:child_process";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { readFreshAirReleaseMetadata, readFreshAirReleaseAsset } from "./air-release-loader.mjs";
import { verifyFreshAirReceipt } from "./air-release-contract.mjs";
import { boundedReleaseRead } from "./air-release-transport.mjs";
import { verifyAirCompilerAssets } from "./air-compiler-assets.mjs";
const root = fileURLToPath(new URL("../../", import.meta.url));
const args = process.argv.slice(2);
if (args.length !== 2 || args[0] !== "--output" || args[1].startsWith("--"))
  throw new Error("Usage: prepare-air-compiler-source.mjs --output DIRECTORY");
const output = resolve(args[1]);
try {
  await lstat(output);
  throw new Error("Source artifact output must not already exist");
} catch (error) {
  if (error.code !== "ENOENT") throw error;
}
const metadata = await readFreshAirReleaseMetadata({ download: true });
const compiler = await verifyAirCompilerAssets();
const source = await readFreshAirReleaseAsset(metadata, "correspondingSource", { download: true });
const receipt = await boundedReleaseRead(
  join(root, "web/toolchain/metal-air-compiler/dist/build-report.json"),
  1024 ** 2,
);
if (
  !isDeepStrictEqual(
    verifyFreshAirReceipt(receipt, metadata.release, metadata.buildInputs),
    compiler,
  )
)
  throw new Error("Source mirror receipt differs from authenticated compiler");
const sha = (bytes) => createHash("sha256").update(bytes).digest("hex");
const revision = spawnSync("git", ["rev-parse", "HEAD"], {
  cwd: root,
  encoding: "utf8",
  timeout: 10000,
});
if (revision.error || revision.status !== 0 || !/^[a-f0-9]{40}$/.test(revision.stdout.trim()))
  throw new Error("Cannot determine source artifact consumer revision");
await mkdir(dirname(output), { recursive: true });
await mkdir(output);
try {
  const records = [];
  async function put(path, bytes) {
    await writeFile(join(output, path), bytes, { flag: "wx" });
    const actual = await readFile(join(output, path));
    if (actual.length !== bytes.length || sha(actual) !== sha(bytes))
      throw new Error("Source artifact changed while copying");
    records.push({ path, bytes: bytes.length, sha256: sha(bytes) });
  }
  await put(metadata.release.assets.correspondingSource.name, source.bytes);
  await put("compiler-build-report.json", receipt);
  await put("release-manifest.json", metadata.manifestBytes);
  await put("build-inputs.json", metadata.buildInputsBytes);
  await put(
    "source-lock.json",
    await boundedReleaseRead(
      join(root, "web/toolchain/metal-air-compiler/dist/source-lock.json"),
      1024 ** 2,
    ),
  );
  await put("release-pin.json", Buffer.from(JSON.stringify(metadata.pin, null, 2) + "\n"));
  await put(
    "BUILD_FROM_SOURCE.txt",
    Buffer.from(
      `Complete corresponding source for AIR ${metadata.pin.version}\n\nThe included ${metadata.release.assets.correspondingSource.name} is the exact release source asset (SHA-256 ${metadata.release.assets.correspondingSource.sha256}). It includes the complete fork Git commit ${metadata.pin.commit}, artifact-only licensing notices and the installed Emscripten runtime source snapshot described by build-inputs.json. The archive preserves the producer's directory layout. Follow wasm/README.md and the pinned release workflow in that source tree to build.\n\nThe compiler was freshly built from https://github.com/${metadata.pin.repository}/tree/${metadata.pin.commit}. build-inputs.json and the unchanged compiler-build-report.json disclose the source, tooling and validation scope. release-manifest.json and release-pin.json bind every mirrored byte to the exact release. They do not claim bit-identical rebuilds or full application/device/GPU correctness.\n\nPreserve all original file-specific licenses and notices. This mirror step only verified/copied bytes; it did not compile LLVM or execute the compiler.\n`,
    ),
  );
  const report = {
    version: 2,
    kind: "pinned-release-source-mirror",
    sourceRevision: revision.stdout.trim(),
    sourceFingerprintSha256: compiler.sourceFingerprintSha256,
    sourceLanguage: compiler.sourceLanguage,
    headerSha256: compiler.headerSha256,
    completeReleaseSourceVerified: true,
    installedSdkRuntimeSourcesIncluded: true,
    llvmCompiledByThisStep: false,
    binaryBuiltFromNewFork: true,
    release: metadata.pin,
    correspondingSource: metadata.release.assets.correspondingSource,
    files: records.sort((a, b) => a.path.localeCompare(b.path)),
  };
  const bytes = Buffer.from(JSON.stringify(report, null, 2) + "\n"),
    digest = sha(bytes);
  await writeFile(join(output, "source-report.json"), bytes, { flag: "wx" });
  if (process.env.GITHUB_OUTPUT)
    await appendFile(process.env.GITHUB_OUTPUT, `source_report_sha256=${digest}\n`);
  console.log(
    JSON.stringify({
      directory: output,
      sourceReportSha256: digest,
      sourceFingerprintSha256: compiler.sourceFingerprintSha256,
      release: metadata.pin,
      llvmCompiled: false,
    }),
  );
} catch (error) {
  await rm(output, { recursive: true, force: true });
  throw error;
}
