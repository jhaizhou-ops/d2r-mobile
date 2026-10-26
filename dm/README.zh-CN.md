# dm/ — 改写器、宿主、图形层、iOS 外壳与验证

English: [README.md](README.md)

这里是 D2R Mobile 的全部代码。本页讲这套东西怎么工作、各目录是什么、常用命令，以及 `host/` 里主要文件各管什么。

准备环境与出包的步骤不在这里重复：

- 总体介绍与构建三步：[../README.zh-CN.md](../README.zh-CN.md)
- 依赖与版本：[../docs/DEPENDENCIES.zh-CN.md](../docs/DEPENDENCIES.zh-CN.md)
- 游戏文件怎么准备、放哪：[../docs/GAME_FILES.zh-CN.md](../docs/GAME_FILES.zh-CN.md)
- 一条命令准备工作区：`../setup.sh`
- Windows ARM64 资源准备、原生构建 / 启动与当前限制：[windows/README.zh-CN.md](windows/README.zh-CN.md)

下文的 `$DM_ROOT` 指工作区根目录（缺省 `~/dm-work`）：游戏文件、改写产物、缓存和运行日志都在它下面，不进仓库。

## 怎么工作

游戏程序是三个 NSO 文件（Switch 的可执行模块格式）：`main`（游戏本体）、`sdk`（任天堂 SDK）、`subsdk0`。它们和 iPhone 用同一种指令集，绝大部分代码可以原样执行；要处理的是它们对 Switch 系统（Horizon）的依赖。

### 1. 改写主程序（`tools/patch_nso.py`）

离线把每个模块改写一次，只动在苹果内核下不能原样执行的指令：

| 原指令 | 为什么不能原样执行 | 改成 |
|---|---|---|
| 读写 `x18` 寄存器 | 苹果内核会改这个寄存器 | 读写一个线程槽（每线程一份的存储位） |
| 读写 `tpidr_el0`、读 `tpidrro_el0` | 游戏用它们找线程本地存储和 Horizon 的线程本地区；苹果内核会改 `tpidr_el0`，Horizon 的线程本地区在苹果上没有对应物 | 读写对应的线程槽 |
| `svc #n`（系统调用） | 苹果上没有 Horizon 内核 | 跳到「宿主门」：一段进入宿主 C 代码的入口 |
| 读 `cntpct_el0`（系统计数器） | Horizon 的计时器在苹果上没有对应物 | 同样走宿主门，由宿主按 19.2 MHz 的系统计数器作答 |

此外还有两张按程序构建号登记的表，都写在 `patch_nso.py` 里：

- **剪口**（`CUTS`）：对个别指令或数据的定点修改，每条带原字节、新字节与理由；改写前逐条核对原字节，对不上就中止。
- **观测门**（`OBS`）：在某个函数入口插一个进宿主回调的门，执行完原指令再跳回，供宿主读取现场。

改写出的代码不在运行时再被修改：线程槽号经一张只读的「键页」间接取得，门地址从线程槽里取。原因是 iOS 不允许执行运行时写入的代码，游戏代码必须作为签过名的代码段编进 App。

每个模块的输出在产物目录里：`<名>.patched.bin`（改写后的映像，键页与补丁区接在后面）、`<名>.fixups.tsv`（布局）、`<名>.gates.tsv`（门表）、`<名>.sites.tsv`、`<名>.cuts.tsv`、`<名>.obs.tsv`。

产物有代号，写在 [PRODUCT](PRODUCT) 一行里；`tools/product.sh build` 用同一个改写器出三种产物到 `$DM_ROOT/build/cache/dm/out/<代号>-<种类>`：

| 种类 | 规则集 | 用在哪 |
|---|---|---|
| `keypage` | `DM_RULESET=native` | `verify/regress.sh` 缺省验它 |
| `rules` | `DM_RULESET=touch-run-v4`，不带宿主私有门（`DM_PRIVATE_GATES=0`） | `verify/rulegate.sh` 缺省用它 |
| `touch` | `DM_RULESET=touch-run-v4`，带宿主私有门 | `ios/run.sh` 缺省编进 App 的那一种 |

### 2. 装载与启动（`host/loader.c`、`host/main.c`）

宿主 `dmhost` 扮演 Horizon 的装载器：

1. `loader.c` 把各模块的改写后映像映射进内存，创建线程槽并填写键页，按 `<名>.gates.tsv` 登记每个门，做跨模块重定位，最后设段权限。代码区一装进来就是只读加执行，此后装载器只写数据区。
2. 解析导入符号（模块向外部要的函数）时分三种情况，见下一节。
3. `main.c` 按 SDK 自己的启动顺序初始化 SDK 运行时，执行各模块的 `init_array`（全局构造函数表），然后进入游戏入口 `nnMain`。

每个游戏线程是一个宿主 pthread。`host/thread.c` 的 `guest_thread_init` 给线程填好那几个线程槽：门入口、`x18` 的替身、线程本地存储块、Horizon 线程本地区。

### 3. 系统调用与系统库接管（`host/hle*.c`）

游戏对系统的依赖分三层，各有各的接法：

- **内核系统调用**。改写后的 `svc` 经 `host/entry.S` 的 `dm_gate` 保存寄存器，进 `host/hle.c` 的 `gate_dispatch`，按门的返回地址查出它原来是哪条指令，系统调用交给 `svc_dispatch`。`hle_svc.c` 实现内存类调用（预留一整块客体地址空间，在里面划堆、栈等分区），`hle_thread.c` 实现线程与同步类调用。SDK 里的 `nn::os`（线程、锁、事件等）因此原样运行，换掉的只是它下面的内核。
- **C 库函数**。`hle_table.c` 把调用约定一致、不依赖结构布局的 C 库函数直接接到苹果的 libc。
- **系统服务**。真机上 SDK 通过进程间通信去连文件、账号、音频、输入等系统服务；宿主没有这些服务进程，所以在 SDK 接口这一层按函数名接管，直接给出宿主实现。接管用 `host/hle.h` 的两个宏登记：`HLE` 填补模块没有定义的导入；`HLE_FORCE` 是强制接管，即使 SDK 里有定义，所有引用也解析到宿主实现，每条必须写理由，登记了却没有模块引用会在装载时中止。

没有任何实现的导入会接到一个记名桩上：被调到时在日志里打印一行 `UNIMPL <函数名>` 并返回 0。

联机、好友、商城等联网功能不在范围内：相关接口统一回「不可用」，游戏走离线分支。

### 4. 图形层（`gfx/`）

游戏用 NVN（Switch 的图形接口）画图。`main` 只导入一个取函数地址的入口 `nvnBootstrapLoader`，宿主强制接管它，SDK 里的 NVN 驱动一条都不执行。`gfx/nvn/hle_nvn.c` 把游戏的 NVN 调用录成命令，`gfx/nvn/nvn_mtl.m` 回放成 Metal 调用。着色器不是手写的：游戏原版着色器由 `gfx/tools/` 下的翻译链离线翻成 Metal 着色器库（`../setup.sh` 的最后几步）。细节见 [gfx/README.zh-CN.md](gfx/README.zh-CN.md)。

### 5. iOS 外壳（`ios/`）

App 把 `host/` 与 `gfx/` 的源码直接编进去，前端接口 `host/front.h` 在 Mac 上由 `host/front_mac.m` 实现、在 iOS 上由 `ios/Sources/front_ios.m` 实现。改写后的模块映像由 `ios/tools/gen_embed.py` 生成内嵌汇编，编进可执行文件；`loader.c` 的 iOS 分支不拷贝代码，只把这几块按原来的相对位置映射回去。触摸操作与界面也在 `ios/` 里。细节见 [ios/README.zh-CN.md](ios/README.zh-CN.md)。

## 目录

| 位置 | 内容 |
|---|---|
| `PRODUCT` | 当前改写产物的代号，一行 |
| `tools/` | 改写器 `patch_nso.py`、产物脚本 `product.sh`、读游戏资源存储的 `dmx.py`、构建期改写资源存储的 `casc_overlay.py`，以及一批读码与分析脚本 |
| `host/` | 宿主：装载、系统调用与系统库接管、Mac 前端、游戏内功能；`host/tests/` 是单元测试 |
| `gfx/` | 图形层：`nvn/`（接口层与 Metal 回放）、`tex/`（贴图格式与解排布）、`tools/`（着色器翻译链）、`tests/` |
| `ios/` | iOS App：`Sources/`、工程描述 `project.yml`、出包与装机脚本 `run.sh`、`tools/`（资源生成、冒烟、界面回归）、`tests/` |
| `verify/` | 验证：改写等价回归 `regress.sh`、整套验收 `accept.sh` 及其各项用到的脚本与格式说明 |
| `data/` | 随仓库的小数据：资源覆盖层的借用路径登记表。启动预取清单公开仓库不带：缺了就不预取，启动时少一步提前解码 |
| `i18n/` | 中英文案表 `ui_text.tsv`；`scan_literals.py` 检查玩家看得到的中文都进了这张表，例外登记在 `literal_whitelist.tsv` |
| `third_party/` | 第三方代码，见 [../docs/DEPENDENCIES.zh-CN.md](../docs/DEPENDENCIES.zh-CN.md) |

## 常用流程

**改写三个模块**（`../setup.sh` 已包含；改了 `patch_nso.py` 或换了代号之后重跑）：

```
dm/tools/product.sh build
```

单独改写一个模块：`patch_nso.py <NSO 文件> <输出目录> <模块名>`。

**核对改写等价**：

```
dm/verify/regress.sh [产物目录] [模块…]
```

做三件事：结构核对（`structck.py`）、用 Unicorn（一个 CPU 模拟库）对每处改写做改写前后的等价试验（`equiv.py`）、宿主门功能（`gatefn`）。缺省产物目录是 `product.sh path keypage`，缺省模块 `main sdk subsdk0`。`--fresh` 先用当前的改写器重出一份再验，`--struct-only` 不跑等价试验。任何一项失败退出码为 1。

**编译并运行 Mac 宿主**：

```
make -C dm/host
cd dm/host
DM_ROMFS=<Update/RomFS 目录> DM_SAVE=<存档目录> DM_TEMP=<临时目录> DM_WINDOW=1 \
  ./dmhost "$(../tools/product.sh path touch)" main sdk
```

用法是 `dmhost <产物目录> [模块…] [-- 游戏参数…]`。`DM_SAVE` 必须设置，没设会直接中止。常用环境开关：

| 开关 | 作用 |
|---|---|
| `DM_ROMFS` / `DM_SAVE` / `DM_TEMP` | 游戏资源、存档、临时文件三个挂载点对应的宿主目录 |
| `DM_WINDOW=1` | 开窗口出画面（不设则不开窗口） |
| `DM_AUDIO=1` | Mac 上真出声（缺省不出声，但按真实时间消耗音频缓冲） |
| `DM_WATCH=<秒>` | 看门狗：到时间结束进程 |
| `DM_SVC=1` / `DM_IMPORTS=1` / `DM_DBG=1` | 分别记录内存类系统调用、导入解析与接管、游戏自己的调试输出 |

**单元测试**：`make -C dm/host/tests run`、`make -C dm/gfx/tests run`、`make -C dm/ios/tests test`。

**出 iOS 包**：`cd dm/ios && ./run.sh build`，装到设备用 `./run.sh install`。签名配置见顶层 README。

**整套验收**：`dm/verify/accept.sh [--fast|--full]` 把改写等价、存档对拍、画面闸门、单测、iOS 构建、功能用例等并在一起跑。它除游戏文件外还要用到参照运行数据和标准存档，这些没有随仓库提供；取参照运行数据用的模拟器驱动脚本也不随仓库提供，所以整套验收在仓库外跑不了。

## host/ 各文件

**装载与执行**

| 文件 | 管什么 |
|---|---|
| `main.c` | 引导：装模块、初始化 SDK 运行时、进 `nnMain` |
| `loader.c` | 多模块装载：映射、线程槽、门登记、跨模块重定位、按名接管、段权限 |
| `entry.S` | 门的公共入口 `dm_gate`，未实现导入的记名桩表 |
| `hle.c` | 导入解析（`resolve_import`）与门分派（`gate_dispatch`） |
| `thread.c` | 每个游戏线程的线程槽初始化 |
| `hle.h`、`dm.h` | 公共头：`HLE` / `HLE_FORCE` 登记宏、模块表、日志 |

**内核与系统库**

| 文件 | 管什么 |
|---|---|
| `hle_svc.c` | 内核系统调用：地址空间与堆、查询、调试输出、计时 |
| `hle_thread.c` | 线程与同步类系统调用（线程、锁、条件变量、事件） |
| `hle_guestthread.c` | 由宿主发起、带完整 SDK 线程上下文的游戏线程 |
| `hle_table.c` | C 库函数直通表 |
| `hle_os.c` | 公共杂项：游戏对象到宿主对象的旁表、时钟 |
| `hle_sdkinit.c` | SDK 启动时各系统服务的初始化接管 |

**系统服务**

| 文件 | 管什么 |
|---|---|
| `hle_fs.c` | 文件系统：挂载名对应到宿主目录 |
| `hle_tact.c`、`hle_granny.c` | 资源存储块的 zlib 解压、Granny 数据段解码的宿主实现 |
| `hle_audio.c`、`hle_codec.c` | 音频输出与 Opus 解码 |
| `hle_hid.c`、`hle_touch.c` | 手柄与触屏输入 |
| `hle_swkbd.c`、`hle_charname.c` | 系统软键盘接成宿主输入框；角色名放行中文 |
| `hle_oe.c`、`hle_time.c`、`hle_account.c` | 运行环境、时钟、本地用户 |
| `hle_movie.c` | 过场视频：不解码、不出画面，按原时长空播 |
| `hle_net.c`、`hle_friends.c`、`hle_misc.c` | 联网与好友接成不可用；其余零散服务 |
| `hle_lifecycle.c` | 宿主的退出流程 |

**前端与诊断**

| 文件 | 管什么 |
|---|---|
| `front.h`、`front_mac.m` | 前端接口与 Mac 实现（窗口、键盘） |
| `dm_input.h` | 前端写给输入层的状态 |
| `crash.c` | 崩溃现场打印、看门狗 |
| `diag.c` | 探针 `DM_PROBE`、界面消息记录 `DM_UIMSG`、主线程采样 `DM_SAMPLE` |
| `memstat.m`、`hle_count.c`、`hle_fdstat.c` | 内存账、接管调用计数、文件描述符统计 |
| `romfs_validate.c` | 启动时核对游戏资源目录 |

**游戏内功能**

`dm_*.c` 是在原版之上加的功能和触摸适配，每个文件开头的注释说明它做什么、依据游戏里的哪些地址。例如 `dm_cursor.c`、`dm_world.c`、`dm_actions.c`（触摸点选、与世界交互、买卖与加点）、`dm_hud.c`（底部界面上的数字）、`dm_vault.c` 与 `dm_vault_ui.c`（编年史仓库）、`dm_pouch.c`（百宝囊）、`dm_rift.c`（秘境）、`dm_cloudsync.c`（iCloud 存档同步）、`dm_custom.c`（这些功能的开关）。文件里的地址都是 3.3.94012 原映像的偏移。

## 从你的游戏文件生成资源

仓库里不放任何从原版游戏文件衍生出来的图片、表格和文字（加宽储藏箱、百宝囊框、选角右侧面板、小地图终点、技能图集、底部 UI、编年史仓库表、经验表等）。它们在构建时从**你自己的游戏文件**现做出来，产物不进 git。

**要哪些文件**

- 必需：Switch 版 1.0.34.0（对应 PC 版 3.3.94012）的更新包 RomFS（`Update/RomFS`，里面有 `data/build_manifest.psv`）。版本不对时，生成脚本会按登记的校验值报错（「原件和冻结的不一样」「原文变了」）。
- 可选：PC 版 D2R 的解包目录（下面是 `hd/global/ui/...`，原始 `.sprite`）。有它，底部 UI 用 PC 版的 2 倍原图；没有它，底部 UI 用 Switch 版素材放大，会略糊，生成时打印「没找到 PC 版文件……底栏用 Switch 素材放大，会略糊」。

**放哪**

- Switch RomFS：缺省 `$DM_ROOT/data/switch/3.3.94012/Update/RomFS`，或用环境变量 `DM_ROMFS_SRC=<你的 Update/RomFS>`。
- PC 版解包目录：缺省 `$DM_ROOT/data/assets-src/raw`，或 `DM_PC_DATA=<目录>`。

**跑哪条命令**

- 出 iOS 包：`cd dm/ios && ./run.sh build`。开头自动跑 `tools/gen_assets.py` 与宿主表头生成，不用单独跑。
- 只生成资源：`python3 dm/ios/tools/gen_assets.py [<Update/RomFS>]`。输入没变就不重出。
- Mac 宿主：`make -C dm/host`。缺的三个生成文件（`dm_vault_tab.h`、`dm_hud_exp.h`、`dm_bright_levels.inc`）按 `DM_ROMFS_SRC` 自动生成。
- iOS 单测：`make -C dm/ios/tests test`。缺的技能索引自动生成。

缺游戏文件时会直接报一句说明要放哪个目录。这几条命令调用 `python3`，要在 `setup.sh` 建的 Python 环境里跑（Pillow、numpy 等装在里面，版本见 [../docs/DEPENDENCIES.zh-CN.md](../docs/DEPENDENCIES.zh-CN.md)）：先执行 `source ~/dm-work/build/cache/dm/venv/bin/activate`（设了 `DM_ROOT` 就把 `~/dm-work` 换成它）。

**出了什么**

| 产物 | 位置（不进 git） | 生成脚本（`dm/ios/tools/` 下） |
|---|---|---|
| 加宽储藏箱底图、编年史页盖板 | `dm/ios/Gen/assets/NativeHUD/widestash/` | `gen_widestash_art.py`、`gen_vault_cover.py` |
| 百宝囊框 | `dm/ios/Gen/assets/NativeHUD/pouch/` | `gen_pouch_art.py` |
| 选角右侧面板 | `dm/ios/Gen/assets/NativeHUD/touch/` | `gen_fe_rightpanel.py` |
| 小地图终点标记 | `dm/ios/Gen/assets/NativeHUD/mapgoal/` | `gen_map_goal_art.py` |
| 底部 UI（底图与布局） | `dm/ios/Gen/assets/NativeHUD/v4/` | `gen_hud_v4.py`（几何在 `hud_v4_design.py`，布局 = 原版布局 + `touchrectv1_patch.json`） |
| 技能图集、技能索引 | `dm/ios/Sources/TouchArt/dm_native_*.png`、`dm/ios/Sources/dm_touch_native_art.c` | `native_art/generate.py`（按 `dm/ios/Sources/TouchArt/native_art_manifest.json` 核原件） |
| 编年史仓库表、经验表 | `dm/host/dm_vault_tab.h`、`dm/host/dm_hud_exp.h` | `gen_vault_table.py`、`gen_hud_exp.py`（由 `dm/host/Makefile` 调用） |
| 关卡昼夜表（3D 画面加亮用） | `dm/host/dm_bright_levels.inc` | `dm/tools/gen_bright_levels.py`（同样由 `dm/host/Makefile` 调用） |

物品注释用到的游戏原文在构建时现读游戏字符串表，仓库里不存原文。旧译名对照表与点评表不随仓库提供，可以自备，格式见 `dm/ios/tools/annot/` 下的说明；没有时这两步自动不做，其余注释照常。

`dm/ios/tools/check_hud_gen.sh` 检查这些生成物没有被加回仓库。
