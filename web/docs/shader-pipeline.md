# Shader pipeline / 着色器流水线

## English

### Purpose and entry points

Convert validated game shader records into compiled MTLB libraries and replay metadata under the [input/extraction trust policy](input-extraction.md). The browser uses the real .NET/WASM translator, SPIRV-Cross and AIR/WASM compiler; it does not invoke Xcode, upload private inputs or substitute source text for binaries.

Start with these implementation boundaries:

- [Catalog](../../web/shader-catalog-adapter.mjs): pinned tables, record selection and owned pack buffers.
- [Translation pipeline](../../web/shader-pipeline.mjs): translator/Cross protocol, stage checks and complete-source proof.
- [Production adapter](../../web/production-shaders.mjs): `generateShaders`, compilation, output budgeting and cleanup.
- [AIR compiler](air-compiler.md): compiler authentication, target/profile and library validation.

Keep signatures, numeric limits and protocol definitions in those sources authoritative rather than duplicating them here.

### Main flow and trust boundary

1. Load the application-authenticated shell, sealed runtime and SHA-pinned catalog. Accept private inputs only through extraction provenance bound to the same selection; caller metadata cannot establish trust.
2. Prepare all 6,927 catalog records from source-validated packs/main-image data, preserving native facts and explicit debug/reduced settings. Translate serially through .NET and Cross.
3. Compile each base source and any native-selected readable-tile variant. Validate containers, embedded digests, entry/stage and compiler/source receipts. Cache hits still repeat translation, reflection and complete-source verification.
4. Require the full normalized-source aggregate `be59c864f087b377` before applying the shell chain and returning entries. Never omit unsupported keys, relabel a mismatch or publish partial results.

Outputs are `msl/b-<chain>/<key>_b.metallib`, replay `_b.json`, optional `_bf.metallib`, and `dm/c1_full.tsv`. Translation candidates alone are not compiled/installable output. Hull/domain jobs remain explicitly gated and source-ported-unverified; do not infer native or device parity.

### Operating limits

Production overlaps serial translation with at most four independent AIR workers and retains a combined 512 MiB library/replay budget. These are rejection/scheduling limits, not browser-wide RAM guarantees. Output readers return owned chunks; consume them before `dispose()`. Cancellation/failure stops workers and wipes retained buffers. Progress reaching `6927/6927` does not mean packaging is complete. See [output cache](browser-lifecycle.md#generated-output-cache) for persistence rules.

Self-host the complete same-origin HTTP(S) worker/compiler closure, including .NET `_framework` and notices; no CDN fallback or private-input-triggered downloads.

### Build and verify

Run from repository root on a developer host. [Verification guidance](build-policy.md#verification-and-e2e-tooling) covers prerequisites; heavyweight profiles can provision SDKs/system dependencies and are local-only.

```sh
node web/verification/verify.mjs shader --provision
node web/verification/verify.mjs spirv-cross --provision
node web/verification/verify.mjs shader-pipeline --provision
node --test web/toolchain/spirv-cross-spike/test.mjs
node web/scripts/validate-ipa-shaders.mjs "/path/to/generated.ipa"
node web/scripts/validate-ipa-shaders-smoke.mjs
```

The IPA checker needs Node 22.12+ or Bun and accepts app-canonical STORE ZIP32/ZIP64, including app-signed output, not repacked archives. It is local/read-only; exit 0 succeeds and 1 fails. Optional `--expected-count 7212` is reference-specific. Integrity success does not prove AIR semantics, compiler provenance, signing or GPU acceptance.

### Source and licensing

[Cross build](../../web/toolchain/spirv-cross-spike/build.sh) pins official Cross `1.4.363.0` and Emscripten `4.0.20`; [.NET build](../../web/verification/recipes/shader-proofs.sh) pins SDK/workload `10.0.402`. Preserve [translator source pins](../../web/toolchain/shader-spike/upstream-source.sha256), [Ryujinx license](../../dm/third_party/ryujinx/LICENSE.txt)/[provenance](../../dm/third_party/ryujinx/README.md), and Cross Apache-2.0 LICENSE/NOTICE with redistribution. Synthetic parity and integrity checks are not full-game/device validation.

## 简体中文

### 用途与入口

按[输入提取信任策略](input-extraction.md)，将验证后的游戏着色器记录转换为真实 MTLB 编译库及回放元数据。浏览器采用真实 .NET/WASM 翻译器、SPIRV-Cross 和 AIR/WASM 编译器，不调用 Xcode、不上传私有输入，也不以源码伪装二进制。

维护时从以下边界入手：

- [目录适配器](../../web/shader-catalog-adapter.mjs)：固定来源表、记录选择、包缓冲区所有权。
- [翻译流水线](../../web/shader-pipeline.mjs)：翻译器/Cross 协议、阶段检查和完整源码证明。
- [生产适配器](../../web/production-shaders.mjs)：generateShaders、编译、输出预算和清理。
- [AIR 编译器](air-compiler.md)：编译器认证、目标配置及库校验。

接口签名、数值上限和协议定义以实现为准，不在本文重复维护全部校验细节。

### 主流程与信任边界

1. 加载应用认证的外壳、密封运行时及 SHA 固定目录。私有输入必须来自绑定同一选择的提取来源证明，调用方元数据不能自建信任。
2. 从已检查来源的包/main 内存镜像准备全部 6,927 条记录，保留原生事实与显式 debug/reduced 设置，再串行执行 .NET/Cross 翻译。
3. 编译基础源码及原生规则选出的可读瓦片变体；验证容器、内嵌摘要、入口/阶段和编译器/源码回执。缓存命中也必须重做翻译、反射及完整源码验证。
4. 完整归一化源码摘要必须匹配 `be59c864f087b377`，之后才能应用外壳 chain 并返回输出。不能省略不支持的键、修改标签掩盖不符或发布部分结果。

输出为 `msl/b-<chain>/<key>_b.metallib`、对应 `_b.json`、可选 `_bf.metallib` 及 `dm/c1_full.tsv`。翻译候选本身并非已编译或可安装输出。Hull/domain 路径仍需显式开启，属于未验证的源码移植，不代表原生或设备等价。

### 使用限制

生产将串行翻译与最多四个独立 AIR 线程重叠，库及回放总预算为 512 MiB。这些是拒绝/调度阈值，不是浏览器总体 RAM 保证。输出块具有独立所有权，消费完成后再 dispose；取消或失败会停止线程并擦除保留缓冲区。显示 `6927/6927` 不代表打包完成。持久化规则见[输出缓存](browser-lifecycle.md#generated-output-cache)。

必须同源 HTTP(S) 自托管完整工作线程/编译器闭包，包括 .NET `_framework` 和许可通知；不使用 CDN 回退，也不因私有输入触发代码下载。

### 构建与检查

在开发机的仓库根目录运行英文段命令。[验证说明](build-policy.md#verification-and-e2e-tooling)列出前置工具；重型 profile 可能安装 SDK/系统依赖，仅供本地显式运行。

IPA 检查需要 Node 22.12+ 或 Bun，仅接受本应用原始 STORE ZIP32/ZIP64（含本应用签名），不支持重新打包文件。检查完全本地只读，成功退出 0、失败 1；可选 `--expected-count 7212` 只适用于相应参考版本。完整性通过不能证明 AIR 语义、编译器来源、签名或 GPU 接受。

### 来源与许可

上述构建脚本固定 Cross 1.4.363.0、Emscripten 4.0.20、.NET SDK/workload 10.0.402。分发时保留翻译器源码固定记录、Ryujinx 许可/来源说明，以及 Cross 的 Apache-2.0 LICENSE/NOTICE。合成差分与完整性检查不等于真实游戏或设备验证。
