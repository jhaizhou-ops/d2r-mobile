# CASC storage / CASC 存储

## English

### Read logical assets

[createCascLogicalAssets](../../web/casc-logical-reader.mjs) connects a selected immutable RomFS index to bounded `logicalAssets.read`. It follows native [dmx.py](../../dm/tools/dmx.py) path aliases and last-duplicate-wins semantics. Only key-layout-0 is supported; unsafe paths, missing resources and inconsistent locations fail, without cross-image fallback.

The manifest is indexed with streaming reads; archive payloads remain lazy and BigInt-addressed. Reads retain the [archive codec](archive-codecs.md) limits and mandatory MD5/CKey checks. Increasing caller maxBytes does not relax decoder bounds. This reader does not authenticate NCA, choose versions or establish game compatibility; [input extraction](input-extraction.md) owns provenance.

### Serialize overlays and supplemental packs

[overlay.mjs](../../web/toolchain/casc-overlay-js/overlay.mjs) implements native TVFS/CFT/path-trie/BLTE serialization. `createPinnedCascOverlayTransform` binds pinned hash-wasm and zlib 1.2.13; arbitrary compression substitutions change output bytes. Ordinary overlays retain 64 MiB input/output, 16 MiB table/replacement and 4,096-job limits.

`inspectCascPackStorageSpecs` resolves original specs; `patchCascPrebuiltReferences` resolves every old slot before copying and changes only selected EKeys/lengths. `createCascPackStorageLookup` reuses one immutable snapshot, optionally indexing large batches. Metadata supports at most 32,768 requests and 8 MiB path bytes. Duplicate/ambiguous/unaligned matches reject. These helpers do not authenticate path/key associations or external pack bytes, reserve locations or prove payload/spec agreement.

[writeCascPack](../../web/casc-pack-writer.mjs) consumes jobs serially into caller-owned staging. Preserve original n/z/raw-BLTE/zlib-BLTE specs; jobs replace existing paths, never add them. Require explicit pack/count budgets, collision checks against actual original/planned inventories, trusted streaming compression and incremental SHA-256. Input chunks must own ordinary backing buffers ≤1 MiB and remain stable until consumed. No whole-record/pack concatenation or RAM fallback is allowed.

The result exposes replayable source, index, records, pack metadata and dispose. Associate original oldEkeys before updating TVFS. The external-reference transform accepts bounded finalized metadata only; it preserves ordinary output order and rejects ordinary/external overlap. It never copies supplemental pack bytes into the overlay or IPA. Metadata receipts are not authentication tokens for imported files.

Keep sources/stages alive through consumption, then await disposal. Cancellation requests iterator closure and drains pending writes before deleting only owned staging. Pending cleanup is reported through a cleanup promise, not successful rollback; retain it and any late-acquired stage. Synchronous blocked code and noncooperative sources cannot be forcibly cancelled. Detailed API shapes and numeric constants remain next to their implementations.

### Verify and preserve provenance

Run from repository root:

```sh
node --test web/tests/casc-storage.test.mjs
node web/toolchain/verify-asset-sources.mjs casc-overlay
node web/toolchain/casc-overlay-js/test/parity.mjs
node --test web/toolchain/casc-overlay-js/test/prebuilt.mjs
# After building the pinned codec with Emscripten 4.0.20:
node web/toolchain/casc-overlay-js/test/parity.mjs --wasm
CASC_PACK_NATIVE_CODEC="$PWD/web/toolchain/png-codec-exact/dist/png_codec_native.so" node --test web/tests/casc-storage.test.mjs
```

Preserve [source provenance](../../web/toolchain/casc-overlay-js/source-provenance.json), [hash-wasm notice](https://github.com/Daninet/hash-wasm/blob/v4.12.0/LICENSE) and codec licenses. Default pack tests use mock staging/host compression; optional native and WASM checks exercise different boundaries. Synthetic serializer/index/hash agreement does not prove browser quotas, large-pack performance, real-game compatibility or installation.

## 简体中文

### 读取逻辑资源

createCascLogicalAssets 把选定且不可变的 RomFS 索引连接到有界 logicalAssets.read。沿用原生 dmx.py 的路径别名及最后重复项胜出规则。只支持 key-layout-0；危险路径、资源缺失或位置不一致直接失败，不跨镜像回退。

清单流式建立索引，存档载荷仍惰性读取且偏移使用 BigInt。保留 archive codec 上限及必需的 MD5/CKey；调用方增大 maxBytes 不能放宽解码限制。此 reader 不认证 NCA、不选版本、不证明游戏兼容，来源由输入提取负责。

### 覆盖层与补充包

overlay.mjs 实现原生 TVFS/CFT/路径树/BLTE 序列化。生产 wrapper 绑定固定 hash-wasm 和 zlib 1.2.13，任意替换压缩器会改变字节。普通覆盖层保持输入/输出 64 MiB、单表/替换 16 MiB、4,096 任务上限。

规格检查解析原始存储方式；引用补丁先解析全部旧槽再复制，只改选定 EKey/长度。可复用 lookup 只建立一次不可变快照，大批量可使用索引。元数据最多 32,768 请求和 8 MiB 路径字节；重复、歧义或未对齐命中拒绝。这些 helper 不认证路径关联/外部包，不预留位置，也不证明载荷符合规格。

writeCascPack 串行消费任务到调用方自有暂存。沿用原始 n/z/raw-BLTE/zlib-BLTE，只替换已有路径、不新增。必须显式提供包/数量预算、实际原档和计划输出的冲突检查、可信流式压缩及增量 SHA-256。输入普通底层缓冲最多 1 MiB，消费前保持不变；不得拼接整记录/整包或回退 RAM。

结果提供可重放来源、索引、记录、包元数据和 dispose。修改 TVFS 前关联原始 oldEkey。外部引用变换只接收有界的最终元数据，保留普通顺序并拒绝普通/外部重叠；不会把补充包载荷复制进 overlay 或 IPA。元数据收据不是导入文件的认证令牌。

消费期间保留来源/暂存，完成后等待释放。取消请求关闭迭代器，先排空写入再删除自有暂存。未完成清理以 Promise 报告，不能称为回滚成功；继续保留它和迟到 stage。同步阻塞和不配合的来源无法强制取消。完整接口形状/常量留在实现旁。

### 验证与来源

上方命令从仓库根执行；WASM 检查需先用 Emscripten 4.0.20 构建固定 codec。保留源码来源、hash-wasm 声明及 codec 许可证。默认测试用暂存/宿主压缩替身，可选原生和 WASM 各有证据边界；合成字节/索引/散列一致不证明浏览器配额、大包性能、真实游戏或安装兼容性。
