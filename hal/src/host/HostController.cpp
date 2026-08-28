#include "GaokunThp.h"

#include <windows.h>

#include <string>

namespace Gaokun::Thp {

namespace {

// 停止事件按调用方的进程 id 命名，允许同一台机器上并存多个控制方（生产服务与调试服务）
// 而不会互相把对方的宿主停掉。Global\ 需要 SeCreateGlobalPrivilege，服务账户具备；
// 退化到会话本地名是为了让非服务上下文（手工试验、测试）也能跑通。
[[nodiscard]] std::wstring StopEventName(DWORD pid) {
    return L"Global\\GaokunThpHostStop." + std::to_wstring(pid);
}

[[nodiscard]] HANDLE CreateStopEvent(const std::wstring &name) noexcept {
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
    if (handle) return handle;

    const std::wstring local = name.substr(name.find(L'\\') + 1);
    return CreateEventW(nullptr, TRUE, FALSE, local.c_str());
}

} // namespace

HostController::~HostController() noexcept {
    if (IsRunning()) (void)Stop();
    CloseHandles();
}

void HostController::CloseHandles() noexcept {
    // 句柄关掉之后退出码就取不到了，所以先留一份。
    if (m_process) {
        DWORD code = 0;
        if (GetExitCodeProcess(static_cast<HANDLE>(m_process), &code) && code != STILL_ACTIVE) {
            m_lastExitCode = static_cast<int>(code);
        }
    }
    if (m_process) {
        CloseHandle(static_cast<HANDLE>(m_process));
        m_process = nullptr;
    }
    if (m_stopEvent) {
        CloseHandle(static_cast<HANDLE>(m_stopEvent));
        m_stopEvent = nullptr;
    }
    m_pid = 0;
}

StartResult HostController::Start(const std::wstring &hostExePath) noexcept {
    if (IsRunning()) return StartResult::AlreadyRunning;
    CloseHandles();

    if (GetFileAttributesW(hostExePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return StartResult::HostNotFound;
    }

    const DWORD self = GetCurrentProcessId();
    const std::wstring eventName = StopEventName(self);

    HANDLE stopEvent = CreateStopEvent(eventName);
    if (!stopEvent) return StartResult::LaunchFailed;

    // 宿主同时等停止事件和本进程句柄。传自己的 pid 是关键：控制方崩溃时宿主据此自行
    // 走完 ThpFuncStop，而不是留下一个占着设备的孤儿等人收拾。
    std::wstring commandLine = L"\"" + hostExePath + L"\" --hosted --parent " +
                               std::to_wstring(self) + L" --stop-event " + eventName;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION info{};

    const BOOL ok = CreateProcessW(hostExePath.c_str(), commandLine.data(), nullptr, nullptr,
                                   FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
    if (!ok) {
        CloseHandle(stopEvent);
        return StartResult::LaunchFailed;
    }

    CloseHandle(info.hThread);
    m_process = info.hProcess;
    m_stopEvent = stopEvent;
    m_pid = info.dwProcessId;

    // 起不来的常见原因是设备仍被原厂服务持有，或原厂目录定位失败。那两种情况下宿主会
    // 很快退出，早一点把它与「已接管」区分开，调用方才不会误报成功。
    if (WaitForSingleObject(static_cast<HANDLE>(m_process), 1500) == WAIT_OBJECT_0) {
        return StartResult::ExitedImmediately;
    }

    return StartResult::Started;
}

bool HostController::Stop(std::chrono::milliseconds timeout) noexcept {
    if (!m_process) return true;

    if (m_stopEvent) (void)SetEvent(static_cast<HANDLE>(m_stopEvent));

    const DWORD wait = static_cast<DWORD>(timeout.count());
    bool clean = WaitForSingleObject(static_cast<HANDLE>(m_process), wait) == WAIT_OBJECT_0;

    if (!clean) {
        // 超时说明宿主没走完收尾。强杀之后设备可能停在中间状态，交还原厂服务时它会自己
        // 复位 AFE，但调用方应当把这次超时记下来：反复出现意味着收尾路径有问题。
        (void)TerminateProcess(static_cast<HANDLE>(m_process), 1);
        (void)WaitForSingleObject(static_cast<HANDLE>(m_process), 2000);
    }

    CloseHandles();
    return clean;
}

bool HostController::IsRunning() const noexcept {
    if (!m_process) return false;
    return WaitForSingleObject(static_cast<HANDLE>(m_process), 0) == WAIT_TIMEOUT;
}

int HostController::ExitCode() const noexcept {
    if (!m_process) return m_lastExitCode;
    DWORD code = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(m_process), &code)) return m_lastExitCode;
    return code == STILL_ACTIVE ? -1 : static_cast<int>(code);
}

} // namespace Gaokun::Thp
