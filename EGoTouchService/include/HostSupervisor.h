#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace Service {

// 巡检的结论。调用方按它决定这一轮做什么，重启动作本身留在调用方那边。
enum class HostAction : uint8_t {
    None = 0,
    Restart,        ///< 判定宿主已死且预算允许，去 Stop + Start
    EnterCooldown,  ///< 窗口内重启次数用尽，这一轮起不再重启，只记一条日志
};

// 一个 gaokun-hal 宿主的「应当在运行」意图与重启预算。
//
// 类本身不碰进程、不读共享内存：进程存活与心跳由调用方采样后传进来，重启也由调用方执行。
// 这样每一条判定分支都能用纯单元测试覆盖，与 TouchProviderCoordinator 的分工一致。
//
// 判死有两条判据，缺一不可。进程没了是显然的一条；另一条是心跳——宿主进程还在、而它内部的
// MCU 循环已经停了的时候，快照的 seqlock 停在最后一帧，读者拿到的仍是一份自洽的旧快照，
// 只有心跳分辨得出来。
class HostSupervisor {
public:
    using Clock = std::chrono::steady_clock;

    // 止损参数的形态与 TouchProviderCoordinator 一致：窗口内允许若干次就地重启，超出则
    // 停手一段时间。宿主起不来通常是厂商 DLL 缺失一类不会自己好转的原因，不停手的话
    // 250 毫秒一轮的巡检会把它反复拉起，日志和 CPU 都被这件事占满。
    static constexpr std::chrono::seconds kRestartWindow{60};
    static constexpr int kMaxRestartsPerWindow = 3;
    static constexpr std::chrono::seconds kRestartCooldown{30};
    // 宿主每秒无条件推进一次心跳，这里留五倍余量：巡检周期只有 250 毫秒，宿主偶尔被调度
    // 晚一拍不该判成死亡，而真死了五秒之内也一定判得出来。
    static constexpr std::chrono::seconds kHeartbeatTimeout{5};
    // 刚重启完的宽限期。宿主要先加载厂商 DLL 再建映射，这段时间里读不到心跳是正常的，
    // 不宽限就会在它起来之前又判它一次死。
    static constexpr std::chrono::seconds kStartGrace{5};
    // 宿主报「厂商组件缺失」之后隔多久再探一次。这条失败在用户重装电脑管家之前每次都得到
    // 同一个结果，5 秒一轮只会刷日志——实测 90 分钟刷了 500 多条。取 5 分钟：组件装回来之后
    // 几分钟内能自动恢复，用户不必重启服务，日志量也回到可忽略。
    static constexpr std::chrono::seconds kVendorMissingReprobe{300};

    explicit HostSupervisor(std::string name) : m_name(std::move(name)) {}

    // 宿主该不该在运行。启动失败时置 false，此后巡检不再管它，直到有人重新表达意图。
    void SetDesiredRunning(bool desired, Clock::time_point now) {
        if (m_desired == desired) return;
        m_desired = desired;
        m_healthy = false;
        ResetHeartbeat();
        m_quietUntil = desired ? now + kStartGrace : Clock::time_point{};
        m_windowStart = Clock::time_point{};
        m_restartsInWindow = 0;
        m_vendorMissing = false;
    }

    [[nodiscard]] bool DesiredRunning() const noexcept { return m_desired; }
    // 宿主此刻可用：进程在、心跳在走。转发快照和下发命令都以它为准。
    [[nodiscard]] bool Healthy() const noexcept { return m_healthy; }
    // 宿主起不来的原因是厂商组件缺失，重探节奏因此是 kVendorMissingReprobe 而不是 5 秒。
    // 界面据它把「等它重启」和「去重装电脑管家」分开说。
    [[nodiscard]] bool VendorMissing() const noexcept { return m_vendorMissing; }
    [[nodiscard]] const std::string& Name() const noexcept { return m_name; }

    // 采样一轮。heartbeat 取自宿主快照，快照读不到时传 nullopt。
    HostAction Tick(Clock::time_point now, bool processAlive,
                    std::optional<uint32_t> heartbeat) {
        if (!m_desired) {
            m_healthy = false;
            return HostAction::None;
        }

        UpdateHeartbeat(now, heartbeat);

        // 宽限期内不下重启判断：宿主刚被拉起，心跳还没开始走，这段时间的采样说明不了问题。
        // 可用与否照常按进程存活回答——真没起来的话通道也打不开，上层本来就显示未知，
        // 而多压五秒只会让托盘在服务刚启动的头几秒把键盘那一项灰掉。
        if (now < m_quietUntil) {
            m_healthy = processAlive;
            return HostAction::None;
        }

        const bool heartbeatStale =
            m_heartbeatSeen && now - m_lastHeartbeatChange > kHeartbeatTimeout;
        m_healthy = processAlive && !heartbeatStale;
        if (m_healthy) {
            // 心跳走起来了就说明厂商组件已经装回来，标志自动摘除，不必等谁来复位。
            m_vendorMissing = false;
            return HostAction::None;
        }

        if (m_windowStart == Clock::time_point{} || now - m_windowStart > kRestartWindow) {
            m_windowStart = now;
            m_restartsInWindow = 0;
        }
        if (m_restartsInWindow >= kMaxRestartsPerWindow) {
            m_quietUntil = now + kRestartCooldown;
            m_windowStart = Clock::time_point{};
            m_restartsInWindow = 0;
            return HostAction::EnterCooldown;
        }

        ++m_restartsInWindow;
        return HostAction::Restart;
    }

    // 一次重启的结果。无论成败都进宽限期：起来了要等它建好通道，没起来则等下一轮再试，
    // 两种情况下立刻再判一次都只会得到同一个结论。
    void NoteRestartResult(bool started, Clock::time_point now) {
        m_healthy = false;
        ResetHeartbeat();
        m_quietUntil = now + kStartGrace;
        m_lastRestartFailed = !started;
    }

    [[nodiscard]] bool LastRestartFailed() const noexcept { return m_lastRestartFailed; }

    // 宿主用 kHostExitVendorComponentsMissing 立刻退出。重探由 quietUntil 驱动，Tick 因此
    // 不需要为这个状态单开分支。
    //
    // 重启窗口的计数一并清零：那份预算是为「偶发崩溃重启几次就好」准备的，用尽之后进 30 秒
    // 冷却再回到 5 秒一轮，正是要避开的循环。确定性失败不占用它。
    void NoteVendorMissing(Clock::time_point now) {
        m_vendorMissing = true;
        m_healthy = false;
        ResetHeartbeat();
        m_quietUntil = now + kVendorMissingReprobe;
        m_windowStart = Clock::time_point{};
        m_restartsInWindow = 0;
    }

private:
    void ResetHeartbeat() {
        m_heartbeatSeen = false;
        m_lastHeartbeat = 0;
        m_lastHeartbeatChange = Clock::time_point{};
    }

    void UpdateHeartbeat(Clock::time_point now, std::optional<uint32_t> heartbeat) {
        if (!heartbeat) return;
        if (!m_heartbeatSeen || *heartbeat != m_lastHeartbeat) {
            m_heartbeatSeen = true;
            m_lastHeartbeat = *heartbeat;
            m_lastHeartbeatChange = now;
        }
    }

    std::string m_name;
    bool m_desired = false;
    bool m_healthy = false;
    bool m_heartbeatSeen = false;
    bool m_lastRestartFailed = false;
    bool m_vendorMissing = false;
    uint32_t m_lastHeartbeat = 0;
    Clock::time_point m_lastHeartbeatChange{};
    Clock::time_point m_quietUntil{};
    Clock::time_point m_windowStart{};
    int m_restartsInWindow = 0;
};

} // namespace Service
