#pragma once

#include "BatteryIndicator.g.h"

namespace winrt::EGoTouchSettings::implementation {

struct BatteryIndicator : BatteryIndicatorT<BatteryIndicator> {
    BatteryIndicator();

    void SetState(bool hasLevel, uint8_t level, bool charging);

private:
    static int BatteryStepFor(uint8_t level) noexcept;
    static winrt::hstring BatteryImagePath(int step, bool charging);

    winrt::hstring m_imagePath;
    winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage m_bitmap{nullptr};
};

} // namespace winrt::EGoTouchSettings::implementation

namespace winrt::EGoTouchSettings::factory_implementation {

struct BatteryIndicator : BatteryIndicatorT<BatteryIndicator, implementation::BatteryIndicator> {};

} // namespace winrt::EGoTouchSettings::factory_implementation
