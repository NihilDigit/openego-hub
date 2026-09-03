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

void BatteryIndicator::SetState(bool hasLevel, uint8_t level, bool charging) {
    IndicatorRoot().Visibility(hasLevel ? Visibility::Visible : Visibility::Collapsed);
    if (!hasLevel) {
        return;
    }

    level = std::min<uint8_t>(level, 100);
    BatteryValue().Text(to_hstring(static_cast<unsigned>(level)));

    // 电量低只在没充电时说得通：正在充的时候提醒电量低，用户能做的正是他已经在做的事。
    const bool low = !charging && level <= kLowLevel;
    BatteryLevelGlyph().Foreground(BrushFor(charging ? L"BatteryChargingBrush"
                                           : low    ? L"BatteryLowBrush"
                                                    : L"BatteryNormalBrush"));

    // 充电时同样画真实档位。这里曾经是一个 0 到 9 循环播放的动画，它占掉了图标唯一的表达位
    // 去说「在充」，而那件事闪电字形、绿色和旁边的百分比已经各说了一遍；代价是图标与真实电量
    // 对不上——98% 时它还在从空扫到满，扫一眼得到的印象要靠读数字纠正回来。
    // 何况插着电是常态不是事件，为常态配一个永不停止的动画，剩下的只有视觉噪音。
    ShowStep(StepFor(level), charging);
}

} // namespace winrt::EGoTouchSettings::implementation
