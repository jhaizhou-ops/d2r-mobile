#!/bin/bash

DEVLOCK_DIR=${DM_DEVLOCK_DIR:-${DM_ROOT:-$HOME/dm-work}/run/ios/device.lock}
DEVLOCK_DEPTH=${DEVLOCK_DEPTH:-0}
DEVLOCK_QUEUE=${DM_DEVLOCK_QUEUE:-${DEVLOCK_DIR%.lock}.queue}   # 拿号排队（抢锁无序，后到的可能先拿，曾饿 20 分钟） | Queue with tickets (lock grabbing is unordered; a later arrival may get it first, and one once starved for 20 minutes)
DEVLOCK_TICKET=
# 持锁上限（曾有手写脚本用 devicectl copy from 轮询日志，申报约 100 秒、实占 17 分钟，后面排队的各等 13–15 分钟；以前只要持有者活着就一直等）。
# devlock_acquire <owner> [上限秒]，或环境变量 DM_DEVLOCK_MAX_S；都没给时设备锁按类别取缺省：冒烟 / 合入 / 一般真机局 600 秒（冒烟台账 1338 次用时 p99 385 秒），
# 测量（DM_DEVLOCK_CLASS=measure）1200 秒。别的锁目录（mini 的 macsmoke 锁、自测的临时锁）没有缺省、不设上限，显式给了才管。
# 拿到锁后持锁者这一侧起看门狗：超过上限仍持锁 = 记【红】（owner、已持有秒数）→ 向持锁进程及其全部子孙（devicectl、iproxy、sleep…）发 TERM，5 秒后还在的 KILL → 释放锁。
# 只动持锁者自己的进程树（看门狗自己除外），不碰别人。可重入（嵌套 acquire / 子进程沿用 DM_DEVLOCK_HELD）不另起看门狗、不改上限
# Lock hold limit (a hand-written script once polled logs with devicectl copy from, declared about 100 seconds and actually held 17 minutes, and those queued behind each waited 13–15 minutes; previously, as long as the holder was alive everyone just kept waiting).
# devlock_acquire <owner> [limit seconds], or the environment variable DM_DEVLOCK_MAX_S; when neither is given, the device lock takes a default by class: smoke test / merge / ordinary real-device run 600 seconds (p99 duration over 1338 smoke test ledger entries is 385 seconds),
# measurement (DM_DEVLOCK_CLASS=measure) 1200 seconds. Other lock directories (the second Mac's macsmoke lock, temporary locks of self-tests) have no default and no limit; they are enforced only when one is given explicitly.
# After the lock is obtained, a watchdog starts on the holder's side: still holding past the limit = log 【红】(red) (owner, seconds held) → send TERM to the holding process and all its descendants (devicectl, iproxy, sleep...), KILL those still alive after 5 seconds → release the lock.
# Only the holder's own process tree is touched (except the watchdog itself), nobody else's. Reentrant cases (nested acquire / child processes inheriting DM_DEVLOCK_HELD) start no extra watchdog and do not change the limit
DEVLOCK_WD_PID=
DEVLOCK_WD_LOG=${DM_DEVLOCK_WD_LOG:-${DM_ROOT:-$HOME/dm-work}/run/ios/devlock_watchdog.log}
# 持锁记录（追加式，一行一事件，制表符分列：时刻 取/放 pid 锁目录 持有者 秒数）：取 = 排队等了几秒，放 = 持有了几秒。
# 用来统计测试机被谁占了多久、排队等了多久；看门狗强收的那次没有「放」行，看 devlock_watchdog.log
# Lock hold log (append only, one event per line, tab separated: time take/release pid lock-directory holder seconds): take = seconds spent queued, release = seconds held.
# Used to track who occupied the test device and for how long, and how long the queue waited; a forced reclaim by the watchdog has no "release" line, see devlock_watchdog.log
DEVLOCK_HIST=${DM_DEVLOCK_HIST:-${DM_ROOT:-$HOME/dm-work}/run/ios/devlock_hist.tsv}
devlock_hist() { mkdir -p "$(dirname "$DEVLOCK_HIST")" 2>/dev/null; printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$(date '+%F %T')" "$1" "$$" "$DEVLOCK_DIR" "$2" "$3" >> "$DEVLOCK_HIST" 2>/dev/null || true; }
devlock_default_max() {
  [ "$DEVLOCK_DIR" = "${DM_ROOT:-$HOME/dm-work}/run/ios/device.lock" ] || { echo 0; return; }
  [ "${DM_DEVLOCK_CLASS:-}" = measure ] && echo 1200 || echo 600
}
# 某进程的全部子孙（不含自己），子孙先于祖先列出
# All descendants of a process (excluding itself), descendants listed before ancestors
devlock_descendants() {
  local root=$1 kids k
  kids=$(ps -axo pid=,ppid= 2>/dev/null | awk -v p="$root" '$2==p{print $1}')
  for k in $kids; do devlock_descendants "$k"; echo "$k"; done
}
devlock_watchdog() {   # 在后台跑：$1 持锁 pid，$2 其启动时刻，$3 上限秒，$4 owner | Runs in the background: $1 holder pid, $2 its start time, $3 limit in seconds, $4 owner
  local holder=$1 lst=$2 max=$3 owner=$4 me=${BASHPID:-$$} now started held victims v
  trap '' HUP
  while sleep 2; do
    [ "$(cat "$DEVLOCK_DIR/pid" 2>/dev/null)" = "$holder" ] || exit 0   # 已放锁 / 锁被换主 | Lock already released / lock changed owner
    [ "$(cat "$DEVLOCK_DIR/lstart" 2>/dev/null)" = "$lst" ] || exit 0
    kill -0 "$holder" 2>/dev/null || exit 0
    started=$(cat "$DEVLOCK_DIR/started" 2>/dev/null || echo 0); now=$(date +%s); held=$((now - started))
    [ "$held" -ge "$max" ] || continue
    local msg="【红】设备锁看门狗：持有者「${owner}」（pid ${holder}）已持有 ${held} 秒，超过上限 ${max} 秒 → 终止持锁进程树、释放锁"
    echo "$msg" >&2; mkdir -p "$(dirname "$DEVLOCK_WD_LOG")"; echo "$(date '+%F %T') ${msg}（锁 ${DEVLOCK_DIR}）" >> "$DEVLOCK_WD_LOG"
    victims=$(devlock_descendants "$holder" | grep -v -x -e "$me" $(devlock_descendants "$me" | sed 's/^/-e /'))
    for v in $victims "$holder"; do kill -TERM "$v" 2>/dev/null; done
    sleep 5
    for v in $victims "$holder"; do kill -0 "$v" 2>/dev/null && kill -KILL "$v" 2>/dev/null; done
    [ "$(cat "$DEVLOCK_DIR/pid" 2>/dev/null)" = "$holder" ] && rm -rf "$DEVLOCK_DIR"
    exit 0
  done
}

# 取号：<类别>_<到达纳秒>_<pid>，名字按（类别, 到达先后）排序；持号进程已死的号作废。
# 类别（排队超过五分钟都太久）：0 = 冒烟 / 合入（缺省），1 = 测量（DM_DEVLOCK_CLASS=measure）。
# 冒烟 / 合入的号排在所有测量号前面，先到先得只在同类内部——任何冒烟最多等正在持锁的那一轮测量（测量单轮持锁 ≤ 4 分钟）。
# 旧版脚本取的号（<到达纳秒>_<pid>，无类别）按字节序落在两类之间，新旧混用期间不乱序。
# Take a ticket: <class>_<arrival nanoseconds>_<pid>; names sort by (class, arrival order); a ticket whose holding process is dead is void.
# Classes (queuing for more than five minutes is too long in any case): 0 = smoke test / merge (default), 1 = measurement (DM_DEVLOCK_CLASS=measure).
# Smoke test / merge tickets rank ahead of all measurement tickets, and first-come-first-served applies only within a class - any smoke test waits at most for the measurement round currently holding the lock (a single measurement round holds the lock ≤ 4 minutes).
# Tickets taken by old scripts (<arrival nanoseconds>_<pid>, no class) fall between the two classes in byte order, so the order is not scrambled while old and new are mixed.
devlock_ticket_take() {
  mkdir -p "$DEVLOCK_QUEUE"
  local cls=0; [ "${DM_DEVLOCK_CLASS:-}" = measure ] && cls=1
  DEVLOCK_TICKET="$DEVLOCK_QUEUE/${cls}_$(python3 -c 'import time;print(time.time_ns())')_$$"
  mkdir "$DEVLOCK_TICKET"; printf "%s\n" "${1:-unknown}" > "$DEVLOCK_TICKET/owner"
}
devlock_ticket_drop() { [ -n "$DEVLOCK_TICKET" ] && rm -rf "$DEVLOCK_TICKET"; DEVLOCK_TICKET=; }
# 前面还有几个存活的号（顺手清掉死号）；标准输出：个数<TAB>下一个的 owner
# How many live tickets are ahead (dead tickets are cleaned up along the way); stdout: count<TAB>owner of the next one
devlock_ahead() {
  local d n=0 first="" mine; mine=$(basename "$DEVLOCK_TICKET")
  # 调用处写的是 IFS=$'\t' read … < <(devlock_ahead)：那个临时 IFS 会带进本函数，下面的 $(ls) 就不按换行拆——队列里有 ≥2 个号时
  # 每个人都把整张队列当成「排在前面的一个号」，全体永远等（曾有四个任务互等 11–20 分钟，锁本身没人持有）。这里钉回缺省。
  # The call site is written as IFS=$'\t' read ... < <(devlock_ahead): that temporary IFS carries into this function, so the $(ls) below is not split on newlines - with ≥2 tickets in the queue
  # everyone takes the whole queue as "one ticket ahead" and all wait forever (four tasks once waited on each other for 11–20 minutes while nobody held the lock). Pin it back to the default here.
  local IFS=$' \t\n'
  for d in $(ls "$DEVLOCK_QUEUE" 2>/dev/null | LC_ALL=C sort); do
    kill -0 "${d##*_}" 2>/dev/null || { rm -rf "$DEVLOCK_QUEUE/$d"; continue; }
    [ "$d" != "$mine" ] || break   # 已按字节序排好：排在自己前面的都算（不用 [ \< ]，它按区域设置比较） | Already sorted in byte order: everything ranked before our own counts (do not use [ \< ], which compares by locale)
    n=$((n + 1)); [ -n "$first" ] || first=$(cat "$DEVLOCK_QUEUE/$d/owner" 2>/dev/null)
  done
  printf "%s\t%s\n" "$n" "$first"
}

# 进程启动时刻（判锁的持有者还在不在、防 pid 复用）。ps -o lstart 的字符串跟语言环境走：中文环境是「四 10月/ 8 18:51:58 2026」，
# C / 英文环境是「Thu Oct  8 18:51:58 2026」。取锁方与抢锁方环境不同时，活着的持有者永远对不上、被当无主锁拆掉，两边同时用手机
# （两路真机局曾因此撞车）。写入一律 LC_ALL=C；比对时按锁里那条记录的格式取（记录含非 ASCII = 中文环境写的）
# Process start time (to judge whether the lock holder is still there, guards against pid reuse). The ps -o lstart string follows the locale: in a Chinese locale it is 「四 10月/ 8 18:51:58 2026」,
# in a C / English locale it is "Thu Oct  8 18:51:58 2026". When the lock taker and the lock grabber have different locales, a live holder never matches and is torn down as an ownerless lock, and both sides use the phone at once
# (two real-device runs once collided because of this). Writes always use LC_ALL=C; comparison uses the format of the record in the lock (a record containing non-ASCII = written in a Chinese locale)
devlock_lstart() {   # $1 pid，$2 参照串（缺省 = C 格式） | $1 pid, $2 reference string (default = C format)
  local loc=C; printf %s "${2:-}" | LC_ALL=C grep -q '[^ -~]' && loc=zh_CN.UTF-8
  # 进程已不在时 ps 返回非零：这里一律回 0、只打空串——调用方开着 set -e + pipefail（touch_e2e.sh）时，$(devlock_lstart 死pid) 的赋值会让整个脚本当场静默退出，
  # 走不到「持有者已死 → 收回锁」那个分支（实例：j 局留下死锁后 k pk sb rf2 pg2 vt1 六局 0 秒退、日志只有电量闸一行）
  # ps returns non-zero when the process is gone: here always return 0 and print only an empty string - when the caller has set -e + pipefail (touch_e2e.sh), the assignment $(devlock_lstart <dead pid>) would make the whole script exit silently on the spot,
  # never reaching the "holder is dead → reclaim the lock" branch (instance: after run j left a dead lock, the six runs k pk sb rf2 pg2 vt1 exited at 0 seconds with only the battery gate line in the log)
  LC_ALL=$loc ps -o lstart= -p "$1" 2>/dev/null | sed 's/^[[:space:]]*//; s/[[:space:]]*$//' || true
}
devlock_is_descendant() {
  local child=$1 ancestor=$2 parent depth=0
  while [ "$depth" -lt 64 ]; do
    [ "$child" = "$ancestor" ] && return 0
    parent=$(ps -o ppid= -p "$child" 2>/dev/null | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
    [ -n "$parent" ] || return 1
    [ "$parent" = "$child" ] && return 1
    child=$parent
    depth=$((depth + 1))
  done
  return 1
}

# 取锁必须响亮：里面任何一步意外失败、或走完了锁却不在自己（或可重入的祖先）手里，都打【红】并返回非零——set -e 的调用方随之退出，不许静默退。
# 函数体放在 || 的左边执行，调用方的 set -e 在里面不生效：没开 set -e 的调用方（冒烟、合入）一直是这样跑的，两种调用方从此同一条路径
# Taking the lock must be loud: if any step inside fails unexpectedly, or it finishes with the lock not held by itself (or a reentrant ancestor), print 【红】(red) and return non-zero - a set -e caller then exits; silent exit is not allowed.
# The function body runs on the left side of ||, so the caller's set -e has no effect inside: callers without set -e (smoke test, merge) have always run this way, and from now on both kinds of caller take the same path
devlock_acquire() {
  local rc=0; _devlock_acquire "$@" || rc=$?
  if [ "$rc" = 0 ] && [ "$(cat "$DEVLOCK_DIR/pid" 2>/dev/null)" != "${DM_DEVLOCK_HELD:-}" ]; then rc=97; fi
  [ "$rc" = 0 ] && return 0
  echo "【红】设备锁：${1:-unknown} 没取到锁（返回 ${rc}；锁 $DEVLOCK_DIR 现在的持有者「$(cat "$DEVLOCK_DIR/owner" 2>/dev/null || echo 无)」pid $(cat "$DEVLOCK_DIR/pid" 2>/dev/null || echo 无)）" >&2
  return "$rc"
}
_devlock_acquire() {
  local owner=${1:-unknown} max=${2:-${DM_DEVLOCK_MAX_S:-}} held_lstart pid lock_owner started now held lock_lstart self_lstart lock_pid_lstart current_pid last_notice=0 lock_max over_notice=0
  [ -n "$max" ] || max=$(devlock_default_max)
  local q0=$SECONDS
  mkdir -p "$(dirname "$DEVLOCK_DIR")" 2>/dev/null && [ -w "$(dirname "$DEVLOCK_DIR")" ] || return 98   # 锁的父目录建不出 / 写不了：下面的抢锁循环会空转，直接报失败 | The lock's parent directory cannot be created / written: the lock-grabbing loop below would spin, so report failure directly
  current_pid=${BASHPID:-$$}
  self_lstart=$(devlock_lstart $$)
  if [ -n "${DM_DEVLOCK_HELD:-}" ] && [ -f "$DEVLOCK_DIR/pid" ] &&
     [ "$(cat "$DEVLOCK_DIR/pid" 2>/dev/null)" = "$DM_DEVLOCK_HELD" ] &&
     # 比的是「持锁进程 DM_DEVLOCK_HELD」现在的启动时刻与锁内记录是否一致（防 pid 复用），不是当前进程自己的——
     # 比自己的会让持锁者的子进程（启动时刻必然不同）判不可复用、排队等自己的父进程，死锁
     # What is compared is whether the current start time of the "lock-holding process DM_DEVLOCK_HELD" matches the record inside the lock (guards against pid reuse), not that of the current process itself -
     # comparing its own would make a child of the holder (whose start time necessarily differs) be judged non-reentrant and queue up waiting for its own parent: deadlock
     held_lstart=$(devlock_lstart "$DM_DEVLOCK_HELD" "$(cat "$DEVLOCK_DIR/lstart" 2>/dev/null)") &&
     [ -n "$held_lstart" ] &&
     [ "$held_lstart" = "$(cat "$DEVLOCK_DIR/lstart" 2>/dev/null)" ] &&
     kill -0 "$DM_DEVLOCK_HELD" 2>/dev/null &&
     devlock_is_descendant "$current_pid" "$DM_DEVLOCK_HELD"; then
    DEVLOCK_DEPTH=$((DEVLOCK_DEPTH + 1))
    return 0
  fi
  # 电量闸：取真机锁 device.lock 前过 devbattery.sh——低于 30% 暂停、充到 60% 放行；拒绝时每分钟重读，
  # 一直不放行等满 DM_DEVBATTERY_WAIT_S（缺省 3600）秒就记【红】退出调用脚本（退出码 4）。只管真机锁：DM_DEVBATTERY_LOCKS 列出的锁目录（空格分隔，缺省 $DM_ROOT/run/ios/device.lock；mini 的测试机2 锁 device2.lock 在那边的配置里加进来），单测的临时锁目录不过闸；DM_DEVBATTERY=0 跳过、=1 强制过闸
  # Battery gate: go through devbattery.sh before taking the device lock device.lock - pause below 30%, release once charged to 60%; while refused, re-read every minute,
  # and if still refused after DM_DEVBATTERY_WAIT_S (default 3600) seconds, log a failure and exit the calling script (exit code 4). Only device locks are gated: the lock directories listed in DM_DEVBATTERY_LOCKS (space-separated, default $DM_ROOT/run/ios/device.lock; the second test device's lock device2.lock is added in that machine's configuration); unit tests' temporary lock directories skip the gate; DM_DEVBATTERY=0 skips, =1 forces the gate
  if { [[ " ${DM_DEVBATTERY_LOCKS:-${DM_ROOT:-$HOME/dm-work}/run/ios/device.lock} " == *" $DEVLOCK_DIR "* ]] && [ "${DM_DEVBATTERY:-1}" != 0 ]; } || [ "${DM_DEVBATTERY:-}" = 1 ]; then
    source "$(dirname "${BASH_SOURCE[0]}")/devbattery.sh"
    local bw0=$SECONDS bwmax=${DM_DEVBATTERY_WAIT_S:-3600}
    local brc
    until devbattery_gate; do
      brc=$?; [ $brc != 3 ] || { echo "【红】电量闸：本机缺命令（见上一行），等也没用，$owner 不取锁、退出" >&2; exit 4; }
      [ $((SECONDS - bw0)) -ge "$bwmax" ] && { echo "【红】电量闸等了 $((SECONDS - bw0)) 秒仍不放行（${DEVBATTERY_LINE:-}），$owner 不取锁、退出" >&2; exit 4; }
      sleep "${DM_DEVBATTERY_POLL_S:-60}"
    done
    echo "电量闸放行：${DEVBATTERY_LINE:-}（${owner}）" >&2
  fi
  devlock_ticket_take "$owner"
  local ahead first q_notice=0
  while :; do
    IFS=$'\t' read -r ahead first < <(devlock_ahead)
    [ "$ahead" = 0 ] && break
    now=$(date +%s); [ $((now - q_notice)) -ge 30 ] && { echo "排队等设备锁：前面还有 $ahead 个（下一个：${first:-?}）" >&2; q_notice=$now; }
    sleep 1
  done
  while :; do
    # 每次抢锁前重看队列：等锁期间来了更优先的号（冒烟 / 合入排在测量前）就让它先抢
    # Re-check the queue before every lock attempt: if a higher-priority ticket arrived while waiting (smoke test / merge rank ahead of measurement), let it grab first
    IFS=$'\t' read -r ahead first < <(devlock_ahead)
    if [ "$ahead" != 0 ]; then sleep 1; continue; fi
    mkdir "$DEVLOCK_DIR" 2>/dev/null && break
    pid=$(cat "$DEVLOCK_DIR/pid" 2>/dev/null || true)
    lock_owner=$(cat "$DEVLOCK_DIR/owner" 2>/dev/null || echo unknown)
    started=$(cat "$DEVLOCK_DIR/started" 2>/dev/null || date +%s)
    lock_lstart=$(cat "$DEVLOCK_DIR/lstart" 2>/dev/null || true)
    now=$(date +%s)
    held=$((now - started))
    lock_pid_lstart=$(devlock_lstart "$pid" "$lock_lstart")
    # 旧格式锁（只有 pid / owner、没有 lstart 文件，如旧版 ios_smoke.sh 或手写的 mkdir 锁）：pid 活着就尊重，不判陈旧
    # Old-format lock (only pid / owner, no lstart file, e.g. an old ios_smoke.sh or a hand-written mkdir lock): if the pid is alive, respect it and do not judge it stale
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && { [ ! -f "$DEVLOCK_DIR/lstart" ] || [ "$lock_pid_lstart" = "$lock_lstart" ]; }; then
      lock_max=$(cat "$DEVLOCK_DIR/max" 2>/dev/null || echo 0)
      if [ "$last_notice" -eq 0 ] || [ $((now - last_notice)) -ge 30 ]; then
        if [ "${lock_max:-0}" -gt 0 ]; then echo "等待设备锁：持有者 ${lock_owner}，已持有 ${held} 秒 / 上限 ${lock_max} 秒" >&2
        else echo "等待设备锁：持有者 ${lock_owner}，已持有 ${held} 秒（未申报上限）" >&2; fi
        last_notice=$now
      fi
      # 超过上限 30 秒还没放（持锁者的看门狗也没管用）：记红，但不碰别人的进程
      # Still not released 30 seconds past the limit (the holder's watchdog did not work either): log a failure line, but do not touch other people's processes
      if [ "${lock_max:-0}" -gt 0 ] && [ "$held" -gt $((lock_max + 30)) ] && [ $((now - over_notice)) -ge 60 ]; then
        echo "【红】设备锁：持有者 ${lock_owner}（pid ${pid}）已持有 ${held} 秒，超过上限 ${lock_max} 秒仍未释放（看门狗没起作用），不代为终止，请找持有者" >&2
        mkdir -p "$(dirname "$DEVLOCK_WD_LOG")"; echo "$(date '+%F %T') 等锁方：持有者「${lock_owner}」pid $pid 已持有 ${held} 秒、上限 ${lock_max} 秒仍未释放（锁 ${DEVLOCK_DIR}）" >> "$DEVLOCK_WD_LOG"
        over_notice=$now
      fi
      sleep 1
    elif [ -z "$pid" ] && [ $((now - $(stat -f %m "$DEVLOCK_DIR" 2>/dev/null || echo 0))) -lt 10 ]; then
      sleep 1   # 别人刚 mkdir、还没写 pid：正在创建，不是陈旧锁 | Someone else just did mkdir and has not written the pid yet: it is being created, not a stale lock
    else
      rm -rf "$DEVLOCK_DIR"
    fi
  done
  printf "%s\n" "$$" > "$DEVLOCK_DIR/pid"
  printf "%s\n" "$owner" > "$DEVLOCK_DIR/owner"
  date +%s > "$DEVLOCK_DIR/started"
  printf "%s\n" "$self_lstart" > "$DEVLOCK_DIR/lstart"
  printf "%s\n" "${max:-0}" > "$DEVLOCK_DIR/max"
  export DM_DEVLOCK_HELD=$$
  devlock_ticket_drop
  DEVLOCK_DEPTH=1
  devlock_hist 取 "$owner" $((SECONDS - q0))
  if [ "${max:-0}" -gt 0 ]; then
    devlock_watchdog "$$" "$self_lstart" "$max" "$owner" </dev/null >/dev/null &
    DEVLOCK_WD_PID=$!; disown "$DEVLOCK_WD_PID" 2>/dev/null || true
  fi
}

devlock_release() {
  [ "$DEVLOCK_DEPTH" -gt 0 ] || return 0
  if [ "$DEVLOCK_DEPTH" -gt 1 ]; then
    DEVLOCK_DEPTH=$((DEVLOCK_DEPTH - 1))
    return 0
  fi
  # 看门狗连同它正在 sleep 的子进程一起收掉（KILL：它的 stdout / stderr 可能连着调用方的管道，拖着不退会让 `… | tail` 多等）
  # 先记下它的子进程、先杀看门狗本身再杀子进程：反过来的话看门狗会替被杀的 sleep 打一行「Killed: 9」到调用方的输出里
  # 每一步都带 || true：看门狗正好在两次 sleep 之间（没有子进程）时 pgrep 返回 1，set -e 的调用方在 EXIT 钩子里跑到这就当场退出、走不到下面删锁，
  # 留下一把持有者已死的锁（j 局出现过；离线复现：持锁 2 秒放锁，退出码 1、锁目录还在）
  # Reap the watchdog together with the child it is currently sleeping in (KILL: its stdout / stderr may be attached to the caller's pipe, and lingering would make `... | tail` wait longer)
  # Record its children first, then kill the watchdog itself before the children: the other way round, the watchdog would print a line "Killed: 9" for the killed sleep into the caller's output
  # Every step carries || true: when the watchdog happens to be between two sleeps (no child), pgrep returns 1, and a set -e caller running this in its EXIT hook would exit on the spot and never reach the lock removal below,
  # leaving a lock whose holder is dead (seen in run j; offline reproduction: hold the lock 2 seconds then release, exit code 1, lock directory still there)
  if [ -n "$DEVLOCK_WD_PID" ]; then local wk; wk=$(pgrep -P "$DEVLOCK_WD_PID" 2>/dev/null) || true; kill -KILL "$DEVLOCK_WD_PID" 2>/dev/null || true; [ -z "$wk" ] || kill -KILL $wk 2>/dev/null || true; DEVLOCK_WD_PID=; fi
  [ "$(cat "$DEVLOCK_DIR/pid" 2>/dev/null)" = "$$" ] || return 0
  devlock_hist 放 "$(cat "$DEVLOCK_DIR/owner" 2>/dev/null)" $(( $(date +%s) - $(cat "$DEVLOCK_DIR/started" 2>/dev/null || date +%s) ))
  rm -rf "$DEVLOCK_DIR"
  DEVLOCK_DEPTH=0
}
