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
};

struct TouchProviderOperations {
    std::function<bool()> stopHuawei;
    std::function<bool()> disableHuawei;
    std::function<bool()> startEGo;
    std::function<bool()> stopEGo;
    std::function<bool()> restoreHuawei;
};

// 串行执行 EGoTouch ↔ HuaweiTHP 的事务式切换，并持有托盘租约。类本身不碰 SCM，
// 因而所有成功、失败和回滚分支都能用纯单元测试覆盖；生产操作由 ServiceHost 注入。
class TouchProviderCoordinator {
public:
    using Clock = std::chrono::steady_clock;
    using StateChanged = std::function<void(PenStatus::TouchProviderState, TouchProviderError)>;

    TouchProviderCoordinator(TouchProviderOperations operations,
                             StateChanged stateChanged,
                             std::chrono::milliseconds leaseTimeout);

    bool AcquireOrRenew(Clock::time_point now);
    bool Release();
    void Tick(Clock::time_point now);
    void Shutdown();

    [[nodiscard]] PenStatus::TouchProviderState State() const noexcept { return m_state; }
    [[nodiscard]] TouchProviderError Error() const noexcept { return m_error; }
    [[nodiscard]] bool HasLease() const noexcept { return m_hasLease; }

private:
    bool SwitchToEGo();
    bool SwitchToHuawei();
    void Publish(PenStatus::TouchProviderState state,
                 TouchProviderError error = TouchProviderError::None);

    TouchProviderOperations m_operations;
    StateChanged m_stateChanged;
    std::chrono::milliseconds m_leaseTimeout;
    Clock::time_point m_leaseDeadline{};
    PenStatus::TouchProviderState m_state = PenStatus::TouchProviderState::Unknown;
    TouchProviderError m_error = TouchProviderError::None;
    bool m_hasLease = false;
};

} // namespace Service
