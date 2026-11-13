# Archives and codecs / 存档与编解码器

## English

### Read and stage safely

[archive-reader.mjs](../../web/archive-reader.mjs) decodes resolved native n/z or table-bearing BLTE N/Z records, not whole archives. Whole-record limits remain 16 MiB, with mandatory block/CKey MD5 checks. Integrity is not provenance: extraction must establish path/archive associations and immutable sources.

[archive-stream-reader.mjs](../../web/archive-stream-reader.mjs) adds `stageArchiveRecord` with explicit encoded/decoded budgets and an owned OPFS stage, not a larger in-memory fallback. It verifies encoded bytes before decoding, rechecks reread blocks, and publishes a read capability only after exact size, CKey and finalization succeed. Reads stay at most 64 KiB; decoded staging reads are at most 1 MiB. Dispose completed stages after consumers finish. Observe pending cleanup promises on failures; timeout does not mean deletion completed.

[archive-read-rpc.mjs](../../web/archive-read-rpc.mjs) registers immutable source closures on a dedicated MessagePort pair. Descriptors come from trusted initialization, not caller paths/keys; offsets remain exact decimal/BigInt. Requests are bounded and backpressured. Cancellation, revocation, malformed traffic or deadline closes the job transport. Parent ownership of its job directory survives Worker termination.

[zip-reader.mjs](../../web/zip-reader.mjs) reads bounded ZIP32 shell STORE/DEFLATE entries and verifies exact lengths/CRC. It rejects unsafe/conflicting paths, symlinks, encryption, ZIP64 input and ambiguous layouts. ZIP64 output is separate. A readable ZIP still must pass the [shell contract](shell-profiles.md). Do not buffer whole archives or claim CRC proves authenticity.

### Exact encoders

[png-codec-exact/codec.mjs](../../web/toolchain/png-codec-exact/codec.mjs) preserves Pillow 10.2.0 PNG/zlib 1.3.1 and CASC zlib 1.2.13 in one fixed-memory WASM. Do not replace them with generic browser compression or replace encoded-PNG hash guards with pixel hashes. `compressZlibStream` requires explicit budgets, exclusive codec ownership and ordinary chunks whose entire backing buffer is ≤1 MiB. Use owned slices, consume output with backpressure, and signal cancellation rather than abandoning iterators. Full parameters/limits live in the codec and stream implementation.

[texture-spike/browser-texture.mjs](../../web/toolchain/texture-spike/browser-texture.mjs) wraps native NVN/bcdec decoding with validated descriptors and owned results. Supported BC textures retain native color/channel semantics; unsupported shapes/layouts fail. Production BC3 encoding uses dependency-free [Rust/WASM](../../web/toolchain/texture-spike/rust-bc3/src/lib.rs), then the native packer. The retained Python encoder is a parity tool, not the production backend. Synchronous native cancellation requires Worker termination. WASM limits do not bound total browser RSS.

### Build and verify

Run from repository root with Emscripten 4.0.20 and, for BC3, Rust 1.99.1:

```sh
node --test web/tests/archive-codecs.test.mjs
node web/toolchain/png-codec-exact/build.mjs
node web/toolchain/png-codec-exact/test/make-fixtures.mjs
node web/toolchain/png-codec-exact/test/equivalence.mjs
node web/toolchain/png-codec-exact/test/stream-smoke.mjs
bash web/toolchain/texture-spike/build-wasm.sh
node web/toolchain/texture-spike/build-bc3.mjs
python3 web/toolchain/texture-spike/native-fixtures.py
node --test web/toolchain/texture-spike/texture.test.mjs
PLAYWRIGHT_ENTRY=/absolute/path/to/playwright/index.mjs node web/toolchain/texture-spike/browser-smoke.mjs
```

Build scripts enforce source hashes, compiler versions and memory flags; do not bypass them or commit generated artifacts. Preserve Pillow, both zlib, native repository/bcdec and [deko3d](https://raw.githubusercontent.com/devkitPro/deko3d/master/LICENSE) notices beside distributed artifacts. See [E2E tooling](build-policy.md#verification-and-e2e-tooling) for optional texture checks. Synthetic/native/WASM parity have distinct coverage; none alone establishes real-game, browser OPFS or device compatibility.

## 简体中文

### 安全读取与暂存

archive-reader 解码已解析的原生 n/z 或带表 BLTE N/Z 记录，不读取整档。整记录仍限 16 MiB，块/CKey MD5 必须通过。一致性不等于来源证明；提取层必须绑定路径/存档及不可变来源。

独立 stageArchiveRecord 接口要求显式编码/解码预算和自有 OPFS 暂存，不回退到更大内存缓冲。先验证编码字节，再解码；重读块再次检查。只有大小、CKey 和最终化都成功才发布读取能力。来源每读最多 64 KiB，解码暂存每读最多 1 MiB；消费完成后释放。失败时观察清理 Promise，超时不等于已删除。

archive-read-rpc 在专用 MessagePort 上注册不可变来源闭包；描述符来自可信初始化，不是调用方路径/键，偏移保持精确。请求有界并背压；取消、撤销、畸形流量或截止会关闭作业运输。线程终止后父级仍拥有作业目录清理责任。

ZIP reader 读取有界 ZIP32 STORE/DEFLATE shell 条目，校验准确长度/CRC，拒绝危险/冲突路径、符号链接、加密、ZIP64 输入和歧义布局。ZIP64 输出属于独立 writer。可读 ZIP 仍须通过 shell 合同，不得缓冲整档或把 CRC 称为真实性证明。

### 精确编码

PNG codec 在单个固定内存 WASM 保留 Pillow 10.2.0/zlib 1.3.1 PNG，以及 CASC zlib 1.2.13。不得替换为浏览器通用压缩，也不得把编码 PNG 散列守卫换成像素散列。流式 zlib 要求显式预算、独占 codec 和整个底层缓冲≤1 MiB 的普通块。使用自有 slice、背压消费并发送取消信号，不要仅丢弃迭代器。具体参数和常量以源码为准。

纹理适配器用经过验证的描述符包装原生 NVN/bcdec，返回自有结果。支持的 BC 纹理保留原生颜色/通道语义，不支持的形状/布局直接失败。生产 BC3 使用无依赖 Rust/WASM 加原生 packer；保留的 Python encoder 是对照工具，不是生产后端。同步原生调用通过终止 Worker 取消；WASM 内存上限不是浏览器总 RSS 上限。

### 构建与验证

上方命令从仓库根执行，使用 Emscripten 4.0.20，BC3 还需 Rust 1.99.1。脚本强制源码散列、编译器版本及内存选项，不要绕过或提交生成产物。分发时保留 Pillow、两版 zlib、仓库/bcdec 和 deko3d 声明。可选纹理检查见 E2E 指南。合成、原生、WASM 对照各有覆盖边界，单项通过不证明真实游戏、浏览器 OPFS 或设备兼容性。
