#include "pch.h"

#include "BatteryIndicator.xaml.h"

#if __has_include("BatteryIndicator.g.cpp")
#include "BatteryIndicator.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::EGoTouchSettings::implementation {

namespace {
// Segoe Fluent Icons 里的电池字形，档位就是码点偏移，两组各十档：E859 已经是满，E85A 起
// 是带闪电的充电组，再往后是节能组（电池旁边一片叶子）。
//
// 这几个数只对应用内实际渲染出来的字形负责。同一批码点，用 GDI+ 按字体名离线渲染出来的
// 填充档位与 FontIcon 画出来的并不一致——两边取到的不是同一份字体——照离线那张表去数，会
// 把满档认成第九档，再另找一个「满档」码点补上，而那个码点是尺寸小一圈的另一枚图标，垫在
// 下层会被上层的空壳整个罩住，看起来就是一格空电池。要调这里，必须在应用里看。
constexpr wchar_t kBatteryBase = 0xE850;          // Battery0..Battery9
constexpr wchar_t kBatteryChargingBase = 0xE85A;  // BatteryCharging0..BatteryCharging9
constexpr int kMaxStep = 9;
// 充电时逐档播放一遍需要的节拍。整轮约一秒半，与相邻的动效节奏接近。
constexpr int kChargingFrameMs = 170;
// 低电阈值。与「电量低」的口径无关的地方不要复用它。
constexpr int kLowLevel = 20;

int StepFor(uint8_t level) {
    return std::clamp((level * kMaxStep + 50) / 100, 0, kMaxStep);
}

wchar_t GlyphFor(int step, bool charging) {
    return static_cast<wchar_t>((charging ? kBatteryChargingBase : kBatteryBase) + step);
}
} // namespace

BatteryIndicator::BatteryIndicator() {
    InitializeComponent();
}

void BatteryIndicator::ShowStep(int step, bool charging) {
    const wchar_t level[2]{GlyphFor(step, charging), L'\0'};
    // 外框那一层用同一组的 0 档，它只有空壳（充电组还带闪电），压在电量上正好把边线盖回
    // 文字色。
    const wchar_t shell[2]{charging ? kBatteryChargingBase : kBatteryBase, L'\0'};
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
        m_chargingFrame = (m_chargingFrame + 1) % (kMaxStep + 1);
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
    ShowStep(StepFor(level), false);
}

} // namespace winrt::EGoTouchSettings::implementation
