#pragma once

#include "Lut.h"

#include <string>

// .cube 解析。IRIDAS 与 DaVinci Resolve 两种写法的差别只在于是否带 TITLE / DOMAIN 行以及
// 1D 还是 3D，语法本身是同一套，因此不需要分别实现。
namespace Gaokun::Display {

struct CubeFile {
    Lut1d lut1d;  // LUT_1D_SIZE 时有效
    Lut3d lut3d;  // LUT_3D_SIZE 时有效

    [[nodiscard]] bool Is1d() const noexcept { return !lut1d.Empty(); }
    [[nodiscard]] bool Is3d() const noexcept { return !lut3d.Empty(); }
};

// 失败时返回 false，error 里是可直接展示给用户的原因（含行号）。
[[nodiscard]] bool ReadCubeFile(const std::wstring &path, CubeFile &out, std::string &error);

} // namespace Gaokun::Display
