# dm/tools：工具索引

English: [README.md](README.md)

这个目录里的东西分三类。第一类是你照着首页或说明页做事时会用到的；第二类是我们开发时用的测量与回归脚手架，一并放出来；第三类是最早分析游戏程序时写的一次性探针，留作记录。

## 你会用到的

| 文件 | 做什么 |
|---|---|
| `patch_nso.py` | 程序改写器：把游戏程序里在苹果内核下不能原样执行的少数指令改写掉，其余逐字节保持原样。`setup.sh` 会调它 |
| `extract_game.sh` | 从你自己的游戏包里取出本项目要的两部分（程序文件与资源），放进工作区 |
| `d2rm_pack.py` | 不用 Xcode 出可玩的安装包：把你自己的游戏文件做进「壳」，见 [docs/NO_XCODE.zh-CN.md](../../docs/NO_XCODE.zh-CN.md) |
| `build_deswizzle.py` | 在 Windows x64 上用 LLVM / NDK 从仓库 C 源码编出素材生成器的本机纹理 DLL；不需要游戏文件、Visual Studio 或 Windows SDK，见 [docs/ANDROID.zh-CN.md](../../docs/ANDROID.zh-CN.md) |
| `guest_dylib.py` | 把改写后的游戏程序写成一个苹果格式的动态库（纯 Python，各系统产出逐字节相同）；`d2rm_pack.py` 调它 |
| `winlf/` | Windows 上让做包工具链统一用 `\n` 换行，保证各系统做出的包一致 |
| `product.sh` | 当前改写产物的代号只写在一处（`dm/PRODUCT`），各脚本经它取路径 |
| `dmx.py` | 游戏资源存储的最小读取器（只读）：列文件、取文件 |
| `casc_overlay.py` | 构建时生成覆盖层：让指定的资源路径指向我们新做的内容，不改动原资源档案 |
| `astc_pack.py` | 旧机型补充包：把游戏贴图里的 BC 格式离线转成这些机型支持的 ASTC |
| `gen_bright_levels.py` | 生成「3D 画面加亮」用的关卡表（构建时从你的游戏文件现做） |
| `tact_bootlist.py` | 生成启动预取清单：进选角页之前会用到的资源块，开局时提前解好 |

## 测量与回归脚手架

| 文件 | 做什么 |
|---|---|
| `host_savecmp.sh` | 存档逐字节对拍：宿主回放一局后存盘，与真值存档比较 |
| `save_txn_regress.sh` | 存档事务的确定性回归 |
| `host_war13.sh` | 一个固定用例的宿主回放，逐拍与存档对拍 |
| `l3reg.py`、`l3seg.sh`、`l3run.sh`、`test_l3reg.py` | 长时间运行的内存回归：按游戏拍数回归内存与句柄的增长斜率，外推到长局 |
| `frameprobe.sh` | 帧节奏稳定性的对照量测 |
| `conc_watch.sh` | 计时轮次的并发检测：有别的重活在跑的计时一律作废 |
| `sampagg.py`、`lockalign.py` | 采样日志的汇总；持锁线程的调用链统计 |
| `hle_inventory.py` | 宿主各系统服务实现的盘点（完整 / 简化 / 空实现） |
| `sdis.py`、`sideeff.py` | 反汇编一个函数；沿调用图找一个函数的副作用 |
| `blackcols.py` | 截图里有没有整列纯黑（画面铺没铺满的检查） |
| `guirun.sh` | 在图形会话里起一个程序并收集它的输出 |
| `mini_sync.sh` | 把工作区同步到第二台 Mac 上跑重活（目标机在本机配置里填） |

## 早期分析用的一次性探针

`nso.py`（拆游戏程序的段）、`fn.py`（从异常表取函数边界）、`sweep.py`（线性扫指令）、`x18.py` 与 `cat18.py`（找出并归类用到苹果保留寄存器 x18 的指令）、`xref_sdk.py`（系统库函数的交叉引用）、`tp.c`（验证线程指针寄存器在苹果内核下的行为）。它们是定下改写方案之前写的，依赖当时的中间文件，不保证能直接跑；改写方案的依据在 [dm/README.zh-CN.md](../README.zh-CN.md) 与 [dm/verify/README.zh-CN.md](../verify/README.zh-CN.md)。
