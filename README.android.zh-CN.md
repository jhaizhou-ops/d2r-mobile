# D2R Mobile · Android

**把你自己购买的 Nintendo Switch 版《暗黑破坏神 II：狱火重生》（含「术士君临」资料片）原生运行在 Android 手机上。** 游戏自身的 aarch64 程序直接在设备上执行，没有 CPU 模拟；平台服务与图形 API 由本仓库的宿主层接管，图形后端用 Vulkan 重新实现，着色器离线翻译。

English: [README.android.md](README.android.md) · 主仓库说明: [README.zh-CN.md](README.zh-CN.md) · 游戏文件准备: [docs/GAME_FILES.zh-CN.md](docs/GAME_FILES.zh-CN.md)

> **状态：开发中。** Android 端已经能完整游玩（进游戏、战斗、存档、设置、触屏操控全部可用），但性能尚未达到 iOS 端的 60 fps 水准——当前实测 **30–40 fps**（视分辨率档，见下文「实测性能」）。发版前的目标与已知限制都写在本文档里。

## 截图

*角色选择（触屏点选，右侧是存档位列表）*

![角色选择](docs/images/android-character-select.jpg)

*游戏内：左下虚拟摇杆、右下技能轮盘、底栏与快捷栏（营地，标准档 2322×1080）*

![游戏内](docs/images/android-gameplay.jpg)

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

### 1.2 游戏文件k

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
| JDK | 17 及以上（打包按 `-source 17` 编译，JDK 21 实测可用） |
| Python 3 | 生成资源与运行校验脚本 |

### 2.2 步骤

```bash
# 1) 打改写模块 + 生成宿主的查表（都要上面那份游戏文件，第 3 步 ninja 编译时就要用）
dm/android/tools/build_modules.sh
#    产物：build/modules-touch/{main,sdk,subsdk0}.patched.bin（打包用），以及宿主的三张查表：
#    dm_hud_exp.h / dm_vault_tab.h（落 build/generated/）、dm_bright_levels.inc（落 dm/host/，
#    3D 画面加亮的洞穴 / 室内判据）。缺游戏文件会明确报 🔴，不会静默漏编。

# 2) 生成资源与 ROMFS 覆盖层（需要上面那份游戏文件）
dm/android/tools/gen_overlay.sh
#    若资源已生成过、只想重打包，可用 DM_ASSETS_REUSE=1 跳过重复解包
#    DM_ASSETS_REUSE=1 dm/android/tools/gen_overlay.sh

# 3) 配置并编译原生库（构建目录不进仓库，新机器第一次要配置）
SDK=${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}
NDK=$SDK/ndk/27.2.12479018
cmake -S dm/android -B dm/android/build -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release
cd dm/android/build && ninja
#    配置时会检查第 2 步产出的 gen-assets/native_art/dm_touch_native_art.c；资源若在别处生成，
#    用 DM_GEN_ASSETS=/path/to/gen-assets 指过去（或把 dm/android/build/gen-assets 整块拷过来）。

# 4) 打包 APK（产物：dm/android/build/apk/d2r-mobile-android-<版本>+<构建号>.apk，例 d2r-mobile-android-0.2-native-android-port+20261020.apk；
#    版本号取自 AndroidManifest.xml，构建号缺省当天日期，可用 DM_APK_BUILD 覆盖）
#    启动图标复用 iOS 那份美术（dm/ios/Assets.xcassets/AppIcon.appiconset/AppIcon1024.png），
#    打包时由 tools/gen_icons.py 缩成 5 档密度，仓库里不另存一套
dm/android/tools/build_apk.sh

# 5) 部署到设备并启动
ADB=adb ANDROID_SERIAL=<你的设备> dm/android/tools/deploy_apk.sh
```

`deploy_apk.sh` 会做四件事：把游戏数据（RomFS 与着色器缓存）推到应用数据目录、把附件覆盖率表种子
`dm/gfx/nvn/pass-coverage.tsv` 推到 shader 目录（可选，`chmod 664` 保证游戏读得到）、安装 APK、启动。数据目录约定：

```
/sdcard/Android/data/org.d2r.binaryprobe/files/d2r-mobile/
├── romfs-view/          游戏资源
├── shaders/             离线翻译好的着色器
├── diagnostics/         日志与诊断开关（见下）
└── ...
```

首次启动会做一次较慢的初始化（解包/索引资源），之后进游戏约 1 秒。

### 2.3 产物命名与换机器打包

产物名（`dm/android/build/apk/` 下）：

```
d2r-mobile-android-<versionName>+<构建号>.apk
例：d2r-mobile-android-0.2-native-android-port+20261020.apk
```

- `<versionName>` 从 `dm/android/AndroidManifest.xml` 读（当前 `0.2-native-android-port`），不另立一份来源；
  空格、`/` 这类不适合进文件名的字符会换成 `-`
- `<构建号>` 缺省是**当天日期** `YYYYMMDD`（所以同一天连打两次名字一样，覆盖前一份）；
  要区分就 `DM_APK_BUILD=a1 dm/android/tools/build_apk.sh`
- 中间产物用同一个基名（`….aligned.apk`），不再出现 `dodola.*`；`deploy_apk.sh` 不带参数时取目录里最新的那份

换一台机器只要满足这几件事，脚本会自己找路径（都是环境变量优先，然后按平台常见位置探测，找不到就报一句人话停下）：

| 需要 | 怎么给 |
|---|---|
| Android SDK（build-tools 里的 aapt2/d8/zipalign/apksigner + `platforms/android-*/android.jar`）| `ANDROID_SDK_ROOT` 或 `ANDROID_HOME`；否则探 `~/Android/Sdk`、`~/Library/Android/sdk` 等 |
| JDK 17+ | `JAVA_HOME`；否则 macOS 的 `/usr/libexec/java_home`，再否则从 PATH 上的 `javac` 反推 |
| python3 + Pillow | 生成启动图标（方/圆各 5 档）用 |
| 已编好的原生库 | 先 `cd dm/android/build && ninja`（NDK 27 + cmake + ninja）|
| 模块与生成资源 | `build_modules.sh` / `gen_overlay.sh` 的产物；缺了会明确报错或警告 |
| 签名密钥 | 缺省用 `~/.android/debug.keystore`，**没有会自动建一个**（标准调试规格）；也可 `DM_KEYSTORE=/path/to.jks` |

macOS 自带的 bash 3.2 与 BSD `sort`（没有 `-V`）都已兜底，不需要额外装 GNU 工具。


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

三档按**纵向像素**定义（800p / 1080p / 原生），宽度由高度乘 SurfaceView 比例派生，
**任何档位都不改变画面比例**（不会拉伸）。世界（3D）预算是**绝对 MP**，由它反算出 3D 比例。
下表的绝对数值来自本机（SurfaceView 2624×1220），别的屏幕按同一公式重算。

| 档位 | 出图（2624×1220 屏实测）| 世界（3D）预算 → 实际 | 营地实测帧间隔 |
|---|---|---|---|
| 省电 | 800p：1720×800（1.38 MP）| 0.85 MP → 比例 0.786，1352×629 | P50 24.8 ms、P95 40.5 ms（≈40 fps）|
| **标准（缺省）** | 1080p：2322×1080（2.51 MP）| 1.10 MP，但需比例 0.662 < 0.70 下限 → **按 0.70 截断**，实得 1.23 MP（1625×756）| P50 24.8–33.0 ms（30–40 fps）|
| 高清 | 面板原生：2624×1220（3.20 MP）| 1.75 MP → 比例 0.739，1940×902 | P50 28.9–33.0 ms（30–35 fps）|

> 标准档每次启动都会打一行黄字：`【黄】世界预算需 3D 比例 0.662 < 0.70，按 0.70 截断`。
> 这是预期的——下限 0.70 是不让世界分辨率掉得太糊；要让 1.10 MP 真正生效，得把下限放到 0.66 以下。
>
> 热态自动降档在 Android 上**默认关**（`DM_3D_SCALE_DYN=1` 才开）：档位表 0.85 / 0.80 / 0.75 / 0.70 / 0.65，
> 下限缺省 0.70；关着时 3D 比例就是上表那个固定值。详见
> [docs/android-resolution-plan.zh-CN.md](docs/android-resolution-plan.zh-CN.md) 附十九。

改档需要重启游戏（出图分辨率在启动时定死）。

除自定义页外，也可以用 `diagnostics/quality.txt`（内容一行：`low` / `standard` / `high`）或
`diagnostics/flag-*`（见 `dm/android/app_jni.c` 的完整列表）覆盖；优先级：
`DM_GAME_RES` / `DM_3D_SCALE` 环境变量 > `DM_QUALITY_TIER` > 自定义页 > 缺省。

### 4.2 实测性能（22081212C，营地场景）

外部尺子 `tools/pf/sf_fps.py`（SurfaceFlinger 实际呈现时间戳），2026-10-20，同一营地，各测 3 个 15 秒窗口：

| 档位（出图 / 世界）| 帧间隔 P50 | P95 | 折合 |
|---|---|---|---|
| 省电 1720×800 / 1352×629 | 24.8 ms | 40.5 ms | ≈40 fps |
| 标准 2322×1080 / 1625×756 | 24.8–33.0 ms | 33.1 ms | 30–40 fps |
| 高清 2624×1220 / 1940×902 | 28.9–33.0 ms | 33.1 ms | 30–35 fps |
| 标准 + `DM_3D_SCALE=0.90` | 33.0 ms | 33.1 ms | ≈30 fps |

**读这张表要先知道一件事**：本机面板是 120 Hz，而呈现节拍落在 3 个 vsync（≈25 ms）或 4 个 vsync
（≈33 ms）两档上（`dm/android/app_jni.c` 里 "25 ms present cadence" 的注释说的是同一件事）。
**同一档位在几分钟内就会在两档之间切换**，所以单次 15 秒窗口不能拿来比较档位——三档都落在
30–40 fps 区间，档间差异淹没在测量噪声里。能确认的是：

- **输出像素影响很小**：省电档出图只有标准档的 55%，帧间隔却测不出稳定差别——瓶颈在**单条提交
  线程的串行工作量**（每帧约 1,977 笔绘制、3.6 万条指令，绑定解析是主要成本；一条渲染线程 100% 满载）。
- **世界比例**：把标准档的 3D 比例提到 0.90（世界 1.23 → 2.03 MP），测到的仍是 33 ms 那一档，没有
  出现更慢的独立档位；但也没有复现旧文档说的"0.722 → 0.90 帧率持平、多 81% 像素免费"。想要更清晰的
  画面可以试 `DM_3D_SCALE=0.85`，以自己机器上的长窗口对照为准（重启生效）。
- **热态影响**：长时间连续游玩后 SoC 降频（实测 X2 2.13 → 1.29 GHz），同一场景再掉 3–5 fps
  （2026-10-19 标准档口径：~30 → ~27 fps）；这是设备热设计，不是软件回退。

### 4.3 与 iOS 版的差距

iOS 版能稳定 60 fps，Android 目前视档位在 **30–40 fps**（高清 30–35、标准/省电约 40）。
标准档的瓶颈不在 GPU，而在**单条提交线程的串行工作量**（每帧约 1,977 笔绘制、3.6 万条指令）；
世界像素拉高（3D 比例 0.90 以上 / 高清档）时更容易落到 30 fps 那一档。
已做与已排除的优化项逐条记录在 [docs/android-resolution-plan.zh-CN.md](docs/android-resolution-plan.zh-CN.md)。

---

## 5. 诊断与调优开关

`diagnostics/` 目录下有若干运行时可用的文件（**写完记得 `chmod 666`**，否则应用读不到）：

| 文件 | 作用 |
|---|---|
| `env.txt` | 启动时导入的环境变量（每行 `KEY=VALUE`） |
| `quality.txt` | 画质档：`low` / `standard` / `high` |
| `flag-*` | 各种预设开关（分块大小、比例、呈现频率等） |
| `loadstore.txt` / `planstate.txt` / `prereset.txt` / `nooccl.txt` / `chunkd.txt` … | **运行中 A/B** 开关：内容 0/1（或档位值），每个统计窗口读一次，不必重启 |
| `att-load-table.txt` | 逐通道覆盖率表的**运行中开关**：`1` 开 / `0` 关，每窗口读一次；冲突隔离后拒绝再开 |
| `enable-att-load-table` / `disable-att-load-table` | 启动时开/关逐通道 `loadOp` 覆盖率表（默认开；回退与实测见 `docs/android-resolution-plan.zh-CN.md` 附二十） |
| `pass-sentinel` / `pass-sentinel-selftest` | 开哨兵探针给覆盖率表填数据（很慢，7–18fps；`selftest` 让探针自证会报红） |

常用环境变量：

| 变量 | 作用 |
|---|---|
| `DM_GAME_RES=1920x864` | 直接指定出图分辨率 |
| `DM_3D_SCALE=0.88` | 直接指定世界比例（允许 0.70–1.0） |
| `DM_PROFILE_DETAIL=1` | 打开逐阶段计时（`RENDER_*_PROFILE` 日志） |
| `DM_VK_DEBUG=1` | 打开驱动侧报错回调（`VK_EXT_debug_utils`，不需要 validation layer） |
| `DM_ATT_LOAD_TABLE=1` | 开逐通道覆盖率表（种子在 `dm/gfx/nvn/pass-coverage.tsv`，部署时推到 shader 目录） |
| `DM_PASS_SENTINEL=1` + `DM_PASS_SENTINEL_WINDOW=1` | 填覆盖率表：逐通道序号扫全，每帧回读上限 4 MB |
| `DM_COV_MIN_SAMPLES=8` | 一条 green 至少要被探针独立命中几次才敢用（缺省 8，调低更激进） |
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

- **性能**：标准 / 省电档约 40 fps、高清档 30–35 fps（营地冷态），尚未到 iOS 的 60 fps；瓶颈在单条提交线程（见第四节与那份方案文档）。
- **验证层**：设备上未安装 Vulkan validation layer；已内置驱动侧报错回调作为替代。
- **地址消毒版**：`cmake -DDM_ASAN=ON` + `DM_PACK_ASAN=1 dm/android/tools/build_apk.sh` 可构建
  ASan 变体（运行时打进 APK），用于排查内存问题。
- **不发版任何游戏内容**：APK 与仓库都不含游戏数据。

---

## 8. 致谢

**没有 [@jhaizhou-ops](https://github.com/jhaizhou-ops) 就没有这个 Android 项目。** 他做的是整个项目的
源头：把 Switch 版的 aarch64 程序直接跑在手机宿主上这条路，NVN 接口接管、命令录制/回放分层、着色器
离线翻译链、触屏与前端那一整套，都出自他：[jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)。
Android 端是这条技术路线的延伸，不是另起一摊。他本人这条线主要做 iOS / Mac（Metal）。

在这之上是另外两层：

- **[@dodola](https://github.com/dodola)** —— Android 的图形化渲染：在 Android 上跑通原生游戏宿主，
  并做出 **Vulkan 回放后端**（`dm/gfx/nvn/nvn_vk.c` 那整套：把 NVN 的录制流重放到 Vulkan、BC 流式
  一致性、比较深度别名、附件反馈、左上原点映射、设备本地管线缓存、16 KB 对齐）。这是 Android 能出
  第一帧图形的地基。UI 侧他搭的是最初始的骨架（`GameActivity` / `TouchControlsView` 的原型），
  可用但只是原生控件级的"能显示、能点"。
- **本分支（Android 移植、UI 与性能）** —— 在这两份工作之上，Android 端的打磨基本都在这里：
  - **UI**：底栏与各面板（储藏箱、百宝囊、选角、技能图集、小地图终点等）、新触摸外观与操作、
    HUD、自定义设置页、分辨率档、国际化文案。`touch_controls_android.c` 从 125 行长到 442 行、
    `TouchControlsView.java` 从 78 行长到 465 行，绝大多数提交都在本分支。
  - **性能**：管线预热与清单自学习、附件带宽削减（营地 917 → 446 MB/帧）、翻译队列与出图节拍
    （营地 30 → 36.6 fps）、绑定计划与命令状态缓存、通道/查询/帧缓冲三线优化等。
  - 实测数据与取舍记录见 [docs/ANDROID_PERF_NOTES.zh-CN.md](docs/ANDROID_PERF_NOTES.zh-CN.md)、
    [docs/android-perf-lessons.zh-CN.md](docs/android-perf-lessons.zh-CN.md)，UI 与两端对齐见
    [docs/ANDROID_UI_PARITY.zh-CN.md](docs/ANDROID_UI_PARITY.zh-CN.md)。

也感谢上游 [deko3d](https://github.com/devkitPro/deko3d)（`nvn_tex.c` 的块线性布局与对齐由它移植）、
[Ryujinx](https://github.com/Ryujinx/Ryujinx)（着色器翻译库）与 [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)
（SPIR-V → Metal Shading Language）。

---

## 9. 许可

MIT。游戏本体、资料片、素材与商标归各自权利人所有；本项目只提供在你自己的设备上运行你自己
购买的游戏的兼容层，不提供、也不分发任何游戏文件。
