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

    // 崩溃恢复策略：5s → 10s → 30s 重启，24h 重置计数器
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
