#include "pch.h"

#include "MainWindow.xaml.h"
#include "AccessoryImageLoader.h"
#include "AppIconResource.h"
#include "DeviceInfo.h"
#include "LogExport.h"
// gaokun-hal 的电池读取。电量、容量、健康度、循环次数都不需要提权，本进程直接读；
// 只有充电阈值要管理员权限，那一项由服务读了经状态通道送回来。
#include "GaokunPower.h"
// 只用到里面的命令枚举，不链接 PenControlChannel.cpp：设置窗从不直接写服务的控制通道，
// 提交一律经托盘转发，以保住那条通道的单写者约束。
#include "PenControlChannel.h"

#include <cmath>
#include <dwmapi.h>
#include <optional>

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
// 恢复尺寸时的下限，单位与上面一致（DIP）。内容区自身 MaxWidth 是 640，比这更窄就开始压
// 卡片里的两列布局了。
constexpr int32_t kMinWindowWidth = 480;
constexpr int32_t kMinWindowHeight = 400;

// DWMWA_USE_IMMERSIVE_DARK_MODE。SDK 里到 Windows 11 才有这个名字，自带一份省得跟着
// WINDOWS_SDK_VERSION 走。
constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;

// 存在注册表里的外观取值。0 是跟随系统，也是没设过时的取值。
constexpr wchar_t kThemeSettingName[] = L"AppTheme";

ElementTheme ThemeFromSetting(DWORD value) {
    switch (value) {
    case 1: return ElementTheme::Light;
    case 2: return ElementTheme::Dark;
    default: return ElementTheme::Default;
    }
}

DWORD SettingFromTheme(ElementTheme theme) {
    switch (theme) {
    case ElementTheme::Light: return 1;
    case ElementTheme::Dark: return 2;
    default: return 0;
    }
}

// 最小化、最大化、关闭这三个按钮由 AppWindowTitleBar 画，不在 XAML 的内容树里，所以窗口
// 内容换成深色时它们还按系统那一档上色。PreferredTheme 就是给这种情况准备的，交代一次
// 深浅即可，不必逐个去设按钮的前景、悬停和按下色。
void ApplyCaptionButtonTheme(Microsoft::UI::Windowing::AppWindow const& appWindow, bool dark) {
    if (const auto titleBar = appWindow.TitleBar()) {
        titleBar.PreferredTheme(
            dark ? TitleBarTheme::Dark : TitleBarTheme::Light);
    }
}

// 设备页取不到的项显示一个破折号，不留空。这些值要么固件里就有、要么永远不会有，没有
// 「还在加载」这个中间状态，而空白看起来正是还没加载完。
void SetInfoText(Controls::TextBlock const& target, std::wstring const& value) {
    target.Text(value.empty() ? hstring{L"—"} : hstring{value});
}

// 秒数写成可读时长。只到分钟：剩余时间的估计误差以分钟计，秒位是噪声。
std::wstring FormatDuration(uint32_t seconds) {
    const uint32_t minutes = (seconds + 30) / 60;
    wchar_t text[64]{};
    if (minutes >= 60) {
        std::swprintf(text, std::size(text), L"%u 小时 %u 分钟", minutes / 60, minutes % 60);
    } else {
        std::swprintf(text, std::size(text), L"%u 分钟", minutes);
    }
    return text;
}

// 毫瓦写成瓦。取绝对值，方向由调用方用文字表达。
std::wstring FormatPower(int32_t milliWatt) {
    wchar_t text[64]{};
    std::swprintf(text, std::size(text), L"%.1f W",
                  std::abs(static_cast<double>(milliWatt)) / 1000.0);
    return text;
}

// 取出嵌在 exe 里的一段资源。图片走 RCDATA 而不是 ms-appx:///——设置窗是非打包应用，
// 那个方案不可用；也不放外部文件，那会多一处部署时会漏掉的东西。
//
// 返回的是视图不是拷贝：资源在模块映射里，进程活着它就有效，也不需要释放。
std::span<const uint8_t> ResourceBytes(int resourceId) {
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!found) return {};
    const DWORD size = SizeofResource(module, found);
    const HGLOBAL loaded = LoadResource(module, found);
    if (size == 0 || !loaded) return {};
    const auto* bytes = static_cast<const uint8_t*>(LockResource(loaded));
    if (!bytes) return {};
    return {bytes, size};
}

// 把一段编码后的图片字节解成 BitmapImage。
IAsyncOperation<Media::Imaging::BitmapImage> DecodeImageAsync(std::span<const uint8_t> bytes) {
    if (bytes.empty()) co_return nullptr;

    Windows::Storage::Streams::InMemoryRandomAccessStream stream;
    Windows::Storage::Streams::DataWriter writer(stream);
    writer.WriteBytes(winrt::array_view<const uint8_t>(bytes.data(), bytes.data() + bytes.size()));
    co_await writer.StoreAsync();
    co_await writer.FlushAsync();
    writer.DetachStream();
    stream.Seek(0);

    Media::Imaging::BitmapImage image;
    co_await image.SetSourceAsync(stream);
    co_return image;
}

// 充电上限那行说明文字。RefreshControls 与 SmartChargeToggled 都要设它——后者不能等服务
// 回显，否则开关已经动了、说明还写着另一种模式。两处各写一份字面量的话，改文案漏掉一处
// 就会出现这种不一致，所以只在这里写一次。
// 72 与 70 是 hal 的 kSmartChargeDelayHours 与 kSmartChargeStopPercent。设置窗不链接
// GaokunHal.lib，只能抄一份字面量；改那两个常量时这里和 XAML 里的智能充电说明都要跟着改。
winrt::hstring ChargeLimitStatusFor(bool smart) {
    return smart ? L"智能充电固定为 70%，连续接电满 72 小时后生效。"
                 : L"长期插电时限制充电上限可以减缓电池老化。";
}

// SMBIOS 的日期是 MM/DD/YYYY。它紧挨着 BIOS 版本号显示，原样留着容易被当成另一个版本号。
std::wstring FormatBiosDate(std::wstring const& raw) {
    if (raw.size() != 10 || raw[2] != L'/' || raw[5] != L'/') return raw;
    return raw.substr(6, 4) + L"-" + raw.substr(0, 2) + L"-" + raw.substr(3, 2);
}

// 保存位置的选择器。
//
// 用 IFileSaveDialog 而不是 WinRT 的 FileSavePicker：这个 exe 不打包（WindowsPackageType
// 为 None），FileSavePicker 在这种进程里还得先 IInitializeWithWindow 关联 HWND，绕一圈拿到
// 的又是 StorageFile，而后面交给 tar 的只是一个路径。同一个 shell 对话框，这条路少一层。
std::optional<std::wstring> PickSaveLocation(HWND owner, std::wstring const& suggested) {
    winrt::com_ptr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) {
        return std::nullopt;
    }

    const COMDLG_FILTERSPEC filter[]{{L"ZIP 压缩文件", L"*.zip"}};
    dialog->SetFileTypes(static_cast<UINT>(std::size(filter)), filter);
    dialog->SetDefaultExtension(L"zip");
    dialog->SetFileName(suggested.c_str());

    // 取消时返回 HRESULT_FROM_WIN32(ERROR_CANCELLED)，与真正的失败一样走这条分支：调用方
    // 对两者的处理相同，都是什么都不做。
    if (FAILED(dialog->Show(owner))) return std::nullopt;

    winrt::com_ptr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return std::nullopt;
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw))) return std::nullopt;
    std::wstring path{raw};
    CoTaskMemFree(raw);
    return path;
}

} // namespace

MainWindow::MainWindow() {
    InitializeComponent();

    ExtendsContentIntoTitleBar(true);
    SetTitleBar(AppTitleBar());

    ConfigureWindow();
    CreateBridgeWindow();

    // 内容那一侧由 RequestedTheme 管，窗框和标题栏按钮要自己跟上。订在 ActualThemeChanged
    // 上，用户改外观和系统改主题走的是同一条路。
    RootLayout().ActualThemeChanged(
        [this](FrameworkElement const&, IInspectable const&) { SyncFrameTheme(); });
    // 元素上树之前 ActualTheme 还不是生效值，首次同步要等到这里。
    RootLayout().Loaded([this](IInspectable const&, RoutedEventArgs const&) { SyncFrameTheme(); });

    LoadStoredSettings();
    ShowAboutVersion();
    FillDevicePage();
    FillBatteryInfoAsync();
    RefreshBatteryLive();
    LoadColorReferenceAsync();
    SetTitleBarIconAsync();
    RefreshState();

    // 到这里控件才算备齐。在此之前 Slider 的 ValueChanged 已经因为 XAML 里的 Minimum
    // 触发过一次，值是 50——若那时就允许提交，打开一次设置窗就会把充电上限真的改成 50%。
    m_uiReady = true;

    m_refreshTimer = DispatcherTimer();
    // 共享内存读取只是一次 240 字节 seqlock 快照。100 ms 的安全轮询让充电边沿即使错过
    // update event 也能及时反映到设备页和正在显示的弹窗，不向 MCU 增加任何查询流量。
    m_refreshTimer.Interval(Windows::Foundation::TimeSpan{1'000'000});
    m_refreshTimer.Tick([this](IInspectable const&, IInspectable const&) {
        RefreshState();
    });
    m_refreshTimer.Start();

    // 电池单开一个 1 Hz 的节拍。不搭 m_refreshTimer 的 100 ms：读一次虽然只要几十微秒，
    // 但显示的数字以分钟为单位，而剩余时间的平滑系数是按每秒一个样本定的。
    m_batteryTimer = DispatcherTimer();
    m_batteryTimer.Interval(std::chrono::seconds(1));
    m_batteryTimer.Tick([this](IInspectable const&, IInspectable const&) {
        RefreshBatteryLive();
    });
    m_batteryTimer.Start();

    Closed([this](IInspectable const&, WindowEventArgs const&) {
        SaveWindowPlacement();
        if (m_refreshTimer) m_refreshTimer.Stop();
        if (m_batteryTimer) m_batteryTimer.Stop();
        // 防抖计时器要一并停掉：窗口关了之后它再触发，SendTrayCommand 会对着已经销毁的
        // 桥窗口发消息。
        if (m_chargeLimitTimer) m_chargeLimitTimer.Stop();
        if (m_colorTemperatureTimer) m_colorTemperatureTimer.Stop();
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

    // 记住的是 DIP 而不是物理像素：这台设备在外接屏和内屏之间缩放不同，按物理像素恢复会让
    // 窗口在另一块屏上大一圈或小一圈。下限拦住的是「上次被拖得极小」这种状态，否则下次打开
    // 会是一个看不到内容的窗口，用户还得自己拉回来。
    const int32_t savedWidth = static_cast<int32_t>(ReadUserSetting(L"WindowWidth", kWindowWidth));
    const int32_t savedHeight = static_cast<int32_t>(ReadUserSetting(L"WindowHeight", kWindowHeight));
    const int32_t targetWidth = std::max(kMinWindowWidth, savedWidth);
    const int32_t targetHeight = std::max(kMinWindowHeight, savedHeight);

    int32_t physicalWidth = static_cast<int32_t>(std::lround(targetWidth * scale));
    int32_t physicalHeight = static_cast<int32_t>(std::lround(targetHeight * scale));

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

    // 记住的位置只在它仍落在某块屏幕上时才用。显示器拔掉、分辨率改小、副屏换边之后，上次
    // 的坐标可能整个在可见区域之外，照搬会得到一个用户找不到的窗口——那时退回居中。
    bool restoredPosition = false;
    if (ReadUserSetting(L"WindowPosValid", 0) != 0) {
        const int32_t savedX = static_cast<int32_t>(ReadUserSetting(L"WindowX", 0));
        const int32_t savedY = static_cast<int32_t>(ReadUserSetting(L"WindowY", 0));
        const RectInt32 wanted{savedX, savedY, physicalWidth, physicalHeight};
        if (DisplayArea::GetFromRect(wanted, DisplayAreaFallback::None) != nullptr) {
            appWindow.Move(PointInt32{savedX, savedY});
            restoredPosition = true;
        }
    }

    if (!restoredPosition && hasArea) {
        appWindow.Move(PointInt32{
            work.X + std::max(0, (work.Width - physicalWidth) / 2),
            work.Y + std::max(0, (work.Height - physicalHeight) / 3)});
    }

    // 最大化放在尺寸和位置都定好之后：对着已经最大化的窗口 Move，动的是它的还原状态，
    // 用户按下还原键就会落到一个没人指定过的地方。
    if (ReadUserSetting(L"WindowMaximized", 0) != 0) {
        if (const auto presenter = appWindow.Presenter().try_as<OverlappedPresenter>()) {
            presenter.Maximize();
        }
    }

    ApplyWindowIcon(hwnd, dpi);
}

// 关窗时记下尺寸，下次按同样大小打开。存 DIP 而不是物理像素，理由见 ConfigureWindow。
//
// 最大化时不覆盖尺寸：那时 AppWindow.Size 是整个工作区，记下来的话用户下次还原窗口会得到一
// 个铺满屏幕的「普通」窗口，等于把他原来的尺寸弄丢了。最大化状态本身单独记一位。
void MainWindow::SaveWindowPlacement() {
    const HWND hwnd = WindowHandle();
    if (!hwnd) return;

    // 走 Win32 而不是 AppWindow.Size()：Closed 触发时 AppWindow 已经不再跟着真实窗口走，
    // 实测把窗口拖到 1200x1100 之后关掉，它报回来的仍是上一次的尺寸，存下去的数就是错的。
    // GetWindowRect 问的是窗口本身，这个时候还准。
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return;

    const bool maximized = IsZoomed(hwnd) != FALSE;
    WriteUserSetting(L"WindowMaximized", maximized ? 1 : 0);
    if (maximized) return;

    const UINT dpi = GetDpiForWindow(hwnd);
    const double scale = dpi > 0 ? dpi / 96.0 : 1.0;
    if (scale <= 0.0) return;

    const long width = rect.right - rect.left;
    const long height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;

    WriteUserSetting(L"WindowWidth", static_cast<DWORD>(std::lround(width / scale)));
    WriteUserSetting(L"WindowHeight", static_cast<DWORD>(std::lround(height / scale)));

    // 位置存物理像素，与 DisplayArea.WorkArea 同一套坐标；换算成 DIP 反而要先知道落在哪块
    // 屏上，而那正是这里要存下来才能回答的问题。左上角可以是负数（副屏在主屏左侧或上方），
    // 存进 DWORD 走的是补码，读出来再转回 int32 就是原值。
    WriteUserSetting(L"WindowX", static_cast<DWORD>(rect.left));
    WriteUserSetting(L"WindowY", static_cast<DWORD>(rect.top));
    WriteUserSetting(L"WindowPosValid", 1);
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
        self->ShowNotification(static_cast<EGoTouchTrayIpc::Notification>(wParam), lParam);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void MainWindow::ShowNotification(EGoTouchTrayIpc::Notification notification, LPARAM payload) {
    if (!m_notificationWindow) {
        m_notificationWindow = make<winrt::EGoTouchSettings::implementation::NotificationWindow>();
    }
    auto implementation = get_self<winrt::EGoTouchSettings::implementation::NotificationWindow>(
        m_notificationWindow);
    // 弹窗是另一个 Window，主题不会从主窗口继承下来。每次弹出前推一遍而不是只在创建时推：
    // 跟随系统时系统自己会变，而弹窗一天只弹几次，重设一次属性不值得为它记状态。
    implementation->ApplyTheme(RootLayout().RequestedTheme());
    if (notification == EGoTouchTrayIpc::Notification::PenDeviation) {
        implementation->ShowDeviation();
        return;
    }
    if (notification == EGoTouchTrayIpc::Notification::PenToolChanged) {
        implementation->ShowToolChanged(payload != 0);
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

// 设置窗一般只读设置，改动都提交给托盘去落盘——那些设置托盘自己也要用。这一条是例外：
// 提醒关没关只是这个窗口的显示状态，托盘不关心，绕一圈只会多一条 IPC 命令。
void MainWindow::WriteUserSetting(const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsRegistryKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    (void)RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                         sizeof(value));
    RegCloseKey(key);
}

void MainWindow::LoadStoredSettings() {
    m_updatingControls = true;
    OneNoteCompatibilityToggle().IsOn(
        ReadUserSetting(L"OneNoteCompatibility", 1) != 0);
    AutoStartToggle().IsOn(ReadUserSetting(L"AutoStart", 1) != 0);
    DeviceNotificationsToggle().IsOn(ReadUserSetting(L"DeviceNotifications", 1) != 0);
    ApplyTheme(ThemeFromSetting(ReadUserSetting(kThemeSettingName, 0)));

    // 充电阈值这里只放一个占位值，真实值随后由状态通道送来（见 RefreshControls）——读它
    // 需要管理员权限，本进程读不到，只有服务读得到。服务没连上时宁可显示上次设定的值，
    // 也不要让滑块停在 Minimum：那看起来像「当前上限是 50%」。
    //
    // 色域仍然读不回：那是把 LUT 推给面板，硬件不提供查询，所以显示的是上次由本程序设定的值。
    const DWORD chargeLimit = ReadUserSetting(L"ChargeLimit", 100);
    ChargeLimitSlider().Value(static_cast<double>(chargeLimit));
    ChargeLimitValueText().Text(winrt::to_hstring(static_cast<int>(chargeLimit)) + L"%");

    // 未设定过时选中 Native（索引 0）：那正是没套任何 LUT 的状态，与开机后的实际情况一致。
    ColorModeCombo().SelectedIndex(static_cast<int>(ReadUserSetting(L"ColorMode", 0)));

    // 色温、护眼、自然色彩的当前值。硬件那侧的真值可以用 GaokunDisplay --status 读回来，
    // 但那要拉起一个进程；这三项只会经托盘改动，托盘每次都会把结果记下来，读这份记录足够。
    // 代价是有人绕过界面直接跑命令行时会不同步。
    const DWORD kelvin = ReadUserSetting(L"ColorTemperature", 0);
    const bool temperatureOn = kelvin >= 2000 && kelvin <= 10000;
    // 关闭时滑条停在中性点，这样一开启就是 6500K，不会突然跳到某个很暖的值。
    ColorTemperatureSlider().Value(temperatureOn ? static_cast<double>(kelvin) : 6500);
    ColorTemperatureSlider().IsEnabled(temperatureOn);
    ColorTemperatureResetButton().IsEnabled(temperatureOn);
    ColorTemperatureToggle().IsOn(temperatureOn);
    ColorTemperatureValueText().Text(
        temperatureOn ? winrt::to_hstring(static_cast<int>(kelvin)) + L"K"
                      : winrt::hstring{L"关闭"});
    EyeComfortToggle().IsOn(ReadUserSetting(L"EyeComfort", 0) != 0);

    m_vendorHintDismissed = ReadUserSetting(L"VendorHintDismissed", 0) != 0;

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

// 设备页的内容在一次会话里不会变，构造时填一次就够。SMBIOS 是固件表，分辨率和刷新率
// 即便改了也要重开窗口才谈得上刷新，为此每次切页重读固件不值得。
void MainWindow::FillDevicePage() {
    const DeviceInfo::Info info = DeviceInfo::Query();

    // 标题用「厂商 + 营销名」，型号码退到副标题：GK-W7X 这种码要对着机器背面才认得出来，
    // 而 MateBook E 是固件自己写下的（SMBIOS 类型 1 的 Family），不是我们认定的。
    std::wstring title = info.manufacturer;
    if (!info.family.empty()) {
        if (!title.empty()) title += L" ";
        title += info.family;
    }
    MachineNameText().Text(title.empty() ? hstring{L"本机"} : hstring{title});

    std::wstring identity = info.productName;
    if (!info.version.empty()) {
        if (!identity.empty()) identity += L" ";
        identity += info.version;
    }
    MachineIdentityText().Text(hstring{identity});

    // 核心数单列一行，不并进处理器名。处理器名本身已经带了主频，后面再接一段就要靠分隔符
    // 断句，而每行只放一个事实不需要分隔符。
    SetInfoText(DeviceProcessorText(), info.processor);
    SetInfoText(DeviceCoresText(),
                info.processorCores > 0 ? std::to_wstring(info.processorCores) + L" 核"
                                        : std::wstring{});

    // 总量取自 GlobalMemoryStatusEx 而不是逐条累加：焊死的内存未必在 SMBIOS 里逐条列全，
    // 而规格（类型与频率）各条一致，取第一条即可。
    std::wstring memory = DeviceInfo::FormatBytes(info.memoryTotalBytes);
    if (!info.memoryModules.empty()) {
        const auto& module = info.memoryModules.front();
        if (!module.type.empty()) memory += L" " + module.type;
        if (module.speedMhz > 0) memory += L" " + std::to_wstring(module.speedMhz) + L" MHz";
    }
    SetInfoText(DeviceMemoryText(), memory);

    std::wstring display;
    if (info.displayWidth > 0 && info.displayHeight > 0) {
        display = std::to_wstring(info.displayWidth) + L" × " + std::to_wstring(info.displayHeight);
        if (info.displayRefreshHz > 0) {
            display += L" @ " + std::to_wstring(info.displayRefreshHz) + L" Hz";
        }
    }
    SetInfoText(DeviceDisplayText(), display);

    std::wstring os = info.osName;
    if (!info.osVersion.empty()) {
        if (!os.empty()) os += L" ";
        os += info.osVersion;
    }
    SetInfoText(DeviceOsText(), os);

    std::wstring bios = info.biosVersion;
    const std::wstring biosDate = FormatBiosDate(info.biosDate);
    if (!biosDate.empty()) {
        bios += bios.empty() ? biosDate : L"（" + biosDate + L"）";
    }
    SetInfoText(DeviceBiosText(), bios);

    SetInfoText(DeviceSerialText(), info.serialNumber);
    SetInfoText(DeviceSkuText(), info.skuNumber);

    LoadMachineImage();
}

// 色彩对照图。图片以 RCDATA 嵌在 exe 里，这里把那段字节喂给 BitmapImage。
//
// 走内存流而不是 ms-appx:///：设置窗是非打包应用，那个方案根本不可用；也不放外部文件，
// 那会多一处部署时会漏掉的东西。
fire_and_forget MainWindow::LoadColorReferenceAsync() {
    auto lifetime = get_strong();
    try {
        if (auto image = co_await DecodeImageAsync(ResourceBytes(IDR_COLOR_REFERENCE))) {
            ColorReferenceImage().Source(image);
        }
    } catch (...) {
        // 解码失败就空着。这张图只是参照物，没有它其余各项照常可用。
    }
}

// 标题栏的品牌标记。
//
// 只能用 ImageIconSource：PathIconSource 会让窗口一显示就崩（microsoft-ui-xaml#9784），
// 内联几何和资源查找都一样。另有一个「设了图标就无法从窗口顶边调整大小」的缺陷
// （#10374），那个在 WindowsAppSDK 1.8 已经修复。
fire_and_forget MainWindow::SetTitleBarIconAsync() {
    auto lifetime = get_strong();
    try {
        if (auto image = co_await DecodeImageAsync(ResourceBytes(IDR_BRAND_MARK))) {
            Microsoft::UI::Xaml::Controls::ImageIconSource source;
            source.ImageSource(image);
            AppTitleBar().IconSource(source);
        }
    } catch (...) {
        // 取不到就不放图标，标题文字照常显示。
    }
}

// Image 自身不裁圆角，靠外层 Border 的 Clip。矩形尺寸得跟着实际布局走，写死会在窗口
// 宽度变化后露出直角。
void MainWindow::ColorReferenceSizeChanged(IInspectable const&,
                                           SizeChangedEventArgs const& args) {
    if (auto clip = ColorReferenceClip()) {
        clip.Rect(Windows::Foundation::Rect{0.0f, 0.0f, args.NewSize().Width,
                                            args.NewSize().Height});
    }
}

// 电池的低频项：容量、健康度、循环次数、型号。放后台线程是因为首次读要向电池驱动发一次
// 真实的 ACPI 事务，实测约 35 ms；此后驱动有缓存，只要 0.3 ms。这些值一次会话内不会变，
// 读一次就够。
fire_and_forget MainWindow::FillBatteryInfoAsync() {
    auto lifetime = get_strong();
    const auto ui = winrt::apartment_context();

    co_await winrt::resume_background();
    Gaokun::Power::BatteryInfo info{};
    const auto result = Gaokun::Power::ReadBatteryInfo(info);
    const double health = Gaokun::Power::HealthPercent(info);
    co_await ui;

    if (result != Gaokun::Power::Result::Ok) {
        SetInfoText(BatteryHealthText(), {});
        SetInfoText(BatteryCyclesText(), {});
        SetInfoText(BatteryCapacityText(), {});
        SetInfoText(BatteryVoltageText(), {});
        SetInfoText(BatteryModelText(), {});
        SetInfoText(BatteryVendorText(), {});
        SetInfoText(BatterySerialText(), {});
        co_return;
    }

    wchar_t healthText[64]{};
    if (health > 0.0) {
        std::swprintf(healthText, std::size(healthText), L"%.1f%%", health);
    }
    SetInfoText(BatteryHealthText(), healthText);

    SetInfoText(BatteryCyclesText(),
                info.cycleCount > 0 ? std::to_wstring(info.cycleCount) + L" 次" : std::wstring{});

    // 用瓦时而不是毫瓦时：36.9 比 36886 好读，而这一位小数已经超出这类读数本身的精度。
    // 满充与设计并排给，衰减程度一眼可见；只给一个数字读者无从判断它是好是坏。
    std::wstring capacity;
    if (info.fullChargedCapacityMWh > 0 && info.designCapacityMWh > 0) {
        wchar_t text[64]{};
        std::swprintf(text, std::size(text), L"%.1f / %.1f Wh",
                      info.fullChargedCapacityMWh / 1000.0, info.designCapacityMWh / 1000.0);
        capacity = text;
    }
    SetInfoText(BatteryCapacityText(), capacity);

    std::wstring voltage;
    if (info.voltageMilliVolt > 0) {
        wchar_t text[32]{};
        std::swprintf(text, std::size(text), L"%.2f V", info.voltageMilliVolt / 1000.0);
        voltage = text;
    }
    SetInfoText(BatteryVoltageText(), voltage);

    // 三个字符串字段是 UTF-8。固件没填的项在 hal 那侧已经滤成空串，这里不必再判占位词。
    SetInfoText(BatteryModelText(), winrt::to_hstring(info.deviceName).c_str());
    SetInfoText(BatteryVendorText(), winrt::to_hstring(info.manufacturer).c_str());
    SetInfoText(BatterySerialText(), winrt::to_hstring(info.serialNumber).c_str());
}

// 电池的高频项：电量、充放电、剩余时间。
void MainWindow::RefreshBatteryLive() {
    Gaokun::Power::LiveState live{};
    const auto result = Gaokun::Power::ReadLiveState(live);
    if (result != Gaokun::Power::Result::Ok || !live.batteryPresent) {
        BatteryPercentText().Text(L"—");
        BatteryStateText().Text(result == Gaokun::Power::Result::NoBattery ? L"未检测到电池"
                                                                          : L"读取失败");
        BatteryLevelBar().Value(0);
        BatteryTimeText().Text(L"");
        m_smoothedValid = false;
        return;
    }

    if (live.percent != Gaokun::Power::kPercentUnknown) {
        BatteryPercentText().Text(winrt::to_hstring(static_cast<int>(live.percent)) + L"%");
        BatteryLevelBar().Value(live.percent);
    }

    // 方向看功率的符号，不看是否接着电源：充电上限生效时机器插着电却在放电，用 acOnline
    // 反推会一直显示成充电中。
    if (live.charging) {
        BatteryStateText().Text(L"正在充电 " + winrt::hstring{FormatPower(live.powerMilliWatt)});
    } else if (live.discharging) {
        BatteryStateText().Text((live.acOnline ? L"已接通电源，正在放电 " : L"正在放电 ") +
                                winrt::hstring{FormatPower(live.powerMilliWatt)});
    } else {
        BatteryStateText().Text(live.acOnline ? L"已接通电源" : L"未在充放电");
    }

    // 充电时优先算「充到上限」而不是「充满」：限到 80% 时后者永远不会兑现，用户等到的是
    // 充电在 80% 停住、时间还剩一截。拿不到阈值（服务没连上）才退回 hal 给的 ToFull。
    uint32_t seconds = live.remainingSeconds;
    uint8_t kind = static_cast<uint8_t>(live.remainingKind);
    if (live.charging && m_chargeStopPercent > 0) {
        const uint32_t toLimit = Gaokun::Power::SecondsToPercent(live, m_chargeStopPercent);
        if (toLimit != Gaokun::Power::kSecondsUnknown) seconds = toLimit;
    }

    if (kind == static_cast<uint8_t>(Gaokun::Power::TimeKind::Unknown) ||
        seconds == Gaokun::Power::kSecondsUnknown) {
        BatteryTimeText().Text(L"");
        m_smoothedValid = false;
        return;
    }

    // 指数平滑，系数 0.1，时间常数约十秒。误差的大头是系统负载让净充电功率在两档之间跳，
    // 实测预测值会随之在 54 分钟与 18 分钟之间摆——直接印瞬时值，这个数字每秒都在变。
    //
    // 不用移动平均：那要留一个窗口的历史，而窗口得开到几十秒才够稳，充放电方向一变那段
    // 历史反而是错的。指数平滑只有一个状态，方向变了重置即可。
    if (!m_smoothedValid || kind != m_smoothedKind) {
        m_smoothedSeconds = static_cast<double>(seconds);
        m_smoothedKind = kind;
        m_smoothedValid = true;
    } else {
        constexpr double kAlpha = 0.1;
        m_smoothedSeconds = kAlpha * static_cast<double>(seconds) +
                            (1.0 - kAlpha) * m_smoothedSeconds;
    }

    // 一律说「充满」。设了上限时算的是充到上限，但那个点对用户来说就是充满——充电到那里
    // 会停下，界面再去区分「上限」和「满电」，只是把实现细节推给他。
    const std::wstring duration = FormatDuration(static_cast<uint32_t>(m_smoothedSeconds));
    BatteryTimeText().Text(kind == static_cast<uint8_t>(Gaokun::Power::TimeKind::ToFull)
                               ? L"距离充满还需 " + winrt::hstring{duration}
                               : L"剩余可用 " + winrt::hstring{duration});
}

// 整机图只解一次。它不像配件图那样跟着连接状态变，所以没有代次校验，也不安排重试：
// 读不到就空着，设备页其余内容照常可用。
fire_and_forget MainWindow::LoadMachineImage() {
    auto lifetime = get_strong();
    if (m_machineImageSource) co_return;

    try {
        auto asset = AccessoryImages::ResolveMachine();
        if (!asset) co_return;

        Media::Imaging::SoftwareBitmapSource source;
        const auto pixelSize =
            co_await AccessoryImages::DecodeCroppedAsync(std::move(asset.encoded), source);
        if (pixelSize.Width <= 0) co_return;

        m_machineImageSource = source;
        MachineImage().Source(source);
    } catch (...) {
        // PC Manager 更新组件时会短暂读到不完整的插件文件。
    }
}

void MainWindow::NavSelectionChanged(
        Microsoft::UI::Xaml::Controls::NavigationView const&,
        Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args) {
    const auto item = args.SelectedItem().try_as<NavigationViewItem>();
    if (!item) return;
    const auto tag = unbox_value_or<hstring>(item.Tag(), L"");

    DevicesPage().Visibility(tag == L"devices" ? Visibility::Visible : Visibility::Collapsed);
    BatteryPage().Visibility(tag == L"battery" ? Visibility::Visible : Visibility::Collapsed);
    ScreenPage().Visibility(tag == L"screen" ? Visibility::Visible : Visibility::Collapsed);
    GeneralPage().Visibility(tag == L"general" ? Visibility::Visible : Visibility::Collapsed);
    DevicePage().Visibility(tag == L"device" ? Visibility::Visible : Visibility::Collapsed);
    AboutPage().Visibility(tag == L"about" ? Visibility::Visible : Visibility::Collapsed);
}

void MainWindow::NavItemInvoked(
        Microsoft::UI::Xaml::Controls::NavigationView const&,
        Microsoft::UI::Xaml::Controls::NavigationViewItemInvokedEventArgs const& args) {
    const auto container = args.InvokedItemContainer();
    if (!container || unbox_value_or<hstring>(container.Tag(), L"") != L"theme") return;
    Controls::Primitives::FlyoutBase::ShowAttachedFlyout(NavTheme());
}

void MainWindow::ThemeSelected(IInspectable const& sender, RoutedEventArgs const&) {
    const auto item = sender.try_as<Controls::MenuFlyoutItem>();
    if (!item) return;
    const auto tag = unbox_value_or<hstring>(item.Tag(), L"");
    const ElementTheme theme = tag == L"light"  ? ElementTheme::Light
                             : tag == L"dark"   ? ElementTheme::Dark
                                                : ElementTheme::Default;
    ApplyTheme(theme);
    WriteUserSetting(kThemeSettingName, SettingFromTheme(theme));
}

// 只表达意图。真正生效的那一档由 XAML 解析出来，随后从 ActualThemeChanged 回来。
void MainWindow::ApplyTheme(ElementTheme theme) {
    RootLayout().RequestedTheme(theme);
    ThemeSystemItem().IsChecked(theme == ElementTheme::Default);
    ThemeLightItem().IsChecked(theme == ElementTheme::Light);
    ThemeDarkItem().IsChecked(theme == ElementTheme::Dark);
    if (m_notificationWindow) {
        get_self<winrt::EGoTouchSettings::implementation::NotificationWindow>(
            m_notificationWindow)->ApplyTheme(theme);
    }
}

// 窗框和标题栏按钮不在 XAML 的管辖内，得单独交代深浅。挂在 ActualTheme 上而不是自己去读
// 注册表：跟随系统时这个事件同样会来，用户改一次系统主题就通知一次，不需要轮询，也不需要
// 区分「用户选的」和「系统给的」——ActualTheme 已经是两者合成之后的结果。
void MainWindow::SyncFrameTheme() {
    const bool dark = RootLayout().ActualTheme() == ElementTheme::Dark;
    const HWND hwnd = WindowHandle();
    const BOOL value = dark ? TRUE : FALSE;
    (void)DwmSetWindowAttribute(hwnd, kDwmwaUseImmersiveDarkMode, &value, sizeof(value));

    ApplyCaptionButtonTheme(
        Microsoft::UI::Windowing::AppWindow::GetFromWindowId(GetWindowIdFromWindow(hwnd)), dark);
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

// 型号变化要去抖。服务应用完一项设置后会重发状态，型号在那一瞬可能跳一下，而换型号的
// 处理是「清空当前图、退回图标、重新解码」——跟着每次跳变走，卡片就会在改设置时闪。
//
// 用去抖而不是识别某个特定值（比如把 0 当作未知）：跳成什么值是服务那侧的实现细节，
// 而无论跳成什么，稳定下来之前都不该动画面。
void MainWindow::RequestPenImage(uint32_t modelId) {
    if (modelId == m_penImageModelId && m_penImageSource) {
        // 又变回当前这个型号了，把待处理的重载取消掉。少了这一步，计时器到点仍会拿着那个
        // 中途值去重载，去抖就形同虚设——卡片照闪。
        if (m_penImageTimer) m_penImageTimer.Stop();
        return;
    }

    // 画面上还是占位图标时不去抖，直接加载。去抖防的是「替换已有的图」那一下闪烁，此时
    // 没有图可闪；等满这一秒正是键盘图一开机就在、笔图却慢一拍的由来——键盘图那边根本
    // 没有这道计时。中途型号解出的通常是通用笔图，那也比图标接近事实，型号稳定之后会被
    // 原地换成正确的那张。
    if (!m_penImageSource) {
        if (m_penImageTimer) m_penImageTimer.Stop();
        m_pendingPenModelId = modelId;
        LoadPenImage(modelId);
        return;
    }

    // 已经在等同一个型号就别重启计时。状态刷新是 100 ms 一次，每次刷新都重启的话计时器
    // 永远等不到触发，图一直加载不出来。只有型号真的变了才重新计时。
    if (m_penImageTimer && m_penImageTimer.IsEnabled() && modelId == m_pendingPenModelId) {
        return;
    }

    m_pendingPenModelId = modelId;
    if (!m_penImageTimer) {
        m_penImageTimer = DispatcherTimer();
        // 1 秒。切换设置引起的跳变远快于此，稳定之前不会动画面；而真的换了一支笔时，
        // 多等这一下察觉不到。
        m_penImageTimer.Interval(std::chrono::seconds(1));
        m_penImageTimer.Tick([this](IInspectable const&, IInspectable const&) {
            m_penImageTimer.Stop();
            LoadPenImage(m_pendingPenModelId);
        });
    }
    m_penImageTimer.Stop();
    m_penImageTimer.Start();
}

fire_and_forget MainWindow::LoadPenImage(uint32_t modelId) {
    auto lifetime = get_strong();
    const ULONGLONG now = GetTickCount64();
    const bool modelChanged = modelId != m_penImageModelId;
    if (!modelChanged && m_penImageSource) co_return;
    if (!modelChanged && now < m_penImageRetryAt) co_return;

    // 型号变了只是先记下来，画面不动。旧图要留到新图解码完成的那一刻再原地换掉：先清空
    // 再解码的话，中间那几十毫秒会露出占位图标，换一支笔就闪一下。
    m_penImageModelId = modelId;
    m_penImageRetryAt = now + 5000;
    const uint64_t generation = ++m_penImageGeneration;

    Media::Imaging::SoftwareBitmapSource decoded{nullptr};
    try {
        if (auto asset = AccessoryImages::ResolvePen(modelId)) {
            Media::Imaging::SoftwareBitmapSource source;
            const auto pixelSize = co_await AccessoryImages::DecodeCroppedAsync(
                std::move(asset.encoded), source);
            if (pixelSize.Width > 0) decoded = source;
        }
    } catch (...) {
        // PC Manager 更新资源时可能短暂拿到不完整文件；当前画面留着，5 秒后重试。
    }

    // 解码期间又换了型号，这一张已经过期，交给后来那次。
    if (generation != m_penImageGeneration || modelId != m_penImageModelId) co_return;

    if (decoded) {
        m_penImageSource = decoded;
        PenImage().Source(decoded);
        PenImage().Visibility(Visibility::Visible);
        PenImageFallback().Visibility(Visibility::Collapsed);
        co_return;
    }

    // 新型号一张图都解不出来。旧图不能继续挂着——那画的是另一支笔。
    if (modelChanged && m_penImageSource) {
        m_penImageSource = nullptr;
        PenImage().Source(nullptr);
        PenImage().Visibility(Visibility::Collapsed);
        PenImageFallback().Visibility(Visibility::Visible);
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
        AccessoryVendorMissingBar().IsOpen(false);
        return;
    }

    // 配件组件缺失的提示直接按当前帧开合，不套用下面那套「整帧空白时保持上次显示」：
    // hostHealth 由服务每轮无条件发布，这里的假值就是「已探测，组件在」，不是缺信息。
    AccessoryVendorMissingBar().IsOpen(
        state->hasHostHealth && (state->penVendorMissing || state->kbdVendorMissing));

    // ── 笔 ──
    if (state->modelName[0] != '\0') {
        PenNameText().Text(to_hstring(state->modelName));
    }
    RequestPenImage(state->modelId);

    // 一个状态位都没有，说明这一帧没带来任何笔的信息——服务应用完设置后重发状态时就是
    // 这样。此时保持上一次的显示，什么都不改；照着写「正在检测」会让卡片在每次改设置时
    // 闪一下，而那一刻笔的状态其实没有变过。
    //
    // 注意这和「在位但链路还没建立」不是一回事：那种情况下 has 位是有的，只是值为假，
    // 显示「正在检测」才是对的。两者的区别就在有没有任何一个 has 位。
    const bool penStateKnown =
        state->hasChargingState || state->hasStylusLink || state->hasDeviceAttached;

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
    } else if (penStateKnown) {
        PenStateText().Text(L"正在检测…");
    }

    if (state->hasBatteryLevel) {
        PenBatteryIndicator().SetState(
            true, state->batteryLevel, state->hasChargingState && state->charging);
    } else if (penStateKnown) {
        // 有别的状态却没有电量，才是真的读不到电量；整帧空白时保持原样，同上。
        PenBatteryIndicator().SetState(false, 0, false);
    }

    // 身份信息同样只在这一帧确实带了笔的信息时才动。整帧空白时把三行全隐藏，这一组就会
    // 折叠，卡片高度骤变——那才是「卡片重新加载」的观感来源，比状态文字更显眼。
    const bool anyPenIdentity =
        (state->hasPenFirmware && state->penFirmware[0]) ||
        (state->hasPenHardware && state->penHardware[0]) ||
        (state->hasPenSerial && state->penSerial[0]);
    if (anyPenIdentity || penStateKnown) {
        SetRowText(PenFirmwareText(), PenFirmwareRow(), state->hasPenFirmware, state->penFirmware);
        SetRowText(PenHardwareText(), PenHardwareRow(), state->hasPenHardware, state->penHardware);
        SetRowText(PenSerialText(), PenSerialRow(), state->hasPenSerial, state->penSerial);
        PenIdentityGroup().Visibility(anyPenIdentity ? Visibility::Visible
                                                     : Visibility::Collapsed);
    }

    // ── 键盘 ──
    const bool present = state->hasKbdPresent && state->kbdPresent;
    // 与笔同理：整帧不带任何键盘信息时保持上一次的显示，见上面那段说明。
    const bool kbdStateKnown = state->hasKbdPresent || state->hasKbdBattery ||
                               state->hasKbdCharging || state->hasKbdDetached;

    // 名称：型号判得出就用判出来的，判不出但键盘确实在，就用不带后缀的通用名——能应答
    // MCU 键盘子系统的只可能是华为一体化键盘，第三方键盘进不到这条通路上来。
    if (state->kbdModelName[0] != '\0') {
        KeyboardNameText().Text(to_hstring(state->kbdModelName));
    } else if (kbdStateKnown) {
        // 整帧空白时不要退回通用名：那一瞬 present 也是假的，名称会从具体型号跳成「键盘」。
        KeyboardNameText().Text(present ? L"HUAWEI 智能磁吸键盘" : L"键盘");
    }
    if (present) LoadKeyboardImage();

    if (!kbdStateKnown) {
        // 什么都不改。
    } else if (!state->hasKbdPresent) {
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
    } else if (kbdStateKnown) {
        KeyboardBatteryIndicator().SetState(false, 0, false);
    }

    // 身份组同样只在这一帧带了键盘信息时才动。整帧空白时折叠它，卡片高度骤变——那正是
    // 键盘卡片「重新加载」的观感来源。
    const bool hasKbdFirmware = present && state->kbdFirmware[0] != '\0';
    if (hasKbdFirmware || kbdStateKnown) {
        KeyboardIdentityGroup().Visibility(hasKbdFirmware ? Visibility::Visible
                                                          : Visibility::Collapsed);
        if (hasKbdFirmware) KeyboardFirmwareText().Text(to_hstring(state->kbdFirmware));
    }
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
    // 不说「已交还 HuaweiTHP」：走到这一步时交还本身也可能失败，那种情况下的提供方状态
    // 由上面那个 switch 单独显示，这里只交代原因。
    case 7: return L"触控宿主反复退出，已停止接管";
    default: return L"触控提供方切换失败";
    }
}

void MainWindow::RefreshControls(const PenStatus::State* state) {
    m_updatingControls = true;

    bool eraserMode = PenModeSelector().SelectedIndex() == 1;
    if (state && state->hasPenButtonMode) {
        // 记下服务当前生效的模式。SelectionChanged 不保证在设值的同一个调用栈里到达，
        // 而 m_updatingControls 是个瞬时标志——事件跨帧才来时它已经清掉了，于是我们会把
        // 刚从服务读到的值又提交回去，把用户刚改的设置覆盖掉。提交前比对这一份才挡得住。
        m_effectivePenMode = state->penButtonMode;
        if (state->penButtonMode == static_cast<uint8_t>(PenButtonMode::WindowsInk)) {
            PenModeSelector().SelectedIndex(0);
            eraserMode = false;
        } else if (state->penButtonMode == static_cast<uint8_t>(PenButtonMode::ToggleEraser)) {
            PenModeSelector().SelectedIndex(1);
            eraserMode = true;
        }
    }

    // 开关一直跟随系统的真实状态，不再因为「本次会话改过」而锁定。两个方向都幂等、也都
    // 立即生效（恢复会把服务起回来），没有需要保护的中间态；改错了当场再改回去就是。
    if (!state || !state->hasVendorServices) {
        VendorServicesStatusText().Text(L"正在连接 OpenEGo Hub 服务…");
    } else {
        // 达标判据分两个方向：停用要「全部禁用且没有进程在跑」，恢复要「没有禁用且全部
        // 起来了」。只看启动类型不够——那一步很快，慢的是进程启停。
        const bool disableDone =
            state->vendorServicesDisabled && !state->vendorServicesRunning;
        const bool restoreDone =
            !state->vendorServicesDisabled && state->vendorServicesAllRunning;
        if ((m_vendorTarget == VendorTarget::Disable && disableDone) ||
            (m_vendorTarget == VendorTarget::Restore && restoreDone)) {
            m_vendorTarget = VendorTarget::None;
        }

        const bool busy = m_vendorTarget != VendorTarget::None;
        VendorServicesToggle().IsEnabled(!busy && m_trayConnected && !m_exitPending);

        if (busy) {
            VendorServicesStatusText().Text(
                m_vendorTarget == VendorTarget::Disable ? L"正在停用…" : L"正在恢复…");
        } else {
            m_updatingControls = true;
            VendorServicesToggle().IsOn(state->vendorServicesDisabled);
            m_updatingControls = false;
            // 关掉之后会失去什么必须说，那是决策依据；接管了什么不必说，用户在这个程序里
            // 看到的每一项都是接管的结果。
            VendorServicesStatusText().Text(
                state->vendorServicesDisabled
                    ? L"已停用 PC Manager 的后台服务。"
                    : L"停用 PC Manager 的常驻后台服务，多屏协同与鼠标穿越会一并停用。");
        }

        // 设置与实际不一致时才提示，而不是笼统地说「改完要重启」——多数情况下并不需要。
        // 操作进行中不提示：那时不一致是正常的中间态。
        VendorServicesRestartBar().IsOpen(!busy && state->vendorServicesDisabled &&
                                          state->vendorServicesRunning);

        VendorHintBar().IsOpen(!busy && !state->vendorServicesDisabled &&
                               !m_vendorHintDismissed);
    }

    // 充电阈值。硬件里的真实值只有服务读得到（那条 WMI 要管理员权限），经状态通道回来。
    // 拖动过程中不要跟随：提交是防抖的，服务发布的还是旧值，跟着它走滑块会被拽回去。
    // 阈值本身要留给剩余时间那侧用：充电时算的是「充到上限」而不是「充满」。这一份不受
    // 拖动防抖影响，硬件里是什么就记什么。
    if (state && state->hasChargeLimit) {
        m_chargeStopPercent = state->chargeLimit;
    }

    // 控件在两个阶段里不跟随状态通道，否则用户看到的值会来回跳一次：
    // 拖动期间提交是防抖的，服务发布的还是旧值；提交之后到硬件生效之间还有几百毫秒
    // （托盘要拉起一个进程去写 WMI，服务再轮询回来），这段时间里回传的同样是旧值。
    // 先前只挡住了前一个阶段，于是防抖计时器一停、新值还没生效的那几轮刷新就把滑块拽回
    // 旧值，随后服务发布新值又拽回去——用户看到的正是「到位、闪一下旧值、再到位」。
    // 这里让本地意图在回显之前是唯一的显示来源：请求值与发布值一致即认为服务已确认。
    if (m_chargeLimitAwaitingEcho) {
        const bool echoed = state && state->hasChargeLimit &&
                            (state->chargeLimitManual ? state->chargeLimit : 0) ==
                                m_chargeLimitRequested;
        if (echoed || GetTickCount64() >= m_chargeLimitEchoDeadline) {
            m_chargeLimitAwaitingEcho = false;
        }
    }

    const bool chargeLimitPending =
        (m_chargeLimitTimer && m_chargeLimitTimer.IsEnabled()) || m_chargeLimitAwaitingEcho;
    if (state && state->hasChargeLimit && !chargeLimitPending) {
        const bool smart = !state->chargeLimitManual;

        m_updatingControls = true;
        SmartChargeToggle().IsOn(smart);
        ChargeLimitSlider().Value(static_cast<double>(state->chargeLimit));
        m_updatingControls = false;

        ChargeLimitValueText().Text(winrt::to_hstring(static_cast<int>(state->chargeLimit)) + L"%");
        // 智能模式下的 70% 是固定的，用户改不了，所以滑块置灰。它确实会兑现，只是要等
        // 连续接电满 72 小时，在那之前照常充满——这个条件写在说明文字里。
        ChargeLimitSlider().IsEnabled(!smart && m_trayConnected && !m_exitPending);
        ChargeLimitStatusText().Text(ChargeLimitStatusFor(smart));
    }

    if (state && state->hasTouchProvider) {
        switch (state->touchProvider) {
        case PenStatus::TouchProviderState::Error:
            // 只有异常才值得打断用户：正常接管与交还都是托盘生命周期的一部分，无需播报。
            ShowStatus(InfoBarSeverity::Error, L"触控提供方异常",
                       ProviderErrorText(state->providerError));
            break;
        default:
            break;
        }
    }

    SetInteractiveEnabled(m_trayConnected && !m_exitPending);
    UpdateOneNoteRow(eraserMode);

    // 键盘在不在，用 MCU 回报的在位状态判断，不要拿「知不知道无线开关的值」代替：
    // 后者只说明收到过一次应答，键盘拔掉之后它依然为真。
    const bool kbdDetected = state && state->hasKbdPresent && state->kbdPresent;
    if (m_kbdDetachAwaitingEcho) {
        const bool echoed = state && state->hasKbdDetachSupport &&
                            state->kbdDetachSupport == m_kbdDetachRequested;
        if (echoed || GetTickCount64() >= m_kbdDetachEchoDeadline) {
            m_kbdDetachAwaitingEcho = false;
        }
    }
    if (kbdDetected && state->hasKbdDetachSupport && !m_kbdDetachAwaitingEcho) {
        // 必须有 m_updatingControls 护栏：程序性 IsOn 也触发 Toggled，缺了它这次回写会被
        // 当成用户操作发回托盘。旧值此时还在通道里，发回去正好把用户刚点的那次抵消——
        // 表现就是开关能关不能开、点了弹回。
        m_updatingControls = true;
        KeyboardWirelessToggle().IsOn(state->kbdDetachSupport);
        m_updatingControls = false;
        KeyboardStatusText().Text(L"键盘与主机分离后继续通过无线方式使用。");
    } else if (!kbdDetected) {
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

// 兜底期限取 2 秒：命令要经托盘写共享内存、服务拉起一个进程调 WMI、再等下一轮 250 ms 的
// 轮询回来，实测在几百毫秒量级。留够余量，同时短到用户察觉不出「界面卡在旧意图上」。
void MainWindow::BeginChargeLimitEcho(uint8_t requested) {
    m_chargeLimitRequested = requested;
    m_chargeLimitAwaitingEcho = true;
    m_chargeLimitEchoDeadline = GetTickCount64() + 2000;
}

void MainWindow::SetInteractiveEnabled(bool enabled) {
    PenModeSelector().IsEnabled(enabled);
    ColorModeCombo().IsEnabled(enabled);
    SmartChargeToggle().IsEnabled(enabled);
    ColorTemperatureToggle().IsEnabled(enabled);
    // 色温关着的时候滑条与重置按钮一起置灰，与充电上限在智能模式下的处理同理。
    ColorTemperatureSlider().IsEnabled(enabled && ColorTemperatureToggle().IsOn());
    ColorTemperatureResetButton().IsEnabled(enabled && ColorTemperatureToggle().IsOn());
    EyeComfortToggle().IsEnabled(enabled);
    // 智能模式下上限由系统决定，滑块另外还要置灰。这一条不能只写在 RefreshControls 里：
    // 托盘断开又恢复时会走到这里，无条件放开就会让滑块在智能模式下重新变成可拖。
    ChargeLimitSlider().IsEnabled(enabled && !SmartChargeToggle().IsOn());
    VendorServicesToggle().IsEnabled(enabled);
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

void MainWindow::PenModeSelectionChanged(
    IInspectable const&, SelectionChangedEventArgs const&) {
    if (m_updatingControls) return;
    const int32_t index = PenModeSelector().SelectedIndex();
    if (index < 0) return;

    const PenButtonMode mode = index == 1
        ? PenButtonMode::ToggleEraser
        : PenButtonMode::WindowsInk;

    // 与服务当前生效的模式相同就不提交。这个事件也会因为我们自己按状态回填下拉项而触发，
    // 而它不保证在设值的同一个调用栈里到达——跨帧才来时 m_updatingControls 已经清掉，
    // 于是把刚读回来的旧值又发一遍，正好覆盖掉用户这一次的选择。
    if (static_cast<uint8_t>(mode) == m_effectivePenMode) {
        UpdateOneNoteRow(mode == PenButtonMode::ToggleEraser);
        return;
    }

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

winrt::fire_and_forget MainWindow::VendorServicesToggled(IInspectable const&,
                                                         RoutedEventArgs const&) {
    if (!m_uiReady || m_updatingControls) co_return;
    const bool disable = VendorServicesToggle().IsOn();

    if (!SendTrayCommand(EGoTouchTrayIpc::Command::SetVendorServicesDisabled, disable)) {
        m_updatingControls = true;
        VendorServicesToggle().IsOn(!disable);
        m_updatingControls = false;
        ShowError(L"无法修改华为后台服务", L"托盘没有响应，设置没有生效。");
        co_return;
    }

    // 记下目标状态，达标之前开关置灰。启停要时间——每个服务最多等十秒——中途放开只会让人
    // 反复点，而每一次都排在上一次后面做完。
    //
    // 不再锁到重启、也不再弹对话框：两个方向都会做到底（禁用停掉实例，恢复把它们起回来），
    // 「做完」因此是个能观察到的状态，不需要拿重启当兜底。
    m_vendorTarget = disable ? VendorTarget::Disable : VendorTarget::Restore;
    VendorServicesToggle().IsEnabled(false);
    VendorServicesStatusText().Text(disable ? L"正在停用…" : L"正在恢复…");
    co_return;
}

void MainWindow::ColorModeChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    if (!m_uiReady || m_updatingControls) return;
    auto combo = ColorModeCombo();
    if (!combo) return;
    const int index = combo.SelectedIndex();
    if (index < 0) return;

    // 顺序与 XAML 里那三项一致：Native / sRGB / Display P3。
    // Native 走 Reset：不套任何 LUT 就是面板的原生色域。
    const auto command = index == 0   ? PenControl::ColorModeCommand::Reset
                         : index == 1 ? PenControl::ColorModeCommand::Srgb
                                      : PenControl::ColorModeCommand::DisplayP3;

    if (!SendTrayCommand(EGoTouchTrayIpc::Command::SetColorMode,
                         static_cast<LPARAM>(command))) {
        ShowError(L"无法切换色域", L"托盘没有响应，设置没有生效。");
    }
}

void MainWindow::ChargeLimitChanged(
    IInspectable const&,
    Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args) {
    const auto value = static_cast<uint8_t>(args.NewValue());

    // XAML 解析到 Minimum/Maximum 时这个回调就会触发一次，而同一行里的 TextBlock 排在
    // Slider 之后，那一刻还没被创建。不判空会在窗口构造途中访问空引用，表现为设置窗
    // 一打开就消失。
    if (auto text = ChargeLimitValueText()) {
        text.Text(winrt::to_hstring(static_cast<int>(value)) + L"%");
    }
    if (!m_uiReady || m_updatingControls) return;

    // 拖动时每一格都会触发。每次提交都要写共享内存、再让服务拉起一个进程去调 WMI，
    // 逐格发送既无意义又会把 MCU 刷爆，所以等停下来再发一次。
    m_pendingChargeLimit = value;
    if (!m_chargeLimitTimer) {
        m_chargeLimitTimer = DispatcherTimer();
        m_chargeLimitTimer.Interval(std::chrono::milliseconds(600));
        m_chargeLimitTimer.Tick([this](IInspectable const&, IInspectable const&) {
            m_chargeLimitTimer.Stop();
            if (SendTrayCommand(EGoTouchTrayIpc::Command::SetChargeLimit,
                                static_cast<LPARAM>(m_pendingChargeLimit))) {
                BeginChargeLimitEcho(m_pendingChargeLimit);
            } else {
                // 发不出去就不进等回显：让状态通道立刻把滑块拉回硬件里的真实值，
                // 免得一个没生效的设定留在界面上。
                ShowError(L"无法设置充电上限", L"托盘没有响应，设置没有生效。");
            }
        });
    }
    m_chargeLimitTimer.Stop();
    m_chargeLimitTimer.Start();
}

void MainWindow::KeyboardWirelessToggled(IInspectable const&, RoutedEventArgs const&) {
    if (m_updatingControls) return;
    const bool requested = KeyboardWirelessToggle().IsOn();
    if (!SendTrayCommand(EGoTouchTrayIpc::Command::SetKeyboardWirelessOnDetach, requested)) {
        m_updatingControls = true;
        KeyboardWirelessToggle().IsOn(!requested);
        m_updatingControls = false;
        ShowError(L"无法修改键盘无线连接设置", L"托盘没有响应，设置没有生效。");
        return;
    }
    // 回显确认前不跟随状态通道，机制同充电阈值，见 RefreshControls 里的说明。
    m_kbdDetachAwaitingEcho = true;
    m_kbdDetachRequested = requested;
    m_kbdDetachEchoDeadline = GetTickCount64() + 3000;
}

// 智能充电与固定上限是同一个硬件字段（SBCM.CHMD）的两种取值，所以这个开关和下面那条滑块
// 走的是同一条提交路径：开启发哨兵 0 交还厂商，关闭则把滑块当前的值作为固定上限写下去。
void MainWindow::SmartChargeToggled(IInspectable const&, RoutedEventArgs const&) {
    if (!m_uiReady || m_updatingControls) return;

    const bool smart = SmartChargeToggle().IsOn();
    // 关闭时用滑块此刻的值。它可能正显示着智能模式的动态取值，那也是个合理的起点——
    // 用户看到的是几，接管之后就固定在几，不会跳。
    const auto percent = static_cast<uint8_t>(smart ? 0 : ChargeLimitSlider().Value());

    if (!SendTrayCommand(EGoTouchTrayIpc::Command::SetChargeLimit,
                         static_cast<LPARAM>(percent))) {
        m_updatingControls = true;
        SmartChargeToggle().IsOn(!smart);
        m_updatingControls = false;
        ShowError(L"无法切换充电模式", L"托盘没有响应，设置没有生效。");
        return;
    }
    // 开关与滑块是同一个硬件字段的两种取值，回显也得一起等：只挡滑块的话，开关会被
    // 中间那几轮还带着旧 chargeLimitManual 的状态弹回去。
    BeginChargeLimitEcho(percent);
    // 立刻反映到滑块的可用性上，不等服务把新状态发回来——那要等下一轮发布，中间这段时间
    // 开关已经动了而滑块还是旧的可用性，看起来像没生效。
    ChargeLimitSlider().IsEnabled(!smart && m_trayConnected && !m_exitPending);
    ChargeLimitStatusText().Text(ChargeLimitStatusFor(smart));
}

// 色温的开关。滑条本身没有「关闭」这个位置：最左端 2000K 是一个很暖的值，不是关掉。
void MainWindow::ColorTemperatureToggled(IInspectable const&, RoutedEventArgs const&) {
    if (!m_uiReady || m_updatingControls) return;

    const bool enabled = ColorTemperatureToggle().IsOn();
    // 开启时用滑条当前的位置，关闭时发 0。滑条在关闭状态下停在中性点，所以一开就是 6500K，
    // 不会突然跳到某个很暖的值。
    const int kelvin = enabled ? static_cast<int>(ColorTemperatureSlider().Value()) : 0;

    if (!SendTrayCommand(EGoTouchTrayIpc::Command::SetColorTemperature,
                         static_cast<LPARAM>(kelvin))) {
        m_updatingControls = true;
        ColorTemperatureToggle().IsOn(!enabled);
        m_updatingControls = false;
        ShowError(L"无法切换色温", L"托盘没有响应，设置没有生效。");
        return;
    }

    const bool usable = enabled && m_trayConnected && !m_exitPending;
    ColorTemperatureSlider().IsEnabled(usable);
    ColorTemperatureResetButton().IsEnabled(usable);
    ColorTemperatureValueText().Text(
        enabled ? winrt::to_hstring(kelvin) + L"K" : winrt::hstring{L"关闭"});
}

// 重置回 6500K，不是关闭——关闭由上面那个开关负责。设成中性点之后色温这一项仍然在生效，
// 只是不再往任何一个方向偏。
void MainWindow::ColorTemperatureResetClicked(IInspectable const&, RoutedEventArgs const&) {
    if (!m_uiReady) return;
    // 直接改滑条的值，让 ValueChanged 走既有的防抖提交路径，不另外发一次命令——两条路径
    // 各发一次的话，防抖那次会在重置之后再把旧值补发出去。
    ColorTemperatureSlider().Value(6500);
}

void MainWindow::EyeComfortToggled(IInspectable const&, RoutedEventArgs const&) {
    if (!m_uiReady || m_updatingControls) return;
    const bool enabled = EyeComfortToggle().IsOn();
    if (!SendTrayCommand(EGoTouchTrayIpc::Command::SetEyeComfort, enabled)) {
        m_updatingControls = true;
        EyeComfortToggle().IsOn(!enabled);
        m_updatingControls = false;
        ShowError(L"无法切换护眼模式", L"托盘没有响应，设置没有生效。");
    }
}

void MainWindow::ColorTemperatureChanged(
        IInspectable const&,
        Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args) {
    const auto kelvin = static_cast<int>(args.NewValue());

    // 与充电上限那条同样的理由：XAML 解析到 Minimum 时这个回调就会触发一次，而同一行里的
    // TextBlock 排在 Slider 之后，那一刻还没被创建。
    if (auto text = ColorTemperatureValueText()) {
        text.Text(winrt::to_hstring(kelvin) + L"K");
    }
    if (!m_uiReady || m_updatingControls) return;

    // 拖动时每一格都会触发，而每次提交都要拉起一个进程去写 PCC。逐格发既无意义又会让画面
    // 连闪，所以等停下来再发一次。
    m_pendingColorTemperature = kelvin;
    if (!m_colorTemperatureTimer) {
        m_colorTemperatureTimer = DispatcherTimer();
        m_colorTemperatureTimer.Interval(std::chrono::milliseconds(400));
        m_colorTemperatureTimer.Tick([this](IInspectable const&, IInspectable const&) {
            m_colorTemperatureTimer.Stop();
            if (!SendTrayCommand(EGoTouchTrayIpc::Command::SetColorTemperature,
                                 static_cast<LPARAM>(m_pendingColorTemperature))) {
                ShowError(L"无法设置色温", L"托盘没有响应，设置没有生效。");
            }
        });
    }
    m_colorTemperatureTimer.Stop();
    m_colorTemperatureTimer.Start();
}

// 关掉这条提醒就永久不再显示。用户已经表达了「我不想禁用」，下次启动再问一遍是骚扰；
// 真想禁用时服务页那个开关一直在。
void MainWindow::VendorHintDismissed(Controls::InfoBar const&, IInspectable const&) {
    (void)WriteUserSetting(L"VendorHintDismissed", 1u);
    m_vendorHintDismissed = true;
}

void MainWindow::VendorHintActionClicked(IInspectable const&, RoutedEventArgs const&) {
    // 跳到服务页，不直接替他动手：停用这些服务会连带关掉多屏协同和鼠标穿越，该由他自己
    // 看过说明再决定。
    NavGeneral().IsSelected(true);
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
    // 对话框挂在 XamlRoot 上，不在 RootLayout 的子树里，主题得单独给。
    dialog.RequestedTheme(RootLayout().RequestedTheme());
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

void MainWindow::ExportLogsClicked(IInspectable const&, RoutedEventArgs const&) {
    ExportLogsAsync();
}

winrt::fire_and_forget MainWindow::ExportLogsAsync() {
    if (m_exportInProgress) co_return;
    const auto lifetime = get_strong();

    // 先看有没有东西可导，再弹选择器。反过来的话用户挑完位置才被告知无事可做。
    if (!LogExport::HasLogs()) {
        ShowExportResult(InfoBarSeverity::Warning, L"未找到日志文件",
                         L"C:\\ProgramData\\OpenEGoHub\\logs 下没有可导出的内容。", false);
        co_return;
    }

    const auto destination = PickSaveLocation(WindowHandle(), LogExport::SuggestedFileName());
    if (!destination) co_return;

    m_exportInProgress = true;
    ExportLogsButton().IsEnabled(false);
    ShowExportResult(InfoBarSeverity::Informational, L"正在导出日志", L"正在打包，请稍候。",
                     false);

    const auto ui = winrt::apartment_context();
    co_await winrt::resume_background();
    const auto result = LogExport::WriteArchive(*destination);
    co_await ui;

    m_exportInProgress = false;
    ExportLogsButton().IsEnabled(true);

    switch (result.status) {
    case LogExport::Status::Success:
        m_lastExportPath = *destination;
        ShowExportResult(InfoBarSeverity::Success, L"日志已导出", hstring{*destination}, true);
        break;
    case LogExport::Status::NoLogs:
        ShowExportResult(InfoBarSeverity::Warning, L"未找到日志文件",
                         L"C:\\ProgramData\\OpenEGoHub\\logs 下没有可导出的内容。", false);
        break;
    default:
        ShowExportResult(InfoBarSeverity::Error, L"导出失败", hstring{result.detail}, false);
        break;
    }
}

void MainWindow::ShowExportResult(InfoBarSeverity severity, hstring const& title,
                                  hstring const& message, bool revealable) {
    ExportLogsBar().IsOpen(false);
    ExportLogsBar().Severity(severity);
    ExportLogsBar().Title(title);
    ExportLogsBar().Message(message);
    ExportLogsRevealButton().Visibility(revealable ? Visibility::Visible
                                                   : Visibility::Collapsed);
    ExportLogsBar().IsOpen(true);
}

void MainWindow::ExportLogsRevealClicked(IInspectable const&, RoutedEventArgs const&) {
    if (m_lastExportPath.empty()) return;
    // /select 让文件在资源管理器里被选中，而不是只打开所在目录——刚导出的这一个才是他要找的。
    const std::wstring arguments = L"/select,\"" + m_lastExportPath + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
}

} // namespace winrt::EGoTouchSettings::implementation
