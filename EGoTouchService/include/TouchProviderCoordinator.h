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
    // 唤醒后的租约下限，从重新接管完成起算。服务可能比用户会话先恢复，需要给托盘的
    // WM_TIMER 心跳留出复苏宽限；宽限内仍未续租则正常交还原厂。
    static constexpr std::chrono::seconds kResumeLeaseGrace{10};

    TouchProviderCoordinator(TouchProviderOperations operations,
                             StateChanged stateChanged,
                             std::chrono::milliseconds leaseTimeout);

    bool AcquireOrRenew(Clock::time_point now);
    bool Release();
    void Tick(Clock::time_point now);
    // 系统挂起与唤醒。宿主里的 THP_Service 自己注册了电源通知，但托管进程没有窗口也没有
    // 消息泵，那些通知一条都收不到：面板断电之后宿主内部已经死了而进程还在，egoAlive 看
    // 不出来（唤醒瞬间服务日志报 running=1, exit=-1）。于是由服务代管——挂起时只停宿主，
    // 原厂保持停止，亮屏时直接重启宿主，避免再次等待原厂停止。
    void OnSuspend(Clock::time_point now);
    void OnResume(Clock::time_point now);
    // systemShutdown 为真表示机器正在关机，此时不把原厂请回来，理由见实现处。
    void Shutdown(bool systemShutdown);

    [[nodiscard]] PenStatus::TouchProviderState State() const noexcept { return m_state; }
    [[nodiscard]] TouchProviderError Error() const noexcept { return m_error; }
    [[nodiscard]] bool HasLease() const noexcept { return m_hasLease; }
    [[nodiscard]] bool IsSuspended() const noexcept { return m_suspended; }

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
    Clock::time_point m_suspendedAt{};
    Clock::time_point m_restartWindowStart{};
    Clock::time_point m_egoCooldownUntil{};
    int m_restartsInWindow = 0;
    PenStatus::TouchProviderState m_state = PenStatus::TouchProviderState::Unknown;
    TouchProviderError m_error = TouchProviderError::None;
    bool m_hasLease = false;
    // 挂起期间主动停掉宿主，正常状态为 EGoSuspended；停止失败时可能已回退到 Huawei。
    // 保留租约并阻止续租、Tick 拉起任一提供方，亮屏后由 OnResume 重新接管。
    // ServiceHost 以显示状态兜底，避免单次唤醒事件丢失后永久停在挂起态。
    bool m_suspended = false;
};

} // namespace Service
