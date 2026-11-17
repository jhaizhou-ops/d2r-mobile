# Application UI and identity / 应用界面与标识

## English

### Use and recovery

Optional advanced fields customize the completed **unsigned IPA** before
[local signing](ipa-signing.md). Blank fields preserve the pinned shell; an
entirely blank request returns the original Blob without parsing or rewriting.
This does not update the game, executable, shell or deployment target.

- Display name: 1–80 Unicode characters, without controls or surrounding whitespace.
  Updates bundle display/name fields, not the application directory or executable.
- Bundle ID: ≤255 ASCII letters, digits, hyphens and periods, with at least two
  nonempty segments and no wildcard. Changing it may create a new app container;
  existing saves/settings are not migrated.
- Release version: three canonical decimal components, e.g. `2.1.0`, ≤32 characters.
  Build version is independent: one to three components, first 1–9999, later 0–99,
  without leading zeros/development suffixes. Neither silently changes the other.
- Icon: local PNG ≤4 MiB, square 256–1024 pixels, non-interlaced 8-bit RGB/RGBA,
  fully opaque. Reject palettes, APNG, malformed checksums and decode overflow.
  Metadata is discarded without color-profile conversion.

Fourteen loose RGB PNGs replace primary icon declarations; original loose icons
and **Assets.car remain byte-identical**. This is not a rebuilt asset catalog.
Modern dark/tinted/layered variants, real Home Screen display and App Store
eligibility are not guaranteed. No Canvas, remote image service or browser image
decoder is used.

Customization failure preserves an explicitly labeled **original, uncustomized
unsigned IPA**, without silently signing it. Signing failure instead retains the
customized unsigned IPA. The signer checks its actual bundle ID against the
profile; a profile for the old ID does not automatically authorize the new one.
Its separate application-ID length limit includes prefix and separator and
reports `SIGNING_APPLICATION_ID_TOO_LONG` distinctly. Private inputs stay local.

### Maintenance boundaries

[Customization](../../web/app-customization.mjs), [plist parsing](../../web/app-customization-plist.mjs)
and [icon decoding](../../web/app-customization-icon.mjs) own exact validation/budgets.
Preserve strict plist rejection, localized `InfoPlist.strings` conflict handling,
bounded ZIP reads and immutable Blob slices. Only root plist/new icons change;
executables, game resources, Guest plist and shader receipts stay unchanged.
No whole-IPA/resource-tree copy is permitted; archive ceiling remains 512 MiB.

The [frontend-panel transform](../../web/frontend-panel-transform.mjs) implements
five source-pinned column copies for one 401×1080 frame, changing only rows
980–1039 and reading original pixels. The caller supplies an independently trusted
input hash. Source: [gen_fe_rightpanel.py](../../dm/ios/tools/gen_fe_rightpanel.py),
SHA-256 `c68ea1e6d5c720eca576b298708cd637d8d2d898d192e34a92c41121a52db424`.
Its recipe composes existing BC3/CASC machinery, not a complete parity claim.

### Validation and references

Run from repository root:

```sh
node --test web/tests/application-ui.test.mjs
```

Apple references: [bundle ID](https://developer.apple.com/documentation/bundleresources/information-property-list/cfbundleidentifier),
[release version](https://developer.apple.com/documentation/bundleresources/information-property-list/cfbundleshortversionstring),
[build version](https://developer.apple.com/documentation/bundleresources/information-property-list/cfbundleversion),
[icons](https://developer.apple.com/library/archive/documentation/General/Reference/InfoPlistKeyReference/Articles/CoreFoundationKeys.html).
Accepted inputs intentionally form a narrower subset. Synthetic tests do not
replace browser/device icon validation; see [asset generation](asset-generation.md).

## 简体中文

### 使用与恢复

高级字段先修改已完成的**未签名 IPA**，再交给可选[本地签名器](ipa-signing.md)。
空字段保留外壳原值；全部为空时直接返回原 Blob，不解析或重写。
这不更新游戏、可执行文件、外壳或最低系统版本。

- 显示名称：1–80 个 Unicode 字符，无控制字符或首尾空白；修改包名称字段，不改目录或可执行文件名。
- 包标识：最多 255 个 ASCII 字母、数字、连字符和点，至少两个非空分段，不支持通配符。
  修改可能创建新应用容器，不迁移存档或设置。
- 发布版本：三个规范十进制分段，例如 `2.1.0`，最多 32 字符。构建版本独立，
  一至三个分段，首段 1–9999，其余 0–99，不支持前导零/开发后缀，两者不联动修改。
- 图标：本地 PNG ≤4 MiB，正方形 256–1024 像素，非交错 8 位 RGB/RGBA，完全不透明。
  拒绝调色板、APNG、错误校验和与解码超限；丢弃元数据，不转换色彩配置。

生成 14 个独立 RGB PNG 并更新主图标声明，原有独立图标和 **Assets.car 字节不变**。
这不是重新编译资产目录，不保证现代深色/着色/分层外观、真实主屏幕或 App Store 适用性。
不使用 Canvas、远程服务或浏览器图像解码器。

定制失败保留明确标注的**原始未定制 IPA**，不自动签名；签名失败则保留定制后的未签名 IPA。
签名器核对实际包标识，旧标识的描述文件不会自动授权新标识。签名应用标识的长度限制另包含
前缀和分隔点，超限独立报告 `SIGNING_APPLICATION_ID_TOO_LONG`。私有输入留在本地。

### 维护与验证

精确规则/预算由上方 customization、plist 和 icon 源码负责。保留严格 plist 拒绝、
本地化 InfoPlist.strings 冲突处理、有界 ZIP 读取及不可变 Blob 切片。仅根 plist/新图标可变，
可执行文件、游戏资源、Guest plist 和着色器凭据不变；不能复制整 IPA/资源树，整包上限仍为 512 MiB。

面板变换按上方固定来源执行五次列复制，输入单帧 401×1080，只改 980–1039 行且读取原始像素。
调用方必须提供独立可信的输入摘要；配方组合已有 BC3/CASC，不代表完整一致性证明。
从仓库根目录运行上方测试。Apple 格式依据见英文链接，本实现只接受其较窄子集；
合成测试不能代替浏览器/真机图标验收，资源生成细节见对应指南。
