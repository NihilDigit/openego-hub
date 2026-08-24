#include "pch.h"

#include "MainWindow.xaml.h"
#include "AccessoryImageLoader.h"
#include "AppIconResource.h"

#include <cmath>

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Windowing;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace winrt::EGoTouchSettings::implementation {

namespace {

constexpr wchar_t kSettingsRegistryKey[] = L"Software\\OpenEGoHub";
// 默认尺寸要装得下全部四个分组共六张卡片，否则一打开就得滚动才能看全。内容区 MaxWidth 是
// 640，宽度取它加上左右各 24 的内边距。
constexpr int32_t kWindowWidth = 688;
constexpr int32_t kWindowHeight = 760;

} // namespace

MainWindow::MainWindow() {
    InitializeComponent();

    ExtendsContentIntoTitleBar(true);
    SetTitleBar(AppTitleBar());

    ConfigureWindow();
    CreateBridgeWindow();
    LoadStoredSettings();
    ShowAboutVersion();
    RefreshState();

    m_refreshTimer = DispatcherTimer();
    // 共享内存读取只是一次 240 字节 seqlock 快照。100 ms 的安全轮询让充电边沿即使错过
    // update event 也能及时反映到设备页和正在显示的弹窗，不向 MCU 增加任何查询流量。
    m_refreshTimer.Interval(Windows::Foundation::TimeSpan{1'000'000});
    m_refreshTimer.Tick([this](IInspectable const&, IInspectable const&) {
        RefreshState();
    });
    m_refreshTimer.Start();

    Closed([this](IInspectable const&, WindowEventArgs const&) {
        if (m_refreshTimer) m_refreshTimer.Stop();
        if (m_notificationWindow) {
            m_notificationWindow.Close();
            m_notificationWindow = nullptr;
        }
        m_reader.Close();
        DestroyBridgeWindow();
    });
}

MainWindow::~MainWindow() {
    if (m_refreshTimer) m_refreshTimer.Stop();
    DestroyBridgeWindow();
}

HWND MainWindow::WindowHandle() {
    winrt::EGoTouchSettings::MainWindow projected = *this;
    const auto windowNative = projected.as<IWindowNative>();
    HWND hwnd = nullptr;
    check_hresult(windowNative->get_WindowHandle(&hwnd));
    return hwnd;
}

HWND MainWindow::FindTrayWindow() const {
    return FindWindowW(EGoTouchTrayIpc::kTrayWindowClass, nullptr);
}

void MainWindow::ConfigureWindow() {
    const HWND hwnd = WindowHandle();
    const WindowId windowId = GetWindowIdFromWindow(hwnd);
    const Microsoft::UI::Windowing::AppWindow appWindow =
        Microsoft::UI::Windowing::AppWindow::GetFromWindowId(windowId);

    // AppWindow.Resize/Move 的单位是物理像素，而 XAML 内容按 DIP 布局再乘 DPI 缩放。本机是
    // 高 DPI 平板（150%~200%），直接用 DIP 数值 Resize 会得到一个装不下内容的小窗口。按窗口
    // 当前 DPI 把 DIP 目标尺寸换算成物理像素。DisplayArea.WorkArea 同样是物理像素，居中计算
    // 与之一致，无需再换算。
    const UINT dpi = GetDpiForWindow(hwnd);
    const double scale = dpi > 0 ? dpi / 96.0 : 1.0;
    int32_t physicalWidth = static_cast<int32_t>(std::lround(kWindowWidth * scale));
    int32_t physicalHeight = static_cast<int32_t>(std::lround(kWindowHeight * scale));

    const DisplayArea areaCells = DisplayArea::GetFromWindowId(windowId, DisplayAreaFallback::Primary);
    const bool hasArea = areaCells != nullptr;
    RectInt32 work{};
    if (hasArea) {
        work = areaCells.WorkArea();
        // 先按工作区截断再 Resize：这台设备在 200% 缩放下，760 DIP 换算出来的物理高度已经接近
        // 屏幕可用高度，不截断的话窗口下沿会连同底部的卡片一起被推到屏幕外。
        physicalWidth = std::min(physicalWidth, work.Width);
        physicalHeight = std::min(physicalHeight, work.Height);
    }
    appWindow.Resize(SizeInt32{physicalWidth, physicalHeight});

    if (const auto presenter = appWindow.Presenter().try_as<OverlappedPresenter>()) {
        // 允许最大化。内容区自身有 MaxWidth 640 并居中，窗口放大不会把卡片拉成一整条。
        presenter.IsMaximizable(true);
        presenter.IsMinimizable(true);
    }

    if (hasArea) {
        appWindow.Move(PointInt32{
            work.X + std::max(0, (work.Width - physicalWidth) / 2),
            work.Y + std::max(0, (work.Height - physicalHeight) / 3)});
    }

    ApplyWindowIcon(hwnd, dpi);
}

// 显式给窗口挂图标。不挂的话壳层退回去读 exe 的图标资源，而那条路会被按**路径**缓存：
// 图标换了而 exe 路径没变时，任务栏可能一直显示旧的那张。WM_SETICON 设在窗口上，
// 任务栏与 Alt+Tab 直接取它，不经过那层缓存。
//
// 大小两档分别加载而不是让系统缩：.ico 里 16/20/24 那几档画的是简化版（去掉中间那层
// 等高线），拿 32 的缩下去会糊成一团。
void MainWindow::ApplyWindowIcon(HWND hwnd, UINT dpi) {
    const HINSTANCE module = GetModuleHandleW(nullptr);
    const auto load = [&](int metric) -> HICON {
        const int side = GetSystemMetricsForDpi(metric, dpi);
        return static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(IDI_APP_ICON),
                                             IMAGE_ICON, side, side, LR_SHARED));
    };
    if (const HICON large = load(SM_CXICON)) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large));
    }
    if (const HICON compact = load(SM_CXSMICON)) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(compact));
    }
}

void MainWindow::CreateBridgeWindow() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = BridgeWndProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = EGoTouchTrayIpc::kSettingsBridgeWindowClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return;
    }

    m_bridgeWindow = CreateWindowExW(
        0, EGoTouchTrayIpc::kSettingsBridgeWindowClass, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), this);
}

void MainWindow::DestroyBridgeWindow() {
    if (!m_bridgeWindow) return;
    const HWND bridge = m_bridgeWindow;
    m_bridgeWindow = nullptr;
    DestroyWindow(bridge);
}

LRESULT CALLBACK MainWindow::BridgeWndProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    } else if (message == EGoTouchTrayIpc::kActivateMessage && self) {
        self->ActivateWindow();
        return 0;
    } else if (message == EGoTouchTrayIpc::kNotificationMessage && self) {
        self->ShowNotification(static_cast<EGoTouchTrayIpc::Notification>(wParam));
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void MainWindow::ShowNotification(EGoTouchTrayIpc::Notification notification) {
    if (!m_notificationWindow) {
        m_notificationWindow = make<winrt::EGoTouchSettings::implementation::NotificationWindow>();
    }
    auto implementation = get_self<winrt::EGoTouchSettings::implementation::NotificationWindow>(
        m_notificationWindow);
    if (notification == EGoTouchTrayIpc::Notification::PenDeviation) {
        implementation->ShowDeviation();
        return;
    }

    PenStatus::State state{};
    if (!m_reader.IsOpen()) m_reader.Open();
    if (m_reader.IsOpen() && m_reader.Read(state)) {
        if (notification == EGoTouchTrayIpc::Notification::KeyboardConnected) {
            implementation->ShowKeyboardConnected(state);
        } else {
            implementation->ShowConnected(state);
        }
    }
}

void MainWindow::ActivateWindow() {
    const HWND hwnd = WindowHandle();
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    Activate();
    SetForegroundWindow(hwnd);
}

DWORD MainWindow::ReadUserSetting(const wchar_t* name, DWORD fallback) {
    DWORD value = fallback;
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsRegistryKey, name,
                     RRF_RT_REG_DWORD, &type, &value, &size) != ERROR_SUCCESS) {
        return fallback;
    }
    return value;
}

void MainWindow::LoadStoredSettings() {
    m_updatingControls = true;
    OneNoteCompatibilityToggle().IsOn(
        ReadUserSetting(L"OneNoteCompatibility", 1) != 0);
    AutoStartToggle().IsOn(ReadUserSetting(L"AutoStart", 1) != 0);
    DeviceNotificationsToggle().IsOn(ReadUserSetting(L"DeviceNotifications", 1) != 0);
    m_updatingControls = false;
}

bool MainWindow::SendTrayCommand(EGoTouchTrayIpc::Command command, LPARAM value) {
    const HWND tray = FindTrayWindow();
    if (!tray) return false;

    DWORD_PTR response = 0;
    const LRESULT sent = SendMessageTimeoutW(
        tray, EGoTouchTrayIpc::kCommandMessage,
        static_cast<WPARAM>(command), value,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &response);
    return sent != 0 && response != 0;
}

// 版本取自 exe 自身的资源，不在源码里写死一份：写死的那份一定会和构建号脱节，而「关于」页
// 存在的意义就是让人报问题时能说清跑的是哪一版。
//
// 取不到就把这一行整个折叠掉。留一个空 TextBlock 不是「什么都没显示」——它照样占一行高，
// 把上面的标题顶离垂直中线，看起来就是标记和标题没对齐。**当前的构建正落在这一支上**：
// 仓库里没有任何 VERSIONINFO 资源，第一步 GetFileVersionInfoSizeW 就返回 0。
void MainWindow::ShowAboutVersion() {
    AboutVersionText().Visibility(Visibility::Collapsed);

    wchar_t module[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, module, MAX_PATH) == 0) return;

    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(module, &ignored);
    if (size == 0) return;
    std::vector<std::byte> buffer(size);
    if (!GetFileVersionInfoW(module, 0, size, buffer.data())) return;

    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSize = 0;
    if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
        info == nullptr || infoSize == 0) {
        return;
    }
    const auto text = std::to_wstring(HIWORD(info->dwFileVersionMS)) + L"." +
                      std::to_wstring(LOWORD(info->dwFileVersionMS)) + L"." +
                      std::to_wstring(HIWORD(info->dwFileVersionLS));
    AboutVersionText().Text(L"版本 " + hstring{text});
    AboutVersionText().Visibility(Visibility::Visible);
}

void MainWindow::NavSelectionChanged(
        Microsoft::UI::Xaml::Controls::NavigationView const&,
        Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args) {
    const auto item = args.SelectedItem().try_as<NavigationViewItem>();
    if (!item) return;
    const auto tag = unbox_value_or<hstring>(item.Tag(), L"");

    DevicesPage().Visibility(tag == L"devices" ? Visibility::Visible : Visibility::Collapsed);
    SettingsPage().Visibility(tag == L"settings" ? Visibility::Visible : Visibility::Collapsed);
    AboutPage().Visibility(tag == L"about" ? Visibility::Visible : Visibility::Collapsed);
}

void MainWindow::DeviceCardsSizeChanged(IInspectable const&,
                                        SizeChangedEventArgs const& args) {
    // 两张设备卡片在够宽时并排。阈值按「一张卡片本身需要的宽度」定：低于它并排只会把两边
    // 都挤扁，不如继续堆叠。用代码切换而不是 AdaptiveTrigger，是因为触发条件要看的是内容区
    // 宽度，而 AdaptiveTrigger 只认窗口宽度——左侧导航栏展开与折叠会让两者差出 180。
    constexpr double kSideBySideMinWidth = 900.0;
    const bool sideBySide = args.NewSize().Width >= kSideBySideMinWidth;

    DeviceSecondColumn().Width(sideBySide ? GridLength{1.0, GridUnitType::Star}
                                          : GridLength{0.0, GridUnitType::Pixel});
    Controls::Grid::SetRow(KeyboardGroup(), sideBySide ? 0 : 1);
    Controls::Grid::SetColumn(KeyboardGroup(), sideBySide ? 1 : 0);
}

fire_and_forget MainWindow::LoadPenImage(uint32_t modelId) {
    auto lifetime = get_strong();
    const ULONGLONG now = GetTickCount64();
    const bool modelChanged = modelId != m_penImageModelId;
    if (!modelChanged && m_penImageSource) co_return;
    if (!modelChanged && now < m_penImageRetryAt) co_return;

    if (modelChanged) {
        m_penImageModelId = modelId;
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
        Media::Imaging::SoftwareBitmapSource source;
        const auto pixelSize = co_await AccessoryImages::DecodeCroppedAsync(
            std::move(asset.encoded), source);
        if (pixelSize.Width <= 0 || generation != m_penImageGeneration ||
            modelId != m_penImageModelId) co_return;
        m_penImageSource = source;
        PenImage().Source(source);
        PenImage().Visibility(Visibility::Visible);
        PenImageFallback().Visibility(Visibility::Collapsed);
    } catch (...) {
        // PC Manager 更新资源时可能短暂拿到不完整文件；保留图标并在 5 秒后重试。
    }
}

fire_and_forget MainWindow::LoadKeyboardImage() {
    auto lifetime = get_strong();
    if (m_keyboardImageSource) co_return;
    const ULONGLONG now = GetTickCount64();
    if (now < m_keyboardImageRetryAt) co_return;
    m_keyboardImageRetryAt = now + 5000;
    const uint64_t generation = ++m_keyboardImageGeneration;

    try {
        auto asset = AccessoryImages::ResolveKeyboard();
        if (!asset) co_return;
        Media::Imaging::SoftwareBitmapSource source;
        const auto pixelSize = co_await AccessoryImages::DecodeCroppedAsync(
            std::move(asset.encoded), source);
        if (pixelSize.Width <= 0 || generation != m_keyboardImageGeneration) co_return;
        m_keyboardImageSource = source;
        KeyboardImage().Source(source);
        KeyboardImage().Visibility(Visibility::Visible);
        KeyboardImageFallback().Visibility(Visibility::Collapsed);
    } catch (...) {
        // 与笔图相同：失败只退到 Fluent 图标，后续重试，不让设备页闪烁。
    }
}

void MainWindow::SetRowText(Controls::TextBlock const& value,
                            FrameworkElement const& row,
                            bool present,
                            const char* utf8) {
    const bool show = present && utf8 && *utf8;
    row.Visibility(show ? Visibility::Visible : Visibility::Collapsed);
    if (show) value.Text(to_hstring(utf8));
}

void MainWindow::RefreshDevicePage(const PenStatus::State* state) {
    if (!state) {
        PenStateText().Text(L"服务未运行");
        PenBatteryIndicator().SetState(false, 0, false);
        PenIdentityGroup().Visibility(Visibility::Collapsed);
        KeyboardStateText().Text(L"服务未运行");
        KeyboardBatteryIndicator().SetState(false, 0, false);
        KeyboardIdentityGroup().Visibility(Visibility::Collapsed);
        return;
    }

    // ── 笔 ──
    if (state->modelName[0] != '\0') {
        PenNameText().Text(to_hstring(state->modelName));
    }
    LoadPenImage(state->modelId);

    // 与托盘面板同一套判据。「未连接」只在设备明确报告不在位时才说——链路状态未知并不等于
    // 没有笔，MCU 常常先给出电量和型号、过一会儿才报链路，这中间说「未连接」是错的。
    // 充电是吸附状态下最直接、也最有用的用户语义；放在活动链路之前，避免笔吸在机身上时
    // 因 0x71 暂时为假而落入一个并非 MCU 原生状态的“待机”。其余分支只陈述已知事实。
    if (state->hasChargingState && state->charging) {
        PenStateText().Text(L"正在充电");
    } else if (state->hasStylusLink && state->stylusLinked) {
        PenStateText().Text(L"已连接");
    } else if (state->hasDeviceAttached && !state->deviceAttached) {
        PenStateText().Text(L"未连接");
    } else {
        PenStateText().Text(L"正在检测…");
    }

    if (state->hasBatteryLevel) {
        PenBatteryIndicator().SetState(
            true, state->batteryLevel, state->hasChargingState && state->charging);
    } else {
        PenBatteryIndicator().SetState(false, 0, false);
    }

    SetRowText(PenFirmwareText(), PenFirmwareRow(), state->hasPenFirmware, state->penFirmware);
    SetRowText(PenHardwareText(), PenHardwareRow(), state->hasPenHardware, state->penHardware);
    SetRowText(PenSerialText(), PenSerialRow(), state->hasPenSerial, state->penSerial);
    const bool anyPenIdentity =
        (state->hasPenFirmware && state->penFirmware[0]) ||
        (state->hasPenHardware && state->penHardware[0]) ||
        (state->hasPenSerial && state->penSerial[0]);
    PenIdentityGroup().Visibility(anyPenIdentity ? Visibility::Visible : Visibility::Collapsed);

    // ── 键盘 ──
    const bool present = state->hasKbdPresent && state->kbdPresent;
    // 名称：型号判得出就用判出来的，判不出但键盘确实在，就用不带后缀的通用名——能应答
    // MCU 键盘子系统的只可能是华为一体化键盘，第三方键盘进不到这条通路上来。
    if (state->kbdModelName[0] != '\0') {
        KeyboardNameText().Text(to_hstring(state->kbdModelName));
    } else {
        KeyboardNameText().Text(present ? L"HUAWEI 智能磁吸键盘" : L"键盘");
    }
    if (present) LoadKeyboardImage();

    if (!state->hasKbdPresent) {
        KeyboardStateText().Text(L"正在检测…");
    } else if (!present) {
        // MCU 只知道有没有链路，分不出「已分离且无线未连上」和「根本没有键盘」，原厂同样没分。
        KeyboardStateText().Text(L"未连接");
    } else if (state->hasKbdCharging && state->kbdCharging) {
        KeyboardStateText().Text(L"正在充电");
    } else if (state->hasKbdDetached && state->kbdDetached) {
        KeyboardStateText().Text(L"已分离，无线连接中");
    } else {
        KeyboardStateText().Text(L"已吸附");
    }

    if (present && state->hasKbdBattery) {
        KeyboardBatteryIndicator().SetState(
            true, state->kbdBatteryLevel,
            state->hasKbdCharging && state->kbdCharging);
    } else {
        KeyboardBatteryIndicator().SetState(false, 0, false);
    }

    const bool hasKbdFirmware = present && state->kbdFirmware[0] != '\0';
    KeyboardIdentityGroup().Visibility(hasKbdFirmware ? Visibility::Visible : Visibility::Collapsed);
    if (hasKbdFirmware) KeyboardFirmwareText().Text(to_hstring(state->kbdFirmware));
}

void MainWindow::RefreshState() {
    m_trayConnected = FindTrayWindow() != nullptr;
    if (m_exitPending && !m_trayConnected) {
        Close();
        return;
    }

    if (!m_reader.IsOpen()) m_reader.Open();
    PenStatus::State state{};
    if (m_reader.IsOpen() && m_reader.Read(state)) {
        RefreshControls(&state);
        RefreshDevicePage(&state);
        if (m_notificationWindow) {
            auto implementation =
                get_self<winrt::EGoTouchSettings::implementation::NotificationWindow>(
                    m_notificationWindow);
            implementation->UpdateState(state);
        }
    } else {
        m_reader.Close();
        RefreshControls(nullptr);
        RefreshDevicePage(nullptr);
    }
}

winrt::hstring MainWindow::ProviderErrorText(uint8_t error) {
    switch (error) {
    case 1: return L"无法停止 HuaweiTHP，已保留华为触控";
    case 2: return L"EGoTouchRev 启动失败，已恢复 HuaweiTHP";
    case 3: return L"无法禁用 HuaweiTHP，已回滚到华为触控";
    case 4: return L"EGoTouchRev 停止失败，暂未切换";
    case 5: return L"HuaweiTHP 恢复失败，已继续使用 EGoTouchRev";
    case 6: return L"两个触控提供方都无法启动";
    default: return L"触控提供方切换失败";
    }
}

void MainWindow::RefreshControls(const PenStatus::State* state) {
    m_updatingControls = true;

    bool eraserMode = PenModeSelector().SelectedIndex() == 1;
    if (state && state->hasPenButtonMode) {
        if (state->penButtonMode == static_cast<uint8_t>(PenButtonMode::WindowsInk)) {
            PenModeSelector().SelectedIndex(0);
            eraserMode = false;
        } else if (state->penButtonMode == static_cast<uint8_t>(PenButtonMode::ToggleEraser)) {
            PenModeSelector().SelectedIndex(1);
            eraserMode = true;
        }
    }

    if (!state || !state->hasTouchProvider) {
        ProviderStatusText().Text(L"正在连接 OpenEGo Hub 服务…");
    } else {
        switch (state->touchProvider) {
        case PenStatus::TouchProviderState::Huawei:
            ProviderToggle().IsOn(false);
            ProviderStatusText().Text(L"HuaweiTHP 正在提供触控");
            break;
        case PenStatus::TouchProviderState::SwitchingToEGo:
            ProviderToggle().IsOn(true);
            ProviderStatusText().Text(L"正在启用 EGoTouchRev…");
            break;
        case PenStatus::TouchProviderState::EGoTouch:
            ProviderToggle().IsOn(true);
            ProviderStatusText().Text(L"EGoTouchRev 正在提供触控");
            break;
        case PenStatus::TouchProviderState::SwitchingToHuawei:
            ProviderToggle().IsOn(false);
            ProviderStatusText().Text(L"正在恢复 HuaweiTHP…");
            break;
        case PenStatus::TouchProviderState::Error:
            ProviderStatusText().Text(L"触控提供方异常");
            break;
        default:
            ProviderStatusText().Text(L"正在确认触控提供方…");
            break;
        }
    }

    SetInteractiveEnabled(m_trayConnected && !m_exitPending);
    UpdateOneNoteRow(eraserMode);

    // 键盘在不在，用 MCU 回报的在位状态判断，不要拿「知不知道无线开关的值」代替：
    // 后者只说明收到过一次应答，键盘拔掉之后它依然为真。
    const bool kbdDetected = state && state->hasKbdPresent && state->kbdPresent;
    if (kbdDetected && state->hasKbdDetachSupport) {
        KeyboardWirelessToggle().IsOn(state->kbdDetachSupport);
        KeyboardStatusText().Text(L"键盘与主机分离后继续通过无线方式使用。");
    } else {
        KeyboardStatusText().Text(L"未检测到键盘，分离后无线连接暂不可用。");
    }
    KeyboardWirelessToggle().IsEnabled(
        m_trayConnected && !m_exitPending && kbdDetected);

    if (m_exitPending) {
        ShowStatus(InfoBarSeverity::Informational, L"正在安全退出",
                   L"正在恢复 HuaweiTHP；确认触控可用后应用会自动关闭。");
    } else if (!m_trayConnected) {
        ShowStatus(InfoBarSeverity::Warning, L"托盘未运行",
                   L"请先启动 OpenEGoHubTray.exe，再修改设置。");
    } else if (state && state->hasProviderError) {
        ShowStatus(InfoBarSeverity::Error, L"触控切换失败",
                   ProviderErrorText(state->providerError));
    } else {
        HideStatus();
    }

    m_updatingControls = false;
}

void MainWindow::UpdateOneNoteRow(bool eraserMode) {
    // 不适用的设置保持可见但禁用，并把原因写进说明文字，而不是整行隐藏：整行隐藏会让页面
    // 高度随双击行为跳动，用户也无从知道这个选项存在。
    OneNoteCompatibilityToggle().IsEnabled(
        m_trayConnected && !m_exitPending && eraserMode);
    OneNoteCompatibilityStatusText().Text(
        eraserMode
            ? L"切换工具时同步 OneNote 的绘图选项。"
            : L"仅在双击行为设为“切换书写与橡皮擦”时生效。");
}

void MainWindow::SetInteractiveEnabled(bool enabled) {
    ProviderToggle().IsEnabled(enabled);
    PenModeSelector().IsEnabled(enabled);
    AutoStartToggle().IsEnabled(enabled);
    ExitButton().IsEnabled(enabled);
}

void MainWindow::ShowStatus(
    InfoBarSeverity severity, hstring const& title, hstring const& message) {
    // InfoBar 打开期间改写 Title/Message 不会重新播报，屏幕阅读器读不到新内容；内容变化时
    // 先关再开触发一次通知。内容不变则原样保留，避免五秒一次的轮询刷新让控件反复闪烁。
    if (StatusInfoBar().IsOpen() && m_statusTitle == title && m_statusMessage == message) {
        return;
    }
    StatusInfoBar().IsOpen(false);
    StatusInfoBar().Severity(severity);
    StatusInfoBar().Title(title);
    StatusInfoBar().Message(message);
    StatusInfoBar().IsOpen(true);
    m_statusTitle = title;
    m_statusMessage = message;
}

void MainWindow::HideStatus() {
    StatusInfoBar().IsOpen(false);
    m_statusTitle = {};
    m_statusMessage = {};
}

void MainWindow::ShowError(hstring const& title, hstring const& message) {
    ShowStatus(InfoBarSeverity::Error, title, message);
}

void MainWindow::ProviderToggled(IInspectable const&, RoutedEventArgs const&) {
    if (m_updatingControls) return;
    const bool requested = ProviderToggle().IsOn();
    if (!SendTrayCommand(EGoTouchTrayIpc::Command::SetProviderEnabled, requested)) {
        m_updatingControls = true;
        ProviderToggle().IsOn(!requested);
        m_updatingControls = false;
        ShowError(L"无法修改触控提供方", L"托盘没有响应，请确认 OpenEGo Hub 正在运行。");
    }
}

void MainWindow::PenModeSelectionChanged(
    IInspectable const&, SelectionChangedEventArgs const&) {
    if (m_updatingControls) return;
    const int32_t index = PenModeSelector().SelectedIndex();
    if (index < 0) return;

    const PenButtonMode mode = index == 1
        ? PenButtonMode::ToggleEraser
        : PenButtonMode::WindowsInk;
    if (!SendTrayCommand(
            EGoTouchTrayIpc::Command::SetPenButtonMode,
            static_cast<LPARAM>(static_cast<uint8_t>(mode)))) {
        ShowError(L"无法修改双击行为", L"服务未连接，设置没有生效。");
    }
    UpdateOneNoteRow(mode == PenButtonMode::ToggleEraser);
}

void MainWindow::OneNoteCompatibilityToggled(
    IInspectable const&, RoutedEventArgs const&) {
    if (m_updatingControls) return;
    const bool requested = OneNoteCompatibilityToggle().IsOn();
    if (!SendTrayCommand(EGoTouchTrayIpc::Command::SetOneNoteCompatibility, requested)) {
        m_updatingControls = true;
        OneNoteCompatibilityToggle().IsOn(!requested);
        m_updatingControls = false;
        ShowError(L"无法保存 OneNote 设置", L"托盘没有响应，设置没有生效。");
    }
}

void MainWindow::AutoStartToggled(IInspectable const&, RoutedEventArgs const&) {
    if (m_updatingControls) return;
    const bool requested = AutoStartToggle().IsOn();
    if (!SendTrayCommand(EGoTouchTrayIpc::Command::SetAutoStart, requested)) {
        m_updatingControls = true;
        AutoStartToggle().IsOn(!requested);
        m_updatingControls = false;
        ShowError(L"无法更新登录启动设置", L"注册表写入失败，设置没有生效。");
    }
}

void MainWindow::DeviceNotificationsToggled(IInspectable const&, RoutedEventArgs const&) {
    if (m_updatingControls) return;
    const bool requested = DeviceNotificationsToggle().IsOn();
    if (!SendTrayCommand(EGoTouchTrayIpc::Command::SetDeviceNotifications, requested)) {
        m_updatingControls = true;
        DeviceNotificationsToggle().IsOn(!requested);
        m_updatingControls = false;
        ShowError(L"无法修改设备接入提示", L"托盘没有响应，设置没有生效。");
    }
}

void MainWindow::KeyboardWirelessToggled(IInspectable const&, RoutedEventArgs const&) {
    if (m_updatingControls) return;
    const bool requested = KeyboardWirelessToggle().IsOn();
    if (!SendTrayCommand(EGoTouchTrayIpc::Command::SetKeyboardWirelessOnDetach, requested)) {
        m_updatingControls = true;
        KeyboardWirelessToggle().IsOn(!requested);
        m_updatingControls = false;
        ShowError(L"无法修改键盘无线连接设置", L"托盘没有响应，设置没有生效。");
    }
}

void MainWindow::ExitClicked(IInspectable const&, RoutedEventArgs const&) {
    ConfirmExit();
}

// 退出会停掉整机触控并把设备交还华为驱动，属于影响面大且不能靠再点一次撤销的操作，按规范
// 先用 ContentDialog 确认。事件处理函数必须返回 void，协程单独拆出来。
winrt::fire_and_forget MainWindow::ConfirmExit() {
    if (m_exitDialogOpen) co_return;
    const auto lifetime = get_strong();
    m_exitDialogOpen = true;

    ContentDialog dialog;
    dialog.XamlRoot(RootLayout().XamlRoot());
    dialog.Title(box_value(hstring{L"退出 OpenEGo Hub？"}));
    dialog.Content(box_value(hstring{
        L"触控将交还 HuaweiTHP 驱动，笔侧键与键盘分离设置随之失效。重新启动 OpenEGo Hub 即可恢复。"}));
    dialog.PrimaryButtonText(L"退出并恢复");
    dialog.CloseButtonText(L"取消");
    // ESC 与手柄 B 本来就映射到 CloseButton，与这里设不设默认按钮无关；把默认按钮也给取消，
    // 是为了让回车一并落在安全的一侧。规范只说默认按钮可选，没有规定破坏性操作该给哪一个。
    dialog.DefaultButton(ContentDialogButton::Close);

    const ContentDialogResult result = co_await dialog.ShowAsync();
    m_exitDialogOpen = false;
    if (result != ContentDialogResult::Primary) co_return;

    if (!SendTrayCommand(EGoTouchTrayIpc::Command::RequestSafeExit)) {
        ShowError(L"无法安全退出", L"托盘没有响应；为避免失去触控，应用不会直接退出。");
        co_return;
    }
    m_exitPending = true;
    RefreshState();
}

} // namespace winrt::EGoTouchSettings::implementation
