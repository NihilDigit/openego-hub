#include "TouchProviderCoordinator.h"

#include <algorithm>
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

TouchProviderCoordinator::Clock::time_point TouchProviderCoordinator::ReadClock() const {
    return m_operations.now ? m_operations.now() : Clock::now();
}

bool TouchProviderCoordinator::AcquireOrRenew(Clock::time_point now) {
    // 挂起期间只延租，不启动宿主，也不因停止失败留下的错误状态重试切换。
    if (m_suspended) {
        m_hasLease = true;
        m_leaseDeadline = now + m_leaseTimeout;
        return true;
    }
    // 纯续租：不做任何切换，调用本身是瞬时的，租期从 now 起算就是对的。
    if (m_state == PenStatus::TouchProviderState::EGoTouch &&
        m_error == TouchProviderError::None) {
        m_hasLease = true;
        m_leaseDeadline = now + m_leaseTimeout;
        return true;
    }
    // 冷却期内不再接管。托盘并不知道宿主刚刚连续崩过，它只会照常每秒续租一次；这里不挡住
    // 的话每一次续租都会重演一遍停原厂、起宿主、宿主又崩的循环。
    if (now < m_egoCooldownUntil) {
        m_hasLease = false;
        return false;
    }
    // 租期由 SwitchToEGo 在切换完成之后自己锚定，这里不能再按 now 算：这一行往下是同步跑
    // 完整个切换，实测占住 4.9 秒，按 now 算的话租约在接管成功的那一刻就已经过期了。
    if (SwitchToEGo()) {
        m_hasLease = true;
        return true;
    }
    m_hasLease = false;
    m_egoCooldownUntil = std::max(now, ReadClock()) + kAcquireFailureCooldown;
    return false;
}

bool TouchProviderCoordinator::Release() {
    m_hasLease = false;
    m_suspended = false;
    // 托盘主动交还，下一次取租约是全新的一轮，不该背着上一轮的崩溃计数和冷却。
    m_restartsInWindow = 0;
    m_restartWindowStart = {};
    m_egoCooldownUntil = {};
    return SwitchToHuawei();
}

void TouchProviderCoordinator::OnSuspend(Clock::time_point now) {
    if (!m_hasLease || m_state != PenStatus::TouchProviderState::EGoTouch) return;

    m_suspended = true;
    m_suspendedAt = now;
    // 只停宿主，让原厂保持停止。否则亮屏接管还要再等原厂内部数秒的清理。
    if (m_operations.stopEGo && m_operations.stopEGo()) {
        Publish(PenStatus::TouchProviderState::EGoSuspended);
        return;
    }
    // 停止未确认时走完整交还兜底，挂起标志保留，亮屏后仍要重新接管。
    if (!SwitchToHuawei()) {
        Publish(PenStatus::TouchProviderState::EGoTouch,
                TouchProviderError::StopEGoFailed);
    }
}

void TouchProviderCoordinator::OnResume(Clock::time_point now) {
    // 短息屏可能未到 ServiceHost 的防抖时限，宿主未停也要给托盘心跳宽限。
    if (!m_suspended) {
        if (m_hasLease) {
            m_leaseDeadline = std::max(m_leaseDeadline, now + kResumeLeaseGrace);
        }
        return;
    }

    m_suspended = false;
    if (!m_hasLease) {
        (void)SwitchToHuawei();
        return;
    }

    // 挂起时若 stopEGo 失败，状态仍停在 EGoTouch；而真实 StartEGoTouchProvider 会把
    // AlreadyRunning 当成功，直接进 SwitchToEGo 会复用断电后已失效的旧宿主。必须先确认
    // 旧宿主已停，再拉起新宿主。
    if (m_state == PenStatus::TouchProviderState::EGoTouch) {
        if (!m_operations.stopEGo || !m_operations.stopEGo()) {
            m_hasLease = false;
            m_egoCooldownUntil = std::max(now, ReadClock()) + kAcquireFailureCooldown;
            Publish(PenStatus::TouchProviderState::EGoTouch,
                    TouchProviderError::StopEGoFailed);
            return;
        }
        Publish(PenStatus::TouchProviderState::EGoSuspended);
    }

    // 不走普通续租短路或崩溃重启预算。原厂已停时 stopHuawei 是空操作；失败则由
    // SwitchToEGo 自带的 restoreHuawei 回退恢复提供方。
    if (SwitchToEGo()) {
        m_hasLease = true;
        m_leaseDeadline = std::max(m_leaseDeadline,
                                   std::max(now, ReadClock()) + kResumeLeaseGrace);
    } else {
        m_hasLease = false;
        m_egoCooldownUntil = std::max(now, ReadClock()) + kAcquireFailureCooldown;
    }
}

void TouchProviderCoordinator::Tick(Clock::time_point now) {
    // 用户会话可能被冻结、时钟可能跨过整个睡眠。不得误判租约过期并启动原厂，
    // 也不探活主动停掉的宿主；亮屏后重新锚定租期并给予心跳宽限。
    if (m_suspended) return;

    if (m_hasLease && now >= m_leaseDeadline) {
        m_hasLease = false;
        (void)SwitchToHuawei();
        return;
    }

    // 看护宿主的判据是「原厂正被我们停着」，不是「我们有没有租约」：只要处在那个状态，
    // 我们就欠着一次恢复，必须持续确认宿主活着。用租约当判据的后果是租约超时那条路先把
    // m_hasLease 置 false 再切换，这次切换若失败并回滚成 EGoTouch/RestoreHuaweiFailed，
    // 宿主就跑在一个再没人问 egoAlive 的状态里，而托盘此时通常已经不在了。
    //
    // 交还原厂之后只剩 EGoTouch 一个状态满足这个判据，与错误码无关：EGoTouch 带着
    // StopEGoFailed 或 RestoreHuaweiFailed 时，恰恰是原厂没起来、宿主还在跑。
    if (m_state != PenStatus::TouchProviderState::EGoTouch) return;

    // 启动时那 1500 ms 的等待只能排除「起来就退」，之后崩溃没有任何人会发现，触控就此
    // 消失且不会回退到原厂。
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

    // 重启不管用：窗口内崩太多次，或这一次连起都起不来。交还原厂并进入冷却。预算用尽时
    // SwitchToEGo 根本没被调用过，状态还停在 EGoTouch，此时原厂仍被我们停着，必须请回来。
    if (m_state == PenStatus::TouchProviderState::EGoTouch ||
        m_state == PenStatus::TouchProviderState::EGoSuspended) {
        (void)SwitchToHuawei();
    }
    m_hasLease = false;
    m_egoCooldownUntil = std::max(now, ReadClock()) + kRestartCooldown;
    // 状态保留 SwitchToHuawei/SwitchToEGo 落到的那个，错误码换成根因：托盘要显示的是宿主
    // 死了，而不是回退路径上顺带记下的那一条。
    Publish(m_state, TouchProviderError::EGoHostDied);
}

void TouchProviderCoordinator::Shutdown(bool systemShutdown) {
    m_hasLease = false;
    m_suspended = false;

    // 关机时只把设备交出来，不请原厂回来。它是 AUTO_START，下次开机自己会起；而此刻
    // StartService 必然返回 ERROR_SHUTDOWN_IN_PROGRESS，等它是纯粹的白等——实测正常停止
    // 要 6331 毫秒，其中一大半花在这一趟上，而 STOP_PENDING 只报了 5000 毫秒 hint。
    // 超时之后 SCM 直接终止进程，终止点可能正落在「原厂已停、宿主也已停」之间，那正是
    // 开机后没有触控的来源。
    //
    // 关机时托盘也在退出，不再发布中间状态。
    if (systemShutdown) {
        if (m_operations.stopEGo) (void)m_operations.stopEGo();
        return;
    }

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

    // 租期在这里锚定，不在调用方进入时锚定。上面这三步是同步的，实测一次接管占住控制
    // 线程 4888~5129 毫秒，而租期就是 5000 毫秒：按进入时刻算的话，接管成功的同一拍
    // Tick 立刻判定租约过期，把刚拿到的触控又交还回去，真机日志上是宿主起停各两次、白折腾
    // 15 秒。就地重启走的也是这里，重启同样要花掉一整个租期，同样必须重新锚定。
    m_leaseDeadline = ReadClock() + m_leaseTimeout;
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
