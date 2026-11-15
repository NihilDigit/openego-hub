#include "pch.h"

#include "NotificationWindow.xaml.h"
#include "AccessoryImageLoader.h"
#include "AccessoryBatteryState.h"

#include <dwmapi.h>

#include <cmath>

#if __has_include("NotificationWindow.g.cpp")
#include "NotificationWindow.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Windowing;
using namespace Microsoft::UI::Xaml;

namespace winrt::EGoTouchSettings::implementation {

namespace {
constexpr int kConnectionWidthDip = 336;
constexpr int kConnectionHeightDip = 102;
constexpr int kDeviationWidthDip = 336;
constexpr int kDeviationHeightDip = 58;
constexpr int kToolWidthDip = 200;
constexpr int kToolHeightDip = 58;
constexpr int kEnterMs = 180;
constexpr int kSwapFadeOutMs = 120;
constexpr int kSwapFadeInMs = 160;
constexpr int kExitMs = 200;
constexpr int kEnterSlideDip = 14;
constexpr int kSwapSlideDip = 8;
constexpr int kExitSlideDip = 10;
constexpr int kTopWorkMarginDip = 24;
constexpr uint32_t kImagePrefetchBudgetMs = 150;
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

bool ClientAnimationsEnabled() noexcept {
    BOOL enabled = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)
        ? (enabled != FALSE)
        : true;
}

void SignalCompletion(const std::shared_ptr<winrt::handle>& signalOnDone) noexcept {
    if (signalOnDone && signalOnDone->get()) {
        (void)SetEvent(signalOnDone->get());
    }
}

} // namespace

NotificationWindow::NotificationWindow() {
    InitializeComponent();
    ConfigureWindow();

    NotificationRoot().ActualThemeChanged(
        [this](FrameworkElement const&, IInspectable const&) { SyncFrameTheme(); });
    // 元素上树之前 ActualTheme 还不是生效值，首次同步要等到这里。
    NotificationRoot().Loaded(
        [this](IInspectable const&, RoutedEventArgs const&) { SyncFrameTheme(); });

    m_windowAnimTimer = this->DispatcherQueue().CreateTimer();
    m_windowAnimTimer.Interval(std::chrono::milliseconds(8));
    m_windowAnimTimer.IsRepeating(true);
    m_windowAnimTickToken = m_windowAnimTimer.Tick(
        [this](Microsoft::UI::Dispatching::DispatcherQueueTimer const& sender, IInspectable const&) {
            if (m_shutdown || !m_presented || !m_windowAnimRunning ||
                m_animGeneration != m_presentationGeneration) {
                m_windowAnimRunning = false;
                m_animOnDone = nullptr;
                sender.Stop();
                return;
            }
            const HWND hwnd = WindowHandle();
            const double elapsed = static_cast<double>(GetTickCount64() - m_animStartTick);
            const double t =
                std::clamp(elapsed / static_cast<double>(m_animDurationMs), 0.0, 1.0);
            const double eased = m_animEaseOut
                ? (1.0 - std::pow(1.0 - t, 3.0))
                : (t * t * t);

            const auto lerp = [eased](LONG a, LONG b) {
                return static_cast<int>(std::lround(a + (b - a) * eased));
            };
            const int left = lerp(m_animFromRect.left, m_animToRect.left);
            const int top = lerp(m_animFromRect.top, m_animToRect.top);
            const int width = lerp(m_animFromRect.right, m_animToRect.right) - left;
            const int height = lerp(m_animFromRect.bottom, m_animToRect.bottom) - top;
            const int alphaInt = static_cast<int>(std::lround(
                m_animFromAlpha +
                (static_cast<int>(m_animToAlpha) - static_cast<int>(m_animFromAlpha)) * eased));
            m_currentAlpha = static_cast<uint8_t>(std::clamp(alphaInt, 0, 255));

            SetLayeredWindowAttributes(hwnd, 0, m_currentAlpha, LWA_ALPHA);
            SetWindowPos(hwnd, HWND_TOPMOST, left, top, width, height,
                         SWP_NOACTIVATE | SWP_NOZORDER);

            if (t >= 1.0) {
                m_windowAnimRunning = false;
                sender.Stop();
                auto onDone = std::move(m_animOnDone);
                m_animOnDone = nullptr;
                if (onDone) onDone();
            }
        });

    m_dwellTimer = this->DispatcherQueue().CreateTimer();
    m_dwellTimer.Interval(std::chrono::seconds(3));
    m_dwellTimer.IsRepeating(false);
    m_dwellTickToken = m_dwellTimer.Tick(
        [this](Microsoft::UI::Dispatching::DispatcherQueueTimer const& sender, IInspectable const&) {
            sender.Stop();
            const uint64_t generation = m_dwellGeneration;
            if (m_shutdown || !m_presented || generation != m_presentationGeneration) return;
            if (!ClientAnimationsEnabled()) {
                HideNotification();
                return;
            }
            const HWND hwnd = WindowHandle();
            RECT curRect{};
            GetWindowRect(hwnd, &curRect);
            const UINT rawDpi = GetDpiForWindow(hwnd);
            const UINT dpi = rawDpi ? rawDpi : 96;
            const int exitPx = MulDiv(kExitSlideDip, dpi, 96);
            const RECT exitRect{
                curRect.left, curRect.top - exitPx, curRect.right, curRect.bottom - exitPx};
            StartWindowMotion(curRect, exitRect, m_currentAlpha, 0, kExitMs, false,
                              [this, generation] {
                                  if (!m_shutdown && generation == m_presentationGeneration) {
                                      HideNotification();
                                  }
                              });
        });
}

NotificationWindow::~NotificationWindow() {
    if (m_dwellTimer) {
        m_dwellTimer.Stop();
        if (m_dwellTickToken.value) {
            m_dwellTimer.Tick(m_dwellTickToken);
            m_dwellTickToken = {};
        }
    }
    if (m_windowAnimTimer) {
        m_windowAnimTimer.Stop();
        if (m_windowAnimTickToken.value) {
            m_windowAnimTimer.Tick(m_windowAnimTickToken);
            m_windowAnimTickToken = {};
        }
    }
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
    exStyle |= WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

    // SetBorderAndTitleBar(false, false) 不动窗口样式位，WS_CAPTION/WS_DLGFRAME 仍留在那里，
    // DWM 据此在边框最内侧画一条高光——深色卡片外面那圈白边就是它。
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME | WS_SYSMENU);
    style |= WS_POPUP;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);

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

fire_and_forget NotificationWindow::LoadPenImage(
        uint32_t modelId,
        std::shared_ptr<winrt::handle> signalOnDone) {
    auto lifetime = get_strong();
    if (m_shutdown) {
        SignalCompletion(signalOnDone);
        co_return;
    }
    const ULONGLONG now = GetTickCount64();
    const bool modelChanged = modelId != m_penModelId;
    if (!modelChanged && m_penImageSource) {
        SignalCompletion(signalOnDone);
        co_return;
    }
    if (!modelChanged && now < m_penImageRetryAt) {
        SignalCompletion(signalOnDone);
        co_return;
    }

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
        if (!asset) {
            SignalCompletion(signalOnDone);
            co_return;
        }
        Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource source;
        const auto pixelSize = co_await AccessoryImages::DecodeCroppedAsync(
            std::move(asset.encoded), source);
        if (m_shutdown || pixelSize.Width <= 0 || generation != m_penImageGeneration ||
            modelId != m_penModelId) {
            SignalCompletion(signalOnDone);
            co_return;
        }
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
    SignalCompletion(signalOnDone);
}

fire_and_forget NotificationWindow::LoadKeyboardImage(
        std::shared_ptr<winrt::handle> signalOnDone) {
    auto lifetime = get_strong();
    if (m_shutdown) {
        SignalCompletion(signalOnDone);
        co_return;
    }
    if (m_keyboardImageSource) {
        SignalCompletion(signalOnDone);
        co_return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now < m_keyboardImageRetryAt) {
        SignalCompletion(signalOnDone);
        co_return;
    }
    m_keyboardImageRetryAt = now + 5000;
    const uint64_t generation = ++m_keyboardImageGeneration;

    try {
        auto asset = AccessoryImages::ResolveKeyboard();
        if (!asset) {
            SignalCompletion(signalOnDone);
            co_return;
        }
        Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource source;
        const auto pixelSize = co_await AccessoryImages::DecodeCroppedAsync(
            std::move(asset.encoded), source);
        if (m_shutdown || pixelSize.Width <= 0 || generation != m_keyboardImageGeneration) {
            SignalCompletion(signalOnDone);
            co_return;
        }
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
    SignalCompletion(signalOnDone);
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
    image.Width(std::max(1.0, nativeWidthDip * fit));
    image.Height(std::max(1.0, nativeHeightDip * fit));
}

RECT NotificationWindow::TargetBounds(int widthDip, int heightDip) {
    const UINT dpi = GetDpiForWindow(WindowHandle());
    const UINT effectiveDpi = dpi ? dpi : 96;
    const int width = MulDiv(widthDip, effectiveDpi, 96);
    const int height = MulDiv(heightDip, effectiveDpi, 96);

    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    RECT work{};
    if (GetMonitorInfoW(monitor, &info)) work = info.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    const int topOffset = MulDiv(kTopWorkMarginDip, effectiveDpi, 96);
    const int minLeft = static_cast<int>(work.left);
    const int maxLeft = (std::max)(minLeft, static_cast<int>(work.right) - width);
    const int anchorX = minLeft + static_cast<int>(work.right - work.left) / 2;
    const int left = std::clamp(anchorX - width / 2, minLeft, maxLeft);
    const int minTop = static_cast<int>(work.top);
    const int maxTop = (std::max)(minTop, static_cast<int>(work.bottom) - height);
    const int top = std::clamp(minTop + topOffset, minTop, maxTop);
    return RECT{left, top, left + width, top + height};
}

// 整窗位移 + 整窗透明度补间：亚克力卡片、DWM 圆角边框与内部元素作为一个整体同步进出，
// 不出现“边框不动、里面元素独自飘”的割裂感。
void NotificationWindow::StartWindowMotion(
        RECT fromRect, RECT toRect,
        uint8_t fromAlpha, uint8_t toAlpha,
        int durationMs, bool easeOut,
        std::function<void()> onDone) {
    if (m_windowAnimTimer) m_windowAnimTimer.Stop();
    m_windowAnimRunning = false;
    m_animOnDone = nullptr;

    const HWND hwnd = WindowHandle();
    if (!ClientAnimationsEnabled() || durationMs <= 0) {
        m_currentAlpha = toAlpha;
        SetLayeredWindowAttributes(hwnd, 0, toAlpha, LWA_ALPHA);
        SetWindowPos(hwnd, HWND_TOPMOST, toRect.left, toRect.top,
                     toRect.right - toRect.left, toRect.bottom - toRect.top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        if (onDone) onDone();
        return;
    }

    m_animFromRect = fromRect;
    m_animToRect = toRect;
    m_animFromAlpha = fromAlpha;
    m_animToAlpha = toAlpha;
    m_animDurationMs = durationMs;
    m_animEaseOut = easeOut;
    m_animStartTick = GetTickCount64();
    m_animGeneration = m_presentationGeneration;
    m_animOnDone = std::move(onDone);
    m_windowAnimRunning = true;

    m_currentAlpha = fromAlpha;
    SetLayeredWindowAttributes(hwnd, 0, fromAlpha, LWA_ALPHA);
    SetWindowPos(hwnd, HWND_TOPMOST, fromRect.left, fromRect.top,
                 fromRect.right - fromRect.left, fromRect.bottom - fromRect.top,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    if (m_windowAnimTimer) m_windowAnimTimer.Start();
}

void NotificationWindow::PresentView(
        UIElement const& view, int widthDip, int heightDip, std::function<void()> apply) {
    if (m_shutdown) return;
    const HWND hwnd = WindowHandle();
    const bool wasVisible = m_presented && IsWindowVisible(hwnd);
    if (m_dwellTimer) m_dwellTimer.Stop();
    if (m_windowAnimTimer) m_windowAnimTimer.Stop();
    m_windowAnimRunning = false;
    m_animOnDone = nullptr;

    const auto generation = ++m_presentationGeneration;
    m_presented = true;

    const UINT rawDpi = GetDpiForWindow(hwnd);
    const UINT effectiveDpi = rawDpi ? rawDpi : 96;

    const auto enterNewView = [this, hwnd, generation, view, widthDip, heightDip, effectiveDpi,
                               apply = std::move(apply)]() {
        if (m_shutdown || generation != m_presentationGeneration) return;
        if (apply) apply();
        SelectView(view);
        m_connectionView = view == ConnectionView().as<UIElement>() ||
                           view == KeyboardConnectionView().as<UIElement>();
        if (!SystemBackdrop()) {
            SystemBackdrop(Microsoft::UI::Xaml::Media::DesktopAcrylicBackdrop{});
        }
        SyncFrameTheme();

        const RECT toRect = TargetBounds(widthDip, heightDip);
        const int slidePx = MulDiv(kEnterSlideDip, effectiveDpi, 96);
        const RECT fromRect{toRect.left, toRect.top - slidePx, toRect.right, toRect.bottom - slidePx};

        m_currentAlpha = 0;
        SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
        SetWindowPos(hwnd, HWND_TOPMOST, fromRect.left, fromRect.top,
                     fromRect.right - fromRect.left, fromRect.bottom - fromRect.top,
                     SWP_NOACTIVATE);
        AppWindow().Show(false);
        SetWindowPos(hwnd, HWND_TOPMOST, fromRect.left, fromRect.top,
                     fromRect.right - fromRect.left, fromRect.bottom - fromRect.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);

        StartWindowMotion(fromRect, toRect, 0, 255, kEnterMs, true);
        RestartDwellTimer();
        if (m_onRefreshDemandChanged) m_onRefreshDemandChanged();
    };

    if (!wasVisible || !ClientAnimationsEnabled()) {
        enterNewView();
        return;
    }

    RECT curRect{};
    GetWindowRect(hwnd, &curRect);
    const RECT nextTarget = TargetBounds(widthDip, heightDip);
    const int swapSlidePx = MulDiv(kSwapSlideDip, effectiveDpi, 96);
    const RECT midRect{
        nextTarget.left, nextTarget.top - swapSlidePx,
        nextTarget.right, nextTarget.bottom - swapSlidePx};

    StartWindowMotion(
        curRect, midRect, m_currentAlpha, 0, kSwapFadeOutMs, false,
        [this, hwnd, generation, view, midRect, nextTarget, apply = std::move(apply)]() {
            if (m_shutdown || generation != m_presentationGeneration) return;
            if (apply) apply();
            SelectView(view);
            m_connectionView = view == ConnectionView().as<UIElement>() ||
                               view == KeyboardConnectionView().as<UIElement>();
            SyncFrameTheme();
            StartWindowMotion(midRect, nextTarget, 0, 255, kSwapFadeInMs, true);
            RestartDwellTimer();
            if (m_onRefreshDemandChanged) m_onRefreshDemandChanged();
        });
    if (m_onRefreshDemandChanged) m_onRefreshDemandChanged();
}

void NotificationWindow::RestartDwellTimer() {
    if (!m_dwellTimer) return;
    m_dwellTimer.Stop();
    m_dwellGeneration = m_presentationGeneration;
    m_dwellTimer.Start();
}

fire_and_forget NotificationWindow::ShowConnected(PenStatus::State state) {
    auto lifetime = get_strong();
    const uint64_t showGeneration = ++m_showGeneration;
    if (!m_penImageSource || m_penModelId != state.modelId) {
        auto readyEvent = std::make_shared<winrt::handle>(
            CreateEventW(nullptr, TRUE, FALSE, nullptr));
        LoadPenImage(state.modelId, readyEvent);
        if (readyEvent->get()) {
            winrt::apartment_context uiThread;
            (void)co_await winrt::resume_on_signal(
                readyEvent->get(), std::chrono::milliseconds(kImagePrefetchBudgetMs));
            co_await uiThread;
        }
    }
    if (m_shutdown || showGeneration != m_showGeneration) co_return;

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
        AccessoryBattery::HasPenBattery(state), state.batteryLevel,
        state.hasChargingState && state.charging);

    LoadPenImage(state.modelId);
    const bool hasImage = m_penImageSource != nullptr;
    PenImage().Visibility(hasImage ? Visibility::Visible : Visibility::Collapsed);
    PenImageFallback().Visibility(hasImage ? Visibility::Collapsed : Visibility::Visible);
}

fire_and_forget NotificationWindow::ShowKeyboardConnected(PenStatus::State state) {
    auto lifetime = get_strong();
    const uint64_t showGeneration = ++m_showGeneration;
    if (!m_keyboardImageSource) {
        auto readyEvent = std::make_shared<winrt::handle>(
            CreateEventW(nullptr, TRUE, FALSE, nullptr));
        LoadKeyboardImage(readyEvent);
        if (readyEvent->get()) {
            winrt::apartment_context uiThread;
            (void)co_await winrt::resume_on_signal(
                readyEvent->get(), std::chrono::milliseconds(kImagePrefetchBudgetMs));
            co_await uiThread;
        }
    }
    if (m_shutdown || showGeneration != m_showGeneration) co_return;

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
        AccessoryBattery::HasKeyboardBattery(state), state.kbdBatteryLevel,
        state.hasKbdCharging && state.kbdCharging);
    LoadKeyboardImage();
    const bool hasImage = m_keyboardImageSource != nullptr;
    KeyboardImage().Visibility(hasImage ? Visibility::Visible : Visibility::Collapsed);
    KeyboardImageFallback().Visibility(hasImage ? Visibility::Collapsed : Visibility::Visible);
}

void NotificationWindow::UpdateState(const PenStatus::State& state) {
    if (!NeedsStateRefresh()) return;
    if (ConnectionView().Visibility() == Visibility::Visible) {
        ApplyConnectedState(state);
    } else if (KeyboardConnectionView().Visibility() == Visibility::Visible) {
        ApplyKeyboardState(state);
    }
}

void NotificationWindow::ShowDeviation() {
    ++m_showGeneration;
    PresentView(DeviationView(), kDeviationWidthDip, kDeviationHeightDip, nullptr);
}

void NotificationWindow::ShowToolChanged(bool eraser) {
    ++m_showGeneration;
    PresentView(ToolView(), kToolWidthDip, kToolHeightDip, [this, eraser] {
        ToolPenIcon().Visibility(eraser ? Visibility::Collapsed : Visibility::Visible);
        ToolEraserIcon().Visibility(eraser ? Visibility::Visible : Visibility::Collapsed);
        ToolText().Text(eraser ? L"已切换到橡皮" : L"已切换到书写");
    });
}

void NotificationWindow::ApplyTheme(ElementTheme theme) {
    NotificationRoot().RequestedTheme(theme);
}

void NotificationWindow::SyncFrameTheme() {
    const bool dark = NotificationRoot().ActualTheme() == ElementTheme::Dark;
    const HWND hwnd = WindowHandle();

    const BOOL immersive = dark ? TRUE : FALSE;
    (void)DwmSetWindowAttribute(hwnd, kDwmwaUseImmersiveDarkMode, &immersive, sizeof(immersive));
    const COLORREF border = dark ? kBorderColorDark : kBorderColorLight;
    (void)DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &border, sizeof(border));
}

bool NotificationWindow::NeedsStateRefresh() const noexcept {
    return !m_shutdown && m_presented && m_connectionView;
}

void NotificationWindow::SetRefreshDemandChanged(std::function<void()> cb) {
    m_onRefreshDemandChanged = std::move(cb);
}

void NotificationWindow::HideNotification() {
    ++m_showGeneration;
    ++m_presentationGeneration;
    if (m_dwellTimer) m_dwellTimer.Stop();
    if (m_windowAnimTimer) m_windowAnimTimer.Stop();
    m_windowAnimRunning = false;
    m_animOnDone = nullptr;
    const HWND hwnd = WindowHandle();
    m_currentAlpha = 0;
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    AppWindow().Hide();
    ShowWindow(hwnd, SW_HIDE);
    SystemBackdrop(nullptr);
    m_presented = false;
    m_connectionView = false;
    if (m_onRefreshDemandChanged) m_onRefreshDemandChanged();
}

void NotificationWindow::Shutdown() {
    if (m_shutdown) return;
    m_shutdown = true;
    ++m_penImageGeneration;
    ++m_keyboardImageGeneration;
    m_onRefreshDemandChanged = nullptr;
    HideNotification();
}

} // namespace winrt::EGoTouchSettings::implementation
