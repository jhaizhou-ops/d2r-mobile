# Apple TV（tvOS）

社区移植，作者 [@yinqingyan](https://github.com/yinqingyan)，经作者同意按 MIT 许可贡献。**开发者预览**：在 Apple TV 4K（第三代，A15）实机上到达了职业选择画面；后续社区实测已进入游戏并操作局内设置；长期稳定性、手柄全键位和可靠存档仍未完成验证，维护者没有实机测试过。

Community port by [@yinqingyan](https://github.com/yinqingyan), contributed under the MIT License with the author's consent. **Developer preview**: it reached the character-selection screen on a real Apple TV 4K (3rd generation, A15); later community testing reached gameplay and in-game settings; long-session stability, full controller mapping and reliable saves are not fully verified, and the maintainers have not tested it on real hardware.

## 是什么 / What it is

- 独立的 Xcode 目标 `DmTV`（写在 `dm/ios/project.yml` 里），和 iOS 共用宿主、图形层与手柄适配（`ios/Sources/dm_gamepad.m`）；入口是 `dm/tvos/main.m`，iOS 目标不受影响。
- 只认扩展手柄（Xbox / PlayStation / 8BitDo 等）；Siri 遥控器不是扩展手柄，玩不了。没有触屏、iCloud、资源下载界面。
- A separate Xcode target `DmTV` (in `dm/ios/project.yml`) sharing the host, renderer and gamepad adapter with iOS; entry point `dm/tvos/main.m`; the iOS target is unchanged.
- Extended gamepads only (Xbox / PlayStation / 8BitDo …); the Siri Remote is not an extended gamepad. No touch controls, iCloud or download screens.

## 构建 / Build

```bash
dm/tvos/build.sh                  # 实机 / real Apple TV（在 Xcode 里登录能签 tvOS 的账号 / sign in Xcode with an account that can sign tvOS apps）
DM_TV_NOSIGN=1 dm/tvos/build.sh   # 不签名，只核能不能编过 / unsigned, compile check only
```

`build.sh` 先跑 iOS 的生成步（`ios/run.sh build` 的生成部分），再用 `build_shaders.py` 把 Mac 着色器缓存里的 MSL 编成 tvOS 的 metallib 随包带上；图标由 iOS 图标生成（`gen_icon.py`）。
`build.sh` runs the iOS generation step, compiles the translated MSL from the Mac shader cache into tvOS metallibs with `build_shaders.py` and bundles them; the icon is generated from the iOS icon by `gen_icon.py`.

**只能在实机上跑**：tvOS 模拟器的显卡没有 Metal 4、不是统一内存、参数缓冲只有一档，这个图形层起不来。
**Real hardware only**: the tvOS Simulator GPU has no Metal 4, no unified memory and only tier-1 argument buffers, so this renderer cannot start there.

## 资源放在哪 / Where the data goes

原版资源 9 GB，放不进安装包，要拷进 App 的 Caches 目录（相对 App 数据容器）：
The 9 GB of game data does not fit in the app, so it goes into the app's Caches directory (relative to the app data container):

| 路径 / Path | 内容 / Contents |
|---|---|
| `Library/Caches/D2R/RomFS/data` | 原版档案文件 / original archive files |
| `Library/Caches/D2R/astc/data` | 老机型补充包（A15 不支持 BC 贴图，实机必需）/ Legacy Device Pack (required on A15, which has no BC textures) |
| `Library/Caches/D2R/romfs_overlay_astc` | 补充包配套的覆盖层与 `pack.json` / the pack's overlay and `pack.json` |
| `Library/Caches/D2R/save` | 存档 / saves |

**存档在 Caches 里**：Caches 是系统空间紧张时可以清掉的目录，存档有可能丢。tvOS 没有给 App 的持久大目录，正式的存档做法（例如 iCloud）还没做。
**Saves live in Caches**, which tvOS may purge when storage runs low, so saves can be lost. tvOS gives apps no large persistent directory; a durable save solution (for example iCloud) is not implemented yet.

启动时加环境变量 `DM_TV_SHOT_SECONDS=<秒>` 会把到点后的下一帧存成 `Library/Caches/D2R/frame.png`（诊断用）。
Launching with `DM_TV_SHOT_SECONDS=<seconds>` saves the next presented frame after that time to `Library/Caches/D2R/frame.png` (diagnostics).

## 内存优化 / Memory optimization

两轮实测、回城/重开失败与脱敏日志见[观察记录](../../docs/qa-tvos-memory.zh-CN.md)。仍有场景切换退出，尚未完成稳定性验收。

See the [observation report](../../docs/qa-tvos-memory.md) for both rounds, transition failures and sanitized log evidence. Scene-transition exits remain unresolved; stability acceptance is incomplete.

游戏销毁纹理时同步撤下宿主描述符缓存中的所有别名，避免旧场景资源一直活到描述符槽再次注册。纹理与视图仍保留到已编码 GPU 工作的 fence 完成；一键重开与洞穴回城需要作为独立的场景切换用例验证。GPU 生命周期回归：`make -C dm/gfx/tests t_tex_finalize && MTL_DEBUG_LAYER=1 dm/gfx/tests/t_tex_finalize`。

When the game finalizes a texture, retire all host descriptor-cache aliases instead of retaining old scene resources until those descriptor slots are registered again. Textures/views remain alive until the encoded GPU work's fence completes. Quick restart and cave-to-town transitions require separate device regression checks. GPU lifetime regression: `make -C dm/gfx/tests t_tex_finalize && MTL_DEBUG_LAYER=1 dm/gfx/tests/t_tex_finalize`.

tvOS 默认保留从补充包拷入 BC3 纹理数组的 ASTC 压缩块，避免先展开为 RGBA8。只对首次写入、格式和块边界兼容的 2D/数组启用；后续若需要普通 BC/缓冲写入，会先逐级逐层保留已有像素，再退回 RGBA 路径。原始资源和补充包无需重新生成。`DM_TV_ASTC_ARRAYS=0` 可关闭此项作对照。

tvOS preserves ASTC blocks copied from the Legacy Device Pack into BC3 texture arrays instead of expanding them to RGBA8. This applies only to the first write into a compatible 2D/array texture with valid block boundaries. A later incompatible/ordinary BC or buffer write preserves existing mip/slice contents before falling back to RGBA. Game archives and the pack need no regeneration. Use `DM_TV_ASTC_ARRAYS=0` for the baseline path.

只读上传缓存采用均衡回收：正常最多每 3 秒扫描一次，至少 600 帧未使用才回收；进程余量低于 160 MiB 进入压力模式，恢复到 224 MiB 才退出。每批通常目标 16 MiB、最多 32 MiB，冷数据不足 4 MiB 时跳过，减少视图缓存失效和重新上传。余量低于 96 MiB 时保留紧急路径：最多每秒扫描、120 帧冷却、每批目标最多 64 MiB。软预算仍为 384 MiB；GPU 写过的贴图不回收，GPU fence 完成后才释放，单张贴图不可分割。活跃资源可能超过预算，因此这不是内存上限保证。`DM_TV_TEXTURE_CACHE_MB=0` 关闭回收，正整数调整软预算。

The read-only upload cache uses balanced eviction: normal scans are at least 3 seconds apart and protect textures used within 600 frames. Pressure mode starts below 160 MiB of process headroom and ends at 224 MiB. Normal batches target 16 MiB, capped at 32 MiB; less than 4 MiB of cold data is left alone to avoid cache invalidation and re-upload churn. Below 96 MiB, the emergency path scans at most once per second, protects the last 120 frames and targets at most 64 MiB per batch. The soft cache budget remains 384 MiB. GPU-written textures are protected; releases wait for the GPU fence, and a texture is indivisible. Active resources may exceed the budget, so this is not a guaranteed cap. Set `DM_TV_TEXTURE_CACHE_MB=0` to disable eviction or a positive integer to adjust the budget.

回归时启用 `DM_MEM_STAT_ALL=1 DM_RES_CENSUS=1 DM_RES_CHECK=1`，检查日志中的内存、数组分配/回退、缓存回收及驻留错误；至少覆盖选角、进局、切图和普通 BC 资源加载。CPU 边界测试：`make -C dm/host/tests build/t_pkg_cache && dm/host/tests/build/t_pkg_cache`。CPU 测试不能代替真机长期游玩与画面核对。

For regression runs, enable `DM_MEM_STAT_ALL=1 DM_RES_CENSUS=1 DM_RES_CHECK=1` and inspect memory, array allocation/fallback, cache eviction and residency errors. Cover character selection, entering a game, area transitions and ordinary BC loads. CPU boundary tests: `make -C dm/host/tests build/t_pkg_cache && dm/host/tests/build/t_pkg_cache`. CPU tests do not replace device soak tests and visual comparison.

性能测试与日常游玩请取消 `DM_RES_CHECK` 和 `DM_RES_CENSUS`：前者在纹理绑定热路径逐次加锁校验，不能把此时的帧率当成正常性能。需要详细内存记录时可单独用 `DM_MEM_STAT_ALL=1`，仍为每 300 帧一条。

Unset `DM_RES_CHECK` and `DM_RES_CENSUS` for performance tests and normal play: residency checks lock on the texture-binding hot path, so those runs are not representative of normal frame rates. `DM_MEM_STAT_ALL=1` can be used alone for detailed memory logging, still once every 300 frames.

排查瞬时峰值可临时设置 `DM_TV_MEM_TRACE=1`：每 100 ms 将进程占用、内核记录的峰值、进程余量和 GPU 分配写入 `Caches/D2R/memory-trace.tsv`，约记录 30 分钟（最多 18000 次采样），下次启用时覆盖。退出后应先取回此文件；100 ms 采样仍可能漏掉更短的峰值。与 `DM_RES_CENSUS=1` 一起用时，还会打印大纹理格式分组。本开关默认关闭，正常性能测试不要开启。

For transient peaks, temporarily set `DM_TV_MEM_TRACE=1`: every 100 ms it writes footprint, the kernel-recorded peak, process headroom and GPU allocation to `Caches/D2R/memory-trace.tsv` for about 30 minutes (at most 18000 samples), overwriting on the next enabled launch. Retrieve it after an exit before relaunching; sampling can still miss shorter peaks. With `DM_RES_CENSUS=1`, large texture format groups are also logged. This diagnostic is off by default; leave it unset for normal performance tests.

## 还没做 / Not done yet

- 已进入游戏并操作设置，但长时间稳定性、手柄全键位、切后台存盘、音频打断、手柄断线重连仍未完成验证。
- Gameplay and settings have been reached, but long-session stability, full controller mapping, saving when going to the background, audio interruptions and controller reconnection are not fully verified.

## 设置分割线修复 / Settings divider fix

原生手柄设置行高为 100，却共用了触屏配置中 150 的横线位置，横线会穿过下一行文字。电视专用入口 `gen_romfs_overlay.py` 将主机和掌机档的横线恢复为 90；自定义页自身行高为 180，因此在该页显式保留横线位置 150。iOS 生成器、目标与默认布局不变。

Native controller settings rows are 100 units high, but the shared touch profile places dividers at 150, crossing the next row. The TV-specific `gen_romfs_overlay.py` entry point restores offset 90 in both console and handheld profiles; the custom page uses 180-unit rows and explicitly retains offset 150. The iOS generator, target and default layout are unchanged.

对上表中要复制到 Caches 的外部覆盖层，使用自己的已有资源和补充包重新生成到独立目录，再部署生成的 `_astc` 目录并重启游戏。`build.sh` 的共享 iOS 生成步不代替这一步。无需重新转换或上传整套纹理。

For the external overlay copied to Caches in the table above, regenerate into a separate directory using your existing resources and Legacy Device Pack, deploy the resulting `_astc` directory and restart. The shared iOS generation step in `build.sh` does not replace this step. No full texture conversion or upload is required.

```bash
DM_ASTC_PACK="<existing-pack-directory>" python3 dm/tvos/gen_romfs_overlay.py "<RomFS-directory>" "<separate-output-directory>/romfs_overlay"
```

## 实测补充（2026-10-15）/ Device-test update (2026-10-15)

我在 Apple TV 4K 第三代（A15）上复测确认，部署修复后分割线显示正常；此前按 A 返回系统桌面的问题也已确认修复。两套配置、自定义页几何与 iOS 原行为检查通过，普通/ASTC 覆盖层及注释变体生成成功。尚未逐页、逐语言和逐字体档做完整视觉回归，暂无修复后截图。真机记录基于此前的本地移植版本（ea86e79），并不表示已对当前上游整合版完成构建和真机回归。

I confirmed on Apple TV 4K (3rd generation, A15) that the dividers display correctly after deploying the fix; the earlier issue where A returned to the system home screen was also confirmed fixed. Both profile configurations, custom-page geometry and preservation of original iOS behavior passed checks; standard/ASTC overlays and annotation variants generated successfully. Full visual regression across pages, languages and font profiles is pending, and no post-fix screenshot is available. Device results are from the earlier local port based on ea86e79, not a full build/device regression of the current upstream integration.

我还测试了固定 1600×900 的 3D 渲染（初始缩放 1.0，关闭动态缩放）；能启动，但随后退出，日志记录 signal 9。未取得能确认原因的系统报告，不能认定是内存或温度问题。现已恢复初始缩放 0.75 并开启动态缩放；不将固定 900p 作为稳定默认值。

I also tested fixed 1600×900 3D rendering (initial scale 1.0, dynamic scaling disabled). It launched but later exited with signal 9; no conclusive system report was obtained, so memory pressure or heat is not established as the cause. Initial scale 0.75 with dynamic scaling enabled was restored; fixed 900p is not promoted as a stable default.
