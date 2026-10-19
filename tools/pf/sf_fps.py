#!/usr/bin/env python3
"""按 SurfaceFlinger 实际呈现时间戳测 FPS（不依赖应用内部计数）。

用法: sf_fps.py <serial> [秒数]
输出: 呈现帧数、FPS、帧间隔 P50/P95、掉帧（间隔 > 1.5x P50）、场景探针行。
"""
import subprocess, sys, time, statistics, re

def adb(serial, *args):
    return subprocess.run(["adb", "-s", serial, *args], capture_output=True, text=True).stdout

def layer(serial):
    """层的完整名字必须包含十六进制前缀与 #id，否则 --latency 只回刷新周期。"""
    out = adb(serial, "shell", "dumpsys SurfaceFlinger --list")
    best = None
    for line in out.splitlines():
        m = re.search(r"RequestedLayerState\{(.*?) parentId=", line)
        if not m:
            continue
        name = m.group(1)
        if "SurfaceView[org.d2r.binaryprobe" in name:
            if "(BLAST)" in name:
                return name
            best = best or name
    return best

def main():
    serial = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
    name = layer(serial)
    if not name:
        print("no layer"); return 2
    adb(serial, "shell", f'dumpsys SurfaceFlinger --latency "{name}"')
    time.sleep(secs)
    raw = adb(serial, "shell", f'dumpsys SurfaceFlinger --latency "{name}"')
    times = []
    for line in raw.splitlines():
        p = line.split()
        if len(p) == 3 and p[0].isdigit():
            actual = int(p[1])
            if actual > 0:
                times.append(actual)
    if len(times) < 10:
        print(f"layer={name} frames={len(times)} (insufficient)"); return 1
    times.sort()
    span = (times[-1] - times[0]) / 1e9
    fps = (len(times) - 1) / span if span > 0 else 0.0
    gaps = [(b - a) / 1e6 for a, b in zip(times, times[1:])]
    p50 = statistics.median(gaps)
    p95 = statistics.quantiles(gaps, n=20)[18] if len(gaps) >= 20 else max(gaps)
    drops = sum(1 for g in gaps if g > p50 * 1.5)
    scene = adb(serial, "shell", "grep '小地图诊断' /sdcard/Android/data/org.d2r.binaryprobe/files/d2r-mobile/diagnostics/game-startup.log | tail -1").strip()
    print(f"layer={name}")
    print(f"frames={len(times)} span={span:.3f}s fps={fps:.3f} p50={p50:.2f}ms p95={p95:.2f}ms drops={drops} ({100.0*drops/max(1,len(gaps)):.1f}%)")
    print(f"scene={scene[:160]}")
    return 0

sys.exit(main())
