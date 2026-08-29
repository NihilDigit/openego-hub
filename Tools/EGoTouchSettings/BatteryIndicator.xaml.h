#pragma once

#include "BatteryIndicator.g.h"

namespace winrt::EGoTouchSettings::implementation {

struct BatteryIndicator : BatteryIndicatorT<BatteryIndicator> {
    BatteryIndicator();

    void SetState(bool hasLevel, uint8_t level, bool charging);
};

} // namespace winrt::EGoTouchSettings::implementation

namespace winrt::EGoTouchSettings::factory_implementation {

struct BatteryIndicator : BatteryIndicatorT<BatteryIndicator, implementation::BatteryIndicator> {};

} // namespace winrt::EGoTouchSettings::factory_implementation
