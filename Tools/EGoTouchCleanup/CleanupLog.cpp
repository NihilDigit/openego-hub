#include "CleanupLog.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>

namespace Cleanup {
namespace {

FILE *g_file = nullptr;
std::wstring g_path;
bool g_console = false;

void EnsureDirectory(const std::wstring &path) {
    // CreateDirectoryW 不建中间层，而 ProgramData\OpenEGoHub\logs 在干净机器上整条都不存在。
    std::wstring buffer = path;
    for (size_t i = 3; i < buffer.size(); ++i) {
        if (buffer[i] != L'\\') continue;
        buffer[i] = L'\0';
        (void)CreateDirectoryW(buffer.c_str(), nullptr);
        buffer[i] = L'\\';
    }
    (void)CreateDirectoryW(buffer.c_str(), nullptr);
}

std::wstring DefaultLogPath() {
    PWSTR programData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData))) return {};
    std::wstring directory = programData;
    CoTaskMemFree(programData);
    directory += L"\\OpenEGoHub\\logs";
    EnsureDirectory(directory);
    return directory + L"\\cleanup.log";
}

std::wstring FallbackLogPath() {
    wchar_t temp[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::wstring(temp) + L"OpenEGoHubCleanup.log";
}

bool TryOpen(const std::wstring &path) {
    if (path.empty()) return false;
    g_file = _wfsopen(path.c_str(), L"a", _SH_DENYWR);
    if (!g_file) return false;
    g_path = path;
    return true;
}

}  // namespace

void OpenLog(const std::wstring &path) {
    // 命令行里 --dry-run 跑的时候有控制台，MSI 的 deferred CA 里没有。
    g_console = GetStdHandle(STD_OUTPUT_HANDLE) != nullptr &&
                GetStdHandle(STD_OUTPUT_HANDLE) != INVALID_HANDLE_VALUE;

    if (TryOpen(path)) return;
    if (TryOpen(DefaultLogPath())) return;
    (void)TryOpen(FallbackLogPath());
}

void CloseLog() {
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

const std::wstring &LogPath() { return g_path; }

void Log(const wchar_t *format, ...) {
    wchar_t message[1024];
    va_list args;
    va_start(args, format);
    const int written = _vsnwprintf_s(message, _TRUNCATE, format, args);
    va_end(args);
    if (written < 0) message[0] = L'\0';

    SYSTEMTIME now{};
    GetLocalTime(&now);

    if (g_file) {
        fwprintf(g_file, L"%04u-%02u-%02u %02u:%02u:%02u.%03u %s\n", now.wYear, now.wMonth,
                 now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, message);
        // 每条都 flush：这个进程整个生命周期就几十秒，而要读日志的场景正是它中途死掉。
        fflush(g_file);
    }
    if (g_console) wprintf(L"%s\n", message);
}

}  // namespace Cleanup
