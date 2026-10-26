# Windows ARM64 平台

English: [README.md](README.md)

**原生 Windows ARM64 开发者预览，尚非完成全面验收的发行版。** 完整宿主已加载真实 Windows 目标模块，在 Qualcomm Adreno X1-85 上通过 Vulkan 显示选角与营地，并走通独立测试角色的移动、物品拖放、保存退出和进程重启后重载。无游戏文件也能构建和自测独立运行时。基本可听音频已通过短时用户复测；实体手柄、更广泛玩法和长时间稳定性仍未验收。首次职业选择运行约 5–6 FPS；后续提升及测量限制见下文。

## 环境要求

- Windows 10/11 **ARM64**，不是 x64 或 ARM64EC。
- [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw/releases) 的 **UCRT** 发行包，包含 `aarch64-w64-mingw32-clang.exe` 和 `clang++.exe`。选择 ARM64 宿主包即可原生运行编译器。本构建不支持 MSVC。
- CMake 3.22+、Ninja、Python 3.10+。平台自测只用 Python 标准库，不需要 Capstone、Unicorn 或游戏文件。
- 构建 / 自测目录所在 NTFS 卷必须支持备用数据流和事务式 NTFS。大偏移测试会短暂生成超过 5 GiB 的文件，请预留至少 6 GiB 磁盘空间。
- 前端构建还需要官方 [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) 和**原生 ARM64** Vulkan loader / 驱动。不依赖 Vulkan SDK 的导入库：CMake 检查已安装 loader 的 PE 架构并生成 ARM64 导入库。仅运行时构建不需要 Vulkan 头文件或显卡驱动。

## 无游戏文件的构建与自测

在仓库根目录使用 PowerShell，构建产物留在仓库外：

```powershell
.\dm\windows\build.ps1 `
  -Toolchain C:\Tools\llvm-mingw-ucrt-aarch64 `
  -VulkanHeaders C:\Tools\Vulkan-Headers `
  -PythonPath C:\Tools\Python313\python.exe `
  -BuildDir C:\dm-work\build\windows-arm64
```

工具不在 `PATH` 时，追加 `-CmakePath C:\Tools\CMake\bin\cmake.exe` 与 `-NinjaPath C:\Tools\ninja.exe`。含空格的路径要加引号。脚本不会安装工具或修改系统 `PATH`。

只构建运行时、共享源码编译检查和原生自测时，追加 `-RuntimeOnly`，并省略 `-VulkanHeaders`。更换工具链时请另用构建目录。

等价的 CMake 入口：

```powershell
cmake -S .\dm\windows -B C:\dm-work\build\windows-arm64 -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=C:\src\d2r-mobile\dm\windows\toolchain.cmake `
  -DLLVM_MINGW=C:\Tools\llvm-mingw-ucrt-aarch64 `
  -DVULKAN_HEADERS=C:\Tools\Vulkan-Headers `
  -DCMAKE_BUILD_TYPE=Release `
  -DDM_BUILD_HOST=OFF
cmake --build C:\dm-work\build\windows-arm64 --parallel 4
ctest --test-dir C:\dm-work\build\windows-arm64 --output-on-failure
```

请替换示例中的仓库和工具路径。`DM_BUILD_FRONTEND=OFF` 可去掉 Vulkan 依赖；`DM_CHECK_SHARED=OFF` 可关闭额外的共享源码编译检查，不改变运行时自测。测试失败就报错，不会在不支持的文件系统上假装成功。

| 检查 | 实际覆盖 |
|---|---|
| `windows-audio` | 真实工作线程 + 模拟 WASAPI 端点：单 / 双声道 PCM 顺序、可变补充帧数、漏事件恢复、不超过 30 ms 和实际容量的队列目标、预填后启动、COM / MMCSS 同线程所有权、停止 / 恢复、播放期计时请求配平、部分初始化清理和设备失效后的显式实时静音消耗；不打开扬声器 |
| `windows-performance` | 保守缺省、试验批量提交的开 / 关、非法模式值拒绝、保留显式环境覆盖，以及画质 / 脏页跟踪设置不变 |
| `windows-runtime` | ARM64 实机执行自造 TLS 改写块；保留 TEB、门回写客体 x18；TLS 隔离；真实 SVC 栈映射 / 解除，邻接映射和页面填充下的准确边界；单一 / 分段保护区内连续 32 次深栈写异常恢复和 C++ 异常展开；pthread 析构前还原全部宿主栈界限；5 GiB 地址预留、内存保护和写异常恢复；COFF 注册；UTF-8 文件名；4 GiB 以上定位读写且不移动文件位置；对齐分配；文件 / 目录 NTFS 元数据；目录原子交换及失败保留旧内容；LP64 日历布局和 2038 年后的时间 |
| `windows-loader` | 复用现有自造 ELF 弱 / 强导入重定位测试，使用真实共享装载器和导入解析器 |
| `windows-tools` | 平台 TLS 指令编码、先分配再暂存的栈帧、不支持的 SP 独占窗口拒绝、Mach-O → COFF 转换、拒绝 x64 / ARM64EC 依赖、原生装载器目标检查；以模拟子进程执行真实启动器环境作用域：不传 / 60 / 120 节拍、成功 / 失败后的未设置 / 空 / 已有值恢复，以及非法目标拒绝 |
| `windows-desktop`、`windows-touch-default` | 使用合成模块和覆盖层标记，验证键盘键帽选择、触摸 / 手柄断开请求后仍保留桌面布局、上下文回车 / Esc 映射、留边坐标，以及非桌面模式的默认行为不变 |
| `windows-mouse` | 复用共享桌面鼠标单测：世界点选 / 按住行走、界面 / 物品手势、悬停合并、捕获丢失取消和同次轮询内的短按 |
| `windows-present` | 真实回调 + 模拟 WSI / GPU：Windows / 混合分隔符 / UNC 着色器路径规范化，保留源图像使用完成 fence、稳态不等整队列、按图像复用信号量、安全重建交换链，以及过期 / 次优 / 超时 / 设备丢失路径；不是 GPU 验证 |
| `windows-native-bc`、`windows-ldc-fold`、`windows-forward-temps`、`windows-pipeline-memo`、`windows-writeback` | 复用生产策略回归：BC 资格 / 拷贝、着色器整理、管线状态 / 生命周期键、写回范围 / 图像读取顺序；是 CPU 测试，不是 GPU 等价验证 |
| 构建 | 共享宿主 / HLE / 图形源码、实际转换后的入口 / NVN 汇编、Windows 入口，以及未禁用时的真实 Vulkan 引擎、重放适配器和 Win32 前端 |

无游戏文件的共享源码检查排除 `diag.c`、`dm_bright.c` 和 `dm_vault_ui.c`，因为它们需要由游戏生成的头文件。对象 / 静态库构建不能证明完整可执行文件已链接或游戏已启动。

## 平台契约

- Windows 的 **x18 始终是 TEB**。客体 x18 / TLS / TLR / 门地址使用真正的 `TlsAlloc` 槽，不依赖 winpthreads 私有布局。装载器及每个客体工作线程都会复核直接槽偏移；直接槽不足或布局不符就中止，不会读取别的线程状态。
- 客体工作线程从已有的 Horizon `MapMemory` 登记取得实际栈底，核对可写范围，并在客体执行前后保存 / 恢复 TEB 的 `StackBase`、`StackLimit` 和 `DeallocationStack`。`VirtualQuery(sp - 1)` 从栈顶所在页开始，而不是从整条栈的底部开始；以前误用它作为栈底，使较深栈上的可恢复异常变成 `0xC0000409 / FAST_FAIL_INVALID_SET_OF_CONTEXT`。不能用该页或整个客体地址空间预留范围代替真实栈映射。这项定向修复不代表整体游戏稳定性已验收。
- `DM_TARGET=windows` 输出 **TARGET 3**，拒绝复用苹果 / 安卓 / 鸿蒙产物。不支持的栈改写会明确失败，Windows 不允许退回到 SP 以下的暂存排法。既有苹果 / 安卓 / 鸿蒙路径保留原来的目标选择。
- 客体为 LP64，Windows 为 LLP64。libc 表适配 long 转换与 56 字节的客体 `tm` 布局；对齐分配使用配套的 Windows 释放器。
- 存档提交要求**事务式 NTFS**，在同一事务中交换已提交和暂存目录。事务失败不会退化成几次可见的非原子重命名。NTFS 备用数据流保存客体文件名大小写。没有独立证明所需语义前，不要用 exFAT、FAT、网络共享或云同步目录。
- Windows 上的 `DM_ROMFS_OVERLAY` 用 **`;`** 分隔，例如 `C:\mods\first;D:\mods\second`，不会在盘符的 `:` 处分裂。
- 前端包含可调整大小的 Win32 窗口、真实 Win32 Vulkan surface / swapchain、等比例留边 blit、XInput 和原生 Unicode 输入框。关闭窗口会先请求存档，再进入共享退出流程。音频使用事件式 WASAPI 共享模式、48 kHz 单 / 双声道 PCM16 和系统格式转换，既有 5.1 下混仍用共享实现。输出错误会记日志，并按实时静音消耗策略处理。
- Windows 入口默认 `DM_APP=1`：使用桌面 / 手柄布局和键盘键帽，收到手柄输入后切到 Xbox 键帽。鼠标触点不会切入缺少原生按钮 / 底栏绘制层的手机布局。
- `mach_absolute_time` 使用单调纳秒时钟。Apple 的退休指令计数接口会明确记录诊断并返回 `ENOTSUP`，不以 CPU 周期冒充指令数。`DM_SAMPLE` 依赖 POSIX 线程信号，在 Windows 上明确失败。

修复栈界限后，用户反馈在保持试验 GPU 批量提交开启的情况下，连续至少五次进局未再复现。采集到的后续日志已进入营地、提交存档并正常完成关闭窗口退出。这只是有限的进局复测证据，不代表长期稳定性或所有玩法已验收。

键位：WASD = 左摇杆；J / 空格 = A，K = B，U = X，I = Y；F / G = 摇杆按下；Q / E = 肩键；Z / C = 扳机；`+` = Plus；Tab = Minus；方向键 = 十字键。Enter 按当前页执行确认 / 输入名称 / 创建，Esc 执行返回，局内没有面板时打开功能页。鼠标左键、拖动和悬停接入与 macOS 共用的桌面手势：前端点击、物品操作、世界点选 / 按住行走，而不是原始 Switch 触屏注入；黑边上的按下不算点击，失焦 / 捕获丢失取消仍按着的手势，不作废已经抬起排队的点击 / 放置。原生输入框打开 / 关闭时释放键鼠状态，避免对话框吞掉松键事件后粘键。右键 = B，滚轮 = 右摇杆脉冲。XInput 使用 Xbox 图标并处理死区。这不代表 PC 版鼠标玩法的完整复刻。

## 用自己的游戏文件准备、构建和启动

输入必须是合并后的 Switch **1.0.34.0 / 3.3.94012** 更新目录，包含 `ExeFS\main`、`ExeFS\sdk` 和 `RomFS\data`。准备入口会核对程序哈希。需要 Python 3.10+ 及 `pillow numpy lz4 capstone unicorn`、.NET SDK 8+、LLVM-MinGW UCRT，以及较新的 `spirv-opt.exe`（有 `--trim-capabilities`）、`spirv-val.exe` 和 `glslc.exe`。离线准备可使用 x64 Python / 工具进程：脚本按该 Python 的架构编译解码 DLL，游戏宿主仍为原生 ARM64。不自动安装工具。

```powershell
python .\dm\windows\prepare.py C:\games\Update C:\dm-work\windows `
  --toolchain C:\Tools\llvm-mingw-ucrt-aarch64 `
  --spirv-tools C:\Tools\VulkanSDK\Bin --jobs 4 --verify

.\dm\windows\build.ps1 `
  -Toolchain C:\Tools\llvm-mingw-ucrt-aarch64 `
  -VulkanHeaders C:\Tools\Vulkan-Headers `
  -BuildDir C:\dm-work\windows\build `
  -BuildHost -GeneratedHeaders C:\dm-work\windows\generated

.\dm\windows\run.ps1 -Root C:\dm-work\windows -GameDir C:\games\Update
```

按实际情况替换工具路径；`python` 必须是装有上述依赖的解释器。准备流程重新生成 Windows 目标 3 的模块、三张真实表头、桌面覆盖层 / 键盘键帽、6,927 个转换后的着色器和 4 个从源码编译的查询核。复用共享生成器的资源缓存，但不因旧产物存在就跳过模块 / 着色器生成。.NET 中间产物、解码 DLL、素材和逐步日志（`prepare-logs`）都放在仓外工作根目录，不向源码树写手机原生素材。`--verify` 增加完整逐处等价和变异检查；不带它仅省略这一步额外验证。

`run.ps1 -CheckOnly` 核对可执行文件的 ARM64 PE 架构、Windows 模块目标以及全部 6,931 个预期 XGS1 包，不启动游戏。正常启动设置进程级资源路径，退出时恢复调用者环境。存档默认放在 `<Root>\saves`；通过 `-SaveDir` 指定独立的启动测试目录。不要同时启动两个宿主读写同一存档目录。`-Executable` 可选择其他构建。重编译前先关闭正在运行的宿主。

真实 `main` / `sdk` 产物分别通过 25,084 / 11,975 个站点，每处至少两次有效试验，零分歧、零 SP 以下读取；每个模块均抓到全部 300 个故意破坏的变异。这不代表完整 `regress.sh` / `structck.py` 已移植到 Windows；参见 [Windows 验证射程](../verify/README.zh-CN.md#windows-arm64独立等价检查)。

交互式实机冒烟使用独立存档中的 1 级离线标准德鲁伊：鼠标选择职业，Enter 打开原生名称输入框并输入 Unicode 文本后取消，Esc 返回；进营地后点地移动、打开面板、鼠标切页签，把蓝色回城卷轴从背包右下移到左上，保存退出并正常关闭宿主。新进程重载该角色、重新进局后，卷轴仍在左上，证明验证的不只是角色创建时的默认存档。这是基本输入和存档循环，不是全流程游玩、战斗、所有物品操作或性能验收。本目录不包含游戏内容、游戏生成头、二进制或工具下载包。

## 性能缺省与试验 GPU 批量提交

完整宿主遵循 CMake 的优化等级：`build.ps1` 选择 Release，因此 `dmcore` 现在使用 **O3**，不再被 O1 覆盖。共享 Vulkan 引擎仍为 O2。这不会重新编译游戏原始的 ARM64 机器码。

Windows 启用已有的按设备能力 / 格式检查的原生 BC 贴图路径（`DM_GFX_NATIVE_BC=1`）、等价的常量缓冲分量折叠（`DM_GFX_LDC_FOLD=1`）、单次写入临时变量转发（`DM_GFX_FORWARD_TEMPS=1`）和管线状态复用（`DM_GFX_PIPE_MEMO=1`）。逐操作 CPU 诊断采样默认关闭（`DM_GFX_PERF_SAMPLE=0`），低频 `NVN PERF` 墙钟 / GPU 报告仍可用。显式环境值仍优先，包括用 `0` 关闭优化、用 `DM_GFX_PERF_SAMPLE=32` 恢复采样分项。不改变默认分辨率、查询、同步或脏页跟踪策略。

GPU 批量提交为**试验功能，默认关闭**。单次启动时选择：

```powershell
.\dm\windows\run.ps1 -Root C:\dm-work\windows -GameDir C:\games\Update `
  -Executable C:\dm-work\windows\build-perf\dmhost.exe `
  -SaveDir C:\dm-work\windows\saves-smoke -ExperimentalGpuBatching `
  *> C:\dm-work\windows\perf-batching.log
```

开关设置进程级 `DM_WINDOWS_GPU_BATCHING=1`，启动脚本退出时恢复。它将已有 Vulkan 设置的缺省值设为 `DM_GFX_DEFER_EVENTS=2`、`DM_GFX_READ_PREFLIGHT=0`、`DM_GFX_XVIEW_GUARD=1`；显式图形环境覆盖和引擎标记文件仍优先。直接运行 exe 也可用同一环境变量选择（`0` 关闭该组合，其他值报错）。此模式在槽退休时发布事件，不再每次 signal 都强制提交；每个绘制前也不再全量清空所有待写结果。已有的按范围回读检查和有保护的跨槽 GPU 拷贝仍保留。不关闭查询，但部分计数器内存读取可能看到上一次已解析报告，引入一帧延迟。Win32 呈现端仍必须等源图像使用完成 fence；下述独立的呈现改动只删除稳态下多余的整队列等待。

Adreno X1-85 上三次由用户分别启动的运行，在热身后的匹配选角场景（970–971 次绘制、56 个通道、8 次计算、97 条管线）得到如下 **60 帧统计窗中位数**。三次均为 1600×900 渲染，配置 3D 比例 1.00，日志无动态比例下降：

| 指标 | 原基线 | 保守缺省 | 试验批量提交 |
|---|---:|---:|---:|
| 匹配统计窗 / 测量时长 | 9 / 83.0 秒 | 21 / 181.9 秒 | 27 / 40.5 秒 |
| 游戏呈现吞吐 FPS（范围） | 6.40（6.18–6.99） | 7.04（6.38–7.41） | 40.40（36.39–45.58） |
| 提交路径 CPU 墙钟毫秒 / 帧，含等待 | 131.06 | 118.66 | 3.80 |
| GPU 时间戳毫秒 / 帧 | 33.70 | 26.29 | 11.88 |
| 每帧提交 / fence 等待 | 10.00 / 10.00 | 10.00 / 10.00 | 1.13 / 1.13 |

同场景保守缺省还把存活图像分配从约 790 MB 降到 309 MB。批量提交运行随后进入营地，另有 6 个观察窗为 35.87–53.01 FPS；这**不是**匹配的营地 A/B 对比。用户反馈画面正常。该次运行在第 125 拍记录过一次已有的 `DM_TOUCH_SCRIPT` 读取画面超时，第 134 拍读取画面才撤下，最终正常退出。该警告仍未解决，战斗、长时间稳定性和渲染等价尚未验收。这些顺序测量是游戏呈现吞吐，不是带仪器的显示扫描输出测量，也不是 A/B/A 研究。

复现时使用独立的原版 / 当前版可执行文件和可丢弃的存档目录，不要让两个宿主同时读写同一存档。固定所选角色、窗口大小、前后台状态、缓存热身及其他设置；两次测量进程均设 `DM_3D_SCALE=1`、`DM_3D_SCALE_DYN=0` 钉住画质，保守运行不带试验开关，在同一场景至少停留 60 秒再正常关闭。装载 / 管线创建稳定后比较 `NVN PERF v=1` 的 60 帧报告，不要混入主菜单、读取画面与营地的样本，也不要把启动阶段或显示器 60 Hz 当作 FPS。

## 120 FPS 节拍与当前限制

`-TargetFps 60` 或 `-TargetFps 120` 通过已有的 `DM_PRESENT_HZ` 和 display 间隔模式，为单次启动显式选择宿主节拍。不传时保留自动节拍与继承的设置；自动策略在 120 Hz 屏幕上仍可能选择 60 FPS。即使子进程失败，启动器也会恢复环境值；原来不存在的变量会删除，不再遗留为空字符串。该参数**不会**修改 Windows 刷新率、模拟拍率、分辨率、3D 比例或 FIFO 呈现，也不保证达到目标 FPS：

```powershell
.\dm\windows\run.ps1 -Root C:\dm-work\windows -GameDir C:\games\Update `
  -Executable C:\dm-work\windows\build-perf\dmhost.exe `
  -SaveDir C:\dm-work\windows\saves-smoke -ExperimentalGpuBatching -TargetFps 120
```

Windows 呈现端在调用共享引擎前规范化着色器目录的分隔符。共享引擎用 `/` 查找父目录；未规范化时，反斜杠路径使标记 / 缓存错误地在 `shaders` 内查找。现在 `<Root>\gfx-pass-profile` 等标记和管线缓存按预期定位在 `shaders` 旁边。不删除旧的 `shaders` 内缓存；正确位置缺少缓存时会重新建立。

本机 Adreno X1-85 的原生 Windows 显示查询仍报告桌面为 **60 Hz**。营地采样钉住 1600×900 渲染、`DM_3D_SCALE=1`、`DM_3D_SCALE_DYN=0`、`DM_3D_SCALE_GPU=0`。GPU 约 10.6–10.7 ms / 帧，超过 8.33 ms 预算；代表性逐通道样本中，主场景约 4.1 ms、阴影约 1.3 ms、深度预通道约 0.74 ms。这些是当前设备 / 显示状态下的观察，不是硬件帧率上限。

已有优化 `DM_GFX_NOLOAD=1` 仅在证明首个绘制完整覆盖附件后，每帧省掉 13 次颜色附件加载。分开进局的营地工作量不同，不作为 A/B 提升证据；改用同一进程内 `gfx-ab-target=noload` 每五秒交替开关、关闭逐操作 CPU 计时，对比相同的 892 次绘制 / 53 个通道 / 10 次计算 / 180 条管线 / 62 个布局，并丢弃每个阶段首个报告窗：

| 60 帧统计窗中位数 | 优化关闭 | 优化开启 |
|---|---:|---:|
| 匹配统计窗 | 17 | 16 |
| GPU 毫秒 / 帧 | 10.70 | 10.68 |
| 提交路径 CPU 墙钟毫秒 / 帧，含等待 | 3.81 | 3.80 |
| 游戏呈现吞吐 FPS | 59.02 | 59.10 |

结果**没有实质收益**，因此 Windows 缺省和试验批量提交组合均不启用 `DM_GFX_NOLOAD`。用户反馈画面 / 声音正常，该次运行的 WASAPI 队列耗尽和游戏 PCM 欠载均为 0；临时采样 / A/B 标记随后移除。CPU 墙钟 / 等待与 GPU 字段有重叠，不能相加或换算成宣称的 FPS。**120 FPS 仍未达到、未验收**；修改节拍或在 60 Hz 下运行都不能证明 120 Hz 上屏。120 档仍是试验节拍，真实 120 Hz 下按帧计数的界面与局内行为尚未验收；仍需继续优化渲染并在固定 120 Hz 下验收。

## 音频连续性与呈现等待

原 WinMM 后端会间歇性地成批归还三个 10 ms 缓冲。在一次断音复现中，游戏 PCM 没有欠载，混音回调最长只有 0.03 ms，工作线程单次等待不超过 12.8 ms，但补充间隔仍达到 40.34 ms。仅把线程提到 MMCSS 未能消除可听的断音 / 杂音。

现在 WASAPI 按 `GetCurrentPadding` 的剩余帧数补充，由音频引擎事件驱动，并保留 10 ms 有界后备检查，以处理漏通知 / 设备失效。同一工作线程负责 COM 初始化、全部端点调用、预填后启动、停止 / 重置及释放，并登记为 MMCSS `Audio` / high。仅播放期间申请本进程 1 ms 计时精度，停止、启动失败与关闭时配平释放。查询音频引擎实际容量，不假定缓冲大小：待播目标**最多为 1,440 帧 / 30 ms**，并受端点实际容量限制。实测设备提供 1,056 帧，即 **22 ms**。这些是应用播放队列长度，不是实测的端到端声学延迟。没有修改游戏侧队列深度或混音系数。

最终用户启动的运行已进入营地和区域 2，提交存档并正常退出。约 120 秒内，11,994 次补充共提交 5,757,696 帧 PCM，**设备队列耗尽为 0，游戏 PCM 欠载为 0**；最长补充间隔 13.18 ms，混音 0.09 ms，含混音的缓冲提交 0.39 ms。用户反馈声音连续、没有杂音。这只是一个端点的短时验收，不代表蓝牙 / 热插拔或长期音频验收。启动失败交回共享静音接收端；活动端点丢失会明确记录，并在停止 / 关闭前按真实时间静音消耗。

呈现端不再每帧调用 `vkQueueWaitIdle`。返回前仍等 blit fence，确保所有外部源图像读取完成，满足共享引擎契约。呈现信号量仍按交换链图像分配，重新取得该图像后才复用；交换链重建和销毁仍清空在途工作。不改 FIFO 呈现、渲染画质或批量提交的选择开关 / 缺省值。

一次**较短的中间诊断版运行**（尚未换 WASAPI，已含这项呈现改动及 MMCSS / 计时调度）取得了热身后的匹配选角统计窗：943–944 次绘制、56 个通道、8 次计算、97 条管线、48 个布局。

| 指标 | 已有批量提交基线 | 中间候选版 |
|---|---:|---:|
| 匹配的 60 帧统计窗 / 时长 | 18 / 26.53 秒 | 3 / 3.06 秒 |
| 游戏呈现 FPS 中位数（范围） | 41.65（30.77–47.37） | 59.56（57.23–59.68） |
| 呈现 CPU 墙钟毫秒 / 帧中位数 | 16.66 | 9.76 |
| 提交 CPU 墙钟毫秒 / 帧中位数 | 3.82 | 3.64 |

这些是顺序观察，不是单独隔离 queue-idle 改动的 A/B/A 实验。最终 WASAPI 运行没有保留匹配的选角统计窗，因此这张表**不是**最终版本的 FPS 基准，也不是营地性能对比。更长的匹配测量需遵循上文的复现控制。
