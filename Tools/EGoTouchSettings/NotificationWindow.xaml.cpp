#include "pch.h"

#include "NotificationWindow.xaml.h"
#include "AccessoryImageLoader.h"

#include <dwmapi.h>

#if __has_include("NotificationWindow.g.cpp")
#include "NotificationWindow.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Windowing;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Media::Animation;

namespace winrt::EGoTouchSettings::implementation {

namespace {
constexpr int kConnectionWidthDip = 336;
constexpr int kConnectionHeightDip = 102;
constexpr int kDeviationWidthDip = 336;
constexpr int kDeviationHeightDip = 58;
constexpr int kToolWidthDip = 200;
constexpr int kToolHeightDip = 58;
constexpr double kPenImageMaxWidthDip = 304.0;
constexpr double kPenImageMaxHeightDip = 28.0;
constexpr double kKeyboardImageMaxWidthDip = 96.0;
constexpr double kKeyboardImageMaxHeightDip = 66.0;
constexpr UINT kDwmwaWindowCornerPreference = 33;
constexpr int kDwmCornerRound = 2;

}

NotificationWindow::NotificationWindow() {
    InitializeComponent();
    ConfigureWindow();

    m_dwellTimer = DispatcherTimer();
    m_dwellTimer.Interval(Windows::Foundation::TimeSpan{30'000'000});
    m_dwellTimer.Tick([this](IInspectable const&, IInspectable const&) {
        m_dwellTimer.Stop();
        StartOpacityAnimation(1.0, 0.0, 300, true);
    });
}

NotificationWindow::~NotificationWindow() {
    if (m_dwellTimer) m_dwellTimer.Stop();
}

HWND NotificationWindow::WindowHandle() {
    winrt::EGoTouchSettings::NotificationWindow projected = *this;
    const auto windowNative = projected.as<IWindowNative>();
    HWND hwnd = nullptr;
    check_hresult(windowNative->get_WindowHandle(&hwnd));
    return hwnd;
}

void NotificationWindow::ConfigureWindow() {
    const HWND hwnd = WindowHandle();
    const WindowId id = GetWindowIdFromWindow(hwnd);
    const Microsoft::UI::Windowing::AppWindow appWindow =
        Microsoft::UI::Windowing::AppWindow::GetFromWindowId(id);
    if (const auto presenter = appWindow.Presenter().try_as<OverlappedPresenter>()) {
        presenter.SetBorderAndTitleBar(false, false);
        presenter.IsResizable(false);
        presenter.IsMaximizable(false);
        presenter.IsMinimizable(false);
    }

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    exStyle |= WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
    // 窗口本身是矩形的：亚克力底铺满整个客户区，圆角只能由 DWM 裁。卡片描边因此不能自己
    // 定半径——描边比裁切大时，四角会露出一截被裁掉的窗口底，看起来像圆角外又画了一圈边框。
    // XAML 那边用 OverlayCornerRadius，与这里的 ROUND 是同一档。
    const int corner = kDwmCornerRound;
    (void)DwmSetWindowAttribute(hwnd, kDwmwaWindowCornerPreference, &corner, sizeof(corner));
}

void NotificationWindow::SelectView(UIElement const& view) {
    for (const UIElement candidate :
         {ConnectionView().as<UIElement>(), KeyboardConnectionView().as<UIElement>(),
          ToolView().as<UIElement>(), DeviationView().as<UIElement>()}) {
        candidate.Visibility(candidate == view ? Visibility::Visible : Visibility::Collapsed);
    }
}

fire_and_forget NotificationWindow::LoadPenImage(uint32_t modelId) {
    auto lifetime = get_strong();
    const ULONGLONG now = GetTickCount64();
    const bool modelChanged = modelId != m_penModelId;
    if (!modelChanged && m_penImageSource) co_return;
    if (!modelChanged && now < m_penImageRetryAt) co_return;

    if (modelChanged) {
        m_penModelId = modelId;
        m_penImageSource = nullptr;
        PenImage().Source(nullptr);
        PenImage().Visibility(Visibility::Collapsed);
        PenImageFallback().Visibility(Visibility::Visible);
    }
    m_penImageRetryAt = now + 5000;
    const uint64_t generation = ++m_penImageGeneration;

    try {
        auto asset = AccessoryImages::ResolvePen(modelId);
        if (!asset) co_return;
        Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource source;
        const auto pixelSize = co_await AccessoryImages::DecodeCroppedAsync(
            std::move(asset.encoded), source);
        if (pixelSize.Width <= 0 || generation != m_penImageGeneration ||
            modelId != m_penModelId) co_return;
        m_penImageSource = source;
        m_penImagePixelSize = pixelSize;
        PenImage().Source(source);
        ApplyNoUpscaleSize(
            PenImage(), pixelSize, kPenImageMaxWidthDip, kPenImageMaxHeightDip);
        PenImage().Visibility(Visibility::Visible);
        PenImageFallback().Visibility(Visibility::Collapsed);
    } catch (...) {
        // 安装资源正被更新或图片损坏时保留 Fluent 占位图，后续状态刷新会重试。
    }
}

fire_and_forget NotificationWindow::LoadKeyboardImage() {
    auto lifetime = get_strong();
    if (m_keyboardImageSource) co_return;
    const ULONGLONG now = GetTickCount64();
    if (now < m_keyboardImageRetryAt) co_return;
    m_keyboardImageRetryAt = now + 5000;
    const uint64_t generation = ++m_keyboardImageGeneration;

    try {
        auto asset = AccessoryImages::ResolveKeyboard();
        if (!asset) co_return;
        Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource source;
        const auto pixelSize = co_await AccessoryImages::DecodeCroppedAsync(
            std::move(asset.encoded), source);
        if (pixelSize.Width <= 0 || generation != m_keyboardImageGeneration) co_return;
        m_keyboardImageSource = source;
        m_keyboardImagePixelSize = pixelSize;
        KeyboardImage().Source(source);
        ApplyNoUpscaleSize(
            KeyboardImage(), pixelSize,
            kKeyboardImageMaxWidthDip, kKeyboardImageMaxHeightDip);
        KeyboardImage().Visibility(Visibility::Visible);
        KeyboardImageFallback().Visibility(Visibility::Collapsed);
    } catch (...) {
        // 与设备页使用同一资源规则；失败时保留 Fluent 键盘图标并稍后重试。
    }
}

void NotificationWindow::ApplyNoUpscaleSize(
        Controls::Image const& image,
        Windows::Foundation::Size const& pixelSize,
        double maxWidthDip,
        double maxHeightDip) {
    if (pixelSize.Width <= 0 || pixelSize.Height <= 0) return;
    const UINT dpi = GetDpiForWindow(WindowHandle());
    const double rasterScale = dpi ? static_cast<double>(dpi) / 96.0 : 1.0;
    const double nativeWidthDip = pixelSize.Width / rasterScale;
    const double nativeHeightDip = pixelSize.Height / rasterScale;
    const double fit = std::min({
        1.0,
        maxWidthDip / nativeWidthDip,
        maxHeightDip / nativeHeightDip,
    });
    // Width/Height 换算回当前窗口的物理像素后不超过源图尺寸：只缩不放。
    image.Width(std::max(1.0, nativeWidthDip * fit));
    image.Height(std::max(1.0, nativeHeightDip * fit));
}

void NotificationWindow::PositionAndShow(int widthDip, int heightDip) {
    const HWND hwnd = WindowHandle();
    const UINT dpi = GetDpiForWindow(hwnd);
    const int width = MulDiv(widthDip, dpi ? dpi : 96, 96);
    const int height = MulDiv(heightDip, dpi ? dpi : 96, 96);

    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    RECT screen{};
    if (GetMonitorInfoW(monitor, &info)) screen = info.rcMonitor;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &screen, 0);

    const int anchorX = screen.left + (screen.right - screen.left) / 2;
    const int anchorY = screen.top + (screen.bottom - screen.top) / 10;
    SetWindowPos(hwnd, HWND_TOPMOST, anchorX - width / 2, anchorY - height / 2,
                 width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void NotificationWindow::StartOpacityAnimation(
        double from, double to, int milliseconds, bool hideWhenDone) {
    if (m_storyboard) m_storyboard.Stop();
    Storyboard storyboard;
    DoubleAnimation opacity;
    opacity.From(from);
    opacity.To(to);
    opacity.Duration(DurationHelper::FromTimeSpan(
        Windows::Foundation::TimeSpan{static_cast<int64_t>(milliseconds) * 10'000}));
    Storyboard::SetTarget(opacity, NotificationRoot());
    Storyboard::SetTargetProperty(opacity, L"Opacity");
    storyboard.Children().Append(opacity);
    if (hideWhenDone) {
        storyboard.Completed([this](IInspectable const&, IInspectable const&) {
            ShowWindow(WindowHandle(), SW_HIDE);
        });
    }
    m_storyboard = storyboard;
    storyboard.Begin();
}

void NotificationWindow::RestartDwellTimer() {
    if (m_dwellTimer.IsEnabled()) m_dwellTimer.Stop();
    m_dwellTimer.Start();
}

void NotificationWindow::ShowConnected(const PenStatus::State& state) {
    SelectView(ConnectionView());
    ApplyConnectedState(state);

    NotificationRoot().Opacity(0.0);
    PositionAndShow(kConnectionWidthDip, kConnectionHeightDip);
    if (m_penImageSource) {
        ApplyNoUpscaleSize(
            PenImage(), m_penImagePixelSize,
            kPenImageMaxWidthDip, kPenImageMaxHeightDip);
    }
    StartOpacityAnimation(0.0, 1.0, 150, false);
    RestartDwellTimer();
}

void NotificationWindow::ApplyConnectedState(const PenStatus::State& state) {
    PenNameText().Text(state.modelName[0] ? to_hstring(state.modelName) : L"HUAWEI M-Pencil");
    PenBatteryIndicator().SetState(
        state.hasBatteryLevel, state.batteryLevel,
        state.hasChargingState && state.charging);

    LoadPenImage(state.modelId);
    const bool hasImage = m_penImageSource != nullptr;
    PenImage().Visibility(hasImage ? Visibility::Visible : Visibility::Collapsed);
    PenImageFallback().Visibility(hasImage ? Visibility::Collapsed : Visibility::Visible);
}

void NotificationWindow::ShowKeyboardConnected(const PenStatus::State& state) {
    SelectView(KeyboardConnectionView());
    ApplyKeyboardState(state);

    NotificationRoot().Opacity(0.0);
    PositionAndShow(kConnectionWidthDip, kConnectionHeightDip);
    if (m_keyboardImageSource) {
        ApplyNoUpscaleSize(
            KeyboardImage(), m_keyboardImagePixelSize,
            kKeyboardImageMaxWidthDip, kKeyboardImageMaxHeightDip);
    }
    StartOpacityAnimation(0.0, 1.0, 150, false);
    RestartDwellTimer();
}

void NotificationWindow::ApplyKeyboardState(const PenStatus::State& state) {
    KeyboardNameText().Text(
        state.kbdModelName[0] ? to_hstring(state.kbdModelName) : L"HUAWEI 智能磁吸键盘");
    KeyboardBatteryIndicator().SetState(
        state.hasKbdBattery, state.kbdBatteryLevel,
        state.hasKbdCharging && state.kbdCharging);
    LoadKeyboardImage();
    const bool hasImage = m_keyboardImageSource != nullptr;
    KeyboardImage().Visibility(hasImage ? Visibility::Visible : Visibility::Collapsed);
    KeyboardImageFallback().Visibility(hasImage ? Visibility::Collapsed : Visibility::Visible);
}

void NotificationWindow::UpdateState(const PenStatus::State& state) {
    if (!IsWindowVisible(WindowHandle())) return;
    // 只改当前视图内容，不重启动画和停留计时。
    if (ConnectionView().Visibility() == Visibility::Visible) {
        ApplyConnectedState(state);
    } else if (KeyboardConnectionView().Visibility() == Visibility::Visible) {
        ApplyKeyboardState(state);
    }
}

void NotificationWindow::ShowDeviation() {
    SelectView(DeviationView());
    NotificationRoot().Opacity(0.0);
    PositionAndShow(kDeviationWidthDip, kDeviationHeightDip);
    StartOpacityAnimation(0.0, 1.0, 150, false);
    RestartDwellTimer();
}

void NotificationWindow::ShowToolChanged(bool eraser) {
    SelectView(ToolView());
    // 橡皮擦与笔尖各用一个字形，文本本身已经说清楚了是哪一支，图标只是让扫一眼就能分辨。
    ToolPenIcon().Visibility(eraser ? Visibility::Collapsed : Visibility::Visible);
    ToolEraserIcon().Visibility(eraser ? Visibility::Visible : Visibility::Collapsed);
    ToolText().Text(eraser ? L"已切换到橡皮擦" : L"已切换到笔尖");
    NotificationRoot().Opacity(0.0);
    PositionAndShow(kToolWidthDip, kToolHeightDip);
    StartOpacityAnimation(0.0, 1.0, 150, false);
    RestartDwellTimer();
}

// 弹窗是独立的 Window，主题不从主窗口继承，由 MainWindow 在创建和切换时推给它。
void NotificationWindow::ApplyTheme(ElementTheme theme) {
    NotificationRoot().RequestedTheme(theme);
}

void NotificationWindow::HideNotification() {
    if (m_dwellTimer) m_dwellTimer.Stop();
    ShowWindow(WindowHandle(), SW_HIDE);
}

} // namespace winrt::EGoTouchSettings::implementation
