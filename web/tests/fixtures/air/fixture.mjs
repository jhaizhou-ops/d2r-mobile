// 仅为测试数据，不是编译器产物、真实发布锁定或执行证据。为真的验证标志只模拟已接受元数据，未执行冒烟测试。
// TEST DATA ONLY. No compiler artifact, real release pin, or execution evidence.
// True verification flags model accepted metadata; no smoke test was executed.
import assert from "node:assert/strict";
import { lstat, readFile, realpath } from "node:fs/promises";
import { join } from "node:path";
import process from "node:process";
import { Buffer } from "node:buffer";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";

const paths = {
  description:
    "Test-only source path shape. Every blob and identity in fixture.mjs is synthetic; this is not release evidence.",
  compilerFiles: [
    "clang/include/clang/Basic/Attr.td",
    "clang/lib/AST/ASTContext.cpp",
    "clang/lib/AST/Decl.cpp",
    "clang/lib/AST/RecordLayoutBuilder.cpp",
    "clang/lib/Basic/Targets/SPIR.h",
    "clang/lib/CodeGen/BackendUtil.cpp",
    "clang/lib/CodeGen/CGBuiltin.cpp",
    "clang/lib/CodeGen/CGCall.cpp",
    "clang/lib/CodeGen/CGExprCXX.cpp",
    "clang/lib/CodeGen/CGOpenCLRuntime.cpp",
    "clang/lib/CodeGen/CMakeLists.txt",
    "clang/lib/CodeGen/CodeGenModule.cpp",
    "clang/lib/CodeGen/CodeGenTypes.cpp",
    "clang/lib/CodeGen/CodeGenTypes.h",
    "clang/lib/Parse/ParseDecl.cpp",
    "clang/lib/Parse/ParseTentative.cpp",
    "clang/lib/Sema/SemaDecl.cpp",
    "clang/lib/Sema/SemaDeclAttr.cpp",
    "clang/lib/Sema/SemaExprCXX.cpp",
    "clang/lib/Sema/SemaInit.cpp",
    "clang/lib/Sema/SemaOverload.cpp",
    "clang/lib/Sema/SemaType.cpp",
    "llvm/include/llvm/Transforms/LibFloor/FloorImage.h",
    "llvm/lib/Bitcode/MetalLib/CMakeLists.txt",
    "llvm/lib/Bitcode/MetalLib/MetalLibReflection.cpp",
    "llvm/lib/Bitcode/MetalLib/MetalLibWriterPass.cpp",
    "llvm/lib/Transforms/LibFloor/FloorImage.cpp",
    "llvm/lib/Transforms/LibFloor/MetalFinal.cpp",
    "llvm/lib/Transforms/LibFloor/MetalImage.cpp",
    "llvm/lib/Transforms/LibFloor/PointerBCFixup.cpp",
    "llvm/lib/Transforms/LibFloor/VulkanFinal.cpp",
  ],
  sdkFiles: [
    "api/CMakeLists.txt",
    "api/air_compiler.cpp",
    "api/air_compiler.h",
    "api/atomic_trust.inc",
    "api/compat_header.inc.in",
    "api/depth_trust.inc",
    "api/include/metal_atomic",
    "api/include/metal_control",
    "api/include/metal_depth",
    "api/include/metal_integer",
    "api/include/metal_math",
    "api/include/metal_simd",
    "api/include/metal_stdlib",
    "api/include/metal_tessellation",
    "api/include/metal_texture",
    "api/include/simd/simd.h",
    "api/texture_trust.inc",
    "api/wasm_entry.cpp",
    "build.mjs",
  ],
};
export const fixtureSha256 = (value) => createHash("sha256").update(value).digest("hex");
export const fixtureJson = (value) => Buffer.from(`${JSON.stringify(value, null, 2)}\n`);
export const fixtureRecord = (bytes) => ({ bytes: bytes.length, sha256: fixtureSha256(bytes) });
const entries = (value) => Object.entries(value).sort(([a], [b]) => (a < b ? -1 : 1));
export function fixtureSourceFingerprint(report) {
  return fixtureSha256(
    `floor-air-wasm-source-v1\n${report.repository}\n${report.commit}\n${report.tree}\n${report.sourceLockSha256}\n` +
      entries(report.sourceInputs)
        .map(([path, digest]) => `${path}\0${digest}\n`)
        .join(""),
  );
}
export function fixtureHeaderDigest(lock) {
  return fixtureSha256(
    "floor-air-header-bundle-v2\n" +
      entries(lock.sdkFiles)
        .filter(([path]) => path.startsWith("api/include/"))
        .map(([path, digest]) => `${path.slice(12)}:${digest}\n`)
        .join(""),
  );
}

// 变更钩子允许拒绝测试重算外层摘要，以证明模式检查拒绝错误元数据，而非仅发现旧校验和。
// Mutation hooks let rejection tests recompute enclosing digests, proving schema
// checks reject bad metadata rather than merely noticing an old outer checksum.
export function createFreshAirReleaseFixture({
  mutateLock,
  mutateInputs,
  mutateReceipt,
  mutateManifest,
} = {}) {
  const sourceFiles = new Map();
  for (const path of [...paths.compilerFiles, ...paths.sdkFiles.map((path) => `wasm/${path}`)])
    sourceFiles.set(path, Buffer.from(`Synthetic source fixture: ${path}\n`));
  const sourceLock = {
    schemaVersion: 1,
    upstream: {
      repository: "https://github.com/example/synthetic-upstream",
      commit: "4".repeat(40),
      archiveSha256: fixtureSha256("synthetic upstream archive, not an actual archive"),
    },
    buildTool: {
      emscriptenVersion: "4.0.20",
      emsdkCommit: "e4fe26ef59168ff44f4c23c466e497bf60b3411e",
      nodeMajor: 24,
      platform: "Linux",
      byteIdenticalRebuildClaimed: false,
    },
    compilerFiles: Object.fromEntries(
      paths.compilerFiles.map((path) => [path, fixtureSha256(sourceFiles.get(path))]),
    ),
    sdkFiles: Object.fromEntries(
      paths.sdkFiles.map((path) => [path, fixtureSha256(sourceFiles.get(`wasm/${path}`))]),
    ),
  };
  mutateLock?.(sourceLock);
  const sourceLockBytes = fixtureJson(sourceLock);
  const sourceInputs = {
    ...sourceLock.compilerFiles,
    ...Object.fromEntries(
      Object.entries(sourceLock.sdkFiles).map(([path, digest]) => [`wasm/${path}`, digest]),
    ),
  };
  const identity = {
    repository: "example/synthetic-air-fixture",
    commit: "1".repeat(40),
    tree: "2".repeat(40),
    sourceLockSha256: fixtureSha256(sourceLockBytes),
    sourceInputs,
    sourceTreeClean: true,
  };
  const version = "v0.0.0-test";
  const sourceFingerprintSha256 = fixtureSourceFingerprint(identity);
  const headerSha256 = fixtureHeaderDigest(sourceLock);
  const api = {
    apiVersion: 2,
    headerBundleVersion: 2,
    sourceLanguage: "floor-metal-3.2",
    headerSha256,
  };
  const buildTool = {
    ...sourceLock.buildTool,
    jobs: 2,
    versions: {
      node: "v24.0.0-test-fixture",
      emcc: "emcc (synthetic metadata only) 4.0.20\nNo compiler executed",
      ...Object.fromEntries(
        ["cmake", "ninja", "nativeCompiler", "python", "ccache"].map((name) => [
          name,
          `synthetic ${name} version, never executed`,
        ]),
      ),
    },
    runner: { imageOS: "synthetic-ubuntu", imageVersion: "synthetic-1", os: "Linux", arch: "X64" },
  };
  const inputs = {
    schemaVersion: 1,
    version,
    ...structuredClone(identity),
    sourceFingerprintSha256,
    buildTool: structuredClone(buildTool),
    commands: [
      [
        "node",
        "wasm/build.mjs",
        "build-host-tools",
        "--work",
        "$AIR_RELEASE_WORK/compiler",
        "--jobs",
        "2",
      ],
      [
        "node",
        "wasm/build.mjs",
        "build-wasm",
        "--work",
        "$AIR_RELEASE_WORK/compiler",
        "--jobs",
        "2",
      ],
      [
        "node",
        "wasm/release/probe.mjs",
        "--build",
        "$AIR_RELEASE_WORK/compiler/api-build-wasm",
        "--header",
        headerSha256,
        "--output",
        "$AIR_RELEASE_WORK/probe.json",
      ],
    ],
    sourceDateEpoch: 1700000000,
    sourceArchivePrefix: `air-wasm-${version}-source/`,
    sdkSource: {
      runtimeSources: {
        archivePrefix: `air-wasm-${version}-sdk-runtime/`,
        fileCount: 7,
        bytes: 1000,
        inventorySha256: fixtureSha256("synthetic SDK inventory"),
      },
      repository: "https://github.com/emscripten-core/emsdk",
      commit: buildTool.emsdkCommit,
      manifestSha256: fixtureSha256("synthetic SDK manifest"),
    },
    tooling: Object.fromEntries(
      [
        ".github/workflows/release-air-wasm.yml",
        "wasm/release/github_release.py",
        "wasm/release/probe.mjs",
        "wasm/release/release-config.json",
        "wasm/release/release.py",
      ].map((path) => [path, fixtureSha256(`synthetic tooling: ${path}`)]),
    ),
  };
  mutateInputs?.(inputs);
  const inputsBytes = fixtureJson(inputs);
  const runtimeFiles = new Map([
    ["air_wasm.mjs", Buffer.from("// Inert fixture, not an executable compiler module.\n")],
    ["air_wasm.wasm", Buffer.from("Inert fixture, not a WebAssembly compiler.\n")],
  ]);
  const files = Object.fromEntries(
    [...runtimeFiles].map(([path, bytes]) => [path, fixtureRecord(bytes)]),
  );
  const receipt = {
    schemaVersion: 1,
    kind: "floor-air-wasm-build",
    version,
    ...structuredClone(identity),
    sourceFingerprintSha256,
    ...api,
    buildTool: structuredClone(buildTool),
    files: structuredClone(files),
    verification: {
      schemaVersion: 1,
      exportsVerified: true,
      headerVerified: true,
      abiVersion: 2,
      headerBundleVersion: 2,
      headerSha256,
      smoke: {
        passed: true,
        cases: [
          { name: "kernel-metallib", passed: true, status: 0, outputBytes: 16, diagnosticBytes: 0 },
          { name: "invalid-source", passed: true, status: 3, outputBytes: 0, diagnosticBytes: 16 },
          { name: "bad-options", passed: true, status: 2, outputBytes: 0, diagnosticBytes: 16 },
        ],
      },
      browserExecutionVerified: false,
      deviceExecutionVerified: false,
      gpuExecutionVerified: false,
      fullApplicationVerified: false,
    },
  };
  mutateReceipt?.(receipt);
  const receiptBytes = fixtureJson(receipt);
  runtimeFiles.set("build-report.json", receiptBytes);
  runtimeFiles.set("source-lock.json", sourceLockBytes);
  runtimeFiles.set("air_compiler.h", sourceFiles.get("wasm/api/air_compiler.h"));
  runtimeFiles.set(
    "README.txt",
    Buffer.from("Synthetic release fixture only. No compiler was built or executed.\n"),
  );
  const noticePaths = [
    "llvm/LICENSE.TXT",
    "clang/LICENSE.TXT",
    "llvm/include/llvm/Support/LICENSE.TXT",
    "llvm/include/llvm/Transforms/LibFloor/cfg/LICENSE",
    "llvm/lib/Transforms/LibFloor/cfg/LICENSE",
    "llvm/lib/Bitcode/MetalLib/bzip2/LICENSE",
    "llvm/lib/Bitcode/MetalLib/tar/LICENSE",
  ];
  const noticeOrigins = {
    schemaVersion: 1,
    repository: identity.repository,
    commit: identity.commit,
    tree: identity.tree,
    files: {},
  };
  for (const path of noticePaths) {
    const bytes = Buffer.from(`Synthetic repository notice fixture: ${path}\n`),
      name = `licenses/${path}`;
    runtimeFiles.set(name, bytes);
    noticeOrigins.files[name] = { repositoryPath: path, ...fixtureRecord(bytes) };
  }
  for (const path of [
    "LICENSE",
    "AUTHORS",
    "system/lib/compiler-rt/LICENSE.TXT",
    "system/lib/libc/musl/COPYRIGHT",
    "system/lib/libcxx/LICENSE.TXT",
    "system/lib/libcxxabi/LICENSE.TXT",
    "system/lib/libunwind/LICENSE.TXT",
  ]) {
    const bytes = Buffer.from(`Synthetic SDK notice fixture: ${path}\n`),
      name = `licenses/emscripten/${path}`;
    runtimeFiles.set(name, bytes);
    noticeOrigins.files[name] = {
      sdkPath: path,
      emsdkCommit: "e4fe26ef59168ff44f4c23c466e497bf60b3411e",
      emscriptenVersion: "4.0.20",
      ...fixtureRecord(bytes),
    };
  }
  // 原样保留公开 GPL 文本，以测试固定的官方源码摘要。
  // Public GPL text is retained exactly to exercise the fixed official-source digest.
  const gpl = readFileSync(new URL("GPL-2.0.txt", import.meta.url));
  runtimeFiles.set("licenses/GPL-2.0.txt", gpl);
  noticeOrigins.files["licenses/GPL-2.0.txt"] = {
    sourceUrl:
      "https://gcc.gnu.org/git/?p=gcc.git;a=blob_plain;f=COPYING;hb=62c888a8f13ddc7184a741dc849fd62e5a2b5962",
    ...fixtureRecord(gpl),
  };
  runtimeFiles.set("licenses/ORIGINS.json", fixtureJson(noticeOrigins));
  for (const path of paths.sdkFiles.filter((path) => path.startsWith("api/include/")))
    runtimeFiles.set(path.slice(4), sourceFiles.get(`wasm/${path}`));
  runtimeFiles.set(
    "CONTENTS.sha256",
    Buffer.from(
      [...runtimeFiles]
        .sort(([a], [b]) => (a < b ? -1 : 1))
        .map(([path, bytes]) => `${fixtureSha256(bytes)}  ${path}\n`)
        .join(""),
    ),
  );
  // 字节占位符只用于认证元数据测试；归档/加载器测试可将其替换为由 runtimeFiles/sourceFiles 构造的惰性 tar/zip。
  // Byte placeholders authenticate metadata tests only. Archive/loader tests can
  // replace these with an inert tar/zip assembled from runtimeFiles/sourceFiles.
  const assetBytes = {
    runtime: Buffer.from("synthetic runtime archive placeholder\n"),
    correspondingSource: Buffer.from("synthetic source archive placeholder\n"),
    buildInputs: inputsBytes,
  };
  const manifest = {
    schemaVersion: 1,
    status: "final",
    kind: "fork-built-compiler",
    binaryBuiltFromNewFork: true,
    repository: identity.repository,
    commit: identity.commit,
    tree: identity.tree,
    version,
    sourceLockSha256: identity.sourceLockSha256,
    sourceFingerprintSha256,
    ...api,
    buildReceiptSha256: fixtureSha256(receiptBytes),
    files,
    assets: Object.fromEntries(
      Object.entries(assetBytes).map(([kind, bytes]) => [
        kind,
        {
          name: `synthetic-${kind}.${kind === "buildInputs" ? "json" : "tgz"}`,
          ...fixtureRecord(bytes),
        },
      ]),
    ),
    runtimeFiles: Object.fromEntries(
      [...runtimeFiles].map(([path, bytes]) => [path, fixtureRecord(bytes)]),
    ),
  };
  mutateManifest?.(manifest);
  const manifestBytes = fixtureJson(manifest);
  const pin = {
    schemaVersion: 1,
    repository: identity.repository,
    commit: identity.commit,
    version,
    manifestSha256: fixtureSha256(manifestBytes),
  };
  const fixture = {
    pin,
    manifest,
    release: manifest,
    manifestBytes,
    receipt,
    receiptBytes,
    inputs,
    inputsBytes,
    sourceLock,
    sourceLockBytes,
    sourceFiles,
    noticeOrigins,
    runtimeFiles,
    assetBytes,
    correspondingSourceBytes: assetBytes.correspondingSource,
  };
  fixture.refreshManifest = () => refreshManifest(fixture);
  return fixture;
}

export function refreshManifest(fixture) {
  fixture.manifestBytes = fixtureJson(fixture.manifest);
  fixture.pin.manifestSha256 = fixtureSha256(fixture.manifestBytes);
  return fixture;
}

// 仅供测试的源码读取器。显式离线审查认证的是所审阅的源码字节，不是公开发布、编译器构建或设备结果。生产代码没有源码目录覆盖选项，也不导入此文件。
// Test-only source reader. Explicit offline review authenticates reviewed
// source bytes, not a published release, compiler build, or device result.
// Production code has no source-directory override and never imports this file.

const hash = (bytes) => createHash("sha256").update(bytes).digest("hex");
const sourceLockSha256 = "39ac9a625495f67de57e10a3976a67f56edc543bd8c3e23bd5aab3df698eab1d";

export const sourceReviewOptions = {
  skip:
    process.env.D2R_AIR_SOURCE_REVIEW_DIR === undefined
      ? "Set D2R_AIR_SOURCE_REVIEW_DIR to opt into offline compiler source review"
      : false,
};

export async function readReviewedSourceLock() {
  const bytes = await readFile(new URL("reviewed-source-lock.json", import.meta.url));
  assert.equal(hash(bytes), sourceLockSha256, "independently reviewed source lock identity");
  const lock = JSON.parse(bytes);
  assert.equal(lock.schemaVersion, 1);
  assert.equal(Object.keys(lock.compilerFiles).length, 31);
  assert.equal(Object.keys(lock.sdkFiles).length, 19);
  return lock;
}

export async function readForkSource(
  path,
  { reviewDirectory = process.env.D2R_AIR_SOURCE_REVIEW_DIR } = {},
) {
  assert.equal(typeof path, "string");
  assert.match(path, /^[A-Za-z0-9_.+/-]+$/);
  assert(
    !path.split("/").some((part) => !part || part === "." || part === ".."),
    "unsafe fork path",
  );
  const lock = await readReviewedSourceLock();
  const expected =
    path === "wasm/source-lock.json"
      ? sourceLockSha256
      : (lock.compilerFiles[path] ??
        (path.startsWith("wasm/") ? lock.sdkFiles[path.slice(5)] : undefined));
  assert.match(expected ?? "", /^[a-f0-9]{64}$/, `No reviewed source identity for ${path}`);
  assert.equal(
    typeof reviewDirectory,
    "string",
    "source review requires D2R_AIR_SOURCE_REVIEW_DIR",
  );
  assert(reviewDirectory.length > 0, "empty source review directory");
  const root = await realpath(reviewDirectory);
  const target = join(root, path);
  assert.equal(await realpath(target), target, "source review path must not traverse symlinks");
  const info = await lstat(target);
  assert(info.isFile() && !info.isSymbolicLink(), "source review blob must be a regular file");
  assert(info.size > 0 && info.size <= 16 * 1024 ** 2, "source review blob size");
  const bytes = await readFile(target);
  assert.equal(bytes.length, info.size, "source review blob changed while reading");
  assert.equal(hash(bytes), expected, `Reviewed fork source mismatch: ${path}`);
  return new TextDecoder("utf-8", { fatal: true }).decode(bytes);
}
