#!/usr/bin/env bash
# 打 Android 安装包：本树 Java/清单 + 本树 CMake 产物 + 本树改写模块 → 仓库内 dm/android/build/apk/。
# 全部输入输出都在本仓库（build/ 不进版本库）；游戏数据（game.romfs 等）只在部署时从数据区推送，不进包。
set -euo pipefail
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
BUILD=$SRC/dm/android/build
MODULES=${DM_MODULES:-$BUILD/modules-touch}
CMAKE=$BUILD
OUT=$BUILD/apk
SDK=${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}
BT=$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)
PLATFORM_JAR=$(ls -d "$SDK"/platforms/android-* | sort -V | tail -1)/android.jar
JAVA_HOME=${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk}
AAPT2="$BT/aapt2"; D8="$BT/d8"; ZIPALIGN="$BT/zipalign"; APKSIGNER="$BT/apksigner"

[ -f "$CMAKE/libd2r_mobile_android.so" ] || { echo "先编 native：cd dm/android/build && ninja" >&2; exit 2; }
[ -f "$MODULES/main.patched.bin" ] || { echo "缺模块产物 $MODULES（先跑 dm/android/tools/build_modules.sh）" >&2; exit 2; }

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
echo "产物名: $APK_NAME（版本 $APK_VER，构建 $APK_BUILD）"

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
    echo "警告：缺 iOS 图标 $APPICON，APK 将没有启动图标"
fi
AAPT_RES=()
[ -d "$RES" ] && AAPT_RES=(-R "$RES")
"$AAPT2" link -o "$OUT/base.apk" -I "$PLATFORM_JAR" \
    --manifest "$SRC/dm/android/AndroidManifest.xml" -A "$OUT/assets" "${AAPT_RES[@]}" --java "$OUT/gen"

# 3) Java → dex
find "$SRC/dm/android/src" "$OUT/gen" -name '*.java' > "$OUT/sources.txt"
"$JAVA_HOME/bin/javac" -encoding UTF-8 -nowarn -Xlint:-options -source 17 -target 17 \
    -classpath "$PLATFORM_JAR" -d "$OUT/classes" @"$OUT/sources.txt"
mapfile -t CLASSES < <(find "$OUT/classes" -name '*.class')
"$D8" --lib "$PLATFORM_JAR" --min-api 29 --output "$OUT/dex" "${CLASSES[@]}"

# 4) 组装
cp "$OUT/base.apk" "$OUT/$APK_STEM.apk"
cp "$OUT/dex/classes.dex" "$OUT/stage/classes.dex"
cp "$CMAKE/libd2r_mobile_android.so" "$OUT/stage/lib/arm64-v8a/"
# ASan 构建时把运行时库一起打进 APK（否则进程加载会因 NEEDED 缺失而起不来）。缺省没有这个文件，跳过。
# An ASan build also ships the sanitizer runtime, otherwise the process cannot resolve the NEEDED lib.
ASAN_RT=$(ls /home/vick/Android/Sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/lib/clang/*/lib/linux/libclang_rt.asan-aarch64-android.so 2>/dev/null | head -1)
if [ -n "$ASAN_RT" ] && [ "${DM_PACK_ASAN:-0}" = "1" ]; then cp "$ASAN_RT" "$OUT/stage/lib/arm64-v8a/"; echo "packed ASan runtime: $(basename "$ASAN_RT")"; fi
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
"$APKSIGNER" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --out "$OUT/$APK_NAME" "$OUT/$APK_STEM.aligned.apk"
"$APKSIGNER" verify "$OUT/$APK_NAME" && echo "signed OK"
# 字节数：GNU stat 是 -c%s，BSD/macOS 是 -f%z，两边的 flag 互不认。先试 GNU、失败再试 BSD，
# 都不行就退回 wc -c（到处都能用）。原先写死 -c%s，在 macOS 上这一行会直接报错。
# Byte count: GNU stat uses -c%s, BSD/macOS uses -f%z, and neither accepts the other's flag. Try GNU, then BSD, and fall
# back to wc -c (works everywhere). It used to be hardcoded -c%s, which fails outright on macOS.
APK_BYTES=$(stat -c%s "$OUT/$APK_NAME" 2>/dev/null || stat -f%z "$OUT/$APK_NAME" 2>/dev/null || wc -c < "$OUT/$APK_NAME" | tr -d ' ')
echo "APK: $OUT/$APK_NAME ($APK_BYTES bytes)"
