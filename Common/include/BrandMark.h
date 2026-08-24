#pragma once
// 由 scripts/make_brand_assets.py 从 Assets/brand/openego-hub.svg 生成。不要手改:
// 改标记要改那份 SVG,再重新跑一次脚本。
//
// 托盘按这些常量自绘图标,.ico 由同一份 SVG 栅格化,两边因此不会漂。

namespace Brand {

struct MarkEllipse {
    float cx;
    float cy;
    float rx;
    float ry;
    float opacity;
    float strokeWidth;   // 0 表示实心填充

    [[nodiscard]] constexpr bool Filled() const { return strokeWidth == 0.0f; }
};

// 几何画在这么大的正方形里,用的时候按目标像素等比缩放。
inline constexpr float kMarkViewBox = 24.0f;

// 三层共用的倾角,度。正方向与 SVG 一致(y 轴向下为正)。
inline constexpr float kMarkRotationDeg = -22.0f;

// 不超过这个像素尺寸时只画首尾两层,且外轮廓按全不透明画:三层套在一起到
// 16 px 上层间净空只剩约 1.4 px,会糊成一团。
inline constexpr int kMarkSmallSizePx = 24;

// 小尺寸下内核的放大倍数,补回中间那层丢掉的墨迹。
inline constexpr float kMarkSmallCoreScale = 1.3f;

// 由外到内。第 0 层是外轮廓,最后一层是实心内核,小尺寸规则取的就是这两层。
inline constexpr MarkEllipse kMarkEllipses[] = {
    { 12.000f, 12.000f, 10.200f,  8.400f, 0.500f, 1.600f },   // contour-outer
    { 11.500f, 11.450f,  6.500f,  5.000f, 0.780f, 1.600f },   // contour-mid
    { 11.000f, 10.900f,  3.000f,  2.300f, 1.000f, 0.000f },   // contour-core
};

} // namespace Brand
