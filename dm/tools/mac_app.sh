#!/bin/bash
# 把 Mac 宿主包成能双击起的 macOS App：mac_app.sh [输出目录]   产出 <输出目录>/D2R Mobile.app（缺省 $DM_ROOT/out/macapp）
# 包里有：宿主程序、改写后的游戏程序（产物目录，种类 DM_MAC_PRODUCT，缺省 touch）、有界面覆盖层就带上（DM_MAC_OVERLAY=<目录>）。
# DM_MAC_LINK=1：不带派生物的出法——改写后的游戏程序、着色器库、界面覆盖层都是从游戏内容派生的，公开发布的包不能带；
#   这样出的包里只有宿主程序和 Resources/paths.txt（各项指向本机构建产物的路径），只在构建它的这台机器上能跑。
# 版本：给人看的版本号 DM_MAC_VERSION（缺省 1.0，与 iOS 包同叫法）；构建号 DM_MAC_BUILD（缺省取当前月日时分，同 iOS 包格式）。产物代号与提交号写在包里的 Resources/build.txt。
# 更新检查：DM_MAC_UPDATE_URL=<版本文件地址>（如下载站的 latest_mac.json）写进 Info.plist 的 SWUpdateURL，App 启动后读它、比包内构建号新就在主菜单提示；不给 = 不检查（公开版 / 自己构建的包）。
# 游戏资源不进包：第一次启动请玩家选资源文件夹（host/app_mac.m）。签名用本地临时签名，本机直接能跑。
# Wrap the Mac host into a macOS App that starts on double-click: mac_app.sh [output directory]   produces <output directory>/D2R Mobile.app (default $DM_ROOT/out/macapp)
# The package holds: the host program, the rewritten game program (build output directory, kind DM_MAC_PRODUCT, default touch), and the UI overlay when there is one (DM_MAC_OVERLAY=<directory>).
# DM_MAC_LINK=1: build without derived content - the rewritten game program, the shader libraries and the UI overlay are all derived from game content and must not be in a publicly released package;
#   a package built this way holds only the host program and Resources/paths.txt (entries pointing at build outputs on this machine), and runs only on the machine that built it.
# Version: the human-readable version DM_MAC_VERSION (default 1.0, named the same way as the iOS package); the build number DM_MAC_BUILD (default: current month-day-hour-minute, same format as the iOS package). The build output generation and the commit are written to Resources/build.txt in the package.
# Update check: DM_MAC_UPDATE_URL=<version file address> is written into SWUpdateURL in Info.plist; after launch the App reads it and shows a hint on the main menu when it is newer than the build number in the package; not given = no check (public builds / packages you build yourself).
# Game resources do not go into the package: on first launch the player is asked to choose the resource folder (host/app_mac.m). Signing is local ad-hoc signing, which runs directly on this machine.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); DM=$HERE/..; ROOT=${DM_ROOT:-$HOME/dm-work}
OUT=${1:-$ROOT/out/macapp}; KIND=${DM_MAC_PRODUCT:-touch}; BID=${DM_MAC_BUNDLE_ID:-org.d2rmobile.mac}
P=$(bash "$HERE/product.sh" path "$KIND"); [ -f "$P/main.patched.bin" ] || { echo "🔴 没有产物 ${P}（先跑 product.sh build）"; exit 2; }
# DM_MAC_M3=1：老接口版（图形后端不用 Metal 4，最低系统 host/Makefile 的 MACOS_MIN_M3）。包名、显示名、包标识都带 M3，能和正式包并存；存档与日志目录也是另一个
# DM_MAC_M3=1: older-API build (the graphics backend does not use Metal 4; minimum system is MACOS_MIN_M3 in host/Makefile). Package name, display name and bundle identifier all carry M3 so it can coexist with the regular package; the save and log directory is a separate one as well
M3=${DM_MAC_M3:-}; NAME="D2R Mobile${M3:+ M3}"; [ -z "$M3" ] || BID=${DM_MAC_BUNDLE_ID:-org.d2rmobile.mac.m3}
make -C "$DM/host" ${M3:+dmhost_m3} > /dev/null
MIN=$(make -s -C "$DM/host" print-macos-min${M3:+-m3})   # 最低系统版本：与编译宿主用的是同一个数（host/Makefile 的 MACOS_MIN / MACOS_MIN_M3） | minimum system version: the same number the host is compiled with (MACOS_MIN / MACOS_MIN_M3 in host/Makefile)
APP="$OUT/$NAME.app"; rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$DM/host/dmhost${M3:+_m3}" "$APP/Contents/MacOS/D2RMobile"
LINK=${DM_MAC_LINK-1}; [ "$LINK" != 0 ] || LINK=; RES="$APP/Contents/Resources"; : > "$RES/paths.txt"
if [ -n "$LINK" ]; then echo "product=$P" >> "$RES/paths.txt"; else cp -R "$P" "$RES/product"; fi
cp "$DM/gfx/nvn/shader_map.tsv" "$APP/Contents/Resources/shader_map.tsv"
cp "$DM/gfx/nvn/pso_manifest.tsv" "$APP/Contents/Resources/pso_manifest.tsv"   # 管线预热的种子清单：图形后端按「与映射表同目录」找它（两种出法都带；漏带的包开局边画边现编管线——的日志里现编了 102 条） | seed list for pipeline warm-up: the graphics backend looks for it "in the same directory as the mapping table" (both ways of building carry it; a package without it compiles pipelines on the fly while drawing at the start of a game - 102 of them in one log)
[ ! -f "$DM/gfx/nvn/c1_full.tsv" ] || cp "$DM/gfx/nvn/c1_full.tsv" "$APP/Contents/Resources/c1_full.tsv"   # 图形后端按「与映射表同目录」找它 | The graphics backend looks for it "in the same directory as the mapping table"
# 编好的着色器库（Mac 版，dmsh_build.py 出的）：只带运行要用的 .metallib 与反射 .json，源码 .metal 不带
# The compiled shader library (Mac variant, produced by dmsh_build.py): only the .metallib and reflection .json needed at run time are included, not the .metal sources
V=$(sed -n 's/.*DMSH_CHAIN "\(.*\)".*/\1/p' "$DM/gfx/nvn/dmsh_chain.h")
# 老接口版：着色器库也要按它的最低系统重编一份（缓存里那份记的目标是编译机的系统版本，老系统上加载不了）；DM_MSL_DIR 给了就用给的
# Older-API build: the shader libraries must also be recompiled for its minimum system (the ones in the cache record the system version of the build machine as target and cannot be loaded on an older system); DM_MSL_DIR is used when given
[ -z "$M3" ] || [ -n "${DM_MSL_DIR:-}" ] || { bash "$DM/gfx/tools/msl_retarget.sh" "$MIN" "$OUT/msl_min$MIN" > /dev/null || { echo "🔴 着色器库按 macOS ${MIN} 重编失败"; exit 2; }; DM_MSL_DIR=$OUT/msl_min$MIN; }
MSL=${DM_MSL_DIR:-$ROOT/build/cache/metal_shader_cache}/b-$V
[ -d "$MSL" ] || { echo "🔴 没有着色器库 ${MSL}（先跑 dm/gfx/tools/dmsh_build.py）"; exit 2; }
if [ -n "$LINK" ]; then echo "msl=$(dirname "$MSL")" >> "$RES/paths.txt"
else mkdir -p "$RES/msl/b-$V"; rsync -a --include='*.metallib' --include='*.json' --exclude='*' "$MSL/" "$RES/msl/b-$V/"; fi
# 界面覆盖层（自定义页、珍宝匣、秘境等我们的页面都在里面）：从游戏资源现生成（约 3 秒）；DM_MAC_OVERLAY=<目录> 用现成的，=none 不带（原版界面）
# UI overlay (the Custom page, the trove, rifts and our other pages all live in it): generated on the spot from the game resources (about 3 seconds); DM_MAC_OVERLAY=<dir> uses an existing one, =none leaves it out (original UI)
if [ "${DM_MAC_OVERLAY:-}" != none ]; then
  OVD="$RES/romfs_overlay"; [ -z "$LINK" ] || { OVD="$OUT/romfs_overlay.local"; echo "romfs_overlay=$OVD" >> "$RES/paths.txt"; }
  # 基础覆盖层与开关组合成套更新，防止复用输出目录时残留旧组合；外部覆盖层也照带同名兄弟目录。
  # Refresh the base overlay and the feature combinations together, so no stale combination survives when an output directory is reused; an external overlay brings its same-named sibling directory too.
  if [ -n "${DM_MAC_OVERLAY:-}" ]; then
    [ "$(cd "$DM_MAC_OVERLAY" && pwd -P)" != "$(mkdir -p "$OVD" && cd "$OVD" && pwd -P)" ] || { echo "DM_MAC_OVERLAY must differ from the output overlay directory"; exit 2; }
  fi
  rm -rf "$OVD" "${OVD}_variants"
  if [ -n "${DM_MAC_OVERLAY:-}" ]; then
    cp -R "$DM_MAC_OVERLAY" "$OVD"
    [ ! -d "${DM_MAC_OVERLAY%/}_variants" ] || cp -R "${DM_MAC_OVERLAY%/}_variants" "${OVD}_variants"
  else DM_OVERLAY_MAC=1 python3 "$DM/ios/tools/gen_romfs_overlay.py" "${DM_ROMFS_SRC:-$ROOT/data/switch/3.3.94012/Update/RomFS}" "$OVD" > /dev/null || { echo "🔴 界面覆盖层生成失败"; exit 2; }; fi
  [ -z "$LINK" ] || [ ! -d "${OVD}_variants" ] || echo "romfs_overlay_variants=${OVD}_variants" >> "$RES/paths.txt"
fi
# Generated cursor images remain private game-derived resources.
if [ "${DM_OVERLAY_PC:-}" = 1 ]; then
  CURSORS="$RES/cursors"
  if [ -n "$LINK" ]; then
    CURSORS="$OUT/cursors.local"
    [ ! -e "$CURSORS" ] || { echo "Cursor output already exists: $CURSORS"; exit 2; }
    echo "cursors=$CURSORS" >> "$RES/paths.txt"
  fi
  python3 "$DM/ios/tools/gen_mac_cursors.py" "${DM_ROMFS_SRC:-$ROOT/data/switch/3.3.94012/Update/RomFS}" "$CURSORS"
fi
[ -s "$RES/paths.txt" ] || rm -f "$RES/paths.txt"
VER=${DM_MAC_VERSION:-1.0}; BUILD=${DM_MAC_BUILD:-$(date +%m%d%H%M)}
echo "版本 ${VER} 构建 ${BUILD} 产物 $(basename "$P") 提交 $(git -C "$DM" rev-parse --short HEAD 2>/dev/null || echo 未知)" > "$RES/build.txt"
cat > "$APP/Contents/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>$NAME</string>
<key>CFBundleDisplayName</key><string>$NAME</string>
<key>CFBundleIdentifier</key><string>$BID</string>
<key>CFBundleExecutable</key><string>D2RMobile</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>$VER</string>
<key>CFBundleVersion</key><string>$BUILD</string>
<key>LSMinimumSystemVersion</key><string>$MIN</string>
${DM_MAC_UPDATE_URL:+<key>SWUpdateURL</key><string>$DM_MAC_UPDATE_URL</string>
}<key>NSHighResolutionCapable</key><true/>
<key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>
<key>GCSupportsControllerUserInteraction</key><true/>
</dict></plist>
PL
# 自检：管线预热的种子清单在包里、与映射表同目录、不是空的
# Self-check: the seed list for pipeline warm-up is in the package, in the same directory as the mapping table, and not empty
[ -s "$RES/pso_manifest.tsv" ] && [ -s "$RES/shader_map.tsv" ] || { echo "🔴 包里缺管线预热种子清单或着色器映射表（Resources/pso_manifest.tsv、shader_map.tsv）"; exit 3; }
# 老接口版另核：程序本体里不许引用任何 Metal 4 符号
# Extra check for the older-API build: the executable must not reference any Metal 4 symbol
[ -z "$M3" ] || { N4=$(nm "$APP/Contents/MacOS/D2RMobile" | grep -c MTL4 || true); [ "$N4" = 0 ] || { echo "🔴 老接口版的程序本体里还有 ${N4} 个 Metal 4 符号"; exit 3; }; }
# 自检：带进包的着色器库记的目标系统不许高于包声明的最低系统（抽查头、中、尾各一个与一个 _bf；链接式的包不带库，不查）
# Self-check: the target system recorded in the shader libraries carried in the package must not be higher than the minimum system the package declares (spot-check one each from the head, middle and tail plus one _bf; a link-style package carries no libraries and is not checked)
if [ -z "$LINK" ]; then ( cd "$RES/msl/b-$V" && for f in $(ls | grep '_b\.metallib$' | sed -n '1p;1000p;$p') $(ls | grep '_bf\.metallib$' | head -1); do
  LV=$(xcrun -sdk macosx metal-objdump -d "$f" 2>/dev/null | sed -n 's/.*target triple = ".*-apple-macosx\([0-9]*\.[0-9]*\).*/\1/p' | head -1)
  [ -n "$LV" ] && [ "$(printf '%s\n%s\n' "$LV" "$MIN" | sort -V | tail -1)" = "$MIN" ] || { echo "🔴 着色器库 ${f} 的目标系统是 macOS ${LV:-读不到}，高于包声明的最低系统 ${MIN}"; exit 3; }
done ) || exit 3; fi
# 自检：程序本体里记的最低系统版本要与 Info.plist 写的相等（不等时老系统上双击只会得到一句「不能与此版本的 macOS 配合使用」）；只许有 arm64 一个架构
# Self-check: the minimum system version recorded in the executable must equal the one written in Info.plist (if they differ, double-clicking on an older system only yields "cannot be used with this version of macOS"); only the single arm64 architecture is allowed
BIN_MIN=$(otool -l "$APP/Contents/MacOS/D2RMobile" | awk '$1=="minos"{print $2; exit}'); PL_MIN=$(/usr/libexec/PlistBuddy -c "Print :LSMinimumSystemVersion" "$APP/Contents/Info.plist")
[ -n "$BIN_MIN" ] && [ "$BIN_MIN" = "$PL_MIN" ] || { echo "🔴 最低系统版本对不上：程序本体 ${BIN_MIN:-读不到}，Info.plist ${PL_MIN}"; exit 3; }
ARCHS=$(lipo -archs "$APP/Contents/MacOS/D2RMobile"); [ "$ARCHS" = arm64 ] || { echo "🔴 程序本体的架构不是只有 arm64：${ARCHS}"; exit 3; }
codesign --force -s - "$APP" > /dev/null 2>&1 || { echo "🔴 本地签名失败"; exit 3; }
echo "MACAPP ${APP}（${M3:+老接口版，}版本 ${VER} 构建 ${BUILD}，最低系统 macOS ${MIN}，产物 $(basename "$P")，$(du -sh "$APP" | cut -f1)${LINK:+，不带派生物：只能在本机跑}）"
