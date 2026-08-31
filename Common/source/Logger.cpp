/**
 * @file Logger.cpp
 * @brief 自研轻量级 MiniLogger 的实现 (零 C++ Streams 依赖优化版)
 */
#include "Logger.h"
#include "GuiLogSink.h"
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <ctime>
#include <cctype>
#include <cstdio>
#if defined(_WIN32)
#include <share.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#endif

namespace MiniFmt {

static void format_single_arg(std::string& out, const std::string& rawSpec, const LogArg& arg) {
    // std::format 风格的说明符以冒号起头：{:06X} 交到这里的 spec 是 ":06X"。此前没有跳过冒号，
    // 于是宽度和进制整段被忽略，所有 {:...} 都按十进制打印——而格式串里的 "0x" 是手写前缀，
    // 于是 id=0x{:06X} 打出来是 "id=0x283"，那个 283 其实是十进制（即 0x11B）。这种日志比没有
    // 更糟：它看着像十六进制，会把人引到错误结论上。
    const std::string spec =
        (!rawSpec.empty() && rawSpec[0] == ':') ? rawSpec.substr(1) : rawSpec;

    char buf[256];
    buf[0] = '\0';

    if (spec.empty()) {
        switch (arg.type) {
            case LogArg::Type::Int:
                snprintf(buf, sizeof(buf), "%d", static_cast<int>(arg.i_val));
                break;
            case LogArg::Type::UInt:
                snprintf(buf, sizeof(buf), "%u", static_cast<unsigned int>(arg.u_val));
                break;
            case LogArg::Type::LongLong:
                snprintf(buf, sizeof(buf), "%lld", arg.i_val);
                break;
            case LogArg::Type::ULongLong:
                snprintf(buf, sizeof(buf), "%llu", arg.u_val);
                break;
            case LogArg::Type::Double:
                snprintf(buf, sizeof(buf), "%f", arg.d_val);
                break;
            case LogArg::Type::String:
                out += arg.s_val;
                return;
            case LogArg::Type::Pointer:
                snprintf(buf, sizeof(buf), "%p", arg.p_val);
                break;
        }
        out += buf;
        return;
    }

    if (spec[0] == '<') {
        int width = 0;
        try {
            width = std::stoi(spec.substr(1));
        } catch (...) {}

        std::string base_str;
        switch (arg.type) {
            case LogArg::Type::Int:
                snprintf(buf, sizeof(buf), "%d", static_cast<int>(arg.i_val));
                base_str = buf;
                break;
            case LogArg::Type::UInt:
                snprintf(buf, sizeof(buf), "%u", static_cast<unsigned int>(arg.u_val));
                base_str = buf;
                break;
            case LogArg::Type::LongLong:
                snprintf(buf, sizeof(buf), "%lld", arg.i_val);
                base_str = buf;
                break;
            case LogArg::Type::ULongLong:
                snprintf(buf, sizeof(buf), "%llu", arg.u_val);
                base_str = buf;
                break;
            case LogArg::Type::Double:
                snprintf(buf, sizeof(buf), "%f", arg.d_val);
                base_str = buf;
                break;
            case LogArg::Type::String:
                base_str = arg.s_val;
                break;
            case LogArg::Type::Pointer:
                snprintf(buf, sizeof(buf), "%p", arg.p_val);
                base_str = buf;
                break;
        }
        
        if (width > 0) {
            std::string fmt_align = "%-" + std::to_string(width) + "s";
            char align_buf[512];
            snprintf(align_buf, sizeof(align_buf), fmt_align.c_str(), base_str.c_str());
            out += align_buf;
        } else {
            out += base_str;
        }
        return;
    }

    bool uppercase = false;
    bool hex_mode = false;
    bool fill_zero = false;
    int width = 0;

    size_t idx = 0;
    if (idx < spec.size() && spec[idx] == '0') {
        fill_zero = true;
        idx++;
    }
    size_t num_start = idx;
    while (idx < spec.size() && std::isdigit(static_cast<unsigned char>(spec[idx]))) {
        idx++;
    }
    if (idx > num_start) {
        try {
            width = std::stoi(spec.substr(num_start, idx - num_start));
        } catch (...) {}
    }
    if (idx < spec.size()) {
        char ch = spec[idx];
        if (ch == 'x' || ch == 'X') {
            hex_mode = true;
            if (ch == 'X') uppercase = true;
        }
    }

    std::string fmt_str = "%";
    if (fill_zero) fmt_str += "0";
    if (width > 0) fmt_str += std::to_string(width);
    
    if (hex_mode) {
        if (uppercase) fmt_str += "llX";
        else fmt_str += "llx";
    } else {
        if (arg.type == LogArg::Type::UInt || arg.type == LogArg::Type::ULongLong) {
            fmt_str += "llu";
        } else {
            fmt_str += "lld";
        }
    }

    if (arg.type == LogArg::Type::UInt || arg.type == LogArg::Type::ULongLong) {
        snprintf(buf, sizeof(buf), fmt_str.c_str(), arg.u_val);
    } else if (arg.type == LogArg::Type::Double) {
        snprintf(buf, sizeof(buf), fmt_str.c_str(), static_cast<long long>(arg.d_val));
    } else if (arg.type == LogArg::Type::Pointer) {
        snprintf(buf, sizeof(buf), fmt_str.c_str(), reinterpret_cast<uintptr_t>(arg.p_val));
    } else if (arg.type == LogArg::Type::String) {
        try {
            long long val = std::stoll(arg.s_val);
            snprintf(buf, sizeof(buf), fmt_str.c_str(), val);
        } catch (...) {
            snprintf(buf, sizeof(buf), "%s", arg.s_val.c_str());
        }
    } else {
        snprintf(buf, sizeof(buf), fmt_str.c_str(), arg.i_val);
    }
    out += buf;
}

std::string format_core(const char* fmt_str, const LogArg* args, size_t count) {
    std::string out;
    out.reserve(256);
    size_t arg_idx = 0;
    
    while (*fmt_str) {
        if (*fmt_str == '{') {
            const char* end = fmt_str + 1;
            while (*end && *end != '}') {
                end++;
            }
            if (*end == '}') {
                std::string spec(fmt_str + 1, end);
                if (arg_idx < count) {
                    format_single_arg(out, spec, args[arg_idx++]);
                } else {
                    out += "{";
                    out += spec;
                    out += "}";
                }
                fmt_str = end + 1;
                continue;
            }
        }
        out += *fmt_str;
        fmt_str++;
    }
    return out;
}

} // namespace MiniFmt

namespace Common {

namespace detail {
std::atomic<int> g_logMinLevel{ static_cast<int>(LogLevel::Info) };
std::atomic<int> g_logLevelPollBudget{ 0 };
} // namespace detail

namespace {

FILE* g_fileStream = nullptr;
std::mutex g_logMutex;
std::filesystem::path g_logPath;
bool g_initialized = false;

// 以下三项只在持有 g_logMutex 时读写。
std::wstring g_iniPath;
std::uint64_t g_iniStamp = 0;       // 上次读到的 logging.ini 修改时间
std::uint64_t g_lastCheckTick = 0;  // 上次查看 ini 的时刻，0 表示还没查过

constexpr std::uintmax_t kMaxLogBytes = 5 * 1024 * 1024;
constexpr std::uint64_t kRecheckIntervalMs = 10000;
constexpr int kPollBudget = 64;  // 每 64 次日志调用才去碰一次文件系统

struct LevelName {
    const wchar_t* wide;
    LogLevel level;
};

constexpr LevelName kLevelNames[] = {
    { L"trace",    LogLevel::Trace },
    { L"debug",    LogLevel::Debug },
    { L"info",     LogLevel::Info },
    { L"warn",     LogLevel::Warn },
    { L"error",    LogLevel::Error },
    { L"critical", LogLevel::Critical },
    { L"off",      LogLevel::Off },
};

LogLevel ParseLevelName(const wchar_t* raw) {
    std::wstring token;
    for (const wchar_t* p = raw; p && *p; ++p) {
        if (*p == L' ' || *p == L'\t' || *p == L'"') continue;
        token.push_back(*p < 128 ? static_cast<wchar_t>(std::tolower(static_cast<unsigned char>(*p)))
                                 : *p);
    }
    for (const auto& entry : kLevelNames) {
        if (token == entry.wide) {
            return entry.level;
        }
    }
    return LogLevel::Info;
}

// 轮转而不是就地截断：截断把上一次启动的现场一并抹掉，而崩溃之后要看的正是那一段。
void RotateOnDisk(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path backup = path;
    backup += ".1";
    fs::remove(backup, ec);
    ec.clear();
    fs::rename(path, backup, ec);
    if (ec) {
        // 改名失败多半是 .1 正被人打开着看；此时宁可退回截断，也不能让文件继续涨下去。
        std::error_code ignored;
        fs::resize_file(path, 0, ignored);
    }
}

FILE* OpenLogStream(const std::filesystem::path& path) {
#if defined(_WIN32)
    // _wfopen_s opens with exclusive sharing, which locks the log file for the whole
    // service lifetime — the diagnostics app, a support bundle, or a plain `type` all
    // fail with a sharing violation while the service runs. _wfsopen with _SH_DENYWR
    // still keeps other writers out, but lets readers in.
    return _wfsopen(path.wstring().c_str(), L"ab", _SH_DENYWR);
#else
    return fopen(path.string().c_str(), "ab");
#endif
}

#if defined(_WIN32)

// 级别不走项目的 config 系统：那套东西活在服务进程里，而日志要在服务起来之前、
// 以及在 hal 下那些没有 config 的 host 进程里同样可用。一个纯文本 ini 谁都读得到。
std::wstring LoggingIniPath() {
    // 走 SHGetKnownFolderPath 而不是读 %ProgramData%：服务在 SYSTEM 下跑，
    // 环境块由 SCM 继承，拿不准里面有什么。
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &raw))) {
        if (raw) CoTaskMemFree(raw);
        return {};
    }
    std::wstring path(raw);
    CoTaskMemFree(raw);
    return path + L"\\OpenEGoHub\\logging.ini";
}

std::uint64_t FileStamp(const std::wstring& path) {
    if (path.empty()) {
        return 0;
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    ULARGE_INTEGER stamp{};
    stamp.LowPart = data.ftLastWriteTime.dwLowDateTime;
    stamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return stamp.QuadPart;
}

LogLevel ReadLevelFromIni(const std::wstring& path) {
    if (path.empty()) {
        return LogLevel::Info;
    }
    wchar_t buffer[32]{};
    GetPrivateProfileStringW(L"log", L"level", L"info", buffer,
                             static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])),
                             path.c_str());
    return ParseLevelName(buffer);
}

// 调用方必须持有 g_logMutex。
void RefreshLevelLocked(bool force) {
    const std::uint64_t now = GetTickCount64();
    if (!force && g_lastCheckTick != 0 && now - g_lastCheckTick < kRecheckIntervalMs) {
        return;
    }
    g_lastCheckTick = now;

    const std::uint64_t stamp = FileStamp(g_iniPath);
    if (!force && stamp == g_iniStamp) {
        return;
    }
    g_iniStamp = stamp;
    Logger::SetMinLevel(ReadLevelFromIni(g_iniPath));
}

#else

std::wstring LoggingIniPath() { return {}; }
void RefreshLevelLocked(bool) {}

#endif

} // namespace

void Logger::Init(const std::string& loggerName, const std::filesystem::path& logDir, std::shared_ptr<GuiLogSink> extraSink) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_initialized) {
        return;
    }

    try {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(logDir, ec);
        if (ec) {
            fprintf(stderr, "Failed to create log directory: %s\n", ec.message().c_str());
            return;
        }

        g_logPath = logDir / (loggerName + ".txt");

        g_iniPath = LoggingIniPath();
        g_iniStamp = 0;
        g_lastCheckTick = 0;
        RefreshLevelLocked(true);

        ec.clear();
        if (fs::exists(g_logPath, ec) && !ec) {
            ec.clear();
            const auto size = fs::file_size(g_logPath, ec);
            if (!ec && size > kMaxLogBytes) {
                RotateOnDisk(g_logPath);
            }
        }

        g_fileStream = OpenLogStream(g_logPath);
        g_initialized = (g_fileStream != nullptr);

        if (g_initialized) {
            const char* separator = "========================================\n";
            fwrite(separator, 1, strlen(separator), g_fileStream);
            fflush(g_fileStream);
        } else {
            fprintf(stderr, "Failed to open log file: %s\n", g_logPath.string().c_str());
        }

    } catch (const std::exception& ex) {
        fprintf(stderr, "Log initialization failed: %s\n", ex.what());
    }
}

void Logger::Shutdown() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_initialized) {
        return;
    }

    if (g_fileStream) {
        fclose(g_fileStream);
        g_fileStream = nullptr;
    }
    g_initialized = false;
}

void Logger::SetMinLevel(LogLevel level) {
    detail::g_logMinLevel.store(static_cast<int>(level), std::memory_order_relaxed);
}

void Logger::RefreshMinLevelFromFile() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    detail::g_logLevelPollBudget.store(kPollBudget, std::memory_order_relaxed);
    RefreshLevelLocked(false);
}

const wchar_t* Logger::MinLevelName() {
    const int current = detail::g_logMinLevel.load(std::memory_order_relaxed);
    for (const auto& entry : kLevelNames) {
        if (static_cast<int>(entry.level) == current) {
            return entry.wide;
        }
    }
    return L"info";
}

void Logger::Log(const char* level, const char* layer, const char* method, const char* state, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_initialized) {
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm_now{};
    localtime_s(&tm_now, &time_t_now);

    std::string timeStr = MiniFmt::format(
        "{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}",
        tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
        tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
        static_cast<int>(ms.count())
    );

    std::string formatted_line = MiniFmt::format(
        "[{}] [{:<7}] [{}] [{}] [{}] {}",
        timeStr, level, layer, method, state, message
    );

    if (g_fileStream) {
        fwrite(formatted_line.c_str(), 1, formatted_line.size(), g_fileStream);
        fputc('\n', g_fileStream);
        fflush(g_fileStream);

        const long written = ftell(g_fileStream);
        if (written > 0 && static_cast<std::uintmax_t>(written) > kMaxLogBytes) {
            fclose(g_fileStream);
            g_fileStream = nullptr;
            RotateOnDisk(g_logPath);
            g_fileStream = OpenLogStream(g_logPath);
            g_initialized = (g_fileStream != nullptr);
        }
    }

#if !defined(NDEBUG)
    fprintf(stderr, "%s\n", formatted_line.c_str());
#endif

    GuiLogSink::Instance()->PushRaw(formatted_line);
}

} // namespace Common
