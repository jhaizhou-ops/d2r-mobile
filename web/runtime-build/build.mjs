// 依据固定工具链、封闭外壳配置和构建凭据生成公开浏览器运行时。
// Build the public browser runtime from pinned toolchains, closed shell profiles and build receipts.
import { createHash } from "node:crypto";
import process from "node:process";
import { spawnSync } from "node:child_process";
import { mkdir, writeFile, readFile, appendFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { resolve } from "node:path";
import { assembleRuntime, SHELL_BUILD_INPUTS } from "./package.mjs";
import { inspectShaderSourceLink } from "./shader-sourcelink-diagnostic.mjs";
import { prepareAirCompilerAssets } from "./air-compiler-assets.mjs";

const root = fileURLToPath(new URL("../../", import.meta.url));
const workspace = resolve(root, "web/runtime-build/.build");
const sdk = "10.0.402";
const crossCommit = "f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7";
const stages = [
  ["optional local IPA signer", ".", "bash", ["web/toolchain/zsign-wasm/build.sh"]],
  [
    "optional local IPA signer WASM smoke",
    ".",
    "node",
    ["web/toolchain/zsign-wasm/wasm-smoke.mjs"],
  ],
  ["texture decoder and BC3 encoder", ".", "bash", ["web/toolchain/texture-spike/build-wasm.sh"]],
  ["Rust BC3 encoder", ".", "node", ["web/toolchain/texture-spike/build-bc3.mjs"]],
  // 编译可选 ASTC 并在 Node 工作线程中检查 WASM；默认不下载，不代表浏览器或设备验证。
  // Compile optional ASTC and check WASM in a Node worker; omitted by default, not browser/device proof.
  ["optional ASTC codec", ".", "bash", ["web/toolchain/astc-encoder-wasm/build.sh"]],
  ["experimental ASTC WASM smoke", ".", "node", ["web/toolchain/astc-encoder-wasm/wasm-smoke.mjs"]],

  ["Pinned PNG and CASC zlib codec", ".", "node", ["web/toolchain/png-codec-exact/build.mjs"]],
  [
    "bounded zlib WASM stream smoke",
    ".",
    "node",
    ["web/toolchain/png-codec-exact/test/stream-smoke.mjs"],
  ],
  ["Rust image kernels", ".", "node", ["web/toolchain/image-kernels-rust/build.mjs"]],
  ["Pinned font rasterizer", ".", "node", ["web/toolchain/font-rasterizer-exact/build.mjs"]],
  ["authenticated extraction", ".", "bash", ["web/toolchain/nsp-extract-spike/build-wasm.sh"]],
  ["ARM64 rewrite runtime", ".", "bash", ["web/toolchain/program-rewrite-spike/build-wasm.sh"]],
  [
    "sealed texture closure",
    ".",
    "node",
    ["web/toolchain/offline-runtime-spike/prepare-bundle.mjs"],
  ],
  ["JavaScript asset closure", ".", "bun", ["web/toolchain/asset-runtime-js/build.mjs"]],
  ["shader restore", "web/toolchain/shader-spike", "dotnet", ["restore", "ShaderSpike.csproj"]],
  [
    "shader browser publish",
    "web/toolchain/shader-spike",
    "dotnet",
    [
      "publish",
      "ShaderSpike.csproj",
      "-c",
      "Release",
      "--no-restore",
      "-p:WasmInlineBootConfig=false",
      "-p:EnableNETAnalyzers=true",
      "-p:AnalysisLevel=latest",
      "-p:TreatWarningsAsErrors=true",
    ],
  ],
  [
    "sealed shader closure",
    "web/toolchain/shader-spike",
    "node",
    ["../offline-runtime-spike/dotnet-prepare.mjs", "bin/Release/net10.0/publish"],
  ],
];
function run(command, args, cwd = root, capture = false) {
  const result = spawnSync(command, args, {
    cwd,
    encoding: "utf8",
    stdio: capture ? "pipe" : "inherit",
    timeout: 45 * 60_000,
    env: {
      ...process.env,
      DOTNET_CLI_TELEMETRY_OPTOUT: "1",
      DOTNET_NOLOGO: "1",
      D2R_EMSCRIPTEN_ENVIRONMENT: "worker",
      RUSTUP_TOOLCHAIN: "1.99.1",
    },
  });
  if (result.error || result.status !== 0)
    throw new Error(
      `${command} failed: ${result.error?.message || result.stderr || result.status}`,
    );
  return result.stdout?.trim();
}
const option = process.argv[2];
if (option === "--plan") {
  console.log(
    JSON.stringify(
      {
        prerequisites: {
          emscripten: "4.0.20",
          rust: "1.99.1",
          dotnet: sdk,
          workload: "wasm-tools 10.0.402",
          bun: "1.4.3",
          commands: ["git", "cmake", "make", "python3", "node", "bun"],
        },
        crossCommit,
        airCompiler: {
          cache: "web/toolchain/metal-air-compiler/dist/",
          inputDirectoryEnvironment: "D2R_AIR_COMPILER_ASSET_DIR",
          receiptDigestEnvironment: "D2R_AIR_COMPILER_RECEIPT_SHA256",
          rebuildsLlvm: false,
          required: true,
        },
        publicShells: SHELL_BUILD_INPUTS,
        stages,
        output: "web/runtime-build/out/",
        productionEnabled: false,
      },
      null,
      2,
    ),
  );
} else if (option === undefined) {
  // 编译器只构建一次；前端构建验证并复用源固定缓存，不隐式重建 LLVM。
  // Build the compiler once; frontend builds verify/reuse the source-bound cache, never implicitly rebuild LLVM.
  await prepareAirCompilerAssets();
  // 先决工具由贡献者或 CI 单独安装；不提权、不安装 SDK、不启动浏览器、测试、游戏输入或签名凭据。
  // Prerequisites are installed separately by the contributor or CI. No sudo,
  // SDK installation, browser launch, tests, game inputs or signing credentials.
  if (!run("emcc", ["--version"], root, true).split("\n")[0].includes("4.0.20"))
    throw new Error("Expected Emscripten 4.0.20");
  if (run("dotnet", ["--version"], resolve(root, "web/toolchain/shader-spike"), true) !== sdk)
    throw new Error(`Expected .NET SDK ${sdk}`);
  await mkdir(workspace, { recursive: true });
  const checkout = resolve(workspace, "spirv-cross");
  // 隔离且固定的修订版本，不使用分支最新提交或用户的检出目录。
  // An isolated exact revision, never a branch tip or a user's checkout.
  run("git", ["init", "-q", checkout]);
  run(
    "git",
    ["fetch", "--depth", "1", "https://github.com/KhronosGroup/SPIRV-Cross.git", crossCommit],
    checkout,
  );
  run("git", ["checkout", "--detach", "FETCH_HEAD"], checkout);
  if (run("git", ["rev-parse", "HEAD"], checkout, true) !== crossCommit)
    throw new Error("Cross source pin mismatch");
  run("bash", [
    "web/toolchain/spirv-cross-spike/build.sh",
    "wasm",
    checkout,
    resolve(workspace, "cross-wasm"),
  ]);
  await writeFile(
    resolve(root, "web/toolchain/shader-spike/global.json"),
    JSON.stringify({
      sdk: { version: sdk, rollForward: "disable", allowPrerelease: false, workloadVersion: sdk },
    }) + "\n",
  );
  const sourceRevision = run("git", ["rev-parse", "HEAD"], root, true);
  for (const [name, directory, command, args] of stages) {
    console.log(`Build runtime: ${name}`);
    run(command, args, resolve(root, directory));
    if (name === "shader browser publish")
      console.log(
        JSON.stringify(
          await inspectShaderSourceLink({
            projectDirectory: resolve(root, directory),
            sourceRevision,
          }),
        ),
      );
  }
  for (const input of SHELL_BUILD_INPUTS) {
    const response = await globalThis.fetch(input.sourceUrl, {
      signal: AbortSignal.timeout(120000),
    });
    if (!response.ok || !response.body) throw new Error("Public shell download failed");
    const reader = response.body.getReader(),
      shellBytes = new Uint8Array(input.bytes);
    let offset = 0;
    try {
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        if (offset + value.length > shellBytes.length) throw new Error("Public shell size limit");
        shellBytes.set(value, offset);
        offset += value.length;
      }
    } finally {
      await reader.cancel();
    }
    if (
      offset !== input.bytes ||
      createHash("sha256").update(shellBytes).digest("hex") !== input.sha256
    )
      throw new Error("Public shell digest mismatch");
    await writeFile(resolve(root, "web", input.buildPath), shellBytes);
  }
  run("bun", ["web/runtime-sealing/build-workers.mjs"]);
  await assembleRuntime({
    build: {
      emscripten: "4.0.20",
      dotnet: sdk,
      crossCommit,
      sourceRevision,
      sourceDirty: Boolean(
        run("git", ["status", "--porcelain", "--untracked-files=no"], root, true),
      ),
    },
  });
  const digest = createHash("sha256")
    .update(await readFile(resolve(root, "web/runtime-build/out/runtime-manifest.json")))
    .digest("hex");
  if (process.env.GITHUB_OUTPUT)
    await appendFile(process.env.GITHUB_OUTPUT, `manifest_sha256=${digest}\n`);
} else {
  throw new Error("Usage: node web/runtime-build/build.mjs [--plan]");
}
