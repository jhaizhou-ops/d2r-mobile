#!/usr/bin/env bash
# 世界档矩阵：一条命令跑完 {出图 2168 / 2624} × {3D 比例 1.0 / 0.75} 四档，各自截图 + 取日志 +
# 自动几何判定（ui_geom.py）。判据：同一出图档，开 3D 比例前后界面几何必须一致（scale≈1.000）。
#
# World-tier matrix: one command runs the four configs {output 2168 / 2624} × {3D scale 1.0 / 0.75},
# screenshotting and log-lining each, then auto-judging UI geometry (ui_geom.py). Rule: within one
# output tier the UI geometry must be identical with and without the 3D scale (scale≈1.000).
#
# 前置：模块已重出（patch_nso 4 条分辨率链补丁进 Android 产物：0x211da30 / 0xe10e48 / 0xe12e88 / 0xb1a660）
# Requires: modules rebuilt with the four resolution-chain patches enabled for Android.
set -u
ADB=${ADB:-/home/vick/Android/Sdk/platform-tools/adb}
PKG=org.d2r.binaryprobe
DIAG=/sdcard/Android/data/$PKG/files/d2r-mobile/diagnostics
LOG=$DIAG/game-startup.log
OUT=${OUT:-/home/vick/dm-work/build/perf/matrix-$(date +%m%d-%H%M)}
LOAD=${LOAD:-100}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$OUT"

flag()   { for t in 1 2 3; do $ADB shell "touch $DIAG/$1; chmod 664 $DIAG/$1"; $ADB shell "[ -f $DIAG/$1 ] && echo ok" | grep -q ok && return 0; sleep 1; done; echo "【红】$1 创建失败"; return 1; }
unflag() { $ADB shell "rm -f $DIAG/$1"; }
clear_world() { unflag flag-gameres-native; unflag flag-gameres-2624; unflag flag-scale-70; unflag flag-scale-75; unflag flag-scale-80; unflag flag-scale-85; unflag flag-3d-ref-native; unflag flag-3d-ref-2624; unflag flag-3d-scale-dyn; }

run_cfg() {  # $1 = 名称, $2... = flag 列表
  local name=$1; shift
  echo "================ $name ================"
  clear_world
  local f; for f in "$@"; do flag "$f"; done
  $ADB shell "am force-stop $PKG"; sleep 1
  $ADB shell "am start -n $PKG/.GameActivity" >/dev/null
  echo "加载 ${LOAD}s..."
  sleep "$LOAD"
  $ADB exec-out screencap -p > "$OUT/$name.png"
  $ADB shell "grep -E 'ANDROID_3D_SCALE_CONFIG|ANDROID_3D_SCALE_(BEGIN|END)|界面画面' $LOG" | tr -d '\r' > "$OUT/$name.log"
  local alive fatal
  alive=$($ADB shell "ps -A | grep -c binaryprobe" | tr -d '\r')
  fatal=$($ADB shell "grep -c ANDROID_FATAL $LOG" | tr -d '\r')
  echo "存活=$alive FATAL=$fatal"; cat "$OUT/$name.log"
  echo "$name: 存活=$alive FATAL=$fatal" >> "$OUT/summary.txt"
  cat "$OUT/$name.log" >> "$OUT/summary.txt"; echo >> "$OUT/summary.txt"
}

echo "输出目录 $OUT"
run_cfg base2168  flag-gameres-native
run_cfg base2624  flag-gameres-2624
run_cfg w2168_75  flag-gameres-native flag-scale-75
run_cfg w2624_75  flag-gameres-2624 flag-scale-75
run_cfg base1080  flag-gameres-1080
run_cfg w1080_80  flag-gameres-1080 flag-scale-80

echo "================ 几何判定（同出图档，开比例前后应 scale≈1.000）================"
PY=${DM_PY:-/home/vick/dm-work/build/cache/dm/venv/bin/python3}; [ -x "$PY" ] || PY=python3
for pair in "base2168 w2168_75" "base2624 w2624_75" "base1080 w1080_80"; do
  set -- $pair
  echo "--- $1 vs $2 ---"
  $PY "$HERE/ui_geom.py" "$OUT/$2.png" "$OUT/$1.png" || true
done
echo "--- 跨出图档参考（屏上段宽比≈出图宽之反比：2624 档 vs 2168 档 ≈ 2168/2624 = 0.826；
     两档画布像素一致、2168 档屏上被出图→交换链横向拉伸 ×1.21——见 perf-notes §7.4）---"
$PY "$HERE/ui_geom.py" "$OUT/base2624.png" "$OUT/base2168.png" || true
echo
echo "游戏停在最后一档（w1080_80）上，可肉眼复核；截图在 $OUT/"
echo "MATRIX-DONE"
