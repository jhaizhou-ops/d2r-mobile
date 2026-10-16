#!/bin/bash
# Mac 上的触屏端到端：与真机 touch_e2e.sh 同参数、同产物名，被测的是同一个 iOS 包（smoke_mac.sh 包好的 <树>/.iosdd_mac/DmMacSmoke.app，先跑过一次 smoke_mac.sh 或自行构建）。
#   [PREFIX=c2s.jsonl] [E2E_REF=956x440] tools/touch_e2e_mac.sh <脚本文件> <结果目录> [秒=60]
# E2E_REF=<宽>x<高>（如真机的 956x440）：脚本里的裸坐标是按这个点数的屏幕写的，由 App 换算到 Mac 窗口（DM_TOUCH_REF，front_ios.m scriptPoint）；@名字 的控件目标不换算。
# 文件直接读写 App 容器（不经 devicectl，每份省约 20 s）；与 Mac 冒烟共用一把锁（同一容器）。
# Touch end-to-end on the Mac: same parameters and same output names as the real-device touch_e2e.sh; what is tested is the same iOS app (<tree>/.iosdd_mac/DmMacSmoke.app packaged by smoke_mac.sh; run smoke_mac.sh once first or build it yourself).
#   [PREFIX=c2s.jsonl] [E2E_REF=956x440] tools/touch_e2e_mac.sh <script file> <result dir> [seconds=60]
# E2E_REF=<width>x<height> (e.g. the real device's 956x440): bare coordinates in the script are written for a screen of this many points and converted by the App to the Mac window (DM_TOUCH_REF, front_ios.m scriptPoint); widget targets of the form @name are not converted.
# Files are read and written directly in the App container (not through devicectl, saving about 20 s per file); shares one lock with the Mac smoke test (same container).
set -u   # 不开 -e：devlock.sh 按 smoke.sh 的口径写（只 set -u），其内部条件式返回非零会被 -e 当失败退出 | Do not enable -e: devlock.sh is written to smoke.sh's convention (only set -u), and a non-zero return from its internal conditionals would be treated by -e as a failure and exit
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/local_env.sh"   # 本机配置（包名、团队号、设备号） | Local config (package name, team ID, device ID)
B=${DM_BUNDLE_ID:?先在本机配置里设 DM_BUNDLE_ID（App 包名，见 dm/ios/local.env.example）}
SCRIPT=$1; OUT=$2; CAP=${3:-60}
TOOLS=$(cd "$(dirname "$0")" && pwd); TREE=$(git -C "$TOOLS" rev-parse --show-toplevel)
W=${E2E_MAC_APP:-$TREE/.iosdd_mac/DmMacSmoke.app}; exe="$W/Wrapper/DmIOS.app/DmIOS"
[ -x "$exe" ] || { echo "【红】没有包：${W}（先跑 dm/ios/tools/smoke_mac.sh 1）"; exit 2; }
mkdir -p "$OUT"; cp "$SCRIPT" "$OUT/script.txt"
t(){ echo "[$(date +%T)] $*"; }
C=""; for m in "$HOME"/Library/Containers/*/.com.apple.containermanagerd.metadata.plist; do
  [ "$(/usr/libexec/PlistBuddy -c 'Print :MCMMetadataIdentifier' "$m" 2>/dev/null)" = "$B" ] && C="$(dirname "$m")/Data"; done
[ -n "$C" ] || { echo "【红】本机没有 $B 的容器（先跑 smoke_mac.sh 1）"; exit 2; }
DOC="$C/Documents"
export DM_DEVLOCK_DIR=${DM_ROOT:-$HOME/dm-work}/run/ios/macsmoke.lock; source "$TOOLS/devlock.sh"; source "$TOOLS/cloud_guard.sh"; source "$TOOLS/save_key.sh"; source "$TOOLS/launch_guard.sh"; devlock_acquire "${SMOKE_OWNER:-未署名} Mac 触屏 $(basename "$OUT")"
# 找实例按可执行文件后缀（系统有时把包迁到随机路径下运行，按原路径认不到）；同一 bundle id 在 Mac 上只能有一个实例，开跑前已有实例即红、不替人结束
# Find the instance by executable suffix (the system sometimes relocates the bundle to a random path to run it, so the original path does not match); the same bundle id can have only one instance on the Mac; an instance already existing before the run starts is a failure, and we do not terminate it on someone else's behalf
pids(){ ps -axo pid=,comm= | awk '{ pid=$1; $1=""; sub(/^ /,""); if ($0 ~ /\/Wrapper\/DmIOS\.app\/DmIOS$/) print pid }'; }
stop_all(){ local p; for p in $(pids); do kill -TERM "$p" 2>/dev/null || true; done; sleep 1; for p in $(pids); do kill -KILL "$p" 2>/dev/null || true; done; }
pre=$(pids | tr '\n' ' '); [ -z "$pre" ] || { echo "【红】本机已有 $B 的实例在跑（pid ${pre}，非本次启动）：先结束它再跑"; devlock_release; exit 2; }
trap 'stop_all; savekey_out "$C"; devlock_release' EXIT; trap 'exit 143' INT TERM
RID=r$(date +%H%M%S)
awk -v r="$RID" '($2=="shot"||$2=="tshot"){$3=r"_"$3} {print}' "$SCRIPT" > "$OUT/script_run.txt"
cp "$OUT/script_run.txt" "$DOC/touch_script.txt"
[ -n "${PREFIX:-}" ] && cp "$PREFIX" "$DOC/e2e_prefix.jsonl"
KEYS=${E2E_KEYS:-'X@OpenPanel:MainMenuButtonRibbon>OpenPanel:CharacterSelectPanel,C0.84:0.208@OpenPanel:CharacterSelectPanel%1>UpdateCharacterInfo,MSG:CharacterSelect:LoadCharacter:0@UpdateCharacterInfo%0.3>FrontEndNavigation:ToGame'}   # E2E_KEYS：换起局键序（如主菜单直接 A 进默认角色） | E2E_KEYS: replace the run-start key sequence (e.g. press A on the main menu to enter the default character directly)
ARGS=(--env DM_SAVE_FRESH=warlock --env DM_TOUCH_SCRIPT=touch_script.txt --env DM_TOUCH_CONTROL_LOG=1
  --env "DM_C2S_LOG=${RID}_c2s.jsonl" --env "DM_UIMSG=${RID}_uimsg.jsonl" --env "DM_STATE=${RID}_state.jsonl"
  --env "DM_TICKS=${RID}_ticks.jsonl" --env DM_TICKS_FULL=1 --env DM_TICKS_N=0 --env "DM_TARGET_LOG=${RID}_target.jsonl"
  --env "DM_KEYS=$KEYS" --env "DM_WATCH=$((CAP + 60))")
# 真机档位（缺省开，同 smoke_mac.sh）：带真机的画面分辨率与左右安全区；DM_MAC_DEVICE_PROFILE=0 回到按 Mac 窗口原样跑（脚本写死窗口点、按旧布局录的用例用这个）
# Real-device profile (on by default, same as smoke_mac.sh): carries the real device's screen resolution and left/right safe areas; DM_MAC_DEVICE_PROFILE=0 goes back to running as-is in the Mac window (use this for scripts with hard-coded window points and cases recorded against the old layout)
[ "${DM_MAC_DEVICE_PROFILE:-1}" != 0 ] && ARGS+=(--env DM_GAME_RES=2868x1320 --env DM_TEST_SAFE_L=62 --env DM_TEST_SAFE_R=62)
# E2E_DESK=1：这台 Mac 有人在用（窗口会被别的窗口盖住）时带上，出图不走显示节拍、窗口被盖也照出帧（同 smoke_mac.sh 的说明）；无人桌面的 mini 不用带
# E2E_DESK=1: set it when someone is using this Mac (the window may be covered by other windows); presentation then does not use the display link and
# frames are still presented with the window covered (see the note in smoke_mac.sh); not needed on the second Mac with an unattended desktop
[ "${E2E_DESK:-0}" = 1 ] && ARGS+=(--env DM_PRESENT_LINK=0)
[ -n "${E2E_REF:-}" ] && ARGS+=(--env "DM_TOUCH_REF=$E2E_REF")
[ -n "${PREFIX:-}" ] && ARGS+=(--env DM_C2S_REPLAY=e2e_prefix.jsonl --env DM_GAME_SEED=305419896 --env DM_FIXED_TIME=1790895128)
# 合并 E2E_ENV（同真机 touch_e2e.sh 的语义）：调用方的键覆盖上面的缺省；值写 null = 去掉这个缺省（如 {"DM_SAVE_FRESH":null,"DM_SAVE_TEST":"1"}：沿用上一局的 save.test）。
# 之前是把 E2E_ENV 逐项追加在缺省后面：null 被拼成字符串「None」、缺省的 DM_SAVE_FRESH=warlock 去不掉，Mac 上沿用测试存档的局起不来（玩法线报）
# Merge E2E_ENV (same semantics as touch_e2e.sh on device): the caller's keys override the defaults above; a null value removes that default (e.g. {"DM_SAVE_FRESH":null,"DM_SAVE_TEST":"1"}: keep the previous run's save.test).
# Previously E2E_ENV was appended after the defaults item by item: null became the string 'None' and the default DM_SAVE_FRESH=warlock could not be removed, so Mac runs reusing the test save failed to start
ENVL=(); while IFS= read -r kv; do ENVL+=(--env "$kv"); done < <(python3 -c "
import json,sys
a=sys.argv[2:]; env={}
for i in range(0,len(a),2): k,v=a[i+1].split('=',1); env[k]=v   # a 是 --env K=V 成对 | a holds --env K=V in pairs
env.update(json.loads(sys.argv[1] or '{}'))
[print(f'{k}={v}') for k,v in env.items() if v is not None]" "${E2E_ENV:-}" "${ARGS[@]}")
ARGS=("${ENVL[@]}")
# iCloud 总闸（tools/cloud_guard.sh）：云端指到一次性本地目录；调用方用 E2E_ENV 自己指了本地云端的照它的
# iCloud master gate (tools/cloud_guard.sh): the cloud is pointed at a one-off local directory; if the caller specified a local cloud itself through E2E_ENV, that one is used
cloud_guard_env "$DOC" "$(python3 -c "import json,sys;print(' '.join(f'{k}={v}' for k,v in json.loads(sys.argv[1] or '{}').items()))" "${E2E_ENV:-}")" || exit 1
[ -z "$CG_ENV" ] || ARGS+=(--env "$CG_ENV")
# 存档钥匙（tools/save_key.sh）：带 DM_SAVE_KEY 的换进自己那份测试存档；多步接着用却不带钥匙的不起
# Save key (tools/save_key.sh): runs with DM_SAVE_KEY swap in their own test save; multi-step continuing runs without a key are not started
savekey_in "$C" "$(python3 -c "import json,sys;print(' '.join(f'{k}={v}' for k,v in json.loads(sys.argv[1] or '{}').items() if v is not None))" "${E2E_ENV:-}")" || exit 1
launch_guard_pre "$C"; mv "$DOC/boot.txt" "$DOC/boot_prev.txt" 2>/dev/null
open -n "$W" "${ARGS[@]}" --args -mode boot "${LG_ARGS[@]}" > "$OUT/launch.log" 2>&1 || { echo "【红】open 启动失败：$(tail -1 "$OUT/launch.log")"; exit 1; }
for _ in $(seq 30); do [ -s "$DOC/boot.txt" ] && break; sleep 0.5; done; [ -s "$DOC/boot.txt" ] || { launch_guard_blocked "$B" "open 之后 15 秒"; exit 3; }
t "已启动，跑 ${CAP}s"; sleep "$CAP"; stop_all; t "已停"
CG_BAD=0; cloud_guard_check "$DOC/boot.txt" || CG_BAD=1; cloud_guard_clean "$DOC"
ALL="boot.txt ${RID}_c2s.jsonl ${RID}_uimsg.jsonl ${RID}_ticks.jsonl ${RID}_target.jsonl ${RID}_state.jsonl $(awk '$2=="shot"{print $3; print $3".txt"} $2=="tshot"{print $3; print $3".json"}' "$OUT/script_run.txt")"
[ -n "${E2E_PULL:-}" ] && ALL=$(for f in $E2E_PULL; do [ "$f" = boot.txt ] && echo boot.txt || echo "${RID}_$f"; done)
for f in $ALL; do
  g=${f#${RID}_}; case $f in ${RID}_*.jsonl) g=e2e_$g;; esac
  if [ -f "$DOC/$f" ]; then cp "$DOC/$f" "$OUT/$g"; [ "$f" = boot.txt ] || rm -f "$DOC/$f"; else t "【红】没有 ${f}（本局没写）"; fi
done
left=$(pids); [ -z "$left" ] && t "本机 DmIOS 进程 0" || t "【红】仍有 DmIOS 进程 $left"
[ "$CG_BAD" = 0 ] || exit 4
