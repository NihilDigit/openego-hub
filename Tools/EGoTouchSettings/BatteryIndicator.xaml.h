#pragma once

#include "BatteryIndicator.g.h"

namespace winrt::EGoTouchSettings::implementation {

struct BatteryIndicator : BatteryIndicatorT<BatteryIndicator> {
    BatteryIndicator();

    void SetState(bool hasLevel, uint8_t level, bool charging);

private:
    void ShowStep(int step, bool charging);
    void StartChargingAnimation();
    void StopChargingAnimation();

    // 非空表示充电动画正在放。
    Microsoft::UI::Xaml::DispatcherTimer m_chargingTimer{nullptr};
    int m_chargingFrame = 0;
};

} // namespace winrt::EGoTouchSettings::implementation

namespace winrt::EGoTouchSettings::factory_implementation {

struct BatteryIndicator : BatteryIndicatorT<BatteryIndicator, implementation::BatteryIndicator> {};

} // namespace winrt::EGoTouchSettings::factory_implementation
