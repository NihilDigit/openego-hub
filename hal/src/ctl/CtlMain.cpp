// gaokun-ctl：各宿主的统一控制与诊断入口。
//
// 它走的是上层会走的同一条路——链接同一个库、调用同一组控制器与读者，所以既是给不想链接
// 静态库的调用方准备的进程接口，也是这些通道的端到端验证手段。
//
// 原生 ARM64：只启停进程、读共享内存与管道，不接触任何厂商 DLL。

#include "GaokunKeyboard.h"
#include "GaokunPen.h"
#include "GaokunThp.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

[[nodiscard]] std::wstring SiblingPath(const wchar_t *name) {
    wchar_t self[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    std::wstring path(self, length);
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return {};
    return path.substr(0, slash) + L"\\" + name;
}

void PrintUsage() {
    wprintf(L"gaokun-ctl -- drive and inspect the hal hosts\n\n"
            L"  thp  --test <seconds>    take the touch device, hold, hand it back\n"
            L"  pen  --watch <seconds>   pen state and events\n"
            L"  kbd  --watch <seconds>   keyboard state and events\n"
            L"  kbd  --detach-support <enable|disable>\n"
            L"                           drive the resident host over the command pipe\n\n"
            L"thp requires HuaweiThpService to be stopped first: the device can only be\n"
            L"held by one implementation at a time.\n");
}

// ---- thp ----

int RunThp(int seconds) {
    const std::wstring host = SiblingPath(L"GaokunThpHost.exe");
    Gaokun::Thp::HostController controller;
    const auto result = controller.Start(host);
    if (result != Gaokun::Thp::StartResult::Started && result != Gaokun::Thp::StartResult::AlreadyRunning) {
        wprintf(L"start failed (result=%d, exit=%d)\n", static_cast<int>(result),
                controller.ExitCode());
        if (result == Gaokun::Thp::StartResult::ExitedImmediately) {
            wprintf(L"  is HuaweiThpService still running?\n");
        }
        return 1;
    }
    wprintf(L"host started; touch and pen should work now, holding %d s\n", seconds);
    Sleep(static_cast<DWORD>(seconds) * 1000);

    const bool clean = controller.Stop();
    wprintf(L"stopped: %ls (exit=%d)\n", clean ? L"clean" : L"terminated", controller.ExitCode());
    return clean ? 0 : 1;
}

// ---- pen ----

const wchar_t *PenKindName(uint32_t kind) {
    using K = Gaokun::Pen::EventKind;
    switch (static_cast<K>(kind)) {
    case K::ConnectRequest: return L"ConnectRequest";
    case K::ConnectResult: return L"ConnectResult";
    case K::KeyFuncChanged: return L"KeyFuncChanged";
    case K::CurrentFunc: return L"CurrentFunc";
    case K::BatteryReminder: return L"BatteryReminder";
    case K::DeviationReminder: return L"DeviationReminder";
    case K::CloseConnectWindow: return L"CloseConnectWindow";
    case K::TransferPenMode: return L"TransferPenMode";
    default: return L"None";
    }
}

void PrintPen(const Gaokun::Pen::Snapshot &s) {
    using F = Gaokun::Pen::Flag;
    const auto has = [&](F f) { return (s.flags & static_cast<uint32_t>(f)) != 0; };
    wprintf(L"  flags      0x%08x\n", s.flags);
    wprintf(L"  heartbeat  %u\n", s.heartbeat);
    if (has(F::HasConnected)) wprintf(L"  connected  %ls\n", has(F::Connected) ? L"yes" : L"no");
    if (has(F::HasBattery)) wprintf(L"  battery    %u%%\n", s.battery);
    if (has(F::HasCharging)) wprintf(L"  charging   %ls\n", has(F::Charging) ? L"yes" : L"no");
    if (has(F::HasModule)) wprintf(L"  module     %u\n", s.moduleId);
    if (has(F::HasKeySupport)) wprintf(L"  keySupport 0x%02x\n", s.keySupport);
    if (has(F::HasKeyFunc)) wprintf(L"  keyFunc    %u\n", s.keyFunc);
    if (has(F::HasFirmware)) wprintf(L"  firmware   %hs\n", s.firmware);
    if (has(F::HasHardware)) wprintf(L"  hardware   %hs\n", s.hardware);
    if (has(F::HasSerial)) wprintf(L"  serial     %hs\n", s.serial);
}

int RunPen(int seconds) {
    Gaokun::Pen::HostController controller;
    const auto result = controller.Start(SiblingPath(L"GaokunPenHost.exe"));
    if (result != Gaokun::Pen::StartResult::Started &&
        result != Gaokun::Pen::StartResult::AlreadyRunning) {
        wprintf(L"start failed (result=%d, exit=%d)\n", static_cast<int>(result),
                controller.ExitCode());
        return 1;
    }
    wprintf(L"host started\n");

    // 宿主要先建好映射与管道，读者才连得上，所以退让几次而不是一次失败就放弃。
    Gaokun::Pen::SnapshotReader snapshots;
    for (int i = 0; i < 20 && !snapshots.Open(); ++i) Sleep(200);
    Gaokun::Pen::EventReader events;
    for (int i = 0; i < 20 && !events.Open(); ++i) Sleep(200);

    const DWORD deadline = GetTickCount() + static_cast<DWORD>(seconds) * 1000;
    uint64_t last = 0;
    while (GetTickCount() < deadline) {
        Gaokun::Pen::Snapshot snapshot{};
        if (snapshots.Read(snapshot) && snapshot.updatedAtUnixMs != last) {
            last = snapshot.updatedAtUnixMs;
            wprintf(L"snapshot:\n");
            PrintPen(snapshot);
        }
        Gaokun::Pen::Event event{};
        while (events.Poll(event)) {
            wprintf(L"event %ls value=%d\n", PenKindName(event.kind), event.value);
        }
        Sleep(250);
    }

    const bool clean = controller.Stop();
    wprintf(L"stopped: %ls (exit=%d)\n", clean ? L"clean" : L"terminated", controller.ExitCode());
    return clean ? 0 : 1;
}

// ---- kbd ----

const wchar_t *KbdKindName(uint32_t kind) {
    using K = Gaokun::Keyboard::EventKind;
    switch (static_cast<K>(kind)) {
    case K::ConnectRequest: return L"ConnectRequest";
    case K::ConnectResult: return L"ConnectResult";
    case K::DetachChanged: return L"DetachChanged";
    case K::DetachSupportChanged: return L"DetachSupportChanged";
    case K::FirstBatteryAfterConnect: return L"FirstBatteryAfterConnect";
    case K::DetachSupportResult: return L"DetachSupportResult";
    default: return L"None";
    }
}

void PrintKbd(const Gaokun::Keyboard::Snapshot &s) {
    using F = Gaokun::Keyboard::Flag;
    const auto has = [&](F f) { return (s.flags & static_cast<uint32_t>(f)) != 0; };
    wprintf(L"  flags        0x%08x\n", s.flags);
    wprintf(L"  heartbeat    %u\n", s.heartbeat);
    if (has(F::HasConnected)) wprintf(L"  connected    %ls\n", has(F::Connected) ? L"yes" : L"no");
    if (has(F::HasDetached)) wprintf(L"  detached     %ls\n", has(F::Detached) ? L"yes" : L"no");
    if (has(F::HasDetachSupport))
        wprintf(L"  detachSupport %ls\n", has(F::DetachSupport) ? L"enabled" : L"disabled");
    if (has(F::HasBattery)) wprintf(L"  battery      %u%%\n", s.battery);
    if (has(F::HasCharging)) wprintf(L"  charging     %ls\n", has(F::Charging) ? L"yes" : L"no");
    if (has(F::HasModule)) wprintf(L"  module       %u\n", s.moduleId);
    if (has(F::HasFirmware)) wprintf(L"  firmware     %hs\n", s.firmware);
    if (has(F::HasHardware)) wprintf(L"  hardware     %hs\n", s.hardware);
    if (has(F::HasSerial)) wprintf(L"  serial       %hs\n", s.serial);
}

int RunKbd(int seconds) {
    Gaokun::Keyboard::HostController controller;
    const auto result = controller.Start(SiblingPath(L"GaokunKeyboardHost.exe"));
    if (result != Gaokun::Keyboard::StartResult::Started &&
        result != Gaokun::Keyboard::StartResult::AlreadyRunning) {
        wprintf(L"start failed (result=%d, exit=%d)\n", static_cast<int>(result),
                controller.ExitCode());
        return 1;
    }
    wprintf(L"host started\n");

    Gaokun::Keyboard::SnapshotReader snapshots;
    for (int i = 0; i < 20 && !snapshots.Open(); ++i) Sleep(200);
    Gaokun::Keyboard::EventReader events;
    for (int i = 0; i < 20 && !events.Open(); ++i) Sleep(200);

    const DWORD deadline = GetTickCount() + static_cast<DWORD>(seconds) * 1000;
    uint64_t last = 0;
    while (GetTickCount() < deadline) {
        Gaokun::Keyboard::Snapshot snapshot{};
        if (snapshots.Read(snapshot) && snapshot.updatedAtUnixMs != last) {
            last = snapshot.updatedAtUnixMs;
            wprintf(L"snapshot:\n");
            PrintKbd(snapshot);
        }
        Gaokun::Keyboard::Event event{};
        while (events.Poll(event)) {
            wprintf(L"event %ls value=%d\n", KbdKindName(event.kind), event.value);
        }
        Sleep(250);
    }

    const bool clean = controller.Stop();
    wprintf(L"stopped: %ls (exit=%d)\n", clean ? L"clean" : L"terminated", controller.ExitCode());
    return clean ? 0 : 1;
}

// 命令通道的端到端验证。宿主已在跑就直接连它的命令管道，不另起一个——多一个实例就会和
// 常驻的那个抢 MCU 端点，那正是这条命令要取代的旧路径。
int RunKbdDetachSupport(bool enable) {
    Gaokun::Keyboard::CommandWriter commands;
    Gaokun::Keyboard::HostController controller;
    bool started = false;

    if (!commands.Open()) {
        wprintf(L"no resident host on the command pipe; starting one\n");
        const auto result = controller.Start(SiblingPath(L"GaokunKeyboardHost.exe"));
        if (result != Gaokun::Keyboard::StartResult::Started &&
            result != Gaokun::Keyboard::StartResult::AlreadyRunning) {
            wprintf(L"start failed (result=%d, exit=%d)\n", static_cast<int>(result),
                    controller.ExitCode());
            return 1;
        }
        started = true;
        for (int i = 0; i < 20 && !commands.Open(); ++i) Sleep(200);
    }

    Gaokun::Keyboard::SnapshotReader snapshots;
    for (int i = 0; i < 20 && !snapshots.Open(); ++i) Sleep(200);
    Gaokun::Keyboard::EventReader events;
    for (int i = 0; i < 20 && !events.Open(); ++i) Sleep(200);

    if (!commands.SetDetachSupport(enable)) {
        wprintf(L"sending the command failed (err=%lu)\n", GetLastError());
        if (started) (void)controller.Stop();
        return 1;
    }
    wprintf(L"sent SetDetachSupport(%ls); watching for the result\n",
            enable ? L"enable" : L"disable");

    // 结果先经事件回来，快照随后翻转，两个都等到才算这条路走通了。
    const DWORD deadline = GetTickCount() + 5000;
    int exitCode = 1;
    while (GetTickCount() < deadline) {
        Gaokun::Keyboard::Event event{};
        while (events.Poll(event)) {
            wprintf(L"event %ls value=%d\n", KbdKindName(event.kind), event.value);
            if (static_cast<Gaokun::Keyboard::EventKind>(event.kind) ==
                Gaokun::Keyboard::EventKind::DetachSupportResult) {
                exitCode = event.value == (enable ? 1 : 0) ? 0 : 1;
            }
        }
        Sleep(100);
    }

    Gaokun::Keyboard::Snapshot snapshot{};
    if (snapshots.Read(snapshot)) {
        wprintf(L"snapshot:\n");
        PrintKbd(snapshot);
    }
    if (started) (void)controller.Stop();
    return exitCode;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc == 4 && _wcsicmp(argv[1], L"kbd") == 0 &&
        _wcsicmp(argv[2], L"--detach-support") == 0) {
        const bool enable = _wcsicmp(argv[3], L"enable") == 0;
        if (!enable && _wcsicmp(argv[3], L"disable") != 0) {
            PrintUsage();
            return 1;
        }
        return RunKbdDetachSupport(enable);
    }

    if (argc < 4) {
        PrintUsage();
        return 1;
    }
    const int seconds = _wtoi(argv[3]);
    if (seconds <= 0) {
        PrintUsage();
        return 1;
    }

    if (_wcsicmp(argv[1], L"thp") == 0 && _wcsicmp(argv[2], L"--test") == 0) return RunThp(seconds);
    if (_wcsicmp(argv[1], L"pen") == 0 && _wcsicmp(argv[2], L"--watch") == 0) return RunPen(seconds);
    if (_wcsicmp(argv[1], L"kbd") == 0 && _wcsicmp(argv[2], L"--watch") == 0) return RunKbd(seconds);

    PrintUsage();
    return 1;
}
