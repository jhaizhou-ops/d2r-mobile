#!/usr/bin/env python3
# 界面几何检查器：从截图里量「血条金黄分段」的节拍/位置，给出两图之间的缩放与平移。
# 世界档实验的判据工具：同一出图配置下，开 3D 比例前后界面应逐像素一致（scale≈1.000）；
# 出现 0.6 之类的 scale 就是 UI 被改变了渲染尺寸（2026-10-16 世界档 UI 变形的特征）。
#
# UI geometry checker: measures the segmented golden life bar in a screenshot and reports the
# scale/offset between two shots. World-tier acceptance rule: within one output config the UI
# must be identical with and without the 3D scale (scale≈1.000). A scale near 0.6 is the
# UI-render-size deformation signature seen on 2026-10-16.
#
# 用法 / usage: ui_geom.py shot.png [ref.png]
#   只给一图：打印金黄分段表。给两图：再打印把 ref 映射到 shot 的 (scale, dx, dy) 与判定。
import sys
from PIL import Image

def yellow(r, g, b):
    return r > 170 and g > 140 and b < 160 and (r - b) > 60

def find_bar(path):
    im = Image.open(path).convert("RGB")
    W, H = im.size
    px = im.load()
    best_y, best_n = -1, 0
    for y in range(int(H * 0.35), int(H * 0.97), 2):
        n = 0
        for x in range(0, W, 2):
            if yellow(*px[x, y]):
                n += 1
        if n > best_n:
            best_n, best_y = n, y
    if best_y < 0:
        return None
    # 在最佳行附近做逐行细分，取黄像素最多的一行（±4）
    for y in range(max(0, best_y - 4), min(H, best_y + 5)):
        n = sum(1 for x in range(0, W) if yellow(*px[x, y]))
        if n > best_n:
            best_n, best_y = n, y
    runs = []
    inrun = False
    for x in range(W):
        ok = yellow(*px[x, best_y])
        if ok and not inrun:
            inrun, start = True, x
        elif not ok and inrun:
            inrun = False
            if x - start >= 6:
                runs.append((start, x - 1))
    if inrun and W - start >= 6:
        runs.append((start, W - 1))
    # 合并微生物（间隔 ≤ 2px）——文字/光泽会切断金黄
    merged = []
    for s, e in runs:
        if merged and s - merged[-1][1] <= 2:
            merged[-1] = (merged[-1][0], e)
        else:
            merged.append((s, e))
    return {"w": W, "h": H, "y": best_y, "runs": merged}

def stats(bar):
    runs = bar["runs"]
    if len(runs) < 2:
        return None
    widths = [e - s + 1 for s, e in runs]
    starts = [s for s, e in runs]
    pitches = [starts[i + 1] - starts[i] for i in range(len(starts) - 1)]
    return {
        "n": len(runs),
        "x0": runs[0][0], "x1": runs[-1][1],
        "wseg": sum(widths) / len(widths),
        "pitch": sum(pitches) / len(pitches) if pitches else 0,
        "y": bar["y"],
    }

def main():
    shot = sys.argv[1]
    bar = find_bar(shot)
    if not bar or not bar["runs"]:
        print(f"{shot}: 未找到血条金黄分段（图不对或场景里 HUD 隐藏）")
        return 2
    s = stats(bar)
    print(f"{shot}: {bar['w']}x{bar['h']} 行 y={bar['y']} 分段 {bar['runs']}")
    print(f"    段数 {s['n']}  段宽 {s['wseg']:.1f}  节拍 {s['pitch']:.1f}  起点 x0={s['x0']}  终点 x1={s['x1']}")
    if len(sys.argv) < 3:
        return 0
    ref = find_bar(sys.argv[2])
    r = stats(ref) if ref else None
    if not r:
        print(f"参考图 {sys.argv[2]} 无血条，无法对比")
        return 2
    kw = (s['wseg'] / r['wseg']) if r['wseg'] else 0
    kp = (s['pitch'] / r['pitch']) if r['pitch'] else 0
    dx = s['x0'] - r['x0']
    dy = s['y'] - r['y']
    print(f"对比 ref={sys.argv[2]}: 段宽比 {kw:.3f}  节拍比 {kp:.3f}  平移 dx={dx} dy={dy}")
    k = (kw + kp) / 2 if kp else kw
    if abs(k - 1.0) <= 0.02 and abs(dx) <= 12 and abs(dy) <= 12:
        print("判定：界面几何一致（缩放≈1）")
    else:
        print(f"判定：界面几何不一致（缩放≈{k:.3f}，超过 ±2% 或平移超 ±12px）")
    return 0

if __name__ == "__main__":
    sys.exit(main())
