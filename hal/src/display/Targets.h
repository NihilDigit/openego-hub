#pragma once

#include "Qdcm.h"

#include <cstdint>
#include <vector>

namespace Gaokun::Display {

// caps 的能力位，含义取自 goodies。
inline constexpr int32_t kCapLut3d = 0x200;
inline constexpr int32_t kCapPcc = 0x2;

// 挑出可下发的目标。要求恰好一个，理由见实现处的注释。
[[nodiscard]] bool EnumTargets(Qdcm &qdcm, std::vector<uint32_t> &targets);

} // namespace Gaokun::Display
