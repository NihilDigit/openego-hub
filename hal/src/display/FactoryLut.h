#pragma once

#include "Qdcm.h"

#include <cstdint>
#include <vector>

// 工厂色彩校准取自 ACPI 固件表 DLUT，不在任何 DLL 里，因此这一部分是纯 Win32，可以在
// 不加载 qdcmlib 的情况下单独验证。
//
// 本机实测：表长 131124 字节，标准 ACPI 头，OEM ID "HUAWEI"，OEM Table ID "3DLUTTBL"。
// 数据区从 0x44 起，每个 preset 占 0x6400 字节。虽然表长能容下 5 个槽，但只有前两个有
// 数据，其余全零。
//
// 每个 preset 内部：
//   +128    低 32 位，大端 uint32，共 4913 项
//   +19780  高 8 位，每项一字节
//   Blue  =  low32        & 0xFFF
//   Green = (low32 >> 12) & 0xFFF
//   Red   = (low32 >> 24) | (high8 << 8)
//
// 注意两处索引顺序是相反的，这是最容易出错的地方：固件表按 i = r*289 + g*17 + b 线性
// 排列，而 qdcmlib 的 3D LUT 要的扁平索引是 b*289 + g*17 + r。按前者读、按后者写，
// 中间必须转置；两边都用同一个顺序，红蓝会互换而画面仍然「像是能用」。
namespace Gaokun::Display {

enum class Preset {
    DisplayP3 = 0,
    Srgb = 1,
};

// 读取整张 DLUT 固件表。返回空表示该机没有这张表。
[[nodiscard]] std::vector<uint8_t> ReadDlutTable() noexcept;

// 从已读入的表中解出一个 preset，输出已按 qdcmlib 的扁平索引排好。
[[nodiscard]] bool DecodePreset(const std::vector<uint8_t> &table, Preset preset,
                                RgbTable &out) noexcept;

// 该槽位是否含有数据。全零槽位表示这台机器没有这个 preset。
[[nodiscard]] bool PresetPresent(const std::vector<uint8_t> &table, Preset preset) noexcept;

} // namespace Gaokun::Display
