#pragma once

#include "NotificationWindow.g.h"

#include <functional>
#include <memory>

namespace winrt::EGoTouchSettings::implementation {

struct NotificationWindow : NotificationWindowT<NotificationWindow> {
    NotificationWindow();
    ~NotificationWindow();

    winrt::fire_and_forget ShowConnected(PenStatus::State state);
    winrt::fire_and_forget ShowKeyboardConnected(PenStatus::State state);
    void UpdateState(const PenStatus::State& state);
    void ShowDeviation();
    void ShowToolChanged(bool eraser);
    void ApplyTheme(Microsoft::UI::Xaml::ElementTheme theme);
    void HideNotification();
    bool NeedsStateRefresh() const noexcept;
    void SetRefreshDemandChanged(std::function<void()> cb);
    void Shutdown();

private:
    HWND WindowHandle();
    void ConfigureWindow();
    void SelectView(Microsoft::UI::Xaml::UIElement const& view);
    void SyncFrameTheme();
    RECT TargetBounds(int widthDip, int heightDip);
    void StartWindowMotion(
        RECT fromRect, RECT toRect,
        uint8_t fromAlpha, uint8_t toAlpha,
        int durationMs, bool easeOut,
        std::function<void()> onDone = nullptr);
    void PresentView(Microsoft::UI::Xaml::UIElement const& view, int widthDip, int heightDip,
                     std::function<void()> apply);
    void RestartDwellTimer();
    void ApplyConnectedState(const PenStatus::State& state);
    void ApplyKeyboardState(const PenStatus::State& state);
    void ApplyNoUpscaleSize(
        Microsoft::UI::Xaml::Controls::Image const& image,
        Windows::Foundation::Size const& pixelSize,
        double maxWidthDip,
        double maxHeightDip);
    winrt::fire_and_forget LoadPenImage(
        uint32_t modelId,
        std::shared_ptr<winrt::handle> signalOnDone = nullptr);
    winrt::fire_and_forget LoadKeyboardImage(
        std::shared_ptr<winrt::handle> signalOnDone = nullptr);

    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_dwellTimer{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_windowAnimTimer{nullptr};
    winrt::event_token m_dwellTickToken{};
    winrt::event_token m_windowAnimTickToken{};
    uint64_t m_showGeneration = 0;
    uint64_t m_presentationGeneration = 0;
    uint64_t m_dwellGeneration = 0;
    uint64_t m_animGeneration = 0;
    bool m_windowAnimRunning = false;
    RECT m_animFromRect{};
    RECT m_animToRect{};
    uint8_t m_animFromAlpha = 0;
    uint8_t m_animToAlpha = 255;
    int m_animDurationMs = 0;
    bool m_animEaseOut = true;
    ULONGLONG m_animStartTick = 0;
    std::function<void()> m_animOnDone;
    uint8_t m_currentAlpha = 0;
    bool m_presented = false;
    bool m_connectionView = false;
    bool m_shutdown = false;
    std::function<void()> m_onRefreshDemandChanged;
    uint32_t m_penModelId = 0;
    uint64_t m_penImageGeneration = 0;
    uint64_t m_keyboardImageGeneration = 0;
    ULONGLONG m_penImageRetryAt = 0;
    ULONGLONG m_keyboardImageRetryAt = 0;
    Windows::Foundation::Size m_penImagePixelSize{};
    Windows::Foundation::Size m_keyboardImagePixelSize{};
    Microsoft::UI::Xaml::Media::ImageSource m_penImageSource{nullptr};
    Microsoft::UI::Xaml::Media::ImageSource m_keyboardImageSource{nullptr};
};

} // namespace winrt::EGoTouchSettings::implementation

namespace winrt::EGoTouchSettings::factory_implementation {

struct NotificationWindow : NotificationWindowT<NotificationWindow, implementation::NotificationWindow> {};

} // namespace winrt::EGoTouchSettings::factory_implementation
