#include "pch.h"

#include "BatteryIndicator.xaml.h"

#if __has_include("BatteryIndicator.g.cpp")
#include "BatteryIndicator.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::EGoTouchSettings::implementation {

namespace {
// Segoe Fluent Icons 里的三组电池字形，连号排布，档位就是码点偏移。档数是把字体渲染出来
// 一个个数的，不是常说的「每组十一档」：普通十档、充电九档，两组数目并不相同，而紧接在
// 充电组后面的是节能组（电池旁边一片叶子）。多算一档就会画出叶子来。
constexpr wchar_t kBatteryBase = 0xE850;          // Battery0..Battery9
constexpr int kBatteryMaxStep = 9;
constexpr wchar_t kBatteryChargingBase = 0xE85A;  // BatteryCharging0..BatteryCharging8
constexpr int kBatteryChargingMaxStep = 8;
// 充电时逐档播放一遍需要的节拍。整轮约一秒半，与相邻的动效节奏接近。
constexpr int kChargingFrameMs = 170;
// 低电阈值。与「电量低」的口径无关的地方不要复用它。
constexpr int kLowLevel = 20;

int StepFor(uint8_t level, int maxStep) {
    return std::clamp((level * maxStep + 50) / 100, 0, maxStep);
}
} // namespace

BatteryIndicator::BatteryIndicator() {
    InitializeComponent();
}

void BatteryIndicator::ShowStep(int step, bool charging) {
    const wchar_t base = charging ? kBatteryChargingBase : kBatteryBase;
    const wchar_t level[2]{static_cast<wchar_t>(base + step), L'\0'};
    // 外框那一层用同一组的 0 档，它只有空壳（充电组还带闪电），压在电量上正好把边线盖回
    // 文字色。
    const wchar_t shell[2]{base, L'\0'};
    BatteryLevelGlyph().Glyph(level);
    BatteryShellGlyph().Glyph(shell);
}

Media::Brush BatteryIndicator::BrushFor(const wchar_t* key) {
    return Resources().Lookup(box_value(hstring{key})).as<Media::Brush>();
}

// 充电动画就是把充电态那十一个字形挨个放一遍再从头来。动画只表示「在充」，不表示充到了
// 多少——真实电量在旁边的百分比里，而按真实档位播放的话，九成以上的电量只剩一两帧行程。
void BatteryIndicator::StartChargingAnimation() {
    if (m_chargingTimer) return;
    m_chargingFrame = 0;
    m_chargingTimer = DispatcherTimer();
    m_chargingTimer.Interval(std::chrono::milliseconds(kChargingFrameMs));
    m_chargingTimer.Tick([this](IInspectable const&, IInspectable const&) {
        m_chargingFrame = (m_chargingFrame + 1) % (kBatteryChargingMaxStep + 1);
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

    // 电量低只在没充电时说得通：正在充的时候提醒电量低，用户能做的正是他已经在做的事。
    const bool low = !charging && level <= kLowLevel;
    BatteryLevelGlyph().Foreground(BrushFor(charging ? L"BatteryChargingBrush"
                                           : low    ? L"BatteryLowBrush"
                                                    : L"BatteryNormalBrush"));

    if (charging) {
        // 已经在放就别重来：状态每秒刷新一次，每次都从头会让它停在第一帧。
        StartChargingAnimation();
        return;
    }

    StopChargingAnimation();
    ShowStep(StepFor(level, kBatteryMaxStep), false);
}

} // namespace winrt::EGoTouchSettings::implementation
