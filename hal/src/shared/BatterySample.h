#pragma once

#include <cstdint>

namespace Gaokun {

inline void ApplyBatterySample(int value, uint8_t &battery, uint32_t &flags,
                               uint32_t hasBatteryFlag) noexcept {
    // Reject before narrowing: an invalid sample must also invalidate a previous reading.
    // Zero is a valid empty battery; no vendor-specific sentinel is assumed here.
    const bool valid = value >= 0 && value <= 100;
    battery = valid ? static_cast<uint8_t>(value) : 0;
    if (valid) {
        flags |= hasBatteryFlag;
    } else {
        flags &= ~hasBatteryFlag;
    }
}

} // namespace Gaokun
