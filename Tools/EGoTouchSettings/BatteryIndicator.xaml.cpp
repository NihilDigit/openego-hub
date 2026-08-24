#include "pch.h"

#include "BatteryIndicator.xaml.h"

#if __has_include("BatteryIndicator.g.cpp")
#include "BatteryIndicator.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Media::Imaging;

namespace winrt::EGoTouchSettings::implementation {

BatteryIndicator::BatteryIndicator() {
    InitializeComponent();
}

int BatteryIndicator::BatteryStepFor(uint8_t level) noexcept {
    constexpr int steps[] = {5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 95, 100};
    for (const int step : steps) {
        if (level <= step) return step;
    }
    return 100;
}

hstring BatteryIndicator::BatteryImagePath(int step, bool charging) {
    wchar_t root[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, 0, root))) return {};

    std::wstring path = root;
    path += L"\\Huawei\\PCManager\\res\\drawable\\iconnect\\commonResources\\discover\\battery\\battery_white";
    path += std::to_wstring(step);
    if (charging) path += L"_charge";
    path += L".png";
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ? hstring{} : hstring{path};
}

void BatteryIndicator::SetState(bool hasLevel, uint8_t level, bool charging) {
    IndicatorRoot().Visibility(hasLevel ? Visibility::Visible : Visibility::Collapsed);
    if (!hasLevel) return;

    level = std::min<uint8_t>(level, 100);
    BatteryValue().Text(to_hstring(static_cast<unsigned>(level)));
    BatteryFill().Width(std::max(1.0, 17.0 * static_cast<double>(level) / 100.0));
    ChargingGlyph().Visibility(charging ? Visibility::Visible : Visibility::Collapsed);

    const hstring path = BatteryImagePath(BatteryStepFor(level), charging);
    if (!path.empty()) {
        if (path != m_imagePath || !m_bitmap) {
            BitmapImage bitmap;
            bitmap.UriSource(Windows::Foundation::Uri{path});
            m_bitmap = bitmap;
            m_imagePath = path;
            BatteryImage().Source(bitmap);
        }
        BatteryImage().Visibility(Visibility::Visible);
        BatteryFallback().Visibility(Visibility::Collapsed);
    } else {
        m_bitmap = nullptr;
        m_imagePath = {};
        BatteryImage().Source(nullptr);
        BatteryImage().Visibility(Visibility::Collapsed);
        BatteryFallback().Visibility(Visibility::Visible);
    }
}

} // namespace winrt::EGoTouchSettings::implementation
