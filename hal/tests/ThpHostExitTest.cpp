// Elevated, isolated integration test. The runner must stop the real providers
// and restore OpenEGoHubThpHost's ImagePath afterwards.
#include "GaokunThp.h"
#include <windows.h>
#include <cstdio>
#include <cwchar>

namespace {
HANDLE stopped;
SERVICE_STATUS_HANDLE statusHandle;
void Status(DWORD state) {
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;
    SetServiceStatus(statusHandle, &status);
}
DWORD WINAPI Control(DWORD control, DWORD, void*, void*) {
    if (control == SERVICE_CONTROL_STOP) SetEvent(stopped);
    return NO_ERROR;
}
void WINAPI Service(DWORD, wchar_t**) {
    statusHandle = RegisterServiceCtrlHandlerExW(L"OpenEGoHubThpHost", Control, nullptr);
    Status(SERVICE_RUNNING);
    WaitForSingleObject(stopped, INFINITE);
    Status(SERVICE_STOPPED);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && !wcscmp(argv[1], L"--service")) {
        stopped = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t*>(L"OpenEGoHubThpHost"), Service}, {}};
        if (!StartServiceCtrlDispatcherW(table)) return 99;
        Sleep(1200);
        CloseHandle(stopped);
        return argc >= 3 ? _wtoi(argv[2]) : 0;
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    for (int code : {0, 7}) {
        Gaokun::Thp::HostController host;
        if (host.Start(path, L"--service " + std::to_wstring(code)) !=
            Gaokun::Thp::StartResult::Started) return 1;
        const auto begin = GetTickCount64();
        const bool clean = host.Stop();
        const auto elapsed = GetTickCount64() - begin;
        printf("exit=%d clean=%d recorded=%d elapsed=%llu\n", code, clean, host.ExitCode(), elapsed);
        if (elapsed < 1200 || !clean || host.ExitCode() != code) return 2;
    }
    // A second Stop must still wait if SCM already reports STOPPED.
    Gaokun::Thp::HostController host;
    if (host.Start(path, L"--service 0") != Gaokun::Thp::StartResult::Started) return 3;
    auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    auto service = OpenServiceW(manager, L"OpenEGoHubThpHost", SERVICE_STOP | SERVICE_QUERY_STATUS);
    SERVICE_STATUS status{};
    if (!service || !ControlService(service, SERVICE_CONTROL_STOP, &status)) return 4;
    const auto deadline = GetTickCount64() + 5000;
    do {
        if (!QueryServiceStatus(service, &status)) return 5;
        if (status.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(10);
    } while (GetTickCount64() < deadline);
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (status.dwCurrentState != SERVICE_STOPPED) return 6;
    if (host.Start(path, L"--service 0") != Gaokun::Thp::StartResult::LaunchFailed) return 7;
    const auto begin = GetTickCount64();
    if (!host.Stop()) return 8;
    const auto elapsed = GetTickCount64() - begin;
    printf("already-stopped wait=%llu\n", elapsed);
    if (elapsed < 900) return 9;
    if (host.Start(path, L"--service 0") != Gaokun::Thp::StartResult::Started) return 10;
    const bool clean = host.Stop(std::chrono::milliseconds(200));
    printf("timeout clean=%d recorded=%d\n", clean, host.ExitCode());
    return !clean && host.ExitCode() == ERROR_TIMEOUT ? 0 : 11;
}
