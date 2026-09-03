/// EGoTouchService 入口点
/// 双模式启动：Windows SCM 服务 或 --console 控制台调试
/// 管理命令：--install / --uninstall

#include "ServiceEntry.h"
#include "ServiceShell.h"
#include "VendorServices.h"
#include "Logger.h"
#if EGOTOUCH_SERVICE_ENABLE_IPC
#include "GuiLogSink.h"
#endif

// ── 服务自注册 / 自卸载 ──────────────────────────────────

static bool EnsureDataDirectory() {
    CreateDirectoryW(L"C:\\ProgramData\\OpenEGoHub", nullptr);
    CreateDirectoryW(L"C:\\ProgramData\\OpenEGoHub\\logs", nullptr);
    return true;
}

#if EGOTOUCH_SERVICE_ENABLE_IPC
static bool InstallService() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        wprintf(L"[ERROR] OpenSCManager failed (err=%lu). Run as Administrator.\n",
                GetLastError());
        return false;
    }

    SC_HANDLE svc = CreateServiceW(
        scm,
        Service::kServiceName,
        L"OpenEGo Hub 触控服务",
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,          // 开机自启
        SERVICE_ERROR_NORMAL,
        exePath,
        nullptr, nullptr, nullptr, nullptr, nullptr);

    if (!svc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            wprintf(L"[WARN] Service already exists.\n");
        } else {
            wprintf(L"[ERROR] CreateService failed (err=%lu).\n", err);
        }
        CloseServiceHandle(scm);
        return err == ERROR_SERVICE_EXISTS;
    }

    // 崩溃恢复策略：5s → 10s → 30s 重启，24h 重置计数器。三条不等于「只重启三次」：
    // 第 N 次失败取数组第 N-1 项，N 超出数组长度时重复最后一项，所以第四次以后一直是
    // 30 秒重启。这一条是服务在接管态崩掉之后触控唯一的自动出路——实测服务被强杀后
    // 5.2 秒被拉回、7.5 秒恢复接管，没有它就是 90 秒内两个提供方都不在跑。
    SC_ACTION actions[3] = {
        { SC_ACTION_RESTART, 5000 },
        { SC_ACTION_RESTART, 10000 },
        { SC_ACTION_RESTART, 30000 },
    };
    SERVICE_FAILURE_ACTIONSW failCfg{};
    failCfg.dwResetPeriod = 86400;  // 24h
    failCfg.cActions = 3;
    failCfg.lpsaActions = actions;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &failCfg);

    // 默认只有「进程没报 SERVICE_STOPPED 就没了」才算失败。启动失败走的是另一条路：
    // ServiceShell 报 SERVICE_STOPPED 并带上 ERROR_SERVICE_SPECIFIC_ERROR，那在默认设置下
    // 是一次干净停止，上面配的重启一次都不会触发。置位之后带非零退出码的停止同样算失败。
    SERVICE_FAILURE_ACTIONS_FLAG failFlag{};
    failFlag.fFailureActionsOnNonCrashFailures = TRUE;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &failFlag);

    // 服务描述
    SERVICE_DESCRIPTIONW desc{};
    desc.lpDescription = const_cast<wchar_t*>(
        L"OpenEGo Hub 触控服务 — 管理触控提供方的接管与交还，并汇总笔与键盘的状态。");
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    EnsureDataDirectory();

    wprintf(L"[OK] Service installed successfully.\n");
    wprintf(L"     Start: sc start %s\n", Service::kServiceName);
    return true;
}

static bool UninstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        wprintf(L"[ERROR] OpenSCManager failed (err=%lu). Run as Administrator.\n",
                GetLastError());
        return false;
    }

    SC_HANDLE svc = OpenServiceW(scm, Service::kServiceName,
                                 SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!svc) {
        wprintf(L"[ERROR] OpenService failed (err=%lu).\n", GetLastError());
        CloseServiceHandle(scm);
        return false;
    }

    // 尝试停止
    SERVICE_STATUS status{};
    ControlService(svc, SERVICE_CONTROL_STOP, &status);
    // 等待停止（最多 10 秒）
    for (int i = 0; i < 20; ++i) {
        QueryServiceStatus(svc, &status);
        if (status.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(500);
    }

    BOOL ok = DeleteService(svc);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    // 卸载要把华为的后台服务一并还回去。禁用状态记在注册表里，本服务删掉之后没有任何人
    // 会再去恢复它们，用户拿到的是一台既没有 OpenEGo Hub 也没有原厂后台的机器。
    //
    // TODO: 登录自启项还没有走这条路，卸载之后它们留在备份键里不还回去，PC Manager 的配件
    // 中心从此静默不再自启。原因是那份记录在 HKCU 而这里跑在 LocalSystem 下，读到的是
    // SYSTEM 自己的 hive；托盘那半边在卸载流程里也不会被调用。可行的做法是让安装包在卸载时
    // 以用户身份跑一次托盘（WiX 里 LaunchTray 已经是 Impersonate="yes" 的现成例子），给它
    // 加一个只做恢复就退出的命令行开关。在那之前 README 里写明要在卸载前自己关掉开关。
    if (!Service::VendorServices::RestoreAll()) {
        wprintf(L"[WARN] Some vendor services could not be restored.\n");
    } else {
        wprintf(L"[OK] Vendor services restored.\n");
    }

    if (ok) {
        wprintf(L"[OK] Service uninstalled.\n");
    } else {
        wprintf(L"[ERROR] DeleteService failed (err=%lu).\n", GetLastError());
    }
    return ok != FALSE;
}
#endif

#if defined(_DEBUG)
// 开启内核的进程句柄追踪（每次 open/close 记录调用栈，dump 里用 !htrace 查询）。
// ProcessHandleTracing 是未公开的信息类，没有 SDK 头，按 ntdll 的实际契约声明。
void EnableProcessHandleTracing() {
    struct HandleTracingEnable {
        ULONG flags = 0;  // 必须为 0
    };
    using NtSetInformationProcessFn =
        LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
    constexpr ULONG kProcessHandleTracing = 32;

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    const auto setInfo = reinterpret_cast<NtSetInformationProcessFn>(
        GetProcAddress(ntdll, "NtSetInformationProcess"));
    if (!setInfo) return;

    HandleTracingEnable enable{};
    const LONG status = setInfo(GetCurrentProcess(), kProcessHandleTracing,
                                &enable, sizeof(enable));
    // 失败只损失追踪能力，不影响服务本体，不必告警到用户可见的层面。
    if (status < 0) {
        OutputDebugStringW(L"OpenEGoHub: handle tracing not enabled\n");
    }
}
#endif

// ── 主入口 seam ─────────────────────────────────────────

class ProductionServiceEntryActions final : public Service::IServiceEntryActions {
public:
#if EGOTOUCH_SERVICE_ENABLE_IPC
    bool InstallService() override { return ::InstallService(); }
    bool UninstallService() override { return ::UninstallService(); }
#endif

    void InitializeServiceProcess() override {
        // Hide console window — logs are forwarded to App via IPC GetLogs
        if (HWND hw = GetConsoleWindow()) ShowWindow(hw, SW_HIDE);

#if defined(_DEBUG)
        // 句柄开关追踪。实机上抓到过一次 STOP 卡死：initThread 的句柄被进程内某处多关了
        // 一次、值被复用，join 等在别人的对象上。静态审计没有找到双关点，所以让内核记下
        // 每次 open/close 的调用栈，再出一次事就在 dump 里 !htrace <值> 直接看是谁关的。
        // 只开在 Debug：追踪对每次句柄操作抓栈，Release 不背这个开销。
        EnableProcessHandleTracing();
#endif

        EnsureDataDirectory();

        // 触控数据路径不在本进程里：它跑在 GaokunThpHost 中，那边由厂商代码自己设
        // REALTIME。本进程只是监督器，轮询快照、看住宿主，给它实时优先级不会让笔更跟手，
        // 反而会在开机那一段把 SCM 和 WMI 的线程压下去，放大启动期的竞争。
        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

        // Debug builds register as a separate service (OpenEGoHubServiceDebug, see
        // scripts/install_debug_service.bat) and can run alongside an installed Release
        // service. They must not share a log file: the writer holds it with _SH_DENYWR,
        // so whichever process starts second would silently lose all logging.
#if defined(_DEBUG)
        constexpr const char* kLoggerName = "OpenEGoHubServiceDebug";
#else
        constexpr const char* kLoggerName = "OpenEGoHubService";
#endif
#if EGOTOUCH_SERVICE_ENABLE_IPC
        Common::Logger::Init(kLoggerName, "C:/ProgramData/OpenEGoHub/logs/",
                              Common::GuiLogSink::Instance());
#else
        Common::Logger::Init(kLoggerName, "C:/ProgramData/OpenEGoHub/logs/", nullptr);
#endif

        LOG_INFO("Service", __func__, "Boot", "Process priority set to ABOVE_NORMAL_PRIORITY_CLASS.");
    }

#if EGOTOUCH_SERVICE_ENABLE_IPC
    void RunConsole() override {
        Service::ServiceShell::Instance()->RunAsConsole();
    }
#endif

    bool StartScmDispatcher() override {
        SERVICE_TABLE_ENTRYW table[] = {
            { const_cast<wchar_t*>(Service::kServiceName),
              Service::ServiceShell::SvcMain },
            { nullptr, nullptr }
        };
        return StartServiceCtrlDispatcherW(table) != FALSE;
    }

    DWORD LastErrorCode() const override {
        return GetLastError();
    }
};

int wmain(int argc, wchar_t* argv[]) {
    ProductionServiceEntryActions actions;
    const int result = Service::ServiceEntryMain(argc, argv, actions);
    Common::Logger::Shutdown();
    return result;
}
