# Android 复刻 iOS UI 体验：实施方案（评审稿 v1）

> **迁移注记（整合树）**：本文写作基线为合并前的 `android-ours@0149751` 快照（自研 Vulkan 层位于 `dm/gfx/vk/`）。
> 现仓库为 `android-integration`（dodola 的 Android/Vulkan 层已合并，后端在 `dm/gfx/nvn/nvn_vk.c`），
> 文中 `dm/gfx/vk/...` 路径与部分行号需按新树重新对照；交互模型、隔离协议与阶段划分的结论不受影响。
> Migration note (integration tree): this document was written against the pre-merge snapshot
> `android-ours@0149751` (self-developed Vulkan layer under `dm/gfx/vk/`). This tree is
> `android-integration` with dodola's Android/Vulkan layer merged (`dm/gfx/nvn/nvn_vk.c`); re-check
> the `dm/gfx/vk/...` paths and line numbers, the design conclusions are unaffected.

> **状态：方案评审稿 —— 未动任何代码。** 评审定稿后才进入实施；实现开始前把本文转成英文版 `ANDROID_UI_PARITY.md`（仓库惯例：英文主文档 + 简体中文对照，见 ANDROID.md / ANDROID.zh-CN.md）。
>
> 实现级设计（接口快照 / JNI 面 / 分阶段伪码 / 验收钩子 / 陷阱清单）：[ANDROID_UI_PARITY_IMPL.zh-CN.md](ANDROID_UI_PARITY_IMPL.zh-CN.md)。
>
> 相关文档：[ANDROID.zh-CN.md](ANDROID.zh-CN.md)（总体移植计划：图形层、平台探针、阶段划分）、[VK_RENDER_PROGRESS.zh-CN.md](VK_RENDER_PROGRESS.zh-CN.md)（渲染进度实测）、[VK_CORRECTNESS_PLAN.zh-CN.md](VK_CORRECTNESS_PLAN.zh-CN.md)（画面正确性）。
>
> 基线：分支 `android-ours`（单提交 `0149751`），merge-base `9f0c257`，`origin/main` 领先 **41** 个提交（2026-10-16 实测；上游持续提交，rebase 时以当日值为准）。本文所有结论带 `file:line` 证据，调研由 6 个并行子线程完成（触摸链路 / overlay / Android 现状 / iOS 外壳 / 提交审计 / 下游消费）。

---

## 0. 结论速览

### 0.1 一句话

**"一模一样" = ①同一套触摸/自动化算法 + ②同一份 UI 数据（overlay RomFS）+ ③同一批时序参数（都活在共享 C 里）+ ④一个把它们接起来的 Android 外壳。①②③已经在仓库里，全部工作量在 ④：接线 + 写壳，不需要"重新发明手感"。**

### 0.2 复用与隔离的三个数字

- UI 范围内，**算法/数据/参数三块 100% 直接复用**（共享 C，零修改，多数已编进 Android 库）。
- Android 需要**新写**的只有平台外壳：约 1,500–2,000 行（Java 采集/绘制/IME/生命周期 + C 接线），对比 iOS 前端 4,664 行（front_ios.m 1,593 + main.m 1,050 + DmTouchOverlay.m 315 + …）——因为 72% 是共享代码（ANDROID.zh-CN.md §0.1）。
- **对 iOS 的影响面：设计目标为零。** 机制上由三层隔离保证（§3），全套方案预期不修改任何 iOS 运行时行为；个别共享文件如确需触碰，走"加法协议 + 双端验证"（§3.2）。

### 0.3 对既有文档的两处修正

1. `ANDROID.md:24` 认为 overlay 覆盖层的艺术资源"需要重画"——**与代码事实不符**：overlay 机制（`hle_fs.c` 的 `DM_ROMFS_OVERLAY` 分层查找）是平台中立的，"视觉差异"由 Python 构建脚本从玩家自己的 RomFS 生成，Android 同一条生成链即可复用（§4.2）。本文后文以 `hle_fs.c` 事实为准。
2. `ANDROID.md:48` 把"Rift HUD、原生 HUD 页"等列入非目标（feature parity 后置）——本文的 UI 对齐**恰好以这些可见功能为主体**（触摸层 + overlay + 外壳），属于提前拉通的范围；图形/云存档等其它非目标不变。

---

## 1. "一模一样"的定义与边界

### 1.1 三个一致层次

| 层次 | 内容 | 可达到的程度 | 依据 |
|---|---|---|---|
| **交互行为层** | 每个手势/点击做什么：摇杆、技能点拖、腰带滑、世界点击、面板双击/长按/拖放…… | **逐比特一致**（同一份算法 + 同一批常数） | 手感算法全在共享 C：`dm_touch_controls.c`（698 行）、`dm_cursor.c`；参数是共享常数（0.08s/10pt/0.17s/0.35s/死区 6/满偏 60…） |
| **视觉呈现层** | 游戏界面里看到什么：提示措辞、底栏、关闭钮、手柄提示有无、覆盖控件位置 | **数据级/几何级一致**（同一渲染器 + 同一 overlay 数据 + 同几何重绘） | 游戏画面同一套宿主渲染；UI 表象 = overlay RomFS（平台中立机制）+ DmTouchOverlay 几何（UIView 纯绘制 → Android View 重画） |
| **时序手感层** | 事件节奏：触摸即时推、60Hz 每帧泵、120Hz 脉冲、按下沿锁存 | **参数级一致**（照抄速率结构） | front_ios.m:216-227（每事件）、:858-877（CADisplayLink 60Hz）、:668（120Hz 脉冲） |
| **平台外壳层** | 系统键盘外观、系统手势、触觉质感、刘海/挖孔、通知样式 | **只能行为等效** | 见 §1.2 豁免清单 |

### 1.2 平台豁免项（允许不同，不列入"一样"的验收）

| 项 | iOS | Android 等效物 | 说明 |
|---|---|---|---|
| 文本输入外观 | UIAlertController + UITextField（深色） | InputMethodManager + 系统输入法 | 规则（字数/字符集/随机名）必须一致，皮肤不要求 |
| 系统手势 | Home indicator 自动隐藏 + 延迟系统手势 | 沉浸式 + 手势导航条 | 等效沉浸，不模拟 iOS 的 indicator 行为 |
| 触觉质感 | UIImpactFeedbackGenerator 三档 | Vibrator/HapticFeedback 三档 | 强度手感由硬件定 |
| 刘海/安全区形状 | safeAreaInsets | WindowInsets/displayCutout | 数值映射算法一致即可 |
| 刷新率 | ProMotion 60/120 | 设备面板刷新率 | 目标帧率策略一致（默认 60 档） |

---

## 2. 复用设计：iOS 代码怎么用

### 2.1 复用分级表（UI 范围）

| # | iOS 组件 | 文件 | 分级 | Android 处理 |
|---|---|---|---|---|
| 1 | 触摸模型（布局/摇杆/手势/腰带/技能组） | `dm/ios/Sources/dm_touch_controls.c/.h` | **A 直接复用** | 已编入 Android（CMakeLists.txt:150）；零改动；rebase 后自动获得浮动摇杆等新版 |
| 2 | 宿主 UI 自动化（光标/点击/面板泵） | `dm/host/dm_cursor.c`、`dm_cursor_item.c` | **A** | 已编入（CMake GLOB）；零改动 |
| 3 | 输入到达（触摸槽→HID、按键→Npad） | `dm/host/hle_touch.c`、`hle_hid.c`、`dm_input.h` | **A** | 已编入；零改动 |
| 4 | overlay 挂载机制 | `dm/host/hle_fs.c`（DM_ROMFS_OVERLAY 分层查找） | **A** | 已编入；Android 只需设环境变量 |
| 5 | 模式串表（手柄↔触摸说法运行时切换） | `dm/host/dm_padmode.c` + touch_strings 旁路表 | **A** | 已编入；零改动 |
| 6 | 掉落震动判定 | `dm/host/dm_drophaptic.c` | **A** | 判定共享；Android 输出侧接 Vibrator |
| 7 | 前端契约 | `dm/host/front.h` | **A（契约）** | 41 提交内零变化（2026-10-16 复核）；Android 实现同一契约 |
| 8 | overlay/UI 数据生成器 | `dm/ios/tools/{touch_layout,touch_strings,gen_romfs_overlay}.py` | **B 构建期复用** | Android 打包脚本调用；不改或纯加法改 |
| 9 | UI 素材生成器（技能图集、底栏 v4） | `dm/ios/tools/{gen_assets,gen_hud_v4,gen_map_goal_art}.py` | **B 构建期复用** | 同批 PNG 供 Android 绘制层与 overlay；产物落在 `dm/ios/Sources`（已被 `dm/ios/.gitignore` 忽略，见 §3.2 注意项） |
| 10 | 触摸采集/坐标映射/发布三件套 | `front_ios.m:216-227`、`:481-670` | **C 逻辑参照移植** | 移植进 `front_android.c` + Java 采集层 |
| 11 | 覆盖绘制层 | `DmTouchOverlay.m`（315 行，纯绘制，`userInteractionEnabled=NO`） | **C 逻辑参照** | Android View 按同几何重画 |
| 12 | 场景/生命周期 | `main.m` SceneDelegate、`front_ios.m:1207-1214` | **C 逻辑参照** | Activity 回调映射同一调用序列 |
| 13 | 文本输入 | `front_ios.m:991-1106`（规则 2-15 字节、随机名、金币数字） | **C 逻辑参照** | IME 对话框；规则建议抽共享函数 |
| 14 | 手柄 GameController 支持 | `dm_gamepad.m`、`dm_pad_set` | **D 本期外（可选）** | 触摸↔手柄切换体验后置 |
| 15 | 图形层（Metal） | `nvn_mtl.m` | **超范围** | 另线：nvn_vk.c（ANDROID.md C 类） |

### 2.2 "直接复用"为什么成立

- 触摸模型、dm_cursor、hle_touch/hle_hid、hle_fs 都是**纯 C + 平台无关**；Android 的 CMake 已经把它们（或它们的同族）编进 `libd2r.so`（`dm/android/jni/CMakeLists.txt:71` GLOB `dm/host/*.c`；:150 显式加入 `dm_touch_controls.c`）。
- 唯一需要的接线：让 Android 前端把事件喂进这些现成函数（下文 P1–P3），没有任何一处需要为 Android 改算法。

---

## 3. 隔离设计：保证不碰 iOS

### 3.1 现有隔离机制（事实核对过的三层）

**① 分支隔离**
- Android 全部工作提交留在 `android-ours`；`origin/main`（iOS/Mac 主线）只读。rebase 只重写 `android-ours`。
- 本文不假设"何时合并回 main"——那是独立决策。

**② 文件隔离（谁在被谁编译）**

| 区域 | iOS/Mac 构建 | Android 构建 | 结论 |
|---|---|---|---|
| `dm/android/**` | **从不引用** | 全部（显式清单 + GLOB） | Android 专属区，**零风险** |
| `dm/host/*.c` | `dm/host/Makefile` 显式 `SRC=` 清单 + `hle_*.c` 通配（Makefile:5） | **GLOB 全收**（CMakeLists.txt:71），再减 `DM_EXCLUDE`（:129：`touch_mac.c`、`hle_lifecycle.c`、`nvn_fmt_mtl.c`） | 共享区。**注意不对称：往 `dm/host/` 放新 `.c` 会被 Android 自动收走、iOS 不收**——共享新文件必须同时进 Makefile；Android 专属代码绝不放这里 |
| `dm/gfx/**` | 通配（`../gfx/*.c` 等，Makefile:10） | GLOB 三个子目录 + 排除 | 共享区，同规则 |
| `dm/ios/Sources/**` | XcodeGen `sources: Sources` **目录 GLOB 全收**（project.yml:19） | 只收显式列出的 `dm_touch_controls.c`（CMakeLists.txt:150） | **危险区：往这里放 Android 专属文件会被 iOS 构建自动吸走**——禁止 |
| `dm/ios/tools/*.py` | `run.sh` / Xcode 预构建脚本调用 | Android 打包脚本将只读调用（新增） | 构建期共享：改它们等于改 iOS 出包行为，按 §3.2 协议 |
| `dm/host/front.h` | 契约 | 契约 | 前端边界的唯一缝；现稳定 |

**③ 构建期隔离**
- Android 打包不动 iOS 产物：`build_apk.sh` 输出到 `dm/android/build/`（已 untracked）；CMake 构建目录独立（build_apk.sh:74-77 特意保留增量）。
- iOS 出包（`run.sh`）完全不读 `dm/android/`。

### 3.2 共享文件修改协议（加法原则）

修改"共享区"文件（`dm/host/**`、`dm/gfx/**`、`dm/ios/Sources/dm_touch_controls.c`、`dm/ios/tools/*.py`、`front.h`）前，必须满足全部五条：

1. **优先零修改**：先问"Android 端能不能不改共享文件完成"。本方案 P1–P5 预期零修改。
2. **加法式**：必须改时只允许新增函数/字段/可选参数，既有行为、既有签名、默认值保持不变（如给生成器加 `--out`，缺省输出路径不变）。
3. **双端可编译**：共享 `.c` 改动必须同时通过 Android CMake 与 `dm/host/Makefile` 编译。
4. **双端验证**：改动后跑 iOS/Mac 回归（Mac 冒烟 + `TouchSmokeTests` 目标，project.yml:142-162），并在文档 §附录 D"共享修改登记表"记一行（文件/原因/对 iOS 影响评估/验证结果）。
5. **禁 fork**：绝不把共享文件复制进 `dm/android/` 改成二份（防漂移；ANDROID.md:44 同一原则）。

**注意项（已知的跨树写入）**：`gen_assets.py` 会把 `Sources/TouchArt/dm_native_*.png` 与 `Sources/dm_touch_native_art.c` **生成在 `dm/ios/Sources` 原位**（gen_assets.py:42，`dm/ios/.gitignore` 忽略）。Android 构建期调用同一生成器时会出现"Android 构建写 iOS 源码树"——两个可选处理，实施 P4 时二选一：
- (a) 接受（产物与 iOS 完全一致、gitignored、不影响 iOS 构建结果）；或
- (b) 给生成器加 `--out`（协议第 2 条），Android 输出到 `dm/android/build/`，缺省行为不变。

### 3.3 本方案对 iOS 的影响面清单（文件级）

| 类别 | 文件 | iOS 影响 |
|---|---|---|
| 新增 Android 专属 | `DmHostActivity.java`、触摸/绘制 View、`front_android.c`、`dm_env.c`、`build_apk.sh`、`CMakeLists.txt` | **零**（iOS 不引用） |
| 构建期只读调用 | `dm/ios/tools/*.py`（overlay 生成链） | **零**（不改文件；注意 §3.2 产出目录问题） |
| 共享、计划内改动 | **当前预期：无。** 候选清单：① `dm_cursor.c` 面板树兜底块（rebase 时核实归属与 iOS 中立性）；② 生成器如需 `--out` 参数（纯加法） | 极小；全部走 §3.2 协议 |
| 明确不碰 | `front_ios.m`、`main.m`、`DmTouchOverlay.m`、`project.yml`、`Makefile` 的 iOS 语义 | **零**（作为参照读取，不改） |

### 3.4 违规检查与护栏

- `build_apk.sh:67` 已在打包前跑 `dead_check.py`（死接线检查）。**建议在 P1 之后扩展**：加一条"Android 新增/修改文件中出现 `dm/ios/Sources/**` 除 `dm_touch_controls.c` 之外的写入/拷贝"即报错；以及"`git diff --stat origin/main...HEAD -- dm/ios dm/host` 出现未登记（附录 D）文件即警告"。
- 评审 checklist：任何 Android PR 合入前人工过一遍 §3.2 五条。

---

## 4. 现状基线（证据）

### 4.1 iOS 触摸链路（完整事实）

| 环节 | 事实 | 证据 |
|---|---|---|
| 采集 | `DmMetalView` 原生 touches（无 gesture recognizer），16 指槽 `_slot[16]`，多点始终全量组装 | front_ios.m:117,132-136,481-558 |
| 手势 | 每个触点先经 `dm_tc_begin_at/move_at/end_at`（纯 C 模型） | dm_touch_controls.c:436/516/537 |
| 坐标 | UIKit 点 → 归一化 [0,1)（clamp 0.99999）→ 面板换算 1952×900/1568×720 → 游戏 1280×720 | front_ios.m:216-221；front.h:33-38 |
| 视口 | letterbox_rect 16:9 居中；iPad 可全屏（g_full）；世界点击同时给视图归一化 u 与出图归一化 w | front_ios.m:894-968,83-85 |
| 发布 | `dm_touch_set` 每次变化整组推；`dm_input_set`/`dm_cursor_set_screen` 每 tick；`dm_ui_world_tap`/`dm_ui_touch` 按事件；全部主线程 | front_ios.m:216-227,455-463,544-554,629 |
| 节奏 | CADisplayLink 主 RunLoop 60Hz；脉冲按键 120Hz（dispatch_after 自驱动）；ProMotion 先申请 80-120Hz | front_ios.m:858-877,668 |
| 触摸模型 | 12 键位映射 + 浮动摇杆（贴边夹回仅影响绘图、轴值按落点）+ 0.08s 点拖判定 + ZL 0.17s 超前 + 腰带 swipe(24pt) + 世界点击 0.35s/10pt + 导航双击/长按 | dm_touch_controls.c:39-82,134-226,497-638 |
| 绘制 | `DmTouchOverlay` 纯绘制（不拾触）：5 技能钮/回城/地图/菜单/换组/摇杆/底栏 v4/拖货影；状态来自 `dm_tg_snapshot` | DmTouchOverlay.m:39,48-61,156-272 |
| 触觉 | 仅高价值掉落：重/中/轻 | front_ios.m:408-411 |

### 4.2 iOS overlay（RomFS 覆盖层）

- **生成**：`gen_assets.py` → `gen_romfs_overlay.py`（run.sh:32,65；gen_romfs_overlay.py 调用 touch_layout/touch_strings）；每套 overlay 必须含 `data/.build.config`（run.sh:69-72 强校验）。
- **内容（16 项用户可见差异）**：22 张手柄键图标透明；3 张光标圆环透明；提示控件/提示条挪出屏；建角/起名/模式页按钮与勾选框；8 职业点击竖条（修点错职业）；选角页换键鼠版布局；面板金色 X 关闭钮；商人刷新货架/修理重排；背包符文之语清单；技能绑定页 F1-F4 键名与格位对调；暂停菜单行列重排；角色页删耐力行；提示文案"按 A 使用"→"双击使用"（zhCN+enUS，SHA-1 校验）；通知去操作词；模式串表运行时旁路（dm_padmode.c）。
- **加载**：`main.m:608-621` setenv `DM_ROMFS_OVERLAY`（含 variants 组合、ASTC 版）；`hle_fs.c:89-90,172-181,207,251-260` 按冒号分层先查 overlay 再查基底——**平台中立**。

### 4.3 iOS 外壳行为

- 方向锁横屏（Info.plist 双 landscape）；`UIRequiresFullScreen`。
- 安全区：`safeAreaInsetsDidChange` → `dm_tc_set_safe_insets(l,r)`；iPad 底边 `dm_tc_set_edge_bottom`；左右折算 2160 基准 → `dm_ui_safe_left/right`（front_ios.m:145-157）。
- 生命周期四事件（inactive/background/foreground/resume）：失活=取消触摸+手柄清零+30s 节流自动存盘；后台=静音+存档上传+最多 2.5s 存盘+客体时钟冻结+GPU 排空；前台反向恢复（front_ios.m:1195-1214；main.m:917-950）。
- 文本输入：主线程弹框、客体信号量阻塞；起名（2-15 字节规则/随机名）+ 金币数字；DM_TEST_KBD_TYPE 测试钩子（front_ios.m:991-1106）。
- 沉浸：状态栏隐藏、home indicator 自动隐藏、延迟系统手势、idleTimerDisabled（front_ios.m:1403）。
- 帧节奏：60Hz 固定 + 120Hz 脉冲；mtl_fps_target 30/60 切换。

### 4.4 Android 现状（P1 盘点）

- **输入**：全工程无 `onTouchEvent`/`MotionEvent`/`KeyEvent`/手柄；`dm_touch_controls.c` 已编入但零调用（死代码）。
- **发布链**：`dm_touch_set`/`dm_cursor_set_screen`/`dm_input_set`/`dm_ui_*` 均无调用者。
- **生命周期**：只实现 SurfaceHolder.Callback；无 `onPause/onResume`；`surfaceDestroyed` 后游戏照跑；`stopHost` JNI 未实现（dm_host_jni.c:112-116），`dm_host_shutdown` 可用（hle_lifecycle_android.c:49）。
- **文本输入**：`front_text_input` 桩返回 -1（front_android.c:226-231）。
- **沉浸**：Manifest 无任何全屏/沉浸标志（AndroidManifest.xml:26-31）。
- **帧驱动**：`front_run` 阻塞式（front_android.c:180-184）；无 Choreographer/vsync；刷新率仅进诊断（:128-135）。
- **已有挂点**：JNI `nativeSetWindow/ClearWindow/SetWindowGeometry`（front_android.c:263-281）；`front_metal_layer()` 可返回 `ANativeWindow*`；`dm_android_window()`；日志转发线程；`dm_env.c` 环境准备（DM_ROOT/ROMFS/SAVE/TEMP）。
- **已编入的共享资产**：dm_cursor、hle_touch/hle_hid、hle_fs、dm_padmode、dm_drophaptic（CMake GLOB）；触摸模型（:150）。
- **待钉死的矛盾（开放问题 #1）**：front_android.c:2 自述"P1 免窗口、无交换链"，但仓库有 274 行实测出图记录（VK_RENDER_PROGRESS.zh-CN.md）且窗口 JNI 交接代码已存在。实现 P8 前必须先确认"当前哪条出图路径是活的"。

### 4.5 共享契约现状

- `front.h`：41 提交零变化（2026-10-16 复核）→ **输入接线可立即开工，契约稳定**。
- `patch_nso.py`：**有变化（此前审计误判为「零变化」）**——r41 修正独占窗口出口桩次序（6 个窗口的「比较交换成功后落进外部出口、被当失败处理 → 一次操作做两遍」缺陷：main 0x1848eb8/0x1848f28/0x1848f84、sdk 0x2ea0fc/0x2ea4cc/0x2ea644），外加 4 个新观测点（0x463f40 转向回弹、0x9da5b0 模型转向、0xb0d560 提示框换位、0x7ce800 秘境预设怪抑制）。**Android 装载器消费其产物——rebase 时必须同步。**
- `dm_touch_controls.c/.h`：origin/main 已演进（浮动摇杆 `joy_zone_r/t/min_x/max_y`、`dm_tc_panel_mode(0/1/2/3)`）——**当前分支没有**（本机 grep 验证为空）。`dm_cursor.c`：新增面板开合栈 `dm_hud_stack_msg()`、通知双态停靠 `NOTE_PANEL_Y=-310`；`afad65c`（v1.0.16）再增**界面代号体系**（`dm_ui_gen/dm_ui_unstable/dm_wref`，修「腰带补满崩溃」等控件悬空指针问题）并把 `dm_hud_info.holding` 从布尔改为**物品 id**（`dm_hud_held/dm_hud_set_held`）——F5/F6 实现以新语义为准。
- 结论：**手感/UI 的"一模一样"必须先对齐 origin/main 的触摸契约**（P0 理由）。

---

## 5. 差距矩阵

### 5.1 交互主链（决定"手感像不像"）

| # | 组件 | iOS（证据） | Android 现状 | 要做到一样 |
|---|---|---|---|---|
| 1 | 触摸采集 | 16 槽 raw touches（front_ios.m:117,481-558） | 无 | Java MotionEvent + 槽位跟踪 → JNI → C |
| 2 | 坐标映射 | 归一化+letterbox+面板换算（front_ios.m:216-221,894-968） | 仅 front_game_res 雏形 | 照抄映射 + surface/insets 接入 |
| 3 | 手势模型 | dm_touch_controls.c 698 行 | 已编入、零调用、旧版 | rebase 后喂事件 |
| 4 | 发布路径 | 三 API 同采样 + 每帧泵（front_ios.m:216-227） | 无调用者 | 移植 pushFrom/tick/pulse |
| 5 | 下游到达 | hle_touch/hle_hid 锁存机制 | 共享代码已在 | 喂对即可 |
| 6 | 游戏内 UI 注入 | dm_ui_* → PanelManager vtable+0x28，每出图帧一条（dm_cursor.c:2465） | 无注入 | 全量接入 + 泵节奏 |
| 7 | OBS_UIMSG 门 | 正常（diag.c:347） | 丢事件（dm_cursor.c:2831-2852 注释实证）；10 帧兜底仅 8 个面板名 | 主链靠 #6 注入绕开；根因单列调查 |

### 5.2 呈现与外壳（决定"观感像不像"）

| # | 组件 | iOS（证据） | Android 现状 | 要做到一样 |
|---|---|---|---|---|
| 8 | overlay RomFS | 生成器 + 平台中立加载 | 未生成、未设 env | 构建期生成 + setenv |
| 9 | 覆盖绘制 | DmTouchOverlay 纯绘制 | 无 | Android View 同几何重画 |
| 10 | 文本输入 | UIAlertController（front_ios.m:991-1106） | 桩 -1 | IME 对话框 + 规则 |
| 11 | 生命周期 | 四事件完整序列（front_ios.m:1207-1214） | 无回调 | onPause/onResume 映射 |
| 12 | 沉浸/安全区 | 隐藏+insets 映射 | 无 | WindowInsets + 沉浸 + KEEP_SCREEN_ON |
| 13 | 帧节奏 | 60Hz+tick+120Hz 脉冲 | 无帧驱动 | Choreographer/Handler（依赖 #1 开放问题） |
| 14 | 触觉 | 掉落三档 | 无 | Vibrator 三档（边缘项） |

---

## 6. 硬判断与开放问题

**硬判断（本方案的设计前提）**

1. **rebase 提到最前**：触摸契约（浮动摇杆、panel_mode、hud 栈）在 origin/main；先接旧线=白做。`front.h` 零变化，但 `patch_nso.py` 带 r41 正确性修正（§4.5）——rebase 顺带修掉装载器链缺陷；风险面在 iOS/Mac/工具脚本，Android 目录零交集。
2. **overlay 不用重画美术**：机制平台中立（§4.2），复用生成链一次到位（16 项可见差异）。
3. **UI 主链不依赖修复 OBS_UIMSG 门**：dm_ui_* 注入 + 现有兜底可覆盖体验；根因调查单列、不阻塞。
4. **手感参数不用调**：全部共享常数；唯一变量是事件频率与帧节奏对齐。
5. **DmTouchOverlay 是纯绘制**（不拾触）→ Android 一个自绘 View 即可，不必模拟 UIKit 视图体系。
6. **隔离靠机制而非纪律**：三层隔离（§3.1）+ 协议（§3.2）已由仓库现有结构支撑；本方案不需要改 iOS 运行时行为。
7. **帧驱动是最大结构空白**，且先要钉死"哪条出图路径活着"（§4.4 矛盾）。

**开放问题**

| # | 问题 | 影响 | 处理 |
|---|---|---|---|
| 1 | 出图路径现状（P1 免窗口 vs 实测出图记录） | P8 帧节奏、P5 绘制叠层方式 | P0-P1 期间上机核实，写回本文 |
| 2 | OBS_UIMSG 门丢事件根因 | 自动化工具链全覆盖 | 单列调查任务（不阻塞主链） |
| 3 | rebase 冲突面（dm_cursor.c 兜底块归属、工具脚本） | P0 工期 | worktree 试 rebase 预演 |
| 4 | T8 前端停滞（MainMenuLoadingPanel 不关等） | 观感（加载态停留） | 大概率随 P0/P6 缓解；仍停则单独立项 |
| 5 | Android 音频缺失 | 非 UI，但"体验"相关 | 见 §10 决策点 2 |

---

## 7. 实施计划 P0–P8

> 每阶段标注 **iOS 影响面**；默认全部为"零"（§3.3）。

### P0 · rebase 对齐（前置）
- **内容**：worktree 里试 `git rebase origin/main`（**41 提交**，2026-10-16 实测）；处理冲突（预计集中在 dm/ios、dm/host、dm/ios/tools）；核实 `dm_cursor.c` 兜底块归属；拆分可评审提交（可选）。上游 v1.0.15–17 顺带到手：图形翻译线程化（`b94670c`）、显存池按需驻留、**v1.0.17 存档合并防丢装备**；`patch_nso.py` 带 r41 正确性修正（§4.5）。
- **复用标注**：无新代码。
- **iOS 影响面**：零（只动 android-ours）。
- **验收**：1) rebase 后 iOS/Mac 冒烟通过（共享文件被重排过）；2) Android 构建过（CMake 不死）；3) `dm_tc_panel_mode`/`joy_zone_r` 在 branch 里 grep 命中；4) 按 41 笔复核前端契约增项（`afad65c` 界面代号 `dm_ui_gen/dm_wref`、`dm_hud holding→id`）；5) `nvn_vk.c` 对新版 `hle_nvn.c`/`nvn_obj.h` 编译通过（翻译线程化接口适配，归图形线）。
- **风险**：冲突面未知 → 先 worktree 预演；兜底块若与上游冲突，按 §3.2 登记。

### P1 · 触摸采集 + 坐标映射
- **内容**：`DmHostActivity` 加 `onTouchEvent`（ACTION_POINTER 槽位表、16 槽、cancel 清理）→ 新 JNI（延续静态注册风格）→ `front_android.c` 填 `dm_touch[]`；移植 front_ios.m:216-221 归一化 + letterbox/front_game_res 映射；surface 尺寸变化热更新。
- **复用标注**：C 逻辑参照移植（无共享修改）。
- **iOS 影响面**：零。
- **验收**：`dm_touch_set` 收到整组推送（日志核对）；槽位行为与 iOS 13 项清单第 1、11 项一致（摇杆起摇、手柄模式吞触点→后续 P3/P6）。

### P2 · 发布三件套 + 帧钟
- **内容**：`front_android.c` 增加前端 tick：每帧 `dm_input_set` + `dm_cursor_set_screen`（pointer_valid 时）+ `dm_ui_mouse_pump` 入口；每次触点变化整理推送；120Hz 脉冲（用于 dm_ui_key_pulse/dm_tc_pulse 等效路径，实现形态：Java Handler/Choreographer 回调或 native 定时，保持"主线程"语义）。帧钟依赖 §6 开放问题 #1 的结论（Choreographer 优先）。
- **验收**：与 iOS 同脚本输入下，三 API 的推送序列/频率双端对齐（±1 帧）。

### P3 · 触摸模型接线
- **内容**：用 surface 尺寸+`dm_tc_scale_for_size` 初始化 `dm_tc_layout_make`；喂 `dm_tc_begin/move/end_at`；insets 先 0、P7 接真实值；`dm_tc_panel_mode` 语义对齐（来自 rebase）。
- **验收**：13 项行为清单中 1–4、6–9、11–12 项在 Android 上逐项复现；摇杆贴边行为与 iOS 一致（浮动摇杆语义）。

### P4 · overlay 生成接入 Android 构建
- **内容**：`build_apk.sh` 在 native 构建前调用 `gen_assets.py`（§3.2 注意事项选 a/b）→ `gen_romfs_overlay.py`（与 iOS 同参数）；产物两个投放方式：**开发流**=`run.sh` adb push 到 `/sdcard/Android/data/org.d2rm.host/files/romfs_overlay`；**发布流**=入 APK assets 首启释放（后续定）。`dm_env.c` 加 `setv_default("DM_ROMFS_OVERLAY", root/romfs_overlay)`（外部可覆盖，符合现有风格 dm_env.c:61）。
- **复用标注**：B 构建期复用（不改脚本则零共享修改）。
- **iOS 影响面**：零（若选 b：生成器加 `--out` 属加法，需登记）。
- **验收**：16 项 overlay 差异全部可见；`.build.config` 自检过（run.sh:69-72 同款检查移进 Android 打包）。

### P5 · 覆盖绘制层（Android View）
- **内容**：新建 `DmTouchOverlayView`（叠在 SurfaceView 上）按 DmTouchOverlay.m 的元素清单重画：5 技能钮/回城/地图/菜单/换组/摇杆/底栏 v4/拖货影；状态源：JNI 暴露 `dm_tg_snapshot` 等效快照 + `refreshPortalBinding` 等效刷新；素材：`gen_assets.py` 产出的 PNG（技能图集）/底栏 v4 数据；几何从 dm_touch_controls 布局结构体直接取。
- **验收**：`controlMetadata` 等效接口（Android 版）输出的控件 rect 与 iOS 数值 diff ≯ 容差；截图对比。

### P6 · 游戏内 UI 注入闭环
- **内容**：接入 `dm_ui_touch/world_tap/item_tap/use_item/long_press/tab_go/bind_begin/key_pulse` 等（front_ios.m:544-629 对照）；确保 `dm_ui_mouse_pump` 每出图帧一条（dm_cursor.c:2465）；保留并核对面板树兜底（dm_cursor.c:2838-2887）；OBS_UIMSG 根因另立小任务。
- **验收**：主菜单→建角→进游戏→背包/商人 全流程自动化在 Android 复现；崩/卡（T8）观察记录。

### P7 · 外壳对齐
- **内容**：`onPause/onResume/onWindowFocusChanged` → focus 事件序列（对照 front_ios.m:1195-1214：dm_focus_set、取消触摸、存盘节流、时钟冻结、GPU 排空待出图路径定）；沉浸式全屏 + `WindowInsets`→`dm_tc_set_safe_insets`/`dm_ui_safe_*`（含 cutout）；`FLAG_KEEP_SCREEN_ON`；IME 对话框 + 规则移植（front_ios.m:1011-1063）；`stopHost` 接 `dm_host_shutdown`；launcher 从 `DmProbeActivity` 切到 `DmHostActivity`（决策点 1）。
- **验收**：§8.4 外壳场景表全过。

### P8 · 帧节奏收口 + 触觉 +（可选）手柄
- **内容**：确认出图路径后绑定帧钟（目标：与 iOS 同帧率策略，60 默认 + 30 档）；120Hz 脉冲最终形态；Vibrator 三档映射 dm_drophaptic 输出；手柄/鼠标模式（dm_pad_set、真鼠标事件队列）视决策点 3。
- **验收**：输入→出图延迟与 iOS 同档（同机无法并测，用帧级日志比对）；掉落震动触发点一致。

---

## 8. 验收协议（"一模一样"怎么证明）

### 8.1 行为清单（13 项，逐项双端对照）

1. 移动摇杆（固定底座→浮动语义按新版）　2. 点主技能（自动选目标）/拖主技能定方向　3. 点/拖技能 1–4（点=自动目标，拖=定方向；第二组带 ZL）　4. 回城钮→宿主直接开免费回城　5. 腰带 1–4 点喝/上滑展开/下滑收起　6. 技能组切换Ⅰ↔Ⅱ　7. 小地图名称条→全屏地图/点空处返回　8. 点世界交互（点空地不发令）　9. 前端页：点=点击、纵向拖>10pt=滚动　10. 面板：单击出说明、双击使用/购买/装备/鉴定、长按补腰带、页签、拖放（买卖存取丢弃）　11. 手柄模式碰屏→切回触屏且该触点不执行　12. 技能绑定（空位十字进树、点技能=光标+Y）　13. 文本输入（起名/金币）规则一致。

### 8.2 视觉清单（overlay 16 项）

见 §4.2 列表（手柄提示全灭、金色 X、职业竖条、底栏 v4、符文助手、技能绑定页键名、暂停菜单、文案替换……）逐项截图核对。

### 8.3 工具化对比（iOS 钩子移植到 Android）

- `captureTouchLayer` / `controlMetadata`（front_ios.m:731-740；DmTouchOverlay.m:273-314）→ Android 同构接口，输出控件 rect/按键/技能绑定 schema，做**几何 diff**。
- `DmScriptTouch`（front_ios.m:88-97）等效脚本触摸 → 相同脚本双端回放，做 **`dm_touch_set`/`dm_ui_*` 日志流 diff**（±1 帧）。
- 截图对比：沿用 VK_RENDER_PROGRESS 的截图工具链。

### 8.4 外壳场景表

后台切出→回前台（存档/时钟/GPU/音频路径）、横屏锁定、沉浸式（状态栏/导航条/刘海）、文本输入（中英/数字/取消）、保屏、退出（dm_host_shutdown）。

---

## 9. 风险登记册

| 风险 | 等级 | 缓解 |
|---|---|---|
| rebase 冲突超预期 | 中 | worktree 预演；拆提交 |
| 出图路径矛盾（P1 自述 vs 实测） | 中 | P0-P1 上机钉死，写回本文 |
| OBS_UIMSG 根因不明导致边缘面板自动化缺位 | 中 | 主链绕开；兜底覆盖 8 面板；根因单列 |
| T8 停滞与 UI 无关的独立原因 | 中 | P6 观察；立专项 |
| 生成器写 iOS 源码树（§3.2 注意项） | 低 | 方案 a/b 二选一 |
| Android 帧驱动与出图节奏不齐 | 中 | P8 收口；延迟日志比对 |
| Launcher 切换影响开发流（DmProbeActivity 常用） | 低 | 保留 probe 入口，仅改默认可点入口 |

---

## 10. 决策点（需拍板）

1. **Launcher**：`DmProbeActivity` → `DmHostActivity` 的切换时机（P7 内做，还是保持现状到收尾）。
2. **音频**：随 P7 顺带接 AAudio 设备层（`audio_dev_android.c` 已有骨架），还是独立立项（非 UI 决策）。
3. **手柄/鼠标**：dm_pad_set / 真鼠标模式进不进本期（P8 可选项；触屏体验不依赖）。
4. **OBS_UIMSG 根因调查**：立刻单列，还是等 P6 观察后再定。
5. **英文版生成时机**：本文评审定稿后生成 `ANDROID_UI_PARITY.md`（推荐定稿即转，避免双份维护）。

---

## 附录 A · 关键证据索引

| 主题 | 证据 |
|---|---|
| 触摸入口/槽位 | front_ios.m:117,132-136,481-558 |
| 归一化/换面板算 | front_ios.m:216-221；front.h:33-38 |
| 发布三件套 | front_ios.m:216-227,455-463,544-554,629 |
| 帧节奏 | front_ios.m:858-877,668 |
| 浮动摇杆/面板模式 | dm_touch_controls.c（origin/main：cfa70b3、d9e95c3） |
| 覆盖绘制元素 | DmTouchOverlay.m:39,48-61,156-272,273-314 |
| overlay 生成 | run.sh:32,62-72；gen_romfs_overlay.py；touch_layout.py:31-52,88-120；touch_strings.py:14-51,63-93,103-127 |
| overlay 加载 | main.m:608-621；hle_fs.c:89-90,172-181,207,251-260 |
| 生命周期 | main.m:917-950；front_ios.m:1195-1214 |
| 文本输入 | front_ios.m:991-1106 |
| Android 现状 | front_android.c:2,180-231,263-281；DmHostActivity.java:45,121-133,216-231；AndroidManifest.xml:26-31,51-57；dm_host_jni.c:112-116；hle_lifecycle_android.c:49 |
| 下游到达 | hle_touch.c:21-46；hle_hid.c:60,407,414,451 |
| 兜底/门 | dm_cursor.c:2465,2831-2887；diag.c:342-363 |
| 隔离机制 | CMakeLists.txt:71-72,129,150；dm/host/Makefile:5,10；project.yml:19,142-162；gen_assets.py:6,42；dm/ios/.gitignore:4,10；build_apk.sh:67,74-77 |

## 附录 B · 共享修改登记表（模板 + 当前记录）

| 日期 | 文件 | 原因 | 对 iOS 行为影响评估 | 验证（Mac 冒烟/TouchSmokeTests/构建） | 提交 |
|---|---|---|---|---|---|
| （待填） | | | | | |

**当前预期记录：无。** 若 P0 核实 `dm_cursor.c` 兜底块为 android-ours 引入的共享改动，在此登记并复核其 iOS 中立性。

## 附录 C · 本方案不覆盖的内容

图形层（nvn_vk.c，见 ANDROID.md C 类）；云存档/更新检查/分享日志等 feature parity；iOS 侧任何重构；把 android 合并回 main 的时机与形态。
