#include "pch.h"

#include "BatteryIndicator.xaml.h"

#if __has_include("BatteryIndicator.g.cpp")
#include "BatteryIndicator.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::EGoTouchSettings::implementation {

BatteryIndicator::BatteryIndicator() {
    InitializeComponent();
}

void BatteryIndicator::SetState(bool hasLevel, uint8_t level, bool charging) {
    IndicatorRoot().Visibility(hasLevel ? Visibility::Visible : Visibility::Collapsed);
    if (!hasLevel) return;

    level = std::min<uint8_t>(level, 100);
    BatteryValue().Text(to_hstring(static_cast<unsigned>(level)));
    BatteryFill().Width(std::max(1.0, 17.0 * static_cast<double>(level) / 100.0));
    ChargingGlyph().Visibility(charging ? Visibility::Visible : Visibility::Collapsed);
}

} // namespace winrt::EGoTouchSettings::implementation
