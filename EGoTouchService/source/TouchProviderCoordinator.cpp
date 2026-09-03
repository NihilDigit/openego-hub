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
    // 纯续租：不做任何切换，调用本身是瞬时的，租期从 now 起算就是对的。
    if (m_state == PenStatus::TouchProviderState::EGoTouch &&
        m_error == TouchProviderError::None) {
        m_hasLease = true;
        m_leaseDeadline = now + m_leaseTimeout;
        return true;
    }
    // 挂起期间照常延租，但不把宿主拉起来。息屏并不冻结托盘，它每秒一次的续租会立刻重起
    // 刚刚停下的宿主，代管也就等于没做过。重新接管的时机由 OnResume 决定。
    if (m_suspended) {
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
    m_egoCooldownUntil = now + kAcquireFailureCooldown;
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

void TouchProviderCoordinator::OnSuspend(Clock::time_point) {
    if (!m_hasLease || m_state != PenStatus::TouchProviderState::EGoTouch) return;

    // 停宿主是必须的：面板断电之后它内部已经死了而进程还在，唤醒瞬间服务日志报
    // running=1, exit=-1，egoAlive 看不出来。
    //
    // 停完之后要不要把原厂请回来，原先的理由是「机器正在睡，起原厂只是让它跟着一起睡，
    // 醒来还要再停一次」。实测推翻了这个理由：HuaweiThpService 启动 323/325/341 毫秒，
    // 停止 1640/3350/3368 毫秒，代价是 0.32 秒起、几秒停；换掉的却是一个没有任何看门狗
    // 的零提供方状态——Tick 遇到它直接 return，AcquireOrRenew 也直接 return，退出它只有
    // OnResume 一条路，真机上息屏一次就是 68 秒两个提供方都不在跑，Resume 那一拍要是没到
    // 就无限期停在那里。所以这里走完整的交还，对外发布的状态是真实的 Huawei。
    m_suspended = true;
    (void)SwitchToHuawei();
}

void TouchProviderCoordinator::OnResume(Clock::time_point now) {
    // 息屏与唤醒之间隔得极短，OnSuspend 还没动手（ServiceHost 那边有防抖）。这次唤醒对
    // 协调器来说什么都没发生，租约照旧由托盘的心跳维持。
    if (!m_suspended) return;

    m_suspended = false;
    // 睡眠期间托盘没了，租约已经在 Tick 里过期。触控留在原厂手里，等托盘自己回来再要。
    if (!m_hasLease) return;

    // 唤醒后的重新接管与普通接管没有区别：同一套冷却、同一套回退（SwitchToEGo 失败时它
    // 自己已经把原厂请回来了）。不再走 HandleEGoHostDeath——那是给「跑着跑着崩掉」准备的
    // 重启预算，而这里根本还没有宿主在跑。
    if (!AcquireOrRenew(now)) return;
    m_leaseDeadline = std::max(m_leaseDeadline, now + kResumeLeaseGrace);
}

void TouchProviderCoordinator::Tick(Clock::time_point now) {
    if (m_hasLease && now >= m_leaseDeadline) {
        m_hasLease = false;
        // 挂起期间也照常放行：此刻触控本来就在原厂手里，SwitchToHuawei 是空操作，而标志
        // 留着不清的话唤醒后 AcquireOrRenew 会一直走「挂起中只延租不接管」那条短路，永远
        // 接管不回来。
        m_suspended = false;
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

    // 挂起期间不看护。正常情况下这里根本到不了（交还原厂之后状态是 Huawei），到得了只有
    // 一种：OnSuspend 的交还失败了，宿主还在跑。那时面板已经断电，宿主必定报死，就地重启
    // 只会在机器睡着的时候白白烧掉重启预算，而唤醒时 OnResume 本来就要重新接管一次。
    if (m_suspended) return;

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
    if (m_state == PenStatus::TouchProviderState::EGoTouch) {
        (void)SwitchToHuawei();
    }
    m_hasLease = false;
    m_egoCooldownUntil = now + kRestartCooldown;
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
    // 这里不发布状态：宿主已停而原厂未起，没有哪个枚举值说的是这件事，而托盘此刻也在退出，
    // 发布一个不准确的状态不如什么都不说。
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
