#pragma once

#include "QuickFlyoutWindow.g.h"
#include "GaokunPower.h"
#include "PowerOverlay.h"

#include <functional>

namespace winrt::EGoTouchSettings::implementation {

namespace QuickFlyoutItems {
constexpr uint32_t kTouchTile                     = 1u << 0;
constexpr uint32_t kEyeComfortTile                = 1u << 1;
constexpr uint32_t kLegacyPenEraserBit            = 1u << 2;
constexpr uint32_t kKbdWirelessTile               = 1u << 3;
constexpr uint32_t kSmartChargeTile               = 1u << 4;
constexpr uint32_t kColorTempSlider               = 1u << 5;
constexpr uint32_t kHostPowerCard                 = 1u << 6;
constexpr uint32_t kAccessories                   = 1u << 7;
constexpr uint32_t kEnergySaverTile               = 1u << 8;
constexpr uint32_t kSmartChargeDefaultMigratedBit = 1u << 30;
constexpr uint32_t kConfiguredBit                 = 1u << 31;
constexpr uint32_t kDefaultMask =
    kConfiguredBit | kSmartChargeDefaultMigratedBit |
    kTouchTile | kEyeComfortTile | kSmartChargeTile |
    kKbdWirelessTile | kHostPowerCard | kAccessories;
} // namespace QuickFlyoutItems

struct QuickFlyoutState {
    bool trayConnected = false;
    bool hasState = false;
    PenStatus::State status{};
    Gaokun::Power::LiveState hostPower{};
    bool hasHostPower = false;
    PowerOverlay::State powerOverlay{};
    bool touchDesired = true;
    bool eyeComfort = false;
    bool hasChargeLimit = false;
    bool smartCharge = true;
    uint8_t chargeLimitPercent = 100;
    int colorTemperatureKelvin = 0;        // 0 = 关闭；2000..10000 = 开启
    int colorTemperatureSliderKelvin = 6500;
    bool kbdDetachPending = false;
    bool kbdDetachPendingValue = false;
    uint32_t itemsMask = QuickFlyoutItems::kDefaultMask;
};

struct QuickFlyoutCallbacks {
    std::function<QuickFlyoutState()> readState;
    std::function<bool(bool)> setTouchProvider;
    std::function<bool(bool)> setEyeComfort;
    std::function<bool(bool)> setKeyboardWireless;
    std::function<bool(bool)> setSmartCharge;
    std::function<bool(PowerOverlay::Mode)> setPowerMode;
    std::function<bool(bool)> setEnergySaver;
    std::function<bool(int)> setColorTemperature;
    std::function<void(uint32_t)> saveItemsMask;
    std::function<void()> openSettings;
    std::function<void()> onVisibilityChanged;
};

struct QuickFlyoutWindow : QuickFlyoutWindowT<QuickFlyoutWindow> {
    QuickFlyoutWindow();
    ~QuickFlyoutWindow();

    void SetCallbacks(QuickFlyoutCallbacks callbacks);
    void ApplyTheme(Microsoft::UI::Xaml::ElementTheme theme);
    void ToggleAt(bool hasAnchor, int anchorX, int anchorY);
    void ShowAt(bool hasAnchor, int anchorX, int anchorY);
    void HideFlyout();
    void UpdateState(const QuickFlyoutState& state);
    bool IsVisible() const noexcept;
    void Shutdown();

    void TouchTileClicked(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void EyeComfortTileClicked(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void KbdWirelessTileClicked(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void SmartChargeTileClicked(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void EnergySaverTileClicked(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void PowerEnergySaverClicked(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void PowerModeEfficiencyClicked(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void PowerModeBalancedClicked(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void PowerModePerformanceClicked(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ColorTempToggleClicked(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ColorTempSliderChanged(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
    void EditFlyoutClicked(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void EditItemCheckClicked(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ResetFlyoutItemsClicked(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenSettingsClicked(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void FlyoutRootKeyDown(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);

private:
    HWND WindowHandle();
    void RestoreWndProc() noexcept;
    static LRESULT CALLBACK FlyoutWndProc(
        HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void ConfigureWindow();
    void SyncFrameTheme();
    RECT ComputeAnchorBounds(bool hasAnchor, int anchorX, int anchorY, int heightDip, bool& openDownward);
    int EstimateHeightDip() const noexcept;
    int MeasureDesiredHeightDip();
    void RelayoutWindowHeight();
    void StartWholeWindowEnterAnimation(const RECT& targetBounds, bool openDownward);
    void FocusDefaultElement();
    void FlushPendingColorTemperature();
    void ApplyStateToControls(const QuickFlyoutState& state);
    void ApplyItemsLayout();
    void SyncEditCheckBoxes();
    void ApplyPowerModeSelection(PowerOverlay::Mode mode);
    void HandleEnergySaverToggle(
        winrt::Microsoft::UI::Xaml::Controls::Primitives::ToggleButton const& source);

    QuickFlyoutCallbacks m_callbacks;
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_colorTempTimer{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_animTimer{nullptr};
    winrt::event_token m_colorTempTickToken{};
    winrt::event_token m_animTickToken{};
    winrt::event_token m_activatedToken{};
    WNDPROC m_originalWndProc = nullptr;
    bool m_initialized = false;
    bool m_loaded = false;
    bool m_updatingControls = false;
    bool m_visible = false;
    bool m_editMode = false;
    bool m_pendingInitialFocus = false;
    bool m_penRowVisible = false;
    bool m_kbdRowVisible = false;
    bool m_shutdown = false;
    bool m_openDownward = false;
    bool m_animRunning = false;
    uint32_t m_itemsMask = QuickFlyoutItems::kDefaultMask;
    bool m_lastHasAnchor = false;
    int m_lastAnchorX = 0;
    int m_lastAnchorY = 0;
    int m_lastMeasuredHeightDip = 0;
    int m_pendingColorTemperature = 6500;
    int m_lastSliderKelvin = 6500;
    RECT m_animTargetBounds{};
    int m_animStartTop = 0;
    ULONGLONG m_animStartTick = 0;
    ULONGLONG m_hiddenByDeactivateTick = 0;
};

} // namespace winrt::EGoTouchSettings::implementation

namespace winrt::EGoTouchSettings::factory_implementation {

struct QuickFlyoutWindow : QuickFlyoutWindowT<QuickFlyoutWindow, implementation::QuickFlyoutWindow> {};

} // namespace winrt::EGoTouchSettings::factory_implementation
