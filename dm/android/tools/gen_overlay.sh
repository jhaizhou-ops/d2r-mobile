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

# 原版衍生资源（底部 UI v4、加宽储藏箱、百宝囊框、选角面板、小地图终点…）的落点：dm/android/build/gen-assets，
# 不再写 iOS 的源码树。gen_paths.gen_dir() 认这个环境变量（iOS 出包不设，缺省仍是 dm/ios/Gen/assets）。
# gen_overlay.sh 与 build_apk.sh 必须用同一个根——前者生成、后者读 hud_v4_ui，根不一样就找不到。
# Where assets derived from the original game go (bottom UI v4, widened stash, pouch frame, character select panel, minimap goal...):
# dm/android/build/gen-assets, so the Android build no longer writes into the iOS source tree. gen_paths.gen_dir() honours this environment
# variable (the iOS package does not set it; its default stays dm/ios/Gen/assets). gen_overlay.sh and build_apk.sh must agree on this root --
# the former generates, the latter reads hud_v4_ui, and a mismatch means it is not found.
export DM_GEN_ASSETS=${DM_GEN_ASSETS:-$SRC/dm/android/build/gen-assets}
# 技能图集（TouchArt PNG）与技能索引那份 .c 同样不写 iOS 源码树：两个落点都由生成器按环境变量认，
# 缺省都是原位（iOS 出包不设这两个变量，行为逐字不变）。
# The skill atlas (TouchArt PNGs) and the .c skill index likewise stop writing into the iOS source tree: the generator honours both
# locations through environment variables, both defaulting to the original places (the iOS package sets neither, behaviour unchanged).
export DM_NATIVE_ART_DIR=${DM_NATIVE_ART_DIR:-$DM_GEN_ASSETS/native_art}
export DM_NATIVE_ART_C=$DM_GEN_ASSETS/native_art/dm_touch_native_art.c
# 那份 .c 里的相对 include（../../host/dm_input.h，按 Sources/ 的深度写的）由生成器换成走 include 路径的写法——
# dm/host 在两边都在 include 路径里，逐字同义，Android 那份因此能独立编译。
# The .c's relative include (../../host/dm_input.h, written for the depth of Sources/) is rewritten by the generator to go through the include
# path -- dm/host is in the include path on both sides, so it is exactly equivalent and Android's copy compiles on its own.
# 两个头文件在仓库里、是同目录引用，一并复制过来，CMake 就不必再去 iOS 树里找（dm_touch_game_state.h 只含系统头，自包含）。
# Both headers are in the repository and are same-directory includes, so copy them along and CMake need not look into the iOS tree for them
# (dm_touch_game_state.h has system headers only, self-contained).
NAT=$DM_GEN_ASSETS/native_art; mkdir -p "$NAT"
cp "$SRC/dm/ios/Sources/dm_touch_native_art.h" "$SRC/dm/ios/Sources/dm_touch_game_state.h" "$NAT/"
echo "原版衍生资源落点：$DM_GEN_ASSETS（不写 iOS 源码树）"
echo "技能图集 / 索引：$DM_NATIVE_ART_DIR"

# 部分工具按 clang 这个名字调编译器，而实际只有 gcc：临时目录里放个垫片（clang → gcc）。
# /tmp 会被清掉，所以每次现建，不留进仓库。
# Some tools invoke the compiler under the name clang while only gcc is installed: put a shim in a temporary directory (clang -> gcc).
# /tmp gets wiped, so it is recreated every run and nothing is kept in the repository.
SHIM=$(mktemp -d); trap 'rm -rf "$SHIM"' EXIT
printf '#!/bin/sh\nexec gcc "$@"\n' > "$SHIM/clang"; chmod +x "$SHIM/clang"
export PATH="$SHIM:$PATH"

"$PY" "$SRC/dm/ios/tools/gen_romfs_overlay.py" "$ROMFS" "$OUT"

[ -f "$DM_NATIVE_ART_C" ] || { echo "技能索引 .c 没生成：$DM_NATIVE_ART_C" >&2; exit 1; }

# 生成器要求每套 overlay 都有这份配置（run.sh:69-72 同款自检）
[ -f "$OUT/data/.build.config" ] || { echo "缺 $OUT/data/.build.config" >&2; exit 1; }
echo "overlay 出好了：$OUT（DM_TOUCH_TWO_CARDS=1，模式页只留标准 / 专家两块方块）"
echo "推送到设备：adb push $OUT/. /sdcard/Android/data/org.d2r.binaryprobe/files/d2r-mobile/diagnostics/overlay/"