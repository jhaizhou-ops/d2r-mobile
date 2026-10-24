// 仅保留失败 CI 的已认证公开编译器；不执行编译器、不发布成功预览。
// Retain authenticated public compiler evidence from failed CI, without execution or a passing preview.
import process from "node:process";
import { Buffer } from "node:buffer";
import { createHash } from "node:crypto";
import { readFile, writeFile, lstat, mkdir, rm } from "node:fs/promises";
import { isDeepStrictEqual } from "node:util";
import { resolve, join } from "node:path";
import { fileURLToPath } from "node:url";
import { verifyAirCompilerAssets } from "./air-compiler-assets.mjs";
import {
  AIR_COMPILER_OUTPUT_NAMES,
  airCompilerReceiptOutput,
  isFreshAirCompilerReceipt,
  airCompilerOutputMaximumBytes,
} from "../air-compiler-runtime-contract.mjs";

const cache = fileURLToPath(new URL("../toolchain/metal-air-compiler/dist/", import.meta.url));
const anchor = fileURLToPath(new URL(".build/air-compiler-receipt.sha256", import.meta.url));
const sha = (bytes) => createHash("sha256").update(bytes).digest("hex");
async function boundedRead(path, maximum) {
  const info = await lstat(path);
  if (!info.isFile() || info.isSymbolicLink() || info.size < 1 || info.size > maximum)
    throw new Error("Invalid bounded diagnostic compiler input");
  const bytes = await readFile(path);
  if (bytes.length !== info.size) throw new Error("Diagnostic compiler input changed");
  return bytes;
}

export function validateDiagnosticSource(
  compiler,
  receipt,
  sourceBytes,
  { sourceReportSha256, sourceFingerprintSha256, sourceRevision, releaseManifestSha256 },
) {
  if (
    !/^[a-f0-9]{64}$/.test(sourceReportSha256 ?? "") ||
    !/^[a-f0-9]{64}$/.test(sourceFingerprintSha256 ?? "") ||
    !/^[a-f0-9]{40}$/.test(sourceRevision ?? "") ||
    sha(sourceBytes) !== sourceReportSha256 ||
    compiler.sourceFingerprintSha256 !== sourceFingerprintSha256 ||
    !isDeepStrictEqual(JSON.parse(receipt), compiler)
  )
    throw new Error("Diagnostic compiler identity or source-report anchor mismatch");
  const source = JSON.parse(sourceBytes);
  const sourceReceipt = source.files?.find((entry) => entry.path === "compiler-build-report.json");
  if (
    (isFreshAirCompilerReceipt(compiler)
      ? source.version !== 2 ||
        source.kind !== "pinned-release-source-mirror" ||
        source.completeReleaseSourceVerified !== true ||
        source.installedSdkRuntimeSourcesIncluded !== true ||
        source.binaryBuiltFromNewFork !== true ||
        source.release?.repository !== compiler.repository ||
        source.release.commit !== compiler.commit ||
        source.release.version !== compiler.version ||
        !/^[a-f0-9]{64}$/.test(releaseManifestSha256 ?? "") ||
        source.release.manifestSha256 !== releaseManifestSha256 ||
        source.files?.find((entry) => entry.path === "source-lock.json")?.sha256 !==
          compiler.sourceLockSha256 ||
        source.correspondingSource?.name !== `air-wasm-${compiler.version}-source.tar.gz` ||
        source.files?.find((entry) => entry.path === source.correspondingSource.name)?.sha256 !==
          source.correspondingSource.sha256
      : source.version !== 1 || source.reconstructedPatchedSourceVerified !== true) ||
    source.sourceRevision !== sourceRevision ||
    source.sourceFingerprintSha256 !== sourceFingerprintSha256 ||
    source.headerSha256 !== compiler.headerSha256 ||
    source.sourceLanguage !== compiler.sourceLanguage ||
    source.llvmCompiledByThisStep !== false ||
    sourceReceipt?.sha256 !== sha(receipt) ||
    sourceReceipt?.bytes !== receipt.length
  )
    throw new Error("Diagnostic compiler does not match complete corresponding source");
  return source;
}

async function prepare(output, sourcePath, environment = process.env) {
  const compiler = await verifyAirCompilerAssets();
  const receipt = await boundedRead(join(cache, "build-report.json"), 1024 * 1024);
  const sourceBytes = await boundedRead(sourcePath, 1024 * 1024);
  const sourceReportSha256 = environment.D2R_AIR_COMPILER_SOURCE_REPORT_SHA256;
  const source = validateDiagnosticSource(compiler, receipt, sourceBytes, {
    sourceReportSha256,
    sourceFingerprintSha256: environment.D2R_AIR_COMPILER_SOURCE_FINGERPRINT,
    sourceRevision: environment.GITHUB_SHA,
    releaseManifestSha256: environment.D2R_AIR_COMPILER_RELEASE_MANIFEST_SHA256,
  });
  const sourceArtifactId = environment.D2R_AIR_COMPILER_SOURCE_ARTIFACT_ID;
  if (!/^[1-9][0-9]*$/.test(sourceArtifactId ?? ""))
    throw new Error("Matching diagnostic source must be uploaded before the compiler");
  await mkdir(output);
  try {
    const files = [];
    async function put(path, bytes) {
      await writeFile(join(output, path), bytes, { flag: "wx" });
      files.push({ path, bytes: bytes.length, sha256: sha(bytes) });
    }
    for (const name of AIR_COMPILER_OUTPUT_NAMES) {
      const bytes = await boundedRead(join(cache, name), airCompilerOutputMaximumBytes(name));
      if (
        bytes.length !== airCompilerReceiptOutput(compiler, name).bytes ||
        sha(bytes) !== airCompilerReceiptOutput(compiler, name).sha256
      )
        throw new Error("Diagnostic compiler output receipt mismatch");
      await put(name, bytes);
    }
    await put("build-report.json", receipt);
    const trust = await boundedRead(anchor, 128);
    if (trust.toString("utf8").trim() !== sha(receipt))
      throw new Error("Diagnostic compiler receipt trust anchor changed");
    await put("air-compiler-receipt.sha256", trust);
    await put(
      "READ_ME.txt",
      Buffer.from(
        "UNVALIDATED COMPILER DIAGNOSTIC / 未通过验证的编译器诊断产物\n\n" +
          "The producing CI runtime step failed. This is not a usable preview or a passing canary.\n" +
          "Only source identity, build receipt and output hashes were verified for retention.\n" +
          "Complete matching source is in air-compiler-diagnostic-source from the same CI run.\n" +
          "Preserve its licenses and source together with these binaries. No game inputs are included.\n" +
          "CI 运行时步骤失败；本产物不是成功预览，也不表示编译探针通过。\n" +
          "仅验证源码身份、构建凭据和文件摘要；完整对应源码在同次 CI 的独立诊断源码产物中。\n",
      ),
    );
    if (!isDeepStrictEqual(await verifyAirCompilerAssets(), compiler))
      throw new Error("Compiler cache changed during diagnostic retention");
    const report = {
      version: 1,
      purpose: "failed-ci-compiler-diagnostic",
      previewValidated: false,
      compilerCanaryStatus: "not-certified-by-this-artifact",
      sourceRevision: source.sourceRevision,
      sourceFingerprintSha256: compiler.sourceFingerprintSha256,
      headerSha256: compiler.headerSha256,
      receiptSha256: sha(receipt),
      sourceArtifact: {
        name: "air-compiler-diagnostic-source",
        id: sourceArtifactId,
        sourceReportSha256,
      },
      files,
    };
    const bytes = Buffer.from(JSON.stringify(report, null, 2) + "\n");
    await writeFile(join(output, "diagnostic-report.json"), bytes, { flag: "wx" });
    console.log(JSON.stringify({ ...report, diagnosticReportSha256: sha(bytes) }));
  } catch (error) {
    await rm(output, { recursive: true, force: true });
    throw error;
  }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const [output, sourcePath] = process.argv.slice(2);
  if (!output || !sourcePath || process.argv.length !== 4)
    throw new Error("Usage: prepare-air-compiler-diagnostic.mjs OUTPUT SOURCE_REPORT");
  await prepare(resolve(output), resolve(sourcePath));
}
