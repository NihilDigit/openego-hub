#pragma once

#include "PenStatusChannel.h"

#include <chrono>
#include <cstdint>
#include <functional>

namespace Service {

enum class TouchProviderError : uint8_t {
    None = 0,
    StopHuaweiFailed,
    StartEGoFailed,
    DisableHuaweiFailed,
    StopEGoFailed,
    RestoreHuaweiFailed,
    RollbackEGoFailed,
    EGoHostDied,
};

struct TouchProviderOperations {
    std::function<bool()> stopHuawei;
    std::function<bool()> disableHuawei;
    std::function<bool()> startEGo;
    std::function<bool()> stopEGo;
    std::function<bool()> restoreHuawei;
    // 宿主进程是否还活着。startEGo 只能证明它起来过：宿主启动 1.5 秒之后崩掉的话，状态位
    // 仍停在 EGoTouch，而设备已经没有任何提供方在驱动。留空则退化为原来的「起来即认定
    // 一直在」，Tick 不做存活检查。
    std::function<bool()> egoAlive;
};

// 串行执行 EGoTouch ↔ HuaweiTHP 的事务式切换，并持有托盘租约。类本身不碰 SCM，
// 因而所有成功、失败和回滚分支都能用纯单元测试覆盖；生产操作由 ServiceHost 注入。
class TouchProviderCoordinator {
public:
    using Clock = std::chrono::steady_clock;
    using StateChanged = std::function<void(PenStatus::TouchProviderState, TouchProviderError)>;

    // 宿主反复崩溃时的止损参数。窗口内允许若干次就地重启，超出就交还原厂并进入冷却；
    // 冷却期内拒绝接管，否则托盘每秒一次的续租会把两个提供方来回切换个不停。
    static constexpr std::chrono::seconds kRestartWindow{60};
    static constexpr int kMaxRestartsPerWindow = 3;
    static constexpr std::chrono::seconds kRestartCooldown{30};
    // 接管失败之后的冷却。托盘每秒续租一次，而一次失败的接管要停原厂服务、起宿主、再把
    // 原厂请回来，全程可能十几秒；不冷却的话下一次续租又会从头来一遍，原厂服务被反复
    // 起停，触控在这段时间里时有时无。比崩溃冷却短得多：失败常常是暂时的。
    static constexpr std::chrono::seconds kAcquireFailureCooldown{10};
    // 唤醒后至少给出这么长的租约，与冻结时剩下的时长取大者。托盘的心跳是每秒一次的
    // WM_TIMER，而用户会话恢复得比服务晚；只按剩下的那点时间计时的话，第一拍心跳还没到
    // 租约就过期了，触控白白交还原厂再抢回来一次。
    static constexpr std::chrono::seconds kResumeLeaseGrace{10};

    TouchProviderCoordinator(TouchProviderOperations operations,
                             StateChanged stateChanged,
                             std::chrono::milliseconds leaseTimeout);

    bool AcquireOrRenew(Clock::time_point now);
    bool Release();
    void Tick(Clock::time_point now);
    // 系统挂起与唤醒。宿主里的 THP_Service 自己注册了电源通知，但托管进程没有窗口也没有
    // 消息泵，那些通知一条都收不到：面板断电之后宿主内部已经死了而进程还在，egoAlive 看
    // 不出来。于是由服务代管——挂起时主动干净停掉，唤醒时重启。
    void OnSuspend(Clock::time_point now);
    void OnResume(Clock::time_point now);
    void Shutdown();

    [[nodiscard]] PenStatus::TouchProviderState State() const noexcept { return m_state; }
    [[nodiscard]] TouchProviderError Error() const noexcept { return m_error; }
    [[nodiscard]] bool HasLease() const noexcept { return m_hasLease; }

private:
    bool SwitchToEGo();
    bool SwitchToHuawei();
    void HandleEGoHostDeath(Clock::time_point now);
    void Publish(PenStatus::TouchProviderState state,
                 TouchProviderError error = TouchProviderError::None);

    TouchProviderOperations m_operations;
    StateChanged m_stateChanged;
    std::chrono::milliseconds m_leaseTimeout;
    Clock::time_point m_leaseDeadline{};
    Clock::time_point m_restartWindowStart{};
    Clock::time_point m_egoCooldownUntil{};
    // 挂起时租约还剩多久。存时长而不是 deadline：睡眠期间 steady_clock 走不走由平台决定，
    // 按绝对时刻恢复的话唤醒的一瞬间租约可能已经过期，触控被交还一次谁也没要求的原厂。
    std::chrono::milliseconds m_leaseRemainingOnSuspend{0};
    int m_restartsInWindow = 0;
    PenStatus::TouchProviderState m_state = PenStatus::TouchProviderState::Unknown;
    TouchProviderError m_error = TouchProviderError::None;
    bool m_hasLease = false;
};

} // namespace Service
