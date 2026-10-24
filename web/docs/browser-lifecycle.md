# Browser lifecycle / 浏览器生命周期

## English

### From bootstrap to output

[prepareBrowserPackager](../../web/browser-packager.mjs) verifies runtime bytes
against the build-bound HTML digest before private input. The UI's
`editableOptions: true` preloads both shell profiles and optional capability bytes;
core runtimes start immediately, ASTC/signing workers only when needed. Target,
preset and signing choices remain editable until generation without losing the
folder or downloading again. Generation snapshots the options. Direct API callers
can retain selected-only preload.

Input is mixed NSP/XCI packages with keys, or an already-merged extracted tree
without key reads. These routes have different trust guarantees: production skips
full base/update hashing and full RomFS scanning, and known Warlock Meta/PublicData
have a narrow unsigned-header exception. Do not describe this as complete content
authentication; see [input extraction](input-extraction.md).

The sequence is program rewrite → art/HUD/tables/overlays → full shader conversion
→ inventory checks → staged IPA. A session generates once; cancellation requires
reload. Private inputs are not uploaded, but the application has **no page-level
CSP network lockdown**. No-upload behavior is not browser-enforced isolation.

[Resource export](packaging-export.md) precedes the IPA download. Existing data
requires a complete bounded comparison; mismatch never overwrites it. Original
packages/keys stay unchanged. Mark incomplete exports; later IPA failure must not
invalidate completed resources. A separate Legacy Device Pack must match the run.
No-DLC shell derivation changes only the pinned ownership initializer under exact
hash/symbol/preimage/result checks, records the variant, and requires re-signing.
Neither selection nor this derivation proves ownership or gameplay.

### Ownership and failure handling

[packaging-job.mjs](../../web/packaging-job.mjs) checks capabilities before discovery,
key reads or output creation. Serial generation uses `secretLifetime: "archive"`;
generators never receive keys. Drain started work, dispose secrets, validate and
clean up before the sole commit boundary, `output.close()`. Cleanup failure
prevents commit; close rejection is not proof of rollback, and cancellation
cannot undo a committed output. Display only `PackagingJobError.report`, never
its raw cause. Synchronous native work cannot always be interrupted immediately.

[Shell loading](../../web/shell-loader.mjs) verifies independently pinned inventory,
actual Mach-O/plist targets and entries. Shader labels may align only after the
complete normalized source digest matches `SHADER_SRC`, never by label alone.
[Raw staging](../../web/raw-file-stage.mjs) requires explicit budgets and finalized
snapshots; await owned cleanup, with no RAM fallback or quota reservation.
[RomFS indexing](../../web/romfs-tree.mjs) does not upgrade the extraction mode's
trust. Exact bounds and lease rules remain in these modules and [runtime guide](sealed-runtime.md).

### Completed download retry

[createDownloadPreparation](../../web/download-preparation.mjs) retains the successful
Blob/filename separately from URL creation. `prepare` returns ready/retryable/
unavailable; retry prepares only the link, never conversion/export. Reuse URLs;
revoke the old one only after replacement succeeds, and dispose on unmount.
Automatic-click failures leave the visible link available. Never relabel a
completed IPA or resource pack as failed because URL preparation failed.

```sh
node --test web/tests/browser-lifecycle.test.mjs web/tests/packaging-export.test.mjs
```

Run from repository root. Synthetic tests do not replace actual browser downloads,
quota/crash recovery or installation; follow the [user guide](../../docs/WEB_IPA_PACKAGER.md).

### Generated-output cache

#### Use and invalidation

The optional cache stores disposable results in `D2R-cache/v1/` under the selected
input folder. Permission, quota, corruption, unavailable storage and timeout
fall back to generation with one fixed nonfatal notice; cancellation still cancels.
“Ignore saved build cache” bypasses reads, while fresh validated results may refresh
entries. Public runtime-download caching is separate. Never cache keys, passwords,
certificates/profiles, extraction state, raw fonts/textures or decoded pixels.

Folder files and neighboring JSON are not provenance. The independent same-origin
Cache Storage ledger records size/SHA-256 of outputs actually generated and
validated by the app. The random folder marker merely selects that ledger.
Every hit checks the owned snapshot against both ledger and current validator.
Clearing browser storage or changing browser/origin requires rebuilding even if
the folder remains. Old `.d2r-cache` is not read, migrated or deleted.

Shader keys bind exact normalized MSL, shader identity, complete source-set digest,
full compiler identity, target/options and runtime-manifest digest. A hit still
translates current inputs, regenerates reflection and participates in the complete
6,927-key source proof: **only AIR compilation is skipped**. Art keys bind freshly
read payload hashes, options, profile and recipe revisions to the runtime digest.
Native-art/HUD/map-goal hits skip rendering, not authentication, source decoding,
other generators or export. Missing runtime identity disables art reuse.

#### Maintenance and deletion

[local-output-cache.mjs](../../web/local-output-cache.mjs) owns independent-ledger
trust, storage deadlines, serialization and Web Lock lifecycle;
[asset-output-cache.mjs](../../web/asset-output-cache.mjs) owns art result/index
validation. Publish indexes only after validated files close. Hold the Web Lock until started I/O settles; other tabs generate uncached. Remove only verified owned leaves, never concurrent replacements. Corrupt/partial
entries recompute; cancellation wipes owned bytes. Preserve exact budget and
late-cleanup checks rather than trusting a folder receipt.

After the job ends, deleting **D2R-cache only** is safe. Do not delete inputs,
resource folders, Legacy Device Packs or IPAs. Resource completion markers lack
source-bound per-file proofs: caching neither certifies those folders nor tells
what is installed on a phone. No timing or reduced-memory claim is made.

#### OPFS advisory and checks

[readOpfsStorageAdvisory](../../web/opfs-storage-advisory.mjs) only queries
`navigator.storage.estimate()`, never requests persistence/permissions or reserves
space. Unknown capacity is not zero. `requiredBytes` must be independently known;
NSP size and an ASTC 8 GiB upper cap are not allocation requirements. Estimates
concern browser-origin staging, not physical disk or selected-folder free space.
Keep them nonblocking; real write failures remain authoritative. Cancellation of
an advisory does not revive a cancelled job. Exact result types/options remain
in the module.

Run from repository root. Synthetic tests cover trust, invalidation, storage
failure/races and advisory limits, not real browser/device timing or compiler output.

## 简体中文

### 从引导到输出

prepareBrowserPackager 在读取私有输入前，以构建绑定的 HTML 摘要验证运行时字节。
界面通过 editableOptions 预加载两个外壳和可选能力；核心线程立即启动，ASTC/签名按需启动。
生成前可改目标、预设和签名，不丢失文件夹、不再下载；生成时固定选项。直接 API 可只加载所选内容。

输入支持带密钥的混合 NSP/XCI，或无需读取密钥的已合并提取目录，两者信任边界不同。
生产跳过基础/更新全量散列和整个 RomFS 扫描，已知 Warlock Meta/PublicData 有严格限定的
无签名头例外；不能称为完整内容认证，详细规则见输入提取指南。

顺序为程序重写、图像/HUD/表/覆盖、完整着色器转换、清单检查、暂存 IPA。每会话仅生成一次，
取消后需刷新。私有内容不上传，但**没有页面级 CSP 网络锁定**，不能把实现承诺说成浏览器强制隔离。

资源导出先于 IPA 下载。已有 data 必须完整有界比较才能复用，冲突不覆盖；原始包和密钥不变。
未完成导出明确标注，后续 IPA 失败不撤销已完成资源。Legacy Device Pack 独立且必须属于同次运行。
无 DLC 外壳派生只在精确摘要/符号/前像/结果校验下改固定初值并记录，必须重新签名；
内容选择或该派生都不证明购买所有权或游戏可玩。

### 所有权与失败

协调器先查能力，再发现输入、读密钥或创建输出。串行生成采用 secretLifetime: archive，
生成器无密钥。等待在途工作，释放秘密、验档和清理后才到唯一提交边界 output.close()。
清理失败阻止提交；close 拒绝不证明回滚，取消不能撤销已提交成果。只显示固定 report，不暴露原始 cause。
同步原生调用不一定能立即中断。

外壳加载核对独立固定清单、实际 Mach-O/plist 目标和条目。着色器链标签只能在完整规范化源码
摘要匹配 SHADER_SRC 后调整。原始暂存需要明确预算和已完成快照，等待自身清理，无 RAM 回退或配额预留。
RomFS 索引不提升所选提取模式的可信度。精确限制与租约规则见上方源码和运行时指南。

### 下载重试与验证

下载准备独立保留成功 Blob/文件名；prepare 返回 ready/retryable/unavailable，重试只准备链接，
不重复转换/导出。同一成果复用 URL，新 URL 成功后才释放旧 URL，卸载必须清理。
自动点击失败仍保留可见链接，不得因此把完成 IPA/资源包标为失败。
从仓库根目录运行上方测试；合成结果不能替代真实下载、配额/崩溃恢复或安装，操作见用户指南。

### 生成输出缓存

#### 使用与失效

可选缓存把可丢弃结果放在所选输入目录的 D2R-cache/v1/。权限、配额、损坏、API 不可用或超时
只显示一次固定非致命提示并回退生成，取消仍终止任务。“忽略构建缓存”只绕过读取，新验证结果
仍可更新；公开运行时下载缓存独立。不得缓存密钥、密码、证书/描述文件、提取状态、原始字体/纹理或解码像素。

目录文件和旁边 JSON 不证明来源。同源 Cache Storage 独立账本记录应用实际生成且校验的大小/
SHA-256，随机标记只选择账本。命中须同时核对持有的文件快照、账本和当前验证器。清除浏览器
数据或换浏览器/源后必须重建，即使目录还在；旧 .d2r-cache 不读取、迁移或删除。

着色器键绑定精确规范化 MSL、着色器身份、完整源码摘要、编译器身份、目标选项和运行时摘要。
命中仍翻译当前输入、重建反射和完成 6,927 键源码证明，**只跳过 AIR 编译**。图像键包含刚读取
负载摘要、选项、外壳、配方及运行时身份。原生图像/HUD/地图目标命中只跳过渲染，不跳过认证、
源解码、其他生成器或导出；无运行时身份不复用。

#### 维护与删除

local-output-cache 负责独立账本、期限、串行提交和 Web Lock；asset-output-cache 负责图像
结果/索引校验。文件验证并关闭后才发布索引；在途 I/O 结束后才释放锁，其他标签页不缓存。只删除核验过的自身叶文件，不删除并发替换项。损坏或部分结果重算，取消擦除持有字节。精确预算
和迟到清理规则见上方模块，不能改为信任目录回执。

任务结束可删除 **D2R-cache 本身**，不可删除输入、资源目录、Legacy Device Pack 或 IPA。
资源完成标记缺少绑定来源的逐文件证明，缓存不能认证这些目录或判断手机已安装内容；不承诺耗时/内存收益。

#### OPFS 建议与测试

容量建议仅调用 storage.estimate，不申请持久化/权限或预留空间。未知不是零；requiredBytes
必须独立已知，不能由 NSP 大小或 ASTC 8 GiB 上限推算。它表示浏览器源暂存估算，不是物理磁盘
或所选目录余量，不应阻断生成，真实写入错误才是依据；查询取消不恢复已取消任务。
精确选项与结果类型见源码。从仓库根目录运行上方测试，合成结果不证明真实浏览器/设备性能或编译输出。
