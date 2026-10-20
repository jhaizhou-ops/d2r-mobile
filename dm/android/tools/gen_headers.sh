#!/usr/bin/env bash
# 从玩家自己的游戏文件生成宿主要的三张表（安卓构建的编译期依赖，不进 git）：
#   dm_hud_exp.h        本级经验门槛表（k_exp[100]）——底栏 v4 的「Lv · %」
#   dm_vault_tab.h      编年史仓库的物品表（dm_vault.c / dm_vault_ui.c）
#   dm_bright_levels.inc 关卡表——3D 画面加亮的规则 ②/③（洞穴、墓穴这类迷宫关一律加满；见 dm/host/dm_bright.c）。
#   前两张落 build/generated/；第三张必须落 dm/host/：dm_bright.c 按同目录相对包含它（与 Mac 的 Makefile 同一个落点），
#   这样进洞时规则 ② 才命中。早先树里放的空占位会挡住 Makefile 的「已是最新」判断，已从仓库移除。
# 输入 = Switch 版 3.3.94012 的 Update/RomFS 目录（要含 data/build_manifest.psv）；环境变量 DM_ROMFS_SRC 可指。
# Generate the host's lookup tables from the player's own game files (build-time compile dependencies for Android, not in git):
#   dm_hud_exp.h        experience thresholds per level (bottom bar v4's 「Lv · %」)
#   dm_vault_tab.h      the Chronicle vault item table (dm_vault.c / dm_vault_ui.c)
#   dm_bright_levels.inc the level table behind rules (2)/(3) of the 3D picture brightening (maze levels such as caves and crypts are always
#                        fully brightened; see dm/host/dm_bright.c). The first two land in build/generated/; the third must land in dm/host/,
#                        because dm_bright.c includes it relative to its own directory (the same target as the Mac Makefile). This is what makes
#                        rule (2) hit when entering a cave. The empty placeholder that used to sit in the tree also made the Makefile consider the
#                        target up to date, so it has been removed from the repository.
# Input = the Update/RomFS directory of Switch 3.3.94012 (must contain data/build_manifest.psv); DM_ROMFS_SRC overrides it.
set -euo pipefail
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
ROMFS=${DM_ROMFS_SRC:-${DM_ROOT:-$HOME/dm-work}/data/switch/3.3.94012/Update/RomFS}
OUT=${1:-$SRC/dm/android/build/generated}
PY=${DM_PY:-${DM_ROOT:-$HOME/dm-work}/build/cache/dm/venv/bin/python3}
[ -x "$PY" ] || PY=python3
if [ ! -f "$ROMFS/data/build_manifest.psv" ]; then
    for alt in "$HOME/dm-work/data/switch/3.3.94012/RomFS-full" "$HOME/dm-work/data/switch/3.3.94012/Update/RomFS"; do
        [ -f "$alt/data/build_manifest.psv" ] && ROMFS=$alt && break
    done
fi
[ -f "$ROMFS/data/build_manifest.psv" ] || { echo "🔴 找不到游戏文件（$ROMFS/data/build_manifest.psv）：设 DM_ROMFS_SRC 指向 3.3.94012 的 Update/RomFS" >&2; exit 1; }
mkdir -p "$OUT"
# 表名、生成器、落点：落点分开是因为 dm_bright_levels.inc 只能生成到 dm/host（见文件头） | table, generator, destination;
# the destinations differ because dm_bright_levels.inc can only be generated into dm/host (see the file header)
gen() {
    local h=$1 g=$2 d=$3
    if [ -f "$d/$h" ] && [ "$d/$h" -nt "$g" ]; then echo "== $h 已是最新"; return; fi
    "$PY" "$g" "$ROMFS" > "$d/$h.tmp" && mv "$d/$h.tmp" "$d/$h"
    echo "== $h 生成（$(basename "$g")）"
}
gen dm_hud_exp.h "$SRC/dm/ios/tools/gen_hud_exp.py" "$OUT"
gen dm_vault_tab.h "$SRC/dm/ios/tools/gen_vault_table.py" "$OUT"
gen dm_bright_levels.inc "$SRC/dm/tools/gen_bright_levels.py" "$SRC/dm/host"
ls -la "$OUT"