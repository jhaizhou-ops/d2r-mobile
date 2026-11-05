# Asset generation / 资源生成

## English

### Purpose and entry points

Generate art, HUD, tables, map labels and overlays from validated local game resources under the [input/extraction trust policy](input-extraction.md). Production uses JavaScript/WASM; Python remains a build/parity reference. Start with [production composition](../../web/production-assets.mjs), [sealed runtime](sealed-runtime.md) and [shell profiles](shell-profiles.md).

- [Production composition](../../web/production-assets.mjs): native art → Switch-upscaled HUD → derived tables → frontend panel → map-goal, mercenary/remaining art and native overlay. Generated counts must match trusted shell capacities.
- [Image adapter](../../web/asset-generator-adapter.mjs): single-use `image-default-frame` subset, not a complete pipeline. Only `frame.png`/`frame.sprite` are packageable. Explicit `enableV61Png` supports v61 PNG only, never v61 sprite output.
- [Linked adapter](../../web/asset-pipeline-adapter.mjs): trusted texture transforms, real BC3 encoding and CASC serialization. Native overlay mode requires named HUD/panel prerequisites and rejects concurrent ad-hoc replacements.
- Recipe APIs: [art](../../web/toolchain/art-recipes-js/recipes.mjs), [HUD](../../web/toolchain/hud-recipes-js/recipe.mjs), [remaining/mercenary](../../web/toolchain/remaining-art-js/recipes.mjs), [tables](../../web/toolchain/asset-transforms-js/transforms.mjs). Inputs and returned files are bounded byte arrays; see source for signatures and validators.

### Contracts that must remain

The authenticated-object API binds private capability identity and selection, not complete-content hashing; copied objects and disposed sessions fail. Source validation follows the input/extraction policy. Recipes, metadata hashes and capacities come from trusted configuration. Supply sealed workers; never invent missing assets or substitute fonts.

Verify source and complete encoded-PNG hashes, including paired release/browser digests. Decode complete atlases with real padded-frame layouts. HUD uses archive fonts and exact glyph inventories. Both supported revisions emit the complete 195-file map-goal inventory including Korean. Revision selection belongs to the shell profile.

Vault art must consume the personal sprite after its BC3 round trip. MainPanel preserves native block-copy geometry. Generated C texts feed `dm/derived.tsv`; never execute, compile or package them. Mercenary background generation does not replace overlay layout changes. See [overlay generation](asset-generation.md#overlay-generation) and [CASC storage](casc-storage.md).

Keep `secretLifetime: "archive"`. Consume output streams before disposal; propagate cancellation, terminate synchronous raster workers and clear owned intermediates/late results. These are bounded in-memory outputs, not OPFS image streams or secure heap erasure. Small adapters use copied 64-KiB chunks and reject unsafe/colliding paths. Final shell/assembly validation still applies.

Budgets are operation-specific, not total-memory guarantees. The small adapter allows 32 recipes; HUD permits 512 files/32 MiB. Inspect entry-point constants and backend limits before extending an inventory.

### Overlay generation

#### Purpose and entry points

[overlay-recipe-js](../../web/toolchain/overlay-recipe-js/recipe.mjs) implements the bounded native overlay recipe without Python, filesystem access or asset fetching. [generateNativeOverlay](../../web/native-overlay-generator.mjs) supplies authenticated assets/generated artwork, then delegates archive serialization to [CASC storage](casc-storage.md). The recipe itself returns ordered files and separate base/item_notes variants, not an installed game.

#### Revision selection

Both production v2.0.0 [shell profiles](shell-profiles.md) select release recipe `b811b90172f4eb40ec1bc89d903308e0c5ab88d0`: Korean/fifth language option and Xbox mapping, without later PlayStation behavior. Empty operation options use the reviewed b811 release; retired v1.0.18 revisions are rejected.

The internal `e3756f8942b40eba6c851e881828a5b1bda17fd6` revision adds later controller/language behavior; both it and the b811 release emit DM_OVERLAY_V1 manifests. No production profile selects it. Do not equate source availability with matching published shell behavior or relabel historical native parity as v2 proof. All revisions keep MOVIE_FFMPEG=0. Exact dependency closures and translation hashes are recorded in [release-contract-source.json](../../web/toolchain/overlay-recipe-js/release-contract-source.json), [contract-source.json](../../web/toolchain/overlay-recipe-js/contract-source.json).

#### Bounds and lifecycle

Recipe payload/output each cap at 128 MiB; individual files/manifest at 16 MiB. Map, row, asset-total and per-variant replacement bounds remain enforced in source. Canonical base64 and consumed-asset hashes are mandatory. Caller extraction provenance and final receipts remain independent gates. Optional ASTC metadata extends only the exact validated inventory; see [ASTC production](astc-production.md), never embed its external archive in the IPA.

#### Verify

```sh
node --test web/tests/asset-generation.test.mjs
node web/toolchain/verify-asset-sources.mjs overlay
python3 web/toolchain/overlay-recipe-js/scripts/generate-contract-source.py --check
# Requires historical b811b901 locally:
python3 web/toolchain/overlay-recipe-js/scripts/generate-contract-source.py --check --release
# Independent native recipe gate, currently pending a reviewed aggregate pin:
node web/toolchain/overlay-recipe-js/test/recipe-parity.mjs
```

Run from repository root. Native parity requires Python/Pillow and the packaged native source; `--wasm` additionally exercises the built exact PNG codec. Never populate a pending native aggregate with a JS-only digest. Preserve original source attribution and dependency notices. Inert contracts and historical synthetic parity do not establish current full-title, browser or device compatibility.

## 简体中文

### 用途与入口

按[输入/提取信任策略](input-extraction.md)使用经过验证的本地游戏资源生成图像、HUD、表格、地图标签和覆盖层。生产使用 JavaScript/WASM；Python 仅作构建/对照来源。入口及运行时、外壳文档见上方链接。

- 生产顺序：原生图像 → Switch 放大 HUD → 派生表 → 前端面板 → 地图目标、佣兵/剩余图像及覆盖层。派生数量必须符合可信外壳容量。
- 小图像适配器只处理单次默认态配方，不是完整流水线；仅 PNG/精灵结果可打包。显式 v61 支持只输出 PNG，不能冒充 v61 游戏精灵。
- 联接适配器执行可信变换、真实 BC3 编码和 CASC 序列化；原生覆盖模式必须具备前序 HUD/面板输入，不能混入临时替换。
- 图像、HUD、剩余图像/佣兵和表格 API 见源码链接。输入和文件结果均有字节预算，具体签名/校验器以代码为准。

### 不可放宽的契约

authenticated-object API 只绑定私有能力对象身份和对应选择，不代表已对全部内容求哈希认证；复制对象及失效会话会被拒绝。来源验证遵循输入/提取策略。配方、元数据摘要和容量来自可信配置；使用密封 worker，不虚构资源或替换字体。

检查源文件和完整 PNG 哈希，保留发布/浏览器摘要配对；完整图集必须保留真实填充步距。HUD 使用归档字体及完整字形清单。两个支持版本的地图目标均包含韩语，共 195 文件；版本由外壳选择，不是用户开关。

Vault 必须使用个人背景经 BC3 编解码后的像素；MainPanel 保留原生块复制几何。生成 C 文本只用于产生 dm/derived.tsv，不执行、编译或打包。佣兵背景不能代替覆盖层布局修改。

保留归档级密钥生命周期，所有输出流消费后再释放。取消要传入读取器，终止同步渲染 worker，并清理中间/迟到字节。这里是有界内存输出，不是 OPFS 图像流，也不保证堆安全擦除。小适配器采用 64-KiB 副本块并拒绝危险/冲突路径；最终外壳校验不可省略。

预算按操作分别限制，不保证总内存占用。小适配器最多 32 配方，HUD 最多 512 文件/32 MiB；扩展清单前检查入口常量及后端限制。

### 覆盖层生成

#### 用途与入口

overlay-recipe-js 不依赖 Python、文件系统或资源下载，执行有界原生覆盖层配方。generateNativeOverlay 提供绑定来源的资源/生成图像，再交 CASC serializer 写档。配方返回有序文件和独立 base/item_notes 变体，不负责安装游戏。

#### 配方版本

两个生产 v2.0.0 shell profile 都使用 b811b901 release 配方：韩语/第五语言和 Xbox 映射，不包含后来的 PlayStation 行为。空操作选项使用已审核的 b811 release；已退役的 v1.0.18 版本会被拒绝。

内部 e3756f8 配方增加后续控制器/语言行为；它与 b811 release 都生成 DM_OVERLAY_V1 清单，但没有生产配置选择它。源码存在不代表已发布 shell 支持，也不能把历史原生对照重新称为 v2 证明。所有版本保持 MOVIE_FFMPEG=0。精确依赖闭包和翻译散列见英文节链接的来源记录。

#### 边界与生命周期

配方载荷/总输出各最多 128 MiB，单文件/manifest 16 MiB；映射、行、图像总量和每变体替换数仍按源码限制。规范 base64 和消费资源散列必须验证，调用方来源及最终收据是独立关卡。可选 ASTC 只扩展精确验证清单，不得把外部存档塞入 IPA。

#### 验证

上方命令从仓库根运行；release 闭包检查要求本地有历史提交。原生配方 gate 需要 Python/Pillow 和打包原生源码，仍等待经审核的聚合 pin；不能用 JS 自算摘要填充。加 --wasm 可验证已构建的精确 PNG codec。保留原始来源和依赖声明。惰性合同及历史合成对照不证明当前完整游戏、浏览器或设备兼容性。

## Build and checks / 构建与验证

Repository-root commands; build tools: Node ≥22, Rust **1.99.1**, emsdk **4.0.20**. / 仓库根目录命令；工具仅用于构建。

```sh
npm --prefix web/toolchain/asset-generation-spike ci --ignore-scripts
npm --prefix web/toolchain/asset-generation-spike run prepare:core
npm --prefix web/toolchain/asset-generation-spike run prepare:runtime
node web/toolchain/image-kernels-rust/build.mjs
node web/toolchain/font-rasterizer-exact/build.mjs
node web/toolchain/verify-asset-sources.mjs art-recipes
node web/toolchain/verify-asset-sources.mjs asset-transforms
node web/toolchain/verify-asset-sources.mjs hud
node web/toolchain/verify-asset-sources.mjs remaining-art
node --test web/tests/asset-generation.test.mjs web/tests/casc-storage.test.mjs
node web/toolchain/asset-transforms-js/test/parity.mjs
# After building pinned PNG/font WASM / 固定 PNG/字体 WASM 构建后
node web/toolchain/art-recipes-js/test/parity.mjs --wasm
node web/toolchain/remaining-art-js/test/parity.mjs --wasm
```

PNG build: [archive codecs](archive-codecs.md). Without `--wasm`, parity uses oracle codecs; neither mode proves full-game/browser/device compatibility. Additional checks: [font](../../web/toolchain/font-rasterizer-exact/test/), [pixels](../../web/toolchain/image-kernels-rust/test/), [HUD](../../web/toolchain/hud-recipes-js/test/).

PNG 构建见归档编解码文档。非 WASM 模式借用参考编解码器；两种测试都不能证明真实游戏、浏览器或设备兼容性。更多验证见测试目录。

## Pins and notices / 固定来源与声明

- Pins: Pillow **10.2.0**, FreeType **2.6.0**, PNG/CASC zlib **1.3.1/1.2.13**, reference Pyodide **0.27.7**. Legacy FreeType requires security review and an upgrade plan. / 版本不可静默替换；旧 FreeType 上线前须安全审查和升级计划。
- Preserve all recipe/source manifests, guards and [native references](../../web/toolchain/asset-generation-spike/); [release bindings](../../web/toolchain/art-recipes-js/release-source-compatibility.json) reject drift. / 保留来源清单、检查器及原生参考，来源变化须重新验证。
- Preserve Pillow HPND notices: [pixels](../../web/toolchain/image-kernels-rust/reference/LICENSE), [fonts](../../web/toolchain/font-rasterizer-exact/reference/LICENSE). This software is based in part on the work of the FreeType Team. Use [FreeType License](../../web/toolchain/font-rasterizer-exact/reference/LICENSE-FreeType), not GPL; retain applicable [BDF](../../web/toolchain/font-rasterizer-exact/reference/LICENSE-bdf), [PCF](../../web/toolchain/font-rasterizer-exact/reference/LICENSE-pcf), [zlib](../../web/toolchain/font-rasterizer-exact/reference/LICENSE-FreeType-zlib) notices. / 保留 Pillow HPND；本软件部分基于 FreeType 团队成果，采用 FreeType 许可而非 GPL，并保留适用声明。
- Retain dependency metadata/repository licenses. No proprietary fonts/images are supplied; local resource use needs appropriate rights. / 保留依赖元数据及仓库许可；项目不提供专有字体/图像，用户资源用途须获适当授权。

A [historical original-default HUD comparison](https://github.com/n3crosis/d2r-mobile/actions/runs/39401602167) found actual pixel differences between native Pillow 11.1 and browser Pillow 10.2. That pre-v2 comparison does not validate or invalidate the current reviewed recipe, and matching a separately configured BASIC backend must not be described as parity with the original default.
