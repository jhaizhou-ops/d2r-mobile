#!/bin/bash
# 纯规则域宿主闸门：回放 V4 真值到指定拍号，逐拍六组状态对拍。
# 逐拍逻辑与每拍出图帧数无关（实测见 logicself.sh 注释），所以回放步长与产品 60 帧/秒不同不影响本闸门
# 用法：rulegate.sh [拍数=400] [产物=out/<当前代号>-rules]
# Pure rule-domain host gate check: replay the V4 ground truth up to the given tick number and compare six groups of state tick by tick.
# Per-tick logic is independent of the number of presented frames per tick (measurements in the logicself.sh comments), so a replay step different from the product's 60 fps does not affect this gate check
# Usage: rulegate.sh [ticks=400] [build output=out/<current codename>-rules]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
X=${DM_ROOT:-$HOME/dm-work}
PY=${DM_PY:-$X/build/cache/dm/venv/bin/python3}
TR=${DM_RULE_TRUTH:-$X/data/switch/gate/rulegate_truth_war13_r44}   # 现役真值：data/ 下只读目录，README 有生成命令；不放 run/（会被当临时产物清掉，事故） | Live truth data: a read-only directory under data/ whose README has the generating command; not under run/ (it was wiped there as a temporary output once)
N=${1:-400}
OUT_ARG=${2:-out/$(bash "$(dirname "$0")/../tools/product.sh" name rules)}
case "$N" in ''|*[!0-9]*) echo "RULEGATE RED 拍数非法：$N"; exit 2;; esac
case "$OUT_ARG" in /*) OUT=$OUT_ARG;; *) OUT=$X/build/cache/dm/$OUT_ARG;; esac
[ -d "$TR" ] || { echo "RULEGATE RED 真值目录不存在：$TR"; exit 2; }
[ -f "$TR/meta.json" ] && [ -f "$TR/c2s.jsonl" ] && [ -f "$TR/ticks.jsonl" ] || { echo "RULEGATE RED 真值文件不全"; exit 2; }
[ -f "$OUT/main.patched.bin" ] || { echo "RULEGATE RED 产物缺 main.patched.bin：$OUT"; exit 2; }
PATCH_SHA=$(shasum -a 256 "$HERE/../tools/patch_nso.py" | awk '{print $1}')
REC_SHA=$(sed -n '1s/^[[:space:]]*\([0-9a-fA-F]\{64\}\).*/\1/p' "$OUT/build_src.sha256" 2>/dev/null)
[ -n "$REC_SHA" ] && [ "$REC_SHA" = "$PATCH_SHA" ] || {
    echo "RULEGATE RED 产物过期，先重出当前代号的 rules 产物（dm/PRODUCT）"
    echo "重出命令：python3 $HERE/../tools/patch_nso.py <main.nso> $OUT main"
    exit 1
}
ROOT=${DM_RULE_RUNROOT:-$X/run/verify/dm/rulegate}
mkdir -p "$ROOT" || exit 2
T=$ROOT/$(date +%Y%m%d_%H%M%S)_$$
mkdir -p "$T/user" || exit 2
START=$($PY - "$TR/meta.json" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
save = m['save'][1] if isinstance(m['save'], list) else m['save']
print(save)
PY
) || { echo "RULEGATE RED 起档路径解析失败"; exit 2; }
[ -d "$START/0" ] || { echo "RULEGATE RED 起档缺 0/：$START"; exit 2; }
cp -R "$START/0" "$T/user/0" || { echo "RULEGATE RED 起档复制失败"; exit 2; }
chmod -R u+w "$T/user/0" || exit 2
cleanup_red() { echo "RULEGATE RED 拍数 $N 用时 $SECONDS 秒；局目录 $T"; exit 1; }
P=
trap 'if [ -n "${P:-}" ]; then kill "$P" 2>/dev/null || true; wait "$P" 2>/dev/null || true; fi' EXIT INT TERM
SECONDS=0
( cd "$HERE/../host" && timeout 4500s make -s ) >"$T/make.log" 2>&1 || { echo "RULEGATE RED 宿主编译失败；局目录 $T"; exit 1; }
K=${KEYS_FRONT:-X@OpenPanel:MainMenuButtonRibbon>OpenPanel:CharacterSelectPanel,DOWN@OpenPanel:CharacterSelectPanel>UpdateCharacterInfo,A@UpdateCharacterInfo%1>CharacterSelect:SelectCharacter,A@CharacterSelect:SelectCharacter>FrontEndNavigation:ToGame}
SEED=$($PY - "$TR/meta.json" <<'PY'
import json, sys
print(json.load(open(sys.argv[1]))['game_seed'])
PY
) || cleanup_red
FT=$($PY - "$TR/meta.json" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
print(m.get('fix_time') if m.get('fix_time') is not None else m['wall_start'])
PY
) || cleanup_red
env DM_SAVE="$T/user/0" DM_GAME_SEED="$SEED" DM_FIXED_TIME="$FT" DM_KEYS="$K" \
    DM_C2S_REPLAY="$TR/c2s.jsonl" DM_TICKS="$T/ticks.jsonl" DM_TICKS_N="$((N + 1))" \
    DM_C2S_LOG="$T/c2s.out" DM_UIMSG=1 DM_PRESENT_HZ=60 DM_WATCH_STALL=15 DM_WATCH=4500 \
    "$HERE/../host/dmhost" "$OUT" main sdk >"$T/host.log" 2>&1 &
P=$!
DEADLINE=$((SECONDS + 4530))
while kill -0 "$P" 2>/dev/null; do
    [ "$SECONDS" -lt "$DEADLINE" ] || cleanup_red
    sleep 1
done
wait "$P"; RC=$?
    [ "$RC" -eq 0 ] || { grep '【红】看门狗' "$T/host.log" | tail -1; cleanup_red; }
"$PY" - "$TR/ticks.jsonl" "$T/truth.ticks.jsonl" "$N" <<'PY'
import json, sys
src, dst, limit = sys.argv[1], sys.argv[2], int(sys.argv[3])
with open(dst, 'w') as out:
    for line in open(src):
        x = json.loads(line)
        if x['tick'] <= limit:
            out.write(line)
PY
cmp_out=$($PY "$HERE/ryu/selfcmp.py" "$T/truth.ticks.jsonl" "$T/ticks.jsonl" 2>&1)
cmp_rc=$?
echo "$cmp_out" | tee "$T/selfcmp.log"
if [ "$cmp_rc" -ne 0 ]; then cleanup_red; fi
echo "RULEGATE OK 拍数 $N 用时 $SECONDS 秒"
rm -rf "$T"
trap - EXIT INT TERM
