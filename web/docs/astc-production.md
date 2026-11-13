# ASTC production / ASTC 生成

## English

### Use and outputs

Older-GPU mode is experimental, off by default and independent of the iOS target. iOS 18 does not imply it is needed. Enable `legacyAstc=1` before generation; Thorough is default, while `astcPreset=fast` trades potential quality for less encoding work without a measured speed/device guarantee. The operation snapshots options; controls lock during generation and afterward.

The editable UI preloads verified public capability bytes before private input, but starts the producer only when needed. No conversion-time network fallback is allowed. The result includes the profile-bound ASTC overlay entries (15 for the current b811 profiles) inside the IPA and a separate `Legacy Device Pack` folder, suffixed `-2`, `-3`, etc. on collision. Copy the completed folder alongside game resources into the installed app's Files location. IPA, resources and pack completion are distinct; partial packs must never be used.

### Pipeline and trust

[astc-producer-client.mjs](../../web/astc-producer-client.mjs) joins source-bound read RPC, verified record staging, serial texture/descriptor conversion and [CASC pack writing](casc-storage.md). The parent enumerates original extraction capabilities, reserves archive locations and passes only bound metadata/sources. Copies, caller EKeys, arbitrary VFS or authentication flags cannot mint provenance. ASTC keeps the first normalized manifest match, unlike generic last-row lookup; the descriptor is mandatory and emitted after successfully converted textures.

The one-shot worker initializes only verified ASTC/PNG/SHA bytes and checks encoder ABI 2. Original encoded/decoded records cap at 256 MiB, production pack at 8 GiB, batches at 4,096 blocks. It retains one verified texture stage rather than complete textures/packs in RAM. Unsupported formats are explicitly skipped; oversized or corrupt records fail. Progress counts completed work, not an inferred overall percentage. Initialization has a 60-second deadline; default inactivity is five minutes, renewed only by advancing validated work. An overall deadline is opt-in.

[ASTC output validation](../../web/astc-output-validator.mjs) and production binding require the exact revision-specific paths and matching pack metadata, never external archive bytes/index TSV inside the IPA. Combined adapter limits still apply; optional files receive no extra allowance. `getAstcOutputBinding()` publishes only after successful generation/cleanup and becomes unavailable after failure/disposal. Metadata receipts do not authenticate imported files.

The parent retains [astc-job-directory.mjs](../../web/astc-job-directory.mjs) ownership throughout export. A returned File does not authorize deleting its backing directory. Stop worker/RPC, then await disposal; retain failed/pending leases for retry. Termination does not prove filesystem writes drained. [legacy-device-pack-export.mjs](../../web/legacy-device-pack-export.mjs) commits only on complete-marker close and preserves unknown/replaced entries during cleanup. Pack-export failure prevents IPA completion; a previously completed folder may remain after later failure.

### Build and verify

```sh
node --test web/tests/astc-production.test.mjs
bash web/toolchain/astc-encoder-wasm/build.sh
node web/toolchain/astc-encoder-wasm/wasm-smoke.mjs
node --test web/toolchain/astc-encoder-wasm/*.test.mjs
```

Run from repository root with Emscripten 4.0.20, Git/CMake and Node ≥24 for WASM smoke. The scalar invariant build pins [Arm 5.0.0 commit 2ff200e0](https://github.com/ARM-software/astc-encoder/tree/2ff200e08c98a61775351687ea2b5d38e455ad8b), preserving 4×4 linear/sRGB, identity channels and closed thorough/fast presets. Rebuild module/WASM together; retain Apache-2.0 license/receipts. Synthetic/fake-codec tests and actual Node WASM smoke are separate evidence. Full native byte/quality parity, browser OPFS termination behavior, greater-than-4-GiB SHA carry and real-device compatibility remain unproven.

## 简体中文

### 使用与输出

旧 GPU 模式是默认关闭的实验功能，独立于 iOS 目标，iOS 18 不代表必须启用。生成前设置 legacyAstc=1；默认 Thorough，astcPreset=fast 用可能降低的画质换较少编码工作，不承诺实测加速或设备兼容。本次操作固定选项，生成中及结束后控件锁定。

可编辑界面在选择私有输入前预载已验证公开字节，仅需要时启动生产器，转换不允许网络回退。IPA 内包含按配置验证的 ASTC overlay（当前 b811 配置为 15 项），另导出 Legacy Device Pack，冲突时使用 -2、-3 等名称。把完整文件夹与游戏资源一起复制到已安装应用的 Files 位置。IPA、资源、补充包完成状态独立；不要使用部分包。

### 管线与信任

producer client 连接来源绑定 RPC、完整性验证暂存、串行纹理/描述转换和 CASC writer。父级枚举原始提取能力、预留位置并只传递绑定元数据/来源；克隆、调用方 EKey、任意 VFS 或认证标志不能生成来源证明。ASTC 采用首个规范化路径匹配，不同于通用最后一行规则；描述缓存必需，成功纹理转换后最后输出。

一次性 worker 只初始化已验证 ASTC/PNG/SHA 字节并检查 ABI 2。生产限制是编码/解码记录各 256 MiB、补充包 8 GiB、每批 4,096 块；保留一个验证暂存，不在 RAM 聚合完整纹理/包。不支持格式明确跳过，超限或损坏失败。进度计已完成工作，不推断总百分比。初始化期限 60 秒；默认不活动期限五分钟，只由验证后的实际进展续期，总期限必须显式指定。

输出验证和绑定要求当前修订的精确路径及匹配包元数据，外部归档/索引 TSV 绝不能进入 IPA。仍受组合适配器预算限制，可选文件没有额外额度。绑定只在生成/清理成功后发布，失败或释放后失效；元数据收据不认证导入文件。

父级在导出期间保留作业目录所有权，获得 File 不代表可删除底层目录。先停止 worker/RPC，再等待释放；失败或未完成租约保留供重试。终止线程不证明写入已经排空。导出最终完整标记关闭才提交，清理保留未知/替换条目。补充包导出失败阻止 IPA 完成，但之后发生其他失败时，已完成目录仍可保留。

### 构建与验证

上方命令从仓库根运行，需要 Emscripten 4.0.20、Git/CMake；WASM smoke 要求 Node≥24。固定 Arm 5.0.0 提交、标量浮点不变性、4×4 线性/sRGB、原通道及封闭预设；模块/WASM 一起重建，保留 Apache-2.0 许可和收据。替身测试与真实 Node WASM smoke 是不同证据。完整原生字节/画质、浏览器 OPFS 终止、超过 4 GiB SHA 进位及真实设备兼容性仍未证明。
