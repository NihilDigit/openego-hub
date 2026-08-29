#include "pch.h"

#include "NotificationWindow.xaml.h"
#include "AccessoryImageLoader.h"

#include <dwmapi.h>

#include <cmath>

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
// 换一条通知时淡出与收缩共用的时长。
constexpr int kSwapMs = 130;
constexpr double kPenImageMaxWidthDip = 304.0;
constexpr double kPenImageMaxHeightDip = 28.0;
constexpr double kKeyboardImageMaxWidthDip = 96.0;
constexpr double kKeyboardImageMaxHeightDip = 66.0;
constexpr UINT kDwmwaWindowCornerPreference = 33;
constexpr int kDwmCornerRound = 2;
constexpr UINT kDwmwaUseImmersiveDarkMode = 20;
constexpr UINT kDwmwaBorderColor = 34;
// COLORREF 是 0x00BBGGRR。取值贴着亚克力卡片在两种主题下的实际明度，见 ApplyTheme。
constexpr COLORREF kBorderColorDark = 0x00303030;
constexpr COLORREF kBorderColorLight = 0x00DFDFDF;

}

NotificationWindow::NotificationWindow() {
    InitializeComponent();
    ConfigureWindow();

    NotificationRoot().ActualThemeChanged(
        [this](FrameworkElement const&, IInspectable const&) { SyncFrameTheme(); });
    // 元素上树之前 ActualTheme 还不是生效值，首次同步要等到这里。
    NotificationRoot().Loaded(
        [this](IInspectable const&, RoutedEventArgs const&) { SyncFrameTheme(); });

    m_dwellTimer = DispatcherTimer();
    m_dwellTimer.Interval(Windows::Foundation::TimeSpan{30'000'000});
    m_dwellTimer.Tick([this](IInspectable const&, IInspectable const&) {
        m_dwellTimer.Stop();
        StartOpacityAnimation(1.0, 0.0, 300, [this] { ShowWindow(WindowHandle(), SW_HIDE); });
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

    // SetBorderAndTitleBar(false, false) 不动窗口样式位，WS_CAPTION/WS_DLGFRAME 仍留在那里，
    // DWM 据此在边框最内侧画一条高光——深色卡片外面那圈白边就是它。这条线不受
    // DWMWA_BORDER_COLOR、ImmersiveDarkMode、NCRENDERING_POLICY 和圆角偏好中的任何一个影响，
    // 逐个试过；把样式位剥成纯 WS_POPUP，DWM 就不再画它。
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME | WS_SYSMENU);
    style |= WS_POPUP;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
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

RECT NotificationWindow::TargetBounds(int widthDip, int heightDip) {
    const UINT dpi = GetDpiForWindow(WindowHandle());
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
    const int left = anchorX - width / 2;
    const int top = anchorY - height / 2;
    return RECT{left, top, left + width, top + height};
}

void NotificationWindow::PositionAndShow(int widthDip, int heightDip) {
    const RECT bounds = TargetBounds(widthDip, heightDip);
    SetWindowPos(WindowHandle(), HWND_TOPMOST, bounds.left, bounds.top,
                 bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// 窗口大小没有补间可用——SetWindowPos 是一次到位的，所以自己按帧插值。两条通知的尺寸差得
// 不小（连接提示比工具提示宽一半），直接跳过去，卡片会在淡出的中途忽然变形。
void NotificationWindow::AnimateBounds(int widthDip, int heightDip, int milliseconds) {
    const RECT from = [this] {
        RECT r{};
        GetWindowRect(WindowHandle(), &r);
        return r;
    }();
    const RECT to = TargetBounds(widthDip, heightDip);

    if (m_boundsTimer) m_boundsTimer.Stop();
    m_boundsTimer = DispatcherTimer();
    m_boundsTimer.Interval(std::chrono::milliseconds(16));
    const auto started = GetTickCount64();
    m_boundsTimer.Tick([this, from, to, started, milliseconds](
                           IInspectable const&, IInspectable const&) {
        const auto elapsed = static_cast<double>(GetTickCount64() - started);
        const double t = std::clamp(elapsed / std::max(1, milliseconds), 0.0, 1.0);
        // 三次缓出：起步快、收尾慢，与同时进行的淡出是同一种手感。
        const double eased = 1.0 - std::pow(1.0 - t, 3.0);
        const auto lerp = [eased](LONG a, LONG b) {
            return static_cast<int>(std::lround(a + (b - a) * eased));
        };
        const int left = lerp(from.left, to.left);
        const int top = lerp(from.top, to.top);
        SetWindowPos(WindowHandle(), HWND_TOPMOST, left, top,
                     lerp(from.right, to.right) - left, lerp(from.bottom, to.bottom) - top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (t >= 1.0) {
            m_boundsTimer.Stop();
            m_boundsTimer = nullptr;
        }
    });
    m_boundsTimer.Start();
}

void NotificationWindow::StartOpacityAnimation(
        double from, double to, int milliseconds, std::function<void()> onComplete) {
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
    if (onComplete) {
        storyboard.Completed(
            [action = std::move(onComplete)](IInspectable const&, IInspectable const&) {
                action();
            });
    }
    m_storyboard = storyboard;
    storyboard.Begin();
}

// 换一条通知时先把当前这张淡出，等看不见了再换内容、改窗口大小，然后淡入。窗口尺寸是一次
// SetWindowPos，本来就没法补间；趁不可见时改，跳变就看不出来了。窗口没在显示时不必淡出，
// 直接走淡入那一半。
void NotificationWindow::PresentView(
        UIElement const& view, int widthDip, int heightDip, std::function<void()> apply) {
    auto lifetime = get_strong();
    const auto swap = [this, lifetime, view, widthDip, heightDip,
                       apply = std::move(apply)]() {
        if (apply) apply();
        SelectView(view);
        NotificationRoot().Opacity(0.0);
        PositionAndShow(widthDip, heightDip);
        StartOpacityAnimation(0.0, 1.0, 150, nullptr);
        RestartDwellTimer();
    };

    if (!IsWindowVisible(WindowHandle())) {
        swap();
        return;
    }
    // 已经在显示：一边淡出旧的一边把窗口收到新尺寸，淡完再换内容。停留计时也推迟到那时。
    if (m_dwellTimer) m_dwellTimer.Stop();
    AnimateBounds(widthDip, heightDip, kSwapMs);
    StartOpacityAnimation(NotificationRoot().Opacity(), 0.0, kSwapMs, swap);
}

void NotificationWindow::RestartDwellTimer() {
    if (m_dwellTimer.IsEnabled()) m_dwellTimer.Stop();
    m_dwellTimer.Start();
}

void NotificationWindow::ShowConnected(const PenStatus::State& state) {
    PresentView(ConnectionView(), kConnectionWidthDip, kConnectionHeightDip, [this, state] {
        ApplyConnectedState(state);
        if (m_penImageSource) {
            ApplyNoUpscaleSize(
                PenImage(), m_penImagePixelSize,
                kPenImageMaxWidthDip, kPenImageMaxHeightDip);
        }
    });
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
    PresentView(
        KeyboardConnectionView(), kConnectionWidthDip, kConnectionHeightDip, [this, state] {
            ApplyKeyboardState(state);
            if (m_keyboardImageSource) {
                ApplyNoUpscaleSize(
                    KeyboardImage(), m_keyboardImagePixelSize,
                    kKeyboardImageMaxWidthDip, kKeyboardImageMaxHeightDip);
            }
        });
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
    PresentView(DeviationView(), kDeviationWidthDip, kDeviationHeightDip, nullptr);
}

void NotificationWindow::ShowToolChanged(bool eraser) {
    PresentView(ToolView(), kToolWidthDip, kToolHeightDip, [this, eraser] {
        // 橡皮擦与笔尖各用一个字形，文本本身已经说清楚了是哪一支，图标只是让扫一眼就能分辨。
        ToolPenIcon().Visibility(eraser ? Visibility::Collapsed : Visibility::Visible);
        ToolEraserIcon().Visibility(eraser ? Visibility::Visible : Visibility::Collapsed);
        // 两句等长，切换时卡片的宽度和字的位置都不动；「书写／橡皮」也是设置里那一项
        // 「切换书写与橡皮擦」用的词。
        ToolText().Text(eraser ? L"已切换到橡皮" : L"已切换到书写");
    });
}

// 弹窗是独立的 Window，主题不从主窗口继承，由 MainWindow 在每次弹出前推给它。深浅也一并
// 传进来：跟随系统时 theme 是 Default，而 ActualTheme 在窗口刚建好、内容还没上树时读不到
// 真正生效的那一档——照它设边框色，深色下会得到一条白边，正是要修的东西。
void NotificationWindow::ApplyTheme(ElementTheme theme) {
    NotificationRoot().RequestedTheme(theme);
}

// 窗口的边界由 DWM 画，不归 RequestedTheme 管，得按 ActualTheme 单独交代。
void NotificationWindow::SyncFrameTheme() {
    const bool dark = NotificationRoot().ActualTheme() == ElementTheme::Dark;
    const HWND hwnd = WindowHandle();

    const BOOL immersive = dark ? TRUE : FALSE;
    (void)DwmSetWindowAttribute(hwnd, kDwmwaUseImmersiveDarkMode, &immersive, sizeof(immersive));
    // 卡片自己不描边，边界只剩这一条。DWMWA_COLOR_NONE 对这个无标题栏的窗口不起作用，实测
    // 仍会描一条系统默认色的线，所以给它一个贴着卡片的颜色，让它退到轮廓的位置上。
    const COLORREF border = dark ? kBorderColorDark : kBorderColorLight;
    (void)DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &border, sizeof(border));
}

void NotificationWindow::HideNotification() {
    if (m_dwellTimer) m_dwellTimer.Stop();
    ShowWindow(WindowHandle(), SW_HIDE);
}

} // namespace winrt::EGoTouchSettings::implementation
