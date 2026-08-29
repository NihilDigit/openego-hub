#include "pch.h"

#include "BatteryIndicator.xaml.h"

#if __has_include("BatteryIndicator.g.cpp")
#include "BatteryIndicator.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Media::Animation;

namespace winrt::EGoTouchSettings::implementation {

namespace {
// 电池内腔的宽度，与 XAML 里那圈边框和内边距对应：23 - 2 * 1.25 边框 - 2 * 2 内边距。
constexpr double kInnerWidth = 17.0;
constexpr int kChargingCycleMs = 1800;

double FillWidthFor(uint8_t level) {
    return std::max(1.0, kInnerWidth * static_cast<double>(level) / 100.0);
}
} // namespace

BatteryIndicator::BatteryIndicator() {
    InitializeComponent();
}

// 充电动画：脉冲条从空扫到满，同时淡出，循环。Width 不是独立属性，动画只能跑在 UI 线程
// 上，所以要显式打开 EnableDependentAnimation——这里只有一个 Border 在动，代价可以忽略；
// 换成独立属性（缩放或位移）就得再套一层裁剪，反而更绕。
void BatteryIndicator::StartChargingAnimation() {
    if (m_charging) return;

    const auto duration = DurationHelper::FromTimeSpan(
        Windows::Foundation::TimeSpan{static_cast<int64_t>(kChargingCycleMs) * 10'000});

    DoubleAnimation sweep;
    sweep.From(0.0);
    sweep.To(kInnerWidth);
    sweep.EnableDependentAnimation(true);
    sweep.Duration(duration);
    Storyboard::SetTarget(sweep, BatteryPulse());
    Storyboard::SetTargetProperty(sweep, L"Width");

    // 越接近满越淡，扫到头时正好消失，于是归位那一下看不见接缝。
    DoubleAnimation fade;
    fade.From(1.0);
    fade.To(0.0);
    fade.Duration(duration);
    Storyboard::SetTarget(fade, BatteryPulse());
    Storyboard::SetTargetProperty(fade, L"Opacity");

    Storyboard storyboard;
    storyboard.Children().Append(sweep);
    storyboard.Children().Append(fade);
    storyboard.RepeatBehavior(RepeatBehaviorHelper::Forever());
    m_charging = storyboard;
    BatteryPulse().Visibility(Visibility::Visible);
    storyboard.Begin();
}

void BatteryIndicator::StopChargingAnimation() {
    if (!m_charging) return;
    m_charging.Stop();
    m_charging = nullptr;
    BatteryPulse().Visibility(Visibility::Collapsed);
    BatteryPulse().Opacity(0.0);
}

void BatteryIndicator::SetState(bool hasLevel, uint8_t level, bool charging) {
    IndicatorRoot().Visibility(hasLevel ? Visibility::Visible : Visibility::Collapsed);
    if (!hasLevel) {
        StopChargingAnimation();
        return;
    }

    level = std::min<uint8_t>(level, 100);
    BatteryValue().Text(to_hstring(static_cast<unsigned>(level)));

    BatteryFill().Width(FillWidthFor(level));

    // 状态每秒刷新一次，而动画一轮 1.8 秒。已经在放就别重来，否则每次刷新都把它按回起点，
    // 看上去就是不动。
    if (charging) StartChargingAnimation();
    else StopChargingAnimation();
}

} // namespace winrt::EGoTouchSettings::implementation
