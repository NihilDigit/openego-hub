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
    // 读取当前时刻。切换是同步的，实测一次接管占住控制线程 4888~5129 毫秒，而租期就是
    // 5000 毫秒；租期只能从切换完成的那一刻起算，所以切换之后必须再读一次时钟，不能沿用
    // 进入时的那个时刻。留空则用 Clock::now()，单元测试注入可控时钟。
    std::function<std::chrono::steady_clock::time_point()> now;
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
    // 唤醒后的租约下限。唤醒走的是与普通接管相同的路径，租期本来就从宿主真正起来的那一刻
    // 起算，多数情况下比这个下限还长；但服务比用户会话先恢复，托盘那每秒一次的 WM_TIMER
    // 要晚一些才回来，接管快的时候第一拍心跳还没到租约就过期了，触控白白交还原厂再抢一次。
    static constexpr std::chrono::seconds kResumeLeaseGrace{10};

    TouchProviderCoordinator(TouchProviderOperations operations,
                             StateChanged stateChanged,
                             std::chrono::milliseconds leaseTimeout);

    bool AcquireOrRenew(Clock::time_point now);
    bool Release();
    void Tick(Clock::time_point now);
    // 系统挂起与唤醒。宿主里的 THP_Service 自己注册了电源通知，但托管进程没有窗口也没有
    // 消息泵，那些通知一条都收不到：面板断电之后宿主内部已经死了而进程还在，egoAlive 看
    // 不出来（唤醒瞬间服务日志报 running=1, exit=-1）。于是由服务代管——挂起时把触控整个
    // 交还原厂，唤醒时重新接管。
    void OnSuspend(Clock::time_point now);
    void OnResume(Clock::time_point now);
    // systemShutdown 为真表示机器正在关机，此时不把原厂请回来，理由见实现处。
    void Shutdown(bool systemShutdown);

    [[nodiscard]] PenStatus::TouchProviderState State() const noexcept { return m_state; }
    [[nodiscard]] TouchProviderError Error() const noexcept { return m_error; }
    [[nodiscard]] bool HasLease() const noexcept { return m_hasLease; }

private:
    bool SwitchToEGo();
    bool SwitchToHuawei();
    void HandleEGoHostDeath(Clock::time_point now);
    void Publish(PenStatus::TouchProviderState state,
                 TouchProviderError error = TouchProviderError::None);
    [[nodiscard]] Clock::time_point ReadClock() const;

    TouchProviderOperations m_operations;
    StateChanged m_stateChanged;
    std::chrono::milliseconds m_leaseTimeout;
    Clock::time_point m_leaseDeadline{};
    Clock::time_point m_restartWindowStart{};
    Clock::time_point m_egoCooldownUntil{};
    int m_restartsInWindow = 0;
    PenStatus::TouchProviderState m_state = PenStatus::TouchProviderState::Unknown;
    TouchProviderError m_error = TouchProviderError::None;
    bool m_hasLease = false;
    // 系统正在睡，触控已经交还原厂，但租约还在托盘手里、唤醒后要自动恢复接管。这件事只能
    // 用内部标志表达：对外发布的状态必须是真实的 Huawei，托盘看到的才是当下真正在驱动
    // 触控的那一方。挂起期间托盘照常每秒续租一次，这个标志就是拦住那些续租、不让它们把刚
    // 停下的宿主又拉起来的地方。
    bool m_suspended = false;
};

} // namespace Service
