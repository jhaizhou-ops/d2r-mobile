# AIR compiler integration / AIR 编译器集成

## English

### Ownership and release contract

Compiler implementation, compatibility headers and LLVM fixes belong to the
reusable [floor_llvm fork](https://github.com/n3crosis/floor_llvm). D2R owns the
application adapter, authored fixtures, strict **31-call canary**, reflection and
compiled-output validation. This repository does not build LLVM, apply compiler
patches or install an AIR SDK; non-AIR runtimes retain their separate build requirements.

[release-pin.json](../../web/toolchain/metal-air-compiler/release-pin.json) selects
[authenticated v0.1.0](https://github.com/n3crosis/floor_llvm/releases/tag/v0.1.0)
at actual build commit `1bd8db2f0fefbd13636e51c209ef09292675dff5`, with exact manifest
SHA-256 `27b7682418911945a5908eaf0276d9a8afc6c2fe2e853119fa540fdfd95a08fc`.
Never follow a floating branch, substitute a digest or describe an older receipt
as fresh fork provenance.

The importer authenticates versioned release assets, complete corresponding source,
runtime members and exact source/header identity **before changing installed
compiler bytes**. Missing pins/source, stale or corrupt caches and mismatched
receipts fail closed, without local-build or alternate-compiler fallback.
Generic `air_wasm.mjs`/`.wasm` are installed under the two existing
`d2r_air_wasm.*` names byte-for-byte; original generic receipts are not rewritten.
ABI-v2 exports, header-bundle-v2 handshake, capabilities, byte budgets, sealing and
game/source digest gates remain mandatory.

### Import, verify and preserve source

Run from repository root:

```sh
node web/runtime-build/build-air-compiler.mjs
node web/runtime-build/air-compiler-assets.mjs verify
node web/runtime-build/prepare-air-compiler-source.mjs --output /path/to/new-source-mirror
node --test web/tests/air-compiler.test.mjs
```

The source-mirror destination must be empty. It includes exact released source,
artifact-only notices, installed Emscripten runtime sources, unchanged receipt/
lock and authenticated release metadata. Preserve
[third-party/corresponding-source obligations](../../web/toolchain/metal-air-compiler/THIRD_PARTY_NOTICES.md)
and [pre-fork notices](../../web/toolchain/metal-air-compiler/history/PRE_FORK_THIRD_PARTY_NOTICES.md).
The fork's released source owns its build workflow, SDK pins and compiler commands;
D2R's [importer](../../web/runtime-build/build-air-compiler.mjs) owns application admission.

D2R's [fixtures](../../web/toolchain/metal-air-compiler/fixtures/) and
[canary](../../web/toolchain/metal-air-compiler/ci-compile-canary.mjs) stay here,
including four native API smoke inputs. Optional native drivers require explicit
external fork clang/header paths and never download/build a compiler. Source
invariant tests authenticate exact reviewed fork inputs; see [test setup](build-policy.md#verification-and-e2e-tooling).

### Acceptance boundary

CI must pass the unchanged D2R canary before publishing runtime assets. Release
authentication or the fork's generic smoke is not D2R browser, complete-corpus,
GPU or device validation. Optional public-only browser smoke is explicit:

```sh
node web/toolchain/metal-air-compiler/browser-smoke-server.mjs \
  --build /path/to/authenticated/dist --sha256 TRUSTED_RECEIPT_SHA256
```

The server verifies the pinned release first; do not add game inputs. Source tests
do not execute this server. Keep [shader pipeline](shader-pipeline.md) output and
reflection checks independent of compiler download/cache success.

## 简体中文

### 所有权与发布契约

编译器实现、兼容头和 LLVM 修复由独立 floor_llvm fork 维护；D2R 保留应用适配、作者编写的
夹具、严格 **31 次调用 canary**、反射和编译输出检查。本仓库不构建 LLVM、不应用编译器补丁，
也不安装 AIR SDK；非 AIR 运行时仍有各自构建前置要求。

release-pin 固定官方 v0.1.0 的实际构建提交 1bd8db2f0fefbd13636e51c209ef09292675dff5
和上方完整清单摘要。不得跟随浮动分支、替换摘要或把旧回执说成新的 fork 来源证明。

导入器在**修改已安装编译器字节前**认证版本化发布资产、完整对应源码、运行时成员和精确源码/
头文件身份。缺固定值/源码、缓存过期/损坏或回执不匹配均失败，不回退本地构建/替代编译器。
通用 air_wasm.mjs/.wasm 只是逐字节别名为既有 d2r_air_wasm.*，不改原通用回执。
ABI-v2、header-bundle-v2 握手、能力、预算、封闭加载及游戏/源码摘要门禁仍必需。

### 导入、源码与验证

从仓库根目录运行上方导入、verify、源码镜像和功能测试命令。镜像目标必须为空，包含精确
发布源码、仅产物声明、已安装 Emscripten 运行时源码、原样 receipt/lock 及认证发布元数据。
保留链接的第三方/对应源码义务和 pre-fork 声明。编译器构建流程、SDK 固定值与命令归 fork，
应用准入归 D2R 导入器。

D2R 的夹具和 canary 保留，包括四个原生 API 冒烟输入。可选原生驱动需显式外部 clang/头路径，
不下载或构建编译器；源码不变量测试核验精确审查输入，环境说明见验证指南。

CI 发布运行时前必须通过原有 canary。发布认证或通用 fork 冒烟不证明 D2R 浏览器、完整语料、
GPU 或设备正确。上方 browser-smoke-server 是明确选择的公共输入检查，先验证发布，不得加入
游戏输入；源码测试不运行它。着色器输出/反射验证不能被下载或缓存成功替代。
