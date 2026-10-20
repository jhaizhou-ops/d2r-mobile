#!/usr/bin/env bash
# 批量扫附件覆盖率：对每个关卡 ID 跑一次「表关 + 探针」，把该关的 pass-coverage.tsv 拉回本地。
# 扫完用 dm/android/tools/coverage_scan_report.py 合并，找出种子里被证伪的 green。
#
# 用法：
#   LEVELS="5 6 7 10" dm/android/tools/scan_coverage.sh
#   ANDROID_SERIAL=192.168.31.222:5555 LEVELS="3 5 6" dm/android/tools/scan_coverage.sh
#
# 说明：
# - 表关（DM_ATT_LOAD_TABLE=0）是为了让探针测「真实负载下的覆盖率」，不会被查表决策干扰；
# - 每关等 WAIT 秒（缺省 60），让探针至少跑出一个统计窗口并落盘；
# - 探针在区域加载/splash 上偶发卡住（日志停在 ~7 个探针），脚本照常拉日志并继续；
#   卡住的那关可以重跑，或从结果里剔除；
# - 探针很慢（7–18 fps），这是诊断工具，不要拿它当正常游玩路径。
set -euo pipefail
ADB=${ADB:-adb}
if [ -n "${ANDROID_SERIAL:-}" ]; then ADB="$ADB -s $ANDROID_SERIAL"; fi
PKG=${DM_PKG:-org.d2r.binaryprobe}
EXT=/sdcard/Android/data/$PKG/files
D=$EXT/d2r-mobile/diagnostics
OUT=${OUT:-coverage-scan}
WAIT=${WAIT:-60}
LEVELS=${LEVELS:-5 6 7 10 11 12 13 14 15 18 20 21 22 23 40 41 42 46 47 48 49 50 75 76 77 78 79 80 100 101 102 103 104 105 109 110 111 112 113 114 115}
mkdir -p "$OUT"
: > "$OUT/results.tsv"

for lv in $LEVELS; do
    echo "=== level $lv $(date +%H:%M:%S)"
    cat > "$OUT/env.tmp" <<EOF
DM_FE_AUTO=1
DM_UI_TAPS_AT=OpenPanel:CharacterSelectPanel
DM_DBG_FE_SEND=6:CharacterSelect:LoadCharacter:0
DM_ATT_LOAD_TABLE=0
DM_PASS_SENTINEL=1
DM_PASS_SENTINEL_WINDOW=1
DM_PASS_SENTINEL_BUDGET_MB=2
DM_TEST_WARP_LV=200:$lv
EOF
    $ADB push "$OUT/env.tmp" "$D/env.txt" >/dev/null
    # 覆盖率表必须可读（660 会让游戏能 stat 却读不到内容）；日志截断失败也无妨，只是多几行旧记录
    $ADB shell "chmod 664 $D/env.txt; rm -f $D/pass-coverage.tsv; truncate -s 0 $D/game-startup.log" >/dev/null 2>&1 || true
    $ADB shell am force-stop "$PKG"
    $ADB shell am start -n "$PKG/.GameActivity" >/dev/null
    sleep "$WAIT"
    $ADB pull "$D/game-startup.log" "$OUT/lv${lv}.log" >/dev/null 2>&1 || true
    $ADB pull "$D/pass-coverage.tsv" "$OUT/lv${lv}.tsv" >/dev/null 2>&1 || true
    probes=$(grep -a -c "PASS_SENTINEL_PROBE" "$OUT/lv${lv}.log" 2>/dev/null || true)
    conflicts=$(grep -a -c "COV_CONFLICT" "$OUT/lv${lv}.log" 2>/dev/null || true)
    reds=$(grep -a -c "COV_RED" "$OUT/lv${lv}.log" 2>/dev/null || true)
    printf "%s\t%s\t%s\t%s\n" "$lv" "$probes" "$conflicts" "$reds" >> "$OUT/results.tsv"
    echo "level $lv probes=$probes conflicts=$conflicts reds=$reds"
    $ADB shell am force-stop "$PKG"
    sleep 2
done
echo "DONE: $OUT（把 $OUT/lv*.tsv 交给 coverage_scan_report.py 合并）"
