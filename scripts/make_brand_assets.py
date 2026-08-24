# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow>=10"]
# ///
"""从品牌 SVG 生成全部图标资源。

    uv run scripts/make_brand_assets.py

产物三样,都进仓库:

    Tools/EGoTouchSettings/Assets/OpenEGoHub.ico   设置窗口、任务栏、开始菜单
    Common/include/BrandMark.h                      托盘自绘用的几何常量
    Tools/EGoTouchSettings/MainWindow.xaml 的标记区块  关于页那三层等高线

标记本身是三层椭圆,几何只写在 Assets/brand/openego-hub.svg 里。这个脚本按
元素 id 把它们读出来,不重新抄一遍坐标——托盘那条 D2D 路径与这里的栅格化必须
是同一组数,各写一份迟早会漂。

栅格化不借 SVG 渲染器:三个椭圆用 Pillow 直接画就够了,而 cairosvg 一类在
Windows 上要装原生库。描边按 SVG 语义画在路径中心线两侧(外扩 w/2 减去内缩
w/2),不是 Pillow 的 outline= 那种单向宽度。
"""

from __future__ import annotations

import re
import struct
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path

from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parent.parent
SVG = REPO / "Assets" / "brand" / "openego-hub.svg"
ICO = REPO / "Tools" / "EGoTouchSettings" / "Assets" / "OpenEGoHub.ico"
HEADER = REPO / "Common" / "include" / "BrandMark.h"
MAIN_XAML = REPO / "Tools" / "EGoTouchSettings" / "MainWindow.xaml"

# README 用的位图。SVG 里的 currentColor 在 <img> 里没有上下文可继承，GitHub 会按
# 初始值当成黑色，深色主题下就看不见了，所以这里出一张着色好的 PNG。
README_PNG = REPO / "Assets" / "brand" / "openego-hub-256.png"

# MainWindow.xaml 里由本脚本维护的那一段的边界。
#
# 几何**内联**在用它的那个 Path 上,不做成 x:Key 资源。放进 Application.Resources 再用
# {StaticResource} 取,会让**从不激活**的那条启动路径(托盘为了弹通知而起的后台实例)在
# Microsoft.UI.Xaml 里抛 0xC000027B 直接退出,于是托盘再也找不到桥接窗口、点托盘图标
# 什么都不发生。二分到这一步:去掉这三个 Path 就好,换成内联几何也好,只有走资源查找会崩。
XAML_BEGIN = "                                    <!-- BRAND-MARK:BEGIN -->"
XAML_END = "                                    <!-- BRAND-MARK:END -->"

# 单色描边在小尺寸上会糊。<= 这个尺寸只画外轮廓与内核,且外轮廓不再半透明。
# 同一个判据在 EGoTouchTray.cpp 里也用,由生成的头文件带过去。
SMALL_SIZE_PX = 24

# 去掉中间那层之后墨迹少了一圈,内核按这个倍数放大补回来。不补的话小尺寸下
# 标记看着是「一个大空圈里点了一下」,与大尺寸的观感对不上。
SMALL_CORE_SCALE = 1.3

# .ico 里放哪些档。16/20/24 是壳层实际会取的三档(SM_CXSMICON 在 100%/125%/150%),
# 256 是资源管理器的特大图标。
ICO_SIZES = [16, 20, 24, 32, 48, 64, 128, 256]

# 图标是静态资源,做不到跟随系统主题,所以取深色主题那一档强调色——它在浅色与
# 深色任务栏上都还看得清,而 #0A59F7 压在深色任务栏上偏暗。
ICON_RGB = (0x31, 0x7A, 0xF7)

# 超采样倍数。椭圆边缘与旋转都靠它拿到抗锯齿。
SS = 8

NS = {"svg": "http://www.w3.org/2000/svg"}


@dataclass(frozen=True)
class Ellipse:
    name: str
    cx: float
    cy: float
    rx: float
    ry: float
    opacity: float
    stroke_width: float   # 0 表示实心填充

    @property
    def filled(self) -> bool:
        return self.stroke_width == 0.0


def parse_svg() -> tuple[float, float, list[Ellipse]]:
    root = ET.parse(SVG).getroot()

    view_box = root.get("viewBox", "").split()
    if len(view_box) != 4:
        raise SystemExit(f"{SVG}: viewBox 缺失或格式不对")
    extent = float(view_box[2])
    if float(view_box[3]) != extent:
        raise SystemExit(f"{SVG}: 标记必须画在正方形 viewBox 里")

    rotation: float | None = None
    ellipses: list[Ellipse] = []

    for node in root.findall("svg:ellipse", NS):
        name = node.get("id")
        if not name:
            raise SystemExit(f"{SVG}: 每个 ellipse 都要有 id,生成的常量按它命名")

        # 三层共用同一个倾角,不同就说明 SVG 被改成了别的东西,这里不去猜。
        match = re.search(r"rotate\(\s*(-?[\d.]+)", node.get("transform", ""))
        if not match:
            raise SystemExit(f"{SVG}: {name} 缺 rotate() 变换")
        angle = float(match.group(1))
        if rotation is None:
            rotation = angle
        elif angle != rotation:
            raise SystemExit(f"{SVG}: {name} 的倾角与其余层不一致")

        stroke_width = float(node.get("stroke-width", 0.0)) if node.get("stroke") else 0.0
        ellipses.append(
            Ellipse(
                name=name,
                cx=float(node.get("cx")),
                cy=float(node.get("cy")),
                rx=float(node.get("rx")),
                ry=float(node.get("ry")),
                opacity=float(node.get("opacity", 1.0)),
                stroke_width=stroke_width,
            )
        )

    if len(ellipses) < 2 or rotation is None:
        raise SystemExit(f"{SVG}: 至少要有外轮廓与内核两层")
    return extent, rotation, ellipses


def layers_for(size: int, ellipses: list[Ellipse]) -> list[Ellipse]:
    """小尺寸只留首尾两层:外轮廓提到全不透明,内核放大补回丢掉的墨迹。"""
    if size > SMALL_SIZE_PX:
        return ellipses
    outer, core = ellipses[0], ellipses[-1]
    return [
        Ellipse(**{**outer.__dict__, "opacity": 1.0}),
        Ellipse(**{**core.__dict__,
                   "rx": core.rx * SMALL_CORE_SCALE,
                   "ry": core.ry * SMALL_CORE_SCALE}),
    ]


def render(size: int, extent: float, rotation: float, ellipses: list[Ellipse]) -> Image.Image:
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    scale = size * SS / extent

    for ring in layers_for(size, ellipses):
        mask = Image.new("L", (size * SS, size * SS), 0)
        draw = ImageDraw.Draw(mask)

        def box(pad: float) -> tuple[float, float, float, float]:
            return (
                (ring.cx - ring.rx - pad) * scale,
                (ring.cy - ring.ry - pad) * scale,
                (ring.cx + ring.rx + pad) * scale,
                (ring.cy + ring.ry + pad) * scale,
            )

        if ring.filled:
            draw.ellipse(box(0.0), fill=255)
        else:
            half = ring.stroke_width / 2.0
            draw.ellipse(box(half), fill=255)
            draw.ellipse(box(-half), fill=0)

        # SVG 的 rotate 以 y 轴向下为正方向,Pillow 的 rotate 是逆时针为正,
        # 两者互为相反数。中心用像素坐标给,否则绕图片中心转会把标记甩偏。
        mask = mask.rotate(
            -rotation,
            resample=Image.BICUBIC,
            center=(ring.cx * scale, ring.cy * scale),
        )
        mask = mask.resize((size, size), Image.LANCZOS)
        if ring.opacity != 1.0:
            mask = mask.point(lambda v, a=ring.opacity: int(v * a + 0.5))

        layer = Image.new("RGBA", (size, size), ICON_RGB + (255,))
        canvas = Image.alpha_composite(canvas, Image.composite(
            layer, Image.new("RGBA", (size, size), (0, 0, 0, 0)), mask))

    return canvas


def write_ico(images: list[Image.Image]) -> None:
    """手写 ICO 容器。

    Pillow 的 ICO 保存只按一张底图缩放,而我们每档的画法不一样(见 layers_for),
    必须逐档给图。容器格式本身很简单,全部条目用 PNG 负载——Vista 以后都认,
    而本项目只跑 Windows 11。
    """
    payloads = []
    for image in images:
        buffer = BytesIO()
        image.save(buffer, format="PNG", optimize=True)
        payloads.append(buffer.getvalue())

    offset = 6 + 16 * len(payloads)
    directory = b""
    for image, payload in zip(images, payloads):
        side = 0 if image.width >= 256 else image.width
        directory += struct.pack("<BBBBHHII", side, side, 0, 0, 1, 32, len(payload), offset)
        offset += len(payload)

    ICO.parent.mkdir(parents=True, exist_ok=True)
    ICO.write_bytes(struct.pack("<HHH", 0, 1, len(payloads)) + directory + b"".join(payloads))


def write_header(extent: float, rotation: float, ellipses: list[Ellipse]) -> None:
    rows = []
    for ring in ellipses:
        rows.append(
            f"    {{ {ring.cx:>6.3f}f, {ring.cy:>6.3f}f, {ring.rx:>6.3f}f, {ring.ry:>6.3f}f, "
            f"{ring.opacity:.3f}f, {ring.stroke_width:.3f}f }},"
            f"   // {ring.name}"
        )

    HEADER.parent.mkdir(parents=True, exist_ok=True)
    HEADER.write_text(
        f"""#pragma once
// 由 scripts/make_brand_assets.py 从 Assets/brand/openego-hub.svg 生成。不要手改:
// 改标记要改那份 SVG,再重新跑一次脚本。
//
// 托盘按这些常量自绘图标,.ico 由同一份 SVG 栅格化,两边因此不会漂。

namespace Brand {{

struct MarkEllipse {{
    float cx;
    float cy;
    float rx;
    float ry;
    float opacity;
    float strokeWidth;   // 0 表示实心填充

    [[nodiscard]] constexpr bool Filled() const {{ return strokeWidth == 0.0f; }}
}};

// 几何画在这么大的正方形里,用的时候按目标像素等比缩放。
inline constexpr float kMarkViewBox = {extent:.1f}f;

// 三层共用的倾角,度。正方向与 SVG 一致(y 轴向下为正)。
inline constexpr float kMarkRotationDeg = {rotation:.1f}f;

// 不超过这个像素尺寸时只画首尾两层,且外轮廓按全不透明画:三层套在一起到
// 16 px 上层间净空只剩约 1.4 px,会糊成一团。
inline constexpr int kMarkSmallSizePx = {SMALL_SIZE_PX};

// 小尺寸下内核的放大倍数,补回中间那层丢掉的墨迹。
inline constexpr float kMarkSmallCoreScale = {SMALL_CORE_SCALE}f;

// 由外到内。第 0 层是外轮廓,最后一层是实心内核,小尺寸规则取的就是这两层。
inline constexpr MarkEllipse kMarkEllipses[] = {{
{chr(10).join(rows)}
}};

}} // namespace Brand
""",
        encoding="utf-8",
    )


def xaml_ellipse(cx: float, cy: float, rx: float, ry: float, rotation: float, indent: str) -> str:
    return (
        f'{indent}<EllipseGeometry Center="{cx:g},{cy:g}" RadiusX="{rx:g}" RadiusY="{ry:g}">\n'
        f"{indent}    <EllipseGeometry.Transform>\n"
        f'{indent}        <RotateTransform Angle="{rotation:g}" CenterX="{cx:g}" CenterY="{cy:g}" />\n'
        f"{indent}    </EllipseGeometry.Transform>\n"
        f"{indent}</EllipseGeometry>\n"
    )


def xaml_path(ring: Ellipse, rotation: float, indent: str) -> str:
    """一层等高线画成一个 Path。

    XAML 的 Geometry 没有描边宽度这个概念,所以 SVG 里那条 1.6 宽的中心线描边要展开成
    外缘减内缘两条边界,靠 EvenOdd 挖空;实心那层直接一个椭圆。
    """
    inner = indent + "        "
    if ring.filled:
        body = xaml_ellipse(ring.cx, ring.cy, ring.rx, ring.ry, rotation, inner)
        fill_rule = ""
    else:
        half = ring.stroke_width / 2.0
        body = xaml_ellipse(ring.cx, ring.cy, ring.rx + half, ring.ry + half, rotation, inner)
        body += xaml_ellipse(ring.cx, ring.cy, ring.rx - half, ring.ry - half, rotation, inner)
        fill_rule = ' FillRule="EvenOdd"'

    opacity = "" if ring.opacity == 1.0 else f' Opacity="{ring.opacity:g}"'
    return (
        f'{indent}<Path Fill="{{ThemeResource AccentFillColorDefaultBrush}}"{opacity}>\n'
        f"{indent}    <Path.Data>\n"
        f"{indent}        <GeometryGroup{fill_rule}>\n"
        f"{body}"
        f"{indent}        </GeometryGroup>\n"
        f"{indent}    </Path.Data>\n"
        f"{indent}</Path>\n"
    )


def write_main_xaml(rotation: float, ellipses: list[Ellipse]) -> None:
    indent = " " * 36
    block = [
        XAML_BEGIN,
        f"{indent}<!-- 由 scripts/make_brand_assets.py 从 Assets/brand/openego-hub.svg 生成，不要手改。 -->",
    ]
    for ring in ellipses:
        block.append(xaml_path(ring, rotation, indent).rstrip("\n"))
    block.append(XAML_END)

    text = MAIN_XAML.read_text(encoding="utf-8")
    start = text.find(XAML_BEGIN)
    stop = text.find(XAML_END)
    if start < 0 or stop < 0:
        raise SystemExit(
            f"{MAIN_XAML}: 找不到标记区块，需要一对\n{XAML_BEGIN}\n{XAML_END}"
        )
    MAIN_XAML.write_text(
        text[:start] + "\n".join(block) + text[stop + len(XAML_END):],
        encoding="utf-8",
    )


def main() -> None:
    extent, rotation, ellipses = parse_svg()

    images = [render(size, extent, rotation, ellipses) for size in ICO_SIZES]
    write_ico(images)
    write_header(extent, rotation, ellipses)
    write_main_xaml(rotation, ellipses)
    render(256, extent, rotation, ellipses).save(README_PNG)

    print(f"{ICO.relative_to(REPO)}  ({', '.join(str(s) for s in ICO_SIZES)})")
    print(f"{README_PNG.relative_to(REPO)}")
    print(f"{HEADER.relative_to(REPO)}  ({len(ellipses)} 层, 倾角 {rotation:g} 度)")
    print(f"{MAIN_XAML.relative_to(REPO)}  (BRAND-MARK 区块)")


if __name__ == "__main__":
    main()
