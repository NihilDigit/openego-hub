// 笔的常驻宿主。加载 x64 的 PenService.dll，因此是 ARM64EC；把状态发布到共享内存、
// 把离散事件送进命名管道，供原生 ARM64 的调用方消费。
//
// 与 GaokunThpHost 一样，它由上层拉起，同时等待停止事件与上层进程句柄：上层崩溃时宿主
// 自行退出，不会留下一个占着 MCU 通道的孤儿。

#include "PenChannelLayout.h"
#include "PenService.h"

#include "shared/HostLog.h"

#include <windows.h>

#include <cstdio>
#include <string>

using namespace Gaokun::Pen;

namespace {

// PenService.dll 随 PC Manager 的配件中心安装。这个目录同时是 HuaweiPenEraserService
// 建议删除的那个 Plugins 目录，被清理过就找不到，这时要说清楚原因而不是只报加载失败。
constexpr const wchar_t *kDependSuffix =
    L"\\components\\accessories_center\\accessories_app\\AccessoryApp\\Lib\\Plugins\\Depend";

[[nodiscard]] bool DiscoverDependDirectory(std::wstring &out) noexcept {
    const wchar_t *roots[] = {
        L"C:\\Program Files\\Huawei\\PCManager",
        L"C:\\Program Files (x86)\\Huawei\\PCManager",
    };
    for (const wchar_t *root : roots) {
        std::wstring candidate = std::wstring(root) + kDependSuffix;
        const std::wstring probe = candidate + L"\\PenService.dll";
        if (GetFileAttributesW(probe.c_str()) != INVALID_FILE_ATTRIBUTES) {
            out = candidate;
            return true;
        }
    }
    return false;
}

int RunHosted(DWORD parentPid, const wchar_t *stopEventName, int refreshSeconds, bool verbose) {
    HANDLE stopEvent = nullptr;
    if (stopEventName && *stopEventName) {
        stopEvent = OpenEventW(SYNCHRONIZE, FALSE, stopEventName);
        if (!stopEvent) {
            // 先取 err 再打印：wprintf 自己会调用 Win32，晚一步取到的可能已经是它的错误码。
            const DWORD err = GetLastError();
            HOST_LOG_ERROR("cannot open stop event %ls (err=%lu)", stopEventName, err);
            wprintf(L"cannot open stop event %ls (err=%lu)\n", stopEventName, err);
            return 2;
        }
    }

    HANDLE parent = nullptr;
    if (parentPid != 0) {
        parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
        if (!parent) {
            const DWORD err = GetLastError();
            HOST_LOG_ERROR("cannot open parent process %lu (err=%lu)", parentPid, err);
            wprintf(L"cannot open parent process %lu (err=%lu)\n", parentPid, err);
            return 2;
        }
    }

    Service service;
    if (!service.Start()) {
        const DWORD err = GetLastError();
        HOST_LOG_ERROR("failed to start PenService (err=%lu)", err);
        wprintf(L"failed to start PenService (err=%lu)\n", err);
        return 1;
    }

    Wire::SnapshotWriter snapshots;
    Wire::EventWriter events;
    Wire::CommandReader commands;
    if (!snapshots.Open(Wire::kSnapshotName)) {
        const DWORD err = GetLastError();
        HOST_LOG_ERROR("cannot create the snapshot mapping (err=%lu)", err);
        wprintf(L"cannot create the snapshot mapping (err=%lu)\n", err);
        return 1;
    }
    if (!events.Open(Wire::kEventPipeName)) {
        const DWORD err = GetLastError();
        HOST_LOG_ERROR("cannot create the event pipe (err=%lu)", err);
        wprintf(L"cannot create the event pipe (err=%lu)\n", err);
        return 1;
    }
    if (!commands.Open(Wire::kCommandPipeName)) {
        const DWORD err = GetLastError();
        HOST_LOG_ERROR("cannot create the command pipe (err=%lu)", err);
        wprintf(L"cannot create the command pipe (err=%lu)\n", err);
        return 1;
    }

    HOST_LOG_INFO("hosted and running (parent=%lu)", parentPid);

    service.RequestRefresh();

    HANDLE waits[2];
    DWORD waitCount = 0;
    if (stopEvent) waits[waitCount++] = stopEvent;
    if (parent) waits[waitCount++] = parent;

    // 主循环节奏由事件流决定，不是由刷新周期决定：事件要尽快转出去，而整表刷新只是兜底,
    // 因为多数字段本来就由 MCU 主动推送。
    const DWORD tickMs = 200;
    const int ticksPerRefresh = refreshSeconds * 1000 / static_cast<int>(tickMs);
    // 心跳每秒一次。周期要明显短于服务侧判定宿主失联的窗口，又不必细到每个 tick——
    // 快照本身没变时，发布只是为了让读者看见心跳在动。
    const int ticksPerHeartbeat = 1000 / static_cast<int>(tickMs);
    int tick = 0;
    int sincePublish = 0;
    uint32_t heartbeat = 0;
    Snapshot lastPublished{};
    bool everPublished = false;

    for (;;) {
        if (waitCount > 0) {
            const DWORD result = WaitForMultipleObjects(waitCount, waits, FALSE, tickMs);
            if (result != WAIT_TIMEOUT) {
                // 哪个句柄先亮决定了这次退出是「被要求停」还是「父进程没了」。
                HOST_LOG_INFO("wait returned %lu (%s)", result,
                              (stopEvent && result == WAIT_OBJECT_0) ? "stop event"
                                                                     : "parent exited");
                break;
            }
        } else {
            Sleep(tickMs);
        }

        events.PollForReader();
        commands.PollForWriter();

        Command command{};
        while (commands.Poll(command)) {
            if (static_cast<CommandKind>(command.kind) != CommandKind::SetCurrentFunc ||
                (command.value != 0 && command.value != 1)) {
                if (verbose) {
                    wprintf(L"ignored command kind=%u value=%d\n", command.kind, command.value);
                }
                continue;
            }
            if (verbose) wprintf(L"setting PenCurrentFunc(%d)\n", command.value);
            service.SetCurrentFunc(command.value);
        }

        Event event{};
        while (service.PopEvent(event)) {
            if (verbose) wprintf(L"event kind=%u value=%d\n", event.kind, event.value);
            (void)events.Send(event);
        }

        Snapshot current = service.GetSnapshot();
        const bool changed =
            !everPublished || current.updatedAtUnixMs != lastPublished.updatedAtUnixMs;
        if (changed || ++sincePublish >= ticksPerHeartbeat) {
            sincePublish = 0;
            current.heartbeat = ++heartbeat;
            snapshots.Publish(current);
            lastPublished = current;
            everPublished = true;
            if (verbose && changed) {
                wprintf(L"snapshot flags=0x%08x battery=%u module=%u\n", current.flags,
                        current.battery, current.moduleId);
            }
        }

        if (ticksPerRefresh > 0 && ++tick >= ticksPerRefresh) {
            tick = 0;
            service.RequestRefresh();
        }
    }

    HOST_LOG_INFO("stopping");
    if (stopEvent) CloseHandle(stopEvent);
    if (parent) CloseHandle(parent);
    return 0;
}

// 一次性发一条笔/橡皮切换命令。诊断用：常驻宿主还没有下行通道，而这条命令是否真的
// 让应用看到橡皮，只能在真机上试出来——评论区已有 CSP/SAI 收不到的反例，判据必须是
// 目标应用本身。初始化后停留几秒是为了收下 MCU 的回显事件，那是命令到达的唯一证据。
int RunSetCurrentFunc(int32_t func) {
    Service service;
    if (!service.Start()) {
        wprintf(L"failed to start PenService (err=%lu)\n", GetLastError());
        return 1;
    }
    if (!service.HasCurrentFuncCommand()) {
        wprintf(L"this PenService.dll does not export CommandSendPenCurrentFunc;\n"
                L"pen/eraser switching is unavailable on this installation.\n");
        return 3;
    }

    wprintf(L"sending PenCurrentFunc(%d) -- %ls\n", func, func ? L"eraser" : L"pen");
    service.SetCurrentFunc(func);

    // 命令是异步的，原厂也不等回应。停留 3 秒把回显和随后的快照打出来。
    for (int i = 0; i < 12; ++i) {
        Sleep(250);
        Event event{};
        while (service.PopEvent(event)) {
            wprintf(L"  event kind=%u value=%d\n", event.kind, event.value);
        }
    }
    const Snapshot after = service.GetSnapshot();
    wprintf(L"keyFunc now %u\n", after.keyFunc);
    return 0;
}

// 侧键绑定的功能，取值见 docs/ACCESSORY_CENTER.md 的 PenKeyFunc 表
// （0 截屏 / 1 语音 / 2 白板 / 3 关闭 / 4 橡皮擦 / 5 全局批注）。这个设置存在笔里，
// 掉电也不丢，所以误改之后必须显式改回去。
int RunSetKeyFunc(int32_t func, int watchSeconds) {
    Service service;
    if (!service.Start()) {
        wprintf(L"failed to start PenService (err=%lu)\n", GetLastError());
        return 1;
    }
    wprintf(L"sending SetPenKeyFunc(%d)\n", func);
    service.SetKeyFunc(func);

    // 原厂在守护进程启动时、以及每一次笔连接事件里都补发这条 0x26，我们从来不发。
    // 若 MCU 要在侧键功能被显式设置过之后才对双击产生 0x2F，那这就是缺的一步；发完
    // 之后独占监听，双击能否引出 0x2F 一次就能定死。
    const int ticks = watchSeconds > 0 ? watchSeconds * 4 : 12;
    if (watchSeconds > 0) {
        wprintf(L"watching for %d s -- double-click the pen side button now\n", watchSeconds);
    }
    for (int i = 0; i < ticks; ++i) {
        Sleep(250);
        Event event{};
        while (service.PopEvent(event)) {
            wprintf(L"  event kind=%u value=%d\n", event.kind, event.value);
        }
    }
    service.RequestRefresh();
    Sleep(500);
    wprintf(L"keyFunc now %u\n", service.GetSnapshot().keyFunc);
    return 0;
}

void PrintUsage() {
    wprintf(L"gaokun-penhost -- pen state and events from the vendor MCU channel\n\n"
            L"  --hosted --parent <pid> --stop-event <name>   run under a supervisor\n"
            L"  --console [seconds]                           run standalone and print updates\n"
            L"  --set-current-func <0|1>                      switch the pen to pen(0)/eraser(1)\n"
            L"  --set-key-func <n> [seconds]                  rebind the side button, then watch\n");
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    Gaokun::HostLog::InitFromCommandLine(L"GaokunPenHost", argc, argv);
    HOST_LOG_INFO("starting (pid=%lu)", GetCurrentProcessId());

    std::wstring depend;
    if (!DiscoverDependDirectory(depend)) {
        HOST_LOG_ERROR("PenService.dll not found under <PCManager>%ls; "
                       "the vendor Plugins directory was probably removed",
                       kDependSuffix);
        wprintf(L"PenService.dll not found under <PCManager>%ls\n"
                L"If that directory was removed to disable the vendor pen handling, this\n"
                L"feature is unavailable until it is restored.\n",
                kDependSuffix);
        return 2;
    }
    HOST_LOG_INFO("depend directory: %ls", depend.c_str());
    (void)SetDllDirectoryW(depend.c_str());

    if (argc > 1 && _wcsicmp(argv[1], L"--console") == 0) {
        const int seconds = argc >= 3 ? _wtoi(argv[2]) : 0;
        wprintf(L"[gaokun-penhost] console mode; depend=%ls\n", depend.c_str());
        if (seconds > 0) {
            // 自带超时，便于无人值守地跑一次冒烟而不需要外部去杀进程。
            HANDLE timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(seconds) * 10000000LL;
            (void)SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            const DWORD self = GetCurrentProcessId();
            wchar_t name[64];
            swprintf_s(name, L"GaokunPenHostConsoleStop.%lu", self);
            HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, name);
            // 计时到点后置位停止事件，复用与托管模式相同的退出路径。
            struct Ctx { HANDLE timer; HANDLE stop; };
            static Ctx ctx{timer, stop};
            CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
                WaitForSingleObject(ctx.timer, INFINITE);
                SetEvent(ctx.stop);
                return 0;
            }, nullptr, 0, nullptr);
            return RunHosted(0, name, 5, true);
        }
        return RunHosted(0, nullptr, 5, true);
    }

    if (argc > 2 && _wcsicmp(argv[1], L"--set-current-func") == 0) {
        return RunSetCurrentFunc(_wtoi(argv[2]));
    }

    if (argc > 2 && _wcsicmp(argv[1], L"--set-key-func") == 0) {
        return RunSetKeyFunc(_wtoi(argv[2]), argc > 3 ? _wtoi(argv[3]) : 0);
    }

    if (argc > 1 && _wcsicmp(argv[1], L"--hosted") == 0) {
        DWORD parentPid = 0;
        const wchar_t *stopEvent = nullptr;
        for (int i = 2; i + 1 < argc; i += 2) {
            if (_wcsicmp(argv[i], L"--parent") == 0) {
                parentPid = static_cast<DWORD>(_wtoi(argv[i + 1]));
            } else if (_wcsicmp(argv[i], L"--stop-event") == 0) {
                stopEvent = argv[i + 1];
            }
        }
        return RunHosted(parentPid, stopEvent, 5, false);
    }

    PrintUsage();
    return 1;
}
