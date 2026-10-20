#!/bin/bash
# 一条命令把工作区准备好：检查依赖 → Python 环境 → 改写主程序 → 取着色器包 → 编译翻译工具 → 翻译全部着色器。
# 做完之后出 App：cd dm/ios && ./run.sh build
# 用法：./setup.sh                 准备工作区
#       ./setup.sh check           体检：逐项报告缺什么、去哪拿、下一步命令，不动任何东西
#       ./setup.sh <游戏包 .xci 或 .nsp> <prod.keys>    先从你自己的游戏包里取出游戏文件，再准备工作区
# 报告的语言跟系统语言走，也可用 DM_LANG=zh / en 指定。
# 工作区根目录 DM_ROOT（缺省 ~/dm-work）：游戏文件放在 $DM_ROOT/data/switch/3.3.94012/Update/{ExeFS,RomFS}，产物与缓存也都在它下面。
# 每一步做过就跳过；想重做某一步，删掉它的产物目录再跑。依赖说明与官方链接见 docs/DEPENDENCIES.zh-CN.md。
set -u
HERE=$(cd "$(dirname "$0")" && pwd); export DM_ROOT=${DM_ROOT:-$HOME/dm-work}
GAME=$DM_ROOT/data/switch/3.3.94012/Update; PY=$DM_ROOT/build/cache/dm/venv/bin/python3
say() { echo "[$(date +%T)] $*"; }
die() { echo "✗ $*" >&2; exit 1; }

# 说话用中文还是英文：DM_LANG=zh / en，没设就看系统语言
case "${DM_LANG:-${LC_ALL:-${LANG:-}}}" in zh*) ZH=1;; *) ZH=0;; esac
m() { if [ "$ZH" = 1 ]; then echo "$1"; else echo "$2"; fi; }

# 给了游戏包与密钥：先把游戏文件取出来放到位（dm/tools/extract_game.sh），再接着往下做
case "${1:-}" in
  *.xci|*.XCI|*.nsp|*.NSP)
    [ $# -ge 2 ] || die "$(m "用法：./setup.sh <游戏包 .xci 或 .nsp> <prod.keys>" "Usage: ./setup.sh <game package .xci or .nsp> <prod.keys>")"
    if [ -d "$GAME" ]; then say "$(m "已有游戏文件目录 ${GAME}，不再解包" "Game files directory $GAME already exists; skipping extraction")"
    else bash "$HERE/dm/tools/extract_game.sh" "$1" "$2" || exit 1; fi
    shift 2;;
esac

# 体检：逐项报「状态 / 缺的话去哪拿 / 下一步命令」。./setup.sh check 只做这一段，不动任何东西
missing=0; NEXT=
ok()  { echo "  ✓ $1"; }
bad() { echo "  ✗ $1"; echo "      → $2"; missing=$((missing+1)); [ -n "$NEXT" ] || NEXT=$2; }
note() { echo "  · $1"; }
tool() {   # 命令  中文用途  英文用途  安装办法
  if command -v "$1" > /dev/null 2>&1; then ok "$1"; else bad "$(m "缺 $1（$2）" "$1 is missing ($3)")" "$4"; fi
}
say "$(m "一、这台电脑" "1. This computer")"
OSV=$(/usr/libexec/PlistBuddy -c "Print ProductVersion" /System/Library/CoreServices/SystemVersion.plist 2> /dev/null)
if [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ]; then ok "$(m "Apple 芯片的 Mac，macOS $OSV" "Apple silicon Mac, macOS $OSV")"
else bad "$(m "需要 Apple 芯片的 Mac（这台是 $(uname -s) $(uname -m)）" "An Apple silicon Mac is required (this is $(uname -s) $(uname -m))")" "$(m "换一台 Apple 芯片的 Mac 再来" "Use an Apple silicon Mac")"; fi
# 图形层整个建在 macOS 26 / iOS 26 才有的图形接口（Metal 4）上：macOS 低于 26 时，在这台 Mac 上运行的那几样（Mac App、宿主、图形层单测、Mac 冒烟）编不过也跑不了
# The graphics layer is built entirely on a graphics interface that exists only from macOS 26 / iOS 26 on (Metal 4): below macOS 26, the things that run on this Mac (the Mac App, the host, the graphics unit tests, the Mac smoke run) neither build nor run
case "${OSV%%.*}" in ''|*[!0-9]*) ;; *) [ "${OSV%%.*}" -ge 26 ] || note "$(m "这台 Mac 是 macOS ${OSV}：低于 26。Mac App（dm/tools/mac_app.sh）、宿主与图形层单测、Mac 冒烟需要 macOS 26 及以上（用到了 macOS 26 才有的图形接口）。要在这台 Mac 上玩，出老接口版：DM_MAC_M3=1 dm/tools/mac_app.sh（最低 macOS 15.0，见 docs/OLDER_SYSTEMS.zh-CN.md）" "This Mac runs macOS ${OSV}, below 26. The Mac App (dm/tools/mac_app.sh), the host and graphics unit tests and the Mac smoke run need macOS 26 or later (they use a graphics interface that exists only from macOS 26 on). To play on this Mac, build the older-API version: DM_MAC_M3=1 dm/tools/mac_app.sh (minimum macOS 15.0, see docs/OLDER_SYSTEMS.md)")";; esac
DK=$DM_ROOT; while [ ! -d "$DK" ] && [ "$DK" != / ]; do DK=$(dirname "$DK"); done; FREE=$(df -g "$DK" 2> /dev/null | awk 'NR==2 {print $4}'); WANT=30; [ -d "$GAME" ] && WANT=15
if [ -n "$FREE" ] && [ "$FREE" -ge "$WANT" ]; then ok "$(m "磁盘剩余 $FREE GB（至少要 $WANT GB）" "$FREE GB free disk space (at least $WANT GB needed)")"
else bad "$(m "磁盘剩余 ${FREE:-?} GB，不够：工作区要约 12 GB，解游戏包另要约 20 GB" "Only ${FREE:-?} GB free: the workspace needs about 12 GB, extracting the game package about 20 GB more")" "$(m "腾出空间，或用 DM_ROOT=<别的盘上的目录> 换位置" "Free up space, or point DM_ROOT=<a directory on another disk> elsewhere")"; fi

say "$(m "二、工具" "2. Tools")"
if command -v xcodebuild > /dev/null 2>&1 && XV=$(xcodebuild -version 2> /dev/null | head -1) && [ -n "$XV" ]; then
  ok "$XV"
  if xcrun -sdk iphoneos metal --version > /dev/null 2>&1; then ok "$(m "Metal 着色器编译器" "Metal shader compiler")"
  else bad "$(m "Xcode 里没有 Metal 着色器编译器（翻译着色器要用）" "Xcode has no Metal shader compiler (needed to translate shaders)")" "xcodebuild -downloadComponent MetalToolchain"; fi
else bad "$(m "缺 Xcode（编译 App 与着色器）" "Xcode is missing (builds the app and the shaders)")" "$(m "从 App Store 安装 Xcode，装完打开一次；再执行 sudo xcode-select -s /Applications/Xcode.app" "Install Xcode from the App Store and open it once; then run sudo xcode-select -s /Applications/Xcode.app")"; fi
command -v brew > /dev/null 2>&1 || note "$(m "没有 Homebrew：下面几个工具用它装最省事，见 https://brew.sh" "Homebrew not found: it is the easiest way to install the tools below, see https://brew.sh")"
tool xcodegen    "生成 Xcode 工程" "generates the Xcode project" "brew install xcodegen"
tool spirv-cross "着色器转 Metal 着色语言" "converts shaders to Metal Shading Language" "brew install spirv-cross"
# spirv-cross 换个版本，翻译出的着色器源码就可能不同（实测最新源码版与 1.4.357.0 差 231 个着色器）。自己整条构建时前后自洽，不受影响；
# 用发布的壳做包（docs/NO_XCODE）时要与壳一致，对不上 d2rm_pack.py 会停下来。这里把版本报出来，不拦。
# A different spirv-cross version may translate shaders differently (231 shaders differ between the latest source and 1.4.357.0). A full build
# of your own stays self-consistent; packing into a released shell (docs/NO_XCODE) must match the shell, and d2rm_pack.py stops if it does not.
SC_WANT=1.4.357.0
if command -v spirv-cross > /dev/null 2>&1; then
  SC_HAVE=$(brew list --versions spirv-cross 2> /dev/null | awk '{print $2}')
  if [ "$SC_HAVE" = "$SC_WANT" ]; then ok "$(m "spirv-cross 的版本是验过的 $SC_WANT" "spirv-cross is the verified version $SC_WANT")"
  elif [ -n "$SC_HAVE" ]; then note "$(m "spirv-cross 是 ${SC_HAVE}，验过的是 ${SC_WANT}：自己整条构建不受影响；往发布的壳里做包时翻译结果可能对不上（工具会报）" "spirv-cross is $SC_HAVE, the verified version is $SC_WANT: a full build of your own is unaffected; packing into a released shell may not match (the tool will say so)")"
  else note "$(m "读不到 spirv-cross 的版本（不是用 Homebrew 装的）；验过的是 $SC_WANT" "Cannot read the spirv-cross version (not installed with Homebrew); the verified version is $SC_WANT")"; fi
fi
tool python3     "生成与验证脚本" "generation and verification scripts" "brew install python"
DOTNET=$(command -v dotnet || { [ -x "$HOME/.dotnet/dotnet" ] && echo "$HOME/.dotnet/dotnet"; } || true)
if [ -n "$DOTNET" ]; then ok "dotnet"; else bad "$(m "缺 dotnet（编译着色器翻译工具）" "dotnet is missing (builds the shader translation tool)")" "brew install dotnet"; fi
# 编出来的翻译工具是个独立的小程序，启动时要自己找 .NET 运行时：只认 DOTNET_ROOT 和几个固定位置，Homebrew 装的不在其中，找不到就起不来
#（那时后面会报成「翻译器不是用仓内源编的」，其实是它没跑起来）。没设 DOTNET_ROOT 时，按 dotnet 自己报的运行时位置设上。
# The translation tool that gets built is a standalone program that has to locate the .NET runtime itself at startup: it only looks at DOTNET_ROOT and a few fixed locations, and a Homebrew install is not among them, so it cannot start
# (which would later be reported as "the translator was not built from the in-repo source" although it simply did not run). When DOTNET_ROOT is not set, set it from the runtime location dotnet itself reports.
if [ -n "$DOTNET" ] && [ -z "${DOTNET_ROOT:-}" ]; then
  DOTNET_ROOT=$("$DOTNET" --list-runtimes 2> /dev/null | sed -n 's/^Microsoft\.NETCore\.App [^ ]* \[\(.*\)\/shared\/Microsoft\.NETCore\.App\]$/\1/p' | head -1)
  [ -n "$DOTNET_ROOT" ] && export DOTNET_ROOT || unset DOTNET_ROOT
fi

say "$(m "三、游戏文件（${GAME}）" "3. Game files ($GAME)")"
gf=0
while read -r want name; do
  if [ ! -f "$GAME/ExeFS/$name" ]; then echo "  ✗ $(m "缺 ExeFS/$name" "ExeFS/$name is missing")"; gf=$((gf+1))
  elif [ "$(shasum -a 256 "$GAME/ExeFS/$name" | cut -c1-64)" = "$want" ]; then ok "ExeFS/${name}（3.3.94012）"
  else echo "  ✗ $(m "ExeFS/$name 不是 3.3.94012 这一版的" "ExeFS/$name is not from version 3.3.94012")"; gf=$((gf+1)); fi
done <<SUMS
55fd909bb7230b002357e5b0d8499c11f06888f345c49e69eb4734df44a27d0e main
1c9f198a91c0bcfee5cf8f5b53fb941895e91334dfb766ce79fc68576797f017 sdk
67a38fcf89cd13e040a021eb3d157072f38ebf6680a907874c35c7b231b5d4e2 subsdk0
SUMS
if [ -f "$GAME/RomFS/data/build_manifest.psv" ]; then ok "RomFS/data/build_manifest.psv"; else echo "  ✗ $(m "缺 RomFS/data/build_manifest.psv" "RomFS/data/build_manifest.psv is missing")"; gf=$((gf+1)); fi
arc=0; for a in 00-00000001 00-00000002 00-00000003 00-00000004 00-00000005 00-00000006 01-00000001 01-00000002 02-00000001 02-00000002 03-00000001 03-00000002 04-00000001 04-00000002 05-00000001 05-00000002 17-00000001 17-00000002; do [ -f "$GAME/RomFS/data/$a.data" ] && arc=$((arc+1)); done
if [ "$arc" = 18 ]; then ok "$(m "手机上要用的 18 个资源档案都在" "All 18 resource archives needed on the phone are present")"; else echo "  ✗ $(m "手机上要用的 18 个资源档案只有 $arc 个" "Only $arc of the 18 resource archives needed on the phone are present")"; gf=$((gf+1)); fi
if [ "$gf" != 0 ]; then
  missing=$((missing+gf))
  if command -v "${HACTOOL:-hactool}" > /dev/null 2>&1; then G="./setup.sh $(m "<你的游戏包 .xci 或 .nsp> <你的 prod.keys>" "<your game package .xci or .nsp> <your prod.keys>")"
  else G="$(m "先装 hactool（https://github.com/SciresM/hactool ，按它的说明编译，放进 PATH），再执行 ./setup.sh <你的游戏包> <你的 prod.keys>" "Install hactool first (https://github.com/SciresM/hactool, build it per its instructions and put it in PATH), then run ./setup.sh <your game package> <your prod.keys>")"; fi
  echo "      → $G"; echo "      → $(m "游戏文件只能从你自己买的 Switch 版导出，怎么导出见 docs/GAME_FILES.zh-CN.md" "Game files can only come from the Switch copy you own; see docs/GAME_FILES.md for how to export them")"; [ -n "$NEXT" ] || NEXT=$G
fi

say "$(m "四、签名（出 App 时才用，现在缺也不拦）" "4. Signing (only needed when building the app; not blocking now)")"
LE=; for f in "${DM_LOCAL_ENV:-}" "$HERE/dm/ios/local.env" "$HOME/.config/dm-ios/local.env"; do [ -n "$f" ] && [ -f "$f" ] && { LE=$f; break; }; done
if [ -z "$LE" ]; then note "$(m "还没有签名配置：cp dm/ios/local.env.example dm/ios/local.env，填 DM_TEAM_ID 与 DM_BUNDLE_ID" "No signing configuration yet: cp dm/ios/local.env.example dm/ios/local.env, then fill in DM_TEAM_ID and DM_BUNDLE_ID")"
else
  tid=$(. "$LE" > /dev/null 2>&1; echo "${DM_TEAM_ID:-}"); bid=$(. "$LE" > /dev/null 2>&1; echo "${DM_BUNDLE_ID:-}"); fa=$(. "$LE" > /dev/null 2>&1; echo "${DM_FREE_ACCOUNT:-}")
  if [ -n "$tid" ] && [ -n "$bid" ]; then ok "$(m "团队号与包名已填（${LE}）" "Team ID and bundle ID are set ($LE)")"; else note "$(m "$LE 里 DM_TEAM_ID / DM_BUNDLE_ID 还没填全" "DM_TEAM_ID / DM_BUNDLE_ID are not both set in $LE")"; fi
  if [ "$fa" = 1 ]; then note "$(m "免费账号档：没有 iCloud 存档同步，App 每 7 天要重新构建重装" "Free account mode: no iCloud save sync; the app must be rebuilt and reinstalled every 7 days")"; fi
fi

if [ "$missing" != 0 ]; then
  echo; m "还差 $missing 项。先做这一步：" "$missing item(s) still missing. Do this first:"; echo "    $NEXT"; m "做完再运行 ./setup.sh check。" "Then run ./setup.sh check again."; exit 1
fi
if [ "${1:-}" = check ]; then echo; m "体检通过。下一步：./setup.sh（首次约 11 分钟）；做完后 cd dm/ios && ./run.sh build" "All checks passed. Next: ./setup.sh (about 11 minutes the first time); then cd dm/ios && ./run.sh build"; exit 0; fi

step() { local name=$1; shift; local t0; t0=$(date +%s); say "$name"; "$@" || die "$name 没成功"; say "$name 完成（$(( $(date +%s) - t0 )) 秒）"; }

py_env() { python3 -m venv "$DM_ROOT/build/cache/dm/venv" && "$PY" -m pip install -q pillow numpy lz4 capstone unicorn opencc-python-reimplemented; }
[ -x "$PY" ] && "$PY" -c 'import PIL, numpy, lz4, capstone, unicorn' 2> /dev/null || step "Python 环境与包" py_env

P=$(head -1 "$HERE/dm/PRODUCT")
[ -f "$DM_ROOT/build/cache/dm/out/$P-touch/main.patched.bin" ] || step "改写主程序（三种产物）" env DM_PY="$PY" bash "$HERE/dm/tools/product.sh" build

packs() {
  mkdir -p "$DM_ROOT/build/cache/dm/dmsh" && cd "$DM_ROOT/build/cache/dm/dmsh" || return 1
  DMX_ROOT="$GAME/RomFS/data" "$PY" "$HERE/dm/tools/dmx.py" list 'data:data/hd/shaders/*.bin' | cut -f1 | while read -r p; do
    DMX_ROOT="$GAME/RomFS/data" "$PY" "$HERE/dm/tools/dmx.py" get "$p" "$(basename "$p")" > /dev/null || exit 1; done
  [ "$(ls | wc -l | tr -d ' ')" = 162 ]
}
[ "$(ls "$DM_ROOT/build/cache/dm/dmsh" 2> /dev/null | wc -l | tr -d ' ')" = 162 ] || step "取着色器包（162 个）" packs

xlate() {
  "$DOTNET" publish "$HERE/dm/gfx/tools/dmsh_xlate" -c Release -o "$DM_ROOT/build/gfx/dmsh_xlate" -v quiet || return 1
  # 编完当场跑一下：起不来（找不到 .NET 运行时等）就在这里把它自己的报错打出来，不留到后面被报成别的事
  "$DM_ROOT/build/gfx/dmsh_xlate/dmsh_xlate" --source-sha > /dev/null 2>&1 && return 0
  echo "  $(m "翻译工具编出来了但起不来，它自己的报错：" "the translation tool was built but does not start; its own error:")"; "$DM_ROOT/build/gfx/dmsh_xlate/dmsh_xlate" --source-sha 2>&1 | head -6 | sed 's/^/    /'
  echo "  $(m "多半是找不到 .NET 运行时：设 DOTNET_ROOT=<.NET 安装目录> 后重跑（Homebrew：export DOTNET_ROOT=\"\$(brew --prefix dotnet)/libexec\"）" "most likely the .NET runtime was not found: set DOTNET_ROOT=<.NET install directory> and rerun (Homebrew: export DOTNET_ROOT=\"\$(brew --prefix dotnet)/libexec\")")"; return 1
}
[ -x "$DM_ROOT/build/gfx/dmsh_xlate/dmsh_xlate" ] || step "编译着色器翻译工具" xlate

# 翻译链版本由源码与工具指纹共同决定。本机工具与仓内登记的不一样（别的 Xcode / spirv-cross 版本、自己编的翻译工具）是正常的：
# 按本机工具重新登记，着色器库就按这套工具出，宿主也只认这一版。
T=$HERE/dm/gfx/tools
"$PY" "$T/dmsh_chain.py" --check-tools > /dev/null 2>&1 || step "按仓内源码重编着色器翻译工具" xlate   # 翻译工具的源码改过（拉了新提交）：旧的二进制不算数
if ! "$PY" "$T/dmsh_chain.py" --check-tools > /dev/null 2>&1; then
  say "本机工具与仓内登记的不同：按本机工具重新登记翻译链"
  "$PY" "$T/dmsh_chain.py" --update-lock > /dev/null && "$PY" "$T/dmsh_chain.py" --header > /dev/null || die "翻译链登记没成功"
fi
V=$("$PY" "$T/dmsh_chain.py")
# 翻译这一步做没做全，只认完成标记里的逐个结果：标记 _b_build.json（写在 Mac 侧缓存目录）里每个着色器都是 ok，并且 iOS 库目录里每个都有对应的库文件。
# 只看「标记在、库目录里至少有一个文件」不够：翻译脚本即使全部失败也会写标记并正常退出，那样会把一个空的或缺了几千个的库当成做完。
# Whether this step is complete is decided only by the per-shader results in the completion marker: every shader in _b_build.json (in the Mac-side cache directory) is ok, and every one has its library file in the iOS library directory.
# "The marker exists and the directory holds at least one file" is not enough: the translation script writes the marker and exits normally even when everything failed, which would accept an empty library or one missing thousands of shaders.
shaders_ok() {   # [say] = 把统计打印出来 | print the tally
  "$PY" - "$DM_ROOT/build/cache/metal_shader_cache/b-$V/_b_build.json" "$DM_ROOT/build/ios/dmsh_b/b-$V" "${1:-}" <<'PYEOF'
import collections, json, os, sys
marker, ios, say = sys.argv[1:4]
try: res = json.load(open(marker, encoding='utf-8'))
except (OSError, ValueError): sys.exit(1)
st = collections.Counter(r['st'] for r in res)
have = set(os.listdir(ios)) if os.path.isdir(ios) else set()
miss = [r['key'] for r in res if r['st'] == 'ok' and r['key'] + '_b.metallib' not in have]
good = bool(res) and set(st) == {'ok'} and not miss
if say:
    print('  着色器 %d：成功 %d%s；iOS 库里缺 %d 个 | shaders %d: ok %d%s; missing from the iOS library: %d' % (
        len(res), st['ok'], ''.join('，%s %d' % kv for kv in st.items() if kv[0] != 'ok'), len(miss),
        len(res), st['ok'], ''.join(', %s %d' % kv for kv in st.items() if kv[0] != 'ok'), len(miss)))
sys.exit(0 if good else 1)
PYEOF
}
shaders() {
  local log="$DM_ROOT/build/cache/dm/shaders.log" rc
  J=${J:-6} IOSOUT="$DM_ROOT/build/ios/dmsh_b" "$PY" "$T/dmsh_build.py" "$DM_ROOT/build/cache/dm/dmsh" "$DM_ROOT/build/cache/metal_shader_cache" > "$log" 2>&1; rc=$?
  tail -12 "$log"
  [ $rc = 0 ] || { echo "  $(m "翻译脚本退出码 ${rc}，完整输出在 ${log}" "the translation script exited with ${rc}; full output in ${log}")"; return 1; }
  shaders_ok say || { echo "  $(m "有着色器没翻译成功或库文件不全，完整输出在 ${log}" "some shaders failed or library files are missing; full output in ${log}")"; return 1; }
}
shaders_ok || step "翻译全部着色器（首次较久）" shaders
# 随着色器库走的清单：哪些着色器的运行时下标没能证明有界（宿主按库版本核对这份清单，版本对不上会报红并退回保守做法）
head -1 "$HERE/dm/gfx/nvn/c1_full.tsv" 2> /dev/null | grep -q "b-$V " || step "出着色器下标清单" "$PY" "$T/c1_guard.py" "$DM_ROOT/build/cache/metal_shader_cache/b-$V" "$HERE/dm/gfx/nvn/c1_full.tsv"

say "工作区就绪：${DM_ROOT}。"
echo "  下一步先进入 setup 准备好的 Python 环境（构建与测试的脚本要用里面装的包）："
echo "      source $DM_ROOT/build/cache/dm/venv/bin/activate"
echo "  然后出 App：cd dm/ios && ./run.sh build（先在 dm/ios/local.env 里填 DM_TEAM_ID 与 DM_BUNDLE_ID；缺省同时面向 iPhone 与 iPad，只要 iPhone 加 DM_DEVICE_FAMILY=1）"
echo "  Next, enter the Python environment setup prepared (the build and test scripts need the packages in it):  source $DM_ROOT/build/cache/dm/venv/bin/activate"
echo "  Then build the app:  cd dm/ios && ./run.sh build   (fill DM_TEAM_ID and DM_BUNDLE_ID in dm/ios/local.env first; it targets both iPhone and iPad by default, add DM_DEVICE_FAMILY=1 for iPhone only)"
echo "  $(m "想在这台 Mac 上直接玩：dm/tools/mac_app.sh 出一个 macOS App（见 docs/MAC.zh-CN.md）" "To play on this Mac directly: dm/tools/mac_app.sh builds a macOS App (see docs/MAC.md)")"
