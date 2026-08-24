#include "pch.h"

#include "App.xaml.h"
#include "MainWindow.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::EGoTouchSettings::implementation {

App::App() {
    InitializeComponent();
    m_singleton = CreateMutexW(nullptr, FALSE, EGoTouchTrayIpc::kSettingsMutexName);
    m_secondaryInstance = m_singleton && GetLastError() == ERROR_ALREADY_EXISTS;

#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
    UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& args) {
        if (IsDebuggerPresent()) {
            [[maybe_unused]] const auto message = args.Message();
            __debugbreak();
        }
    });
#endif
}

App::~App() {
    if (m_singleton) CloseHandle(m_singleton);
}

void App::OnLaunched(LaunchActivatedEventArgs const&) {
    const bool background = GetCommandLineW() &&
        std::wstring_view{GetCommandLineW()}.find(L"--background") != std::wstring_view::npos;
    if (m_secondaryInstance) {
        HWND bridge = nullptr;
        for (int attempt = 0; attempt < 10 && !bridge; ++attempt) {
            bridge = FindWindowExW(
                HWND_MESSAGE, nullptr, EGoTouchTrayIpc::kSettingsBridgeWindowClass, nullptr);
            if (!bridge) Sleep(50);
        }
        if (bridge && !background) {
            PostMessageW(bridge, EGoTouchTrayIpc::kActivateMessage, 0, 0);
        }
        Exit();
        return;
    }

    m_window = make<MainWindow>();
    if (!background) m_window.Activate();
}

} // namespace winrt::EGoTouchSettings::implementation
