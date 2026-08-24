#pragma once

#include "MainWindow.g.h"
#include "NotificationWindow.xaml.h"

namespace winrt::EGoTouchSettings::implementation {

struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();
    ~MainWindow();

    void ProviderToggled(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void PenModeSelectionChanged(
        IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OneNoteCompatibilityToggled(
        IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void AutoStartToggled(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void KeyboardWirelessToggled(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void DeviceNotificationsToggled(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ExitClicked(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void NavSelectionChanged(
        Microsoft::UI::Xaml::Controls::NavigationView const&,
        Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
    void DeviceCardsSizeChanged(
        IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);

private:
    static LRESULT CALLBACK BridgeWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    HWND WindowHandle();
    HWND FindTrayWindow() const;
    void CreateBridgeWindow();
    void DestroyBridgeWindow();
    void ActivateWindow();
    void ShowNotification(EGoTouchTrayIpc::Notification notification);
    void ConfigureWindow();
    static void ApplyWindowIcon(HWND hwnd, UINT dpi);
    void LoadStoredSettings();
    void RefreshState();
    void RefreshControls(const PenStatus::State* state);
    void RefreshDevicePage(const PenStatus::State* state);
    void ShowAboutVersion();
    winrt::fire_and_forget LoadPenImage(uint32_t modelId);
    winrt::fire_and_forget LoadKeyboardImage();
    static void SetRowText(
        Microsoft::UI::Xaml::Controls::TextBlock const& value,
        Microsoft::UI::Xaml::FrameworkElement const& row,
        bool present,
        const char* utf8);
    void UpdateOneNoteRow(bool eraserMode);
    void SetInteractiveEnabled(bool enabled);
    bool SendTrayCommand(EGoTouchTrayIpc::Command command, LPARAM value = 0);
    void ShowStatus(
        Microsoft::UI::Xaml::Controls::InfoBarSeverity severity,
        winrt::hstring const& title,
        winrt::hstring const& message);
    void HideStatus();
    void ShowError(winrt::hstring const& title, winrt::hstring const& message);
    winrt::fire_and_forget ConfirmExit();
    static DWORD ReadUserSetting(const wchar_t* name, DWORD fallback);
    static winrt::hstring ProviderErrorText(uint8_t error);

    PenStatus::Reader m_reader;
    Microsoft::UI::Xaml::DispatcherTimer m_refreshTimer{nullptr};
    HWND m_bridgeWindow = nullptr;
    bool m_updatingControls = false;
    bool m_exitPending = false;
    bool m_trayConnected = false;
    bool m_exitDialogOpen = false;
    winrt::hstring m_statusTitle;
    winrt::hstring m_statusMessage;
    uint32_t m_penImageModelId = 0;
    uint64_t m_penImageGeneration = 0;
    uint64_t m_keyboardImageGeneration = 0;
    ULONGLONG m_penImageRetryAt = 0;
    ULONGLONG m_keyboardImageRetryAt = 0;
    winrt::Microsoft::UI::Xaml::Media::ImageSource m_penImageSource{nullptr};
    winrt::Microsoft::UI::Xaml::Media::ImageSource m_keyboardImageSource{nullptr};
    winrt::EGoTouchSettings::NotificationWindow m_notificationWindow{nullptr};
};

} // namespace winrt::EGoTouchSettings::implementation

namespace winrt::EGoTouchSettings::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

} // namespace winrt::EGoTouchSettings::factory_implementation
