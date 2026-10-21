#!/usr/bin/env python3
# 慢帧归因：输出慢帧清单，每帧拆成 CPU 侧 / 等待侧 / GPU 侧。两种输入：
#   · 宿主日志（boot.txt 或经日志端口实时读回的流，开 DM_SLOWFRAME=1）：只含掉帧帧的「[slowframe]\t帧号\t@名=值…」行
#     与每 300 帧一行分母，读数阶段分段用同一份日志里的 UIMSG；用法 slowframe.py boot.txt 或 … | slowframe.py -
#   · 逐帧文件（DM_NVN_TSV，可加 DM_NVN_TSV_CALLS=0）+ 可选 boot 日志：slowframe.py frames.tsv [boot.txt]
# --all 连启动 / 读取阶段的慢帧也逐帧列出
#
# 慢帧口径：上屏间隔 @presented_ms > 16.7×1.5 ms 或 −1（系统丢弃）记一次掉帧；进局前的帧单列。
# 无上屏时刻的日志（Mac 无窗口）退回用 @frame_ms > 25 ms，并在表头注明。
# 各分项（ms，均为该帧号，即上一次出图调用到本次出图调用之间）：
#   间隔   @frame_ms     出图线程两次调 QueuePresent 的间隔
#   CPU    @cpu_ms       出图线程本帧在 CPU 上的时间（−1 = 换了线程）；其中 @sub_cpu_ms 提交回放、@upload_ms 贴图上传、
#                        @lib_new / @pso_new 同步建库 / 建管线个数；@proc_cpu_ms 全进程所有线程 CPU（看是否别的线程吃满核）
#   等待   @wait_ms      取图时按 60 Hz 节拍等（主动让出，不是问题）；@pq_wait_ms 等出图队列归还交换链贴图；
#          离 CPU 其余 = 间隔 − CPU − 节拍等 − 队列等（锁等待、被抢占、客体同步等）
#   GPU    @gpu_ms 本帧命令缓冲 GPU 忙时之和；@gpu_end_ms − @call_ms = 出图调用后多久 GPU 做完；
#          @pres_at_ms − @gpu_end_ms = GPU 做完到上屏；@drawable_ms 出图线程等 nextDrawable
# Slow-frame attribution: outputs a list of slow frames, each broken into CPU side / wait side / GPU side. Two kinds of input:
#   · Host log (boot.txt or the stream read back live via the log port, with DM_SLOWFRAME=1): contains only "[slowframe]\tframe number\t@name=value..." lines for dropped frames
#     and one denominator line per 300 frames; segmentation by reading phase uses the UIMSG in the same log; usage slowframe.py boot.txt or ... | slowframe.py -
#   · Per-frame file (DM_NVN_TSV, optionally with DM_NVN_TSV_CALLS=0) + optional boot log: slowframe.py frames.tsv [boot.txt]
# --all also lists slow frames in the launch / loading phases frame by frame
# Slow-frame definition: a present interval @presented_ms > 16.7×1.5 ms or −1 (dropped by the system) counts as one dropped frame; frames before entering the game are listed separately.
# Logs without present times (Mac without a window) fall back to @frame_ms > 25 ms, noted in the table header.
# Components (ms, all for that frame number, i.e. between the previous frame-output call and this frame-output call):
#   Interval   @frame_ms     interval between the frame-output thread's two calls to QueuePresent
#   CPU    @cpu_ms       the frame-output thread's time on CPU this frame (−1 = the thread changed); of which @sub_cpu_ms is submission replay, @upload_ms texture upload,
#                        @lib_new / @pso_new the number of synchronous library / pipeline builds; @proc_cpu_ms CPU of all threads in the whole process (to see whether other threads saturate the cores)
#   Wait   @wait_ms      waiting on the 60 Hz tick when acquiring the image (a voluntary yield, not a problem); @pq_wait_ms waiting for the frame-output queue to return a swapchain texture;
#          other off-CPU = interval − CPU − tick wait − queue wait (lock waits, preemption, guest synchronization, etc.)
#   GPU    @gpu_ms sum of GPU busy time of this frame's command buffers; @gpu_end_ms − @call_ms = how long after the frame-output call the GPU finished;
#          @pres_at_ms − @gpu_end_ms = from GPU finish to present; @drawable_ms the frame-output thread waiting on nextDrawable
import re, sys, collections, math

PERIOD = 1000 / 60
DROP = PERIOD * 1.5

def load(lines):
    """返回 (逐帧字段, 日志分母行 (judged, slow, 判据) 或 None, 原始行列表供分段) | Returns (per-frame fields, log denominator line (judged, slow, criterion) or None, list of raw lines for segmentation)"""
    F = collections.defaultdict(dict); den = None; raw = []
    for l in lines:
        raw.append(l)
        p = l.rstrip("\n").split("\t")
        if p[0].endswith("[slowframe]") and len(p) > 2:   # 日志行（前面可能带日志前缀） | Log line (may be preceded by a log prefix)
            if p[1] == "#":
                kv = dict(x.split("=", 1) for x in p[3:] if "=" in x)
                den = (int(kv["judged"]), int(kv["slow"]), kv.get("判据", "?"))
            elif p[1].isdigit():
                for x in p[2:]:
                    k, _, v = x.partition("=")
                    try: F[int(p[1])][k] = float(v)
                    except ValueError: pass
            continue
        if len(p) != 3 or not p[0].isdigit() or not p[1].startswith("@"): continue   # 末行可能被截断 | The last line may be truncated
        try: F[int(p[0])][p[1]] = float(p[2])
        except ValueError: pass
    return F, den, raw

def phases(lines):
    """UIMSG 读取画面区间 → (第一次打开读取画面的时刻, [(开, 关)…])；没有任何 UIMSG 行返回 None | UIMSG loading screen intervals → (time the loading screen was first opened, [(open, close)...]); returns None if there are no UIMSG lines at all"""
    iv, o, first, seen = [], None, None, False
    for l in lines:
        if "UIMSG" in l: seen = True
        m = re.search(r"UIMSG \[([\d.]+)s\] PanelManager (OpenPanel|UnloadPanel) LoadScreenPanel", l)
        if not m: continue
        t = float(m.group(1))
        if m.group(2) == "OpenPanel": o = t; first = t if first is None else first
        elif o is not None: iv.append((o, t)); o = None
    if o is not None: iv.append((o, 1e18))
    return (first, iv) if seen else None

def phase_of(d, ph):
    if ph is None or "@t_s" not in d: return "未分段"
    first, iv = ph
    t = d["@t_s"]
    if first is None or t < first: return "前端"
    return "读取" if any(a <= t <= b for a, b in iv) else "局内"

def g(d, k):
    return d.get(k)

def attribute(d):
    """返回 (CPU 侧 ms, 等待侧 ms, 离 CPU 其余 ms, GPU 侧描述, 判定) | Returns (CPU-side ms, wait-side ms, other off-CPU ms, GPU-side description, verdict)"""
    fr, cpu = g(d, "@frame_ms"), g(d, "@cpu_ms")
    wait, pq = g(d, "@wait_ms") or 0.0, g(d, "@pq_wait_ms") or 0.0
    cpu_ok = cpu is not None and cpu >= 0
    other = fr - cpu - wait - pq if fr is not None and cpu_ok else None
    call, g1, pres = g(d, "@call_ms"), g(d, "@gpu_end_ms"), g(d, "@pres_at_ms")
    gpu_after = g1 - call if g1 is not None and call is not None else None
    disp = pres - g1 if pres is not None and g1 is not None else None
    # 判定：出图调用间隔没超（CPU 按时交了帧）→ GPU / 显示侧；超了 → 看间隔里最大的一块
    # Verdict: present call interval not exceeded (the CPU delivered the frame on time) → GPU / display side; exceeded → look at the largest piece within the interval
    if fr is None: verdict = "缺 @frame_ms"
    elif fr <= DROP:
        if gpu_after is not None and gpu_after > PERIOD: verdict = "GPU 侧（出图调用后 %.1f ms 才做完）" % gpu_after
        elif (g(d, "@drawable_ms") or 0) > PERIOD / 2: verdict = "显示侧（等 drawable %.1f ms）" % d["@drawable_ms"]
        else: verdict = "CPU 按时、GPU 按时：看前几帧积压 / 系统丢弃"
    elif not cpu_ok: verdict = "间隔超时，缺 @cpu_ms（旧日志或换线程），无法拆"
    else:
        parts = {"CPU": cpu, "队列等": pq, "离 CPU 其余（锁 / 抢占）": other}
        k = max(parts, key=parts.get)
        verdict = "%s 主导 %.1f / %.1f ms" % (k, parts[k], fr)
    return cpu if cpu_ok else None, wait + pq, other, gpu_after, disp, verdict

def fmt(v, w=6):
    return ("%*.1f" % (w, v)) if v is not None else " " * (w - 1) + "-"

def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    if not args: sys.exit(__doc__ or "用法：slowframe.py frames.tsv [boot.txt] [--all]")
    show_all = "--all" in argv
    src = sys.stdin if args[0] == "-" else open(args[0], errors="replace")
    F, den, raw = load(src)
    ph = phases(open(args[1], errors="replace") if len(args) > 1 else raw)
    frames = sorted(F)
    # 有上屏时刻但缺间隔仍是窗口日志，不能用 CPU 提交间隔代替上屏间隔。
    # A present timestamp without an interval still identifies a windowed log; CPU submission intervals are not presentation intervals.
    has_pres = any("@presented_ms" in F[f] or "@pres_at_ms" in F[f] for f in frames)
    if den is not None:   # 日志输入：只有掉帧帧的行，分母来自宿主每 300 帧的统计行 | Log input: only lines for dropped frames; the denominator comes from the host's stats line every 300 frames
        print(f"# 日志输入（DM_SLOWFRAME）：宿主判据 {den[2]}，宿主统计 可判 {den[0]} 帧、掉帧 {den[1]} 帧（截至最后一行分母）；日志里掉帧行 {len(frames)} 帧")
    key = "@presented_ms" if has_pres else "@frame_ms"
    def slow(d):
        v = d.get(key)
        return v is not None and (v > DROP or (has_pres and v == -1))
    # 0 / 缺值不可判；只接受有限正间隔及上屏丢弃标记 −1，避免稀释分母。
    # Zero / missing values are unjudged; accept only finite positive intervals and the presentation discard marker -1, so the denominator is not diluted.
    def valid(d):
        v = d.get(key)
        return v is not None and math.isfinite(v) and (v > 0 or (has_pres and v == -1))
    judged = [f for f in frames if valid(F[f])]
    S = [f for f in judged if slow(F[f])]
    n_cpu = sum(1 for f in frames if F[f].get("@cpu_ms", -1) >= 0)
    print(f"# 帧 {len(frames)}（{frames[0] if frames else '-'}–{frames[-1] if frames else '-'}）；判据 {key} > {DROP:.1f} ms"
          + (" 或 −1" if has_pres else "（无上屏时刻，退回出图调用间隔）")
          + f"；可判 {len(judged)} 帧；有 @cpu_ms {n_cpu} 帧；分段 {'boot 日志' if ph else '无'}")
    by = collections.Counter(phase_of(F[f], ph) for f in judged)
    sl = collections.Counter(phase_of(F[f], ph) for f in S)
    if den is None: print("# 各段 慢帧 / 可判帧：" + "；".join(f"{p} {sl[p]}/{by[p]}" for p in ("前端", "读取", "局内", "未分段") if by[p]))
    else: print("# 各段 慢帧：" + ("；".join(f"{p} {sl[p]}" for p in ("前端", "读取", "局内", "未分段") if sl[p]) or "无"))
    rows = [f for f in S if show_all or phase_of(F[f], ph) in ("局内", "未分段")]
    print("帧\tt_s\t阶段\t上屏ms\t间隔ms\tCPU\t其中提交\t上传\t建库/管线\t进程CPU\t节拍+队列等\t离CPU其余\tGPU忙\tGPU完成-调用\t完成-上屏\t等drawable\t前3帧最大间隔\t判定")
    verdicts = collections.Counter()
    for f in rows:
        d = F[f]
        cpu, waits, other, gafter, disp, verdict = attribute(d)
        prev = d.get("@prev3_max_frame_ms", max((F[p].get("@frame_ms", 0) for p in range(f - 3, f) if p in F), default=None))
        verdicts[verdict.split("（")[0].split(" 主导")[0]] += 1
        cr = "%d/%d" % (d.get("@lib_new", 0), d.get("@pso_new", 0)) if "@lib_new" in d else "-"
        print("\t".join([str(f), fmt(d.get("@t_s"), 1).strip(), phase_of(d, ph), fmt(d.get("@presented_ms")).strip(), fmt(d.get("@frame_ms")).strip(),
                         fmt(cpu).strip(), fmt(d.get("@sub_cpu_ms")).strip(), fmt(d.get("@upload_ms")).strip(), cr, fmt(d.get("@proc_cpu_ms")).strip(),
                         fmt(waits).strip(), fmt(other).strip(), fmt(d.get("@gpu_ms")).strip(), fmt(gafter).strip(), fmt(disp).strip(),
                         fmt(d.get("@drawable_ms")).strip(), fmt(prev).strip(), verdict]))
    print("# 列出 %d 帧%s；判定汇总：%s" % (len(rows), "" if show_all else "（只列局内 / 未分段，--all 看全部）",
                                     "；".join(f"{k} {v}" for k, v in verdicts.most_common()) or "无"))

if __name__ == "__main__":
    main(sys.argv[1:])
