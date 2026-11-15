#pragma once

#include "PenStatusChannel.h"

namespace AccessoryBattery {

inline bool HasPenBattery(const PenStatus::State& state) {
    if (!state.hasBatteryLevel || state.batteryLevel > 100) return false;
    if (state.hasHostHealth && (!state.penHostHealthy || state.penVendorMissing)) {
        return false;
    }
    // 吸附和书写链路分别上报；离开充电位置后仍可通过笔链路使用。
    const bool active = (state.hasStylusLink && state.stylusLinked) ||
                        (state.hasChargingState && state.charging);
    return active || !state.hasDeviceAttached || state.deviceAttached;
}

inline bool HasKeyboardBattery(const PenStatus::State& state) {
    if (!state.hasKbdBattery || state.kbdBatteryLevel > 100) return false;
    if (state.hasHostHealth && (!state.kbdHostHealthy || state.kbdVendorMissing)) {
        return false;
    }
    return !state.hasKbdPresent || state.kbdPresent;
}

} // namespace AccessoryBattery
