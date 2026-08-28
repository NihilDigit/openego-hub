#pragma once

#include "Qdcm.h"

#include <cstdint>
#include <vector>

// 自定义查找表。出厂 preset 走 FactoryLut，尺寸天然吻合；这里是给用户提供的 .cube 用的,
// 它的尺寸任意，必须重采样到硬件要的 17^3 与 257。
//
// 三处索引顺序不同，是这块最容易出错的地方：
//   .cube 文件行序    b*n^2 + g*n + r   （red 变化最快）
//   qdcmlib 扁平索引  b*289 + g*17 + r  （与上相同）
//   DLUT 固件表       r*289 + g*17 + b  （blue 变化最快，需转置，见 FactoryLut.h）
// 本文件统一采用前者，因此写给 qdcmlib 时无需转换。
namespace Gaokun::Display {

struct Rgb {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

class Lut3d {
public:
    Lut3d() = default;
    explicit Lut3d(int size) : m_size(size), m_data(static_cast<size_t>(size) * size * size) {}

    [[nodiscard]] int Size() const noexcept { return m_size; }
    [[nodiscard]] bool Empty() const noexcept { return m_size < 2; }

    [[nodiscard]] Rgb &At(int r, int g, int b) noexcept { return m_data[Index(r, g, b)]; }
    [[nodiscard]] const Rgb &At(int r, int g, int b) const noexcept { return m_data[Index(r, g, b)]; }

    // 四面体插值。相比三线性，它在灰轴上不会引入偏色，是色彩管理里的惯例做法；
    // 六个分支对应把立方体切成的六个四面体。
    [[nodiscard]] Rgb Sample(float r, float g, float b) const noexcept;

    [[nodiscard]] Lut3d Resample(int newSize) const;

    // 转成 qdcmlib 要的定点表。索引顺序与本类一致，不需要转置。
    [[nodiscard]] RgbTable ToQdcm() const;

private:
    [[nodiscard]] size_t Index(int r, int g, int b) const noexcept {
        return static_cast<size_t>(b) * m_size * m_size + static_cast<size_t>(g) * m_size +
               static_cast<size_t>(r);
    }

    int m_size = 0;
    std::vector<Rgb> m_data;
};

// 3x1D 输入整形。三个通道共用一张索引，与 qdcmlib 的 IGCData 对应。
class Lut1d {
public:
    Lut1d() = default;
    explicit Lut1d(int size) : m_data(static_cast<size_t>(size)) {}

    [[nodiscard]] int Size() const noexcept { return static_cast<int>(m_data.size()); }
    [[nodiscard]] bool Empty() const noexcept { return m_data.size() < 2; }

    [[nodiscard]] Rgb &At(int i) noexcept { return m_data[static_cast<size_t>(i)]; }
    [[nodiscard]] const Rgb &At(int i) const noexcept { return m_data[static_cast<size_t>(i)]; }

    [[nodiscard]] Lut1d Resize(int newSize) const;
    [[nodiscard]] RgbTable ToQdcm() const;

private:
    std::vector<Rgb> m_data;
};

} // namespace Gaokun::Display
