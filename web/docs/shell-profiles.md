# Shell profiles / 外壳配置

## English

### Select a pinned public shell

Production bundles iOS 26 and iOS 18 shells from the
[official v2.0.0 release](https://github.com/jhaizhou-ops/d2r-mobile/releases/tag/v2.0.0),
bound to source `b811b90172f4eb40ec1bc89d903308e0c5ab88d0`.
[shell-profiles.mjs](../../web/shell-profiles.mjs) is the closed registry;
[v2 standard](../../web/toolchain/shell-release-evidence/v2.0.0.json) and
[iOS 18](../../web/toolchain/shell-release-evidence/v2.0.0-ios18.json) records hold
exact release URLs, sizes, archive/member hashes and modes. Runtime assembly
downloads verified public archives; binaries are not checked into these records.
Never accept package-supplied pins or reuse offsets from another profile.

Both plist and Mach-O targets must match. The no-DLC derivation changes only the
unique `_aoc_owned.v` initializer in file-backed `__DATA,__data`, from -1 to zero.
Verify original hash, symbol/section/offset, preimage and full resulting hash.
This invalidates the existing executable signature: sign before installation.
It does not prove base-only gameplay.

### Compatibility and maintenance

Both v2 hosts require EXP **100 scalar values**, VTPART 11 rows, VTROW 562,
VTSET 35 and TAROW 429. [Capacity evidence](../../web/toolchain/shell-release-evidence/v2.0.0-derived-capacities.json)
and [iOS 18 evidence](../../web/toolchain/shell-release-evidence/v2.0.0-ios18-derived-capacities.json)
retain function hashes and instruction windows. These are compiled guards,
not inferred array spacing or generator-derived declarations. Verify the exact
shell/executable identity, then require exact generated counts; missing/unknown/
mismatched capacities reject. Counts alone do not validate table values.

Required metadata is `CHAIN=420aeb7c8bc6`, `PRODUCT=3.3.94012-r44`,
`SHADER_SRC=be59c864f087b377`, SPIRV-Cross `1.4.363.0`.
The publisher's exact build fingerprints are not reproduced by committed chain
inputs. Like native `d2rm_pack.py`, align directory/per-shader/c1 labels only after
the **complete normalized source digest** matches SHADER_SRC. Keep reflection,
compiled-library checks and genuine compiler receipts; never rewrite provenance
to impersonate a native build. See [shader pipeline](shader-pipeline.md).

Targets select matching b811 art/overlay recipes, not arbitrary revisions.
Optional ASTC inventory is exactly 15 files for the current b811 profiles. Missing/extra outputs reject. b811 includes Korean/Xbox UI,
not later PlayStation glyphs or iOS VP8 additions. The iOS loader does not consume
Android's overlay.manifest validation path; browser integrity checks still apply.

Pre-v2 shell profiles and their release-specific evidence have been retired.
Both current v2.0.0 targets, iOS 26 and iOS 18, remain supported with their exact
independent evidence. Retired IDs reject rather than falling back.

### Recheck without execution

```sh
python3 web/toolchain/shell-release-evidence/verify-v2-shell-evidence.py \
  --profile v2.0.0 --archive /path/to/D2RMobile-shell-build-input.zip
python3 web/toolchain/shell-release-evidence/verify-v2-shell-evidence.py \
  --profile v2.0.0-ios18 --archive /path/to/D2RMobile-shell-ios18-build-input.zip
node --test web/tests/shell-profiles.test.mjs
```

Run from repository root. The helper reads/validates members and hashes the
four-byte derivation in memory; it never writes or executes a shell. Preserve
source-comparison records and later browser/private-input/device acceptance gates.

## 简体中文

### 选择固定公开外壳

生产捆绑官方 v2.0.0 的 iOS 26/iOS 18 外壳，绑定源码 b811b90172f4eb40ec1bc89d903308e0c5ab88d0。
上方封闭注册表和两份 JSON 记录精确发布 URL、大小、归档/成员摘要及模式；组装时下载验证，
记录中不提交二进制。不可接受输入包自带固定值，也不能复用另一配置的偏移。

plist 和 Mach-O 目标都必须匹配。无 DLC 派生只把文件支持的 __DATA,__data 中唯一
_aoc_owned.v 初值从 -1 改成零，必须检查原摘要、符号/节/偏移、前像和完整结果摘要。
原可执行文件签名因此失效，安装前需签名；这不证明无 DLC 游戏实际可玩。

### 兼容与维护

两个 v2 宿主均要求 EXP 100 个**标量**、VTPART 11 行、VTROW 562、VTSET 35、TAROW 429。
容量证据保留函数摘要和指令窗口，依据编译后的保护逻辑，不是数组间距或生成输入计数。
先验证精确外壳/可执行身份，再检查生成计数完全相等；未知、缺失或冲突必须拒绝。计数不证明表值正确。

必须匹配 CHAIN=420aeb7c8bc6、PRODUCT=3.3.94012-r44、SHADER_SRC=be59c864f087b377、
SPIRV-Cross 1.4.363.0。当前链输入不复现发布者完整工具指纹；只能像原生打包器一样，在
**完整规范化源码摘要**匹配后调整目录/单着色器/c1 标签，不能伪造来源。反射、编译库和真实
编译器凭据检查仍必需，详见着色器指南。

目标选择对应 b811 图像/覆盖配方，不开放任意修订。b811 可选 ASTC 精确 15 文件，
缺项/多项拒绝。b811 含韩文/Xbox UI，不含后来的 PlayStation 字形或 iOS VP8。
iOS 加载器不走 Android 的 overlay.manifest 校验，但浏览器完整性检查仍保留。

v2 之前的外壳配置及其专属证据已移除。当前 v2.0.0 的 iOS 26 与 iOS 18
目标及各自精确证据均保留。旧版 ID 会拒绝，不会回退到其他配置。

### 无执行复核

从仓库根目录运行上方两个验证命令和功能测试。辅助程序读取并验证成员，在内存中计算四字节
派生摘要，不写入/执行外壳。保留来源比较记录，浏览器、私有输入和设备验收另行完成。
