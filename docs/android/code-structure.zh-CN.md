# Android 端代码结构（D2R Mobile）

> **这份文档是什么（人话）**：Android 版从 Java 壳到底层 Vulkan 回放、再到原版游戏模块的完整落地图——哪个目录、哪个文件干什么，
> 启动时按什么顺序装配，和上游 / iOS 共享的那一半边界在哪，想改一处东西该去哪个文件。
> **给谁看**：第一次读 Android 代码的人（含 AI），以及加功能 / 调性能 / 同步上游之前要先定位的人。
> **不在这里重复**：构建步骤见 [README.android.zh-CN.md](../../README.android.zh-CN.md) §2，运行期开关见它 §5，
> 改上游文件的硬性规矩见 [docs/android/isolation-analysis.zh-CN.md](isolation-analysis.zh-CN.md)，每次同步的记录见
> [docs/android/upstream-sync.zh-CN.md](upstream-sync.zh-CN.md)，与 iOS 的功能对照见 [docs/android/ui-parity/analysis.zh-CN.md](ui-parity/analysis.zh-CN.md)。

---

## 1. 分层与线程

```text
Java 壳（dm/android/src，跑在 UI 线程）
 ├─ GameActivity          生命周期、Surface、模块解包校验、250 ms 状态轮询
 ├─ TouchControlsView     Canvas 触摸层：摇杆 / 技能弧 / 底栏 / 地图键，收手势、每帧读布局数组
 └─ ShellTextInputDialog  起名 / 文本框
        │  JNI（nativeConfigure / nativeStart / nativeSurface / nativeEvent / nativeFrame …）
        ▼
原生层 libd2r_mobile_android.so（dm/android/*.c + dm/host + dm/gfx/nvn）
 ├─ app_jni.c             JNI 入口 + game_thread 装配（各层唯一的串联点）
 ├─ 平台服务              services_platform.c / services_sdk.c / services_fs.c / romfs_image.c
 ├─ 输入                  input_backend.c → dm_input；touch_controls_android.c 适配共享触屏状态机
 ├─ 图形                  nvn_vk.c（NVN 录制 → Vulkan 回放）+ nvn_platform_clock.c
 ├─ 游戏侧接管            svc_host_android.c / hud_read_android.c / diag_android.c / ui_state_android.c
 └─ 共享宿主              dm/host/*.c、dm/ios/Sources/{dm_touch_controls,dm_touch_game_*}
        │
        ▼
原版游戏模块（main / sdk / subsdk0，本地改写器打补丁后由 loader.c 装入执行）
```

线程就三条：

| 线程 | 谁建的 | 干什么 |
|---|---|---|
| Java UI 线程 | Android | Activity 生命周期；TouchControlsView 的 `onDraw` / `onTouchEvent`（`postInvalidateOnAnimation` 逐帧重画） |
| `game_thread` | `nativeStart` 里的 `pthread_create`（8 MB 栈） | 校验资源、装载模块、跑 `nnMain`；Vulkan 提交发生在调用 NVN 的那个游戏线程上（首版队列是同步的） |
| 游戏自建线程 | 原版 `nn::os` | 经 `dm/host/thread.c` + `thread_slots.c` 落到 pthread |

---

## 2. 目录总览

```text
dm/android/
├── CMakeLists.txt            三个 OBJECT 库 + 一个 .so + 三个探针可执行（见 §8）
├── AndroidManifest.xml       GameActivity / sensorLandscape / minSdk 29 / versionName 0.2-native-android-port
├── app_jni.c                 JNI 入口；game_thread 的装配顺序全在这一个文件里
├── src/org/d2r/binaryprobe/  Java 壳（3 个文件，见 §3）
├── tools/                    构建、生成、部署、扫描、审计（13 个脚本，见 §8）
├── CommonCrypto/             CommonDigest.h 垫片头（实现见 commoncrypto_shim.c）
├── TargetConditionals.h      共享宿主代码用的 Apple 条件宏，全部为 0
├── apple_time_android.h      Apple 时钟 / stat 写法垫片（CMake 用 -include 强制编进宿主 .c）
└── build/                    构建产物（APK、原生库、覆盖层、生成资源；不进版本库）
```

---

## 3. Java 壳（`src/org/d2r/binaryprobe`）

| 文件 | 职责 | 关键点 |
|---|---|---|
| `GameActivity.java` | Activity：建 SurfaceView、解包校验模块、`nativeConfigure` / `nativeStart`、轮询 `nativeStatus` | 模块从 `assets/modules/*` 按 `module-manifest.json` 的 SHA-256 校验后落到 files 目录；`surfaceWorker` 串行化 Surface 替换，避免换面时卡生命周期线程；250 ms 轮询把状态画到启动页，并按 `nativeFrontend()` 决定触摸层显隐（局外隐藏） |
| `TouchControlsView.java` | 触摸层：画按钮 / 摇杆 / 技能图标 / 底栏 / 地图键，处理多指 | 每帧从 `nativeFrame()` 取布局数组（坐标、半径、按住位、HUD 值），触摸走 `nativeEvent()`；技能图标与充能来自 `nativeSkillIcons()`；无障碍节点也在这里生成 |
| `ShellTextInputDialog.java` | 文本框（起名等） | 与 `keyboard_android.c` 的 `nativeKeyboardRequest` / `nativeKeyboardResult` 配对 |

JNI 入口一览：

| 来自 | 函数 | 去向 |
|---|---|---|
| GameActivity | `nativeConfigure` / `nativeStart` / `nativeSurface` | `app_jni.c`（配置、起 `game_thread`、交接 ANativeWindow） |
| GameActivity | `nativeTouch` / `nativeActions` | `app_jni.c` 转发进 `input_backend.c`（触摸点 / 手柄动作发布） |
| GameActivity | `nativeStatus` / `nativeKeyboard*` | `app_jni.c`、`keyboard_android.c` |
| TouchControlsView | `nativeEvent` / `nativeFrame` / `nativeActivate` / `nativeEnable` / `nativeCancel` / `nativeResize` / `nativeFrontend` / `nativeHud` / `nativeSkillIcons` / `nativeUiText` | `touch_controls_android.c`（适配层 + 共享触屏状态机） |

---

## 4. 原生层文件（按职责）

### 4.1 启动与装配

| 文件 | 职责 |
|---|---|
| `app_jni.c` | 所有 `Java_org_d2r_binaryprobe_*` 入口；`game_thread` 按 §6 的顺序把资源、服务、门、模块、图形一路装起来；画质档 `quality.txt` 也在这里读 |
| `services_platform.c` | 平台服务后端：profile 目录、窗口状态、locale、熵源；OE / AOC / ErrorContext / ParentalControl / AbortObserver 的 HLE 初始化；缺后端直接启动失败 |
| `services_sdk.c` | 服务注册表（`DM_ANDROID_SERVICE_*` → 后端函数） |
| `thread_slots.c` | 客机 TLS / 门槽：Android Q+ 保留了 `TLS_SLOT_APP`，所以 x18 / TLR / 门另存自有数组 + pthread key，Bionic 的 TP 与 x18 不动 |
| `apple_time_android.h` | `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)`、`st_mtimespec` 等 Apple 写法的等价实现 |
| `TargetConditionals.h` | 让共享宿主里的 `<TargetConditionals.h>` 在 Android 上得到全 0 |

### 4.2 资源与存档

| 文件 | 职责 |
|---|---|
| `romfs_image.c` | 把玩家自己的 `game.romfs`（8.9 GB，大小与哈希都校验）挂成只读虚拟视图：只建目录、不解包、不落地；`DM_ROMFS=view_root` |
| `services_fs.c` | 事务式存档：目录克隆、`fsync`、原子换名提交；guest 文件名用 xattr `user.org.d2rmobile.guest-name` 保存（大小写 / 非法字符） |
| `services_fs_probe.c` | 存档提交的探针实现（编进 core，供自检调用） |
| `game_resource_probe.c` | 启动时的资源自检：对 `romfs_samples.h` 里的期望值逐项比对（期望值由 `tools/gen_romfs_samples.py` 从玩家文件生成） |

### 4.3 输入

| 文件 | 职责 |
|---|---|
| `input_backend.c` / `.h` | 触摸与手柄的路由：校验指针 id、样式、handheld 模式，把事件发布进 `dm_input`（游戏读的就是它） |
| `touch_controls_android.c` | 共享触屏状态机（`dm/ios/Sources/dm_touch_controls.c`）的 Android 适配：JNI、每帧布局数组、面板掩码 / 导航态、技能绑定、HUD、i18n 取串 |
| `keyboard_android.c` | 文本输入桥（`dm_ui_text_*` ↔ Java 对话框） |
| `ui_state_android.c` | UI 消息门 → 前端 / 面板状态机（对齐 iOS `front_ios.m`），触摸层按它切掩码与导航 |

### 4.4 图形

| 文件 | 职责 |
|---|---|
| `dm/gfx/nvn/nvn_vk.c` | Android 的 Vulkan 回放后端：保持录制侧的 `mtl_*` 接口不变，把 NVN 命令翻译成 Vulkan；不支持的语义按名字报边界，不静默丢 draw / dispatch。文件自己 `#ifdef __ANDROID__` 挡住 Apple 构建 |
| `nvn_platform_clock.c` / `.h` | 帧时钟、等待分类记账（fence / queue idle / acquire / present / event）、渲染线程调度与睡眠 |
| `nvn_platform_backend.h` | ANativeWindow 与 Vk 生命周期接口（`dm_vk_graphics_begin/end`） |
| `vulkan_probe.c` | 独立 Vulkan 探针可执行（不入 APK，见 §8） |

### 4.5 音频与视频

| 文件 | 职责 |
|---|---|
| `dm/host/hle_audio.c` + `audio_aaudio.h` | 音频 HLE 落到 AAudio；头文件只暴露诊断用的指标快照 |
| `codec_opus.c` | 原版 SDK 自带 libopus 软解（Horizon 的硬件 Opus IPC 在 Android 上没有），对应排除掉的 `hle_codec.c` |
| `movie_start.c` | 视频启动应答：等解码器自己的 error / input 回调再回报成功（MediaCodec 的应答可能早于真实错误），对应排除掉的 `hle_movie.c` |
| `audio_queue_test.c` | 音频队列 / 事件的独立压测（不进 CMake、不入包） |

### 4.6 游戏侧接管（svc / 观测门）

| 文件 | 职责 |
|---|---|
| `svc_host_android.c` | svc 0x80+ 的分发：TACT BLTE 解压（libdeflate）、技能瞄准门、技能渲染门，其余"原样代调"回被剪掉的原函数并回填返回值 |
| `hud_read_android.c` | 服务端拍（`OBS_TICK`）读受控玩家 → `dm_hud`：等级 / 经验‰ / 腰带 / 回城 / 生命法力 / 坐标 / 区域 |
| `diag_android.c` | `diag.c` 在安卓不编，这里补最小面：请游戏存盘、秘境里拦原版难度提示、`libSystem` 计数接口的中性实现 |
| `i18n_android.c` | 只提供 `dm_sys_lang`：系统 locale → 原版语言号（中文及汉语方言 → 8，其余 → 0）；取串本身是共享的 `dm_i18n.c` |
| `feature_stubs_android.c` | 尚未移植的功能子系统的中性桩（自定义页 / 极乐世界 / 免回城 / 秘境 / 编年史 / 符文之语 / 百宝囊 / 自动喝药 …），每项注明真实实现所在文件 |

### 4.7 诊断与探针

| 文件 | 职责 |
|---|---|
| `diagnostics.c` | 信号处理与崩溃转储、`game-startup.log` / `status.json`、出图分辨率（`front_game_res`：`quality.txt` 档位 → 出图 + 3D 比例，`DM_GAME_RES` 最优先） |
| `host_probe.c` | 宿主探针可执行：装载模块、跑门检查（配 `gate_check.S`） |
| `input_probe.c` | 输入探针可执行 |
| `services_fs_probe.c` | 存档事务探针（编进 core 库，不是独立可执行） |
| `gate_check.S` | 门检查用的汇编入口（`dm_android_gate_check`） |

### 4.8 兼容垫片

| 文件 | 职责 |
|---|---|
| `commoncrypto_shim.c` | SHA-1 / MD5 的 CommonCrypto 等价实现（`dm_padmode.c` 的字符串旁路表校验、诊断用） |

---

## 5. 与上游 / iOS 共享的那一半

APK 里的宿主代码**大部分不是 Android 目录写的**，而是与 iOS / Mac 共用同一份源码：

| 来源 | 编进 Android 的东西 |
|---|---|
| `dm/host/` | `loader.c`、`hle.c`、`entry.S`、`thread.c`；**所有 `hle_*.c` 用通配收**；`dm_*.c` 功能模块（`dm_cursor` / `dm_custom` / `dm_i18n` / `dm_rift` / `dm_vault_ui` …）逐条列出 |
| `dm/ios/Sources/` | `dm_touch_controls.c`（可移植触屏状态机）、`dm_touch_game_bridge.c`、`dm_touch_game_state.c`、`dm_touch_game_identity.c` |
| `dm/gfx/nvn/` | `hle_nvn.c`、`nvn_entry.S`、`nvn_thunks.S`、`nvn_tex.c`、`nvn_dirty.c`、`nvn_vk.c`（后者是 Android 专属） |

`hle_*.c` 用通配而不是手写清单，是因为 `hle_charname.c` 曾经静默漏编（真机：中文角色名输进去什么都不剩）。不编的必须进 `DM_HLE_SKIP` 并写原因：

| 排除 | 原因 / 安卓替代 |
|---|---|
| `hle_codec` | 安卓用 `codec_opus.c` 走原版 libopus，两个都编会撞符号 |
| `hle_granny` | 安卓在 `svc_host_android.c` 里有等价实现 |
| `hle_movie` | 安卓用 `movie_start.c` 接管 `movie::*` |
| `hle_private_aim` | 安卓在 `svc_host_android.c` 里有等价实现 |
| `hle_tact` | 同上（BLTE / 瞄准 / 渲染门） |
| `hle_count` / `hle_fdstat` / `hle_table` | Mac 诊断 / `getdtablesize` / swhle 段，安卓编了没用 |

往共享文件里写代码只有三种合法手法（细则见 [docs/android/isolation-analysis.zh-CN.md](isolation-analysis.zh-CN.md)「硬性规矩」）：

1. **双分支** `#ifdef __ANDROID__ … #else … #endif`；
2. **小钩子**：共享文件里只留一两行带 `#ifdef __ANDROID__` 的调用，实现放 `dm/android/`；
3. **追加区段**：头文件 / 枚举加东西必须进 `ANDROID-BEGIN / ANDROID-END` 块。

两条配套规矩：

- **Android 独有符号，引用处必须守**（规矩 6）：实现在 `dm/android/` 的函数，共享文件里每一次引用都要进 `#ifdef`，否则 Apple 侧链接会挂在 `undefined symbols`。
- **新文件自己声明平台归属**（规矩 7）：例如 `nvn_vk.c` 自己 `#ifdef __ANDROID__` 挡住 Apple 通配构建；反过来，若构建入口显式列出它，就定义 `DM_EXPECT_<something>` 让走错分支时 `#error`（`DM_EXPECT_VULKAN_BACKEND`）。

**平台开关只对 Android 出包打开**，保证 iOS 那份逐字不变（都集中在 `dm/android/tools/gen_overlay.sh`）：

| 变量 | 影响 |
|---|---|
| `DM_TOUCH_TWO_CARDS` | 建角模式页只留标准 / 专家两块牌子（挪走资料片 / 经典版那一行） |
| `DM_TOUCH_RES_TIER` | 自定义页多一行「分辨率」（写 `dm_custom.c` 的 `q_res`，重启生效） |
| `DM_TOUCH_LANG_RU_WIDE` | 语言行俄语格加宽 200 → 250（西里尔字母宽，不折行） |

每次收尾跑 `bash dm/android/tools/sync_surface_audit.sh`：数「新建文件 / 改上游文件 / 冲突面」，冲突面只许降或持平。

---

## 6. 启动链路（`app_jni.c` 的 `game_thread`）

1. `crash_thread_init()`；若存在 `diagnostics/debug-hold` 就 `SIGSTOP`（等调试器）。
2. 校验 `files/game.romfs` 存在且大小为 8,973,693,207 字节。
3. `romfs_image_setup_checked()` 建只读虚拟视图；建 `save/ temp/ profile/` 三个私有目录。
4. 设环境：`DM_ROMFS`、`DM_SAVE`、`DM_TEMP`、`DM_ROMFS_OVERLAY`（= `diagnostics/overlay`）、`DM_LOCALE_TAG`；`dm_custom_init(port_root)`（语言开关要在挂存档之前就能读到）。
5. `dm_android_platform_bind()` + `dm_android_platform_register_services()`：平台服务与 sdk 后端就位。
6. 装观测门：`dm_android_ui_state_install()`、`dm_android_hud_install()`、`dm_android_diag_install()`；上报初始窗口状态。
7. `load_modules()`（main / sdk / subsdk0）+ `guest_thread_init()`。
8. 触屏相关装配：`dm_padmode_boot()`（布局前缀 + 字符串旁路表）、`target_rank_install()`（点按自动选怪）、`dm_turnsmooth_install()`、技能绑定桥 `dm_touch_game_attach()`。
9. `svc_thread_main_init()` → `_init_libc0` → `nnosInitialize(handle,0)` → `_init_libc1` / `nninitInitializeSdkModule` / `nninitStartup` / `_init_libc2` → 资源自检 `dm_android_real_resource_probe()`。
10. `run_module_inits()`，设 host argc / argv，`nnMain()` 进游戏；返回后写「游戏已退出」。

---

## 7. 运行期目录

应用数据根：`/sdcard/Android/data/org.d2r.binaryprobe/files/`（deploy 脚本推的就是这里）

| 路径 | 内容 | 谁读 |
|---|---|---|
| `game.romfs` | 玩家自己的原版 RomFS 镜像（8.9 GB，部署时推一次） | `romfs_image.c` |
| `d2r-mobile/modules/` | `main / sdk / subsdk0` 的 `.patched.bin`（从 assets 校验后落盘） | `loader.c` |
| `d2r-mobile/shaders/` | 离线翻译好的着色器缓存 + `pass-coverage.tsv` 种子 | 图形层 |
| `d2r-mobile/save/` | 事务式存档（工作目录 + 已提交） | `services_fs.c` |
| `d2r-mobile/diagnostics/` | `game-startup.log`、`status.json`、`env.txt`、`quality.txt`、`flag-*` 等运行开关 | `diagnostics.c` / `app_jni.c` |
| `d2r-mobile/diagnostics/overlay/` | RomFS 覆盖层（`gen_overlay.sh` 产物，adb 可写，先于基底镜像生效） | `hle_fs.c` |
| `d2r-mobile/sw_custom.txt` | 自定义页设置（`q_res` / `lang` / 各开关） | `dm_custom.c` |

---

## 8. 构建目标与产物

| 目标 | 类型 | 收什么 |
|---|---|---|
| `dm_android_core` | OBJECT | 共享宿主（`loader` / `hle` / `hle_*.c` 通配 / `dm_*.c` / iOS 触屏四件）+ 平台侧 `.c` 列表（服务、输入、UI 状态、HUD、诊断…） |
| `dm_android_gfx` | OBJECT | 共享图形源 + `nvn_vk.c` + `nvn_platform_clock.c` + `movie_start.c` |
| `dm_libdeflate` | OBJECT | 第三方 libdeflate 的解压部分（TACT BLTE） |
| `d2r_mobile_android` | SHARED | `app_jni.c` + `game_resource_probe.c`，链上面三个 → `libd2r_mobile_android.so`（APK 里唯一的原生库） |
| `dm_android_host_probe` / `dm_android_input_probe` / `dm_android_vulkan_probe` | 可执行 | 探针，不入 APK |

打包不走 Gradle：`tools/build_apk.sh` 用 SDK 的 `aapt2` / `d8` / `zipalign` / `apksigner` 直接出包。

`tools/` 一览：

| 脚本 | 用途 |
|---|---|
| `build_modules.sh` | 用本树改写器出 `main / sdk / subsdk0` 的 patched 模块 + 三张编译期查表 |
| `gen_overlay.sh` | 出 RomFS 覆盖层（**Android 平台开关都在这**） |
| `build_apk.sh` / `deploy_apk.sh` | 出包签名 / 装机 + 推数据 + 启动 |
| `gen_headers.sh` | 三张查表（`dm_hud_exp.h`、`dm_vault_tab.h`、`dm_bright_levels.inc`） |
| `gen_icons.py` / `gen_module_manifest.py` / `gen_romfs_samples.py` | 图标 / 模块清单 / 资源自检期望值 |
| `sync_surface_audit.sh` | 同步面审计 |
| `scan_coverage.sh` + `coverage_scan_report.py` | 逐通道覆盖率表批量扫描与合并 |
| `worldtier_matrix.sh` + `ui_geom.py` | 世界档矩阵 + 从截图量界面几何（判 UI 有没有被缩放） |

---

## 9. 常见改动落点

| 想改什么 | 去哪 |
|---|---|
| 加一个 HLE / 平台服务 | `services_sdk.h` 的枚举 + `services_platform.c` 的初始化 + `dm_android_platform_register_services()` |
| 自定义页加一行开关 | `dm/host/dm_custom.c`（`k_keys` / `k_def` / 下标宏）+ `dm/ios/tools/touch_layout.py` 的 `_CUSTOM_KEYS` + `dm/i18n/ui_text.tsv` |
| 改触屏布局 / 外观 | `dm/ios/tools/touch_layout.py`（布局）、`TouchControlsView.java`（Canvas 外观）；平台差异用 `gen_overlay.sh` 里的变量 |
| 加界面文字 | 只改 `dm/i18n/ui_text.tsv`，再 `python3 dm/ios/tools/dm_i18n.py --header > dm/host/dm_i18n_tab.h`（**不要手改**） |
| 分辨率档 / 出图尺寸 | `dm/android/diagnostics.c` 的 `front_game_res` / `quality_out_compute`；界面档来自 `dm_custom_res_tier()` |
| 开局 / 进局的接管逻辑 | `dm/host/` 里对应模块；平台相关的观测门在 `svc_host_android.c` / `hud_read_android.c` / `diag_android.c` |
| 崩溃、启动卡住、黑屏 | `diagnostics/game-startup.log`（信号处理与状态写在 `diagnostics.c`） |
| 新增平台专有符号 | 实现放 `dm/android/`；共享文件里的引用按规矩 6 加 `#ifdef __ANDROID__` |

---

## 10. 校验入口

```bash
cd dm/android/build && ninja                     # 原生库（-Werror）
bash dm/android/tools/sync_surface_audit.sh      # 冲突面只许降 / 持平
python3 dm/ios/tools/dm_i18n.py --check          # 串表与生成头一致
python3 dm/i18n/scan_literals.py                 # 源码里的中文字面量全在白名单
python3 dm/ios/tests/test_touch_layout_options.py
python3 dm/ios/tests/test_touch_layout_language.py
python3 dm/host/tests/native_write_scan.py --selftest dm/host   # 不写原版控件
python3 dm/host/tests/widget_cache_scan.py --selftest dm/host
```

真机自检、诊断开关与性能档见 [README.android.zh-CN.md](../../README.android.zh-CN.md) §4–§6；
性能经验与坑见 [docs/android/perf/lessons.zh-CN.md](perf/lessons.zh-CN.md)。
