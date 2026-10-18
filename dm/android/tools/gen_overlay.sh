#!/usr/bin/env bash
# 出 Android 的 RomFS 覆盖层（dm/android/build/romfs-overlay），之后 adb push 到
# <files>/d2r-mobile/diagnostics/overlay（hle_fs.c 分层查找，先查这层再查基底）。
#
# 生成器本身是共用的（dm/ios/tools/gen_romfs_overlay.py → touch_layout.py），这一层只做两件事：
#   1) 打开 Android 专属的布局开关 DM_TOUCH_TWO_CARDS —— 建角模式页只留标准 / 专家两块方块 + 极乐世界那个 ☑，
#      「资料片 / 经典版」那一行挪出屏幕。iOS 出包不带这个变量，那边布局逐字不变（共享文件里的加法式平台开关，
#      方案文档 §3.2 第 2 条）。开关集中在这里，别散到别处去。
#   2) 选对解释器（要 numpy / Pillow 的那个 venv）与 clang shim（部分工具用 clang 的名字调 gcc）。
#
# The generator itself is shared (dm/ios/tools/gen_romfs_overlay.py → touch_layout.py); this wrapper only does two things:
#   1) Turn on the Android-only layout switch DM_TOUCH_TWO_CARDS -- the character creation mode page keeps just the two Standard / Hardcore cards
#      plus the Elysium ☑, and the 「Expansion / Classic」 row is moved off screen. The iOS package does not set this variable, so its layout is
#      unchanged word for word (an additive platform switch in a shared file, plan document §3.2 rule 2). The switch lives here and nowhere else.
#   2) Pick the right interpreter (the venv that has numpy / Pillow) and the clang shim (some tools invoke gcc under the name clang).
set -euo pipefail
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
ROMFS=${DM_ROMFS_SRC:-${DM_ROOT:-$HOME/dm-work}/data/switch/3.3.94012/Update/RomFS}
OUT=${1:-$SRC/dm/android/build/romfs-overlay}
PY=${DM_PY:-${DM_ROOT:-$HOME/dm-work}/build/cache/dm/venv/bin/python3}
[ -x "$PY" ] || PY=python3

mkdir -p "$OUT"
export DM_TOUCH_TWO_CARDS=1

# 部分工具按 clang 这个名字调编译器，而实际只有 gcc：临时目录里放个垫片（clang → gcc）。
# /tmp 会被清掉，所以每次现建，不留进仓库。
# Some tools invoke the compiler under the name clang while only gcc is installed: put a shim in a temporary directory (clang -> gcc).
# /tmp gets wiped, so it is recreated every run and nothing is kept in the repository.
SHIM=$(mktemp -d); trap 'rm -rf "$SHIM"' EXIT
printf '#!/bin/sh\nexec gcc "$@"\n' > "$SHIM/clang"; chmod +x "$SHIM/clang"
export PATH="$SHIM:$PATH"

"$PY" "$SRC/dm/ios/tools/gen_romfs_overlay.py" "$ROMFS" "$OUT"

# 生成器要求每套 overlay 都有这份配置（run.sh:69-72 同款自检）
[ -f "$OUT/data/.build.config" ] || { echo "缺 $OUT/data/.build.config" >&2; exit 1; }
echo "overlay 出好了：$OUT（DM_TOUCH_TWO_CARDS=1，模式页只留标准 / 专家两块方块）"
echo "推送到设备：adb push $OUT/. /sdcard/Android/data/org.d2r.binaryprobe/files/d2r-mobile/diagnostics/overlay/"