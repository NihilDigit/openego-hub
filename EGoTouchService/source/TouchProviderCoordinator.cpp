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
    // 冷却期内不再接管。托盘并不知道宿主刚刚连续崩过，它只会照常每秒续租一次；这里不挡住
    // 的话每一次续租都会重演一遍停原厂、起宿主、宿主又崩的循环。
    if (now < m_egoCooldownUntil) {
        m_hasLease = false;
        return false;
    }
    if (SwitchToEGo()) return true;
    m_hasLease = false;
    return false;
}

bool TouchProviderCoordinator::Release() {
    m_hasLease = false;
    // 托盘主动交还，下一次取租约是全新的一轮，不该背着上一轮的崩溃计数和冷却。
    m_restartsInWindow = 0;
    m_restartWindowStart = {};
    m_egoCooldownUntil = {};
    return SwitchToHuawei();
}

void TouchProviderCoordinator::Tick(Clock::time_point now) {
    if (!m_hasLease) return;

    if (now >= m_leaseDeadline) {
        m_hasLease = false;
        (void)SwitchToHuawei();
        return;
    }

    // 租约还在有效期内，但宿主可能已经死了。启动时那 1500 ms 的等待只能排除「起来就退」，
    // 之后崩溃没有任何人会发现，触控就此消失且不会回退到原厂。
    if (m_state != PenStatus::TouchProviderState::EGoTouch ||
        m_error != TouchProviderError::None) {
        return;
    }
    if (!m_operations.egoAlive || m_operations.egoAlive()) return;

    HandleEGoHostDeath(now);
}

void TouchProviderCoordinator::HandleEGoHostDeath(Clock::time_point now) {
    if (m_restartWindowStart == Clock::time_point{} ||
        now - m_restartWindowStart > kRestartWindow) {
        m_restartWindowStart = now;
        m_restartsInWindow = 0;
    }
    ++m_restartsInWindow;

    // 先就地重启。原厂链偶发崩溃重启就能回来，而交还原厂服务要停掉再起一整个服务，代价
    // 大得多，触控中断也更久。SwitchToEGo 自带回退，重启失败时它已经把原厂请回来了。
    if (m_restartsInWindow <= kMaxRestartsPerWindow && SwitchToEGo()) return;

    // 重启不管用：窗口内崩太多次，或这一次连起都起不来。交还原厂并进入冷却。
    if (m_state == PenStatus::TouchProviderState::EGoTouch) {
        (void)SwitchToHuawei();
    }
    m_hasLease = false;
    m_egoCooldownUntil = now + kRestartCooldown;
    // 状态保留 SwitchToHuawei/SwitchToEGo 落到的那个，错误码换成根因：托盘要显示的是宿主
    // 死了，而不是回退路径上顺带记下的那一条。
    Publish(m_state, TouchProviderError::EGoHostDied);
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
