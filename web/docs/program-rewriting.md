# Program rewriting / 程序重写

## English

### Purpose and entry points

Rewrite the registered Switch D2R 3.3.94012 `main`/`sdk` executable pair and assemble Guest.framework. JavaScript orchestrates rewriting; real Capstone ARM64/LZ4 WASM provides decoding/decompression. Python is used for build-time audits and independent reference comparisons, not browser execution.

Start here:

- [Release catalog](../../web/program-release-catalog.mjs): exact ordered input hashes, build identity, source pin and limits.
- [Program adapter](../../web/program-rewriter-adapter.mjs): one-shot `generateProgram`, provenance, output validation and disposal.
- [Rewrite modules](../../web/toolchain/program-rewrite-spike/js/): numerical contracts, relocations, rules and output formatting.
- [Shell profiles](shell-profiles.md): authenticated host identities and program allowlists.

Current bundled shells are v2.0.0 for iOS 26/iOS 18; pre-v2 shell profiles are no longer registered. Registration and generation do not prove device compatibility.

### Main flow and trust boundary

1. Authenticate extraction provenance for the selected inputs. Snapshot both module buffers before asynchronous hashing and match the complete ordered `main`, `sdk` tuple against the repository catalog.
2. Reject partial/unknown matches and releases outside the selected shell's allowlist before creating a worker. The worker independently checks input digest, available build ID and pinned rewriter source.
3. Rewrite main then sdk serially, preserving native byte-preimage assertions, relocation semantics and output formats. Validate source receipts, fixups and main rules against the actual generated image.
4. Assemble verified images into Guest.framework and return the framework/plist plus required `dm/` fixup, gate and main-cut sidecars. Consume output streams before calling `dispose()`.

Caller version labels, filenames, source hashes, custom shell metadata or offsets cannot override admission. Synthetic fixture/catalog routes are isolated from sealed production. Compatibility diagnostics expose fixed codes, not private hashes, paths or raw exceptions.

### Limits and maintenance rules

Input ceilings are 32 MiB/module and 64 MiB combined; combined rewrite outputs are capped at 128 MiB. Detailed segment/image limits live in the catalog. Cancellation or failure closes the worker and wipes owned buffers; readers return independent chunks. These are rejection bounds, not measured peak-memory guarantees.

Apple is the only supported target. New fixups declare `TARGET 0`; legacy omission is accepted, but nonzero/duplicate targets are rejected. Ad-hoc Guest construction is not distribution signing. Preserve BigInt for large immediates, safe-integer addresses and Capstone re-decoding checks rather than replacing the decoder.

Adding a release needs independently established executable identities, reviewed native rules/host ABI, matching source pins, regenerated sealed assets and an authenticated compatible shell, followed by separate private-input/device acceptance. A registry row or self-hashed shell is insufficient.

### Build and verify

Run from repository root; see [verification guidance](build-policy.md#verification-and-e2e-tooling). The heavy profile may install official tools and Chromium dependencies; it is local-only.

```sh
python3 web/toolchain/program-rewrite-spike/audit-apple-source.py
python3 web/toolchain/program-rewrite-spike/audit-js-source.py
bash web/toolchain/program-rewrite-spike/build-wasm.sh
node web/verification/verify.mjs program --provision
node --test web/tests/program-rewriting.test.mjs
```

The build creates directories and audits sources before dependency downloads. The profile compares synthetic Chromium outputs with native Python oracles; passes are not game/device proof.

### Source and licensing

[Source receipt](../../web/toolchain/program-rewrite-spike/upstream-source.sha256) pins [patch_nso.py](../../dm/tools/patch_nso.py); [Apple baseline](../../web/toolchain/program-rewrite-spike/apple-source-baseline.json) records reviewed target-specific changes. [Toolchain lock](../../web/toolchain/program-rewrite-spike/toolchain-lock.json) pins Capstone 5.0.3, LZ4 1.10.0 and Emscripten 4.0.20. Preserve Capstone BSD-3-Clause/LZ4 BSD-2-Clause notices in `dist/licenses`. Removing Python runtime dependencies does not authorize removing source-derived notices. Results remain `repository-registered-unverified`, with `realGameVerified: false`.

## 简体中文

### 用途与入口

重写已登记的 Switch D2R 3.3.94012 main/sdk 程序组合，并组装 Guest.framework。JavaScript 负责调度和重定位，真实 Capstone ARM64/LZ4 WASM 负责解码/解压。Python 仅用于构建时审计和独立参考比较，不在浏览器中执行。

维护入口：

- [版本目录](../../web/program-release-catalog.mjs)：精确有序输入摘要、构建身份、源码固定值及上限。
- [程序适配器](../../web/program-rewriter-adapter.mjs)：单次 generateProgram、来源验证、输出校验及释放。
- [重写模块](../../web/toolchain/program-rewrite-spike/js/)：数值契约、重定位、规则和输出格式。
- [外壳配置](shell-profiles.md)：认证宿主身份和程序白名单。

当前随包外壳为 v2.0.0 iOS 26/iOS 18，已移除 v2 之前的外壳登记。登记成功或生成 framework 均不能证明真实游戏/设备兼容性。

### 主流程与信任边界

1. 验证与当前选择绑定的提取来源，在异步摘要前同步复制两个模块，再将完整有序 main/sdk 组合匹配仓库目录。
2. 创建线程前拒绝部分匹配、未知版本和不在外壳白名单中的版本。线程还须独立检查字节摘要、已有构建 ID 和固定重写器源码。
3. 按 main 后 sdk 串行重写，保留原生字节前置断言、重定位语义及输出格式；针对实际生成镜像验证源码回执、fixup 和 main 规则。
4. 将验证过的镜像组装为 Guest.framework，返回 framework/plist 及必需的 dm/ fixup、gate、main-cut 侧车。读取输出完毕后再 dispose。

调用方版本标签、文件名、源码摘要、自定义外壳元数据或偏移均不能绕过门禁。合成夹具/目录路径与密封生产隔离；兼容性诊断只暴露固定代码，不泄露私有摘要、路径或原始异常。

### 限制与维护规则

输入上限每模块 32 MiB、总计 64 MiB；重写输出总计最多 128 MiB，段/镜像细分限制以目录实现为准。取消或失败会关闭线程、擦除拥有的缓冲区，读取器返回独立分块。这些是拒绝阈值，不是峰值内存保证。

只支持 Apple 目标。新 fixup 声明 TARGET 0，旧格式可缺省，但非零或重复目标拒绝。Guest 的 ad-hoc 构建不是分发签名。保留大立即数 BigInt、安全整数地址及 Capstone 重新解码检查，不能替换为简化手写解码器。

新增版本需要独立确认可执行身份，审查原生规则/宿主 ABI，匹配源码固定值、重新生成密封资产、认证兼容外壳，再单独完成私有输入/设备验收。仅增加目录行或外壳自报摘要远远不够。

### 构建与验证

在仓库根目录运行英文段命令，前置工具见[验证说明](build-policy.md#verification-and-e2e-tooling)。重型 profile 可能安装官方工具和 Chromium 系统依赖，仅供本地显式运行。

构建脚本先创建输出目录并执行全部源码审计，再下载依赖。program profile 准备合成夹具、原生 Python oracle，并逐字节比较 Chromium 工作线程输出；合成通过不是真实游戏或设备证明。

### 来源与许可

[源码回执](../../web/toolchain/program-rewrite-spike/upstream-source.sha256)固定 patch_nso.py，[Apple 基线](../../web/toolchain/program-rewrite-spike/apple-source-baseline.json)记录已审查的目标专用变化。[工具链锁定文件](../../web/toolchain/program-rewrite-spike/toolchain-lock.json)固定 Capstone 5.0.3、LZ4 1.10.0、Emscripten 4.0.20。保留 dist/licenses 中 Capstone BSD-3-Clause/LZ4 BSD-2-Clause 通知；移除 Python 运行时不授权删除派生源码许可。结果仍标记 repository-registered-unverified、realGameVerified: false。
