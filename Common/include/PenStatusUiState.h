#pragma once

#include "PenStatusChannel.h"

#include <cstddef>
#include <cstring>

namespace PenStatus {

namespace Detail {

template <std::size_t N>
[[nodiscard]] inline bool FixedStringEqual(const char (&lhs)[N], const char (&rhs)[N]) noexcept {
    return std::strncmp(lhs, rhs, N) == 0;
}

}  // namespace Detail

// 比较两个 State 是否对 UI 呈现完全一致。
// 刻意忽略仅用于时间戳刷新（如每 250ms 心跳推进）的 updatedAtUnixMs，
// 避免宿主状态未变化时触发无意义的 WinUI XAML 属性写入和布局重排。
[[nodiscard]] inline bool SameUiState(const State& lhs, const State& rhs) noexcept {
    return lhs.hasBatteryLevel == rhs.hasBatteryLevel &&
           lhs.batteryLevel == rhs.batteryLevel &&
           lhs.hasChargingState == rhs.hasChargingState &&
           lhs.charging == rhs.charging &&
           lhs.hasDeviceAttached == rhs.hasDeviceAttached &&
           lhs.deviceAttached == rhs.deviceAttached &&
           lhs.hasStylusLink == rhs.hasStylusLink &&
           lhs.stylusLinked == rhs.stylusLinked &&
           lhs.hasPenButtonMode == rhs.hasPenButtonMode &&
           lhs.penButtonMode == rhs.penButtonMode &&
           lhs.hasEraserActive == rhs.hasEraserActive &&
           lhs.eraserActive == rhs.eraserActive &&
           lhs.hasTouchProvider == rhs.hasTouchProvider &&
           lhs.touchProvider == rhs.touchProvider &&
           lhs.hasProviderError == rhs.hasProviderError &&
           lhs.providerError == rhs.providerError &&
           lhs.hasInputSuppressed == rhs.hasInputSuppressed &&
           lhs.inputSuppressed == rhs.inputSuppressed &&
           lhs.hasKbdDetachSupport == rhs.hasKbdDetachSupport &&
           lhs.kbdDetachSupport == rhs.kbdDetachSupport &&
           lhs.hasHostHealth == rhs.hasHostHealth &&
           lhs.penHostHealthy == rhs.penHostHealthy &&
           lhs.kbdHostHealthy == rhs.kbdHostHealthy &&
           lhs.penVendorMissing == rhs.penVendorMissing &&
           lhs.kbdVendorMissing == rhs.kbdVendorMissing &&
           lhs.hasVendorServices == rhs.hasVendorServices &&
           lhs.vendorServicesDisabled == rhs.vendorServicesDisabled &&
           lhs.vendorServicesRunning == rhs.vendorServicesRunning &&
           lhs.vendorServicesAllRunning == rhs.vendorServicesAllRunning &&
           lhs.hasChargeLimit == rhs.hasChargeLimit &&
           lhs.chargeLimitManual == rhs.chargeLimitManual &&
           lhs.chargeLimit == rhs.chargeLimit &&
           lhs.hasPenFirmware == rhs.hasPenFirmware &&
           lhs.hasPenHardware == rhs.hasPenHardware &&
           lhs.hasPenSerial == rhs.hasPenSerial &&
           lhs.hasKbdPresent == rhs.hasKbdPresent &&
           lhs.kbdPresent == rhs.kbdPresent &&
           lhs.hasKbdDetached == rhs.hasKbdDetached &&
           lhs.kbdDetached == rhs.kbdDetached &&
           lhs.hasKbdBattery == rhs.hasKbdBattery &&
           lhs.kbdBatteryLevel == rhs.kbdBatteryLevel &&
           lhs.hasKbdCharging == rhs.hasKbdCharging &&
           lhs.kbdCharging == rhs.kbdCharging &&
           lhs.notificationSequence == rhs.notificationSequence &&
           lhs.notificationKind == rhs.notificationKind &&
           lhs.updateState == rhs.updateState &&
           lhs.updateMajor == rhs.updateMajor &&
           lhs.updateMinor == rhs.updateMinor &&
           lhs.updatePatch == rhs.updatePatch &&
           lhs.modelId == rhs.modelId &&
           Detail::FixedStringEqual(lhs.modelName, rhs.modelName) &&
           Detail::FixedStringEqual(lhs.penFirmware, rhs.penFirmware) &&
           Detail::FixedStringEqual(lhs.penHardware, rhs.penHardware) &&
           Detail::FixedStringEqual(lhs.penSerial, rhs.penSerial) &&
           Detail::FixedStringEqual(lhs.kbdModelName, rhs.kbdModelName) &&
           Detail::FixedStringEqual(lhs.kbdFirmware, rhs.kbdFirmware);
}

}  // namespace PenStatus
