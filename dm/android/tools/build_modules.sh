#!/usr/bin/env bash
# 用本树改写器出 Android 游戏模块（默认 touch 产物 = 与 iOS 同一规则域：跑步补丁 + 私有门 6-22）。
# 输出：$REPO/dm/android/build/modules-touch/{main,sdk,subsdk0}.{patched.bin,fixups.tsv,gates.tsv,...}
set -euo pipefail
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
NSO=${DM_NSO_DIR:-${DM_ROOT:-$HOME/dm-work}/data/switch/3.3.94012/Update/ExeFS}
OUT=${1:-$SRC/dm/android/build/modules-touch}
PY=${DM_PY:-${DM_ROOT:-$HOME/dm-work}/build/cache/dm/venv/bin/python3}
[ -x "$PY" ] || PY=python3
mkdir -p "$OUT"
for m in main sdk subsdk0; do
    echo "== $m（DM_RULESET=${DM_RULESET:-touch-run-v4} DM_TARGET=${DM_TARGET:-android}）"
    env DM_RULESET=${DM_RULESET:-touch-run-v4} DM_TARGET=${DM_TARGET:-android} "$PY" "$SRC/dm/tools/patch_nso.py" "$NSO/$m" "$OUT" "$m" 2>&1 | tail -2
done
ls -la "$OUT"
