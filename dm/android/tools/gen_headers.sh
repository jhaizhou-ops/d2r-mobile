#!/usr/bin/env bash
# 从玩家自己的游戏文件生成宿主要的三张表（安卓构建的编译期依赖，落在 build/generated/，不进 git）：
#   dm_hud_exp.h   本级经验门槛表（k_exp[100]）——底栏 v4 的「Lv · %」
#   dm_vault_tab.h 编年史仓库的物品表（dm_vault.c / dm_vault_ui.c）
#   （dm_bright_levels.inc 早已在树里，编译器按 dm/host 的相对路径找）
# 输入 = Switch 版 3.3.94012 的 Update/RomFS 目录（要含 data/build_manifest.psv）；环境变量 DM_ROMFS_SRC 可指。
# Generate the host's lookup tables from the player's own game files (build-time compile dependencies for Android, in build/generated/, not in git):
#   dm_hud_exp.h   experience thresholds per level (bottom bar v4's 「Lv · %」)
#   dm_vault_tab.h the Chronicle vault item table (dm_vault.c / dm_vault_ui.c)
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
for t in "dm_hud_exp.h:gen_hud_exp.py" "dm_vault_tab.h:gen_vault_table.py"; do
    h=${t%%:*}; g=${t##*:}
    if [ -f "$OUT/$h" ] && [ "$OUT/$h" -nt "$SRC/dm/ios/tools/$g" ]; then echo "== $h 已是最新"; continue; fi
    "$PY" "$SRC/dm/ios/tools/$g" "$ROMFS" > "$OUT/$h.tmp" && mv "$OUT/$h.tmp" "$OUT/$h"
    echo "== $h 生成（$g）"
done
ls -la "$OUT"