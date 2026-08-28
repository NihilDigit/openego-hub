#include "FactoryLut.h"

#include <windows.h>

namespace Gaokun::Display {

namespace {

// provider 签名按大端拼（'A','C','P','I' 从高位到低位），而 table id 按小端拼。两者规则
// 不同是 Win32 的既有约定，写反了 GetSystemFirmwareTable 只会返回 0，看起来就像这台机器
// 没有这张表。
constexpr uint32_t kProviderAcpi = 0x41435049; // 'ACPI'
constexpr uint32_t kTableDlut = 0x54554C44;    // 'DLUT'

constexpr size_t kDataStart = 0x44;
constexpr size_t kPresetStride = 0x6400;
constexpr size_t kLow32Offset = 128;
constexpr size_t kHigh8Offset = 19780;

[[nodiscard]] size_t PresetOffset(Preset preset) noexcept {
    return kDataStart + static_cast<size_t>(preset) * kPresetStride;
}

[[nodiscard]] bool SliceValid(const std::vector<uint8_t> &table, size_t base) noexcept {
    return table.size() >= base + kPresetStride;
}

[[nodiscard]] uint32_t ReadBe32(const uint8_t *p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

} // namespace

std::vector<uint8_t> ReadDlutTable() noexcept {
    const UINT size = GetSystemFirmwareTable(kProviderAcpi, kTableDlut, nullptr, 0);
    if (size == 0) return {};

    std::vector<uint8_t> table(size);
    if (GetSystemFirmwareTable(kProviderAcpi, kTableDlut, table.data(), size) != size) {
        return {};
    }
    return table;
}

bool PresetPresent(const std::vector<uint8_t> &table, Preset preset) noexcept {
    const size_t base = PresetOffset(preset);
    if (!SliceValid(table, base)) return false;

    // 未使用的槽位整段为零。只看白点即可：任何真实的校准表白点都不是 0。
    const size_t white = kLutEntries - 1;
    const uint32_t low32 = ReadBe32(&table[base + kLow32Offset + white * 4]);
    const uint8_t high8 = table[base + kHigh8Offset + white];
    return low32 != 0 || high8 != 0;
}

bool DecodePreset(const std::vector<uint8_t> &table, Preset preset, RgbTable &out) noexcept {
    const size_t base = PresetOffset(preset);
    if (!SliceValid(table, base)) return false;

    out.red.assign(kLutEntries, 0);
    out.green.assign(kLutEntries, 0);
    out.blue.assign(kLutEntries, 0);

    const uint8_t *low32s = &table[base + kLow32Offset];
    const uint8_t *high8s = &table[base + kHigh8Offset];

    size_t i = 0;
    for (int r = 0; r < kLutSize; ++r) {
        for (int g = 0; g < kLutSize; ++g) {
            for (int b = 0; b < kLutSize; ++b, ++i) {
                const uint32_t low32 = ReadBe32(low32s + i * 4);
                const uint32_t high8 = high8s[i];

                // 转置：读的顺序以 red 为最高位，写给 qdcmlib 的顺序以 blue 为最高位。
                const size_t target = static_cast<size_t>(b) * kLutSize * kLutSize +
                                      static_cast<size_t>(g) * kLutSize +
                                      static_cast<size_t>(r);

                out.blue[target] = low32 & 0xFFF;
                out.green[target] = (low32 >> 12) & 0xFFF;
                out.red[target] = ((low32 >> 24) | (high8 << 8)) & 0xFFF;
            }
        }
    }
    return true;
}

} // namespace Gaokun::Display
