# 安卓分辨率与画质档：适配方案（2026-10-18，交给执行进程）

> **一句话**：**屏幕尺寸是唯一输入**，出图分辨率、3D 比例、UI 画布是**三个派生值**。目标是"任何比例的机器都 1:1 不拉伸、任何性能的机器都落在同一成本带、玩家有 3 档可选"。
> 配套的问题记录：[`docs/android-screen-adaptation.zh-CN.md`](android-screen-adaptation.zh-CN.md)；性能实测：[`docs/android-perf-notes.md`](android-perf-notes.md) §0.2 / §7.5 / §7.8。

---

## 0. 硬约束（不许破坏）

0. **不许写死分辨率（本方案的根）**：运行时决定分辨率的路径**只能**由"屏幕尺寸 × 预算"算出。
   - 三个档位只写**预算（MP）**，不写分辨率；
   - 老档名（`2624-075` 等）**只作别名**，不再参与计算；
   - 代码里除调试用途（`flag-gameres-*`、`flag-scale-*`）外，不得出现 `2624×1220` 这类字面量；
   - 屏幕尺寸在 `nativeStart` 之前就已经到位（`nativeConfigure` 先传显示尺寸、`nativeSurface` 再传真实 Surface 尺寸），所以正常路径永远拿真实屏幕。

1. **iOS 逐字不变**：改动只落在 `dm/android/**` 与 `dm/host/hle_oe.c` 的 Android 分支；`dm/android/diagnostics.c` 是安卓专属文件。
2. **已完成功能不变**：`quality.txt` < `flag-*` < `env.txt` 的优先级、`DM_GAME_RES` / `DM_3D_SCALE` 的单值覆盖语义**全部保留**；**老档名保留为别名**，别让现有脚本/文档失效。
3. **出图分辨率只能在启动时定**（窗口贴图启动时一次建好）；**3D 比例可以运行中改**。
4. 推覆盖层到设备后必须 `chmod -R 777`，否则 App 进不去目录、游戏 UI 初始化直接挂（见 [`docs/android-upstream-sync.zh-CN.md`](android-upstream-sync.zh-CN.md) §5-3）。

## 1. 现状（实测 + 代码位置）

| 项 | 现在 | 位置 |
|---|---|---|
| 测试设备 | 22081212C，**1220×2712 px @480dpi = 20:9**，Adreno 730 | `adb shell wm size` |
| 出图分辨率 | **写死 2624×1220（19.35:9）** → 被宿主横向拉 3.36% | `dm/android/diagnostics.c:250-261` |
| 呈现 | 出图 `vkCmdBlitImage` **拉满整屏**（不保比例） | `dm/gfx/nvn/nvn_vk.c` `mtl_present`（约 3443 行） |
| 3D 比例 | 安卓缺省 **0.75**（iOS 同款），动态降档 0.72 → 0.70 | `dm/host/hle_oe.c:379, 386-390` |
| 游戏 UI 画布 | 宽度硬上限 **4560 单位 = 19:9**，高度 2160 单位 = 屏高 | `docs/android-perf-notes.md:280` |
| 画质档 | 6 个"出图×比例"组合（2624-075 / 2168-075 / 1080-080 / 2624-085 / 2624-100 / 1600-100） | `dm/android/app_jni.c:410-432` |
| 窗口尺寸 | **已经拿得到**：`nativeSurface()` 存进 `window_state`，`dm_android_platform_get_window()` 可读 | `dm/android/app_jni.c:457-470`、`services_platform.h:38` |
| **写死清单**（本方案要清掉的） | `diagnostics.c:260` 默认 `2624×1220`；`app_jni.c:418-422` 档位表里的 6 个分辨率字面量 | 见 §3 ①② |
| 可保留的显式覆盖 | `flag-gameres-*` / `flag-scale-*`（调试开关，不是默认） | `app_jni.c:379-380, 396-400` |
| GPU 实测（2624-075） | 世界主通道 11.4 ms（1968×915）、纯深度 2.7、阴影 1.95、全屏效果链 2.7、UI 0.78、合成 0.5；总 ~20–23 ms；最重营地 25–27 帧 | `docs/android-perf-notes.md` §7.8 / §0.2 |

## 2. 设计

### 2.1 三个派生值（唯一输入 = 屏幕）

```
S      = 屏幕像素（SurfaceView 尺寸）
canvas = { 宽: min(S.w, S.h × 19/9), 高: S.h }        // 游戏自己的 UI 画布上限
out    = S 按"出图预算"等比缩（取偶、保比例）
scale  = min(1.0, sqrt(世界预算 / (out.w × out.h)))
world  = out × scale
```

### 2.2 三档（玩家可见）+ 两档（调试，不进 UI）

| 档名（写进 `quality.txt`，**ASCII**） | 界面上叫 | 出图预算 | 世界预算 | 你机器（2712×1220）上实际是 | UI 放大 |
|---|---|---|---|---|---|
| **`low`** | 省电 | 2.0 MP | 1.05 MP | 出图 2106×946，世界 1529×686（scale 0.73） | 1.29× |
| **`standard`（默认）** | 标准 | 3.0 MP | 1.60 MP | 出图 2582×1160，世界 1885×847（scale 0.73） | 1.05× |
| **`high`** | 高清 | 4.2 MP（=屏幕原生封顶） | 2.24 MP | 出图 2712×1220，世界 2034×914（scale 0.75） | 1.00× |
| `switch`（调试，不进 UI） | — | — | — | 1600×900 / 1.0（Switch 原档） | — |
| `native`（调试，不进 UI） | — | — | — | 屏幕原生 / 1.0 | — |

> 档名用 ASCII 是为了 `quality.txt` 好写、日志好 grep；界面上的"省电/标准/高清"走中英表（`dm/i18n/ui_text.tsv`），不要写死中文。

> 现在跑的 2624-075 ≈ **`high`**；用户反馈"吃不住"，所以**默认档放 `standard`**。

### 2.3 跨屏幕的落点（预算制的意义：成本带收窄）

| 屏幕 | 出图 | 放大 | 世界 |
|---|---|---|---|
| 2712×1220（20:9） | 2582×1160 | 1.05× | 1885×847 |
| 2400×1080（20:9） | 2400×1080（原生） | 1.00× | 1884×848 |
| 3200×1440（20:9） | 2580×1160 | 1.24× | 1886×848 |
| 2340×1080（19.5:9） | 原生 | 1.00× | 1860×858 |
| 1920×1080（16:9） | 原生 | 1.00× | 1686×948 |
| 2560×1600（平板） | 2190×1368 | 1.17× | 1600×998 |
| 2208×1840（折叠） | 1896×1580 | 1.16× | 1384×1154 |

## 3. 改动清单（按顺序，每步可独立验收）

### ① `dm/android/diagnostics.c` —— 出图跟屏幕（收益最大）

`front_game_res()` 现在直接 `return 2624×1220`。改成：

```c
/* 默认出图 = 屏幕尺寸按预算缩；DM_GAME_RES 显式值仍然优先（块不动） */
unsigned out_budget_px = dm_android_quality_out_budget();   /* 新接口，见 ②；缺省 3.0e6 */
dm_android_window_state ws; dm_android_platform_get_window(&ws);
unsigned W = ws.width, H = ws.height;
double k = 1.0;
if (W > 2880) k = 2880.0 / W;                                  /* UI 原生程度上限 */
if ((double)W * H * k * k > out_budget_px) k = sqrt(out_budget_px / ((double)W * H));
unsigned w = (unsigned)(W * k) & ~1u, h = (unsigned)(H * k) & ~1u;
LOG("出图：屏幕 %ux%u → %ux%u（预算 %.1f MP，比例不变）", W, H, w, h, out_budget_px / 1e6);
return ((uint64_t)h << 32) | w;
```

**前提：屏幕尺寸必须在游戏问分辨率之前就到位**（当前 Java 的调用顺序已经保证：

- `GameActivity` → `nativeConfigure(files, locale, touchRoute)`：**新增两个参数**（或新接口）把
  `getWindowManager().getCurrentWindowMetrics().bounds` 的宽高传进来，存成 `panel_w/h`；
- `nativeSurface(surface, w, h, …)`：到达后**用真实 Surface 尺寸覆盖** `panel_w/h`（权威值）；
- `nativeStart()` → 游戏主线程 → `guest_thread_init` → `front_game_res()`。

两条都拿不到（理论上不可达）才兜底：`1280×720`（游戏自己的面板缺省）+ `【红】`日志。
**不要**再兜底成 2624×1220——那正好是"换机就歪"的来源。

### ② `dm/android/app_jni.c` —— 档位表换成"预算"，老档名保留别名

- `tiers[]` 结构由 `{name, gamres, scale}` 改成 `{name, out_budget_px, world_budget_px}`；
- 新增 3 档：`low`(2.0e6 / 1.05e6)、`standard`(3.0e6 / 1.60e6)、`high`(4.2e6 / 2.24e6)；
- 老档名做成别名映射（`2624-075`/`2624-085`/`2624-100` → `high`，`1080-080`/`2168-075` → `low`，`1600-100` → `switch`）；
- 把选中的预算导出成两个小接口给 ①③ 读（例如 `dm_android_quality_out_budget()` / `..._world_budget()`，实现放 `diagnostics.c` 或 `app_jni.c` 均可，别引入新文件）；
- 日志保留：`ANDROID_QUALITY tier=...`。

### ③ `dm/host/hle_oe.c` —— 3D 比例按世界预算反算

把安卓缺省 `float base = 0.75f;`（约 390 行）改成：

```c
float base = 0.75f;
#if defined(__ANDROID__)
{ unsigned out = dm_android_quality_out_px();          /* 当前出图像素（见 ①） */
  unsigned budget = dm_android_quality_world_budget(); /* 见 ② */
  if (out && budget) base = fminf(1.0f, sqrtf((float)budget / (float)out)); }
#endif
const char *e = getenv("DM_3D_SCALE"); if (e) base = atof(e);   /* 显式覆盖最优先（现有逻辑，别动） */
```

动态降档（0.72/0.70）与下限逻辑保持不变，它作用在 `base` 之上。

### ④ 画布矩形（贴边元素）—— `dm_touch_controls.c` + `touch_controls_android.c`

- 新增 `dm_tc_set_canvas_width(float w)`（0 = 全屏，缺省行为不变）；
- 布局里所有"贴右/贴左"的元素（主技能圆弧锚点、地图键、摇杆静止位）按 `canvas` 计算，而不是 `l->width`；
- `touch_controls_android.c` 的 `nativeResize` 里：`w` 是屏宽，按 `min(w, h*19/9)` 调一次；
- **iOS 侧不调这个接口 = 行为逐字不变**（共享文件改动必须两端编译，Android 由 CMake、iOS 由 Makefile/工程，见隔离文档 §3.2）。

### ⑤ `dm/gfx/nvn/nvn_vk.c` —— blit 保比例（安全网，可后做）

`mtl_present` 里 `dstOffsets` 用整个 `swap_extent`。改成按 `out` 比例算居中的 dst 矩形（letterbox）。正常情况 `out` 已等于屏幕比例，不触发；只是防止 `DM_GAME_RES` 手设比例时把画面拉歪。

### ⑥ 日志一行

启动时打：`S=2712x1220 canvas=2712x1220 out=2582x1160 world=1885x847 scale=0.73 tier=standard`。

## 4. 验收

**离线（每步都跑）**

```bash
cd dm/android/build && ninja          # 原生库
cd - && dm/android/tools/build_apk.sh # 打包
bash dm/android/tools/sync_surface_audit.sh   # 冲突面只许降/持平（当前 43）
python3 dm/i18n/scan_literals.py
python3 dm/ios/tests/test_touch_layout_options.py
```

**真机**（手机当前走无线 adb：`192.168.31.46:5555`，也可插 USB）

```bash
dm/android/tools/deploy_apk.sh
# 覆盖层没有改动就不用重推；若重推，记得 chmod（见 §0-4）
```

1. **三档各跑一次**：`diagnostics/quality.txt` 写 `low` / `standard` / `high`，重启，核对日志那行尺寸与截图；
2. **比例扫描**（同一台机，`wm size`）：

```bash
adb shell wm size 1220x2169   # 16:9
adb shell wm size 1220x2440   # 18:9
adb shell wm size 1220x2643   # 19.5:9
adb shell wm size 1220x2712   # 20:9（原生）
adb shell wm size 1220x2847   # 21:9
adb shell wm size 1220x1464   # ~1.2:1（折叠）
adb shell wm size 1220x1952   # 16:10（平板）
adb shell wm size reset
```

每档看三件事：①底栏格子与游戏药水格是否对齐；②技能弧/地图键是否在画布内、离边缘多远；③有没有黑边或拉伸。
3. **帧率**：最重场景（营地，每帧 1100+ 绘制）跑 30 秒记录；对比"标准/高清"的差；
4. **回归**：语言、建角、背包、选项、自定义页、暂停菜单各点一遍（清单见 `docs/android-ui-parity-lessons.zh-CN.md`）。

## 5. 风险与取舍

| 风险 | 说明 | 处理 |
|---|---|---|
| UI 变软 | 出图低于屏幕原生时，游戏自己的文字被放大 1.05–1.29× | 预算别低于 3.0 MP（`standard`）；只有 `low` 档才更低 |
| 手感变化 | 贴边元素改贴画布后，21:9/折叠上按钮离边缘更远 | 真机确认；必要时给"贴屏边"开关（默认贴画布） |
| 世界预算不分场景 | 按像素算，不区分复杂度 | 靠现有动态降档兜底 |
| 老档名失效 | 玩家/脚本里写的是 `2624-075` 之类 | 全部保留为别名，行为映射到新档 |
| 平板/折叠没真机 | 只能靠 `wm size` 模拟 | SDK 设备档：Medium Tablet 2560×1600、Fold-out 2208×2480、Fold-in 1768×2208，至少按这些比例过一遍 |

## 6. 明确不做

- 不做时域上采样（FSR / GSR / ASR）——下一个台阶，工程量另算；
- 不改 iOS 的 0.75 缺省与动态降档逻辑；
- 不动 `quality.txt` < `flag-*` < `env.txt` 的优先级；
- 不在运行中改出图分辨率（做不到，窗口贴图一次性）。
