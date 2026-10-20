#!/usr/bin/env bash
# 装机 + 数据目录检查 + 启动。APK 来自本仓库 dm/android/build/apk/；
# game.romfs / 着色器缓存是玩家自己的大数据（不进仓库），从数据区推送一次后不再推。
set -euo pipefail
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
ADB=${ADB:-$HOME/Android/Sdk/platform-tools/adb}
PKG=org.d2r.binaryprobe
DATA=${DM_DATA:-${DM_ROOT:-$HOME/dm-work}/build/dodola-data}
APK=${1:-}
# 不给参数就取打包目录里最新的那个 d2r-mobile-android-*.apk。产物名带版本号和构建号，没法在脚本里写死一个路径；
# 按修改时间取最新，正好对上"刚打完包就装机"这个用法。找不到就报清楚。
# With no argument, take the newest d2r-mobile-android-*.apk in the output directory. The name carries a version and a build
# number, so a literal path cannot be hardcoded; taking the most recently modified file matches "package, then install".
# If there is none, say so plainly.
if [ -z "$APK" ]; then
  APK=$(ls -t "$SRC"/dm/android/build/apk/d2r-mobile-android-*.apk 2>/dev/null | head -1 || true)
  [ -n "$APK" ] || { echo "缺 APK：$SRC/dm/android/build/apk/ 下没有 d2r-mobile-android-*.apk（先跑 tools/build_apk.sh）" >&2; exit 2; }
fi
EXT=/sdcard/Android/data/$PKG/files

[ -f "$APK" ] || { echo "缺 APK：$APK" >&2; exit 2; }
echo "== 安装 $APK"
"$ADB" install -r -t "$APK"
"$ADB" shell mkdir -p "$EXT"

REMOTE=$("$ADB" shell "stat -c%s $EXT/game.romfs 2>/dev/null || echo 0" | tr -d '\r')
if [ "$REMOTE" != "8973693207" ] || [ "${FORCE_ROMFS:-0}" = 1 ]; then
    echo "== 推 game.romfs（本地 $(stat -c%s "$DATA/game.romfs") 字节，远端 $REMOTE）"
    "$ADB" push "$DATA/game.romfs" "$EXT/game.romfs"
else
    echo "== game.romfs 已在位"
fi

SH=$("$ADB" shell "stat -c%s $EXT/d2r-mobile/shaders/shader_index.bin 2>/dev/null || echo 0" | tr -d '\r')
if [ "$SH" = "0" ] || [ "${FORCE_SHADERS:-0}" = 1 ]; then
    echo "== 推着色器缓存"
    tar -C "$DATA" --format=ustar --transform 's,^shader-cache,d2r-mobile/shaders,' -cf /tmp/shaders.tar shader-cache
    "$ADB" push /tmp/shaders.tar "$EXT/shaders.tar"
    "$ADB" shell "cd $EXT && rm -rf d2r-mobile/shaders && tar xf shaders.tar && rm -f shaders.tar && echo extracted \$(ls d2r-mobile/shaders | wc -l) files"
else
    echo "== 着色器缓存已在位"
fi

echo "== 启动"
"$ADB" shell am force-stop $PKG
"$ADB" shell am start -n $PKG/.GameActivity
