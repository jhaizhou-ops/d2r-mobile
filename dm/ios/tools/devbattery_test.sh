#!/bin/bash
# devbattery.sh 单测：假读数（DEVBATTERY_FAKE）+ 临时目录，不碰设备。期望值按规则手算：不在充时 < 30 拒并落标记、标记在时 < 60 拒、≥ 60 撤标记放行；接电且在充时 ≥ 20 放行（撤标记）、< 20 拒；读不到拒
# Unit test for devbattery.sh: fake readings (DEVBATTERY_FAKE) + a temporary directory, never touching a device. Expected values computed by hand from the rules: not charging < 30 refuses and drops the marker, with the marker < 60 refuses, >= 60 removes the marker and releases; plugged in and charging >= 20 releases (removing the marker), < 20 refuses; unreadable refuses
cd "$(dirname "$0")"; T=$(mktemp -d); export DEVBATTERY_DIR=$T;   # 单测一律写临时目录，不碰 $DM_ROOT/run/ios 的真日志与告警 | Unit tests always write to a temporary directory and never touch the real logs and alerts in $DM_ROOT/run/ios
 source ./devbattery.sh; bad=0
ck() { local want=$1 got; shift; DEVBATTERY_FAKE="$*" devbattery_gate 2>/dev/null; got=$?; [ "$got" = "$want" ] && echo "通 $* → $got" || { echo "不通 $* → ${got}（期望 ${want}）"; bad=1; }; }
ck 0 80 0 1 1          # 80%：放行 | 80%: pass
ck 1 25 0 1 0          # 不在充、25%：拒，落标记 | Not charging, 25%: refused, marker dropped
[ -f $T/device_low_battery ] && echo "通 低电落标记" || { echo "不通 低电没落标记"; bad=1; }
ck 1 45 0 1 0          # 不在充、标记在、45%：拒 | Not charging, marker present, 45%: refused
ck 0 60 0 1 0          # 不在充、60%：撤标记放行 | Not charging, 60%: marker removed, released
[ ! -f $T/device_low_battery ] && echo "通 撤标记" || { echo "不通 标记没撤"; bad=1; }
ck 0 45 0 1 0          # 不在充、没标记、45%：放行 | Not charging, no marker, 45%: released
ck 1 25 0 0 0          # 拔电、25%：拒，落标记（下面三条接着用这个标记） | Unplugged, 25%: refused, marker dropped (the next three cases keep using this marker)
ck 0 25 1 1 0          # 接电在充、标记在、25%：≥ 20 放行 | Plugged in and charging, marker present, 25%: >= 20, released
[ ! -f $T/device_low_battery ] && echo "通 充电中放行撤标记" || { echo "不通 充电中放行却没撤标记"; bad=1; }
ck 1 15 1 1 0          # 接电在充、15%：< 20 拒 | Plugged in and charging, 15%: < 20, refused
ck 1 15 0 0 0          # 拔电、15%：拒并落标记 | Unplugged, 15%: refused and marker dropped
ck 1 45 0 1 0          # 接电但不在充、标记在、45%：仍按 60% 拒 | Plugged in but not charging, marker present, 45%: still refused by the 60% rule
ck 0 20 1 1 0          # 接电在充、20%：等于门槛放行 | Plugged in and charging, 20%: at the threshold, released
# 本机缺 timeout：与设备离线分开报，返回 3（PATH 换成只有基本命令的目录，UDID 给个假的；期望按规则：读电量前先找工具，找不到即 3）
# timeout missing locally: reported separately from a device being offline, returns 3 (PATH is replaced by a directory with only basic commands, the UDID is a fake one; expected by rule: the tools are looked up before the battery is read, and not finding them gives 3)
E=$T/emptybin; mkdir -p $E; for c in date mkdir dirname cat head awk; do ln -s "$(command -v $c)" $E/$c; done
( PATH=$E DM_IOS_UDID=fake; unset DEVBATTERY_FAKE; devbattery_gate > $T/miss.out 2>&1; echo $? > $T/miss.rc )
[ "$(cat $T/miss.rc)" = 3 ] && grep -q "本机缺读电量要用的命令" $T/miss.out && ! grep -q "离线" $T/miss.out && echo "通 缺 timeout → 3，报缺命令不报离线" || { echo "不通 缺 timeout → $(cat $T/miss.rc)：$(head -1 $T/miss.out)"; bad=1; }
out=$(DEVBATTERY_FAKE="50 0 1 0" devbattery_gate 2>&1); case "$out" in *供电跟不上*) echo "通 插电不充记红";; *) echo "不通 插电不充没记红"; bad=1;; esac
out=$(DEVBATTERY_FAKE="99 0 1 0" devbattery_gate 2>&1); case "$out" in *供电跟不上*) echo "不通 99% 停充被当成供电跟不上"; bad=1;; *) echo "通 99% 停充不记红";; esac
DEVBATTERY_FAKE= DM_IOS_UDID=00000000-0000000000000000 devbattery_gate 2>/dev/null && { echo "不通 读不到却放行"; bad=1; } || echo "通 读不到拒"
grep -q '读不到' $T/battery.log && echo "通 读不到记「读不到」" || { echo "不通 读不到没记对"; bad=1; }
DEVBATTERY_FAKE= DM_IOS_UDID= DM_LOCAL_ENV=/dev/null HOME=$T devbattery_gate 2>/dev/null && { echo "不通 没配 UDID 却放行"; bad=1; } || echo "通 没配 UDID 拒"
grep -q '没配 UDID' $T/battery.log && echo "通 没配 UDID 记「没配 UDID」" || { echo "不通 没配 UDID 没分开记"; bad=1; }
# 拔电走网络：ideviceinfo 换成必失败的假命令；App 读数文件新鲜按它判，过期且声明局内判 → 放行（有暂停标记仍拒），过期不声明 → 读不到拒
# Unplugged over the network: ideviceinfo replaced with a command that always fails; a fresh App reading file is used, stale with an in-run check declared -> released (still refused with the pause marker), stale without -> unreadable, refused
F=$T/fbin; mkdir -p $F; printf '#!/bin/sh\nexit 1\n' > $F/ideviceinfo; chmod +x $F/ideviceinfo; U=00000000-1111; rm -f $T/device_low_battery
ab() { local want=$1 got; shift; ( PATH=$F:$PATH DM_IOS_UDID=$U; unset DEVBATTERY_FAKE; export "$@"; devbattery_gate > /dev/null 2>&1 ); got=$?; [ "$got" = "$want" ] && echo "通 App 读数 $* → $got" || { echo "不通 App 读数 $* → ${got}（期望 ${want}）"; bad=1; }; }
echo "75 放电" > $T/app_battery_$U.txt; ab 0 X=1                      # 新鲜 75% 放电：放行 | Fresh 75% discharging: released
echo "25 放电" > $T/app_battery_$U.txt; ab 1 X=1                      # 新鲜 25% 放电：拒并落标记 | Fresh 25% discharging: refused and marker dropped
[ -f $T/device_low_battery ] && echo "通 App 读数低电落标记" || { echo "不通 App 读数低电没落标记"; bad=1; }
touch -t 202001010000 $T/app_battery_$U.txt; ab 1 DM_DEVBATTERY_INRUN=1    # 过期 + 局内判，但暂停标记在：拒 | Stale + in-run check, but the pause marker is present: refused
rm -f $T/device_low_battery; ab 0 DM_DEVBATTERY_INRUN=1                  # 过期 + 局内判、无标记：放行 | Stale + in-run check, no marker: released
ab 1 X=1                                                                  # 过期、不声明局内判：读不到拒 | Stale, no in-run check declared: unreadable, refused
grep -q 通知 $T/device_alert.txt && echo "通 告警文件有记录" || { echo "不通 告警文件没记录"; bad=1; }
# devlock 接入：device.lock 目录取锁前过闸——电量够直接取到；电量低等满上限退出 4、不取锁
# devlock integration: go through the gate before taking the device.lock directory lock - with enough battery the lock is taken directly; with low battery it waits out the full limit, exits 4 and does not take the lock
L=$T/device.lock; export DEVBATTERY_DIR=$T
DM_DEVBATTERY=1 DEVBATTERY_FAKE="80 0 1 0" DM_DEVLOCK_DIR=$L bash -c 'source ./devlock.sh; devlock_acquire 单测 2>/dev/null; [ -d "$DEVLOCK_DIR" ] && devlock_release' && echo "通 devlock 电量够取到锁" || { echo "不通 devlock 电量够没取到"; bad=1; }
DM_DEVBATTERY=1 DEVBATTERY_FAKE="10 0 1 0" DM_DEVBATTERY_WAIT_S=2 DM_DEVBATTERY_POLL_S=1 DM_DEVLOCK_DIR=$L bash -c 'source ./devlock.sh; devlock_acquire 单测 2>/dev/null; echo 不该走到这' >/dev/null; r=$?
[ $r = 4 ] && [ ! -d $L ] && echo "通 devlock 电量低退出 4 且没取锁" || { echo "不通 devlock 电量低 → $r"; bad=1; }
# 锁目录在 DM_DEVBATTERY_LOCKS 名单里（mini 测试机2 的 device2.lock 走这条，不靠 DM_DEVBATTERY=1）：不在充 + 25% → 退出 4、落暂停标记；名单外的锁不过闸
# The lock directory is listed in DM_DEVBATTERY_LOCKS (the second test device's device2.lock on the other machine takes this path, not DM_DEVBATTERY=1): not charging + 25% -> exit 4 and drop the pause marker; locks not on the list skip the gate
L2=$T/device2.lock; rm -f $T/device_low_battery
( unset DM_DEVBATTERY; DM_DEVBATTERY_LOCKS="$T/x.lock $L2" DEVBATTERY_FAKE="25 0 1 0" DM_DEVBATTERY_WAIT_S=1 DM_DEVBATTERY_POLL_S=1 DM_DEVLOCK_DIR=$L2 bash -c 'source ./devlock.sh; devlock_acquire 单测 2>/dev/null; echo 不该走到这' >/dev/null ); r=$?
[ $r = 4 ] && [ ! -d $L2 ] && [ -f $T/device_low_battery ] && echo "通 名单内锁 不在充 25% → 暂停（退出 4、落标记）" || { echo "不通 名单内锁 不在充 25% → $r"; bad=1; }
( unset DM_DEVBATTERY; DM_DEVBATTERY_LOCKS="$T/x.lock" DEVBATTERY_FAKE="25 0 1 0" DM_DEVLOCK_DIR=$L2 bash -c 'source ./devlock.sh; devlock_acquire 单测 2>/dev/null; [ -d "$DEVLOCK_DIR" ] && devlock_release' ) && echo "通 名单外锁不过闸" || { echo "不通 名单外锁被闸住"; bad=1; }
rm -rf "$T"; [ $bad = 0 ] && echo "devbattery 单测全过" || { echo "devbattery 单测有不通"; exit 1; }
