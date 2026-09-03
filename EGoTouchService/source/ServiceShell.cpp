#include "ServiceShell.h"
#include "SystemStateMonitor.h"
#include "ServiceHost.h"
#include "Logger.h"

#include <powersetting.h>

#include <atomic>
#include <thread>

namespace Service {

struct ServiceShell::Impl {
    SERVICE_STATUS_HANDLE statusHandle = nullptr;
    SERVICE_STATUS status{};
    HANDLE stopEvent = nullptr;
    ServiceHost host;

    // 初始化跑在后台。SvcMain 停止时等 initDone 而不是 join 那个后台线程：句柄追踪
    // 抓到过一次死锁——std::thread 的线程句柄值在初始化早期被进程内某处 CloseHandle
    // 关掉，随后被 SystemStateMonitor 的 CreateEvent 复用成一个自动重置事件，std::thread
    // 对象仍缓存旧值，join 于是 WaitForSingleObject 到那个服务运行期永不触发的事件上，
    // 永久卡死，安装升级停不掉旧服务。initDone 是我们独占、绝不外泄的手动重置事件，
    // 后台线程创建后立即 detach，谁也不再 join 那个会被复用的句柄。
    HANDLE initDone = nullptr;
    std::atomic<bool> startFailed{false};

    // PBT power setting notification handles
    HPOWERNOTIFY hDisplayNotify = nullptr;
    HPOWERNOTIFY hLidNotify = nullptr;
    HPOWERNOTIFY hSuspendNotify = nullptr;
};

static ServiceShell s_instance;

ServiceShell::ServiceShell()
    : m_impl(std::make_unique<Impl>()) {}

ServiceShell::~ServiceShell() = default;

ServiceShell* ServiceShell::Instance() {
    return &s_instance;
}

// ─── SCM 模式 ────────────────────────────────

void WINAPI ServiceShell::SvcMain(DWORD argc, LPWSTR* argv) {
    auto* s = Instance();

    s->m_impl->statusHandle = RegisterServiceCtrlHandlerExW(
        kServiceName, SvcCtrlHandlerEx, s);
    if (!s->m_impl->statusHandle) {
        LOG_ERROR("Service", __func__, "Boot", "RegisterServiceCtrlHandlerExW failed.");
        return;
    }

    s->ReportStatus(SERVICE_START_PENDING, 3000);
    s->m_impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // 先报 RUNNING，再在后台做初始化。整条启动路径要停华为服务、起三个 hal 宿主、读一次
    // WMI，谁都可能花上十几秒；SCM 在此期间不会把控制码送进来，安装程序的 StartServices
    // 就停在「正在启动服务」，用户看到的是装不上，而不是启动慢。
    //
    // 更硬的理由是 SCM 重入：启动路径上的 Release() 会去 StartServiceW 拉起 HuaweiThpService，
    // 而本服务自己还是 START_PENDING，等于在 SCM 里嵌套一次服务操作，有死锁窗口。
    s->ReportStatus(SERVICE_RUNNING);

    // 手动重置：一旦初始化线程置位就保持，SvcMain 无论何时来等都读得到。
    s->m_impl->initDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    std::thread([s] {
        LOG_INFO("Service", "SvcMain", "Boot", "Starting modules...");
        if (!s->m_impl->host.Start()) {
            LOG_ERROR("Service", "SvcMain", "Boot", "ServiceHost::Start() failed; stopping.");
            s->m_impl->startFailed.store(true, std::memory_order_release);
            s->SignalShutdownTransportAndStop();
        } else {
            s->RegisterPowerNotifications();
            LOG_INFO("Service", "SvcMain", "Running", "All modules started.");
        }
        // 成败都置位：SvcMain 收尾要等初始化真正结束，才能安全地 host.Stop()。
        if (s->m_impl->initDone) SetEvent(s->m_impl->initDone);
    }).detach();  // 立即放弃线程句柄，此刻它还有效，detach 干净关闭；见 Impl 注释。

    LOG_INFO("Service", __func__, "Running", "Service is running. Waiting for stop signal...");
    s->WaitForStop();

    // 停止信号可能在初始化还没跑完时到达。等 initDone 而非 join：ServiceHost::Start 与
    // Stop 经 ServiceLifecycleStateMachine 串行化，Stop 会等在飞的 Start 走完再回滚，但
    // 这里必须先确认 Start 那条后台线程已跑到头，否则 host.Stop 与它并发拆同一批对象。
    if (s->m_impl->initDone) {
        WaitForSingleObject(s->m_impl->initDone, INFINITE);
    }
    s->CloseStopEvent();
    if (s->m_impl->initDone) {
        CloseHandle(s->m_impl->initDone);
        s->m_impl->initDone = nullptr;
    }

    s->UnregisterPowerNotifications();
    s->m_impl->host.Stop();

    // 启动失败要报非零退出码。一律 NO_ERROR 会让 SCM 把失败当作干净停止，
    // ServiceEntry 配的 SC_ACTION_RESTART（5s/10s/30s）于是永远不触发。
    //
    // 光有非零退出码还不够：默认只有「进程没报 SERVICE_STOPPED 就没了」才算失败，带错误码
    // 的正常停止同样不触发恢复动作。这条路能走通依赖 ServiceEntry 里那个
    // SERVICE_CONFIG_FAILURE_ACTIONS_FLAG，两处要一起看。
    if (s->m_impl->startFailed.load(std::memory_order_acquire)) {
        const auto phase = s->m_impl->host.LastFailedPhase();
        LOG_ERROR("Service", __func__, "Stopped",
                  "Service stopped after a failed start (phase={}).",
                  static_cast<unsigned>(phase));
        s->ReportStatus(SERVICE_STOPPED, 0, ERROR_SERVICE_SPECIFIC_ERROR,
                        static_cast<DWORD>(phase));
        return;
    }

    s->ReportStatus(SERVICE_STOPPED);
    LOG_INFO("Service", __func__, "Stopped", "Service stopped.");
}

DWORD WINAPI ServiceShell::SvcCtrlHandlerEx(
        DWORD ctrl, DWORD evtType, LPVOID evtData, LPVOID ctx) {
    auto* s = static_cast<ServiceShell*>(ctx);

    auto signalEvent = [](Host::SystemStateNamedEventId id) {
        return Host::SystemStateMonitor::SignalNamedEvent(id);
    };

    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
    case SERVICE_CONTROL_PRESHUTDOWN:
        LOG_INFO("Service", __func__, "Stopping", "Received stop/shutdown control code={}.", ctrl);
        // 关机与普通停止要分开：关机时把原厂请回来是白等，它下次开机自己会起。
        if (ctrl == SERVICE_CONTROL_SHUTDOWN || ctrl == SERVICE_CONTROL_PRESHUTDOWN) {
            s->m_impl->host.NoteSystemShutdown();
        }
        s->SignalShutdownTransportAndStop();
        // 实测一次正常停止要 6331 毫秒——停宿主 4.4 秒，交还原厂又几秒——而这里原先只报
        // 5000 毫秒。报少了 SCM 会在服务还在交还的中途认定它没响应。
        s->ReportStatus(SERVICE_STOP_PENDING, 20000);
        return NO_ERROR;

    case SERVICE_CONTROL_POWEREVENT: {
        if (evtType == PBT_APMSUSPEND) {
            LOG_INFO("Service", __func__, "Power", "PBT_APMSUSPEND");
            signalEvent(Host::SystemStateNamedEventId::PbtApmSuspend);
            return NO_ERROR;
        }

        if (evtType == PBT_APMRESUMEAUTOMATIC) {
            LOG_INFO("Service", __func__, "Power", "PBT_APMRESUMEAUTOMATIC");
            signalEvent(Host::SystemStateNamedEventId::PbtApmResumeAutomatic);
            return NO_ERROR;
        }

        if (evtType == PBT_APMRESUMESUSPEND) {
            LOG_INFO("Service", __func__, "Power", "PBT_APMRESUMESUSPEND");
            signalEvent(Host::SystemStateNamedEventId::PbtApmResumeSuspend);
            return NO_ERROR;
        }

        if (evtType != PBT_POWERSETTINGCHANGE || !evtData)
            return NO_ERROR;
        auto* pbs = static_cast<POWERBROADCAST_SETTING*>(evtData);

        // GUID_CONSOLE_DISPLAY_STATE: 0=off, 1=on, 2=dimmed
        static const GUID kDisplayGuid =
            {0x6fe69556, 0x704a, 0x47a0,
             {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};
        // GUID_LIDSWITCH_STATE_CHANGE
        static const GUID kLidGuid =
            {0xba3e0f4d, 0xb817, 0x4094,
             {0xa2, 0xd1, 0xd5, 0x63, 0x79, 0xe6, 0xa0, 0xf3}};

        if (pbs->PowerSetting == kDisplayGuid && pbs->DataLength >= 4) {
            DWORD state = *reinterpret_cast<DWORD*>(pbs->Data);
            LOG_INFO("Service", __func__, "Power", "GUID_CONSOLE_DISPLAY_STATE = {}", state);
            if (state >= 1) {
                signalEvent(Host::SystemStateNamedEventId::MonitorConsoleDisplayOn);
            } else {
                signalEvent(Host::SystemStateNamedEventId::MonitorConsoleDisplayOff);
            }
        }
        else if (pbs->PowerSetting == kLidGuid && pbs->DataLength >= 4) {
            DWORD state = *reinterpret_cast<DWORD*>(pbs->Data);
            LOG_INFO("Service", __func__, "Power", "GUID_LIDSWITCH_STATE = {} (1=open, 0=closed)", state);
            if (state == 1) {
                signalEvent(Host::SystemStateNamedEventId::MonitorLidOn);
            } else {
                signalEvent(Host::SystemStateNamedEventId::MonitorLidOff);
            }
        }
        return NO_ERROR;
    }

    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// ─── 控制台模式 ──────────────────────────────

#if EGOTOUCH_SERVICE_ENABLE_IPC
BOOL WINAPI ServiceShell::ConsoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        Instance()->SignalShutdownTransportAndStop();
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

void ServiceShell::SignalShutdownTransportAndStop() noexcept {
    Host::SystemStateMonitor::SignalNamedEvent(Host::SystemStateNamedEventId::MonitorShutDown);
    if (m_impl != nullptr && m_impl->stopEvent != nullptr) {
        SetEvent(m_impl->stopEvent);
    }
}

#if EGOTOUCH_SERVICE_ENABLE_IPC
void ServiceShell::RunAsConsole() {
    m_impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    SetConsoleCtrlHandler(&ServiceShell::ConsoleCtrlHandler, TRUE);

    LOG_INFO("Service", __func__, "Boot", "Starting modules (console mode)...");
    if (!m_impl->host.Start()) {
        LOG_ERROR("Service", __func__, "Boot", "ServiceHost::Start() failed.");
        return;
    }

    LOG_INFO("Service", __func__, "Running", "Service running in console mode. Press Ctrl+C to stop.");
    WaitForStop();
    CloseStopEvent();
    m_impl->host.Stop();
    LOG_INFO("Service", __func__, "Stopped", "Console mode stopped.");
}
#endif

// ─── 辅助 ────────────────────────────────────

void ServiceShell::ReportStatus(DWORD state, DWORD waitHint,
                                DWORD win32Exit, DWORD specificExit) {
    if (!m_impl->statusHandle) return;

    m_impl->status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    m_impl->status.dwCurrentState = state;
    m_impl->status.dwWin32ExitCode = win32Exit;
    // 只有 dwWin32ExitCode 为 ERROR_SERVICE_SPECIFIC_ERROR 时 SCM 才读这一项。
    m_impl->status.dwServiceSpecificExitCode = specificExit;
    m_impl->status.dwWaitHint = waitHint;

    if (state == SERVICE_START_PENDING) {
        m_impl->status.dwControlsAccepted = 0;
    } else {
        m_impl->status.dwControlsAccepted =
            SERVICE_ACCEPT_STOP |
            SERVICE_ACCEPT_SHUTDOWN |
            SERVICE_ACCEPT_PRESHUTDOWN |
            SERVICE_ACCEPT_POWEREVENT;
    }

    static DWORD checkPoint = 1;
    if (state == SERVICE_RUNNING || state == SERVICE_STOPPED) {
        m_impl->status.dwCheckPoint = 0;
    } else {
        m_impl->status.dwCheckPoint = checkPoint++;
    }

    SetServiceStatus(m_impl->statusHandle, &m_impl->status);
}

// 等待期间不关句柄。初始化线程失败时也会来置这个事件，而它可能与 SCM 送来的 STOP 撞在
// 一起；等醒来就关掉，另一边的 SetEvent 就打在一个已经回收、可能已被复用的句柄上。
// 关闭推迟到初始化线程 join 之后。
void ServiceShell::WaitForStop() {
    if (m_impl->stopEvent) {
        WaitForSingleObject(m_impl->stopEvent, INFINITE);
    }
}

void ServiceShell::CloseStopEvent() {
    if (m_impl->stopEvent) {
        CloseHandle(m_impl->stopEvent);
        m_impl->stopEvent = nullptr;
    }
}

void ServiceShell::RegisterPowerNotifications() {
    if (!m_impl->statusHandle) return;

    // GUID_CONSOLE_DISPLAY_STATE
    static const GUID kDisplayGuid =
        {0x6fe69556, 0x704a, 0x47a0,
         {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};
    // GUID_LIDSWITCH_STATE_CHANGE
    static const GUID kLidGuid =
        {0xba3e0f4d, 0xb817, 0x4094,
         {0xa2, 0xd1, 0xd5, 0x63, 0x79, 0xe6, 0xa0, 0xf3}};
    // GUID_SYSTEM_AWAYMODE (away mode / connected standby)
    static const GUID kAwayGuid =
        {0x98a7f580, 0x01f7, 0x48aa,
         {0x9c, 0x0f, 0x44, 0x35, 0x2c, 0x29, 0xe5, 0xc0}};

    m_impl->hDisplayNotify = RegisterPowerSettingNotification(
        m_impl->statusHandle, &kDisplayGuid, DEVICE_NOTIFY_SERVICE_HANDLE);
    m_impl->hLidNotify = RegisterPowerSettingNotification(
        m_impl->statusHandle, &kLidGuid, DEVICE_NOTIFY_SERVICE_HANDLE);
    m_impl->hSuspendNotify = RegisterPowerSettingNotification(
        m_impl->statusHandle, &kAwayGuid, DEVICE_NOTIFY_SERVICE_HANDLE);

    LOG_INFO("Service", __func__, "Power", "Registered PBT notifications (display={}, lid={}, away={}).", m_impl->hDisplayNotify != nullptr, m_impl->hLidNotify != nullptr, m_impl->hSuspendNotify != nullptr);
}

void ServiceShell::UnregisterPowerNotifications() {
    if (m_impl->hDisplayNotify) {
        UnregisterPowerSettingNotification(m_impl->hDisplayNotify);
        m_impl->hDisplayNotify = nullptr;
    }
    if (m_impl->hLidNotify) {
        UnregisterPowerSettingNotification(m_impl->hLidNotify);
        m_impl->hLidNotify = nullptr;
    }
    if (m_impl->hSuspendNotify) {
        UnregisterPowerSettingNotification(m_impl->hSuspendNotify);
        m_impl->hSuspendNotify = nullptr;
    }
}

} // namespace Service

