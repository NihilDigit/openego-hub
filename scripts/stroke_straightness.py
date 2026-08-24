"""直尺锚点语料的直线度:对每条笔画自己做最小二乘拟合,量垂直偏差。

用途是回归验收。`dvr20260824_012655` 是贴着直尺画的三条长笔画,真值就是「直的」,
所以这份语料不需要厂商当参照物——它是四份对拍语料之外唯一一个绝对基准。
重写之前的基线是三条笔画各 0.27..0.32 mm 中位偏差,改完不应比这个差。

笔画按 DvrReplay 输出的 strokeId 分组;旧的 CSV 没有那一列时退回按接触点 id 分组,
那样会把复用同一个号的两条笔画拼在一起,直线度会假性变差,所以只在没有别的选择时用。

    python scripts/stroke_straightness.py <replay.csv> [--min-samples 50]
"""
import argparse
import csv
import math
from collections import defaultdict

# 传感器格间距。60x40 格覆盖 2560x1600 像素的面板,格心间距按 4.5 mm 计。
CELL_PITCH_MM = 4.5


def read_strokes(path, min_samples):
    byStroke = defaultdict(list)
    fellBack = False
    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        contactsCol = header.index("contacts")
        for row in reader:
            if len(row) <= contactsCol or not row[contactsCol]:
                continue
            for item in row[contactsCol].split(";"):
                p = item.split(":")
                if len(p) < 6 or p[3] != "1":
                    continue
                if len(p) > 10 and p[10] != "0":
                    key = ("stroke", int(p[10]))
                else:
                    key = ("track", int(p[0]))
                    fellBack = True
                byStroke[key].append((float(p[4]), float(p[5])))
    return {k: v for k, v in byStroke.items() if len(v) >= min_samples}, fellBack


def median_deviation_mm(points):
    """点到最小二乘直线的垂直偏差中位数。

    拟合用主轴而不是 y = kx + b:竖直的笔画在后者下斜率发散,这份语料正好有一条。
    """
    n = len(points)
    mx = sum(x for x, _ in points) / n
    my = sum(y for _, y in points) / n
    sxx = sum((x - mx) ** 2 for x, _ in points)
    syy = sum((y - my) ** 2 for _, y in points)
    sxy = sum((x - mx) * (y - my) for x, y in points)
    # 主轴方向:协方差矩阵的最大特征向量。
    theta = 0.5 * math.atan2(2 * sxy, sxx - syy)
    dx = math.cos(theta)
    dy = math.sin(theta)
    devs = sorted(abs(-(x - mx) * dy + (y - my) * dx) for x, y in points)
    return devs[n // 2] * CELL_PITCH_MM


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("replay")
    ap.add_argument("--min-samples", type=int, default=50,
                    help="短于这个采样数的笔画不参与,点击段没有直线度可言")
    args = ap.parse_args()

    strokes, fellBack = read_strokes(args.replay, args.min_samples)
    if fellBack:
        print("注意:这份 CSV 没有 strokeId 列,退回按接触点 id 分组,数值会偏悲观")
    if not strokes:
        print("没有够长的笔画")
        return 1
    for key in sorted(strokes):
        pts = strokes[key]
        print(f"  {key[0]} {key[1]:>4}  {len(pts):>4} 点  中位垂直偏差 "
              f"{median_deviation_mm(pts):.3f} mm")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
