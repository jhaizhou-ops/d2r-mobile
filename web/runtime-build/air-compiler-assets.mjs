// 构建时导入经认证的发布版本，不在本地构建编译器或重贴来源标签。
// Build-time authenticated release import. No local compiler build or provenance relabelling.
import process from "node:process";
import { createHash } from "node:crypto";
import { writeFile, readdir, lstat, mkdir, mkdtemp, rm, rename } from "node:fs/promises";
import { spawnSync } from "node:child_process";
import { resolve, join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { isDeepStrictEqual } from "node:util";
import {
  readFreshAirReleaseMetadata,
  readFreshAirReleaseAsset,
  loadFreshAirRelease,
  verifyFreshAirRuntimeFiles,
} from "./air-release-loader.mjs";
import { airReleaseFileMaximum } from "./air-release-contract.mjs";
import { boundedReleaseRead } from "./air-release-transport.mjs";
import {
  airCompilerReceiptOutput,
  validateAirCompilerBuildReceipt,
} from "../air-compiler-runtime-contract.mjs";
const compiler = fileURLToPath(new URL("../toolchain/metal-air-compiler/", import.meta.url));
const cache = join(compiler, "dist");
const cacheAnchor = fileURLToPath(new URL(".build/air-compiler-receipt.sha256", import.meta.url));
const hash = (bytes) => createHash("sha256").update(bytes).digest("hex");
const localName = (name) =>
  ["air_wasm.mjs", "air_wasm.wasm"].includes(name) ? `d2r_${name}` : name;
async function inventory(directory, prefix = "", expectedNames) {
  const info = await lstat(directory);
  if (!info.isDirectory() || info.isSymbolicLink())
    throw Error("AIR compiler cache must be a regular directory");
  const paths = [];
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    const path = prefix + entry.name;
    if (entry.isSymbolicLink()) throw Error("AIR compiler cache cannot contain symbolic links");
    if (entry.isDirectory()) {
      if (!expectedNames.some((name) => name.startsWith(path + "/")))
        throw Error("AIR compiler cache contains an unlisted directory");
      paths.push(...(await inventory(join(directory, entry.name), path + "/", expectedNames)));
    } else if (entry.isFile()) paths.push(path);
    else throw Error("AIR compiler cache cannot contain special files");
  }
  return paths.sort();
}
export async function verifyAirCompilerAssets(directory = cache, expectedDigest) {
  const metadata = await readFreshAirReleaseMetadata();
  if (expectedDigest === undefined) {
    if (resolve(directory) !== resolve(cache))
      throw Error("External AIR compiler assets require a trusted receipt SHA-256");
    expectedDigest = (await boundedReleaseRead(cacheAnchor, 128)).toString("utf8").trim();
  }
  if (
    !/^[a-f0-9]{64}$/.test(expectedDigest) ||
    expectedDigest !== metadata.release.buildReceiptSha256
  )
    throw Error("AIR compiler receipt differs from pinned release");
  const expectedNames = Object.keys(metadata.release.runtimeFiles).map(localName).sort();
  if (!isDeepStrictEqual(await inventory(directory, "", expectedNames), expectedNames))
    throw Error("AIR compiler cache contains missing or unlisted files");
  const files = new Map();
  for (const [name, record] of Object.entries(metadata.release.runtimeFiles)) {
    const bytes = await boundedReleaseRead(
      join(directory, localName(name)),
      airReleaseFileMaximum(name),
    );
    if (bytes.length !== record.bytes || hash(bytes) !== record.sha256)
      throw Error(`AIR compiler output receipt mismatch: ${name}`);
    files.set(name, bytes);
  }
  const receipt = files.get("build-report.json");
  if (hash(receipt) !== expectedDigest) throw Error("AIR compiler build receipt digest mismatch");
  const { receipt: report } = verifyFreshAirRuntimeFiles(
    files,
    metadata.release,
    metadata.buildInputs,
  );
  validateAirCompilerBuildReceipt(report);
  return report;
}
export async function importAirCompilerAssets({ directory, expectedDigest }) {
  if (!directory || !/^[a-f0-9]{64}$/.test(expectedDigest ?? ""))
    throw Error("AIR compiler import requires an input directory and trusted receipt SHA-256");
  // 缺少版本锁定或源码时，在修改编译器目录、锚点或 dist 之前失败。
  // Missing pin/source fails before any compiler directory, anchor or dist mutation.
  const metadata = await readFreshAirReleaseMetadata({ download: true });
  await readFreshAirReleaseAsset(metadata, "correspondingSource", { download: true });
  await verifyAirCompilerAssets(directory, expectedDigest);
  await mkdir(compiler, { recursive: true });
  const staging = await mkdtemp(join(compiler, ".dist-"));
  try {
    for (const name of Object.keys(metadata.release.runtimeFiles)) {
      const target = join(staging, localName(name));
      await mkdir(dirname(target), { recursive: true });
      await writeFile(
        target,
        await boundedReleaseRead(join(directory, localName(name)), airReleaseFileMaximum(name)),
        { flag: "wx" },
      );
    }
    const report = await verifyAirCompilerAssets(staging, expectedDigest);
    await rm(cache, { recursive: true, force: true });
    await rename(staging, cache);
    await mkdir(dirname(cacheAnchor), { recursive: true });
    await writeFile(cacheAnchor, expectedDigest + "\n");
    return report;
  } catch (error) {
    await rm(staging, { recursive: true, force: true });
    throw error;
  }
}
export async function importPinnedAirCompiler() {
  const verified = await loadFreshAirRelease({ download: true });
  await mkdir(compiler, { recursive: true });
  const staging = await mkdtemp(join(compiler, ".release-"));
  try {
    for (const [name, bytes] of verified.files) {
      const target = join(staging, localName(name));
      await mkdir(dirname(target), { recursive: true });
      await writeFile(target, bytes, { flag: "wx" });
    }
    return await importAirCompilerAssets({
      directory: staging,
      expectedDigest: verified.release.buildReceiptSha256,
    });
  } finally {
    await rm(staging, { recursive: true, force: true });
  }
}
export async function prepareAirCompilerAssets({
  directory = process.env.D2R_AIR_COMPILER_ASSET_DIR,
  expectedDigest = process.env.D2R_AIR_COMPILER_RECEIPT_SHA256,
} = {}) {
  if (directory !== undefined || expectedDigest !== undefined)
    return importAirCompilerAssets({ directory, expectedDigest });
  return verifyAirCompilerAssets();
}
export async function airCompilerProvenanceAssets(report) {
  const { release } = await readFreshAirReleaseMetadata();
  if (report.sourceFingerprintSha256 !== release.sourceFingerprintSha256)
    throw Error("AIR provenance release differs");
  return Object.entries(release.runtimeFiles)
    .filter(([name]) => name === "source-lock.json" || name.startsWith("licenses/"))
    .map(([name, record]) => ({ path: `toolchain/metal-air-compiler/dist/${name}`, ...record }));
}
async function probeAirCompiler(buildDirectory, expectedHeaderSha256, compileCanary = false) {
  const probe = spawnSync(
    process.execPath,
    [
      "--input-type=module",
      "--eval",
      `import {readFile} from 'node:fs/promises';
       import {pathToFileURL} from 'node:url';
       import {resolve} from 'node:path';
       globalThis.self=globalThis;
       globalThis.location={href:'https://invalid.test/air-receipt-worker.mjs'};
       globalThis.fetch=globalThis.importScripts=()=>{throw Error('AIR receipt probe network forbidden')};
       const root=process.argv[1];
       const compiled=await WebAssembly.compile(await readFile(resolve(root,'d2r_air_wasm.wasm')));
       const factory=(await import(pathToFileURL(resolve(root,'d2r_air_wasm.mjs')).href)).default;
       const module=await factory({print(){},printErr(){},locateFile(){throw Error('Unexpected runtime URL')},instantiateWasm(imports,receive){const instance=new WebAssembly.Instance(compiled,imports);receive(instance,compiled);return instance.exports;}});
       for(const name of ['malloc','free','air_compile_v2','air_result_status','air_result_data','air_result_size','air_result_free','air_result_diagnostics','air_result_diagnostics_size','air_compiler_header_sha256','air_compiler_header_bundle_version']) if(typeof module['_'+name]!=='function') throw Error('Missing AIR export '+name);
       const pointer=module._air_compiler_header_sha256();
       if(!Number.isSafeInteger(pointer)||pointer<=0||pointer+65>module.HEAPU8.length||module.HEAPU8[pointer+64]!==0) throw Error('Invalid AIR header identity');
       const headerSha256=new TextDecoder().decode(module.HEAPU8.subarray(pointer,pointer+64));
       const headerBundleVersion=module._air_compiler_header_bundle_version();
       if(headerSha256!==process.argv[2]||headerBundleVersion!==2) throw Error('Built AIR compiler header identity mismatch');
       const result={headerSha256,headerBundleVersion};
       if(process.argv[3]==='1') result.canary=await (await import(process.argv[4])).runAirCompilerCanary(module,{expectedHeaderSha256:headerSha256});
       console.log(JSON.stringify(result));`,
      resolve(buildDirectory),
      expectedHeaderSha256,
      compileCanary ? "1" : "0",
      new URL("../toolchain/metal-air-compiler/ci-compile-canary.mjs", import.meta.url).href,
    ],
    { encoding: "utf8", timeout: 120000, maxBuffer: 1024 * 1024 },
  );
  if (probe.error || probe.status !== 0)
    throw new Error(`AIR compiler identity probe failed: ${probe.error?.message ?? probe.stderr}`);
  const runtime = JSON.parse(probe.stdout.trim());
  if (runtime.headerSha256 !== expectedHeaderSha256 || runtime.headerBundleVersion !== 2)
    throw new Error("Built AIR compiler does not match the authored header bundle");
  return runtime;
}

// 每次 CI 都检查已认证的实际输出，包括缓存命中；不改写信任凭据、不重编 LLVM。
// Probe authenticated actual output on every CI run, including cache hits; never rewrite receipts or rebuild LLVM for this check.
export async function verifyAirCompilerCanary() {
  const report = await verifyAirCompilerAssets();
  const runtime = await probeAirCompiler(cache, report.headerSha256, true);
  if (!isDeepStrictEqual(await verifyAirCompilerAssets(), report))
    throw new Error("AIR compiler cache changed during compile canary");
  return {
    ...runtime.canary,
    sourceFingerprintSha256: report.sourceFingerprintSha256,
    headerSha256: report.headerSha256,
    factorySha256: airCompilerReceiptOutput(report, "d2r_air_wasm.mjs").sha256,
    wasmSha256: airCompilerReceiptOutput(report, "d2r_air_wasm.wasm").sha256,
    browserExecutionVerified: false,
    fullGameCompilationVerified: false,
    deviceExecutionVerified: false,
  };
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const [command, ...args] = process.argv.slice(2);
  const option = (name) => {
    const index = args.indexOf(name);
    return index < 0 ? undefined : args[index + 1];
  };
  if (command === "import")
    console.log(
      JSON.stringify(
        await importAirCompilerAssets({
          directory: option("--input"),
          expectedDigest: option("--sha256"),
        }),
      ),
    );
  else if (command === "verify") {
    const report = await prepareAirCompilerAssets();
    const expected = option("--source-fingerprint");
    if (expected !== undefined && expected !== report.sourceFingerprintSha256)
      throw Error("AIR compiler source differs from CI cache key");
    console.log(
      JSON.stringify({ verified: true, sourceFingerprintSha256: report.sourceFingerprintSha256 }),
    );
  } else
    throw Error(
      "Usage: air-compiler-assets.mjs import --input DIR --sha256 DIGEST | verify [--source-fingerprint DIGEST]",
    );
}
