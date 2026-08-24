#!/usr/bin/env python3
"""把厂商实现的逐帧上报与我们自己的重放结果对齐,量出分歧。

重写求解层期间,这是唯一不依赖人手感的验收口径:同一份录制,同一批帧号,
两边各报了几个接触点、报在哪、笔画有没有中途断开。

用法:
    oracle_compare.py <oracle.csv> <ours.csv> [--verbose]

oracle.csv 由 C:\\Tools\\re\\oracle\\oracle.exe 产出,一行一个接触点;
ours.csv 由 DvrReplay 产出,一行一帧、接触点挤在最后一列。两边的帧号同源,
都来自同一个 .dvrbin,所以可以直接按帧号对齐。

不比对什么:
  - 绝对坐标的系统性偏置。两边的格→像素换算未必同构,先看形状与连续性。
  - id 的具体取值。只看 id 的分合结构,不看编号。
"""

import argparse
import csv
import sys
from collections import defaultdict

# TSACore 的事件取值,实测:2=down 4=move 64=up。
EVENT_DOWN = 2
EVENT_UP = 64


def read_oracle(path):
    """→ {frame: [(id, x_cell, y_cell, event, edgeFlag)]}"""
    out = defaultdict(list)
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            # 用像素坐标而不是 dim1/dim2:后者是整数格号,量化噪声本身就有半格,
            # 会淹没真正的算法分歧。像素坐标是亚格的,与我们的浮点格心可比。
            out[int(row["frame"])].append((
                int(row["touchIdx"]),
                float(row["x"]),
                float(row["y"]),
                int(row["event"]),
                int(row["edgeFlag"]),
            ))
    return out


def read_ours(path):
    """→ {frame: [(id, x_cell, y_cell, strokeId)]},只取被上报的接触点。

    最后一列里,接触点之间用**分号**分隔,接触点内部的字段用冒号分隔:
    id:state:reportEvent:isReported:x:y:area:signalSum:sizeMm:peakSignal:strokeId:strokePhase
    末两个字段是第 5 级(笔画层)加的,旧的 CSV 没有,读出来记 0。

    分号不是随手选的:整个文件是逗号分隔的,这一列如果也用逗号,列数就会随接触点
    数变化。曾经在这里读错过——按逗号切只会拿到第一个接触点,而单指语料照样
    100% 吻合,于是多指语料的数字全错而毫无征兆。
    """
    out = defaultdict(list)
    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        frameCol = header.index("frame")
        contactsCol = header.index("contacts")
        for row in reader:
            if len(row) <= contactsCol:
                continue
            frame = int(row[frameCol])
            cell = row[contactsCol]
            if not cell:
                continue
            for item in cell.split(";"):
                p = item.split(":")
                if len(p) < 6 or p[3] != "1":
                    continue
                strokeId = int(p[10]) if len(p) > 10 else 0
                out[frame].append((int(p[0]), float(p[4]), float(p[5]), strokeId))
    return out


def stroke_id_stats(ours):
    """按笔画层自己发的编号数笔画。

    有了显式编号之后,按帧连续性数笔画就成了错的量具:接续判据本来就允许笔画跨过
    几帧空档,那些空档会被连续性计数当成新笔画。编号是单调递增且不复用的,直接数
    不同的编号即可。

    返回 (笔画数, 没归属的接触点数)。后者应当是 0——不为 0 说明有接触点没经过
    笔画层,得先查那条路径,再看笔画数。
    """
    ids = set()
    orphans = 0
    for frame in ours:
        for c in ours[frame]:
            if len(c) < 4 or c[3] == 0:
                orphans += 1
            else:
                ids.add(c[3])
    return len(ids), orphans


def stroke_stats(frames_to_contacts, id_index=0, gap_tolerance=4):
    """数笔画数与笔画中途的断开次数。

    按 id 收集它出现过的帧号,再按帧号是否连续切段:间隔在 gap_tolerance 帧以内
    算同一条笔画中途断了一下,超过就算两条不同的笔画。

    不要写成「这个 id 以前出现过就算重连」。**id 会在抬起之后被回收**,
    那样数会把「换了一根手指、复用了同一个编号」当成断触。这个错误真发生过:
    边缘补偿修好之后,四边语料的角落点击段多识别出九次点击,每次复用一个旧 id,
    于是「断触」从 33 涨到 42,看起来像是修坏了,其实是多认出了九次点击。

    gap_tolerance 取 4 帧,约 33 ms:比这更长的空档,手指多半真的抬起来了。
    """
    frames_by_id = defaultdict(list)
    for frame in sorted(frames_to_contacts):
        for c in frames_to_contacts[frame]:
            frames_by_id[c[id_index]].append(frame)

    strokes = breaks = 0
    for frames in frames_by_id.values():
        strokes += 1
        for prev, cur in zip(frames, frames[1:]):
            gap = cur - prev
            if gap <= 1:
                continue
            if gap <= gap_tolerance + 1:
                breaks += 1
            else:
                strokes += 1
    return strokes, breaks


def event_stats(oracle):
    """厂商侧按事件数笔画:落下几次、抬起几次。两者应当相等。

    数的是**跳变**不是行数。同一个事件值会在连续若干帧上重复出现,按行数会把
    一次抬起算成几十次——这与 DvrReplay 那边不能按 reportEvent 行数数 down
    是同一个陷阱。槽位号在抬起后立即复用,所以按槽位分别追踪。
    """
    prev = {}
    down = up = 0
    for f in sorted(oracle):
        for (slot, _, _, ev, _) in oracle[f]:
            was = prev.get(slot)
            if ev == EVENT_DOWN and was != EVENT_DOWN:
                down += 1
            elif ev == EVENT_UP and was != EVENT_UP:
                up += 1
            prev[slot] = ev
    return down, up


def self_test():
    """用已知答案的输入验一遍两个解析器和笔画计数。

    加这个是因为这两处都静默读错过:接触点串按逗号切(只拿到第一个接触点),
    以及把 id 回收当成断触。两次都是单指语料照样吻合、毫无征兆。
    """
    import io
    import tempfile
    import os

    failures = []

    def check(name, actual, expected):
        if actual != expected:
            failures.append(f"{name}: 得到 {actual},应为 {expected}")

    # 一帧两个接触点,分号分隔;另一帧一个被上报、一个未被上报。
    ours_csv = (
        "frame,contacts\n"
        "0,1:0:2:1:10.0:20.0:8:100:6.0;2:0:2:1:30.0:20.0:8:100:6.0\n"
        "1,1:0:4:1:11.0:20.0:8:100:6.0;2:0:4:0:31.0:20.0:8:100:6.0\n"
        "2,\n"
        "9,1:0:2:1:50.0:20.0:8:100:6.0\n"
    )
    fd, path = tempfile.mkstemp(suffix=".csv", text=True)
    with io.open(fd, "w", newline="") as f:
        f.write(ours_csv)
    try:
        ours = read_ours(path)
        check("帧 0 的接触点数", len(ours[0]), 2)
        check("帧 1 只算被上报的", len(ours[1]), 1)
        check("空串不产生接触点", len(ours.get(2, [])), 0)
        # id 1 出现在帧 0、1、9。0→1 连续;1→9 间隔 8 帧,超过容差,算两条笔画。
        # id 2 只在帧 0 被上报,自成一条。合计三条笔画,零次中途断开。
        strokes, breaks = stroke_stats(ours)
        check("笔画数", strokes, 3)
        check("中途断开数", breaks, 0)

        # 同一个 id 隔两帧回来,应当算一次中途断开而不是新笔画。
        gap_csv = (
            "frame,contacts\n"
            "0,1:0:2:1:10.0:20.0:8:100:6.0\n"
            "3,1:0:4:1:11.0:20.0:8:100:6.0\n"
        )
        with io.open(path, "w", newline="") as f:
            f.write(gap_csv)
        strokes, breaks = stroke_stats(read_ours(path))
        check("短空档算断开而非新笔画(笔画数)", strokes, 1)
        check("短空档算断开而非新笔画(断开数)", breaks, 1)

        # 带笔画编号的一列:两条轨迹(id 1、2)属于同一条笔画 7,按编号数是一条。
        # 第三个接触点没有编号,应当被数成「没归属」而不是一条笔画。
        stroke_csv = (
            "frame,contacts\n"
            "0,1:0:2:1:10.0:20.0:8:100:6.0:900:7:1\n"
            "5,2:0:4:1:11.0:20.0:8:100:6.0:900:7:1\n"
            "6,3:0:4:1:40.0:20.0:8:100:6.0:900:0:1\n"
        )
        with io.open(path, "w", newline="") as f:
            f.write(stroke_csv)
        idStrokes, orphans = stroke_id_stats(read_ours(path))
        check("按编号数笔画", idStrokes, 1)
        check("没归属的接触点", orphans, 1)
    finally:
        os.unlink(path)

    if failures:
        for f in failures:
            print("自检失败 " + f)
        return 1
    print("自检通过")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("oracle", nargs="?")
    ap.add_argument("ours", nargs="?")
    ap.add_argument("--self-test", action="store_true",
                    help="用已知答案的输入验一遍解析与计数,不读语料")
    ap.add_argument("--verbose", action="store_true",
                    help="列出接触点数不一致的帧")
    ap.add_argument("--match-radius", type=float, default=3.0,
                    help="配对两边接触点的最大距离,单位是格")
    ap.add_argument("--grid", default="60x40",
                    help="传感器格数,用来把我们的格心折成像素以便配对")
    ap.add_argument("--res", default="2560x1600",
                    help="面板分辨率,厂商的 x/y 就在这个量纲里")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.oracle or not args.ours:
        ap.error("需要两个 CSV 路径,或者用 --self-test")

    cols, rows = (int(v) for v in args.grid.lower().split("x"))
    resX, resY = (int(v) for v in args.res.lower().split("x"))
    # 配对用的临时换算。真正的换算关系随后由最小二乘拟合出来,这里只要够近就行。
    sx, sy = resX / cols, resY / rows

    oracle = read_oracle(args.oracle)
    ours = read_ours(args.ours)
    frames = sorted(set(oracle) | set(ours))
    if not frames:
        print("两边都没有接触点,无从比对")
        return 1

    both = agree = 0
    onlyOracle = onlyOurs = 0
    countMismatch = []
    deltas = []

    for f in frames:
        a = oracle.get(f, [])
        b = ours.get(f, [])
        if a and b:
            both += 1
            if len(a) == len(b):
                agree += 1
            else:
                countMismatch.append((f, len(a), len(b)))
            # 贪心最近邻配对。数量少(≤16),不值得上匈牙利。
            # 在像素空间里配,我们的格心先按标称换算折过去。
            free = list(range(len(b)))
            radiusPx = (args.match_radius * max(sx, sy)) ** 2
            for (_, ax, ay, _, _) in a:
                best, bestD = None, radiusPx
                for j in free:
                    dx = b[j][1] * sx - ax
                    dy = b[j][2] * sy - ay
                    d = dx * dx + dy * dy
                    if d < bestD:
                        best, bestD = j, d
                if best is not None:
                    free.remove(best)
                    # 存的是「我们的格心」与「厂商的像素」,换算关系稍后拟合。
                    deltas.append((b[best][1], b[best][2], ax, ay))
        elif a:
            onlyOracle += 1
        elif b:
            onlyOurs += 1

    oDown, oUp = event_stats(oracle)
    oStrokes, oBreaks = stroke_stats(oracle)
    uStrokes, uBreaks = stroke_stats(ours)

    print(f"帧数 {len(frames)}")
    print(f"  两边都有接触点   {both}")
    print(f"  只有厂商有       {onlyOracle}")
    print(f"  只有我们有       {onlyOurs}")
    if both:
        print(f"  接触点数一致     {agree}/{both}  ({100.0 * agree / both:.1f}%)")
    print()
    print("笔画分合(中途断开是边缘掉触的直接症状)")
    print(f"  厂商   笔画 {oStrokes}  中途断开 {oBreaks}   （事件计数:落下 {oDown} 抬起 {oUp}）")
    print(f"  我们   笔画 {uStrokes}  中途断开 {uBreaks}   （按帧连续性数,与厂商同一把尺）")
    idStrokes, orphans = stroke_id_stats(ours)
    if idStrokes or orphans:
        print(f"  我们   笔画 {idStrokes}（按笔画层的编号数)"
              + (f"   没归属的接触点 {orphans}" if orphans else ""))
    print()
    if deltas:
        n = len(deltas)
        print(f"配对成功的接触点 {n} 对")
        # 先把「格心 → 像素」的换算拟合出来。两边的原点与刻度约定未必相同,
        # 不先扣掉这层,残差里全是约定差,看不见算法分歧。
        for axis, ourIdx, oracleIdx, nominal in (("x", 0, 2, sx), ("y", 1, 3, sy)):
            us = [d[ourIdx] for d in deltas]
            them = [d[oracleIdx] for d in deltas]
            mu, mv = sum(us) / n, sum(them) / n
            cov = sum((u - mu) * (v - mv) for u, v in zip(us, them))
            var = sum((u - mu) ** 2 for u in us)
            scale = cov / var if var else nominal
            offset = mv - scale * mu
            resid = sorted(abs(v - (scale * u + offset)) / (scale or 1)
                           for u, v in zip(us, them))
            print(f"  {axis}: 拟合 像素 = {scale:.3f} * 格 {offset:+.2f}"
                  f"   (标称刻度 {nominal:.3f})")
            print(f"     残差 中位 {resid[n // 2]:.3f} 格   "
                  f"p95 {resid[int(n * 0.95)]:.3f}   最大 {resid[-1]:.3f}")
    else:
        print("没有配对成功的接触点——两边的坐标系可能不同构,先查格→像素换算")

    if args.verbose and countMismatch:
        print(f"\n接触点数不一致的帧({len(countMismatch)} 个,列前 40):")
        for f, na, nb in countMismatch[:40]:
            print(f"  帧 {f}: 厂商 {na}, 我们 {nb}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
