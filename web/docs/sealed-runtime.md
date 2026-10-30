# Verified runtime and workers / 已验证运行时与工作线程

## English

### Bootstrap and ownership

[prepareSealedRuntime](../../web/sealed-runtime/clients.mjs) accepts a same-origin
runtime base and `manifestSha256` from the trusted application build, never from
user input or the unverified manifest. Before private reads, verify the top digest,
inventory, component manifests and every selected public byte. Reject redirects,
collisions, missing/mismatched bytes, service-worker control and missing browser
capabilities. Bootstrap is one-shot even on failure; reload after terminal failure.

The “sealed” API name does **not** mean page-level network isolation: production
adds no CSP lockdown. Workers use verified in-memory bytes without dependency
fetches; private inputs are not uploaded/cached. Public startup errors can name
components; private job errors stay sanitized. See [browser lifecycle](browser-lifecycle.md).

Use session factories instead of adapters' network-loading defaults. Assets use
JS/WASM, texture encoding requires separate Rust BC3, program rewriting uses
registered-release JS/Capstone/LZ4, extraction transfers its worker once, and
shader factories route initialized translator/Cross/AIR clients. No Python fallback
or synthetic program `caseName` is accepted. Exact interfaces and selection rules
are in [clients.mjs](../../web/sealed-runtime/clients.mjs).

Each component allows one active request. Idle client close releases its lease;
closing during its request terminates the session. Retire program, asset/texture
and shader groups only after their leases/work finish; retirement checks the whole
group before stopping anything. Keep output adapters and ASTC File leases until
consumption completes. Always close the session and observe cleanup errors.
`readPublicAsset` returns verified copies until close; opt-in shader-WASM release
only drops unaliased whole-buffer references, not copies or shared payloads, and
promises neither erasure nor immediate garbage collection.

### Build and maintenance

[Worker bundling](../../web/runtime-sealing/build-workers.mjs) emits closed NSP,
program, Cross, ASTC, signing and AIR entries, rejecting split outputs/literal
imports. Initialize once from exact public asset pairs, await `initialized:true`,
then permit private RPC. Never introduce arbitrary modules or URL fallbacks.

[Asset assembly](../../web/toolchain/asset-runtime-js/build.mjs) checks source guards,
backend receipts, zero host imports, exports and initialized versions, then emits
one worker, three WASM files, manifest and licenses. Preserve serial execution,
standalone transferable outputs and limits in the [contract](../../web/toolchain/asset-runtime-js/contract.mjs).
Keep Pillow-derived notices plus PNG zlib 1.3.1 and CASC zlib 1.2.13 notices despite
removing Python binaries. The internal [prebuilt-CASC route](../../web/casc-overlay-writer.mjs)
checks reservations/metadata but does not authenticate external pack bytes;
it is not a UI option. See [CASC storage](casc-storage.md).

[MD5 verification](../../web/checksum-verifier.mjs) hashes only the supplied view
in bounded yielding chunks, preserving cancellation and releasing per-call state.
Callers must not mutate pending input. Keep [hash-wasm 4.12.0 MIT notice](https://github.com/Daninet/hash-wasm/blob/v4.12.0/LICENSE).
Initialization is asynchronous; independent stream instances are released on success, cancellation or error. The pinned package embeds its MD5 WASM bytes, so hashing needs no network fetch. Collision-broken MD5 satisfies the archive format, not authenticity.

[Streaming SHA-256](../../web/toolchain/sha256-stream-wasm/adapter.mjs) uses
[pinned hactool/mbedTLS 2.6.1](../../web/toolchain/sha256-stream-wasm/source-pin.json),
fixed 256 KiB WASM memory and ≤64 KiB reads. Hash completed staging snapshots;
retain generation-token ownership, cancellation cleanup and source/license receipts.
The caller authenticates WASM; mock ABI checks do not. **Greater-than-4-GiB counter
carry remains unverified**, regardless of small-file smoke success.

### Commands (repository root)

```sh
node web/toolchain/asset-runtime-js/build.mjs --plan
bun web/toolchain/asset-runtime-js/build.mjs
bun web/runtime-sealing/build-workers.mjs
node --test web/tests/sealed-runtime.test.mjs web/tests/archive-codecs.test.mjs
node web/toolchain/sha256-stream-wasm/native-test.mjs --source /tmp/hactool-source-probe
node --test web/toolchain/sha256-stream-wasm/adapter.test.mjs
node web/toolchain/sha256-stream-wasm/build.mjs --source /explicit/pinned/hactool-checkout
node web/toolchain/sha256-stream-wasm/wasm-smoke.mjs
```

Use preinstalled pinned backends/toolchains; source/build receipts, synthetic tests,
actual browser execution and device parity are distinct validation gates.

## 简体中文

### 引导与所有权

prepareSealedRuntime 接受同源目录和可信应用构建提供的 manifestSha256，不能接受用户输入或
未验证清单自带摘要。读取私有内容前核对顶层摘要、库存/组件清单及全部所选公开字节。
重定向、路径冲突、缺失/错误字节、Service Worker 控制或能力缺失均拒绝；引导失败也不能重试，需刷新。

“sealed” 旧名**不表示页面级网络隔离**，生产没有 CSP 锁定。线程从已验证内存字节启动，
不再获取依赖；私有输入不上传/缓存。启动错误只说明公开组件，私有作业错误脱敏。

适配器使用会话工厂，不使用网络加载默认实现。资产为 JS/WASM，纹理编码必须有 Rust BC3，
程序是注册发布的 JS/Capstone/LZ4，提取线程只转交一次，着色器工厂路由已初始化客户端。
无 Python 回退，不接受合成程序 caseName；精确接口/选择规则见 clients 源码。

每组件一次只允许一个活动请求。空闲客户端关闭只释租约，自身请求期间关闭终止会话。
程序、资产/纹理、着色器组仅在租约/工作全部完成后退休，整组先检查再停止。输出适配器和 ASTC
File 租约保留至消费完成，最后关闭会话并观察清理错误。readPublicAsset 提供验证副本直至关闭；
显式释放着色器 WASM 只丢独占整缓冲区引用，不改副本/共享载荷，不承诺擦除或立即 GC。

### 构建与维护

Bun 打包封闭的六类线程，拒绝分片/残留字面量导入。精确公开资产对只初始化一次，收到
initialized:true 后才接受私有 RPC；不得引入任意模块或 URL 回退。
资产组装检查来源、后端回执、零宿主导入、导出及初始化版本，再输出一个 worker、三个 WASM、
清单和许可证。保留契约中的串行执行、独立可转移输出和预算。移除 Python 二进制不免除
Pillow 派生声明及 PNG zlib 1.3.1、CASC zlib 1.2.13 声明。内部 prebuilt-CASC 验证预留位置/
元数据但不认证外部包字节，也不是 UI 选项，详见 CASC 指南。

MD5 只散列传入视图，分块让出任务，保留取消并在所有结束路径释放状态；调用方期间不得改输入。
初始化为异步；独立流实例在成功、取消或失败后释放。固定包内嵌 MD5 WASM，无需联网获取。保留 hash-wasm 4.12.0 MIT 来源。MD5 已失去抗碰撞能力，只满足格式，不能证明真实性。

流式 SHA-256 使用固定 hactool/mbedTLS 2.6.1，固定内存 256 KiB、读取 ≤64 KiB。
散列已完成暂存快照，保留代次令牌所有权、取消清理及来源/许可证凭据。调用方认证 WASM，
模拟 ABI 不提供来源证明。**超过 4 GiB 计数进位尚未验证**，小文件冒烟不能覆盖。

从仓库根目录运行上方命令，预先准备固定后端/工具链。来源/构建回执、合成测试、真实浏览器与
设备一致性是不同验证关卡，不得互相替代。
