# Packaging and export / 打包与导出

## English

### Outputs and choices

A complete setup needs the IPA plus separate game resources. Default export creates `data` directly inside the authorized input folder. An existing `data` is reused read-only only after its complete file set, sizes and bytes match current input. Failed verification changes nothing. Old UUID-wrapped exports are not moved automatically; their inner data folder may be moved manually and must pass the same checks.

`generate({exportResources:false})` skips only this original-resource copy. Select it only when matching game/update/DLC resources are already on the device; the browser does not inspect those device files. Input changes reset the choice. Authentication, generation, validation and an enabled [Legacy Device Pack](astc-production.md) still run. Results explicitly report resources skipped, independently of IPA success.

### Flow and ownership

[resource-folder-export.mjs](../../web/resource-folder-export.mjs) consumes live extraction-bound RomFS capabilities, never caller-authored provenance. It checks the required native archive inventory before output and exports all data members, including optional languages. Keep the extraction/key Worker alive until lazy copies finish. The input directory is the previously authorized destination; exporters do not open pickers, broaden access, overwrite packages/keys or upload data.

Reads remain ≤64 KiB, coalesced into serial ≤1 MiB writes. Progress counts successful writes, not buffered bytes. The final complete-marker close commits resource export; later IPA failure does not undo that completed folder. Failure aborts live writers, verifies handle identity and removes only individually tracked owned entries. Unknown additions/replacements survive; incomplete markers and cleanup reports must remain visible. Web Locks coordinate packager tabs, not hostile external writers or crash recovery.

[browser-staged-output.mjs](../../web/browser-staged-output.mjs) writes an owned OPFS stage, independently validates actual ZIP bytes, and revalidates the immutable snapshot copied to the destination. Owned stage cleanup precedes destination close, the commit boundary. Never call success before validation/close, delete unknown entries or promise rollback after uncertain browser I/O. [browser-blob-output.mjs](../../web/browser-blob-output.mjs) caps the production IPA at 512 MiB and may retain it in browser memory; bulk resources remain separate.

[zip64-writer.mjs](../../web/zip64-writer.mjs) is explicit opt-in with a mandatory payload-plus-overhead budget and a streaming sink, not a large in-memory fallback. ZIP64 arithmetic does not establish installability. Storage estimates are advisory OPFS capacity, not destination-drive free space or reservation; actual quota/permission failures still stop output. Detailed limits and commit rules live in these implementations.

[d2r-runtime-content-mode.mjs](../../web/d2r-runtime-content-mode.mjs) derives base-only behavior only from trusted selection and exact pinned shell identity. It changes the verified add-on-count data initializer, not instructions, and preserves unrelated shell hashes. Modified signatures are invalid; signing/installing remain separate. Copy completed data/Legacy Device Pack folders to the installed app's Files/Documents location as described by the [native guide](../../dm/ios/README.md).

### Verify

```sh
node --test web/tests/packaging-export.test.mjs
```

Run from repository root; [E2E tooling](build-policy.md#verification-and-e2e-tooling) documents optional `staged-output` and `zip64` profiles. ZIP formatting follows [PKWARE APPNOTE](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT). Synthetic streams and OPFS test destinations do not prove OS save-picker behavior, crash recovery, real-game disk capacity or device compatibility.

## 简体中文

### 输出与选项

完整安装需要 IPA 和独立游戏资源。默认在已授权输入目录直接创建 data。已有 data 只有完整文件集合、尺寸及字节都匹配当前输入时才只读复用；验证失败不会修改。旧 UUID 外层目录不自动迁移，可手动移动内部 data 后重新验证。

`generate({exportResources:false})` 仅跳过原始资源复制。只有设备已有相同游戏/更新/DLC 资源时才应选择；浏览器不检查设备文件。更改输入会重置选项。认证、生成、验证和已启用的 Legacy Device Pack 仍执行。结果明确显示跳过资源，独立于 IPA 成败。

### 流程与所有权

资源导出使用有效且绑定提取来源的 RomFS 能力，不能接受调用方伪造证明。输出前检查原生必需档案清单，并导出包含可选语言在内的全部 data 成员。惰性复制完成前保持提取/密钥 Worker 有效。目标是先前授权的输入目录；导出器不打开选择器、不扩大权限、不覆盖软件包/密钥、不上传。

读取保持≤64 KiB，合并为串行≤1 MiB 写入；进度只计成功写入。最终完整标记关闭才提交资源导出，之后 IPA 失败不会撤销已完成目录。失败先中止活动 writer，再检查句柄身份并逐项删除自有条目，保留未知新增/替换；未完成标记及清理报告必须可见。Web Lock 协调打包器标签页，不防御恶意外部写入或实现崩溃恢复。

暂存输出在自有 OPFS 中写入，独立校验实际 ZIP 字节，并重新验证复制到目标的不可变快照。目标 close 是提交边界，之前先清理自有暂存。不得在验证/关闭前报告成功、删除未知条目或对不确定 I/O 承诺回滚。生产 Blob IPA 上限 512 MiB，可能留存在浏览器内存；大资源保持独立。

ZIP64 必须显式选择、提供含元数据开销的预算及流式 sink，不是大内存回退。支持 ZIP64 运算不证明可安装。空间估计只代表 OPFS 建议容量，不是目标磁盘剩余空间或预留；实际配额/权限失败仍终止输出。具体常量和提交规则见源码。

基础模式只由可信选择及精确固定 shell 推导，修改已验证的附加内容数量数据初值，不改指令，保留无关散列。更改使旧签名失效；签名/安装另行完成。按原生指南将完整 data/Legacy Device Pack 复制到应用 Files/Documents。

### 验证

上方测试从仓库根执行。E2E 指南另列 staged-output/zip64 可选验证，ZIP 格式参考 PKWARE。合成流和 OPFS 测试目标不证明 OS 文件选择器、崩溃恢复、真实游戏磁盘容量或设备兼容性。
