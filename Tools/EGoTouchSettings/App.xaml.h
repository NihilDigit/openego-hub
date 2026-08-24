#pragma once

#include "App.xaml.g.h"

namespace winrt::EGoTouchSettings::implementation {

struct App : AppT<App> {
    App();
    ~App();

    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

private:
    HANDLE m_singleton = nullptr;
    bool m_secondaryInstance = false;
    winrt::Microsoft::UI::Xaml::Window m_window{nullptr};
};

} // namespace winrt::EGoTouchSettings::implementation
