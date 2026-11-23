# 鸿蒙移植

English: [README.md](README.md)

鸿蒙 NEXT（API 26）应用：在共用宿主和 `dm/gfx/vulkan` 的 Vulkan 引擎（与安卓移植同一套）上运行游戏。
移植在华为 Pura X（VDE-AL00，麒麟 9020）和 Mate 90 Pro Max（CMM-AL10，马良 955）上开发，系统均为 HarmonyOS 7.0.0.109；本版（共用引擎）只在 Mate 90 Pro Max 上跑过。
这是给拥有游戏的人用的开发版：一切从游戏文件生成的东西都只留在玩家自己的电脑和手机上。

## 源码布局

- `app/`：DevEco（hvigor）工程。ArkTS 外壳（`entry/src/main/ets`）：XComponent 画面、触摸转发、生命周期、按文件夹导入游戏数据、把运行包解压到 `files/dm`。
- `native/`：N-API 库 `libdm.so`。
  - `napi_ohos.c`：启动、资源位置、鸿蒙缺省值（见下）、生命周期。
  - `front_ohos.c`、`touch_ohos.c`：前端与触摸，与安卓一样接到共用的触屏状态机 `dm/ios/Sources/dm_touch_controls.c`。
  - `gameperf_ohos.c`：Game Service Kit 游戏性能（温控档、GPU 负载与主频）写进性能日志。
  - `android_shim/`：把复用的安卓文件调用的少数安卓 NDK 接口映射到语义完全对应的鸿蒙接口（`ANativeWindow` → `OHNativeWindow`、
    Vulkan 表面 → `VK_OHOS_surface`、日志 → hilog），`dm/android/vulkan_present.cpp`、`dm/android/overlay_validate.c` 与 `dm/android/compat` 不改即可编译。
- `probe/tsd_probe.c`：实测 musl 线程槽布局（`tpidr_el0 − 272`）的设备探针，改写器 `DM_TARGET=ohos` 用的就是它的结果。
- `tools/gen_strings.py`：从 `dm/i18n/ui_text.tsv` 生成 ArkTS 字符串表。

为本移植改动的共用代码都用 `__OHOS__` 包住（或是别处不用的新文件）；安卓和苹果构建的行为不变。每处改动在 PR 里列出。

## 构建

需要：DevEco Studio 6 与 HarmonyOS 26 SDK、带 dm 工具依赖（Pillow）的 Python 3、你自己的游戏文件。

1. 在仓库之外用自己的游戏生成每个玩家各自的文件，与安卓相同：宿主表与技能索引（`dm/ios/tools/native_art/generate.py`、`gen_vault_table.py`、
   `gen_hud_exp.py`、`dm/tools/gen_bright_levels.py`）、覆盖层（`dm/ios/tools/gen_romfs_overlay.py`，再跑 `dm/android/tools/stage_touch_assets.py`）、
   改写模块（`DM_TARGET=ohos DM_RULESET=touch-run-v4 dm/tools/patch_nso.py`，`main`、`sdk`、`subsdk0` 各一次）、着色器包（见 `dm/android/README.md` 的「Shader bundles」）。
2. 把 `app/entry/src/main/cpp/local.cmake.example` 复制成 `local.cmake`，指向生成的头文件和你的 Python；把 `app/build-profile.template.json5`
   复制成 `app/build-profile.json5`，在 DevEco 里填自己的签名配置。这两个文件都不入库。
3. 调试版可以内置运行包：`app/scripts/stage_debug_pack.sh <含 out shaders overlay ui-art 的目录>` 把压缩包放进 `entry/src/debugpack`（git 忽略），
   应用首次启动时解压。没有内置包时，应用让玩家选一个装着 `dm/` 和 `data/` 的文件夹导入。发布包不得带运行包：`app/scripts/check_release_clean.py <hap>`。
4. `hvigorw --mode module -p module=entry@default -p product=default assembleHap`，再 `hdc install -r entry/build/default/outputs/default/entry-default-signed.hap`。

### v2.3.0 本地壳

- 使用本源码树的 `dm/tools/d2rm_android.py --target ohos` 生成 `3.3.94012-r45` 运行包。手机里已有游戏数据时，只需把新版 `dm` 放到手机上的文件夹内，在启动页点「导入游戏资源」并选择该文件夹。
- 启动页可选择是否显示帧率，并可在游戏启动前导入或导出角色、共享仓库和游戏设置存档。将安卓导出的备份文件夹传到手机即可导入。鸿蒙导出的 ZIP 要先解压。同名角色会跳过，来自其他安装的同类共享仓库可在打开储藏箱时合并。
- 游戏内「选项 → 自定义」包含三档 3D 清晰度、暗处加亮、省电帧率和物品注释等设置。物品注释和旧译名需重启应用。越肩与第一人称镜头可以在右半屏空白处拖动。使用实体手柄会切换键位布局，再点击屏幕可回到触屏布局。
- 打包公开壳时先把生成表复制到独立目录，对该副本运行 `dm/ios/tools/shell_stubs.py stubs-gen`，将构建的 `DM_GEN_DIR` 指向该目录。同时加入 `gen_bright_levels.py` 生成的关卡亮度参数。发布包使用 `release` 目标，运行组件由玩家单独导入。

## 鸿蒙缺省值与实测

`napi_ohos.c` 只在环境里没有时设下面这些（`files/dm/env` 与标记文件仍可覆盖）：

| 设置 | 原因 |
|---|---|
| `DM_NVN_SYNC=0` | 翻译走单独线程（同 iOS）。标记 `dm/nvn-async` 写 `0` 退回 |
| `DM_PACE_ADAPT=1` | 自适应出图档（`dm/gfx/nvn/nvn_adaptive_pace.h`）：游戏帧按设备撑得住的最高整除档均匀放行，错过上屏节拍就降档，稳定后带退避地试升 |
| 完成通知、`gfx-legacy-waitall=0` | 仅鸿蒙上的引擎缺省（`xbg_completion_wait.h`）：栅栏等待由目标栅栏退休唤醒，不再持引擎锁 |
| 期望上屏时间戳、4 张交换链图 | `dm/android/vulkan_present.cpp` 的 `__OHOS__` 分支 |
| `DM_GFX_NATIVE_BC=0` | 马良不支持 BC 贴图（`textureCompressionBC = 0`）：CPU 软解，`xbg_texture.c` 有整块快路径 |
| `DM_GFX_READBACK_CACHED=1` | GPU 写、CPU 读的缓冲用带缓存内存（马良最佳实践） |
| `DM_GFX_DEFER_EVENTS=2`、`DM_GFX_READ_PREFLIGHT=0`、`DM_GFX_NOLOAD=1`、`DM_GFX_PIPE_MEMO=1`、`DM_GFX_QUAD_DERIVATIVES=1`、`DM_GFX_WARP32=1`、`DM_GFX_NAN_MINMAX=1` | 同安卓 |
| `DM_3D_SCALE_GPU=1`、`DM_3D_SCALE_DYN=1` | 动态 3D 比例 |

Mate 90 Pro Max，在罗格营地按住摇杆 12 秒（hitrace）：59.7 fps，上屏间隔 p50 16.6 / p99 18.1 ms，抖动 0.8 ms。
同一构建进程内 A/B 完成通知：游戏帧抖动 3.7 对 8.2 ms，超过 16.7 ms 的帧 40 对 179。

尚未做：`gfx-present-native`（按游戏出图尺寸上屏）在鸿蒙上先关，等真机核对缓冲尺寸与合成缩放；改写器的无红区排法暂只用于安卓
（`patch_nso.py` 的 `NOREDZONE`），鸿蒙要重出模块并真机测试后再开。
