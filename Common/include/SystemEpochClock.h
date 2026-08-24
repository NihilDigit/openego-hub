#pragma once
// 采集戳的取法:用 QPC 计数,只在进程启动时与系统时钟对一次表。
//
// 直接每帧调 system_clock::now() 会跟着系统时钟走——校时、夏令时、虚拟机迁移都会
// 让它跳变甚至倒退,而这个戳是拿来量帧间隔的,倒退一次就足以让下游把一条笔画判成
// 两条。QPC 单调,与 epoch 的差值只算一次,之后逐帧累加。
//
// 服务端与工作台各有一份拷贝的时候,两边的锚点会差几百微秒,拼不到一起。所以放在
// 这里共用。

#include <chrono>
#include <cstdint>

#include <windows.h>

namespace Common {

inline uint64_t CaptureSystemEpochUs() {
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value;
    }();
    static const uint64_t qpcAtEpochUs = [] {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        const auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const uint64_t elapsedUs = (static_cast<uint64_t>(counter.QuadPart) * 1000000ull) /
            static_cast<uint64_t>(frequency.QuadPart);
        return static_cast<uint64_t>(nowUs) - elapsedUs;
    }();

    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const uint64_t elapsedUs = (static_cast<uint64_t>(counter.QuadPart) * 1000000ull) /
        static_cast<uint64_t>(frequency.QuadPart);
    return qpcAtEpochUs + elapsedUs;
}

} // namespace Common
