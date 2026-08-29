#pragma once

#include "NotificationWindow.g.h"

namespace winrt::EGoTouchSettings::implementation {

struct NotificationWindow : NotificationWindowT<NotificationWindow> {
    NotificationWindow();
    ~NotificationWindow();

    void ShowConnected(const PenStatus::State& state);
    void ShowKeyboardConnected(const PenStatus::State& state);
    void UpdateState(const PenStatus::State& state);
    void ShowDeviation();
    void ShowToolChanged(bool eraser);
    void ApplyTheme(Microsoft::UI::Xaml::ElementTheme theme, bool dark);
    void HideNotification();

private:
    HWND WindowHandle();
    void ConfigureWindow();
    void SelectView(Microsoft::UI::Xaml::UIElement const& view);
    void PositionAndShow(int widthDip, int heightDip);
    void StartOpacityAnimation(double from, double to, int milliseconds, bool hideWhenDone);
    void RestartDwellTimer();
    void ApplyConnectedState(const PenStatus::State& state);
    void ApplyKeyboardState(const PenStatus::State& state);
    void ApplyNoUpscaleSize(
        Microsoft::UI::Xaml::Controls::Image const& image,
        Windows::Foundation::Size const& pixelSize,
        double maxWidthDip,
        double maxHeightDip);
    winrt::fire_and_forget LoadPenImage(uint32_t modelId);
    winrt::fire_and_forget LoadKeyboardImage();

    Microsoft::UI::Xaml::DispatcherTimer m_dwellTimer{nullptr};
    Microsoft::UI::Xaml::Media::Animation::Storyboard m_storyboard{nullptr};
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
