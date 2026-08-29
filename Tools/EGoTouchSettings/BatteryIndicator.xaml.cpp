#include "pch.h"

#include "BatteryIndicator.xaml.h"

#if __has_include("BatteryIndicator.g.cpp")
#include "BatteryIndicator.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::EGoTouchSettings::implementation {

namespace {
// Segoe Fluent Icons 里两组连号的字形：Battery0..Battery10 和 BatteryCharging0..10，
// 各十一档，档位就是码点的偏移量。
constexpr wchar_t kBatteryBase = 0xE850;
constexpr wchar_t kBatteryChargingBase = 0xE85B;
constexpr int kSteps = 10;
// 充电时逐档播放一遍需要的节拍。整轮约两秒，与相邻的动效节奏接近。
constexpr int kChargingFrameMs = 180;

int StepFor(uint8_t level) {
    return std::clamp((level + 5) / 10, 0, kSteps);
}
} // namespace

BatteryIndicator::BatteryIndicator() {
    InitializeComponent();
}

void BatteryIndicator::ShowStep(int step, bool charging) {
    const wchar_t glyph[2]{
        static_cast<wchar_t>((charging ? kBatteryChargingBase : kBatteryBase) + step), L'\0'};
    BatteryGlyph().Glyph(glyph);
}

// 充电动画就是把充电态那十一个字形挨个放一遍再从头来。动画只表示「在充」，不表示充到了
// 多少——真实电量在旁边的百分比里，而按真实档位播放的话，九成以上的电量只剩一两帧行程。
void BatteryIndicator::StartChargingAnimation() {
    if (m_chargingTimer) return;
    m_chargingFrame = 0;
    m_chargingTimer = DispatcherTimer();
    m_chargingTimer.Interval(std::chrono::milliseconds(kChargingFrameMs));
    m_chargingTimer.Tick([this](IInspectable const&, IInspectable const&) {
        m_chargingFrame = (m_chargingFrame + 1) % (kSteps + 1);
        ShowStep(m_chargingFrame, true);
    });
    m_chargingTimer.Start();
}

void BatteryIndicator::StopChargingAnimation() {
    if (!m_chargingTimer) return;
    m_chargingTimer.Stop();
    m_chargingTimer = nullptr;
}

void BatteryIndicator::SetState(bool hasLevel, uint8_t level, bool charging) {
    IndicatorRoot().Visibility(hasLevel ? Visibility::Visible : Visibility::Collapsed);
    if (!hasLevel) {
        StopChargingAnimation();
        return;
    }

    level = std::min<uint8_t>(level, 100);
    BatteryValue().Text(to_hstring(static_cast<unsigned>(level)));

    if (charging) {
        // 已经在放就别重来：状态每秒刷新一次，每次都从头会让它停在第一帧。
        StartChargingAnimation();
        return;
    }

    StopChargingAnimation();
    ShowStep(StepFor(level), false);
}

} // namespace winrt::EGoTouchSettings::implementation
