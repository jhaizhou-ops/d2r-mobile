# 不用 Xcode 出安装包（试验性）

平常的路是：在 Mac 上用 Xcode 把整个 App 构建出来（见首页「构建」一节）。这一页讲另一条路：我们事先构建好一个**不含任何游戏内容的「壳」安装包**，你用自己的游戏文件在自己的电脑上把内容做进去，再用常见的自签工具装到手机上。全程不需要用 Xcode 构建和签名，也不需要苹果的付费开发者账号。

English: [NO_XCODE.md](NO_XCODE.md)

**这是第一版，先看清它现在的边界：**

| 这一步 | 现状 |
|---|---|
| 壳 + 工具做出可玩的安装包 | 已在 Mac 与真机上各走通一局（建角、进局、有声） |
| 不需要苹果的着色器编译器 | 电脑上没有它（Windows / Linux / 没装 Xcode 的 Mac）时，着色器以源码放进安装包，手机首次启动时一次编完（带进度，见下）。这条路在 Mac 上强制走过，真机上验过进局 |
| 在 Windows 上跑这个工具 | 整条走通（Windows 11、Python 3.12）：从头做一个包约 7 分钟。与 Mac 上用同一份游戏文件做的包逐文件比对：游戏程序等 14017 个文件逐字节相同；不同的只有界面数字的字距与 31 张英文标签图（差不到 1 像素——Windows 的图像库缺一个文字排版组件，不影响游玩）和由它们打成的覆盖层档案。Windows 上做的包拷到 Mac 上签名后装到 iPhone：全新安装、导入资源、首次启动编完 7212 个着色器（约 7 分钟）、建角进局换幕都正常。编译器、.NET、Python 是那台机器上原有的，「从零装依赖」这一段没有单独验 |
| 用自签工具签名装机 | **还没验过，欢迎开 Issue 告诉我们结果。** 我们验的是：用免费的 Apple ID 经 Xcode 自动签名装机，以及用开发证书重新签名来模拟自签工具这一步；下面写的自签工具步骤是按这些工具的通常用法写的 |

## 壳里有什么、没有什么

壳从本仓库的[发布页](https://github.com/jhaizhou-ops/d2r-mobile/releases)下载。发布页上有两份：`D2RMobile-shell-build-input.zip` 给 iOS 26 及以上，`D2RMobile-shell-ios18-build-input.zip` 给 iOS 18 – 25（[OLDER_SYSTEMS.zh-CN.md](OLDER_SYSTEMS.zh-CN.md)），按手机的系统挑一份，后面的步骤相同。壳（约 4 MB）里只有我们自己的宿主程序、图标和两张随仓库公开的表。它**没有**：游戏程序、着色器、覆盖层、界面素材、任何从游戏文件生成的数据表。构建壳的脚本（`dm/ios/tools/shell_build.sh`）在最后会拿真实的游戏内容去壳里逐个文件搜一遍，搜到就不出壳。

**壳是做安装包的原料，不能直接装来玩**（所以它是 `.zip`，不是 `.ipa`）。把它改名直接装上去，App 启动时会弹框「这个安装包还不能玩」，列出缺的内容，告诉你回到下面第一步；不会进游戏。

**每次换新版本，都要用新版的壳和同一版本的源码重新做一遍第一步**，再装新做出来的安装包。手机上第三步放的资源档案和版本无关，不用重拷。

## 你需要准备的

- 你自己购买的游戏的文件：Switch 版 1.0.34.0（对应 PC 版 3.3.94012）的 `ExeFS` 与 `RomFS`，见 [GAME_FILES.zh-CN.md](GAME_FILES.zh-CN.md)
- 这个仓库的源码，版本要与壳是同一个发布版本（工具会核对，不一致会直说）
- Python 3.9 以上，以及这几个包：`python3 -m pip install pillow numpy lz4 capstone unicorn opencc-python-reimplemented`
- 一个 C 编译器（`clang`；Windows 上装 LLVM 即可）、.NET SDK。**不需要**苹果的 Metal 着色器编译器：有它（装了 Xcode 的 Mac）就在电脑上把着色器编成库，没有就走手机上现编
- `spirv-cross`，**版本要是 1.4.357.0**（发布标签 `vulkan-sdk-1.4.357.0`）。换个版本，翻译出的着色器就可能不同——我们实测最新源码版与它差 231 个着色器。壳里带着全部着色器源码的摘要，工具翻译完会比对，对不上就停下来告诉你，不会做出一个画面可能出错的包
  - macOS：`brew install spirv-cross`，再用 `brew list --versions spirv-cross` 看版本
  - Windows：没有现成的安装包，从源码编（要 `git`、`cmake`、`ninja` 和上面的 LLVM），编出来的 `spirv-cross.exe` 所在目录加进 `PATH`：

    ```
    git clone --branch vulkan-sdk-1.4.357.0 --depth 1 https://github.com/KhronosGroup/SPIRV-Cross
    cmake -S SPIRV-Cross -B SPIRV-Cross/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DSPIRV_CROSS_ENABLE_TESTS=OFF
    cmake --build SPIRV-Cross/build --target spirv-cross
    ```
- 约 12 GB 磁盘空间（缓存与中间产物，放在 `DM_ROOT`，缺省 `~/dm-work`）

缺什么，工具开头会逐项列出来。

## 第一步：把游戏内容做进壳

```
python3 dm/tools/d2rm_pack.py <D2RMobile-shell-build-input.zip> <你的游戏文件目录> <输出.ipa>
```

`<你的游戏文件目录>` 是含 `ExeFS/` 与 `RomFS/` 的那一层。它依次：改写游戏程序并写成一个动态库 → 从游戏文件生成界面素材与几张数据表 → 生成覆盖层 → 把游戏的着色器翻译成 Metal → 照壳里的清单核对一项不缺 → 写出新的安装包（约 130 MB）。第一次约十几分钟，主要花在翻译着色器上；每一步做过就跳过。

游戏文件只在你的电脑上读。**做出来的安装包里有游戏内容，只能自己用，不要分发。**

它在翻译着色器时会按你电脑上的工具版本重新登记，顺带改写仓库里两个文件（`dm/gfx/tools/dmsh_chain.lock`、`dm/gfx/nvn/dmsh_chain.h`）。这是预期的，不用管，也不要提交它们。

## 第二步：签名装机

用自签工具（如 Sideloadly、AltStore）打开上一步的 `输出.ipa`，登录你自己的 Apple ID，装到手机上。免费的 Apple ID 就可以，苹果对它的限制是：签名 7 天过期（到期重签重装，存档还在）、最多 3 台设备、每台最多 3 个 App。第一次装完要到「设置 → 通用 → VPN 与设备管理」里点一次信任；如果把它卸载了再装（而不是直接覆盖安装），系统会把信任撤掉，要再点一次（实测）。

免费账号下没有 iCloud 存档同步，其余功能不受影响。

## 第三步：把游戏资源放进手机

安装包里不含那 5 GB 的资源档案（太大，也不该进安装包）。装好后把你游戏文件 `RomFS/data` 里的 18 个 `.data` 档案放进一个文件夹，用 Finder 或「文件」App 拷进这个 App 的文件夹；启动后屏上会列出已有几个、还缺哪几个。

## 首次启动：准备着色器（只有不用 Xcode 做出的包才有）

**这一步只出现在没有苹果着色器编译器的电脑上做出的包里**——也就是 Windows、Linux，或没装 Xcode 的 Mac。用 Xcode 构建的包（首页「构建」那条路），以及在装了 Xcode 的 Mac 上用做包工具做出的包，着色器在电脑上就编好放进安装包了：没有这一页，第一次打开直接进主菜单。

在没有那个编译器的电脑上做的包，第一次打开 App 会先停在一页「首次启动：正在准备着色器」，有进度条和预计剩余时间，做完自动进主菜单。在 iPhone 17 Pro Max 上实测约 7 分钟（7212 个着色器，410 秒）。这几分钟四个核心连续满载，做完时机器比较热（实测发热到中等一档，没有被系统降到最热档，也没有被结束），属于正常；刚做完的头一两分钟画面可能不够流畅，凉下来就好。这一步把游戏的全部着色器在手机上编一遍，系统会把结果缓存起来，之后游戏里不会因为着色器卡顿。

- 只需要做一次。系统更新、重装 App、或系统清理了缓存之后，会再做一次。
- 可以切到后台或关掉；回来从头数一遍，已经编过的每个只要不到一毫秒，等于接着做。
- 这几分钟手机会发热，建议插着电、别套太厚的壳。

## 老机型（A16 及更早的芯片）

iPhone 14 系列、iPhone 15 / 15 Plus 以及更早的机型，要多做一份贴图包，并在第一步里带上它。完整说明见 [LEGACY_DEVICES.zh-CN.md](LEGACY_DEVICES.zh-CN.md)，这条路上的做法是：

1. 准备编码器 astcenc：从上游发布页下载对应系统的压缩包解开即可（<https://github.com/ARM-software/astc-encoder/releases>，Windows 用 `astcenc-…-windows-x64.zip` 里的 `bin/astcenc-avx2.exe`）。
2. 做贴图包（三条命令，`<游戏文件目录>` 同上，`D` 是你选的输出目录）：

   ```
   python3 dm/tools/astc_pack.py <游戏文件目录>/RomFS D/work --astcenc <astcenc 的路径> --preset thorough
   python3 dm/tools/casc_overlay.py pack <游戏文件目录>/RomFS D/pack D/work/repl.tsv 0-8
   python3 dm/tools/astc_pack.py finalize D/pack
   ```

3. 做包时带上它：把环境变量 `D2RM_ASTC_PACK` 设成 `D/pack` 的完整路径，再照第一步运行 `d2rm_pack.py`。工具会说一句「带老机型支持」。
4. 第三步放游戏资源时，把 `D/pack/Legacy Device Pack` 这个文件夹也整个拷进手机，和游戏资源文件夹放在同一层。

贴图包和安装包是配对的：重新做了贴图包，就要重新做包。

## 出了问题

- 工具报「壳与这份源码不是同一版」：换成与壳同一个发布版本的源码。
- 启动报「这个安装包里还没有游戏程序」：装的是壳本身，不是第一步的输出。
- 启动后日志里有「没读全」「没有 dm/derived.tsv」：第一步没做完整，重跑。
- 在 Mac 上直接跑这个包做试验时：别把它放在 `/tmp` 一类的临时目录下，系统会让它停在启动前不动；放到你的用户目录里。

## 自己出壳

`dm/ios/tools/shell_build.sh` 出壳。出壳的机器要有游戏文件和全套构建环境（和平常出包一样），因为要用真实的生成结果来定几张表的大小、定「壳里缺什么」的清单，并做最后那道内容核对。
