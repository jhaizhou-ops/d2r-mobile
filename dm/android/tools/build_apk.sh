#!/usr/bin/env bash
# 打 Android 安装包：本树 Java/清单 + 本树 CMake 产物 + 本树改写模块 → 仓库内 dm/android/build/apk/。
# 全部输入输出都在本仓库（build/ 不进版本库）；游戏数据（game.romfs 等）只在部署时从数据区推送，不进包。
set -euo pipefail
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
BUILD=$SRC/dm/android/build
MODULES=${DM_MODULES:-$BUILD/modules-touch}
CMAKE=$BUILD
OUT=$BUILD/apk
# 工具位置：一律先看环境变量，再按各平台常见安装位置探测，都没有就报一句人话停下来。
# 不写死任何人的家目录——这个脚本要能在 Linux、Mac、CI 上跑，写死路径等于只对一台机器有效。
# Tool locations: the environment variable wins first, then the usual install location on each platform is probed, and if
# neither turns one up the script stops with a plain sentence. No machine's home directory is hardcoded -- this script has to
# run on Linux, Mac and CI, and a hardcoded path only ever works on one machine.
# SDK：ANDROID_SDK_ROOT / ANDROID_HOME 都认（前者是官方变量，后者是历史名，很多环境只设后者）。
# SDK: both ANDROID_SDK_ROOT and ANDROID_HOME are honoured (the former is the official variable, the latter the legacy name
# that many environments still set).
SDK=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}
if [ -z "$SDK" ]; then
    for c in "$HOME/Android/Sdk" "$HOME/Library/Android/sdk" "$HOME/Library/Android/Sdk" /usr/local/share/android-sdk /opt/android-sdk; do
        [ -d "$c" ] && { SDK=$c; break; }
    done
fi
[ -n "$SDK" ] && [ -d "$SDK" ] || { echo "找不到 Android SDK：设 ANDROID_SDK_ROOT（或 ANDROID_HOME）指过去" >&2; exit 2; }
# 注意 `|| true`：本脚本是 set -euo pipefail，而 `ls -d <不存在的目录>/*` 会以非零退出；
# 经过管道时 pipefail 会把这个失败带上来，set -e 于是就地退出——连下面那句"找不到 X"都打不出来，
# 只剩一个干巴巴的退出码。加 `|| true` 把失败吃掉，交给下面显式的 [ -n ... ] 检查去报错。
# Note the `|| true`: this script runs under set -euo pipefail, and `ls -d <missing dir>/*` exits non-zero; pipefail carries
# that failure up the pipeline and set -e exits on the spot -- so not even the "missing X" message below gets printed, and all
# that is left is a bare exit code. `|| true` swallows it and leaves the explicit [ -n ... ] check to report the problem.
# 取"最新一版"目录：macOS 自带的是 BSD sort，没有 -V。直接 `sort -V` 在那边会报错退出，管道被 pipefail 带下来，
# 最后只会看到"找不到 build-tools"这种误导性报错。所以先试 GNU 的 sort -V，不支持就用 python3 按数字段比较
# （打包流程本来就要 python3）。
# Newest directory: macOS ships BSD sort without -V. A bare `sort -V` fails there and pipefail turns it into a
# misleading "no build-tools" error. Try GNU sort -V first, otherwise compare numeric chunks in python3 (python3 is
# required by the packaging flow anyway).
pick_latest() {
    if printf '1.0\n' | sort -V >/dev/null 2>&1; then sort -V | tail -1
    else python3 -c 'import re, sys
key = lambda p: [int(x) for x in re.findall(r"[0-9]+", p)]
lines = [l for l in sys.stdin.read().splitlines() if l.strip()]
print(max(lines, key=key) if lines else "")'
    fi
}
BT=$(ls -d "$SDK"/build-tools/* 2>/dev/null | pick_latest || true)
[ -n "$BT" ] && [ -x "$BT/aapt2" ] || { echo "SDK 里没有 build-tools（缺 aapt2）：用 sdkmanager 装一份，SDK=$SDK" >&2; exit 2; }
PLATFORM_JAR=$(ls -d "$SDK"/platforms/android-* 2>/dev/null | pick_latest || true)/android.jar
[ -f "$PLATFORM_JAR" ] || { echo "SDK 里没有 platforms/android-*/android.jar：用 sdkmanager 装一个 platform，SDK=$SDK" >&2; exit 2; }
# JDK：JAVA_HOME 优先；其次用 macOS 的 /usr/libexec/java_home（Apple 自带，能挑到已装的 JDK）；再退到 PATH 上的 javac 反推。
# JDK: JAVA_HOME first; then macOS's /usr/libexec/java_home (shipped by Apple, picks an installed JDK); finally derive it from
# the javac on PATH.
if [ -z "${JAVA_HOME:-}" ] && [ -x /usr/libexec/java_home ]; then
    JAVA_HOME=$(/usr/libexec/java_home 2>/dev/null || true)
fi
if [ -z "${JAVA_HOME:-}" ]; then
    _jc=$(command -v javac 2>/dev/null || true)
    if [ -n "$_jc" ]; then
        # 解符号链接：旧 macOS 的 readlink 没有 -f，所以用 python3 兜底（打包流程本来就要 python3）。
        # Resolve the symlink: old macOS readlink has no -f, so fall back to python3 (the packaging flow needs python3 anyway).
        _real=$(readlink -f "$_jc" 2>/dev/null || python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$_jc")
        JAVA_HOME=$(cd "$(dirname "$_real")/.." && pwd)
    fi
fi
[ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/javac" ] || { echo "找不到 JDK：设 JAVA_HOME 指到 JDK 21+ 的根目录" >&2; exit 2; }
AAPT2="$BT/aapt2"; D8="$BT/d8"; ZIPALIGN="$BT/zipalign"; APKSIGNER="$BT/apksigner"

[ -f "$CMAKE/libd2r_mobile_android.so" ] || { echo "先编 native：cd dm/android/build && ninja" >&2; exit 2; }
[ -f "$MODULES/main.patched.bin" ] || { echo "缺模块产物 ${MODULES}（先跑 dm/android/tools/build_modules.sh）" >&2; exit 2; }

rm -rf "$OUT"; mkdir -p "$OUT"/{classes,dex,gen,assets/modules,stage/lib/arm64-v8a}

# 产物名：d2r-mobile-android-<版本>+<构建号>.apk。两个号都从 AndroidManifest.xml 读（与包内一致，不另立一份），
# 版本名里不适合进文件名的字符（空格、/ 等）换成 -；构建号缺省取当天日期 YYYYMMDD，可用 DM_APK_BUILD 覆盖
# （同一天连打两次就能区分）。中间产物也用同一个基名，免得打包目录里一堆 dodola.*。
# Output name: d2r-mobile-android-<version>+<build>.apk. Both numbers are read from AndroidManifest.xml (the same ones the
# package carries, not a second copy); characters in the version name that do not belong in a file name (spaces, slashes) become
# -; the build number defaults to today's date YYYYMMDD and can be overridden with DM_APK_BUILD (so two builds on the same day
# stay distinguishable). Intermediate files use the same base name instead of littering the output directory with dodola.*
MANIFEST=$SRC/dm/android/AndroidManifest.xml
APK_VER=$(sed -n 's/.*android:versionName="\([^"]*\)".*/\1/p' "$MANIFEST" | head -1)
APK_BUILD=${DM_APK_BUILD:-$(date +%Y%m%d)}
APK_STEM=d2r-mobile-android-$(printf '%s' "${APK_VER:-dev}" | tr ' /' '--')+${APK_BUILD}
APK_NAME=$APK_STEM.apk
echo "产物名: ${APK_NAME}（版本 ${APK_VER}，构建 ${APK_BUILD}）"

# 1) assets: 模块 + 清单 + 底栏 v4 素材（gen_assets 产物）
python3 "$SRC/dm/android/tools/gen_module_manifest.py" "$MODULES" "$OUT/assets"
# 原版衍生资源的根，与 gen_overlay.sh 里同一个（那边生成、这边读）。缺省落在 dm/android/build/gen-assets，不碰 iOS 源码树；
# 老 setups 若已生成在 dm/ios/Gen/assets，显式给 DM_GEN_ASSETS 指过去即可（不改仓库也能接着用）。
# Root of the assets derived from the original game -- the same one gen_overlay.sh uses (it generates, this reads). Defaults to
# dm/android/build/gen-assets and never touches the iOS source tree; a setup that already generated them under dm/ios/Gen/assets can just point
# DM_GEN_ASSETS there (works without any repository change).
GEN_ROOT=${DM_GEN_ASSETS:-$BUILD/gen-assets}
V4=${DM_HUD_V4_DIR:-$GEN_ROOT/NativeHUD/v4/hud_v4_ui}
# 技能图集（TouchArt PNG）——与 gen_overlay.sh 里同一个落点。那份 .c 索引仍在 iOS 源码树原位（两边都编、相对 include）。
# The skill atlas (TouchArt PNGs) -- same location as in gen_overlay.sh. The .c index stays in the iOS source tree (both builds compile it, relative include).
TA=${DM_TOUCH_ART_DIR:-${DM_NATIVE_ART_DIR:-$GEN_ROOT/native_art}}
[ -d "$TA" ] && { mkdir -p "$OUT/assets"; cp "$TA"/*.png "$OUT/assets/"; echo "技能图集: $(ls "$TA"/*.png | wc -l) 张"; } || echo "警告：没有技能图集（TouchArt），技能钮只有文字"
[ -d "$V4" ] && { mkdir -p "$OUT/assets/hud_v4_ui"; cp "$V4"/* "$OUT/assets/hud_v4_ui/"; echo "hud_v4_ui: $(ls "$OUT/assets/hud_v4_ui" | wc -l) 个文件"; } || echo "警告：没有 hud_v4_ui 素材，底栏 v4 不画"
# 新触摸外观的通用嵌槽贴图（上游 46cb318 的新美术，只有 iOS 源码树里这一份）：和技能图集一起放进 assets 根。
# The generic socket texture of the new touch look (new upstream art in 46cb318, kept only in the iOS source tree): stage it in the assets root next to the skill atlases.
[ -f "$SRC/dm/ios/Sources/ControlArt/dm_mobile_socket.png" ] && { mkdir -p "$OUT/assets"; cp "$SRC/dm/ios/Sources/ControlArt/dm_mobile_socket.png" "$OUT/assets/"; echo "触摸外观: dm_mobile_socket.png"; } || echo "警告：没有 dm_mobile_socket.png，技能钮退回纯绘制底（dm_button_draw）"

# 2) 清单 + 资源链接
# 启动图标复用 iOS 的那份美术（dm/ios/Assets.xcassets/AppIcon.appiconset/AppIcon1024.png），构建时缩成 Android 各密度，
# 不另存一套进仓库（同其它原版衍生资源的惯例）。iOS 那份不在就跳过并在最后提示——图标只是外观，不该挡住打包。
# The launcher icon reuses the iOS artwork (dm/ios/Assets.xcassets/AppIcon.appiconset/AppIcon1024.png), scaled to every
# Android density at build time rather than keeping a second copy in the repository (same convention as the other assets
# derived from the original game). If the iOS file is missing, skip it and warn at the end: an icon is only appearance and
# must not block packaging.
APPICON=$SRC/dm/ios/Assets.xcassets/AppIcon.appiconset/AppIcon1024.png
RES=$OUT/res
if [ -f "$APPICON" ]; then
    python3 "$SRC/dm/android/tools/gen_icons.py" "$APPICON" "$RES" || { echo "🔴 图标生成失败" >&2; exit 1; }
else
    echo "警告：缺 iOS 图标 ${APPICON}，APK 将没有启动图标"
fi
# res/ 得先 aapt2 compile 成编译单元再交给 link：`-R` 是"overlay 语义的编译单元"（zip/apk），
# 不收源码目录——直接 -R res 会以 "failed to read magic from input: Is a directory" 失败（真机链路实测）。
# 所以走标准两步：compile --dir → 产出 res.zip，再作为普通输入位置参数交给 link。
# res/ must be compiled into a compilation unit before link: `-R` means "compilation unit with overlay semantics"
# (a zip/apk), and it does not take a source directory -- passing `-R res` fails with "failed to read magic from
# input: Is a directory" (measured on a real packaging run). So use the standard two-step: compile --dir -> res.zip,
# then hand that to link as an ordinary positional input.
AAPT_RES=()
if [ -d "$RES" ]; then
    "$AAPT2" compile --dir "$RES" -o "$OUT/res.zip" || { echo "🔴 资源编译失败" >&2; exit 1; }
    AAPT_RES=("$OUT/res.zip")
fi
"$AAPT2" link -o "$OUT/base.apk" -I "$PLATFORM_JAR" \
    --manifest "$SRC/dm/android/AndroidManifest.xml" -A "$OUT/assets" ${AAPT_RES[@]+"${AAPT_RES[@]}"} --java "$OUT/gen"

# 3) Java → dex
find "$SRC/dm/android/src" "$OUT/gen" -name '*.java' > "$OUT/sources.txt"
"$JAVA_HOME/bin/javac" -encoding UTF-8 -nowarn -Xlint:-options -source 17 -target 17 \
    -classpath "$PLATFORM_JAR" -d "$OUT/classes" @"$OUT/sources.txt"
# 收集 .class 到数组：不用 mapfile（bash 4 才有，macOS 自带的是 3.2，会报 "mapfile: command not found"）。
# 用 while read 从 find 的输出逐行读——路径里没有换行，这种读法对空格是安全的（逐行，不做分词）。
# Collect the .class files into an array without mapfile (bash 4 only; macOS ships 3.2, where it is "command not found").
# Read find's output line by line -- paths contain no newlines, and reading per line is safe with spaces (no word splitting).
CLASSES=()
while IFS= read -r _c; do CLASSES+=("$_c"); done < <(find "$OUT/classes" -name '*.class')
[ "${#CLASSES[@]}" -gt 0 ] || { echo "javac 没有产出任何 .class（检查上面的编译输出）" >&2; exit 1; }
"$D8" --lib "$PLATFORM_JAR" --min-api 29 --output "$OUT/dex" ${CLASSES[@]+"${CLASSES[@]}"}

# 4) 组装
cp "$OUT/base.apk" "$OUT/$APK_STEM.apk"
cp "$OUT/dex/classes.dex" "$OUT/stage/classes.dex"
cp "$CMAKE/libd2r_mobile_android.so" "$OUT/stage/lib/arm64-v8a/"
# ASan 构建时把运行时库一起打进 APK（否则进程加载会因 NEEDED 缺失而起不来）。缺省没有这个文件，跳过。
# An ASan build also ships the sanitizer runtime, otherwise the process cannot resolve the NEEDED lib.
# 从 SDK 里的 NDK 自己找：取最新一版 NDK，主机三元组按当前系统算（linux-x86_64 / darwin-aarch64 …），
# 不再写死某台机器的绝对路径和 NDK 版本号。找不到就跳过——只有 DM_PACK_ASAN=1 时才真的需要它。
# Find it in the SDK's NDK: take the newest NDK and compute the host triple from the current system (linux-x86_64,
# darwin-aarch64, ...), instead of hardcoding one machine's absolute path and NDK version. If it is not there, skip --
# only DM_PACK_ASAN=1 actually needs it.
if [ "${DM_PACK_ASAN:-0}" = "1" ]; then
    NDK=$(ls -d "$SDK"/ndk/* 2>/dev/null | pick_latest || true)   # 同上：没有 ndk/ 目录时别让 pipefail 立刻退出 | same as above: a missing ndk/ must not trip pipefail
    case "$(uname -s)" in
        Darwin) HOST_TAG=darwin-$(uname -m);;
        *)      HOST_TAG=linux-$(uname -m);;
    esac
    ASAN_RT=$(ls "$NDK"/toolchains/llvm/prebuilt/"$HOST_TAG"/lib/clang/*/lib/linux/libclang_rt.asan-aarch64-android.so 2>/dev/null | head -1 || true)   # 同上 | same as above
    if [ -n "$ASAN_RT" ]; then cp "$ASAN_RT" "$OUT/stage/lib/arm64-v8a/"; echo "packed ASan runtime: $(basename "$ASAN_RT")"
    else echo "警告：DM_PACK_ASAN=1 但没找到 ASan 运行时（NDK=${NDK:-未安装}，host=${HOST_TAG}）" >&2; fi
fi
python3 - "$OUT/$APK_STEM.apk" "$OUT/stage" <<'EOF'
import os, sys, zipfile
apk, stage = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk, 'a', zipfile.ZIP_DEFLATED) as z:
    for root, _d, files in os.walk(stage):
        for f in files:
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, stage))
print('added', len(zipfile.ZipFile(apk).namelist()), 'entries total')
EOF
unzip -l "$OUT/$APK_STEM.apk" | grep -E "classes.dex|\.so|AndroidManifest|module-manifest|patched.bin" | head -15

# 5) 对齐 + 签名
"$ZIPALIGN" -f -p 4 "$OUT/$APK_STEM.apk" "$OUT/$APK_STEM.aligned.apk"
KS=${DM_KEYSTORE:-$HOME/.android/debug.keystore}
# 换机器后调试密钥通常还不存在（只有 Android Studio 会建）。缺了就按官方调试密钥的规格现建一个；
# 口令/别名与 Android 的调试约定一致（storepass=keypass=android，alias=androiddebugkey）。
# A fresh machine usually has no debug keystore yet (only Android Studio creates one). Create it with the standard
# debug spec; the password and alias match Android's debug convention (storepass=keypass=android, alias=androiddebugkey).
if [ ! -f "$KS" ] && [ -z "${DM_KEYSTORE:-}" ]; then
    echo "没有调试签名密钥，现建一个：$KS"
    mkdir -p "$(dirname "$KS")"
    "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$KS" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1 \
        || { echo "建调试密钥失败（缺 keytool？）：$KS" >&2; exit 2; }
fi
[ -f "$KS" ] || { echo "缺签名密钥：$KS（设 DM_KEYSTORE 指到你的 keystore）" >&2; exit 2; }
"$APKSIGNER" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --out "$OUT/$APK_NAME" "$OUT/$APK_STEM.aligned.apk"
"$APKSIGNER" verify "$OUT/$APK_NAME" && echo "signed OK"
# 字节数：GNU stat 是 -c%s，BSD/macOS 是 -f%z，两边的 flag 互不认。先试 GNU、失败再试 BSD，
# 都不行就退回 wc -c（到处都能用）。原先写死 -c%s，在 macOS 上这一行会直接报错。
# Byte count: GNU stat uses -c%s, BSD/macOS uses -f%z, and neither accepts the other's flag. Try GNU, then BSD, and fall
# back to wc -c (works everywhere). It used to be hardcoded -c%s, which fails outright on macOS.
APK_BYTES=$(stat -c%s "$OUT/$APK_NAME" 2>/dev/null || stat -f%z "$OUT/$APK_NAME" 2>/dev/null || wc -c < "$OUT/$APK_NAME" | tr -d ' ')
echo "APK: $OUT/$APK_NAME ($APK_BYTES bytes)"
