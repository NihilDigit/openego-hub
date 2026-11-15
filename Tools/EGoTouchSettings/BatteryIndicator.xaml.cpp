#include "pch.h"

#include "BatteryIndicator.xaml.h"

#if __has_include("BatteryIndicator.g.cpp")
#include "BatteryIndicator.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::EGoTouchSettings::implementation {

namespace {

constexpr int kLowLevel = 20;

} // namespace

BatteryIndicator::BatteryIndicator() {
    InitializeComponent();
    ApplyMetrics();
}

void BatteryIndicator::Configure(bool showText, bool largeGlyph, bool compactText) {
    m_showText = showText;
    m_largeGlyph = largeGlyph;
    m_compactText = compactText;
    ApplyMetrics();
    SetState(m_hasState, m_level, m_charging);
}

void BatteryIndicator::ApplyMetrics() {
    using XamlCornerRadius = winrt::Microsoft::UI::Xaml::CornerRadius;
    if (m_largeGlyph) {
        BatteryBody().Width(38.0);
        BatteryBody().Height(19.0);
        BatteryBody().CornerRadius(XamlCornerRadius{4.0, 4.0, 4.0, 4.0});
        BatteryBody().BorderThickness(Thickness{2.0, 2.0, 2.0, 2.0});
        BatteryBody().Padding(Thickness{2.0, 2.0, 2.0, 2.0});
        BatteryFillBar().CornerRadius(XamlCornerRadius{1.5, 1.5, 1.5, 1.5});
        BatteryCap().Width(3.0);
        BatteryCap().Height(8.0);
        BatteryCap().Margin(Thickness{1.5, 0.0, 0.0, 0.0});
        BatteryCap().CornerRadius(XamlCornerRadius{0.0, 2.0, 2.0, 0.0});
    } else if (m_compactText) {
        BatteryBody().Width(22.0);
        BatteryBody().Height(11.0);
        BatteryBody().CornerRadius(XamlCornerRadius{2.5, 2.5, 2.5, 2.5});
        BatteryBody().BorderThickness(Thickness{1.5, 1.5, 1.5, 1.5});
        BatteryBody().Padding(Thickness{1.5, 1.5, 1.5, 1.5});
        BatteryFillBar().CornerRadius(XamlCornerRadius{1.0, 1.0, 1.0, 1.0});
        BatteryCap().Width(2.0);
        BatteryCap().Height(5.0);
        BatteryCap().Margin(Thickness{1.0, 0.0, 0.0, 0.0});
        BatteryCap().CornerRadius(XamlCornerRadius{0.0, 1.5, 1.5, 0.0});
        BatteryValue().FontSize(14.0);
        BatteryPercentUnit().FontSize(10.0);
    } else {
        BatteryBody().Width(24.0);
        BatteryBody().Height(12.0);
        BatteryBody().CornerRadius(XamlCornerRadius{3.0, 3.0, 3.0, 3.0});
        BatteryBody().BorderThickness(Thickness{1.5, 1.5, 1.5, 1.5});
        BatteryBody().Padding(Thickness{1.5, 1.5, 1.5, 1.5});
        BatteryFillBar().CornerRadius(XamlCornerRadius{1.0, 1.0, 1.0, 1.0});
        BatteryCap().Width(2.0);
        BatteryCap().Height(5.0);
        BatteryCap().Margin(Thickness{1.0, 0.0, 0.0, 0.0});
        BatteryCap().CornerRadius(XamlCornerRadius{0.0, 1.5, 1.5, 0.0});
        BatteryValue().FontSize(16.0);
        BatteryPercentUnit().FontSize(11.0);
    }
    BatteryTextGroup().Visibility(m_showText ? Visibility::Visible : Visibility::Collapsed);
}

Media::Brush BatteryIndicator::BrushFor(const wchar_t* key) {
    return Resources().Lookup(box_value(hstring{key})).as<Media::Brush>();
}

void BatteryIndicator::SetState(bool hasLevel, uint8_t level, bool charging) {
    hasLevel = hasLevel && level <= 100;
    m_hasState = hasLevel;
    m_level = level;
    m_charging = charging;

    if (!m_showText) {
        IndicatorRoot().Visibility(Visibility::Visible);
        if (!hasLevel) {
            BatteryFillBar().Visibility(Visibility::Collapsed);
            BatteryIconGroup().Opacity(0.45);
            return;
        }
        BatteryIconGroup().Opacity(1.0);
    } else {
        IndicatorRoot().Visibility(hasLevel ? Visibility::Visible : Visibility::Collapsed);
        if (!hasLevel) {
            return;
        }
        BatteryIconGroup().Opacity(1.0);
        BatteryValue().Text(to_hstring(static_cast<unsigned>(level)));
    }

    const bool low = !charging && level <= kLowLevel;
    BatteryFillBar().Background(BrushFor(charging ? L"BatteryChargingBrush"
                                        : low      ? L"BatteryLowBrush"
                                                   : L"BatteryNormalBrush"));

    const double maxFillWidth = m_largeGlyph ? 30.0 : (m_compactText ? 16.0 : 18.0);
    const double minSliver = m_largeGlyph ? 3.0 : 2.0;

    if (level == 0) {
        BatteryFillBar().Visibility(Visibility::Collapsed);
    } else if (level >= 100) {
        BatteryFillBar().Width(maxFillWidth);
        BatteryFillBar().Visibility(Visibility::Visible);
    } else {
        const double width =
            std::clamp(maxFillWidth * (static_cast<double>(level) / 100.0), minSliver, maxFillWidth);
        BatteryFillBar().Width(width);
        BatteryFillBar().Visibility(Visibility::Visible);
    }
}

} // namespace winrt::EGoTouchSettings::implementation
