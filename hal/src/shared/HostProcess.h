#pragma once

#include <windows.h>

#include <chrono>
#include <string>

// 宿主进程的生命周期管理，触控、笔、键盘三处共用。
//
// 所有宿主的契约相同：以 --hosted --parent <pid> --stop-event <name> 启动，同时等待停止
// 事件与调用方的进程句柄。等句柄这一条不能省——调用方崩溃时宿主据此自行走完厂商 DLL 的
// 收尾再退出。用 Job Object 的 KILL_ON_JOB_CLOSE 兜底是不够的：那是直接终止，DLL 得不到
// 复位设备的机会，下一个接手者要多做一次恢复。
namespace Gaokun::Host {

enum class LaunchResult {
    Started = 0,
    AlreadyRunning,
    NotFound,
    LaunchFailed,
    ExitedImmediately,
};

class Process {
public:
    Process() noexcept = default;
    ~Process() noexcept {
        if (IsRunning()) (void)Stop(std::chrono::seconds(10));
        CloseHandles();
    }

    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;

    // eventPrefix 用来区分不同宿主的停止事件，例如 L"GaokunPenHostStop"。
    // extraArgs 原样拼在固定参数之后，调用方负责好引号，目前只用来传 --log-level。
    [[nodiscard]] LaunchResult Start(const std::wstring &exePath, const wchar_t *eventPrefix,
                                     const std::wstring &extraArgs = {}) noexcept {
        if (IsRunning()) return LaunchResult::AlreadyRunning;
        CloseHandles();

        if (GetFileAttributesW(exePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return LaunchResult::NotFound;
        }

        const DWORD self = GetCurrentProcessId();
        // 事件名带上调用方的 pid，好让同一台机器上并存的多个调用方（生产服务与调试服务）
        // 不会互相把对方的宿主停掉。
        const std::wstring eventName =
            L"Global\\" + std::wstring(eventPrefix) + L"." + std::to_wstring(self);

        HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        if (!stopEvent) {
            const std::wstring local = eventName.substr(eventName.find(L'\\') + 1);
            stopEvent = CreateEventW(nullptr, TRUE, FALSE, local.c_str());
        }
        if (!stopEvent) return LaunchResult::LaunchFailed;

        std::wstring commandLine = L"\"" + exePath + L"\" --hosted --parent " +
                                   std::to_wstring(self) + L" --stop-event " + eventName;
        if (!extraArgs.empty()) commandLine += L" " + extraArgs;

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        const BOOL ok = CreateProcessW(exePath.c_str(), commandLine.data(), nullptr, nullptr,
                                       FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
        if (!ok) {
            CloseHandle(stopEvent);
            return LaunchResult::LaunchFailed;
        }

        CloseHandle(info.hThread);
        m_process = info.hProcess;
        m_stopEvent = stopEvent;

        // 起不来的常见原因是设备被别的实现占着，或厂商目录定位失败。那两种情况下宿主会
        // 很快退出，早点把它与「已接管」区分开，调用方才不会误报成功。
        if (WaitForSingleObject(m_process, 1500) == WAIT_OBJECT_0) {
            return LaunchResult::ExitedImmediately;
        }
        return LaunchResult::Started;
    }

    [[nodiscard]] bool Stop(std::chrono::milliseconds timeout) noexcept {
        if (!m_process) return true;
        if (m_stopEvent) (void)SetEvent(m_stopEvent);

        const bool clean =
            WaitForSingleObject(m_process, static_cast<DWORD>(timeout.count())) == WAIT_OBJECT_0;
        if (!clean) {
            (void)TerminateProcess(m_process, 1);
            (void)WaitForSingleObject(m_process, 2000);
        }
        CloseHandles();
        return clean;
    }

    [[nodiscard]] bool IsRunning() const noexcept {
        return m_process && WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
    }

    [[nodiscard]] int ExitCode() const noexcept {
        if (!m_process) return m_lastExitCode;
        DWORD code = 0;
        if (!GetExitCodeProcess(m_process, &code)) return m_lastExitCode;
        return code == STILL_ACTIVE ? -1 : static_cast<int>(code);
    }

private:
    void CloseHandles() noexcept {
        // 句柄关掉之后退出码就取不到了，所以先留一份：调用方最需要它的时刻正是停止之后。
        if (m_process) {
            DWORD code = 0;
            if (GetExitCodeProcess(m_process, &code) && code != STILL_ACTIVE) {
                m_lastExitCode = static_cast<int>(code);
            }
            CloseHandle(m_process);
            m_process = nullptr;
        }
        if (m_stopEvent) {
            CloseHandle(m_stopEvent);
            m_stopEvent = nullptr;
        }
    }

    HANDLE m_process = nullptr;
    HANDLE m_stopEvent = nullptr;
    int m_lastExitCode = -1;
};

} // namespace Gaokun::Host
