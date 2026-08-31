/**
 * @file Logger.h
 * @brief 通用日志模块 (自研轻量级 MiniLogger)
 * @description 零 spdlog 依赖，支持多线程安全、文件持久化与 GuiLogSink 转发。
 * 强制层次化前缀: [Layer][Class::Method][State] Message
 */
#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <filesystem>
#include <type_traits>

namespace MiniFmt {

struct LogArg {
    enum class Type {
        Int,
        UInt,
        LongLong,
        ULongLong,
        Double,
        String,
        Pointer
    };

    Type type;
    union {
        long long i_val;
        unsigned long long u_val;
        double d_val;
        const void* p_val;
    };
    std::string s_val;

    LogArg() : type(Type::Int), i_val(0) {}

    template <typename T, typename std::enable_if<std::is_integral<T>::value && std::is_signed<T>::value && !std::is_same<T, bool>::value, int>::type = 0>
    LogArg(T v) : type(Type::LongLong), i_val(static_cast<long long>(v)) {}

    template <typename T, typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value && !std::is_same<T, bool>::value, int>::type = 0>
    LogArg(T v) : type(Type::ULongLong), u_val(static_cast<unsigned long long>(v)) {}

    LogArg(bool v) : type(Type::Int), i_val(v ? 1 : 0) {}
    LogArg(double v) : type(Type::Double), d_val(v) {}
    LogArg(float v) : type(Type::Double), d_val(static_cast<double>(v)) {}
    LogArg(const char* v) : type(Type::String), s_val(v ? v : "") {}
    LogArg(char* v) : type(Type::String), s_val(v ? v : "") {}
    LogArg(const std::string& v) : type(Type::String), s_val(v) {}
    LogArg(std::string_view v) : type(Type::String), s_val(v) {}

    template <typename T, typename std::enable_if<std::is_pointer<T>::value && !std::is_same<T, const char*>::value && !std::is_same<T, char*>::value, int>::type = 0>
    LogArg(T v) : type(Type::Pointer), p_val(static_cast<const void*>(v)) {}
};

std::string format_core(const char* fmt_str, const LogArg* args, size_t count);

template <typename... Args>
std::string format(const char* fmt_str, const Args&... args) {
    if constexpr (sizeof...(args) == 0) {
        return fmt_str;
    } else {
        LogArg arg_array[sizeof...(args)] = { LogArg(args)... };
        return format_core(fmt_str, arg_array, sizeof...(args));
    }
}

} // namespace MiniFmt



namespace Common {

class GuiLogSink;

// 数值递增即严重度递增，比较用整数而不是级别名：宏在编译期就把级别定成常量，
// 热路径上只剩一次 atomic 读和一次整数比较。
enum class LogLevel : int {
    Trace    = 0,
    Debug    = 1,
    Info     = 2,
    Warn     = 3,
    Error    = 4,
    Critical = 5,
    Off      = 6
};

namespace detail {
// 放在 detail 里而不是 Logger 的成员，是为了让 LevelEnabled 能内联到每个日志点，
// 同时不把可写状态暴露成类的公开接口。
extern std::atomic<int> g_logMinLevel;
// 距离下次查看 ini 还剩多少次日志调用。递减故意不用原子读改写：它只是个节流计数，
// 多线程下丢几次计数只让检查稍早或稍晚发生，真正的频率由文件侧的时间节流兜住。
extern std::atomic<int> g_logLevelPollBudget;
} // namespace detail

class Logger {
public:
    /**
     * @brief 初始化全局日志实例
     * @param loggerName 日志记录器的名称
     * @param logDir 日志存放目录
     */
    static void Init(const std::string& loggerName = "EGoTouch", 
                     const std::filesystem::path& logDir = "C:/ProgramData/OpenEGoHub/logs/",
                     std::shared_ptr<GuiLogSink> extraSink = nullptr);

    /**
     * @brief 关闭并清理日志
     */
    static void Shutdown();

    /**
     * @brief 核心日志输出静态方法
     */
    static void Log(const char* level, const char* layer, const char* method, const char* state, const std::string& message);

    /**
     * @brief 当前级别是否放行
     */
    static bool LevelEnabled(LogLevel level) {
        // 顺带承担 ini 轮询。放在这里而不是 Log() 里：级别一旦设成 off，Log() 再也不会被
        // 调用，改回 info 就永远读不到了。
        const int budget = detail::g_logLevelPollBudget.load(std::memory_order_relaxed);
        if (budget > 0) {
            detail::g_logLevelPollBudget.store(budget - 1, std::memory_order_relaxed);
        } else {
            RefreshMinLevelFromFile();
        }
        return static_cast<int>(level) >= detail::g_logMinLevel.load(std::memory_order_relaxed);
    }

    /**
     * @brief 立刻按 logging.ini 校正一次级别（内部仍有 10 秒时间节流）
     */
    static void RefreshMinLevelFromFile();

    /**
     * @brief 设置最低输出级别，立即对所有线程生效
     */
    static void SetMinLevel(LogLevel level);

    /**
     * @brief 当前最低级别的小写名字（trace/debug/info/warn/error/critical/off）
     * @description 服务把它经 --log-level 透传给 hal 下的 host 进程。
     */
    static const wchar_t* MinLevelName();

    /**
     * @brief 获取底层 logger 桩函数
     */
    static std::shared_ptr<GuiLogSink> Get() { return nullptr; }
};

// ---------------------------------------------------------
// 宏定义：带层次化结构的日志输出
// 格式要求: [Layer][Class::Method][State] Message
// ---------------------------------------------------------

// 底层辅助宏，负责将格式拼接并送入自研 Log 引擎。
// 级别判断放在最前面：格式串拼接（MiniFmt::format）远比一次 atomic 读贵，被过滤掉的日志
// 不应该付这份代价。levelEnum 是 LogLevel 的枚举名，levelStr 是写进文件的那个小写名字。
#define LOG_INTERNAL(levelEnum, levelStr, layer, method, state, msg, ...)          \
    do {                                                                          \
        if (Common::Logger::LevelEnabled(Common::LogLevel::levelEnum)) {          \
            Common::Logger::Log(levelStr, (layer), (method), (state),             \
                                MiniFmt::format(msg __VA_OPT__(,) __VA_ARGS__));  \
        }                                                                         \
    } while (0)

// 暴露给业务层使用的便捷宏。
// TRACE 与 DEBUG 在 Release 下仍然编译期裁掉，它们分布在热路径上；INFO 一律保留，
// 由运行期级别决定是否落盘——Release 的启动日志此前是空的，排障无从下手。
#if defined(NDEBUG)
#define LOG_TRACE(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#else
#define LOG_TRACE(layer, method, state, msg, ...) LOG_INTERNAL(Trace, "trace", layer, method, state, msg __VA_OPT__(,) __VA_ARGS__)
#define LOG_DEBUG(layer, method, state, msg, ...) LOG_INTERNAL(Debug, "debug", layer, method, state, msg __VA_OPT__(,) __VA_ARGS__)
#endif

#define LOG_INFO(layer,  method, state, msg, ...) LOG_INTERNAL(Info,  "info",  layer, method, state, msg __VA_OPT__(,) __VA_ARGS__)
#define LOG_WARN(layer,  method, state, msg, ...) LOG_INTERNAL(Warn,  "warn",  layer, method, state, msg __VA_OPT__(,) __VA_ARGS__)
#define LOG_ERROR(layer, method, state, msg, ...) LOG_INTERNAL(Error, "error", layer, method, state, msg __VA_OPT__(,) __VA_ARGS__)
#define LOG_CRIT(layer,  method, state, msg, ...) LOG_INTERNAL(Critical, "critical", layer, method, state, msg __VA_OPT__(,) __VA_ARGS__)

} // namespace Common
