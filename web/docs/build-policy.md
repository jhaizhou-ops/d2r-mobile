# Build and runtime policy / 构建与运行时策略

## English

### Frontend and public-runtime build

Use Bun **1.4.3**, Node **22.12+**, Oxlint **1.89.0**, Oxfmt **0.74.0**.
[package.json](../../web/package.json) and [bun.lock](../../web/bun.lock) are the
frontend authority; toolchain packages keep their separate manifests/locks.
`bunfig.toml` disables implicit peer/runtime installs: missing imports must fail,
not download during execution. From repository root:

```sh
(cd web && bun install --frozen-lockfile)
(cd web && bun run lint && bun run format:check)
(cd web && bun run format:docs:check && bun run format:workflows:check)
(cd web && bun run build)
(cd web && bun run build:bundle) # Frontend only, without repeating checks.
(cd web && bun audit --audit-level=high)
node web/runtime-build/build.mjs --plan
node web/runtime-build/build.mjs
node --test web/tests/build-policy.test.mjs
```

`build` checks translations, lint, formatting and TypeScript before bundling;
unit tests are separate. Respect authored/vendor/generated exclusions, disabled
sorting and language-specific Ruff/clang-format/Bash/C# checks. Do not run the web
formatter over native/vendor trees. See [verification below](#verification-and-e2e-tooling).

Runtime prerequisites: official Emscripten **4.0.20**, commit
`e4fe26ef59168ff44f4c23c466e497bf60b3411e`; .NET SDK/workload **10.0.402** with
`wasm-tools`; Rust **1.99.1**, Git, CMake, make, Python, Node and Bun on PATH.
The script checks versions, does not install SDKs or run browser proofs.
[AIR is imported](air-compiler.md), never built locally. Other components compile
from pinned sources, including SPIRV-Cross
`f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7`; asset runtime is JS/WASM, not Pyodide.

### Packaging and optional groups

[package.mjs](../../web/runtime-build/package.mjs) produces ignored
`web/runtime-build/out/` only after all components succeed. Keep exact allowlists,
source/compiler receipts and notices; exclude game inputs, keys, fixtures and
proof pages. Caps are 64 MiB/file, 512 MiB total and 4,096 files, rejecting symlinks
and collisions. The trusted manifest digest is passed separately into staging/
HTML, never accepted from its own untrusted artifact. Build receipts are not
cryptographic attestations. Exact [transport](../../web/runtime-build/transport.mjs)
checks cover physical payloads and logical slices, including bounded decompression.

[Optional groups](../../web/runtime-asset-groups.mjs) are absent or complete:
`legacy-astc` contains producer/ASTC/streaming-SHA assets; `ipa-signing` contains
signer/WASM and receipts/notices. Reuse common PNG bytes. Preserve common ownership
and separate optional-only transport partitions; selecting one group must not
fetch the other's unique bytes. Unknown/partial groups fail. Direct API selection
defaults to no optional groups; the editable UI preloads both and starts workers
on demand from verified bytes. Retain ASTC File leases through output commit;
prepare signing before credential reads. See [runtime](sealed-runtime.md) and
[signing](ipa-signing.md). `productionEnabled:false` is legacy inventory metadata,
not a UI enablement switch or proof of production acceptance.

### Reproducibility and release boundaries

Keep real revision/dirty/compiler provenance. Cross derives `SOURCE_DATE_EPOCH`
from the pinned commit and uses `TZ=UTC` ([CMake](https://cmake.org/cmake/help/latest/command/string.html#timestamp)).
[ShaderSpike.csproj](../../web/toolchain/shader-spike/ShaderSpike.csproj) sets
`IncludeSourceRevisionInInformationalVersion=false`, `EnableSourceLink=false`,
`DebugType=portable`, `DebugSymbols=true` ([Microsoft](https://learn.microsoft.com/en-us/dotnet/core/compatibility/sdk/8.0/source-link),
[SourceLink](https://github.com/dotnet/sourcelink/blob/main/docs/README.md#enablesourcelink)).
These remove specific metadata variation, not all nondeterminism.

One Linux Verify build assembles/stages manifest-checked `web-preview`, with a
180-minute ceiling; no extra matrix or proof job. GitHub Pages must use GitHub Actions and allow only `pr/web-ipa-packager`; successful pushes/manual runs on that branch deploy the verified artifact without a local build. PRs and `feature/web-review-consolidation` do not deploy.
A successful build is not browser startup, private-input parity or device acceptance.

### Verification and E2E tooling

#### Start with the shared quick check

Run the shared local entry point from repository root:

```sh
node web/verification/verify.mjs quick
(cd web && bun scripts/check-local.mjs)
(cd web && bun run test)
```

`quick` needs Bun 1.4.3, Node 22.12+ (24 recommended), Python with venv/pip and
Bash. It checks the frozen install, frontend/types/lint/format and authored-language
style, **not unit tests, browser proofs or device matrices**. The next commands
are separate explicit local checks. Install exact style tools once if absent:

```sh
python3 -m venv /tmp/d2r-style-tools
/tmp/d2r-style-tools/bin/pip install --index-url https://pypi.org/simple --only-binary=:all: ruff==0.14.0 clang-format==21.1.2
export PATH="/tmp/d2r-style-tools/bin:$PATH"
(cd web && D2R_AIR_SOURCE_REVIEW_DIR=/absolute/path/to/reviewed/floor_llvm bun run test)
```

AIR source-review cases otherwise skip explicitly. A supplied empty/missing/wrong
source tree fails; tests neither download nor execute the compiler. Runtime
release authentication and canary remain mandatory.

#### Explicit heavyweight proofs

Review [profiles](../../web/verification/profiles.json) and
[recipes](../../web/verification/recipes/) first. Supported local profiles include
`extraction`, `signed-nca`, `texture`, `program`, `shader`, `spirv-cross`,
`shader-pipeline`, `offline-wasm`, `offline-dotnet`, `staged-output`, `zip64`,
`desktop-build`, `desktop`, and `full`. Heavy profiles are rejected in Actions.
`full` runs Linux recipes sequentially, not Windows/macOS or a Node matrix.
Some recipes provision pinned tools, use sudo/apt or install workloads/Chromium dependencies:

```sh
node web/verification/verify.mjs texture --provision
node web/verification/verify.mjs full --provision
```

Use a suitable environment and exact review commit. Do not bypass sandbox Chromium
restrictions or label blocked proofs as passes. Desktop recipes require matching
OS and producer-verified artifacts/digests; preserve notices and exclude private
inputs/browser profiles.

The isolated [texture proof](../../web/toolchain/offline-runtime-spike/browser-proof.mjs)
uses strict CSP, sentinel positive controls and server receipts proving no requests
after sealing. Production does **not** have that lockdown. Its direct commands are:

```sh
bash web/toolchain/texture-spike/build-wasm.sh
node web/toolchain/offline-runtime-spike/prepare-bundle.mjs
PLAYWRIGHT_ENTRY=/absolute/path/to/playwright/index.mjs node web/toolchain/offline-runtime-spike/browser-proof.mjs
```

The .NET proof requires `-p:WasmInlineBootConfig=false` and a message listener via
`addEventListener`, not the loader-owned `onmessage` slot. Its
[worker](../../web/toolchain/offline-runtime-spike/dotnet-worker.mjs) preserves verified resource
hooks without broad eval fallback; [official loader APIs](https://github.com/dotnet/runtime/blob/v10.0.13/src/mono/browser/runtime/loader/run.ts)
remain the reference.

#### Artifacts, cache and deployment

`web-preview` contains only built frontend/public runtimes and digest manifests,
retained seven days; not a game IPA. Serve over HTTPS, not file://. Staging rejects
unexpected/changed files. See [build policy](build-policy.md).

Non-AIR SDK caching covers only Emscripten download archives: fresh pinned SDK
checkout, exact OS/architecture/image/version/commit key, no fallback keys,
`EMSDK_KEEP_DOWNLOADS=1`; install/activate/source checks still run.
[actions/cache v6.1.0](https://github.com/actions/cache/tree/55cc8345863c7cc4c66a329aec7e433d2d1c52a9)
and [SDK behavior](https://github.com/emscripten-core/emsdk/blob/e4fe26ef59168ff44f4c23c466e497bf60b3411e/emsdk.py)
are pinned. AIR caches separately authenticate release/source/receipts and canary.
Cache hits are neither provenance nor measured speedup.

## 简体中文

### 前端与公开运行时

使用 Bun 1.4.3、Node 22.12+、Oxlint 1.89.0、Oxfmt 0.74.0。前端以 package.json/
bun.lock 为准，独立工具链保留自己的清单/锁；bunfig 禁用隐式 peer 和运行时安装，缺导入必须失败。
从仓库根目录运行上方命令。build 在打包前检查翻译、lint、格式和 TypeScript，单元测试另跑。
保留作者文件/供应商/生成物排除规则、禁用排序及各语言检查，不要把 web 格式器跑到原生/供应商树。

运行时要求官方 Emscripten 4.0.20 及上方精确提交、.NET SDK/workload 10.0.402 的 wasm-tools、
Rust 1.99.1，以及 Git/CMake/make/Python/Node/Bun。脚本核对版本，不安装 SDK 或运行浏览器证明。
AIR 从固定发布导入，不本地编译；其他组件依固定源码构建，包括上方 Cross 提交。资产运行时为 JS/WASM，非 Pyodide。

### 打包与可选组

所有组件成功后才生成忽略的 runtime-build/out。保留准确允许列表、来源/编译器回执及许可证，
排除游戏、密钥、夹具和证明页面。上限单文件 64 MiB、总计 512 MiB、4,096 文件，拒绝符号链接/冲突。
可信清单摘要独立传给暂存/HTML，不能从同一未可信产物取得；构建回执不是密码学证明。
传输源码负责物理载荷和逻辑切片验证，包括有界解压。

legacy-astc 和 ipa-signing 只能完整存在或缺席，前者含生成器/ASTC/流式 SHA，后者含签名器/
WASM/回执声明；共用 PNG 不重复。保留公共归属和独立可选分区，选一组不能夹带另一组独有字节。
未知/部分组拒绝。API 默认不选可选组；可编辑 UI 预载两组，按需从验证字节启动。
ASTC File 租约保留至提交，签名前先准备线程再读凭据。productionEnabled:false 只是旧清单元数据，
不是 UI 开关或生产验收证明。

### 可复现性与发布

保留真实修订、dirty 和编译器来源。Cross 的 SOURCE_DATE_EPOCH 来自固定提交并用 TZ=UTC。
ShaderSpike 禁用修订信息版本和 SourceLink，保留 portable 调试符号；上方官方依据与源码说明
精确开关。这只消除已知元数据变化，不保证全部确定性。

单个 Linux Verify 构建/暂存清单验证的 web-preview，限 180 分钟，无额外矩阵/证明任务。
Pages 选择 GitHub Actions，环境只允许 pr/web-ipa-packager；该分支成功推送/手动运行部署验证产物，无需本地构建。PR 和 feature/web-review-consolidation 不部署。
构建成功不代表浏览器启动、私有输入一致性或设备验收。

### 验证与端到端工具

#### 先跑共用快速检查

仓库根目录命令见上方。
quick 需 Bun 1.4.3、Node 22.12+（推荐 24）、带 venv/pip 的 Python 和 Bash；检查冻结安装、
前端/类型/lint/格式及作者代码样式，**不跑单元测试、浏览器证明或设备矩阵**。其余命令是显式本地检查。
缺样式工具时，用上方临时 venv 安装精确 Ruff/clang-format 版本。

AIR 源码审查用例默认明确跳过；显式指定源码目录必须存在且匹配摘要，空/错目录失败。
测试不下载或执行编译器；发布认证及 canary 不受影响。

#### 显式重型验证

先查看 profiles 和 recipes。上方列表是支持的本地配置，Actions 拒绝重型配置。
full 顺序执行 Linux 方案，不含 Windows/macOS 或 Node 矩阵。部分方案安装固定工具、
使用 sudo/apt 或安装 workload/Chromium 依赖，必须显式 --provision。用适合环境及确切评审提交。
沙箱阻止 Chromium 时不得绕过或报告通过；桌面方案需对应系统和生产端验证的产物/摘要，
保留声明，不传私有输入或浏览器配置。

独立 texture 证明使用严格 CSP、可达性正控和服务端收据检查封闭后零请求；生产应用没有该锁定。
直接构建/准备/浏览器命令见上。独立 .NET 证明需外部 boot config 标志及 addEventListener，
不能占用加载器的 onmessage；资源钩子只给验证字节，不用宽泛 eval 回退，官方 API 链接见英文。

#### 产物、缓存与部署

web-preview 只包含构建网页、公开运行时和摘要清单，保留七天，不是游戏 IPA。通过 HTTPS 托管，
不能用 file://；暂存拒绝额外或变更文件。非 AIR 缓存只保存 Emscripten 下载归档，每次仍重新检出
固定 SDK，以精确系统/架构/镜像/版本/提交键恢复，无回退键，并重新安装、激活、核对来源。
EMSDK_KEEP_DOWNLOADS=1 保留归档；Action/SDK 来源见上。AIR 独立缓存仍认证发布、源码、回执并跑 canary。
命中不等于来源证明或实测加速。

Pages 选择 GitHub Actions，环境只允许 pr/web-ipa-packager。只有该分支成功推送/手动运行
部署同一产物，PR 和 feature/web-review-consolidation 不部署；配置好不代表站点已上线。
