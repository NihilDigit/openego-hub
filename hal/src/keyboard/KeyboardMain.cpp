// 键盘宿主。加载 x64 的 KeyboardService.dll，因此是 ARM64EC。
//
// 一个可执行同时承担两种用法：常驻宿主（--hosted，把状态与事件发布给上层），以及一次性
// 命令行（--detach-support，用于手工查看或改动开关）。两者共用同一份 DLL 封装，不必为了
// 改一个开关而让上层去理解厂商 DLL 的异步模型。

#include "KbdChannelLayout.h"
#include "KeyboardService.h"

#include "shared/HostLog.h"

#include <windows.h>

#include <cstdio>
#include <string>

using namespace Gaokun::Keyboard;

namespace {

// KeyboardService.dll 随 PC Manager 的配件中心安装。这个目录同时是 HuaweiPenEraserService
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
        const std::wstring probe = candidate + L"\\KeyboardService.dll";
        if (GetFileAttributesW(probe.c_str()) != INVALID_FILE_ATTRIBUTES) {
            out = candidate;
            return true;
        }
    }
    return false;
}

int RunHosted(DWORD parentPid, const wchar_t *stopEventName, bool verbose) {
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
        HOST_LOG_ERROR("failed to start KeyboardService (err=%lu)", err);
        wprintf(L"failed to start KeyboardService (err=%lu)\n", err);
        return 1;
    }

    Wire::SnapshotWriter snapshots;
    Wire::EventWriter events;
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

    HOST_LOG_INFO("hosted and running (parent=%lu)", parentPid);

    service.RequestRefresh();

    HANDLE waits[2];
    DWORD waitCount = 0;
    if (stopEvent) waits[waitCount++] = stopEvent;
    if (parent) waits[waitCount++] = parent;

    const DWORD tickMs = 200;
    const int ticksPerRefresh = 5000 / static_cast<int>(tickMs);
    int tick = 0;
    Snapshot lastPublished{};
    bool everPublished = false;

    for (;;) {
        if (waitCount > 0) {
            const DWORD signalled = WaitForMultipleObjects(waitCount, waits, FALSE, tickMs);
            if (signalled != WAIT_TIMEOUT) {
                // 哪个句柄先亮决定了这次退出是「被要求停」还是「父进程没了」。
                HOST_LOG_INFO("wait returned %lu (%s)", signalled,
                              (stopEvent && signalled == WAIT_OBJECT_0) ? "stop event"
                                                                       : "parent exited");
                break;
            }
        } else {
            Sleep(tickMs);
        }

        events.PollForReader();

        Event event{};
        while (service.PopEvent(event)) {
            if (verbose) wprintf(L"event kind=%u value=%d\n", event.kind, event.value);
            (void)events.Send(event);
        }

        const Snapshot current = service.GetSnapshot();
        if (!everPublished || current.updatedAtUnixMs != lastPublished.updatedAtUnixMs) {
            snapshots.Publish(current);
            lastPublished = current;
            everPublished = true;
            if (verbose) {
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

void PrintUsage() {
    wprintf(L"gaokun-keyboard -- keyboard state, events, and the detach switch\n\n"
            L"  --detach-support [enable|disable]            read or change the switch\n"
            L"  --hosted --parent <pid> --stop-event <name>   run under a supervisor\n"
            L"  --console                                     run standalone and print updates\n");
}

int RunDetachSupport(int argc, wchar_t **argv) {
    Service service;
    if (!service.Start()) {
        wprintf(L"failed to start the keyboard service (err=%lu)\n", GetLastError());
        return 1;
    }

    if (argc >= 3) {
        const bool enable = _wcsicmp(argv[2], L"enable") == 0;
        if (!enable && _wcsicmp(argv[2], L"disable") != 0) {
            PrintUsage();
            return 1;
        }
        service.SetDetachSupport(enable);
        // 原厂的 set 不等回应，所以立刻读回来确认，否则无从知道是否真的生效。
        bool now = false;
        if (service.QueryDetachSupport(now)) {
            wprintf(L"detach support: %ls\n", now ? L"enabled" : L"disabled");
            return now == enable ? 0 : 1;
        }
        wprintf(L"command sent, but reading it back timed out\n");
        return 1;
    }

    bool enabled = false;
    if (!service.QueryDetachSupport(enabled)) {
        wprintf(L"timed out waiting for the keyboard MCU\n");
        return 1;
    }
    wprintf(L"detach support: %ls\n", enabled ? L"enabled" : L"disabled");
    return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    Gaokun::HostLog::InitFromCommandLine(L"GaokunKeyboardHost", argc, argv);
    HOST_LOG_INFO("starting (pid=%lu)", GetCurrentProcessId());

    std::wstring depend;
    if (!DiscoverDependDirectory(depend)) {
        HOST_LOG_ERROR("KeyboardService.dll not found under <PCManager>%ls; "
                       "the vendor Plugins directory was probably removed",
                       kDependSuffix);
        wprintf(L"KeyboardService.dll not found under <PCManager>%ls\n"
                L"If that directory was removed to disable the vendor pen handling, this\n"
                L"feature is unavailable until it is restored.\n",
                kDependSuffix);
        return 2;
    }
    HOST_LOG_INFO("depend directory: %ls", depend.c_str());
    (void)SetDllDirectoryW(depend.c_str());

    if (_wcsicmp(argv[1], L"--detach-support") == 0) return RunDetachSupport(argc, argv);

    if (_wcsicmp(argv[1], L"--console") == 0) {
        wprintf(L"[gaokun-keyboard] console mode; depend=%ls\n", depend.c_str());
        return RunHosted(0, nullptr, true);
    }

    if (_wcsicmp(argv[1], L"--hosted") == 0) {
        DWORD parentPid = 0;
        const wchar_t *stopEvent = nullptr;
        for (int i = 2; i + 1 < argc; i += 2) {
            if (_wcsicmp(argv[i], L"--parent") == 0) {
                parentPid = static_cast<DWORD>(_wtoi(argv[i + 1]));
            } else if (_wcsicmp(argv[i], L"--stop-event") == 0) {
                stopEvent = argv[i + 1];
            }
        }
        return RunHosted(parentPid, stopEvent, false);
    }

    PrintUsage();
    return 1;
}
