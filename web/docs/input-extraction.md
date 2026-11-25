# Input and extraction / 输入与提取

## English

### Use

Choose one local folder containing base/update NSP or XCI packages and `prod.keys`; `title.keys` is optional until encrypted content requires it. Every discovered root-level package is included. Warlock DLC is optional in the production profile, but unknown or incompatible DLC rejects. Package names do not establish identity, and multiple content versions require explicit selection rather than silently choosing newest.

Alternatively, choose already-merged `ExeFS/main`, `ExeFS/sdk`, `RomFS/data/build_manifest.psv` and full-sized `RomFS/data/*.data`. This route needs no keys, does not merge updates, and cannot use `.holes` instead of archives. Packages take precedence when present; missing/invalid keys never trigger a fallback. Inputs stay local and read-only.

### Boundaries and maintenance

Start with [game-input.mjs](../../web/game-input.mjs), [package-input.mjs](../../web/package-input.mjs) and [extracted-input.mjs](../../web/extracted-input.mjs). Discovery only checks layout; deeper inspection is not a runnable-game guarantee. [content-selection.mjs](../../web/content-selection.mjs) owns CNMT parsing/association; [d2r-expansion-policy.mjs](../../web/d2r-expansion-policy.mjs) owns optional-Warlock, base/update-only and mandatory-Warlock modes. Keep exact executable allowlisting independent of metadata versions.

The production browser skips full base/update NCA SHA-256 and the full RomFS integrity scan. Signed base/update headers, section/package checks, selected ExeFS verification and exact executable hashes remain required. Known small Warlock NCAs are fully hashed; only their Meta/PublicData headers have a narrow unsigned-header exception, never executable base/update content. Do not describe this as whole-content authentication. Independent strict native APIs remain available. Extracted-folder provenance instead requires a catalog-matched complete main/sdk tuple, rehashed by the rewriter; it cannot authenticate missing CNMT or establish that indistinguishably merged DLC is absent. It is explicitly base/update-only.

[extraction-job-adapter.mjs](../../web/extraction-job-adapter.mjs) and [extraction-worker-client.mjs](../../web/extraction-worker-client.mjs) enforce trusted factories, fixed operations, bounded queues/deadlines and generation-bound reads of at most 64 KiB. Stage main/sdk before opening RomFS, which revokes ExeFS readers. Generators receive narrow read capabilities, never keys or arbitrary Worker access. Lazy output needs `secretLifetime: "archive"`; drain producers, dispose before commit, and wipe staged modules. Timeout, malformed replies or cancellation terminate the whole session. Keys use explicit memory-only files, sanitized errors and best-effort zeroization, not browser persistence or a forensic-erasure promise.

Parsers reject unsupported formats and over-budget inputs rather than guessing. The authoritative limits and format checks live in [XCI input](../../web/xci-input.mjs), [NSO validation](../../web/nso-validation.mjs) and the [native boundary](../../web/toolchain/nsp-extract-spike/boundary.c). NSO consistency alone never proves a supported build.

### Build and verify

Run from repository root with Emscripten 4.0.20 active:

```sh
bash web/toolchain/nsp-extract-spike/build-wasm.sh
node --test web/tests/input-extraction.test.mjs
node --test web/toolchain/synthetic-signed-nca/generate.test.mjs
# Optional heavyweight dual-WASM proof:
bash web/toolchain/synthetic-signed-nca/run-ci.sh
```

The build pins [hactool 1d64a834](https://github.com/SciresM/hactool/tree/1d64a83450e025622f3468c28fc4164dad2c5ef6), preserves 64-bit file offsets and caps WASM at 256 MiB. Keep its ISC license, bundled mbedTLS Apache-2.0 and all linked-source notices; never distribute keys/game content or the synthetic test-root runtime. [Atmosphère CNMT](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libstratosphere/include/stratosphere/ncm/ncm_content_meta.hpp) and [LZ4 block format](https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md) are format references, not copied implementations. Synthetic checks and optional [extraction profiles](build-policy.md#verification-and-e2e-tooling) do not establish real-game/XCI/device compatibility.

## 简体中文

### 使用

选择包含基础/更新 NSP 或 XCI 及 `prod.keys` 的本地目录；只有加密内容需要时才必须提供 `title.keys`。所有根级软件包都会包含。生产配置允许不提供 Warlock DLC，但拒绝未知或不兼容 DLC。文件名不能证明身份；多个内容版本必须明确选择，不能静默选最新。

也可选择已合并的 `ExeFS/main`、`ExeFS/sdk`、`RomFS/data/build_manifest.psv` 及完整 `RomFS/data/*.data`。此路径无需密钥，不合并更新，`.holes` 不能替代存档。存在软件包时优先使用软件包；密钥缺失或错误不会触发回退。输入始终只读且留在本地。

### 边界与维护

从英文节链接的输入适配器开始；目录发现只检查布局，不证明游戏可运行。content-selection 负责 CNMT 解析/关联，d2r-expansion-policy 负责可选 Warlock、仅基础更新和必需 Warlock 模式。精确可执行文件允许列表必须独立于元数据版本。

生产浏览器跳过完整基础/更新 NCA SHA-256 和整个 RomFS 扫描，但仍要求基础/更新签名头、节/包结构、所选 ExeFS 和精确可执行散列检查。已知小型 Warlock NCA 完整散列；只有其 Meta/PublicData 头存在有限无签名例外，不适用于基础/更新可执行内容。不要称为完整内容认证；独立严格原生 API 仍可用。提取目录则要求 main/sdk 完整组合匹配允许列表，并由重写器再次散列；它不能认证缺失的 CNMT，也不能证明不可区分的混入 DLC 不存在，因此明确仅支持基础/更新。

Worker 客户端和作业适配器限制可信 factory、固定操作、队列/截止时间及最多 64 KiB 的代际绑定读取。先暂存 main/sdk，再打开会撤销 ExeFS 读取器的 RomFS。生成器只得到狭窄读取能力，不获得密钥或任意 Worker 权限。惰性输出需要 `secretLifetime: "archive"`；生产器耗尽后、提交前释放并擦除模块。超时、畸形回复或取消终止整个会话。密钥仅使用显式内存文件、净化错误和尽力清零，不持久保存，也不承诺取证级擦除。

不支持的格式或超预算输入直接拒绝，不猜测。XCI、NSO 和原生 boundary 中的常量/校验是权威定义。NSO 一致性不能证明受支持构建。

### 构建与验证

上方命令从仓库根运行，需要 Emscripten 4.0.20。固定 hactool 提交、64 位文件偏移和 256 MiB WASM 上限不得随意更改。保留 hactool ISC、mbedTLS Apache-2.0 及全部链接依赖声明；不得发布密钥、游戏内容或合成测试根运行时。上方格式链接是参考，不是复制实现。合成及可选双 WASM/提取验证不等于真实游戏、XCI 或设备兼容性证明。
