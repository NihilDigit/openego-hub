#include "Lut.h"

#include <algorithm>
#include <cmath>

namespace Gaokun::Display {

namespace {

struct Bounds {
    int lo;
    int hi;
    float t;
};

[[nodiscard]] Bounds GetBounds(float value, int size) noexcept {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    const float scaled = clamped * static_cast<float>(size - 1);
    int lo = static_cast<int>(std::floor(scaled));
    lo = std::clamp(lo, 0, size - 1);
    const int hi = std::min(lo + 1, size - 1);
    return Bounds{lo, hi, scaled - static_cast<float>(lo)};
}

[[nodiscard]] Rgb Mix(float wa, const Rgb &a, float wb, const Rgb &b, float wc, const Rgb &c,
                      float wd, const Rgb &d) noexcept {
    return Rgb{wa * a.r + wb * b.r + wc * c.r + wd * d.r,
               wa * a.g + wb * b.g + wc * c.g + wd * d.g,
               wa * a.b + wb * b.b + wc * c.b + wd * d.b};
}

[[nodiscard]] uint32_t ToFixed(float value) noexcept {
    const float scaled = std::round(std::clamp(value, 0.0f, 1.0f) * static_cast<float>(kLutMax));
    return static_cast<uint32_t>(scaled);
}

} // namespace

Rgb Lut3d::Sample(float red, float green, float blue) const noexcept {
    if (Empty()) return Rgb{};

    const Bounds R = GetBounds(red, m_size);
    const Bounds G = GetBounds(green, m_size);
    const Bounds B = GetBounds(blue, m_size);

    const Rgb &v000 = At(R.lo, G.lo, B.lo);
    const Rgb &v001 = At(R.lo, G.lo, B.hi);
    const Rgb &v010 = At(R.lo, G.hi, B.lo);
    const Rgb &v011 = At(R.lo, G.hi, B.hi);
    const Rgb &v100 = At(R.hi, G.lo, B.lo);
    const Rgb &v101 = At(R.hi, G.lo, B.hi);
    const Rgb &v110 = At(R.hi, G.hi, B.lo);
    const Rgb &v111 = At(R.hi, G.hi, B.hi);

    const float x = R.t;
    const float y = G.t;
    const float z = B.t;

    // 六个分支按 x、y、z 的大小关系选中所在的四面体。每个分支恰好用到立方体的四个顶点,
    // 权重之和为 1，因此灰轴（x==y==z）上结果严格落在 V000 与 V111 的连线上，不会偏色。
    if (x > y && y > z) return Mix(1 - x, v000, x - y, v100, y - z, v110, z, v111);
    if (x > y && x > z) return Mix(1 - x, v000, x - z, v100, z - y, v101, y, v111);
    if (x > y) return Mix(1 - z, v000, z - x, v001, x - y, v101, y, v111);
    if (z > y) return Mix(1 - z, v000, z - y, v001, y - x, v011, x, v111);
    if (z > x) return Mix(1 - y, v000, y - z, v010, z - x, v011, x, v111);
    return Mix(1 - y, v000, y - x, v010, x - z, v110, z, v111);
}

Lut3d Lut3d::Resample(int newSize) const {
    if (newSize == m_size) return *this;

    Lut3d out(newSize);
    const float scale = static_cast<float>(newSize - 1);
    for (int r = 0; r < newSize; ++r) {
        for (int g = 0; g < newSize; ++g) {
            for (int b = 0; b < newSize; ++b) {
                out.At(r, g, b) = Sample(static_cast<float>(r) / scale,
                                         static_cast<float>(g) / scale,
                                         static_cast<float>(b) / scale);
            }
        }
    }
    return out;
}

RgbTable Lut3d::ToQdcm() const {
    RgbTable table;
    const size_t count = m_data.size();
    table.red.resize(count);
    table.green.resize(count);
    table.blue.resize(count);
    for (size_t i = 0; i < count; ++i) {
        table.red[i] = ToFixed(m_data[i].r);
        table.green[i] = ToFixed(m_data[i].g);
        table.blue[i] = ToFixed(m_data[i].b);
    }
    return table;
}

Lut1d Lut1d::Resize(int newSize) const {
    if (newSize == Size()) return *this;

    Lut1d out(newSize);
    const float scale = static_cast<float>(newSize - 1);
    const int oldMax = Size() - 1;
    for (int i = 0; i < newSize; ++i) {
        const float position = static_cast<float>(i) / scale * static_cast<float>(oldMax);
        const int lo = std::clamp(static_cast<int>(std::floor(position)), 0, oldMax);
        const int hi = std::min(lo + 1, oldMax);
        const float t = position - static_cast<float>(lo);
        const Rgb &a = At(lo);
        const Rgb &b = At(hi);
        out.At(i) = Rgb{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
    }
    return out;
}

RgbTable Lut1d::ToQdcm() const {
    RgbTable table;
    const size_t count = m_data.size();
    table.red.resize(count);
    table.green.resize(count);
    table.blue.resize(count);
    for (size_t i = 0; i < count; ++i) {
        table.red[i] = ToFixed(m_data[i].r);
        table.green[i] = ToFixed(m_data[i].g);
        table.blue[i] = ToFixed(m_data[i].b);
    }
    return table;
}

} // namespace Gaokun::Display
