#!/usr/bin/env python3
"""按笔画聚合我们自己的接触点,用厂商的上报与否当标签,看哪些特征能把误报分开。

为什么这么做:掌抑制我们不照抄厂商的机制(它是整帧否决,一个长条会把同帧另外三四个
正常触点一起带走),但厂商**判得对不对**是可以拿来当标签的——同一份录制,同一批帧,
它报了而我们也报的算真阳性,我们报了而它没报的算误报。

特征取 Chromium 的那一套(`neural_stylus_palm_detection_filter`):按 tracking id 聚合
成笔画,取笔画级的最大尺寸、最大面积、路径长度、首尾直线距离、同时存在的邻居数。
Chromium 的短笔画启发式用的是「长轴 ≥ 20 mm 或面积 ≥ 400 mm²」,阈值在我们的量纲上
不能直接用——这个脚本就是用来重新标定它们的。

用法:
    palm_features.py <oracle.csv> <ours.csv> [--grid 60x40] [--res 2560x1600]
"""

import argparse
import csv
import sys
from collections import defaultdict


# 传感器间距,列 4.5 mm、行 4.125 mm。网格不是方的,按单值处理会让面积高估约 9%。
COL_PITCH_MM = 4.5
ROW_PITCH_MM = 4.125
CELL_AREA_MM2 = COL_PITCH_MM * ROW_PITCH_MM


def read_ours(path):
    """→ {frame: [(id, x, y, area, signalSum, sizeMm)]},只取被上报的。"""
    out = defaultdict(list)
    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        fi = header.index("frame")
        ci = header.index("contacts")
        for row in reader:
            if len(row) <= ci or not row[ci]:
                continue
            for item in row[ci].split(";"):
                p = item.split(":")
                if len(p) < 9 or p[3] != "1":
                    continue
                # 第 10 个字段是峰值信号,旧的 CSV 没有,缺了就填 0。
                peak = int(p[9]) if len(p) >= 10 else 0
                out[int(row[fi])].append((int(p[0]), float(p[4]), float(p[5]),
                                          int(p[6]), int(p[7]), float(p[8]), peak))
    return out


def read_oracle(path, sx, sy):
    """→ {frame: [(x_cell, y_cell)]}。厂商的坐标是像素,折回格便于配对。"""
    out = defaultdict(list)
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            out[int(r["frame"])].append((float(r["x"]) / sx, float(r["y"]) / sy))
    return out


def build_strokes(ours, gap_tolerance=4):
    """按 id 切成笔画。间隔超过容差就算两条,与 oracle_compare.py 同一套口径。"""
    frames_by_id = defaultdict(list)
    for frame in sorted(ours):
        for c in ours[frame]:
            frames_by_id[c[0]].append((frame, c))

    strokes = []
    for cid, seq in frames_by_id.items():
        cur = [seq[0]]
        for prev, item in zip(seq, seq[1:]):
            if item[0] - prev[0] > gap_tolerance + 1:
                strokes.append(cur)
                cur = [item]
            else:
                cur.append(item)
        strokes.append(cur)
    return strokes


def stroke_features(stroke, ours, oracle, match_radius_cells=3.0):
    """Chromium 那一套笔画级特征,加上用厂商上报算出的标签。"""
    xs = [s[1][1] for s in stroke]
    ys = [s[1][2] for s in stroke]
    areas = [s[1][3] for s in stroke]
    sigs = [s[1][4] for s in stroke]
    sizes = [s[1][5] for s in stroke]
    peaks = [s[1][6] for s in stroke]

    path = 0.0
    for (x0, y0), (x1, y1) in zip(zip(xs, ys), zip(xs[1:], ys[1:])):
        path += ((x1 - x0) ** 2 + (y1 - y0) ** 2) ** 0.5
    straight = ((xs[-1] - xs[0]) ** 2 + (ys[-1] - ys[0]) ** 2) ** 0.5

    # 同时存在的接触点数:整条笔画上取最大值。Chromium 的邻居特征同源。
    maxConcurrent = max(len(ours[s[0]]) for s in stroke)

    # 标签:这条笔画有多少帧能在厂商那边找到同位置的接触点。
    agreed = 0
    r2 = match_radius_cells ** 2
    for frame, c in stroke:
        for ox, oy in oracle.get(frame, ()):
            if (ox - c[1]) ** 2 + (oy - c[2]) ** 2 <= r2:
                agreed += 1
                break

    return {
        "frames": len(stroke),
        # 信号量分峰值代理与总和两种:厂商的按下确认门槛比的是**峰值**,
        # 我们手上只有区内总和与面积,总和除面积是每格均值,是峰值的粗代理。
        "maxPeakSignal": max(peaks),
        "maxSignalSum": max(sigs),
        "maxSignalPerCell": max(s / max(1, a) for s, a in zip(sigs, areas)),
        "maxAreaCells": max(areas),
        "maxAreaMm2": max(areas) * CELL_AREA_MM2,
        "maxSizeMm": max(sizes),
        "pathCells": path,
        "straightCells": straight,
        "maxConcurrent": maxConcurrent,
        "agreedFrames": agreed,
        "agreeRatio": agreed / len(stroke),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("oracle")
    ap.add_argument("ours")
    ap.add_argument("--grid", default="60x40")
    ap.add_argument("--res", default="2560x1600")
    ap.add_argument("--agree-threshold", type=float, default=0.5,
                    help="厂商认可的帧数占比高于此值算真阳性")
    args = ap.parse_args()

    cols, rows = (int(v) for v in args.grid.lower().split("x"))
    resX, resY = (int(v) for v in args.res.lower().split("x"))
    sx, sy = resX / cols, resY / rows

    ours = read_ours(args.ours)
    oracle = read_oracle(args.oracle, sx, sy)
    strokes = build_strokes(ours)

    feats = [stroke_features(s, ours, oracle) for s in strokes]
    good = [f for f in feats if f["agreeRatio"] >= args.agree_threshold]
    bad = [f for f in feats if f["agreeRatio"] < args.agree_threshold]

    print(f"笔画 {len(feats)} 条:厂商认可 {len(good)},厂商不认 {len(bad)}")
    print()

    keys = ["frames", "maxPeakSignal", "maxSignalSum", "maxSignalPerCell",
            "maxAreaCells", "maxAreaMm2", "maxSizeMm",
            "pathCells", "straightCells", "maxConcurrent"]
    print(f"{'特征':<16}{'认可 中位':>12}{'不认 中位':>12}{'认可 p10':>12}{'不认 p90':>12}")
    for k in keys:
        g = sorted(f[k] for f in good)
        b = sorted(f[k] for f in bad)
        if not g or not b:
            continue
        print(f"{k:<16}{g[len(g)//2]:>12.1f}{b[len(b)//2]:>12.1f}"
              f"{g[len(g)//10]:>12.1f}{b[-max(1,len(b)//10)]:>12.1f}")

    # 单特征能分到什么程度:扫一遍阈值,报最好的那个。
    print()
    # 两个方向都扫:有些特征是「大了才是掌」,有些是「小了才是假」。
    print("单特征阈值扫描(误伤按三倍计,宁可漏压):")
    for k in keys:
        best = None
        vals = sorted({f[k] for f in feats})
        for t in vals:
            for direction in (">", "<"):
                if direction == ">":
                    tp = sum(1 for f in bad if f[k] > t)
                    fp = sum(1 for f in good if f[k] > t)
                else:
                    tp = sum(1 for f in bad if f[k] < t)
                    fp = sum(1 for f in good if f[k] < t)
                score = tp - 3 * fp
                if best is None or score > best[0]:
                    best = (score, direction, t, tp, fp)
        if best:
            _, d, t, tp, fp = best
            print(f"  {k:<17} 判掌条件 {d} {t:>9.1f}   压掉误报 {tp:>3}/{len(bad)}"
                  f"   误伤 {fp:>3}/{len(good)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
