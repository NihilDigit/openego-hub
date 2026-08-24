#include "TouchProviderCoordinator.h"

#include <utility>

namespace Service {

TouchProviderCoordinator::TouchProviderCoordinator(
        TouchProviderOperations operations,
        StateChanged stateChanged,
        std::chrono::milliseconds leaseTimeout)
    : m_operations(std::move(operations)),
      m_stateChanged(std::move(stateChanged)),
      m_leaseTimeout(leaseTimeout) {}

void TouchProviderCoordinator::Publish(
        PenStatus::TouchProviderState state,
        TouchProviderError error) {
    m_state = state;
    m_error = error;
    if (m_stateChanged) m_stateChanged(state, error);
}

bool TouchProviderCoordinator::AcquireOrRenew(Clock::time_point now) {
    m_hasLease = true;
    m_leaseDeadline = now + m_leaseTimeout;
    if (m_state == PenStatus::TouchProviderState::EGoTouch &&
        m_error == TouchProviderError::None) {
        return true;
    }
    if (SwitchToEGo()) return true;
    m_hasLease = false;
    return false;
}

bool TouchProviderCoordinator::Release() {
    m_hasLease = false;
    return SwitchToHuawei();
}

void TouchProviderCoordinator::Tick(Clock::time_point now) {
    if (!m_hasLease || now < m_leaseDeadline) return;
    m_hasLease = false;
    (void)SwitchToHuawei();
}

void TouchProviderCoordinator::Shutdown() {
    m_hasLease = false;
    (void)SwitchToHuawei();
}

bool TouchProviderCoordinator::SwitchToEGo() {
    Publish(PenStatus::TouchProviderState::SwitchingToEGo);

    if (!m_operations.stopHuawei || !m_operations.stopHuawei()) {
        if (m_operations.restoreHuawei) (void)m_operations.restoreHuawei();
        Publish(PenStatus::TouchProviderState::Huawei,
                TouchProviderError::StopHuaweiFailed);
        return false;
    }

    if (!m_operations.startEGo || !m_operations.startEGo()) {
        const bool restored = m_operations.restoreHuawei && m_operations.restoreHuawei();
        Publish(restored ? PenStatus::TouchProviderState::Huawei
                         : PenStatus::TouchProviderState::Error,
                restored ? TouchProviderError::StartEGoFailed
                         : TouchProviderError::RestoreHuaweiFailed);
        return false;
    }

    // 只有 EGo runtime 已经确认启动后才禁用 Huawei。否则机器会同时失去两个提供方。
    if (!m_operations.disableHuawei || !m_operations.disableHuawei()) {
        if (m_operations.stopEGo) (void)m_operations.stopEGo();
        const bool restored = m_operations.restoreHuawei && m_operations.restoreHuawei();
        Publish(restored ? PenStatus::TouchProviderState::Huawei
                         : PenStatus::TouchProviderState::Error,
                restored ? TouchProviderError::DisableHuaweiFailed
                         : TouchProviderError::RestoreHuaweiFailed);
        return false;
    }

    Publish(PenStatus::TouchProviderState::EGoTouch);
    return true;
}

bool TouchProviderCoordinator::SwitchToHuawei() {
    if (m_state == PenStatus::TouchProviderState::Huawei &&
        m_error == TouchProviderError::None) {
        return true;
    }

    Publish(PenStatus::TouchProviderState::SwitchingToHuawei);

    if (!m_operations.stopEGo || !m_operations.stopEGo()) {
        Publish(PenStatus::TouchProviderState::EGoTouch,
                TouchProviderError::StopEGoFailed);
        return false;
    }

    if (m_operations.restoreHuawei && m_operations.restoreHuawei()) {
        Publish(PenStatus::TouchProviderState::Huawei);
        return true;
    }

    // Huawei 起不来时尝试恢复刚刚停止的 EGo，宁可保持自研触控也不能留下无触控状态。
    const bool egoRestored = m_operations.startEGo && m_operations.startEGo();
    if (egoRestored && m_operations.disableHuawei) {
        (void)m_operations.disableHuawei();
    }
    Publish(egoRestored ? PenStatus::TouchProviderState::EGoTouch
                        : PenStatus::TouchProviderState::Error,
            egoRestored ? TouchProviderError::RestoreHuaweiFailed
                        : TouchProviderError::RollbackEGoFailed);
    return false;
}

} // namespace Service
