#include "ThpConfig.h"
#include "ThpModule.h"
#include "VendorPath.h"

#include "shared/HostLog.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

// 原厂在 InitializeComponent 里把 ServiceName 设为 HUAWEIThpService，而 SCM 中注册的名字
// 是 HuaweiThpService。大小写不一致但服务名不区分大小写，照抄以免留下无谓的差异。
constexpr const wchar_t *kServiceName = L"HUAWEIThpService";
// 与 kServiceName 同一个服务，另立一个常量是因为两处的角色不同：kServiceName 是本进程
// 作为原厂服务的替身向 SCM 注册时用的名字，这一个是托管模式下要操作的那个原厂服务。
// 拼写照抄 VendorPath.cpp，两处指的是 SCM 里注册的同一条记录。
constexpr const wchar_t *kVendorServiceName = L"HuaweiThpService";
constexpr const wchar_t *kEventSource = L"THPEvent";
constexpr const wchar_t *kEventLogName = L"THPLog";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
Thp::Module g_module;
bool g_consoleMode = false;

void WriteEventLog(WORD type, const wchar_t *text) noexcept {
    HANDLE source = RegisterEventSourceW(nullptr, kEventSource);
    if (!source) return;
    const wchar_t *strings[] = {text};
    (void)ReportEventW(source, type, 0, 0, nullptr, 1, 0, strings, nullptr);
    (void)DeregisterEventSource(source);
}

// 等价于 .NET 的 EventLog.CreateEventSource(THPEvent, THPLog)。没有它，ReportEvent 写出的
// 记录在事件查看器里显示为「找不到事件源」。
void EnsureEventSource() noexcept {
    wchar_t path[512];
    swprintf_s(path, L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\%s\\%s",
               kEventLogName, kEventSource);

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }

    const wchar_t *messageFile = L"%SystemRoot%\\System32\\EventCreate.exe";
    (void)RegSetValueExW(key, L"EventMessageFile", 0, REG_EXPAND_SZ,
                         reinterpret_cast<const BYTE *>(messageFile),
                         static_cast<DWORD>((wcslen(messageFile) + 1) * sizeof(wchar_t)));
    const DWORD types = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_INFORMATION_TYPE;
    (void)RegSetValueExW(key, L"TypesSupported", 0, REG_DWORD,
                         reinterpret_cast<const BYTE *>(&types), sizeof(types));
    RegCloseKey(key);
}

// 四个回调。原厂把委托存在静态字段里防止 GC 回收；这里是普通函数，地址天然稳定。

int __cdecl PrintEventLogCallback(int) noexcept {
    // 原厂取回消息后不做任何处理就返回。消息内容不落盘，但这次调用本身是必要的：
    // GetMESSAGE 会把 DLL 内部的待发消息取走，不调用则队列不前进。
    char buffer[Thp::kMessageLength]{};
    int length = Thp::kMessageLength;
    g_module.GetMessage(buffer, &length);
    return 0;
}

int __cdecl EventLogStatusCallback(int) noexcept {
    // 原厂返回静态字段 eventStatus，该字段从未被赋值，恒为 0。
    return 0;
}

int __cdecl SetPenEleValueCallback(int value) noexcept {
    return Thp::ConfigStore::Instance().SetPenEleValue(value);
}

int __cdecl GetPenEleValueCallback(int index) noexcept {
    return Thp::ConfigStore::Instance().GetPenEleValue(index);
}

void RegisterCallbacks() noexcept {
    // 注册顺序与原厂 FunctionRegister 一致：PrintEventLog、EventLogStatus、
    // SetPenEleValue、GetPenEleValue。
    g_module.RegisterPrintEventLog(&PrintEventLogCallback);
    g_module.RegisterEventLogStatus(&EventLogStatusCallback);
    g_module.RegisterSetPenEleValue(&SetPenEleValueCallback);
    g_module.RegisterGetPenEleValue(&GetPenEleValueCallback);
}

void ReportStatus(DWORD state, DWORD exitCode = NO_ERROR) noexcept {
    if (g_consoleMode) return;

    static DWORD checkPoint = 1;

    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = 0;

    // .NET ServiceBase 的默认值决定了这一组：CanStop 为 true，CanHandlePowerEvent 被显式
    // 设为 true，而 CanShutdown 保持默认的 false。所以原厂并不接受 SERVICE_CONTROL_SHUTDOWN，
    // 这里也不声明它——多接受一个控制码会让 SCM 在关机时改走另一条路径。
    g_status.dwControlsAccepted =
        state == SERVICE_START_PENDING ? 0 : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_POWEREVENT);

    g_status.dwCheckPoint =
        (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkPoint++;

    (void)SetServiceStatus(g_statusHandle, &g_status);
}

void StopService() noexcept {
    if (g_module.IsLoaded()) {
        HOST_LOG_INFO("stopping: ThpFuncStop then unload");
        (void)g_module.ThpFuncStop();
        g_module.Unload();
    }
}

DWORD WINAPI ServiceCtrlHandler(DWORD control, DWORD, LPVOID, LPVOID) noexcept {
    switch (control) {
    case SERVICE_CONTROL_STOP:
        ReportStatus(SERVICE_STOP_PENDING);
        StopService();
        ReportStatus(SERVICE_STOPPED);
        return NO_ERROR;

    case SERVICE_CONTROL_POWEREVENT:
        // 原厂声明接受电源事件，但 OnPowerEvent 未被重写，基类默认返回 true 且不做处理。
        // THP_Service.dll 自己通过 RegisterPowerSettingNotification 收电源通知，与这条路径无关。
        return NO_ERROR;

    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;

    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

bool StartThp() noexcept {
    if (!g_module.Load()) {
        WriteEventLog(EVENTLOG_ERROR_TYPE, L"THP_Service.dll could not be loaded.");
        HOST_LOG_ERROR("THP_Service.dll could not be loaded (err=%lu)", GetLastError());
        return false;
    }
    RegisterCallbacks();
    (void)g_module.ThpFuncStart();
    HOST_LOG_INFO("ThpFuncStart called; VHFFunction=%d LogFunction=%d",
                  Thp::ConfigStore::Instance().GetPenEleValue(0),
                  Thp::ConfigStore::Instance().GetPenEleValue(1));
    return true;
}

void WINAPI ServiceMain(DWORD, LPWSTR *) noexcept {
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ServiceCtrlHandler, nullptr);
    if (!g_statusHandle) return;

    ReportStatus(SERVICE_START_PENDING);

    if (!StartThp()) {
        ReportStatus(SERVICE_STOPPED, ERROR_FILE_NOT_FOUND);
        return;
    }

    ReportStatus(SERVICE_RUNNING);
}

// 原厂在构造函数里设实时优先级。触摸采集对调度延迟敏感，这是原厂的选择，照搬。
void ApplyProcessPriority() noexcept {
    (void)SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
}

// 父进程消失后把原厂触控服务请回来，只用于「父进程没了」这条退出来路。
//
// 服务进程非正常消失（崩溃、被强杀）时没有任何人做交还：本进程等的是父进程句柄，父进程
// 一没就退出，此后系统里既没有 OpenEGo 的提供方也没有华为的，触控整个消失。实测杀掉正式
// 服务后：137 毫秒服务没了，2217 毫秒本进程退出，5198 毫秒 SCM 按 SC_ACTION_RESTART 把
// 服务拉回，5777 毫秒华为 RUNNING——中间 5 秒是零提供方，而且补上它的是 SCM 的恢复动作，
// 配额只有 24 小时 3 次。配额用尽之后就是永久无触控：在没有配恢复动作的服务上实测，杀掉
// 之后 90 秒里 svc=STOPPED hw=STOPPED thp=0 一动不动，正是用户报告的「触摸失灵，华为驱动
// 也没启动上」。
//
// 本进程是这条链上最后一个还活着的成员，跑在 LocalSystem 下，有 SCM 权限，所以由它关灯。
// 调用点必须排在 StopService() 之后：ThpFuncStop 并卸载 DLL 才算把设备干净交出来，设备还
// 被我们占着的时候拉起华为，等于让它接手一个中间状态。
void RestoreVendorService() noexcept {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        HOST_LOG_ERROR("restore: OpenSCManager failed (err=%lu)", GetLastError());
        return;
    }

    SC_HANDLE service =
        OpenServiceW(manager, kVendorServiceName, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!service) {
        HOST_LOG_ERROR("restore: cannot open %ls (err=%lu)", kVendorServiceName, GetLastError());
        CloseServiceHandle(manager);
        return;
    }

    if (!StartServiceW(service, 0, nullptr)) {
        const DWORD err = GetLastError();
        // 已经在运行是成功，不是失败：服务侧可能已经先一步把华为拉起来了。
        if (err == ERROR_SERVICE_ALREADY_RUNNING) {
            HOST_LOG_INFO("restore: %ls already running", kVendorServiceName);
        } else {
            HOST_LOG_ERROR("restore: StartService %ls failed (err=%lu)", kVendorServiceName, err);
        }
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return;
    }

    // StartServiceW 只表示请求被受理，还要等它真的进入 RUNNING 才能在日志里断言恢复成功。
    //
    // 超时按实测定：华为服务启动耗时 323/325/341 毫秒，2 秒是它的六倍左右，足够容下一次
    // 明显的抖动。不能再长——这段代码跑在进程退出的最后一段，等待期间本进程一直不消失，
    // 外面看到的是宿主赖着不退。超时也只是不再等，SCM 那边该起还是会起。
    constexpr DWORD kStartTimeoutMs = 2000;
    constexpr DWORD kPollIntervalMs = 50;

    DWORD waited = 0;
    for (;;) {
        SERVICE_STATUS status{};
        if (!QueryServiceStatus(service, &status)) {
            HOST_LOG_ERROR("restore: QueryServiceStatus failed (err=%lu)", GetLastError());
            break;
        }
        if (status.dwCurrentState == SERVICE_RUNNING) {
            HOST_LOG_INFO("restore: %ls running after %lu ms", kVendorServiceName, waited);
            break;
        }
        if (waited >= kStartTimeoutMs) {
            HOST_LOG_ERROR("restore: %ls still in state %lu after %lu ms", kVendorServiceName,
                           status.dwCurrentState, waited);
            break;
        }
        Sleep(kPollIntervalMs);
        waited += kPollIntervalMs;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
}

// 托管模式。OpenEGoHub 拉起本进程接管触控，切走或自身退出时再把设备交还原厂。
//
// 停止有两条来路，都必须走到 ThpFuncStop：父进程主动置位停止事件，或父进程自己没了。
// 后者不能只靠 Job Object 的 KILL_ON_JOB_CLOSE 兜底——那是直接终止，DLL 没有机会复位 AFE，
// 设备会停在中间状态，随后原厂服务接手时要多一次恢复。等父进程句柄把这条路径也收干净：
// 停止事件那条由父进程自己负责交还，父进程没了那条则由 RestoreVendorService 收尾。
int RunHosted(DWORD parentPid, const wchar_t *stopEventName) noexcept {
    HANDLE stopEvent = nullptr;
    if (stopEventName && *stopEventName) {
        // 由父进程创建，这里只打开。名字对不上时视为参数错误，不静默降级成「永不停止」。
        stopEvent = OpenEventW(SYNCHRONIZE, FALSE, stopEventName);
        if (!stopEvent) {
            // 先取 err 再打印：wprintf 自己会调用 Win32，晚一步取到的可能已经是它的错误码。
            const DWORD err = GetLastError();
            HOST_LOG_ERROR("cannot open stop event %ls (err=%lu)", stopEventName, err);
            wprintf(L"[hwthpec] cannot open stop event %ls (err=%lu)\n", stopEventName, err);
            return 2;
        }
    }

    HANDLE parent = nullptr;
    if (parentPid != 0) {
        parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
        if (!parent) {
            const DWORD err = GetLastError();
            HOST_LOG_ERROR("cannot open parent process %lu (err=%lu)", parentPid, err);
            wprintf(L"[hwthpec] cannot open parent process %lu (err=%lu)\n", parentPid, err);
            if (stopEvent) CloseHandle(stopEvent);
            return 2;
        }
    }

    if (!StartThp()) {
        HOST_LOG_ERROR("hosted start failed; exiting with 1");
        if (stopEvent) CloseHandle(stopEvent);
        if (parent) CloseHandle(parent);
        return 1;
    }

    HOST_LOG_INFO("hosted and running (parent=%lu)", parentPid);

    HANDLE waits[2];
    DWORD count = 0;
    if (stopEvent) waits[count++] = stopEvent;
    if (parent) waits[count++] = parent;

    if (count == 0) {
        // 两者都没给，退化成一直运行，只能靠外部终止。允许这种用法便于手工试验。
        for (;;) Sleep(1000);
    }

    const DWORD signalled = WaitForMultipleObjects(count, waits, FALSE, INFINITE);
    // 哪个句柄先亮决定了这次退出是「被要求停」还是「父进程没了」，两者的后续排查方向不同，
    // 也决定了下面要不要替父进程把华为请回来。
    const bool stopRequested = stopEvent && signalled == WAIT_OBJECT_0;
    const char *reason = "parent exited";
    if (stopRequested) reason = "stop event";
    else if (signalled == WAIT_FAILED) reason = "wait failed";
    HOST_LOG_INFO("wait returned %lu (%s)", signalled, reason);

    StopService();

    // 父进程主动要求停止时不插手：服务自己会处理交还，宿主再去启动华为只会和它抢同一个
    // 服务。等待本身失败也走恢复——那说明这条链已经不可靠，宁可把触控还给原厂。
    if (!stopRequested) RestoreVendorService();

    if (stopEvent) CloseHandle(stopEvent);
    if (parent) CloseHandle(parent);
    return 0;
}

int RunConsole() noexcept {
    wprintf(L"[hwthpec] console mode; Ctrl+C to stop\n");
    if (!StartThp()) {
        wprintf(L"[hwthpec] failed to start THP\n");
        return 1;
    }
    wprintf(L"[hwthpec] running. VHFFunction=%d LogFunction=%d\n",
            Thp::ConfigStore::Instance().GetPenEleValue(0),
            Thp::ConfigStore::Instance().GetPenEleValue(1));

    (void)SetConsoleCtrlHandler(
        [](DWORD) -> BOOL {
            StopService();
            ExitProcess(0);
        },
        TRUE);

    for (;;) Sleep(1000);
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    // THP_Service.dll 及其整条依赖链都在原厂安装目录里，而本程序不装到那里，所以必须显式
    // 把搜索路径指过去。原厂是 .NET 服务，CLR 从程序集所在目录解析 P/Invoke 的 DLL，
    // 位置与它一致才不需要这一步。
    Gaokun::HostLog::InitFromCommandLine(L"GaokunThpHost", argc, argv);
    HOST_LOG_INFO("starting (pid=%lu)", GetCurrentProcessId());

    std::wstring vendorDir;
    if (!Thp::DiscoverVendorDirectory(vendorDir)) {
        HOST_LOG_ERROR("cannot locate the vendor install directory; "
                       "is HuaweiThpService registered?");
        wprintf(L"[hwthpec] cannot locate the vendor install directory "
                L"(is HuaweiThpService registered?)\n");
        return 2;
    }
    HOST_LOG_INFO("vendor directory: %ls", vendorDir.c_str());
    (void)SetCurrentDirectoryW(vendorDir.c_str());
    (void)SetDllDirectoryW(vendorDir.c_str());

    ApplyProcessPriority();
    EnsureEventSource();

    if (argc > 1 && _wcsicmp(argv[1], L"--check") == 0) {
        // 只读配置，不加载 THP_Service.dll，因而不碰设备。用来在替换服务之前确认
        // ConfigStore 与原厂读的是同一份文件、解析出同样的值。
        auto &config = Thp::ConfigStore::Instance();
        wprintf(L"config   : %s\n", Thp::ConfigStore::FilePath());
        wprintf(L"VHF (0)  : %d\n", config.GetPenEleValue(0));
        wprintf(L"Log (1)  : %d\n", config.GetPenEleValue(1));
        // 诊断输出一律用 ASCII。控制台代码页是 936 时，wprintf 遇到无法编码的字符会
        // 中断整行输出，后面的内容会静默消失，看起来像程序提前退出。
        wprintf(L"OOB (7)  : %d   (vendor returns 0 for out-of-range)\n",
                config.GetPenEleValue(7));
        return 0;
    }

    if (argc > 1 && _wcsicmp(argv[1], L"--hosted") == 0) {
        g_consoleMode = true; // 不向 SCM 汇报状态：本进程不是服务。
        DWORD parentPid = 0;
        const wchar_t *stopEvent = nullptr;
        for (int i = 2; i + 1 < argc; i += 2) {
            if (_wcsicmp(argv[i], L"--parent") == 0) {
                parentPid = static_cast<DWORD>(_wtoi(argv[i + 1]));
            } else if (_wcsicmp(argv[i], L"--stop-event") == 0) {
                stopEvent = argv[i + 1];
            }
        }
        return RunHosted(parentPid, stopEvent);
    }

    if (argc > 1 && _wcsicmp(argv[1], L"--console") == 0) {
        g_consoleMode = true;
        return RunConsole();
    }

    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kServiceName), ServiceMain},
                                    {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        // 从命令行直接双击运行时会走到这里。给出提示而不是静默退出。
        wprintf(L"[hwthpec] not started by the SCM; use --console to run interactively\n");
        return 1;
    }
    return 0;
}
