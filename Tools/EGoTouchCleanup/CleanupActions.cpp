#include "CleanupActions.h"

#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include "CleanupLog.h"

namespace Cleanup {
namespace {

constexpr unsigned kServiceStopTimeoutMs = 30000;
constexpr unsigned kPollIntervalMs = 250;
constexpr int kFileDeleteAttempts = 10;
constexpr unsigned kFileDeleteRetryMs = 100;

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &path))) return {};
    std::wstring result = path;
    CoTaskMemFree(path);
    return result;
}

bool NameMatches(const std::vector<std::wstring> &names, const wchar_t *candidate) {
    for (const auto &name : names) {
        if (_wcsicmp(name.c_str(), candidate) == 0) return true;
    }
    return false;
}

std::vector<DWORD> FindProcesses(const std::vector<std::wstring> &imageNames) {
    std::vector<DWORD> found;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        Log(L"  process snapshot failed, error %lu", GetLastError());
        return found;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentProcessId();
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == self) continue;
            if (NameMatches(imageNames, entry.szExeFile)) found.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

std::wstring ImageNameOf(DWORD pid) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return L"?";
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::wstring name = L"?";
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == pid) {
                name = entry.szExeFile;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return name;
}

void TerminatePid(DWORD pid) {
    const HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!process) {
        Log(L"  cannot open pid %lu, error %lu", pid, GetLastError());
        return;
    }
    if (TerminateProcess(process, 0)) {
        WaitForSingleObject(process, 2000);
        Log(L"  terminated pid %lu", pid);
    } else {
        Log(L"  TerminateProcess failed for pid %lu, error %lu", pid, GetLastError());
    }
    CloseHandle(process);
}

bool SetFileWritable(const std::wstring &path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    if ((attributes & FILE_ATTRIBUTE_READONLY) == 0) return true;
    return SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY) != FALSE;
}

// 刚被停掉的服务和刚被杀掉的进程还可能按着文件句柄，重试比一次失败就放弃划算。
bool DeleteFileWithRetry(const std::wstring &path) {
    for (int attempt = 0; attempt < kFileDeleteAttempts; ++attempt) {
        (void)SetFileWritable(path);
        if (DeleteFileW(path.c_str())) return true;
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return true;
        Sleep(kFileDeleteRetryMs);
    }
    Log(L"  skipped (still locked): %s", path.c_str());
    return false;
}

bool DeleteTree(const std::wstring &directory) {
    WIN32_FIND_DATAW data{};
    const HANDLE find = FindFirstFileW((directory + L"\\*").c_str(), &data);
    bool complete = true;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring child = directory + L"\\" + name;
            const bool isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            // 重解析点只删链接本身，不跟进去：跟进去会把链接指向的目录一起清掉。
            const bool isReparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (isDirectory && !isReparse) {
                if (!DeleteTree(child)) complete = false;
            } else if (isDirectory) {
                if (!RemoveDirectoryW(child.c_str())) complete = false;
            } else {
                if (!DeleteFileWithRetry(child)) complete = false;
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }

    for (int attempt = 0; attempt < kFileDeleteAttempts; ++attempt) {
        if (RemoveDirectoryW(directory.c_str())) return complete;
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) {
            return complete;
        }
        Sleep(kFileDeleteRetryMs);
    }
    Log(L"  skipped (directory not empty or locked): %s", directory.c_str());
    return false;
}

// 清空 SCM 的失败动作。脚本装出来的 EGoTouchService 配了 restart/5s，不清掉就会边停边活，
// 停到 STOPPED 之后 SCM 又把它拉起来，接着 DeleteService 只是标记删除，服务留在那里。
void ClearFailureActions(SC_HANDLE service) {
    SC_ACTION none[1]{};
    SERVICE_FAILURE_ACTIONSW actions{};
    actions.dwResetPeriod = 0;
    actions.lpRebootMsg = const_cast<LPWSTR>(L"");
    actions.lpCommand = const_cast<LPWSTR>(L"");
    actions.cActions = 0;
    // lpsaActions 为 NULL 表示「保持原样」，要清空必须给一个非空指针配 cActions = 0。
    actions.lpsaActions = none;
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &actions)) {
        Log(L"  could not clear failure actions, error %lu", GetLastError());
    }
}

bool WaitForStopped(SC_HANDLE service, unsigned timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    SERVICE_STATUS status{};
    do {
        if (!QueryServiceStatus(service, &status)) return false;
        if (status.dwCurrentState == SERVICE_STOPPED) return true;
        Sleep(kPollIntervalMs);
    } while (GetTickCount64() < deadline);
    return false;
}

}  // namespace

KnownFolders ResolveKnownFolders() {
    KnownFolders folders;
    folders.programFiles = KnownFolder(FOLDERID_ProgramFiles);
    folders.programData = KnownFolder(FOLDERID_ProgramData);
    folders.commonPrograms = KnownFolder(FOLDERID_CommonPrograms);
    return folders;
}

void KillProcesses(const std::vector<std::wstring> &imageNames, bool dryRun) {
    const auto pids = FindProcesses(imageNames);
    if (pids.empty()) {
        Log(L"  none running");
        return;
    }
    for (const DWORD pid : pids) {
        const std::wstring name = ImageNameOf(pid);
        if (dryRun) {
            Log(L"  would terminate %s (pid %lu)", name.c_str(), pid);
        } else {
            Log(L"  terminating %s (pid %lu)", name.c_str(), pid);
            TerminatePid(pid);
        }
    }
}

void KillProcessesAfterGrace(const std::vector<std::wstring> &imageNames, unsigned graceMs,
                             bool dryRun) {
    if (dryRun) {
        KillProcesses(imageNames, true);
        return;
    }
    const ULONGLONG deadline = GetTickCount64() + graceMs;
    while (GetTickCount64() < deadline) {
        if (FindProcesses(imageNames).empty()) {
            Log(L"  all hosts exited on their own");
            return;
        }
        Sleep(kPollIntervalMs);
    }
    KillProcesses(imageNames, false);
}

void RemoveService(const std::wstring &name, bool dryRun) {
    // 保留清单是硬门，不靠调用方传对名字。
    if (IsProtectedService(name)) {
        Log(L"  refusing to touch protected service %s", name.c_str());
        return;
    }

    const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        Log(L"  OpenSCManager failed, error %lu", GetLastError());
        return;
    }

    // --dry-run 只要查询权限。要停删的那套权限普通用户拿不到，OpenService 会以 error 5
    // 失败,清单里那一行就分不出服务是不存在还是够不着——而这正是 --dry-run 的用处。
    const DWORD access = dryRun ? SERVICE_QUERY_STATUS
                                : (SERVICE_QUERY_STATUS | SERVICE_STOP | SERVICE_CHANGE_CONFIG |
                                   DELETE);
    const SC_HANDLE service = OpenServiceW(manager, name.c_str(), access);
    if (!service) {
        const DWORD error = GetLastError();
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            Log(L"  %s is not installed", name.c_str());
        } else {
            Log(L"  OpenService(%s) failed, error %lu", name.c_str(), error);
        }
        CloseServiceHandle(manager);
        return;
    }

    if (dryRun) {
        SERVICE_STATUS current{};
        const bool queried = QueryServiceStatus(service, &current) != FALSE;
        Log(L"  would clear failure actions, stop and delete %s (state %lu)", name.c_str(),
            queried ? current.dwCurrentState : 0);
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return;
    }

    ClearFailureActions(service);

    SERVICE_STATUS status{};
    if (QueryServiceStatus(service, &status) && status.dwCurrentState != SERVICE_STOPPED) {
        if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
            Log(L"  stop request for %s failed, error %lu", name.c_str(), GetLastError());
        }
        if (WaitForStopped(service, kServiceStopTimeoutMs)) {
            Log(L"  %s stopped", name.c_str());
        } else {
            Log(L"  %s did not reach STOPPED in time, deleting anyway", name.c_str());
        }
    }

    if (DeleteService(service)) {
        Log(L"  %s deleted", name.c_str());
    } else {
        Log(L"  DeleteService(%s) failed, error %lu", name.c_str(), GetLastError());
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
}

void WaitForVendorTouchService(unsigned timeoutMs, bool dryRun) {
    if (dryRun) {
        Log(L"  would wait up to %u ms for HuaweiThpService to run again", timeoutMs);
        return;
    }

    const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        Log(L"  OpenSCManager failed, error %lu", GetLastError());
        return;
    }
    const SC_HANDLE service = OpenServiceW(manager, L"HuaweiThpService", SERVICE_QUERY_STATUS);
    if (!service) {
        Log(L"  HuaweiThpService is not installed, nothing to wait for");
        CloseServiceHandle(manager);
        return;
    }

    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    SERVICE_STATUS status{};
    bool back = false;
    do {
        if (!QueryServiceStatus(service, &status)) break;
        if (status.dwCurrentState == SERVICE_RUNNING ||
            status.dwCurrentState == SERVICE_START_PENDING) {
            back = true;
            break;
        }
        Sleep(kPollIntervalMs);
    } while (GetTickCount64() < deadline);

    Log(back ? L"  HuaweiThpService owns touch again"
             : L"  HuaweiThpService did not come back in time, continuing");

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
}

void RemovePath(const PathTarget &target, const std::wstring &programData, bool dryRun) {
    if (IsProtectedPath(target.path, programData)) {
        Log(L"  refusing to delete protected path %s", target.path.c_str());
        return;
    }

    const DWORD attributes = GetFileAttributesW(target.path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        Log(L"  absent: %s", target.path.c_str());
        return;
    }

    if (dryRun) {
        Log(L"  would delete: %s (%s)", target.path.c_str(), target.reason.c_str());
        return;
    }

    Log(L"  deleting: %s (%s)", target.path.c_str(), target.reason.c_str());
    const bool complete = target.kind == TargetKind::Directory ? DeleteTree(target.path)
                                                               : DeleteFileWithRetry(target.path);
    if (complete) Log(L"  removed: %s", target.path.c_str());
}

}  // namespace Cleanup
