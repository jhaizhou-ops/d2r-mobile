# D2R Mobile · Android

**把你自己购买的 Nintendo Switch 版《暗黑破坏神 II：狱火重生》（含「术士君临」资料片）原生运行在 Android 手机上。** 游戏自身的 aarch64 程序直接在设备上执行，没有 CPU 模拟；平台服务与图形 API 由本仓库的宿主层接管，图形后端用 Vulkan 重新实现，着色器离线翻译。

English: [README.android.md](README.android.md) · 主仓库说明: [README.zh-CN.md](README.zh-CN.md) · 游戏文件准备: [docs/GAME_FILES.zh-CN.md](docs/GAME_FILES.zh-CN.md)

> **状态：开发中。** Android 端已经能完整游玩（进游戏、战斗、存档、设置、触屏操控全部可用），但性能尚未达到 iOS 端的 60 fps 水准——当前实测约 **30 fps**（见下文「实测性能」）。发版前的目标与已知限制都写在本文档里。

---

## 1. 你需要准备什么

### 1.1 设备

| 项目 | 要求 |
|---|---|
| 系统 | Android 10 及以上（API 29+） |
| 架构 | arm64-v8a |
| 图形 | 支持 Vulkan 1.1，厂商驱动（**Adreno 已实测**） |
| 内存 | 建议 6 GB 以上（游戏 RomFS 约 8.4 GB，另需存档与着色器缓存空间） |
| 存储 | 建议预留 10 GB 以上可用空间 |

**已实测设备**：Xiaomi 22081212C（Snapdragon 8+ Gen 1 / Adreno 730 / Android 16，12 GB）——本文档中所有性能数据都来自这台。

### 1.2 游戏文件

**你必须拥有正版游戏。** 仓库里没有、以后也不会有任何游戏文件。需要 Switch 版 **1.0.34.0**（对应 PC 版 **3.3.94012**）的两部分：

- 程序文件（ExeFS）：`main`、`sdk`、`subsdk0`，约 26 MB
- 更新包资源（RomFS）：`data/` 目录，约 8.4 GB

导出方法见 [docs/GAME_FILES.zh-CN.md](docs/GAME_FILES.zh-CN.md)（用你自己的主机与 nxdumptool 导出）。**版本必须对上**，否则生成脚本会直接报错并指出不一致的地方。

---

## 2. 构建

### 2.1 工具链

| 依赖 | 版本 |
|---|---|
| Android NDK | 27.2.12479018 |
| CMake | 3.22 及以上 |
| Ninja | 任意近期版本 |
| JDK | 17（打包 APK 用） |
| Python 3 | 生成资源与运行校验脚本 |

### 2.2 步骤

```bash
# 1) 生成资源与 ROMFS 覆盖层（需要上面那份游戏文件）
dm/android/tools/gen_overlay.sh
#    若资源已生成过、只想重打包，可用 DM_ASSETS_REUSE=1 跳过重复解包
#    DM_ASSETS_REUSE=1 dm/android/tools/gen_overlay.sh

# 2) 编译原生库
cd dm/android/build && ninja

# 3) 打包 APK（产物：dm/android/build/apk/d2r-mobile-android-<版本>+<构建号>.apk，例 d2r-mobile-android-0.2-native-android-port+20261020.apk；
#    版本号取自 AndroidManifest.xml，构建号缺省当天日期，可用 DM_APK_BUILD 覆盖）
#    启动图标复用 iOS 那份美术（dm/ios/Assets.xcassets/AppIcon.appiconset/AppIcon1024.png），
#    打包时由 tools/gen_icons.py 缩成 5 档密度，仓库里不另存一套
dm/android/tools/build_apk.sh

# 4) 部署到设备并启动
ADB=adb ANDROID_SERIAL=<你的设备> dm/android/tools/deploy_apk.sh
```

`deploy_apk.sh` 会做三件事：把游戏数据（RomFS 与着色器缓存）推到应用数据目录、安装并启动 APK。数据目录约定：

```
/sdcard/Android/data/org.d2r.binaryprobe/files/d2r-mobile/
├── romfs-view/          游戏资源
├── shaders/             离线翻译好的着色器
├── diagnostics/         日志与诊断开关（见下）
└── ...
```

首次启动会做一次较慢的初始化（解包/索引资源），之后进游戏约 1 秒。

---

## 3. 操作方式

- **触屏**：完整的触控方案——左下虚拟摇杆、右下技能轮盘、底栏（生命/法力/经验/腰带/回城）、
  面板态自动切换布局、技能钮图标与充能角标。手柄（蓝牙/USB）同时可用。
- **面板与菜单**：与 iOS 版同构，语言跟随游戏设置（简中/英文）。
- 触摸布局与几何由生成脚本产出，校验脚本见第 6 节。

---

## 4. 画质与性能

### 4.1 分辨率档（仅 Android）

自定义页（设置 → Custom）里有「分辨率」一档，三选一：

| 档位 | 出图预算 | 世界（3D）预算 | 说明 |
|---|---|---|---|
| 省电 | 1.80 MP（1966×914）| 1.00 MP | 最省，画面最软 |
| **标准（缺省）** | 2.40 MP（2270×1056）| 1.25 MP（3D 比例 0.722）| 本机实测约 30 fps |
| 高清 | 原生出图 | 0.55 × 出图 | UI 最锐利，世界分辨率低于标准 |

出图宽度由高度按屏幕比例派生，**任何档位都不改变画面比例**（不会拉伸）。改档需要重启游戏。

除自定义页外，也可以用 `diagnostics/quality.txt`（内容一行：`low` / `standard` / `high`）或
`diagnostics/flag-*`（见 `dm/android/app_jni.c` 的完整列表）覆盖；优先级：
`DM_GAME_RES` / `DM_3D_SCALE` 环境变量 > `DM_QUALITY_TIER` > 自定义页 > 缺省。

### 4.2 实测性能（22081212C，标准档，营地场景）

| 指标 | 数值 |
|---|---|
| 帧率 | **29.9–30.1 fps**（凉态，X2 1.5–2.1 GHz；SurfaceFlinger 实际呈现时间戳） |
| 帧间隔 | P50 33.06 ms、P95 41.3 ms、掉帧 0 |
| CPU（游戏线程） | 约 33 ms/帧，其中 replay 16–19 ms、指令录制约 3 ms |
| GPU | 约 28 ms/帧（利用率 80–90%） |
| 单核占用 | 一条渲染线程 100% 满载（瓶颈所在），全进程约 2 核 |

**热态影响**：长时间连续游玩后 SoC 会降频（实测 X2 从 2.13 GHz 降到 1.29 GHz），同一场景掉到
约 **27 fps**；这是设备热设计的结果，不是软件回退。想要更高帧率需要更好的散热条件。

**世界比例与帧率的关系**（同一营地逐档实测）：标准档出图固定 2270×1056 时，世界比例
0.722 → 0.90 帧率持平（**多 81% 世界像素不要帧率**），只有到 1.0 才掉约 3%。想要更清晰的画面
可以设 `DM_3D_SCALE=0.88`（重启生效）。

### 4.3 与 iOS 版的差距

iOS 版能稳定 60 fps，Android 目前 30 fps。差距不在 GPU 侧（GPU 尚有余量），而在**单条提交线程的
串行工作量**：每帧约 1,977 笔绘制、3.6 万条指令，绑定解析是主要成本。已做与已排除的优化项
逐条记录在 [docs/android-resolution-plan.zh-CN.md](docs/android-resolution-plan.zh-CN.md)。

---

## 5. 诊断与调优开关

`diagnostics/` 目录下有若干运行时可用的文件（**写完记得 `chmod 666`**，否则应用读不到）：

| 文件 | 作用 |
|---|---|
| `env.txt` | 启动时导入的环境变量（每行 `KEY=VALUE`） |
| `quality.txt` | 画质档：`low` / `standard` / `high` |
| `flag-*` | 各种预设开关（分块大小、比例、呈现频率等） |
| `loadstore.txt` / `planstate.txt` / `prereset.txt` / `nooccl.txt` / `chunkd.txt` … | **运行中 A/B** 开关：内容 0/1（或档位值），每个统计窗口读一次，不必重启 |

常用环境变量：

| 变量 | 作用 |
|---|---|
| `DM_GAME_RES=1920x864` | 直接指定出图分辨率 |
| `DM_3D_SCALE=0.88` | 直接指定世界比例（允许 0.70–1.0） |
| `DM_PROFILE_DETAIL=1` | 打开逐阶段计时（`RENDER_*_PROFILE` 日志） |
| `DM_VK_DEBUG=1` | 打开驱动侧报错回调（`VK_EXT_debug_utils`，不需要 validation layer） |
| `DM_SUB_RING=0` | 关掉主提交上下文双缓冲（默认开，回退用） |
| `DM_UI_NO_QUERY=0` / `DM_SHADOW_NO_QUERY=0` | 恢复 UI / 阴影通道的遮挡查询（默认跳过） |
| `DM_AFFINITY_ALL=1` | 放宽渲染线程亲和性到全部核心（默认关） |

**测帧率请用外部尺子**，不要只看应用内部计数：

```bash
python3 tools/pf/sf_fps.py <设备序列号> 20
# 输出：SurfaceFlinger 实际呈现 fps、帧间隔 P50/P95、掉帧率，并附带场景探针行
```

---

## 6. 自检脚本

```bash
python3 dm/i18n/scan_literals.py                  # 文案字面量检查
python3 dm/ios/tests/test_touch_layout_options.py # 触摸布局契约（Android 档位 19 行 / iOS 18 行）
bash dm/android/tools/sync_surface_audit.sh       # 表面同步审计（冲突面保持 43）
```

---

## 7. 已知限制

- **性能**：约 30 fps，尚未达到 40 fps 目标（原因与已排除的手段见第四节与那份方案文档）。
- **验证层**：设备上未安装 Vulkan validation layer；已内置驱动侧报错回调作为替代。
- **地址消毒版**：`cmake -DDM_ASAN=ON` + `DM_PACK_ASAN=1 dm/android/tools/build_apk.sh` 可构建
  ASan 变体（运行时打进 APK），用于排查内存问题。
- **不发版任何游戏内容**：APK 与仓库都不含游戏数据。

---

## 8. 许可

MIT。游戏本体、资料片、素材与商标归各自权利人所有；本项目只提供在你自己的设备上运行你自己
购买的游戏的兼容层，不提供、也不分发任何游戏文件。
