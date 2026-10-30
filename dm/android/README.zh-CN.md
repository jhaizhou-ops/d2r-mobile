# 安卓移植

English: [README.md](README.md)

这是 ARM64 安卓版。APK 接入了公用宿主、Vulkan 回放引擎、AAudio、与 iOS 同一套画法的透明触摸覆盖层及系统输入框，已在真机上玩过（营地、战斗、各面板、NPC 对话），实测数据在 `docs/android-notes/`。玩家装发布页的 APK、导入自己的游戏内容即可，见下面「玩家怎么装」。

## 源码结构

- `app/`：Java Activity、全屏 SurfaceView、触摸覆盖层及 Gradle 应用工程。
- `jni_android.c`、`front_android.c`：启动、资源路径、生命周期及平台前端接口。
- `touch_android*`：接入公用 `dm/ios/Sources/dm_touch_controls.c` 状态机；`text_input_jni.c` 将客体同步输入请求接到安卓系统对话框。
- `vulkan_present.*`：安卓窗口引用、交换链及呈现。
- `../gfx/vulkan/`：NVN 回放适配器和 Vulkan 引擎。
- `compat/`：宿主 Apple 平台接口的安卓实现。
- `tools/`：源码清单解析、ELF 汇编转换及本地化资源生成。

## 玩家怎么装

发布页的 `D2RMobile-android.apk` 不含任何游戏内容（由 `dm/android/tools/shell_apk.sh` 出：三张从游戏文件生成的表换成替身，运行时读导入的 `derived.tsv`，做法同 iOS 壳）。游戏内容用你自己的游戏文件在电脑上做：

```sh
python3 dm/tools/d2rm_android.py <Switch 版 3.3.94012 的 Update 目录> <输出目录>
```

它改写游戏程序（安卓目标）、把全部着色器翻成 Vulkan 包、生成触屏覆盖层与几张数据表，写出 `<输出目录>/android/`。需要的工具见脚本开头（Python 包、.NET SDK、Vulkan SDK 里的 `spirv-opt` / `spirv-val` / `glslc`），缺什么它会逐项报。

把这个 `android` 文件夹整个拷到手机的 `Download/S2X/D2R Mobile/` 下，游戏资源（`RomFS/data` 整个目录）拷成同一处的 `data`。打开 App：发现新的导入包就拷进内部目录 `files/dm/`（显示进度，第一次一两分钟），然后启动。导入包要与 App 同一版本（`pack_info.txt` 里的产物代号与 App 编入的相同），不同时 App 会说明。导入完，手机上的 `android` 文件夹可以删掉；导入包里有游戏内容，只给自己用。

## 本地构建

当前配置使用 JDK 17 或更新版本、Gradle 9.8.1、Android SDK 36、NDK 29.0.14206865、CMake 3.31.6 和 Python 3。ARM64 设备要求 Android 12/API 31 或更新系统及具备所需设备能力的 Vulkan 1.1。这些是构建和清单要求，不代表符合条件的机型都已验证。首台目标是 vivo X200 Ultra（Android 16/API 36、Adreno 830）。

按根目录准备说明提供自己的游戏文件。参考 `dm/host/Makefile` 的规则，在仓外生成 `dm_vault_tab.h`、`dm_hud_exp.h`、`dm_bright_levels.inc`。`ANDROID_HOME` 与 `ANDROID_SDK_ROOT` 必须指向同一个 SDK，`JAVA_HOME` 指向 JDK。在仓库根目录执行：

```sh
gradle -p dm/android :app:assembleDebug :app:lintDebug \
  -PdmGenDir=/absolute/path/to/private/generated/headers \
  -PdmPython=/absolute/path/to/python
```

APK 位于 `dm/android/app/build/outputs/apk/debug/app-debug.apk`，原生链接启用 `--no-undefined`。`-PdmPrebuiltJni=...` 只供使用已有库打包，不是当前原生源码编译通过的证据。

在 Linux 或 WSL 上，可用 `dm/android/docker/` 在容器里跑同一构建，上述版本均已钉死，不需要在 Windows 侧装 Python、NDK 或 CMake：

```sh
docker build -t dm-android-build:1 dm/android/docker
DM_GEN_DIR=/绝对路径/私有生成表头目录 dm/android/docker/build.sh
```

脚本以当前用户身份运行，调试签名钥匙保存在 `~/.android-dm`；挂载说明见脚本开头。

## 仓外运行材料

APK 和 Git 仓库均不含游戏程序、着色器、游戏美术或存档。这些由导入包提供（见「玩家怎么装」），App 导入到下表的内部目录。

| 位置 | 内容 |
| --- | --- |
| 应用内部 `files/dm/out/` | 使用 `DM_TARGET=android` 生成的 `main`、`sdk` 改写产物，含 patched 模块、fixups 和 gates |
| 应用内部 `files/dm/shaders/` | 与游戏及 Vulkan 后端匹配的仓外 `.xgs` 包 |
| 应用内部 `files/dm/overlay/` | 必需 CASC 触摸覆盖层，含 `overlay.manifest`，由匹配源码及用户资源生成 |
| 应用内部 `files/dm/overlay/touchart/`、`files/dm/overlay/hud_v4_ui/` | 由 `stage_touch_assets.py` 放入的技能图集及其银色剪影、地图图标、圆框、底栏素材，以及技能钮底座（仓库的 `dm/ios/Sources/ControlArt/dm_mobile_socket.png`）。触屏控件与 iOS 同一套画法（`dm/ios/Sources/DmTouchControlArt.h`、`DmTouchOverlay.m`），摇杆用代码画 |
| 共享 `Download/S2X/D2R Mobile/data/` | 用户资源档案及 `.build.config`，需要安卓“所有文件访问”授权；没有这个目录、而早先的 `Download/data/` 在时，App 沿用 `Download/data/` |
| 应用内部 `files/dm/derived.tsv`、`files/dm/pack_info.txt` | 从游戏文件生成的数据表（不含游戏内容的发布包启动时读）；导入包的产物代号与生成时刻 |
| 应用内部 `files/save/` | 存档，升级时必须保留 |
| 应用内部 `files/temp/`、`files/dm/boot.log` | 临时文件及追加写入的启动日志 |

内部没有主程序时，启动器回退到应用外部私有目录下的 `dm/`。启动前会检查必要路径，但不能据此证明着色器身份及资源兼容性。调试包可使用 `run-as` 导入内部文件，升级使用相同签名覆盖安装，不以卸载或清数据作为升级步骤。真机验证前备份已有存档，不向 PR 上传 APK 或上述私人文件。

覆盖层生成器 `dm/ios/tools/gen_romfs_overlay.py` 支持用 `DM_DESWIZZLE_LIB` 指定本地编译的纹理处理库。生成的精简 CASC 只含变更并引用原资源，不得替换原始资源目录。

### 着色器包

`files/dm/shaders/` 里，游戏用到的每个着色器各有一个 `<键>.xgs`；键是该着色器控制段 SHA-256 的前 16 位十六进制。包是 XGS1 文件：文件头、各绑定、顶点输入位置、c1 字节，然后是 SPIR-V（`dm/gfx/vulkan/engine/xbg_game.h` 的 `xbg_shader_header`）。目前测试用的一套共 6,931 个包：

- **6,927 个是游戏自己着色器的翻译。** 它们出自游戏文件，所以不进 Git、PR 和 APK。`dm/gfx/tools/dmsh_vk.py` 从头做出它们：用 `dm/gfx/tools/dmsh_xlate` 翻成面向 Vulkan 的 SPIR-V，绑定号改成单描述符集的编号（原号 × 6 + 阶段号）、补上「未定格式」存储图像读写的能力、裁掉多余能力，导出 `<键>.vk.json`，再用 `pack_game_shaders.py` 的打包函数写成包。做出的 6,927 个包与 biglawine 鸿蒙着色器流程做出的测试包逐字节相同（`dmsh_vk.py --compare <参考目录> <输出目录>`）。
- **4 个是 main 程序本体内嵌的查询核**（`q_val0`、`q_val8`、`q_dlt0`、`q_dlt8`；Mac 与 iOS 用 `dm/gfx/nvn/nvn_builtin.h` 里的 MSL 版）。它们是手写的，能从源码做：`python3 dm/gfx/tools/pack_builtin_queries.py --output <新目录>` 用 `glslc`（NDK 的 `shader-tools/` 下就有）编译 `dm/gfx/vulkan/kernels/builtin_queries.comp`，写出这 4 个文件。

一套包还可以附带 `vk-pipeline-list.seed`。首次启动、还没有 `files/dm/vk-pipeline-list.bin` 时，引擎在启动期间编好种子里列出的管线，种子覆盖的地方第一次去就不会因为现编管线而卡。`dm/android/tools/merge_warm_lists.py` 把几台设备的 `files/dm/vk-pipeline-list.bin` 合成一个种子。种子只能和录它时的那套包一起用，所以随包发放：`dm/tools/d2rm_android.py --warm-seed <文件>` 把它放进导入包（在耗时的步骤之前先核种子），并记在 `pack_info.txt` 的 `WARM_SEED=` 一行。种子放在仓库外。种子只对首次启动有用：手机上已经有 `files/dm/vk-pipeline-list.bin` 的，重新导入后仍用自己的清单。工具的检查随 `make -C dm/host/tests run` 跑（也可以 `python3 dm/tools/tests/test_warm_seed.py`）。实测见 `docs/android-notes/2026-10-22-first-play-stutter.zh-CN.md`。

## 验证与剩余工作

执行根目录贡献检查及 `:app:lintDebug`。纯 Java 输入测试不需要手机：

```sh
javac -encoding UTF-8 -d /tmp/dm-input-tests \
  dm/android/app/src/main/java/com/xiaoboluo/d2r/TextInputRules.java \
  dm/android/tests/TextInputRulesTest.java
java -cp /tmp/dm-input-tests com.xiaoboluo.d2r.TextInputRulesTest
```

真机验收须分别记录安装、真实 GPU 出图/帧率、建角、进局、移动停止、NPC 交互、背包/技能、声音、后台恢复及保存重进。无窗口宿主帧率不等于渲染帧率。本后端尚未接入 Metal GPU 计时及实际扫描显示时间，缺失读数不能当作零耗时。

Vulkan 当前每次全画，并使用同步宿主翻译（`DM_NVN_SYNC=1`）；上游新增异步路径需独立审计对象生命周期。呈现层仍逐帧等待队列。iOS 技能图、空技能槽菜单、专用数量/属性加点浮层及商店拖动跟随图尚未完整实现，通用控件和系统输入框不代表完整 iOS 功能对齐。提交上游集成前，还须在 Apple 构建机上验证 macOS/iOS 回归。

覆盖层生成器将源码指纹和逐文件哈希写入 `overlay.manifest`，APK 编入同一生成器输入指纹。缺失、不完整或不匹配时，启动界面提示并在宿主线程启动前退出启动流程。须部署完整生成目录，不可把新清单贴到旧资源上，也不可在手机启动时补清单；生成器或布局修改后应重新生成。这项校验不负责认证第三方下载来源。

`gen_romfs_overlay.py` 显式请求只生成 overlay 所需资产，跳过 iOS 技能图集及未使用的技能剪影；完整 `gen_assets.ensure()` 仍要求这些产物。技能图集改核尺寸和解码 RGBA 像素，保留原 sprite SHA256/MD5。生成子进程显式使用 UTF-8/LF，并保留已有只读资源适配器；Windows/Linux 的字节一致性仍须以同一游戏输入和工具版本实测。

Vulkan 兼容路径按实际格式能力选择，不根据 Android 版本推断。启动日志记录 `format_feature_flags2` 与 `depth16_as_d24`；仅 D16 缺线性过滤且 X8_D24 满足全部用途时拓宽深度格式。Vulkan 1.1 版本号本身不等于兼容通过；vivo 应保持 `1 / 0`，Redmi K50 Ultra 仍需单独真机验证。
# 当期iOS触控

安卓复用上游触控状态机及只读技能绑定观测桥。用自己的匹配RomFS在仓外生成绑定索引和图集PNG：

```
python dm/ios/tools/native_art/generate.py <RomFS> --output-dir <仓外native-art>
```

只把`dm_touch_native_art.c`复制到现有`DM_GEN_DIR`。CMake要求该仓外索引与原有宿主生成头同时存在。生成新的overlay和HUD后执行：

```
python dm/android/tools/stage_touch_assets.py --native-art <仓外native-art>/TouchArt --hud <assets>/NativeHUD/v4/hud_v4_ui --overlay <候选overlay>
```

导入完整且已生成最终manifest的候选overlay。技能PNG和HUD图像留在外部资源载荷，不加Git或APK assets；绘制层从已校验导入的`files/dm/overlay`读取。设置`DM_NATIVE_ART_DIR=<仓外native-art>`可让素材采图测试核验仓外索引和像素。五个导航键保留，等完整替代背包流程通过真机验证后再调整。

## Debug性能参数

仅CMake的Debug配置读取`<files>/dm/android-debug.env`，Release不包含读取函数或调用。它与游戏通用`env`不同，只接受`DM_3D_SCALE`、`DM_3D_SCALE_DYN`、`DM_3D_SCALE_FLOOR`，每行一个`KEY=VALUE`。支持空行、`#`注释及CRLF；未知/重复键、文件超过4096字节、行超过127字符、非有限值/错误数字、非法开关、下限高于起点均整份拒绝。起点和下限范围为0.50至1.00，DYN只允许0或1。日志仅记录接受/拒绝，不输出数值。

安卓默认比例上限0.85（其他手机平台0.75）、动态比例开启、下限0.60。固定比例A/B对照示例：

```
DM_3D_SCALE=0.75
DM_3D_SCALE_DYN=0
DM_3D_SCALE_FLOOR=0.60
```

同步仍开启；此文件不能覆盖同步、帧重叠或其他设置。解析器回归位于`dm/android/tests/test_debug_env.c`，可独立于游戏构建运行。
