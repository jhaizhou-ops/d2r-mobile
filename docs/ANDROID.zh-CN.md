# 在安卓手机上玩

安卓版的做法和 iOS 的「壳」一样：我们在发布页放一个**不含任何游戏内容的 APK**，你用自己的游戏文件在电脑上做一个**导入包**，拷进手机，App 打开时自己导入。不用 adb，不用 Android Studio，也不用自己编 APK。

English: [ANDROID.md](ANDROID.md)

**先看现状：**

| 这一步 | 现状 |
|---|---|
| 电脑上做导入包 | Mac 从头做约 5 分钟（Apple 芯片的 Mac），导入包约 670 MB。Windows x64 也已验证生成、导入及小米 15 上的职业选择画面，见下方「Windows / PowerShell」；Linux 尚未验证 |
| 导入包里的着色器 | 6,927 个游戏着色器包与移植作者真机测试用的那套逐字节相同 |
| 装 APK、导入、进局 | vivo X200 Ultra（Android 16）上走通：授权、导入、建角、进局、各面板、NPC 对话 |
| 帧率 | 移植作者实测：Redmi K50 Ultra（骁龙 8+ 一代）营地与战斗中位 60 帧；OnePlus 13 能玩 |

## 手机要求

- ARM64，Android 12（API 31）及以上，显卡支持 Vulkan 1.1
- 约 7 GB 空间：游戏资源约 5.6 GB，导入后的内容约 0.7 GB（导入完可以删掉手机上的导入包，省回 0.7 GB）

## 你需要准备的

- 你自己购买的游戏的文件：Switch 版 1.0.34.0（对应 PC 版 3.3.94012）的 `ExeFS` 与 `RomFS`，见 [GAME_FILES.zh-CN.md](GAME_FILES.zh-CN.md)
- 这个仓库的源码，**与 APK 是同一个发布版本**（App 会核对，不一致会直说）
- Python 3.9 以上，以及这几个包：`python3 -m pip install pillow numpy lz4 capstone unicorn`
- .NET 10 SDK（编着色器翻译工具，只有运行时不够）：<https://dotnet.microsoft.com/download>
- C 编译器 `clang`（生成界面素材时编译本机纹理转换库），或用 `DM_DESWIZZLE_LIB` 指定已编好的本机库。Windows x64 可以按下节用 LLVM / NDK 单独构建 DLL
- Vulkan SDK 里的 `spirv-opt`、`spirv-val`、`glslc`：Windows / Linux 装 [LunarG Vulkan SDK](https://vulkan.lunarg.com/)；macOS `brew install spirv-tools shaderc`。`spirv-opt` 要新到支持 `--trim-capabilities`（工具会检查）；安卓 NDK 自带的那份太旧，别让它排在 `PATH` 前面。`glslc` 也可以用安卓 NDK 里的
- 约 2 GB 磁盘空间：缓存与中间产物约 0.9 GB（放在 `DM_ROOT`，缺省 `~/dm-work`），做出的导入包约 0.7 GB

缺什么，工具一开头会逐项列出来、告诉你去哪拿。

### Windows / PowerShell

在仓库根目录执行，先把示例里的 Vulkan SDK、LLVM 和游戏目录换成自己的路径。LLVM 目录需要 `clang.exe`，以及 `ld.lld.exe`、`lld-link.exe` 或 `lld.exe` 中的一种；实测用的是 Android NDK r28c 的 `toolchains/llvm/prebuilt/windows-x86_64/bin`。

```powershell
$env:DM_ROOT = Join-Path $env:USERPROFILE 'dm-work'
python -m venv (Join-Path $env:DM_ROOT 'venv')
$python = Join-Path $env:DM_ROOT 'venv\Scripts\python.exe'
& $python -m pip install pillow numpy lz4 capstone unicorn

$env:PATH = "C:\VulkanSDK\<version>\Bin;$env:PATH"
$llvmBin = 'C:\Android\android-ndk-r28c\toolchains\llvm\prebuilt\windows-x86_64\bin'
$env:DM_DESWIZZLE_LIB = Join-Path $env:DM_ROOT 'build\tools\deswizzle.dll'
& $python dm/tools/build_deswizzle.py --llvm-bin $llvmBin --output $env:DM_DESWIZZLE_LIB

& $python dm/tools/d2rm_android.py 'C:\YourGame\Update' (Join-Path $env:DM_ROOT 'import')
```

`build_deswizzle.py` 用仓库原有 `nvn_deswizzle.c`、`nvn_tex.c` 编译 Windows x64 DLL，补上两项最小内存函数，因此不需要 Visual Studio 或 Windows SDK。它检查三个导出函数，成功后才替换输出文件；无需游戏文件即可运行。输出路径应在 `dm/` 之外。环境变量只对当前 PowerShell 会话生效，之后照第二、三步传资源并装机。

已有可用 `clang` 的完整本机 C 工具链时，也可让素材生成器自动编库。指定了 `DM_DESWIZZLE_LIB` 时，打包器会先检查文件、能否加载及所需导出函数；不会忽略坏路径回退到自动编译。未指定库又没有 `clang` 时，会在改写游戏程序之前提示。

本次实测：Windows x64、Python 3.13、.NET SDK 10.0.402、NDK r28c 的 clang 19.0.1 / glslc 2022.3，以及 SPIRV-Tools v2026.4-78-gb8c3b64b。6,927 个游戏着色器全部翻译并通过 Vulkan 1.1 校验，加上 4 个查询核，共 6,931 个包，导入包约 668 MiB。官方 v2.0.2 APK 在小米 15（Android 16、Adreno 830）上通过覆盖层校验并显示中文职业选择页（含术士）。这次没有验证建角、进局、战斗或保存重进，也没有与 Mac 产物逐字节对比。实测从匹配的 `ExeFS` / `RomFS` 开始，使用了已有的 .NET / NDK；从零安装全部依赖及 Windows 下的 `extract_game.sh` 未验证。

不读游戏文件的辅助工具测试：

```powershell
$env:DM_TEST_LLVM_BIN = $llvmBin
& $python -m unittest discover -s dm/tools -p test_build_deswizzle.py
```

## 第一步：在电脑上做导入包

在仓库根目录：

```
python3 dm/tools/d2rm_android.py <游戏文件目录> <输出目录>
```

`<游戏文件目录>` 是 Switch 版 3.3.94012 的 `Update` 目录（里面有 `ExeFS` 和 `RomFS`）。它会改写游戏程序（安卓目标）、取出并翻译全部着色器、生成触屏覆盖层与几张数据表，最后写出 `<输出目录>/android/`：

```
android/out/          改写后的游戏程序与配套表
android/shaders/      Vulkan 着色器包（6,931 个）
android/overlay/      触屏覆盖层、界面素材
android/derived.tsv   几张从游戏文件生成的数据表
android/pack_info.txt 这份导入包是给哪个版本做的
```

做过的步骤再跑会跳过。这个文件夹里有游戏内容，请只自己用，不要分发。

## 第二步：把文件拷进手机

用 USB 连电脑（手机选「文件传输」），在手机存储的 `Download` 里建 `S2X/D2R Mobile/`，放两样东西：

```
Download/S2X/D2R Mobile/android/   第一步做出来的 android 文件夹，整个拷过去
Download/S2X/D2R Mobile/data/      游戏资源：你游戏文件 RomFS/data 里的 18 个 .data 档案和 .build.config
```

`data` 里要的 18 个档案见 [GAME_FILES.zh-CN.md](GAME_FILES.zh-CN.md) 的「手机上要放的那个游戏资源文件夹」；另外别漏了 `.build.config`（点开头的隐藏文件，电脑上常看不见；存成不带点的 `build.config` 也认）。有 `.data.holes` 文件的话一起拷，不拷也行。

## 第三步：装 APK、打开

1. 从[发布页](https://github.com/jhaizhou-ops/d2r-mobile/releases)下载 `D2RMobile-android.apk`，拷进手机，在「文件管理」里点它安装。手机可能先要你允许「安装未知应用」，也可能弹安全检测页，按提示确认即可。
2. 打开 App。它要「所有文件访问」权限才能读 `Download` 里的游戏资源：点屏幕进系统设置，打开后返回。
3. App 发现 `android` 导入包，会带进度条把它导入到自己的内部目录（第一次一两分钟，留在这个页面），然后进游戏。

导入完，手机上的 `android` 文件夹可以删掉；`data` 文件夹要留着，游戏一直从那里读资源。

## 换新版本

每个新版本都要两样一起换：**下载新版 APK**，**用同一版本的源码重做第一步**，把新的 `android` 文件夹拷进手机覆盖旧的。新 APK 直接覆盖安装，存档保留；打开时 App 发现新的导入包会自动重新导入。`data` 文件夹不用动。

APK 与导入包不是同一版本时，App 会提示「导入包与这版 App 不配套」，照提示重做导入包即可。

## 存档与卸载

存档在 App 的内部目录里。**卸载 App 会把存档一起删掉**。以下情况要先卸载才能装我们发布的 APK，装之前请先想办法备份存档：

- 手机上装着移植作者早先签名的版本（同一个包名、签名不同）
- 你自己从源码编过 APK（签名是你电脑上的钥匙）

反过来也一样：一直用我们发布的 APK 升级，存档就一直在。

## 遇到问题

| 提示 | 怎么办 |
|---|---|
| 需要「所有文件访问」权限 | 点屏幕进设置打开，再回到 App |
| 未找到游戏资源 | `data` 文件夹不在 `Download/S2X/D2R Mobile/data`，或里面没有 `.data` 档案 |
| 缺少 .build.config | 把游戏文件 `RomFS/data` 里的 `.build.config` 也拷进 `data` |
| 还没有导入游戏内容 | `android` 文件夹不在 `Download/S2X/D2R Mobile/android`，或没拷全 |
| 导入包与这版 App 不配套 | 用与 APK 同一版本的源码重做第一步 |
| 导入没有完成 | 多半是手机空间不够，腾出约 1 GB 再打开 |

想自己从源码编 APK、或了解安卓版的源码结构，见 [dm/android/README.zh-CN.md](../dm/android/README.zh-CN.md)。
