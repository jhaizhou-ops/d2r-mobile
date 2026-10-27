# 依赖一览

构建需要的东西都在这一页：每一项是什么、用来做什么、去哪里拿、怎么装。表里的版本是我们实际构建用过的版本；更新的版本一般也能用，出问题时先换回表里的版本。

English: [DEPENDENCIES.md](DEPENDENCIES.md)

## 你自己要准备的

| 需要什么 | 用来做什么 | 说明 |
|---|---|---|
| 一台 Apple 芯片的 Mac | 构建与运行宿主 | 需要 macOS 与下面的 Xcode |
| 你自己的游戏文件 | 主程序与全部游戏资源 | 本仓库不含任何游戏文件。需要 Switch 版 1.0.34.0（对应 PC 版 3.3.94012）的程序文件（ExeFS）与更新包资源（RomFS），从你自己购买并拥有的游戏导出，见 [GAME_FILES.zh-CN.md](GAME_FILES.zh-CN.md) |
| Apple 开发者账号 | 给 App 签名并装到自己的设备上 | 付费账号可用全部功能。免费账号（个人团队）签不了 iCloud、持续执行、加大内存上限这几项：出包时加 `DM_FREE_ACCOUNT=1` 会去掉它们，少的是 iCloud 存档同步，游戏照常运行；免费账号签的 App 每 7 天要重装一次。用免费账号时：构建前把手机用数据线连上并解锁、在本机配置里填 `DM_IOS_UDID`（构建以这台手机为目标，苹果才肯给免费团队生成描述文件）；装好后第一次打开前，到手机「设置 → 通用 → VPN 与设备管理」里信任你的开发者账号。这一档已真拿免费账号签过、装机进局（12 GB 内存的机器）；6 GB / 8 GB 的机器没验 |

## 构建工具

| 工具 | 版本 | 用来做什么 | 官方地址 | 安装 |
|---|---|---|---|---|
| Xcode（含 Metal 编译器） | 26.6（要在 iPhone Duo 的内屏上铺满，用 27.1） | 编译 App、把着色器编成 Metal 库；用 27.1 时 `./setup.sh` 会用它的编译器重新编着色器 | <https://developer.apple.com/xcode/> | 从 App Store 或开发者网站安装 |
| Homebrew | — | 安装下面几个命令行工具 | <https://brew.sh> | 按官网首页的一行命令 |
| XcodeGen | 2.45.4 | 由 `project.yml` 生成 Xcode 工程 | <https://github.com/yonaskolb/XcodeGen> | `brew install xcodegen` |
| .NET SDK | 10.0 | 编译着色器翻译工具 | <https://dotnet.microsoft.com/download> | `brew install dotnet` |
| SPIRV-Cross | 1.4.357 | 把翻译出的 SPIR-V 转成 Metal 着色语言 | <https://github.com/KhronosGroup/SPIRV-Cross> | `brew install spirv-cross` |
| Python 3 | 3.14 | 运行生成与验证脚本 | <https://www.python.org/downloads/> | macOS 自带或 `brew install python` |

## Python 包

| 包 | 版本 | 用来做什么 | 地址 |
|---|---|---|---|
| Pillow | 12.1.1 | 由游戏文件生成界面图 | <https://pypi.org/project/pillow/> |
| numpy | 2.4.4 | 同上 | <https://pypi.org/project/numpy/> |
| lz4 | 4.4.5 | 解开主程序文件 | <https://pypi.org/project/lz4/> |
| capstone | 5.0.9 | 反汇编，用于改写主程序与验证 | <https://pypi.org/project/capstone/> |
| unicorn | 2.1.4 | 逐处核对改写前后行为等价 | <https://pypi.org/project/unicorn/> |

一次装齐：`python3 -m pip install pillow numpy lz4 capstone unicorn`（`./setup.sh` 会自动建独立环境并装好，不用手动执行）

## 安卓版另外需要

玩家装安卓版不用构建：装发布页的 APK，再用 `dm/tools/d2rm_android.py` 做导入包（它要 Python 包、.NET SDK 和 Vulkan SDK 里的 `spirv-opt` / `spirv-val` / `glslc`，见该脚本开头）。要自己编 APK 才需要下面这些工具（合作者在 Linux / WSL2 上构建验证过），版本与 [dm/android/README.zh-CN.md](../dm/android/README.zh-CN.md) 一致；Linux / WSL 上也可以直接用 `dm/android/docker/` 里版本都钉死的构建环境，就不用自己装下面这些。

| 工具 | 版本 | 用来做什么 | 官方地址 |
|---|---|---|---|
| JDK | 17 或更新 | 跑 Gradle | <https://adoptium.net> |
| Gradle | 9.8.1 | 构建 APK | <https://gradle.org> |
| Android SDK | 36 | 编译与打包 | <https://developer.android.com/studio> |
| Android NDK | 29.0.14206865 | 编译宿主与 Vulkan 图形层 | <https://developer.android.com/ndk> |
| CMake | 3.31.6 | 原生代码的构建脚本 | <https://cmake.org> |
| adb | 随 Android SDK | 开发调试时看日志、推文件（玩家不需要） | <https://developer.android.com/tools/adb> |
| Docker（可选） | — | 用 `dm/android/docker/` 的现成构建环境 | <https://www.docker.com> |

## 可选

| 工具 | 用来做什么 | 官方地址 |
|---|---|---|
| astcenc | 给不支持 BC 贴图格式的旧机型（A16 及更早）转换贴图；从源码编，编法与整条做法见 [LEGACY_DEVICES.zh-CN.md](LEGACY_DEVICES.zh-CN.md)。我们用的是 v5.2.0 | <https://github.com/ARM-software/astc-encoder> |
| hactool | 用 `dm/tools/extract_game.sh` 从你的游戏包取文件时要用；游戏文件已经用别的办法备好时不用装。我们实测用的是上游提交 `1d64a83` | <https://github.com/SciresM/hactool> |
| FFmpeg | 可选的 VP8 过场播放，缺省不开；缺省构建和我们发布的包里都没有它。Mac（`MOVIE_FFMPEG=1`）：`brew install ffmpeg pkg-config`。iOS（`DM_MOVIE_FFMPEG=1`）：`dm/ios/tools/build_ffmpeg_ios.sh` 从 ffmpeg.org 下载官方 FFmpeg 7.1.1 源码包、核 SHA-256，只编 Matroska 解复用、VP8 解码和 swscale 的静态库，见 [dm/host/MOVIE.zh-CN.md](../dm/host/MOVIE.zh-CN.md)。许可证：这套配置没开 GPL 和 non-free 部分，编出的库是 LGPL 2.1 或更新版本；Homebrew 版可能开了 GPL 部分（用 `ffmpeg -L` 看）。如果你分发静态链接了 FFmpeg 的 App，LGPL 的义务要自己履行（提供 FFmpeg 源码，并让收到的人能换一份改过的 FFmpeg 重新链接） | <https://ffmpeg.org> |

## 真机与验证脚本额外需要

只出包、在 Mac 上冒烟用不到这些；跑真机冒烟、界面回归或 `dm/verify/` 下的验证脚本时才要。

| 工具 | 用来做什么 | 安装 |
|---|---|---|
| timeout | 给跑局和构建设墙钟上限（多个脚本用到） | `brew install coreutils` |
| iproxy、ideviceinfo | 真机冒烟经 USB 转发日志端口、读设备电量 | `brew install libimobiledevice` |
| llvm-objdump、llvm-objcopy | 改写等价核对里反汇编与取段 | `brew install llvm` |

## 随仓库的第三方代码

这些已经在仓库里，不用另外下载；各自的许可证在对应目录。

| 位置 | 内容 | 许可证 | 上游 |
|---|---|---|---|
| `dm/third_party/bcdec` | BC 贴图解压 | MIT 或 Unlicense（任选） | <https://github.com/iOrange/bcdec> |
| `dm/third_party/flip` | 画面差异度量 | BSD 3-Clause | <https://github.com/NVlabs/flip> |
| `dm/third_party/libdeflate` | 解压 | MIT | <https://github.com/ebiggers/libdeflate> |
| `dm/gfx/vulkan/engine/third_party/bcdec` | 安卓 Vulkan 图形层里解压 BC 贴图（与上面的 bcdec 同源，单独固定了版本） | MIT 或 Unlicense（任选） | <https://github.com/iOrange/bcdec> |
| `dm/gfx/vulkan/engine/third_party/pillow_reference` | Pillow 的 BC6 解码参考表，只用于 Vulkan 图形层的测试 | HPND（Pillow 的许可证） | <https://github.com/python-pillow/Pillow> |
| `dm/third_party/spirv-tools` | SPIRV-Tools 的优化器库和它引用的 SPIR-V 头文件，编进安卓包，用来在 Adreno 750 这一族上内联着色器的辅助函数 | Apache 2.0（SPIR-V 头文件：Khronos 的 MIT 式许可） | <https://github.com/KhronosGroup/SPIRV-Tools>、<https://github.com/KhronosGroup/SPIRV-Headers> |
| `dm/third_party/ryujinx` | Ryujinx 1.3.2 的着色器翻译库（三个项目，未修改），把 Switch 原版着色器翻译成 SPIR-V | MIT | <https://github.com/Ryubing> |
| `dm/gfx/nvn/include/nvn.h` | 图形接口声明 | MIT | 文件头注明作者 |
| `dm/gfx/nvn/nvn_tex.c` 中的排布尺寸算法 | 移植自 deko3d | zlib | <https://github.com/devkitPro/deko3d> |
| `dm/tools/patch_nso.py` 里让游戏离线运行的 7 处改写 | 取自社区的离线修复补丁（LFS Mod Offline Fix） | 未知 | 首发处与作者我们没有确切记录，知道的请开 Issue，见 [DERIVED.zh-CN.md](DERIVED.zh-CN.md) |
