#include "VendorServices.h"

#include "Logger.h"

#include <windows.h>

#include <tlhelp32.h>

#include <algorithm>
#include <string>
#include <vector>

namespace Service::VendorServices {

namespace {

// 名单。每一项都是本程序已经承担、或对本机无用的后台服务。选择依据见头文件。
constexpr const wchar_t* kServices[] = {
    L"HiConnectivityService",     // PC Manager 的连接服务
    L"HwDistributedMainService",  // 多屏协同
    L"HwPCCoreService",           // BasicService，PC Manager 核心
    L"LCD_Service",               // 显示增强，色彩由本程序的显示组件接管
    L"MBAMainService",            // MateBookService
    L"MouseCrossingDaemon",       // 鼠标穿越
    L"WUCSProxy",                 // 升级代理
};

// 刻意不在名单里的还有 HW_OSDServer。它的名字看着像个提示浮层，实际是整套 Fn 功能键的
// 后端：HardwareHal.dll 导出 SetVolume、SetScreen、SetMic 与 SetMicLightStatus（麦克风静音
// 指示灯）、SetWifi、SetRefreshRate 乃至 SyncRefreshRateToBIOS，按键事件则经
// StartMonitorSpiInterrupt 收上来。本程序没有任何一项的替代品，禁用会真的丢功能。

constexpr const wchar_t* kBackupKey =
    L"SOFTWARE\\OpenEGoHub\\VendorServiceBackup";

[[nodiscard]] bool ReadBackup(const wchar_t* name, DWORD& startType) noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kBackupKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD size = sizeof(startType);
    DWORD type = 0;
    const bool ok = RegQueryValueExW(key, name, nullptr, &type,
                                     reinterpret_cast<BYTE*>(&startType), &size) == ERROR_SUCCESS &&
                    type == REG_DWORD;
    RegCloseKey(key);
    return ok;
}

void WriteBackup(const wchar_t* name, DWORD startType) noexcept {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kBackupKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    (void)RegSetValueExW(key, name, 0, REG_DWORD,
                         reinterpret_cast<const BYTE*>(&startType), sizeof(startType));
    RegCloseKey(key);
}

void EraseBackup(const wchar_t* name) noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kBackupKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    (void)RegDeleteValueW(key, name);
    RegCloseKey(key);
}

// 取服务当前的启动类型。服务不存在时返回 false，这是常态：不同机型装的组件并不一样。
[[nodiscard]] bool QueryStartType(SC_HANDLE manager, const wchar_t* name,
                                  DWORD& startType) noexcept {
    SC_HANDLE service = OpenServiceW(manager, name, SERVICE_QUERY_CONFIG);
    if (!service) return false;

    DWORD needed = 0;
    (void)QueryServiceConfigW(service, nullptr, 0, &needed);
    bool ok = false;
    if (needed > 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<BYTE> buffer(needed);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (QueryServiceConfigW(service, config, needed, &needed)) {
            startType = config->dwStartType;
            ok = true;
        }
    }
    CloseServiceHandle(service);
    return ok;
}

[[nodiscard]] bool SetStartType(SC_HANDLE manager, const wchar_t* name, DWORD startType) noexcept {
    SC_HANDLE service = OpenServiceW(manager, name, SERVICE_CHANGE_CONFIG);
    if (!service) return false;
    const bool ok = ChangeServiceConfigW(service, SERVICE_NO_CHANGE, startType, SERVICE_NO_CHANGE,
                                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                         nullptr) != FALSE;
    CloseServiceHandle(service);
    return ok;
}

[[nodiscard]] bool IsRunning(SC_HANDLE manager, const wchar_t* name) noexcept {
    SC_HANDLE service = OpenServiceW(manager, name, SERVICE_QUERY_STATUS);
    if (!service) return false;
    SERVICE_STATUS status{};
    const bool running = QueryServiceStatus(service, &status) != FALSE &&
                         status.dwCurrentState != SERVICE_STOPPED;
    CloseServiceHandle(service);
    return running;
}

// 启动服务并等它真的起来。恢复路径要用：只把启动类型改回自动，服务仍是停着的，用户要等到
// 下次开机才看得到东西回来——而禁用是立刻生效的，两个方向不对称会让人以为恢复没成功。
void StartAndWait(SC_HANDLE manager, const wchar_t* name) noexcept {
    SC_HANDLE service = OpenServiceW(manager, name, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!service) return;

    SERVICE_STATUS status{};
    if (QueryServiceStatus(service, &status) && status.dwCurrentState == SERVICE_STOPPED) {
        if (StartServiceW(service, 0, nullptr)) {
            for (int i = 0; i < 100; ++i) {
                if (!QueryServiceStatus(service, &status)) break;
                if (status.dwCurrentState == SERVICE_RUNNING) break;
                Sleep(100);
            }
        }
    }
    CloseServiceHandle(service);
}

// 停止并等到真的停下来。
//
// ControlService 只是把请求投递出去就返回，不等服务退出。这些服务里至少 HiConnectivityService
// 配了 FAILURE_ACTIONS = RESTART（延迟 1 秒），停止途中若被判成失败，SCM 会把它拉回来，
// 于是「禁用」看上去只对一部分服务生效。等到 SERVICE_STOPPED 再往下走，这个窗口就没有了。
void StopAndWait(SC_HANDLE manager, const wchar_t* name) noexcept {
    SC_HANDLE service = OpenServiceW(manager, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!service) return;

    SERVICE_STATUS status{};
    if (QueryServiceStatus(service, &status) && status.dwCurrentState != SERVICE_STOPPED) {
        (void)ControlService(service, SERVICE_CONTROL_STOP, &status);

        // 最多等 10 秒。等不到就继续：启动类型已经改了，下次开机总归不会再起来。
        for (int i = 0; i < 100; ++i) {
            if (!QueryServiceStatus(service, &status)) break;
            if (status.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(100);
        }
    }
    CloseServiceHandle(service);
}

// 终止 PC Manager 的用户态常驻进程。
//
// 只改服务配置不够。这些进程由华为的服务拉起，服务停掉之后它们仍然活着，并且会把
// HiConnectivityService 的启动类型改回自动、再把它启动起来——实测禁用后两秒内就被拉回，
// 而 SetStartType 那一侧看到的全是成功。HiConnectivityService 与 HwDistributedMainService
// 还会互相拉起，逐个处理时永远有一个活着去救另一个，三轮都收敛不了。
//
// 只按目录匹配，不按进程名：PC Manager 的组件散在多层子目录里，名字也不成体系。
// HuaweiThpService 不在这两个目录下，不会被误伤——它是本程序接管失败时的触控兜底。
void TerminateVendorProcesses() noexcept {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry)) {
        CloseHandle(snapshot);
        return;
    }

    do {
        HANDLE process = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
        if (!process) continue;

        wchar_t path[MAX_PATH]{};
        DWORD length = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, path, &length)) {
            std::wstring lowered(path, length);
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

            if (lowered.find(L"\\huawei\\pcmanager\\") != std::wstring::npos ||
                lowered.find(L"\\huawei\\hiview\\") != std::wstring::npos) {
                (void)TerminateProcess(process, 0);
            }
        }
        CloseHandle(process);
    } while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);
}

} // namespace

Status Query() noexcept {
    Status status{};
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return status;

    for (const wchar_t* name : kServices) {
        DWORD startType = 0;
        if (!QueryStartType(manager, name, startType)) continue;
        ++status.total;
        if (startType == SERVICE_DISABLED) ++status.disabled;
        if (IsRunning(manager, name)) ++status.running;
    }
    CloseServiceHandle(manager);
    return status;
}

bool DisableAll() noexcept {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        LOG_ERROR("Service", __func__, "Vendor", "Cannot open the SCM (err={}).", GetLastError());
        return false;
    }

    // 顺序是实测定下来的，三步都不能挪位。先断掉守护进程，否则后面每一步都会被撤销。
    TerminateVendorProcesses();

    bool allOk = true;

    // 一、记录原值并全部改成禁用。这一遍不停服务：一边停一边改，会给还活着的服务留出
    // 把同伴改回自动的窗口。
    for (const wchar_t* name : kServices) {
        DWORD startType = 0;
        if (!QueryStartType(manager, name, startType)) continue;  // 本机没装，跳过
        if (startType == SERVICE_DISABLED) continue;              // 已禁用，别覆盖既有记录

        WriteBackup(name, startType);
        if (!SetStartType(manager, name, SERVICE_DISABLED)) {
            LOG_WARN("Service", __func__, "Vendor", "Cannot disable {} (err={}).",
                     std::wstring(name).c_str(), GetLastError());
            EraseBackup(name);
            allOk = false;
        }
    }

    // 二、停掉当前实例，否则要等到下次开机才见效，而用户刚点完按钮就会去看效果。
    for (const wchar_t* name : kServices) {
        StopAndWait(manager, name);
    }

    // 三、复查。停止过程中启动类型仍可能被改回自动，而那一侧的 SetStartType 早已返回成功，
    // 不复查就只能等用户发现「有几个没被禁用」。此时同伴都已停止，重设不会再被撤销。
    for (const wchar_t* name : kServices) {
        DWORD after = 0;
        if (!QueryStartType(manager, name, after)) continue;
        if (after == SERVICE_DISABLED) continue;

        if (SetStartType(manager, name, SERVICE_DISABLED)) {
            StopAndWait(manager, name);
        } else {
            LOG_WARN("Service", __func__, "Vendor", "{} came back as start type {}.",
                     std::wstring(name).c_str(), after);
            allOk = false;
        }
    }

    CloseServiceHandle(manager);
    return allOk;
}

bool RestoreAll() noexcept {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return false;

    bool allOk = true;
    for (const wchar_t* name : kServices) {
        DWORD current = 0;
        if (!QueryStartType(manager, name, current)) continue;  // 本机没装
        if (current != SERVICE_DISABLED) continue;              // 已经是启用的，不动

        // 有记录就按记录写回。没有记录说明这个服务不是本程序禁用的——用户可能用别的工具
        // 关过它。此时仍然恢复为自动启动：这几个都是 PC Manager 随开机拉起的常驻服务，
        // 而「按了恢复却什么都没发生」比多启动一个服务更糟，实测踩到过这个坑。
        DWORD target = SERVICE_AUTO_START;
        const bool hadBackup = ReadBackup(name, target);

        if (SetStartType(manager, name, target)) {
            if (hadBackup) EraseBackup(name);
        } else {
            LOG_WARN("Service", __func__, "Vendor", "Cannot restore {} (err={}).",
                     std::wstring(name).c_str(), GetLastError());
            allOk = false;
        }
    }

    // 改完启动类型再把它们起回来。只改类型的话服务仍是停着的，要等下次开机才回来，而禁用
    // 那一侧是立刻生效的——两个方向不对称，用户按了恢复看不到任何变化，会以为没成功。
    //
    // 放在第二遍而不是边改边起：这几个服务之间互相拉起过（见 TerminateVendorProcesses 的
    // 说明），全部改回自动之后再启动，不会有谁在半途被同伴按回去。
    for (const wchar_t* name : kServices) {
        DWORD startType = 0;
        if (!QueryStartType(manager, name, startType)) continue;
        if (startType == SERVICE_DISABLED) continue;
        StartAndWait(manager, name);
    }

    CloseServiceHandle(manager);
    return allOk;
}

} // namespace Service::VendorServices
