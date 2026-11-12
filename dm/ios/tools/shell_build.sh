#!/bin/bash
# 出「壳」：一个不含任何游戏内容、没有签名的安装包。使用者用自己的游戏文件（dm/tools/d2rm_pack.py）把游戏程序与衍生内容
# 做进去，再用自签工具整包签名装机——全程不需要 Xcode，也不需要 Mac。
#   shell_build.sh [ios|mac]      ios（缺省）= 真机用的壳；mac = 「Mac 上跑的 iOS 包」变体，只给我们自己在 Mac 上验这条路用
# 产物（$DM_ROOT/build/shell/<ios|mac>/）：
#   D2RMobile-shell-build-input.zip  壳（构建原料，不能直接安装：不用 .ipa 扩展名，自签工具认不出来；装上去缺游戏内容也只会弹框说明）。里面没有：游戏程序、着色器库、覆盖层、界面素材、任何从游戏文件生成的表
#   shell_manifest.txt       壳里缺的东西的清单（安装包内路径，一行一个；以 / 结尾 = 整个目录）。d2rm_pack.py 照它核对做全了没有
#   ref/                     对照：用本机游戏文件生成的那些内容（不发布，只用来核对工具的产出）
# 做法：把源码树拷到临时副本里构建，不动当前工作树（三份「从游戏文件生成、编进代码」的文件要换成替身，换在副本里）。
# 出壳的这台机器要有游戏文件与全套构建环境（和平时出包一样）：要用真实的生成结果定表的大小、定清单。
# Build the "shell": an unsigned install package with no game content. Users build the game program and derived content into it from their own game files (dm/tools/d2rm_pack.py),
# then sign the whole package with a sideloading tool and install it, with no Xcode and no Mac needed.
#   shell_build.sh [ios|mac]      ios (default) = the shell for devices; mac = the "iOS package running on Mac" variant, only for us to check this path on a Mac
# Output ($DM_ROOT/build/shell/<ios|mac>/):
#   D2RMobile-shell-build-input.zip  the shell (build input, not installable: it does not use the .ipa extension so sideloading tools do not recognize it; installed as is, it lacks the game content and only shows a message). It does not contain: the game program, shader library, overlay, UI art, or any table generated from the game files
#   shell_manifest.txt       list of what the shell lacks (paths inside the package, one per line; a trailing / = a whole directory). d2rm_pack.py checks against it that everything was built in
#   ref/                     reference: that content generated from the local game files (not published, only used to check the tool's output)
# Method: copy the source tree to a temporary copy and build there, leaving the current work tree alone (three files that are generated from the game files and compiled into the code are replaced with stand-ins, in the copy).
# The machine that builds the shell needs the game files and the full build environment (same as a normal build): the real generated results determine the table sizes and the manifest.
set -u
KIND=${1:-ios}; case $KIND in ios|mac) ;; *) echo "用法：shell_build.sh [ios|mac]"; exit 2;; esac
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); SRC=$(cd "$HERE/../.." && pwd)        # SRC = dm/
ROOT=${DM_ROOT:-$HOME/dm-work}; O=$ROOT/build/shell/$KIND; W=$O/tree; DD=$O/dd
t() { echo "[$(date +%T)] $*"; }; die() { t "🔴 $*"; exit 1; }
mkdir -p "$O" && rm -rf "$W" "$O/ref" "$O/app" "$O/D2RMobile-shell-build-input.zip" "$O/shell_manifest.txt" && mkdir -p "$W" "$O/ref" || die "建不了 $O"
rsync -a --exclude '/ios/Gen' --exclude '/ios/DmIOS.xcodeproj' --exclude '.iosdd*' --exclude '/ios/local.env' "$SRC/" "$W/dm/" || die "拷源码树失败"
# 壳的包名只是占位：自签工具装机时会换成使用者自己的。团队号同理（不签名，用不到）
# The shell's package name is only a placeholder: the sideloading tool replaces it with the user's own at install time. Same for the team ID (not signed, so it is not used)
printf 'DM_TEAM_ID=0000000000\nDM_BUNDLE_ID=org.d2rmobile.app\nDM_FREE_ACCOUNT=1\n' > "$O/shell.env"; export DM_LOCAL_ENV=$O/shell.env DM_FREE_ACCOUNT=1 DM_ROOT=$ROOT
cd "$W/dm/ios" || die "副本里没有 dm/ios"
t "生成（与平时出包同一步：用本机游戏文件）"; DM_GEN_ONLY=1 DM_IOS_DD="$DD" bash run.sh build > "$O/gen.log" 2>&1 || { tail -5 "$O/gen.log"; die "生成失败，日志 $O/gen.log"; }
# 三份编进代码的表：真实数据留作对照，源文件换成替身
# The three tables compiled into the code: the real data is kept as reference, and the source files are replaced with stubs
python3 tools/shell_stubs.py data "$W/dm" "$O/ref/derived.tsv" || die "拆表失败"
python3 tools/shell_stubs.py stubs "$W/dm" || die "写替身失败"
# 模块映像不进可执行文件：内嵌汇编清空、去掉它的链接参数；打开装载器的壳分支
# The module images do not go into the executable: empty the embedded assembly and remove its link flags; turn on the loader's shell branch
OUT=$(bash ../tools/product.sh path touch) || die "取不到产物目录"
python3 ../tools/guest_dylib.py "$OUT" "$O/ref" main sdk > /dev/null || die "对照用的动态库没写出来"
echo '// 壳：模块映像不在可执行文件里（tools/shell_build.sh）' > Gen/embed.S
sed -i '' -e 's/ -Wl,-segprot,__DM[A-Za-z0-9]*W,rw,rw//g' -e 's/^OTHER_CFLAGS = \$(inherited) /OTHER_CFLAGS = $(inherited) -DDM_SHELL /' Gen/dm.xcconfig
grep -q -- '-DDM_SHELL' Gen/dm.xcconfig && ! grep -q segprot Gen/dm.xcconfig || die "构建配置没改对：$(grep -n 'OTHER_' Gen/dm.xcconfig)"
# 壳里不留本机路径（断言带的 __FILE__ 会编进可执行文件，暴露用户名与目录结构）：源码副本与构建目录换成中性前缀；链接留下的目标文件路径在下面 strip -S 去掉
case "$W$DD" in *' '*) die "工作目录带空格，编译参数里放不下：$W";; esac
WR=$(cd "$W" && pwd -P); sed -i '' -e "s#-DDM_SHELL #-DDM_SHELL -ffile-prefix-map=$W=src -ffile-prefix-map=$WR=src -ffile-prefix-map=$DD=build #" Gen/dm.xcconfig
grep -q -- '-ffile-prefix-map' Gen/dm.xcconfig || die "路径替换参数没加上：$(grep -n 'OTHER_CFLAGS' Gen/dm.xcconfig)"
# 真机用的壳同时面向 iPhone 与 iPad（设备族是构建设置，要作为命令行参数传）；Mac 变体不传，窗口与坐标换算照开发包
# The device shell targets both iPhone and iPad (device family is a build setting and must be passed as a command-line argument); the Mac variant does not pass it, and its window and coordinate mapping follow the dev build
if [ "$KIND" = mac ]; then DEST='platform=macOS,arch=arm64,variant=Designed for iPad'; PROD=Release-iphoneos; FAM=; else DEST='generic/platform=iOS'; PROD=Release-iphoneos; FAM=DM_DEVICE_FAMILY=1,2; fi
t "构建（不签名）"
xcodebuild -project DmIOS.xcodeproj -scheme DmIOS -configuration Release -destination "$DEST" -derivedDataPath "$DD" \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY= $FAM build > "$O/build.log" 2>&1 || { grep -E 'error:' "$O/build.log" | head -5; die "构建失败，日志 $O/build.log"; }
APP=$(ls -d "$DD"/Build/Products/Release-*/DmIOS.app 2> /dev/null | head -1); [ -d "$APP" ] || die "没找到构建出的 App"
cp -R "$APP" "$O/app" || die "拷 App 失败"; A=$O/app
# 壳里不该有的：凡是从游戏文件来的，一律搬到 ref/ 对照，并记进清单。下面这张表就是「衍生内容」的全部定义——
# 往包里加新的衍生内容时要来这里登记，否则末尾的核对会把它当成壳的一部分发出去（核对：壳里不许出现清单外的大文件 / 图片）。
# What must not be in the shell: anything that comes from the game files is moved to ref/ as reference and recorded in the manifest. The table below is the complete definition of "derived content" --
# when adding new derived content to the package, register it here, otherwise the check at the end would ship it as part of the shell (check: no large files / images outside the manifest may appear in the shell).
: > "$O/shell_manifest.txt"
take() {   # 安装包内路径（以 / 结尾 = 目录） [nolist = 只搬走、不记进清单：使用者不需要补的东西] | path inside the install package (ending with / = a directory) [nolist = only moved away, not recorded in the manifest: things users do not need to add]
  local p=${1%/}; [ -e "$A/$p" ] || return 0
  mkdir -p "$O/ref/$(dirname "$p")" && mv "$A/$p" "$O/ref/$p" || die "搬不走 $p"; [ "${2:-}" = nolist ] || echo "$1" >> "$O/shell_manifest.txt"; }
V=$(sed -n 's/^#define DMSH_CHAIN "\(.*\)"/\1/p' ../gfx/nvn/dmsh_chain.h); [ -n "$V" ] && [ -d "$A/msl/b-$V" ] || die "着色器库目录不对（链版本「${V}」）"
take "msl/b-$V/"; rmdir "$A/msl" 2> /dev/null
for d in romfs_overlay romfs_overlay_variants hud_v4_ui mapgoal mercslots; do take "$d/"; done
# 测试存档（我们自己的回归用）与老机型补充包的覆盖层（没带补充包时是空目录）：壳里不带，使用者也不用补
# Test saves (for our own regression) and the overlay of the Legacy Device Pack (an empty directory when no Legacy Device Pack is included): not carried in the shell, and users do not need to add them either
for d in save_ref save_ref_warlock romfs_overlay_astc romfs_overlay_astc_variants; do take "$d/" nolist; done
for f in "$A"/dm/*.fixups.tsv "$A"/dm/*.gates.tsv "$A"/dm/bootlist.tsv "$A"/dm/main.cuts.tsv "$A"/dm/c1_full.tsv "$A"/dm_native_*.png "$A"/native_art_manifest.json; do [ -e "$f" ] && take "${f#$A/}"; done
echo "dm/derived.tsv" >> "$O/shell_manifest.txt"; echo "Frameworks/Guest.framework/" >> "$O/shell_manifest.txt"
# 清单与版本信息随壳走（d2rm_pack.py 读它们：壳要的产物代号、着色器链版本对不上就拒绝往里做）
# The manifest and version info travel with the shell (d2rm_pack.py reads them: if the build output codename or shader chain version the shell requires does not match, it refuses to pack into it)
cp "$O/shell_manifest.txt" "$A/dm/shell_manifest.txt"
# 着色器源码摘要与出壳这台机器的 spirv-cross 版本也随壳走：使用者的翻译工具版本不同、翻出来的源码不一样时，d2rm_pack.py 当场报出来
# The shader source digest and the spirv-cross version of the machine that built the shell ship with the shell too: when the user's translator version differs and produces different sources, d2rm_pack.py reports it on the spot
SD=$(python3 ../tools/d2rm_pack.py --src-digest "$ROOT/build/cache/metal_shader_cache/b-$V") && [ -n "$SD" ] || die "算不出着色器源码摘要"
SCV=$(brew list --versions spirv-cross 2> /dev/null | awk '{print $2}')
printf 'PRODUCT=%s\nCHAIN=%s\nKIND=%s\nSHADER_SRC=%s\nSPIRV_CROSS=%s\n' "$(head -1 ../PRODUCT)" "$V" "$KIND" "$SD" "${SCV:-unknown}" > "$A/dm/shell_info.txt"
# 资源下载扩展：只有走苹果托管资源包的正式包才用；自签装机时扩展还要多占一个应用标识，壳里不带
# Asset download extension: only release packages that go through Apple-hosted asset packs use it; when sideloading, the extension would also take up one more App ID, so the shell does not carry it
rm -rf "$A/Extensions" "$A/PlugIns"
for k in BAAppGroupID BAHasManagedAssetPacks BAUsesAppleHosting; do /usr/libexec/PlistBuddy -c "Delete :$k" "$A/Info.plist" 2> /dev/null; done
rm -rf "$A/_CodeSignature" "$A/embedded.mobileprovision"
# 链接时记下的目标文件路径（调试映射）也是本机路径：去掉调试符号（壳不签名，使用者的自签工具装机时整包重签）；再查一遍整个壳不含本机路径
strip -S "$A/DmIOS" || die "strip 失败"
leak=$(LC_ALL=C grep -raoE -- "/Users/[A-Za-z0-9._-]+|$HOME" "$A" | head -3); [ -z "$leak" ] || die "壳里带着本机路径：$leak"
# 核对：壳的可执行文件里没有模块映像节；壳里剩下的文件逐个列出来留档
# Check: the shell's executable has no module image sections; the files left in the shell are listed one by one for the record
n=$(otool -l "$A/DmIOS" | grep -c -E 'sectname __dm.*x[01]|segname __DM.*W'); [ "$n" = 0 ] || die "壳的可执行文件里还有模块映像节 $n 个"
# 再核一道内容：对照数据里的每个字符串（物品名、图标路径、套装名，8 个字符以上的）在壳的任何文件里都搜不到；
# 模块映像开头的 64 字节也搜不到（防止哪天又被编进来）
# One more check of the content: no string in the reference data (item names, icon paths, set names, those of 8 characters or more) can be found in any file of the shell;
# the first 64 bytes of the module images cannot be found either (in case they get compiled in again some day)
python3 - "$A" "$O/ref" "$W/dm" <<'PYEOF' || die "壳里搜到了游戏内容（见上）"
import os, sys
app, ref, src = sys.argv[1:]
needles = set()
for l in open(os.path.join(ref, 'derived.tsv'), encoding='utf-8'):
    f = l.rstrip('\n').split('\t')
    if f[0] in ('VTROW', 'VTSET'): needles.update(x.encode() for x in f[1:] if len(x) >= 8 and not x.lstrip('-').isdigit())
g = open(os.path.join(ref, 'Guest.framework', 'Guest'), 'rb').read(); needles.add(g[0x4000:0x4040]); needles.add(g[0x200000:0x200040])
# 仓库源码里本来就写着的字符串不算（少数物品名是我们的功能代码里点名用的，随源码公开）：副本里三份生成文件此时已是替身，搜到的都是手写源码
blob = b'\0'.join(open(os.path.join(d, fn), 'rb').read() for top in ('host', 'gfx', 'ios/Sources') for d, _, fs in os.walk(os.path.join(src, top)) for fn in fs if fn.endswith(('.c', '.m', '.h')))
own = {n for n in needles if n in blob}; needles -= own
bad = 0
for d, _, fs in os.walk(app):
    for fn in fs:
        b = open(os.path.join(d, fn), 'rb').read()
        hit = [n for n in needles if n in b]
        if hit: bad += 1; print('  %s 里有 %d 处对照内容，如 %r' % (os.path.relpath(os.path.join(d, fn), app), len(hit), hit[0][:40]))
print('内容核对：%d 个特征串（另有 %d 个是源码里本来就有的，不算）× %d 个文件，命中 %d 个文件' % (len(needles), len(own), sum(len(fs) for _, _, fs in os.walk(app)), bad)); sys.exit(1 if bad else 0)
PYEOF
( cd "$A" && find . -type f | sort | while read -r f; do printf '%s\t%s\n' "$(stat -f %z "$f")" "$f"; done ) > "$O/shell_files.tsv"
# 预编的着色器翻译工具随壳走（壳 zip 顶层 Tools/dmsh_xlate/，d2rm_pack.py 取出来用、不放进安装包）：没有 .NET SDK、只有 .NET 运行时的机器（S2Arm 启动器）也能做包。
#   不带调试符号、源码路径映射成 /_/，下面照样核不含本机路径
# The prebuilt shader translator ships with the shell (top-level Tools/dmsh_xlate/ of the shell zip; d2rm_pack.py takes it out and uses it, it does not go into the install package): machines with only the .NET runtime and no SDK (the S2Arm launcher) can pack too.
#   No debug symbols, source paths mapped to /_/; checked below for local paths like everything else
rm -rf "$O/Tools" && dotnet publish ../gfx/tools/dmsh_xlate -c Release -o "$O/Tools/dmsh_xlate" -v quiet -p:DebugType=none -p:DebugSymbols=false "-p:PathMap=$(cd ../.. && pwd)=/_/" || die "预编着色器翻译工具失败"
leak=$(LC_ALL=C grep -raoE -- "/Users/[A-Za-z0-9._-]+|$HOME" "$O/Tools" | head -3); [ -z "$leak" ] || die "预编的翻译工具里带着本机路径：$leak"
( cd "$O" && rm -rf Payload && mkdir Payload && cp -R app Payload/DmIOS.app && zip -qry D2RMobile-shell-build-input.zip Payload Tools && rm -rf Payload ) || die "打包失败"
t "壳：$O/D2RMobile-shell-build-input.zip（$(du -h "$O/D2RMobile-shell-build-input.zip" | cut -f1)，$(wc -l < "$O/shell_files.tsv" | tr -d ' ') 个文件）；缺的内容 $(wc -l < "$O/shell_manifest.txt" | tr -d ' ') 项，清单 $O/shell_manifest.txt"
