#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

// 宿主进程的文件日志。三个宿主共用。
//
// 只有头文件：hal 的构建脚本按目录展开 src\<target>\*.cpp，src\shared 不属于任何目标，
// 放 .cpp 进去不会被编到任何一个宿主里。
//
// 不复用 Common/Logger：hal 是独立的 ARM64EC 构建，不 include 上层任何东西。
//
// 存在的理由是宿主被服务以 CREATE_NO_WINDOW 拉起，wprintf 的输出没有去处，宿主为什么
// 退出在外面只剩一个退出码。
namespace Gaokun::HostLog {

enum class Level { Trace = 0, Debug, Info, Warn, Error, Critical, Off };

namespace Detail {

inline SRWLOCK g_lock = SRWLOCK_INIT;
inline FILE *g_file = nullptr;
inline Level g_min = Level::Info;
inline wchar_t g_path[MAX_PATH]{};

inline constexpr long kRotateBytes = 2 * 1024 * 1024;

inline const char *LevelTag(Level level) noexcept {
    switch (level) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO ";
    case Level::Warn: return "WARN ";
    case Level::Error: return "ERROR";
    case Level::Critical: return "CRIT ";
    default: return "?????";
    }
}

// 逐级建目录。CreateDirectoryW 不建中间层，而 ProgramData\OpenEGoHub 与它下面的 logs
// 在干净机器上都不存在。
inline void EnsureDirectory(const wchar_t *path) noexcept {
    wchar_t buffer[MAX_PATH];
    if (wcslen(path) >= MAX_PATH) return;
    wcscpy_s(buffer, path);
    for (wchar_t *p = buffer + 3; *p; ++p) {
        if (*p != L'\\') continue;
        *p = L'\0';
        (void)CreateDirectoryW(buffer, nullptr);
        *p = L'\\';
    }
    (void)CreateDirectoryW(buffer, nullptr);
}

inline bool OpenFile() noexcept {
    // _SH_DENYWR：允许别人读（tail、记事本），但不允许第二个写者。同名宿主被重复拉起时
    // 后来者拿不到文件，日志少一份好过两份交错到分不出是谁写的。
    g_file = _wfsopen(g_path, L"a", _SH_DENYWR);
    return g_file != nullptr;
}

// 超过阈值就把当前文件让位给 .1。只留一代：宿主的日志是给「这次为什么没起来」用的，
// 更早的轮转文件没人会去看。
inline void RotateIfNeeded() noexcept {
    if (!g_file || ftell(g_file) < kRotateBytes) return;

    wchar_t previous[MAX_PATH];
    swprintf_s(previous, L"%s.1", g_path);

    fclose(g_file);
    g_file = nullptr;
    (void)DeleteFileW(previous);
    (void)MoveFileW(g_path, previous);
    (void)OpenFile();
}

} // namespace Detail

inline Level ParseLevel(const wchar_t *name) noexcept {
    if (!name) return Level::Info;
    if (_wcsicmp(name, L"trace") == 0) return Level::Trace;
    if (_wcsicmp(name, L"debug") == 0) return Level::Debug;
    if (_wcsicmp(name, L"info") == 0) return Level::Info;
    if (_wcsicmp(name, L"warn") == 0 || _wcsicmp(name, L"warning") == 0) return Level::Warn;
    if (_wcsicmp(name, L"error") == 0) return Level::Error;
    if (_wcsicmp(name, L"critical") == 0) return Level::Critical;
    if (_wcsicmp(name, L"off") == 0) return Level::Off;
    return Level::Info;
}

// hostName 是不带扩展名的宿主名，例如 L"GaokunPenHost"，决定日志文件名。
inline void Init(const wchar_t *hostName, Level minimum) noexcept {
    AcquireSRWLockExclusive(&Detail::g_lock);

    if (Detail::g_file) {
        fclose(Detail::g_file);
        Detail::g_file = nullptr;
    }
    Detail::g_min = minimum;

    if (minimum != Level::Off) {
        wchar_t root[MAX_PATH];
        if (GetEnvironmentVariableW(L"ProgramData", root, MAX_PATH) == 0) {
            wcscpy_s(root, L"C:\\ProgramData");
        }

        wchar_t directory[MAX_PATH];
        swprintf_s(directory, L"%s\\OpenEGoHub\\logs", root);
        Detail::EnsureDirectory(directory);

        swprintf_s(Detail::g_path, L"%s\\%s.log", directory, hostName);
        (void)Detail::OpenFile();
    }

    ReleaseSRWLockExclusive(&Detail::g_lock);
}

inline void Init(const wchar_t *hostName, const wchar_t *levelName) noexcept {
    Init(hostName, ParseLevel(levelName));
}

// 从命令行取 --log-level <lvl>，缺省 info。三个宿主的参数解析各不相同，但这一条对所有
// 模式都一样，而且要在别的参数被处理之前生效——最早的失败分支就在解析之前。
inline void InitFromCommandLine(const wchar_t *hostName, int argc, wchar_t **argv) noexcept {
    const wchar_t *level = L"info";
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], L"--log-level") == 0) {
            level = argv[i + 1];
            break;
        }
    }
    Init(hostName, level);
}

inline void Shutdown() noexcept {
    AcquireSRWLockExclusive(&Detail::g_lock);
    if (Detail::g_file) {
        fclose(Detail::g_file);
        Detail::g_file = nullptr;
    }
    ReleaseSRWLockExclusive(&Detail::g_lock);
}

// 格式串是窄字符，宽字符实参用 %ls。未 Init 时整条是 no-op，宿主在命令行模式下照常运行。
inline void Write(Level level, const char *format, ...) noexcept {
    if (level < Detail::g_min) return;

    char message[1024];
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (written < 0) message[0] = '\0';

    SYSTEMTIME now{};
    GetLocalTime(&now);

    AcquireSRWLockExclusive(&Detail::g_lock);
    if (Detail::g_file) {
        fprintf(Detail::g_file, "%04u-%02u-%02u %02u:%02u:%02u.%03u %s %s\n", now.wYear,
                now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                Detail::LevelTag(level), message);
        // 每条都 flush：日志要读的场景恰恰是宿主异常退出，缓冲区里的内容那时已经没了。
        fflush(Detail::g_file);
        Detail::RotateIfNeeded();
    }
    ReleaseSRWLockExclusive(&Detail::g_lock);
}

} // namespace Gaokun::HostLog

#define HOST_LOG_TRACE(...) ::Gaokun::HostLog::Write(::Gaokun::HostLog::Level::Trace, __VA_ARGS__)
#define HOST_LOG_DEBUG(...) ::Gaokun::HostLog::Write(::Gaokun::HostLog::Level::Debug, __VA_ARGS__)
#define HOST_LOG_INFO(...) ::Gaokun::HostLog::Write(::Gaokun::HostLog::Level::Info, __VA_ARGS__)
#define HOST_LOG_WARN(...) ::Gaokun::HostLog::Write(::Gaokun::HostLog::Level::Warn, __VA_ARGS__)
#define HOST_LOG_ERROR(...) ::Gaokun::HostLog::Write(::Gaokun::HostLog::Level::Error, __VA_ARGS__)
#define HOST_LOG_CRITICAL(...) \
    ::Gaokun::HostLog::Write(::Gaokun::HostLog::Level::Critical, __VA_ARGS__)
