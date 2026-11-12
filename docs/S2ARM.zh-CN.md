# 与 S2Arm 联动

English: [S2ARM.md](S2ARM.md)

[S2Arm](https://github.com/jhaizhou-ops/S2Arm) 是通用的 Switch 移植启动器。D2R Mobile 在它里面登记为「下游深度方案」：玩家在 S2Arm 里导出这款游戏、在确认页选了 D2R Mobile 方案后，导入、解包、签名、装机、推游戏资源都由 S2Arm 做，**做包这一步调本仓库的工具**。两边各自发版、互不复制代码，靠这一页写的约定连起来。改本仓库里下面提到的任何一样，先看这一页。

## 一、包件

每个发布版附一个 `D2RMobile-s2arm-bundle.zip`，S2Arm 按它登记的 SHA-256 下载（与解包同时进行）。里面三样：

| 路径 | 内容 |
|---|---|
| `bundle.json` | `tag`（发布版标签）、`commit`（40 位提交号）、`product`（`dm/PRODUCT` 的产物代号）。S2Arm 核 `commit` 与它登记的一致才用 |
| `shell.zip` | iOS 壳（`dm/ios/tools/shell_build.sh ios` 的产物），顶层 `Tools/dmsh_xlate/` 带预编的着色器翻译工具 |
| `src/` | 这个提交的仓库源码（`git archive`） |

出包件：`python3 dm/tools/s2arm_bundle.py <壳 zip> <输出 zip> [标签]`。工作树有没提交的改动时不出：包件里的源码必须就是记下的那个提交。

## 二、S2Arm 怎么调

```
<S2Arm 自带的 Python> src/dm/tools/d2rm_pack.py shell.zip <S2Arm 的解包目录> <输出.ipa>
    环境变量：DM_ROOT=<S2Arm 给的工作目录>  D2RM_S2ARM_PACK=<S2Arm 资源包输出目录>
```

- 游戏目录是 S2Arm 的解包目录，里面是小写的 `exefs/`、`romfs/`。`d2rm_pack.py` 与 `d2rm_android.py` 都认，在区分大小写的盘上会自动软链成 `ExeFS/`、`RomFS/`。
- 设了 `D2RM_S2ARM_PACK`，做完安装包后再出一份 S2Arm 格式的资源包，见第四节。

## 三、做包只能用 S2Arm 随包带的依赖

S2Arm 的启动器自带一套运行时，玩家机器上不另装东西。所以做包路径（`d2rm_pack.py` 以及它调用的脚本）只能用：

| 依赖 | S2Arm 现在带的 | 备注 |
|---|---|---|
| Python | 3.12，加 `capstone`、`lz4`、`numpy`、`Pillow` | 别的第三方包不能进做包路径（`unicorn` 只给测试与验证脚本用）。`dm/tools/tests/test_s2arm_deps.py` 检查 |
| spirv-cross | S2Arm 发版时的 Homebrew 版本（现在是 1.4.363.0） | 壳里带着色器源码摘要，翻出来的对不上就停下来说清楚。S2Arm 升级 spirv-cross 后摘要若变了，就用那个版本重出壳 |
| .NET | 只有运行时，没有 SDK | 所以壳里带预编的 `dmsh_xlate`。也可以用环境变量 `DM_XLATE_PREBUILT=<目录>` 指定 |
| 苹果的 Metal 着色器编译器 | 在 Xcode 里（S2Arm 的 Mac 启动器要求装 Xcode） | 没有它时着色器以源码放进安装包，手机首次启动时现编 |

## 四、游戏资源怎么上手机

S2Arm 资源包 = `data/<档名>`（手机要的 18 个档案，与 `dm/host/romfs_validate.c` 的 `need[]` 同一份）+ `index.tsv`（路径、字节数、SHA-256）+ `manifest.json`（`version` 2、`index_sha256` = `index.tsv` 文本的 SHA-256）。

S2Arm 装好 App 后按自己的推送把资源推进去：每批不超过 1 GB，打成 pax tar 放进 `Documents/资源包/.incoming/`，再推 `index.tsv` 与 `manifest.json`，然后带环境变量 `S2A_PACK_APPLY_ONLY=1` 起一次 App。App（`dm/ios/Sources/s2a_pack_apply.c`）只做：

- 按名字顺序解开每个 tar，解完就删；
- 按 `index.tsv` 逐个核对文件在、大小对；
- 存 `index.applied.tsv`，写 `.applied` = `index_sha256`；
- 往 `Documents/unpack.log` 追加结果行，然后退出。

日志行的写法是约定的一部分，S2Arm 靠它判进度、完成和失败：`进度：解开 <包>（k/n）…`、`解开 N 个包 … .applied = <sha>…`、`解包失败：<原因>`。截短的推送包连同后面的包和推送记录 `.pushed` 一起删掉，S2Arm 下次按 `index.applied.tsv` 补推。解开后资源在 `Documents/资源包/data/`，`main.m` 找资源的第②条照常认到。

## 五、版本

- 游戏只支持 1.0.34.0：改写规则按主程序构建号登记，每个补丁位置逐字节核原字节。别的构建号，`patch_nso.py` 会说明是哪一版、支持的是哪一版，然后停下，不出包。S2Arm 的确认页同样只让 1.0.34.0 选这个方案，其余照原版方案出。
- S2Arm 的登记表（S2Arm 仓库 `pack/downstream/schemes.json`）写死本仓库的标签、提交号、包件地址与 SHA-256。升级 = 改那一条，走 S2Arm 的合入流程。玩家升级 S2Arm 后，下次导出就用新版；游戏资源不变就不重推。

## 六、发版时多做的一步

1. 出 iOS 壳：`dm/ios/tools/shell_build.sh ios`。出壳机器的 spirv-cross 与 S2Arm 随包的同版本。
2. `python3 dm/tools/s2arm_bundle.py <壳 zip> D2RMobile-s2arm-bundle.zip <标签>`。
3. 把包件附进发布页，同时传到 s2arm.com。
4. 给 S2Arm 提一条改登记表的合入（标签、提交、地址、SHA-256）。

## 七、还没接的

iOS 18 – 25 的旧接口壳（包件里现在只有 iOS 26 的壳）、安卓（`d2rm_android.py` + APK）、鸿蒙、Mac 原生版，以及 Windows 上的 S2Arm 都还没接，玩家暂时按 README 各节走。
