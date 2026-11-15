#pragma once

#include "BatteryIndicator.g.h"

namespace winrt::EGoTouchSettings::implementation {

struct BatteryIndicator : BatteryIndicatorT<BatteryIndicator> {
    BatteryIndicator();

    void Configure(bool showText, bool largeGlyph, bool compactText);
    void SetState(bool hasLevel, uint8_t level, bool charging);

private:
    void ApplyMetrics();
    Microsoft::UI::Xaml::Media::Brush BrushFor(const wchar_t* key);

    bool m_showText = true;
    bool m_largeGlyph = false;
    bool m_compactText = false;
    bool m_hasState = false;
    uint8_t m_level = 0;
    bool m_charging = false;
};

} // namespace winrt::EGoTouchSettings::implementation

namespace winrt::EGoTouchSettings::factory_implementation {

struct BatteryIndicator : BatteryIndicatorT<BatteryIndicator, implementation::BatteryIndicator> {};

} // namespace winrt::EGoTouchSettings::factory_implementation
