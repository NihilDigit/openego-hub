#pragma once

#include "MainWindow.g.h"
#include "NotificationWindow.xaml.h"

namespace winrt::EGoTouchSettings::implementation {

struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();
    ~MainWindow();

    void PenModeSelectionChanged(
        IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OneNoteCompatibilityToggled(
        IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void AutoStartToggled(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void KeyboardWirelessToggled(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void SmartChargeToggled(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void VendorHintDismissed(
        Microsoft::UI::Xaml::Controls::InfoBar const&, IInspectable const&);
    void VendorHintActionClicked(
        IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ColorTemperatureToggled(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ColorTemperatureResetClicked(
        IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void EyeComfortToggled(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ColorTemperatureChanged(
        IInspectable const&,
        Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
    void DeviceNotificationsToggled(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget VendorServicesToggled(
        IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ColorModeChanged(
        IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void ChargeLimitChanged(
        IInspectable const&,
        Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
    void ExitClicked(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void NavSelectionChanged(
        Microsoft::UI::Xaml::Controls::NavigationView const&,
        Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
    void NavItemInvoked(
        Microsoft::UI::Xaml::Controls::NavigationView const&,
        Microsoft::UI::Xaml::Controls::NavigationViewItemInvokedEventArgs const&);
    void ThemeSelected(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void DeviceCardsSizeChanged(
        IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
    void ColorReferenceSizeChanged(
        IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);

private:
    static LRESULT CALLBACK BridgeWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    HWND WindowHandle();
    HWND FindTrayWindow() const;
    void CreateBridgeWindow();
    void DestroyBridgeWindow();
    void ActivateWindow();
    void ShowNotification(EGoTouchTrayIpc::Notification notification, LPARAM payload);
    void ApplyTheme(Microsoft::UI::Xaml::ElementTheme theme);
    void SyncFrameTheme();
    void ConfigureWindow();
    static void ApplyWindowIcon(HWND hwnd, UINT dpi);
    void LoadStoredSettings();
    void RefreshState();
    void RefreshControls(const PenStatus::State* state);
    void RefreshDevicePage(const PenStatus::State* state);
    void ShowAboutVersion();
    void FillDevicePage();
    // 低频项走后台线程：首次读要向电池驱动发一次真实的 ACPI 事务，实测约 35 ms。
    winrt::fire_and_forget FillBatteryInfoAsync();
    winrt::fire_and_forget LoadColorReferenceAsync();
    winrt::fire_and_forget SetTitleBarIconAsync();
    void RefreshBatteryLive();
    // 型号变化经去抖后才真正重载，见实现处的说明。
    void RequestPenImage(uint32_t modelId);
    winrt::fire_and_forget LoadPenImage(uint32_t modelId);
    winrt::fire_and_forget LoadKeyboardImage();
    winrt::fire_and_forget LoadMachineImage();
    static void SetRowText(
        Microsoft::UI::Xaml::Controls::TextBlock const& value,
        Microsoft::UI::Xaml::FrameworkElement const& row,
        bool present,
        const char* utf8);
    void UpdateOneNoteRow(bool eraserMode);
    void SetInteractiveEnabled(bool enabled);
    // 提交充电上限之后进入等回显的状态，见实现处的说明。
    void BeginChargeLimitEcho(uint8_t requested);
    bool SendTrayCommand(EGoTouchTrayIpc::Command command, LPARAM value = 0);
    void ShowStatus(
        Microsoft::UI::Xaml::Controls::InfoBarSeverity severity,
        winrt::hstring const& title,
        winrt::hstring const& message);
    void HideStatus();
    void ShowError(winrt::hstring const& title, winrt::hstring const& message);
    winrt::fire_and_forget ConfirmExit();
    static DWORD ReadUserSetting(const wchar_t* name, DWORD fallback);
    static void WriteUserSetting(const wchar_t* name, DWORD value);
    static winrt::hstring ProviderErrorText(uint8_t error);

    PenStatus::Reader m_reader;
    Microsoft::UI::Xaml::DispatcherTimer m_refreshTimer{nullptr};
    // 电池的高频项。1 Hz 足够：读一次只要几十微秒，但显示的数字本身以分钟为单位。
    Microsoft::UI::Xaml::DispatcherTimer m_batteryTimer{nullptr};
    // 剩余时间的平滑状态。实测同一次充电里系统负载会让功率在 8.4 W 与 12.7 W 之间跳，
    // 预测值随之在 54 分钟与 18 分钟之间摆——这是误差的大头，比电池自身的涓流减速大一个
    // 数量级，不平滑没法看。充放电方向变化时重置，那时旧样本已经没有意义。
    double m_smoothedSeconds = 0.0;
    bool m_smoothedValid = false;
    uint8_t m_smoothedKind = 0;
    // 当前的停充阈值，来自状态通道。有它才能算「充到上限」而不是「充满」——限到 80% 时
    // 后者永远不会兑现。0 表示还没拿到。
    uint8_t m_chargeStopPercent = 0;
    // 充电上限的防抖计时器：滑块每走一格都会触发一次事件，逐格提交没有意义。
    Microsoft::UI::Xaml::DispatcherTimer m_chargeLimitTimer{nullptr};
    uint8_t m_pendingChargeLimit = 100;
    // 已经发给服务、但状态通道还没回显的上限值。0 表示请求的是智能充电。
    // 命令发出到硬件生效之间有几百毫秒，这段时间里回传的仍是旧值，照单刷新就会闪一帧旧值，
    // 详见 RefreshControls 里的说明。
    uint8_t m_chargeLimitRequested = 0;
    bool m_chargeLimitAwaitingEcho = false;
    // 回显的兜底期限。命令有可能根本不生效（硬件拒绝、WMI 失败），没有期限的话滑块会一直
    // 停在用户的意图上，看起来像成功了。
    ULONGLONG m_chargeLimitEchoDeadline = 0;
    // 色温滑条的防抖。每条命令要拉起一个进程写 PCC，逐 tick 发就是几十个进程连续写，画面会抖。
    Microsoft::UI::Xaml::DispatcherTimer m_colorTemperatureTimer{nullptr};
    int m_pendingColorTemperature = 0;
    // 构造完成前不接受任何控件事件：XAML 解析本身就会触发它们，那时控件还没备齐，
    // 而滑块此刻的值是 Minimum，照单提交等于替用户改设置。
    bool m_uiReady = false;
    // 正在等待华为服务达到某个目标状态。启停要时间——每个服务最多等十秒——这段时间里
    // 开关置灰，否则用户会在中途反复点，而每一次都要等上一次做完。
    // 达标之后自动解锁，不像先前那样锁到重启。
    enum class VendorTarget : uint8_t { None, Disable, Restore };
    VendorTarget m_vendorTarget = VendorTarget::None;
    // 顶部那条「华为服务仍在运行」的提醒是否已被关掉。持久化，关一次就不再出现。
    bool m_vendorHintDismissed = false;
    HWND m_bridgeWindow = nullptr;
    bool m_updatingControls = false;
    // 服务当前生效的侧键模式，跟着状态通道走。提交前与它比对，避免把刚读回来的值再发一遍——
    // 详见 RefreshControls 里的说明。0xFF 表示还没读到过。
    uint8_t m_effectivePenMode = 0xFF;
    bool m_exitPending = false;
    bool m_trayConnected = false;
    bool m_exitDialogOpen = false;
    winrt::hstring m_statusTitle;
    winrt::hstring m_statusMessage;
    uint32_t m_penImageModelId = 0;
    Microsoft::UI::Xaml::DispatcherTimer m_penImageTimer{nullptr};
    uint32_t m_pendingPenModelId = 0;
    uint64_t m_penImageGeneration = 0;
    uint64_t m_keyboardImageGeneration = 0;
    ULONGLONG m_penImageRetryAt = 0;
    ULONGLONG m_keyboardImageRetryAt = 0;
    winrt::Microsoft::UI::Xaml::Media::ImageSource m_penImageSource{nullptr};
    winrt::Microsoft::UI::Xaml::Media::ImageSource m_keyboardImageSource{nullptr};
    winrt::Microsoft::UI::Xaml::Media::ImageSource m_machineImageSource{nullptr};
    winrt::EGoTouchSettings::NotificationWindow m_notificationWindow{nullptr};
};

} // namespace winrt::EGoTouchSettings::implementation

namespace winrt::EGoTouchSettings::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

} // namespace winrt::EGoTouchSettings::factory_implementation
