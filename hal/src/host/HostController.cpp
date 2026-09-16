#include "GaokunThp.h"

#include <windows.h>

#include <string>

namespace Gaokun::Thp {

namespace {

// 宿主服务的名字。固定值，不按构建区分：同一台设备同一时刻只能有一个触控提供方，Debug 与
// Release 的服务本来就互斥，所以两者共用这一条注册，每次 Start 时把 ImagePath 对齐到当前
// 这份宿主即可。
//
// 这个名字与 THP_Service.dll 内部硬编码的 HuaweiThpService 无关，也不必与之相同：
// RegisterServiceCtrlHandlerExW 对 SERVICE_WIN32_OWN_PROCESS 忽略服务名。占用原厂的名字
// 反而会挡住「随时让路给原厂」这条退路。
constexpr const wchar_t *kHostServiceName = L"OpenEGoHubThpHost";
constexpr const wchar_t *kHostServiceDisplayName = L"OpenEGo Hub 触控宿主";

constexpr DWORD kPollIntervalMs = 100;

[[nodiscard]] std::wstring BuildImagePath(const std::wstring &exePath,
                                          const std::wstring &extraArgs) {
    std::wstring path = L"\"" + exePath + L"\"";
    if (!extraArgs.empty()) path += L" " + extraArgs;
    return path;
}

class ScopedHandle {
public:
    explicit ScopedHandle(SC_HANDLE h = nullptr) noexcept : m_handle(h) {}
    ~ScopedHandle() noexcept {
        if (m_handle) CloseServiceHandle(m_handle);
    }
    ScopedHandle(const ScopedHandle &) = delete;
    ScopedHandle &operator=(const ScopedHandle &) = delete;

    [[nodiscard]] SC_HANDLE get() const noexcept { return m_handle; }
    explicit operator bool() const noexcept { return m_handle != nullptr; }

    void reset(SC_HANDLE h) noexcept {
        if (m_handle) CloseServiceHandle(m_handle);
        m_handle = h;
    }

private:
    SC_HANDLE m_handle;
};

[[nodiscard]] bool QueryStatus(SC_HANDLE service, SERVICE_STATUS_PROCESS &out) noexcept {
    DWORD needed = 0;
    return QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                reinterpret_cast<BYTE *>(&out), sizeof(out), &needed) != FALSE;
}

// 轮询等待服务离开某个过渡状态。SCM 没有可等待的句柄，只能问。
[[nodiscard]] DWORD WaitForState(SC_HANDLE service, DWORD pending, DWORD timeoutMs) noexcept {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    SERVICE_STATUS_PROCESS status{};
    for (;;) {
        if (!QueryStatus(service, status)) return 0;
        if (status.dwCurrentState != pending) return status.dwCurrentState;
        if (GetTickCount64() >= deadline) return status.dwCurrentState;
        Sleep(kPollIntervalMs);
    }
}

} // namespace

HostController::~HostController() noexcept {
    if (IsRunning()) (void)Stop();
}

StartResult HostController::Start(const std::wstring &hostExePath,
                                  const std::wstring &extraArgs) noexcept {
    if (IsRunning()) return StartResult::AlreadyRunning;

    if (GetFileAttributesW(hostExePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return StartResult::HostNotFound;
    }

    ScopedHandle manager{
        OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE)};
    if (!manager) return StartResult::LaunchFailed;

    const std::wstring imagePath = BuildImagePath(hostExePath, extraArgs);

    ScopedHandle service{OpenServiceW(manager.get(), kHostServiceName,
                                      SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS |
                                          SERVICE_CHANGE_CONFIG)};
    if (!service) {
        // SERVICE_ERROR_IGNORE：宿主起不来不该拦住开机。没有配恢复动作，交还原厂由宿主
        // 自己在退出路径上完成，不依赖 SCM 的重启配额。
        service.reset(CreateServiceW(manager.get(), kHostServiceName, kHostServiceDisplayName,
                                     SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS |
                                         SERVICE_CHANGE_CONFIG,
                                     SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
                                     SERVICE_ERROR_IGNORE, imagePath.c_str(), nullptr, nullptr,
                                     nullptr, nullptr, nullptr));
        if (!service) return StartResult::LaunchFailed;
    } else {
        // 换构建目录或改日志级别都会让 ImagePath 过时，每次对齐而不是只在创建时写一次。
        (void)ChangeServiceConfigW(service.get(), SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
                                   SERVICE_NO_CHANGE, imagePath.c_str(), nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr);
    }

    // 传自己的 pid：控制方崩溃时宿主据此自行走完 ThpFuncStop 并把原厂服务请回来。服务之间
    // 没有父子关系，SCM 不会因为控制方消失而停掉宿主，这条线是唯一的兜底。
    const std::wstring self = std::to_wstring(GetCurrentProcessId());
    const wchar_t *args[] = {L"--parent", self.c_str()};

    if (!StartServiceW(service.get(), 2, args)) {
        if (GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) return StartResult::LaunchFailed;
        return StartResult::AlreadyRunning;
    }

    const DWORD state = WaitForState(service.get(), SERVICE_START_PENDING, 15000);
    if (state != SERVICE_RUNNING) {
        // 起不来的常见原因是设备仍被原厂服务持有，或原厂目录定位失败。两种情况下宿主都会
        // 在 StartThp 里失败并报告 STOPPED，与「已接管」区分开，调用方才不会误报成功。
        SERVICE_STATUS_PROCESS status{};
        if (QueryStatus(service.get(), status)) {
            m_lastExitCode = static_cast<int>(status.dwWin32ExitCode);
        }
        return StartResult::ExitedImmediately;
    }

    m_lastExitCode = -1;
    return StartResult::Started;
}

bool HostController::Stop(std::chrono::milliseconds timeout) noexcept {
    ScopedHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager) return true;

    ScopedHandle service{
        OpenServiceW(manager.get(), kHostServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS)};
    if (!service) return true; // 没注册过，没有东西要停

    SERVICE_STATUS_PROCESS status{};
    if (!QueryStatus(service.get(), status)) return true;
    if (status.dwCurrentState == SERVICE_STOPPED) {
        m_lastExitCode = static_cast<int>(status.dwWin32ExitCode);
        return true;
    }

    const DWORD pid = status.dwProcessId;

    SERVICE_STATUS legacy{};
    if (!ControlService(service.get(), SERVICE_CONTROL_STOP, &legacy) &&
        GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        return false;
    }

    const DWORD waited =
        WaitForState(service.get(), SERVICE_STOP_PENDING, static_cast<DWORD>(timeout.count()));
    if (waited == SERVICE_STOPPED) {
        if (QueryStatus(service.get(), status)) {
            m_lastExitCode = static_cast<int>(status.dwWin32ExitCode);
        }
        return true;
    }

    // 超时说明宿主没走完收尾。强杀之后设备可能停在中间状态，交还原厂服务时它会自己复位
    // AFE，但调用方应当把这次超时记下来：反复出现意味着收尾路径有问题。
    if (pid != 0) {
        HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (process) {
            (void)TerminateProcess(process, 1);
            (void)WaitForSingleObject(process, 2000);
            CloseHandle(process);
        }
    }
    return false;
}

bool HostController::IsRunning() const noexcept {
    ScopedHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager) return false;

    ScopedHandle service{OpenServiceW(manager.get(), kHostServiceName, SERVICE_QUERY_STATUS)};
    if (!service) return false;

    SERVICE_STATUS_PROCESS status{};
    if (!QueryStatus(service.get(), status)) return false;
    return status.dwCurrentState != SERVICE_STOPPED;
}

int HostController::ExitCode() const noexcept { return m_lastExitCode; }

} // namespace Gaokun::Thp
