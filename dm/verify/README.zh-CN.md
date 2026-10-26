# dm/verify — 验证工具

English: [README.md](README.md)

D2R Mobile 把 Switch 版游戏程序改写后直接在 iOS 上执行。这个目录回答两个问题：改写后的程序与原版行为是否一致；每次改动有没有把已经对的东西弄坏。

几个贯穿全目录的词：

- **产物**：改写器 `dm/tools/patch_nso.py` 对 `main`、`sdk`、`subsdk0` 三个模块的输出目录，位于 `$DM_ROOT/build/cache/dm/out/<代号>-<种类>`，种类有 `keypage`、`rules`、`touch` 三种。`dm/tools/product.sh build` 一次出齐，`product.sh path <种类>` 打印路径。
- **宿主**：`dm/host` 编出的 `dmhost`，在 Mac 上装载产物并执行游戏。大多数验证跑的是它。
- **真值**：用打了补丁的 Ryujinx 模拟器跑**原版**游戏得到的参照数据——截图、逐拍状态、命令包、存档。宿主的结果与它比。
- **拍**：游戏服务端逻辑的一步。很多判据是「逐拍相同」。
- **局目录**：一次运行的全部输出（日志、截图、记录文件）所在的目录。
- `$DM_ROOT`：工作区根目录，缺省 `~/dm-work`。游戏文件、产物、真值数据、运行现场都在它下面，不进仓库。

## 先看这张表：跑什么需要什么

| 需要的东西 | 能跑的 |
|---|---|
| 仓库 + Python 环境 | `touchcases/test_lib.py`、`test_accept.py` 这两套脚本自测：输入都是合成数据或替身，不起宿主、不读游戏文件 |
| 游戏文件 + 产物（`./setup.sh` 备好） | `regress.sh`（改写等价回归，另需 llvm，见下文「外部工具」）、`fnfuzz.py`、宿主与图形层单测（`make -C dm/host/tests run`、`make -C dm/gfx/tests run`） |
| 再加一份标准存档 | `touchchain.sh`（触摸功能用例） |
| 再加真值数据 | `shotgate.sh`（画面闸门）、`rulegate.sh`、`logicself.sh`、`opmode.sh`、`dm/tools/host_savecmp.sh`、`dm/tools/save_txn_regress.sh`、`gfxq/`，以及把它们串起来的 `accept.sh` |

**真值数据仓库里没有，也不会有**：它是原版游戏的运行记录。取真值用的模拟器驱动脚本（构建模拟器、起局、驱动与录制）也不随仓库提供，所以最后一行这些依赖真值的闸门在仓库外跑不了。本页仍保留它们的说明与真值的格式，因为整套验证以真值为依据；见下文「真值从哪来」与「要自己准备的数据」。

外部工具：

- Python 包 `lz4`、`capstone`、`unicorn`、`numpy`、`Pillow`，装在 `$DM_ROOT/build/cache/dm/venv`（`./setup.sh` 建）。多数脚本缺省用这个环境的解释器，可用 `DM_PY` 换；个别脚本与各个 `make` 直接调 `python3`，所以先执行 `source ~/dm-work/build/cache/dm/venv/bin/activate`（设了 `DM_ROOT` 就把 `~/dm-work` 换成它）再跑。
- `structck.py` 用 `llvm-objdump` 与 `llvm-objcopy` 作独立反汇编器，目录由 `LLVM_BIN` 指定，缺省 `/opt/homebrew/opt/llvm@23/bin`。`brew install llvm` 装到的不一定是 23 版，这个目录不存在时设 `LLVM_BIN=$(brew --prefix llvm)/bin`。
- 多个脚本用到 `timeout` 命令；系统里没有的话要自己装（GNU coreutils 里有）。

## 改写等价回归：`regress.sh`

验证「改写没有改变程序行为」。只要游戏文件与产物，秒级跑完。

```
dm/verify/regress.sh [--fresh] [--struct-only] [产物目录] [模块…]
```

缺省验 `keypage` 产物的 `main sdk subsdk0`。它依次跑三样：

| 工具 | 验什么 |
|---|---|
| `structck.py` | **结构核对**，不执行代码，逐字核对改写器的输出：除登记过的剪口外，非代码段与原映像逐字节相同；代码段里被改的字恰好是登记的改写点；用独立反汇编器重数一遍该改的指令，一条不漏、一条不多；补丁区里不再出现原来不能执行的指令；每个宿主门、参数门、观测门的字节等于模板，并与门表一一对上。核对项的完整清单在文件头 |
| `equiv.py` | **逐处等价**：用 Unicorn（CPU 模拟库）对每一处改写，以多组随机状态分别执行「原指令」与「补丁块」，比较全部通用寄存器与向量寄存器、标志位、栈指针、内存写入、结束地址与访存异常。带变异自检：故意把一批补丁块改坏，必须全部被判出不等价 |
| `gatefn/` | **门功能**：用宿主现行的 `dm/host/entry.S` 现编一个测试台，逐个门执行，核对寄存器回写的约定 |

- `--struct-only`：只跑结构核对与门功能，不跑 `equiv.py`。
- `--fresh`：先用本工作树的改写器重出一份到 `$DM_ROOT/run/verify/dm/fresh/` 再验，并与给定目录逐文件比对，用来发现共用的产物是否过期。
- 环境变量：`DM_PY`、`DM_NSO_DIR`（原版模块所在目录，缺省 `$DM_ROOT/data/switch/3.3.94012/Update/ExeFS`）、`SEED`、`JOBS`、`LLVM_BIN`。
- 输出：末尾一张汇总表，每个模块「结构」「等价」各一行，带分母与射程（实际覆盖了多少）；末行 `REGRESS OK|FAIL <产物名>`，任何一项失败退出码 1。日志在 `$DM_ROOT/run/verify/dm/regress_<时刻>.log`。

相关的独立工具：

- `fnfuzz.py <产物目录> <模块> <NSO>`：函数级差分模糊测试。对每个含改写点的函数，原映像与改写映像各自整函数执行同样的随机输入，比终态。比 `equiv.py` 慢，不在 `regress.sh` 里，按需单跑；选项见文件头。
- `keypage_check.py`：检查键页间接访问的工具，用法见文件头。
- `nsoimg.py`：把 NSO 解成内存映像的公共模块，上面几个工具共用。

## Windows ARM64：独立等价检查

`equiv.py` 也支持 Windows **TARGET 3** 产物。使用 `DM_TARGET=windows` 准备自己的 3.3.94012 模块，然后在 PowerShell 中用装有 `lz4`、`capstone`、`unicorn` 的 Python 环境运行：

```powershell
python .\dm\verify\equiv.py C:\dm-work\windows\out main C:\games\Update\ExeFS\main --jobs 4 --tsv C:\dm-work\windows\main.equiv.tsv
python .\dm\verify\equiv.py C:\dm-work\windows\out sdk C:\games\Update\ExeFS\sdk --jobs 4 --tsv C:\dm-work\windows\sdk.equiv.tsv
```

Windows 模型保持真实 x18 指向合成 TEB，客体 x18 保存在 `TEB + 0x1480 + 槽号*8` 的直接 TLS 槽，键页写入字节偏移。单独核对 TEB 指针未变（包括访存异常试验），内存写入比较只排除已登记的 TLS 槽，不排除整个 TEB。与安卓一样，SP 以下读取判失败。Windows 的 Python 工作进程使用 `spawn`，其他系统仍使用 `fork`。

覆盖改写站点语义、独占窗口分支的两个方向和故意破坏的变异，**不**执行宿主门语义或有意的剪口。Windows CMake 自测另外覆盖原生门 / TLS / 栈行为。`structck.py`、`regress.sh`、真值闸门和完整验收套件尚未移植 / 验收到 Windows；独立检查成功不能写成 `REGRESS OK`。参见 [Windows 准备与启动](../windows/README.zh-CN.md)。

## 宿主验收：`accept.sh`

把各项检查并行跑一遍，任一项红即整体红。

```
dm/verify/accept.sh [--fast|--full] [产物目录]
```

| 项 | 内容 | 判据 | 前提 |
|---|---|---|---|
| ⓪ | `bootchk.sh`：启动块清单（启动时预读的资源块列表） | 产物目录的 `bootlist.tsv` 存在、构建号与 `main.cuts.tsv` 一致、与 `dm/data/bootlist_<构建号>.tsv` 逐字节相同、校验和自洽、逐块与资源包一致。公开仓库不带这份清单：缺了只是不预取、启动时少一步提前解码，但这一项会报红；要用的话拿 `dm/tools/tact_bootlist.py` 自己录一份，放到 `dm/data/bootlist_<构建号>.tsv` 再重出产物 | 游戏文件、启动块清单 |
| ① | `regress.sh` 验被测产物；再对 `touch` 产物（编进 App 的那一种）跑 `--struct-only` | 两行 `REGRESS OK` | 游戏文件 |
| ② | `dm/tools/host_savecmp.sh`：宿主回放一局真值的输入，存盘退出，存档与真值的存档逐字节比（`savecmp.py`） | 逐拍 6 格状态一致；存档一致；负对照（真值存档对起始存档）必须不一致；宿主日志无红线 | 真值局 |
| ②B/C | `rulegate.sh 400` 与 `logicself.sh <touch 产物> 230` | 见下节 | 真值局 |
| ③ | `shotgate.sh`：[画面闸门](screenshot_gate.zh-CN.md) | 每页对真值参照图绿 | 真值参照图 |
| ④ | 单测：`make -C dm/host/tests run`、`make -C dm/gfx/tests run`、`make -C dm/ios/tests test`、`test_accept.py`，以及模拟器驱动脚本自带的两套自测（不随仓库提供） | 六套全过，无构建或链接错误 | — |
| ⑤ | 把 ② 在 `DM_TEST_LAG=400` 下再跑一遍：第 400 拍入口人为睡一会儿，造出「补拍积压」 | 冻结拍仍落在预期的拍；存档判据同 ②；并核实积压确实造出来了 | 真值局 |
| ⑥ | `dm/tools/save_txn_regress.sh`：在回放局里按存档写入次数注入「进程被杀」「写失败」，核对已提交的存档目录。只在全档跑 | 4 例全部命中；已提交的文件等于期望版本；校验和自洽；负对照成立 | 真值局 |
| ⑧ | iOS 构建，只编不装（`DM_NOINSTALL=1 dm/ios/run.sh build`） | 构建成功 | Xcode 与 `dm/ios` 的签名配置 |
| ⑨ | `touchchain.sh` 跑 `touchcases/` 下全部用例 | 每例断言通过 | 标准存档 |
| ⑩ | `opmode.sh`：进局后把运行模式从主机切到掌机 | 切换前已进局；游戏取走两条模式变更通知；进程活到时限、不崩；切换后至少再出 300 帧；窗口尺寸正确；宿主日志无红线 | 真值局 |

（清单里没有 ⑦，序号沿用脚本里的。）

档位与范围：

- `--full`（缺省）：画面闸门跑全部页，加 ⑥。`--fast`：画面闸门只抽 `camp_e stash wild charselect skilltree` 五页，不跑 ⑥。
- 按改动范围自动少跑：改动只在 `dm/ios`（被宿主一起编译的源文件除外）与文档时只跑 ⓪ ⑧；只在 `dm/gfx`、`dm/ios` 与文档时跑 ⓪ ③ ④ ⑧ ⑩；其余情况全量。「改动」指本工作树相对 `main` 分支分叉点的已提交改动加未提交改动；没有这个分支时按全量跑。被跳过的项会逐项列出「跳过」，不静默。`ACCEPT_ALL=1` 强制全量。
- 同一时刻只跑一路：锁在 `$DM_ROOT/run/verify/dm/accept.lock`，后来的排队等。

输出：每项一行读数，末尾一张「期望 / 真结果 / 用时」对照表与 `ACCEPT OK|RED` 一行；退出码 0 = 全绿。日志在 `$DM_ROOT/run/verify/dm/accept/<时刻>_<工作树名>_…/`，红的那次目录里会留一个 `KEEP` 文件。

| 环境变量 | 作用 |
|---|---|
| `TRUTH_RUN` | ②⑤⑩ 用的真值局目录，缺省 `$DM_ROOT/data/switch/truth_runs/cvd5`（`cvd5` 只是脚本里写死的目录名，没有别的含义） |
| `ACCEPT_ALL=1` | 不按改动范围省略 |
| `ACCEPT_SCOPE_ONLY=1` | 只打印范围判定那一行就退出；配 `ACCEPT_SCOPE_CH=<路径清单>` 可以直接给改动路径来试 |
| `FUNC_JOBS` | ⑨ 同时跑几例，缺省 2 |
| `FUNC_LIMIT_S` | ⑨ 每例的卡死兜底秒数，缺省 60 |
| `DM_TOUCH_OUT`、`DM_RULE_OUT`、`DM_LOGICSELF_OUT` | ①、②B、②C 所用产物目录 |

`test_accept.py` 是 `accept.sh` 自己的接线测试：用替身脚本顶替各项，只验快 / 全档的接线与失败传播，不跑真宿主。

### 两道逻辑闸门

- `rulegate.sh [拍数=400] [产物=rules]`：宿主用 `rules` 产物回放一局真值的输入，逐拍 6 格状态与真值的 `ticks.jsonl` 对拍（对拍脚本 `selfcmp.py` 属于模拟器驱动脚本那一组，不随仓库提供）。真值局目录由 `DM_RULE_TRUTH` 指定，须有 `meta.json`、`c2s.jsonl`、`ticks.jsonl`。开跑前先核产物是不是用当前的 `patch_nso.py` 出的，过期就拒跑。末行 `RULEGATE OK|RED`。
- `logicself.sh [产物=touch] [拍数=230]`：产物与**它自己的基线**比。回放同一局真值输入，把逐拍的单位数与哈希同仓库里的 `logicself/touch.json` 比，用来发现「改了不该影响逻辑的东西，逻辑却变了」。真值局目录由 `DM_LOGICSELF_TRUTH` 指定（缺省 `$DM_ROOT/data/switch/truth_runs/cvd5`）。`LOGICSELF_REBASE=1` 重录基线：连跑两遍，两遍一致才写入。末行 `LOGICSELF OK|RED`。

三种记录文件的格式各有一页：[tick_state_format.zh-CN.md](tick_state_format.zh-CN.md)、[command_packet_format.zh-CN.md](command_packet_format.zh-CN.md)、[ui_message_stream.zh-CN.md](ui_message_stream.zh-CN.md)。

内部的分支合并流程脚本不随仓库提供，外部贡献不需要它。

## 画面闸门：`shotgate.sh`

宿主画出的关键画面与真值参照图逐页比，现有 17 页。判据、页规格、怎么出参照图都在 [screenshot_gate.zh-CN.md](screenshot_gate.zh-CN.md)。

配套的小工具：`hostshot.sh`（起一个宿主截一张图）、`shotgate.py`（比一页）、`dynmask.py`（做动画遮罩）、`shotgate_gc.sh`（清旧现场）、`logred.sh`（宿主日志红线关键字表，画面闸门与 `accept.sh` 共用）。

## 触摸功能用例：`touchcases/` 与 `touchchain.sh`

每个用例是 `touchcases/<名>/` 下的一对文件：

- `script.txt`：触点脚本（哪个时刻、哪根手指、在哪按下或抬起），经 `DM_TOUCH_SCRIPT` 交给宿主的触摸前端。
- `assert.py`：断言。读局目录里宿主写下的四份记录——逐拍状态 `ticks.jsonl`、命令包 `c2s.jsonl`、UI 消息 `uimsg.jsonl`、状态探针 `state.jsonl`——判断这次触摸有没有产生该有的结果（物品挪没挪、发没发某个命令包、面板开没开）。公共函数在 `touchcases/lib.py`，它自己的测试是 `touchcases/test_lib.py`。

可选文件：`prefix`（一行，一个前缀目录的路径；先回放这段真值输入，把角色带到野外等位置再开始）、`ticks`（一行，本例总拍数）、`pending`（有这个文件的用例不跑，但会被列出，首行写原因）。

```
dm/verify/touchchain.sh <用例名>
```

- 用例跑在与 iOS 包相同的界面覆盖层上（`dm/ios/tools/gen_romfs_overlay.py` 由游戏资源现生成），测的是玩家实际看到的布局。
- 判据都是逻辑结果，所以不按产品节奏跑：出图不限速，游戏时钟按固定步长快进，触点脚本按服务端的拍推进，结果与本机负载无关。
- 末行 `TOUCHCHAIN <用例> OK|RED 用时 N 秒`。通过的局目录会删掉，`DM_TOUCH_KEEP=1` 保留；红的保留在 `$DM_ROOT/run/verify/dm/touchchain/`。
- 环境变量：`DM_HOST_OUT`（产物，缺省 `touch`）、`DM_TOUCH_SAVE`（起始存档的 `0/` 目录）、`DM_TOUCH_PREFIX`、`DM_TOUCH_RUNROOT`、`DM_ROMFS_SRC`、`DM_TICKS_N`。

前提是一份标准存档（见下文）。现有断言是照那份存档的内容写的——哪个背包格里有物品、角色学了什么技能——换一份存档就要相应改断言。带 `prefix` 的用例（现有 `skill_main`）还需要对应的真值前缀目录。

## 其他工具

| 文件 | 用途 |
|---|---|
| `savecmp.py` | 两份存档目录逐字节对拍。校验和字段各自核自洽，两处墙钟时间字段核来源，其余必须全同 |
| `savesha.sh` | 算一份存档目录的内容指纹 |
| `uicmp.py`、`c2scmp.py` | UI 消息流、命令包序列的对比，规则见各自的格式页 |
| `oracle/swgdb.py`、`oracle/swtick.py` | 模拟器 GDB 接口的最小客户端；经它读逐拍状态 |
| `bootlist_static.py` | 启动块清单的静态核对，`bootchk.sh` 调用 |
| `gfxq/` | 画质度量与视觉等价尺子，见 [gfxq/image_quality.zh-CN.md](gfxq/image_quality.zh-CN.md) |
| `overlay_contract.json` | iOS 触摸覆盖层控件验收的约定草稿；目前没有脚本读取它 |

## 真值从哪来

真值来自 Ryujinx（一个开源的 Switch 模拟器）跑原版游戏。原版 Ryujinx 不够用，要打几处补丁：让它能直接加载解包的游戏目录、能被脚本喂按键与截图、能在指定位置读出游戏内部状态。

构建这个模拟器、起局、驱动与录制用的脚本不随仓库提供，所以只凭本仓库录不出真值，依赖真值的闸门也就跑不了。仓库里留下的是读取这一侧的最小部件 `oracle/swgdb.py`、`oracle/swtick.py`（模拟器 GDB 接口的客户端），以及三种记录的格式说明。

录制的做法：驱动脚本经模拟器的 GDB 接口，在游戏的 UI 消息投递处与服务端拍入口下断点，按游戏自己的消息推进——每一步都是「等前置消息 → 发输入 → 等确认消息」，不按秒数猜。建局种子与建局时刻两边定成同一个值，宿主侧对应 `DM_GAME_SEED`、`DM_FIXED_TIME`。模拟器跑的 `main` 带着改写器登记的剪口（标为「仅宿主」的除外），所以真值与宿主跑的是带同样定点修改的程序。

录一局得到一个「真值局」目录，里面有：`uimsg.jsonl`、`ticks.jsonl`、`c2s.jsonl`（三种记录，格式见各自的页）、`events.jsonl`（驱动过程的事件流）、`meta.json`（种子、时刻、起始存档指纹等）、`save_after/`（结束时的存档）、`shots/`（截图）。

## 要自己准备的数据

| 数据 | 脚本找它的位置 | 用它的 | 怎么来 |
|---|---|---|---|
| 标准存档 | `$DM_ROOT/data/switch/save_ref_warlock/0000000000000001/`（里面有 `0/` 等子目录） | 几乎所有跑宿主的检查、真值模拟器 | 读者自己的存档。`dm/tools/host_savecmp.sh` 还按 `$DM_ROOT/data/switch/<存档名>/MANIFEST.sha256` 认存档：每行「sha256 + 两个空格 + `0000000000000001/相对路径`」，按路径排序 |
| 一局基准真值局 | `$DM_ROOT/data/switch/truth_runs/cvd5/` | `accept.sh` 的 ②⑤⑥⑩、`logicself.sh` | 用模拟器驱动脚本录（不随仓库提供）。需要 `meta.json`、`c2s.jsonl`、`ticks.jsonl`、`events.jsonl`、`save_after/0`。录这一局用的具体参数仓库里没有留下 |
| 规则闸门的真值局 | `DM_RULE_TRUTH` 指定 | `rulegate.sh` | 同上，需要 `meta.json`、`c2s.jsonl`、`ticks.jsonl` |
| 画面闸门参照图、动画遮罩、各页的回放文件 | 页规格里写的路径，都在 `$DM_ROOT/data/switch/` 下 | `shotgate.sh`、`gfxq/` | 见 [screenshot_gate.zh-CN.md](screenshot_gate.zh-CN.md) |
| 触摸用例的前缀目录 | 用例的 `prefix` 文件里写的路径 | `touchchain.sh skill_main` | 一段真值输入：`manifest.json` 加 `c2s.jsonl` |
| 画质度量战斗帧的回放文件 | 路径写死在 `gfxq/shoot.sh`、`gfxq/ruler.sh` | `gfxq/` | 一局带战斗的真值局的 `c2s.jsonl` |

真值局的 `meta.json` 里记着起始存档的指纹，回放它的脚本会核对；所以标准存档一换，真值局、参照图都要用新存档重录。
