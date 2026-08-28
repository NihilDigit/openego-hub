#pragma once

#include <cstdint>
#include <string_view>

// 配件的产品名。这张表是逆向结论，所以留在 hal 而不是上层：上层拿到的应该是可直接显示的
// 名字，不必知道模组 ID 长什么样。
//
// 依据是 PC Manager 配件中心的插件（CD54SPenApp 覆盖面最全，向后兼容所有型号）。它的
// picIndex / nameIndex 两张字典把十进制模组 ID 映到产品图与产品名：
//
//   49       0x31       M-Pencil            初代
//   282      0x11A      M-Pen 2
//   283      0x11B      M-Pencil            二代
//   65819    0x1011B    M-Pencil            二代 R
//   4468738  0x443002   HUAWEI M-Pencil 3   三代
//
// 注意 282：EGoTouchRev 曾把它当作 CD52 并因此显示为「第一代 M-Pencil」，而原厂把它归为
// M-Pen 2，真正的初代是 49——那个 ID 在旧表里根本不存在。这里按原厂的归属写。
namespace Gaokun::Models {

inline constexpr uint32_t kPenMPencil1 = 49;
inline constexpr uint32_t kPenMPen2 = 282;
inline constexpr uint32_t kPenMPencil2 = 283;
inline constexpr uint32_t kPenMPencil2R = 65819;
inline constexpr uint32_t kPenMPencil3 = 4468738;

[[nodiscard]] constexpr std::string_view PenDisplayName(uint32_t moduleId) noexcept {
    switch (moduleId) {
    case kPenMPencil1: return "HUAWEI M-Pencil（第一代）";
    case kPenMPen2: return "HUAWEI M-Pen 2";
    case kPenMPencil2:
    case kPenMPencil2R: return "HUAWEI M-Pencil（第二代）";
    case kPenMPencil3: return "HUAWEI M-Pencil（第三代）";
    default: return {};
    }
}

// 键盘按固件串的平台前缀判定，而不是模组 ID：模组 ID 在本机回报为 0，而前缀始终带着
// 平台名。判据与结论见 EGoTouchRev 的 docs/KEYBOARD_IDENTITY.md。
[[nodiscard]] inline std::string_view KeyboardDisplayName(std::string_view firmware) noexcept {
    if (firmware.find("GAOKUN") != std::string_view::npos) return "HUAWEI 智能磁吸键盘";
    if (firmware.find("DIRACR") != std::string_view::npos) return "HUAWEI 智能磁吸键盘";
    return {};
}

} // namespace Gaokun::Models
