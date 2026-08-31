#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>

namespace Service {

/// 服务名称常量。必须与 scripts/OpenEGoHubSetup.wxs 的 ServiceInstall Name 一致：
/// 名字对不上时 --install 会装出第二个服务，与安装包装的那个并存，两个一起驱同一块
/// Himax；--uninstall 则卸不掉安装包装的那个，因为它按这个名字去找。
inline constexpr wchar_t kServiceName[] = L"OpenEGoHubService";

/// 最外层壳：负责 Windows SCM 注册和控制台回退。
/// 不了解任何业务模块，只持有一个 ServiceHost。
class ServiceShell {
public:
    ServiceShell();
    ~ServiceShell();

    ServiceShell(const ServiceShell&) = delete;
    ServiceShell& operator=(const ServiceShell&) = delete;
    ServiceShell(ServiceShell&&) = delete;
    ServiceShell& operator=(ServiceShell&&) = delete;

    static ServiceShell* Instance();

#if EGOTOUCH_SERVICE_ENABLE_IPC
    /// 控制台调试模式（--console 参数或无 SCM 时退回）
    void RunAsConsole();
#endif

    /// SCM 回调入口（静态桥接到 Instance()）
    static void WINAPI SvcMain(DWORD argc, LPWSTR* argv);

private:
    struct Impl;

    static DWORD WINAPI SvcCtrlHandlerEx(
        DWORD ctrl, DWORD evtType,
        LPVOID evtData, LPVOID ctx);
#if EGOTOUCH_SERVICE_ENABLE_IPC
    static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType);
#endif

    void SignalShutdownTransportAndStop() noexcept;
    void RegisterPowerNotifications();
    void UnregisterPowerNotifications();
    void ReportStatus(DWORD state, DWORD waitHint = 0,
                      DWORD win32Exit = NO_ERROR, DWORD specificExit = 0);
    void WaitForStop();
    void CloseStopEvent();

    std::unique_ptr<Impl> m_impl;
};

} // namespace Service
