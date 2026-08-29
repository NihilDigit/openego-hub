#include "PenService.h"

#include <windows.h>

#include <deque>
#include <mutex>
#include <string>
#include <string_view>

#include "shared/ModelNames.h"

namespace Gaokun::Pen {

namespace {

using CallbackFn = int(__cdecl *)(int);
using VoidFn = void(__cdecl *)();
using SetIntFn = void(__cdecl *)(int32_t);
using RegisterFn = void(__cdecl *)(CallbackFn);
using GetTextFn = void(__cdecl *)(char *, int32_t *);

struct Api {
    VoidFn procPipeMsg = nullptr;
    VoidFn getInterruptPipeMsg = nullptr;

    VoidFn getBattery = nullptr;
    VoidFn getChargingStatus = nullptr;
    VoidFn getConnectStatus = nullptr;
    VoidFn getFirmwareVersion = nullptr;
    VoidFn getHardwareVersion = nullptr;
    VoidFn getSerialNo = nullptr;
    VoidFn getModule = nullptr;
    VoidFn getKeySupport = nullptr;
    VoidFn getKeyFunc = nullptr;
    SetIntFn setKeyFunc = nullptr;
    // 下行的笔/橡皮切换。与 RegisterCallbackPenCurrentFunc 同名而方向相反：那个是状态变更
    // 的回显，这个才是发起切换的命令。原厂由 AcAppDaemon 的按型号插件（CD54RPenApp.dll 等）
    // 在侧键双击时调用，OneNote 白名单也在那一层——我们不加载插件，因此不受白名单约束。
    SetIntFn setCurrentFunc = nullptr;

    GetTextFn textFirmware = nullptr;
    GetTextFn textHardware = nullptr;
    GetTextFn textSerial = nullptr;
    GetTextFn textModule = nullptr;
};

Api g_api{};

std::mutex g_mutex;
Snapshot g_snapshot{};
std::deque<Event> g_events;

// 队列有界。UI 未连接或读得慢时丢最旧的：笔的事件都是即时语义，补投一条几秒前的
// 侧键按下没有意义，反而会让界面在恢复连接的瞬间连闪几下。
constexpr size_t kMaxQueued = 64;

[[nodiscard]] uint64_t NowUnixMs() noexcept {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER v{};
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    // FILETIME 是 1601 起的 100ns 计数，减去到 1970 的差值再转毫秒。
    return (v.QuadPart - 116444736000000000ULL) / 10000ULL;
}

void SetFlag(Flag flag, bool on) noexcept {
    const uint32_t bit = static_cast<uint32_t>(flag);
    if (on) {
        g_snapshot.flags |= bit;
    } else {
        g_snapshot.flags &= ~bit;
    }
}

void Touch() noexcept { g_snapshot.updatedAtUnixMs = NowUnixMs(); }

void PushEvent(EventKind kind, int32_t value) noexcept {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_events.size() >= kMaxQueued) g_events.pop_front();
    g_events.push_back(Event{static_cast<uint32_t>(kind), value, NowUnixMs()});
}

// 取回一段窄字符串。与 THP 的 GetMESSAGE 同形：传入容量，按引用收回实际长度。
void CopyText(GetTextFn fn, char *dst, int32_t capacity) noexcept {
    if (!fn) return;
    int32_t length = capacity;
    fn(dst, &length);
    dst[capacity - 1] = '\0';
}

// ---- 回调 ----
// 全部在 DLL 的消息循环线程上被调用，必须短且不抛。

int __cdecl OnBattery(int value) noexcept {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_snapshot.battery = static_cast<uint8_t>(value);
    SetFlag(Flag::HasBattery, true);
    Touch();
    return 0;
}

int __cdecl OnCharging(int value) noexcept {
    std::lock_guard<std::mutex> guard(g_mutex);
    SetFlag(Flag::HasCharging, true);
    SetFlag(Flag::Charging, value != 0);
    Touch();
    return 0;
}

int __cdecl OnConnect(int value) noexcept {
    std::lock_guard<std::mutex> guard(g_mutex);
    SetFlag(Flag::HasConnected, true);
    SetFlag(Flag::Connected, value != 0);
    Touch();
    return 0;
}

int __cdecl OnFirmware(int) noexcept {
    std::lock_guard<std::mutex> guard(g_mutex);
    CopyText(g_api.textFirmware, g_snapshot.firmware, kVersionCapacity);
    SetFlag(Flag::HasFirmware, g_snapshot.firmware[0] != '\0');
    Touch();
    return 0;
}

int __cdecl OnHardware(int) noexcept {
    std::lock_guard<std::mutex> guard(g_mutex);
    CopyText(g_api.textHardware, g_snapshot.hardware, kVersionCapacity);
    SetFlag(Flag::HasHardware, g_snapshot.hardware[0] != '\0');
    Touch();
    return 0;
}

int __cdecl OnSerial(int) noexcept {
    std::lock_guard<std::mutex> guard(g_mutex);
    CopyText(g_api.textSerial, g_snapshot.serial, kSerialCapacity);
    SetFlag(Flag::HasSerial, g_snapshot.serial[0] != '\0');
    Touch();
    return 0;
}

int __cdecl OnModule(int value) noexcept {
    std::lock_guard<std::mutex> guard(g_mutex);
    // 模块 ID 既可能由回调实参带回，也可能要用 GetPenModule 取文本再解析。先信实参，
    // 为 0 时再退回文本。
    if (value != 0) {
        g_snapshot.moduleId = static_cast<uint32_t>(value);
    } else {
        char text[kVersionCapacity]{};
        CopyText(g_api.textModule, text, kVersionCapacity);
        g_snapshot.moduleId = static_cast<uint32_t>(strtoul(text, nullptr, 10));
    }
    SetFlag(Flag::HasModule, g_snapshot.moduleId != 0);

    // 顺手把产品名填好。上层拿到的应该是能直接显示的名字，而不是一个还要再查表的模组 ID。
    const std::string_view name = Models::PenDisplayName(g_snapshot.moduleId);
    const size_t copied = name.copy(g_snapshot.modelName, kModelNameCapacity - 1);
    g_snapshot.modelName[copied] = '\0';

    Touch();
    return 0;
}

int __cdecl OnKeySupport(int value) noexcept {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_snapshot.keySupport = static_cast<uint8_t>(value);
    SetFlag(Flag::HasKeySupport, true);
    Touch();
    return 0;
}

int __cdecl OnKeyFuncGet(int value) noexcept {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_snapshot.keyFunc = static_cast<uint8_t>(value);
    SetFlag(Flag::HasKeyFunc, true);
    Touch();
    return 0;
}

int __cdecl OnKeyFuncSet(int value) noexcept {
    PushEvent(EventKind::KeyFuncChanged, value);
    return 0;
}

int __cdecl OnCurrentFunc(int value) noexcept {
    PushEvent(EventKind::CurrentFunc, value);
    return 0;
}

int __cdecl OnConnectRequest(int value) noexcept {
    PushEvent(EventKind::ConnectRequest, value);
    return 0;
}

int __cdecl OnConnectResult(int value) noexcept {
    PushEvent(EventKind::ConnectResult, value);
    return 0;
}

int __cdecl OnBatteryReminder(int value) noexcept {
    PushEvent(EventKind::BatteryReminder, value);
    return 0;
}

int __cdecl OnDeviationReminder(int value) noexcept {
    PushEvent(EventKind::DeviationReminder, value);
    return 0;
}

int __cdecl OnCloseConnectWindow(int value) noexcept {
    PushEvent(EventKind::CloseConnectWindow, value);
    return 0;
}

int __cdecl OnTransferPenMode(int value) noexcept {
    PushEvent(EventKind::TransferPenMode, value);
    return 0;
}

// ---- 加载 ----

template <typename Fn>
bool Resolve(HMODULE dll, const char *name, Fn &out) noexcept {
    out = reinterpret_cast<Fn>(GetProcAddress(dll, name));
    return out != nullptr;
}

template <typename Fn>
void ResolveOptional(HMODULE dll, const char *name, Fn &out) noexcept {
    out = reinterpret_cast<Fn>(GetProcAddress(dll, name));
}

void RegisterOptional(HMODULE dll, const char *name, CallbackFn callback) noexcept {
    auto fn = reinterpret_cast<RegisterFn>(GetProcAddress(dll, name));
    if (fn) fn(callback);
}

DWORD WINAPI ProcPipeThread(LPVOID) noexcept {
    g_api.procPipeMsg();
    return 0;
}

DWORD WINAPI InterruptPipeThread(LPVOID) noexcept {
    g_api.getInterruptPipeMsg();
    return 0;
}

[[nodiscard]] bool WaitUntilThreadBlocks(HANDLE thread) noexcept {
    ULONG64 last = 0;
    int idle = 0;
    for (int spins = 0; spins < 2000000; ++spins) {
        ULONG64 cycles = 0;
        if (!QueryThreadCycleTime(thread, &cycles)) return false;
        if (cycles == last) {
            if (++idle > 200) return true;
        } else {
            idle = 0;
        }
        last = cycles;
        (void)SwitchToThread();
    }
    return false;
}

} // namespace

Service::~Service() noexcept {
    // 与 KeyboardService 一样：StopProcPipeMsg / StopLoop / FreeLibrary 在消息循环线程仍在
    // 库内时调用会访问违例，原厂工具解析了却从不使用。交给进程退出回收。
    if (m_procThread) CloseHandle(static_cast<HANDLE>(m_procThread));
    if (m_interruptThread) CloseHandle(static_cast<HANDLE>(m_interruptThread));
}

bool Service::Start() noexcept {
    if (m_module) return true;

    m_module = LoadLibraryW(L"PenService.dll");
    if (!m_module) return false;
    auto *dll = static_cast<HMODULE>(m_module);

    if (!Resolve(dll, "ProcPipeMsg", g_api.procPipeMsg) ||
        !Resolve(dll, "GetInterruptPipeMsg", g_api.getInterruptPipeMsg)) {
        return false;
    }

    ResolveOptional(dll, "CommandSendGetPenBattery", g_api.getBattery);
    ResolveOptional(dll, "CommandSendGetPenChargingStatus", g_api.getChargingStatus);
    ResolveOptional(dll, "CommandSendGetPenConnectStatus", g_api.getConnectStatus);
    ResolveOptional(dll, "CommandSendGetPenFirmwareVersion", g_api.getFirmwareVersion);
    ResolveOptional(dll, "CommandSendGetPenHardwareVersion", g_api.getHardwareVersion);
    ResolveOptional(dll, "CommandSendGetPenSerialNo", g_api.getSerialNo);
    ResolveOptional(dll, "CommandSendGetPenModule", g_api.getModule);
    ResolveOptional(dll, "CommandSendGetPenKeySupport", g_api.getKeySupport);
    ResolveOptional(dll, "CommandSendGetPenKeyFunc", g_api.getKeyFunc);
    ResolveOptional(dll, "CommandSendSetPenKeyFunc", g_api.setKeyFunc);
    // 可选：随 HuaweiPenApp 分发的那份 PenService.dll 据报没有这个导出，缺它时橡皮切换
    // 不可用，但其余功能照常。调用方用 HasCurrentFuncCommand 区分「没这个能力」和
    // 「发了没反应」——两者的排查方向完全不同。
    ResolveOptional(dll, "CommandSendPenCurrentFunc", g_api.setCurrentFunc);

    ResolveOptional(dll, "GetPenFirmwareVersion", g_api.textFirmware);
    ResolveOptional(dll, "GetPenHardwareVersion", g_api.textHardware);
    ResolveOptional(dll, "GetPenSerialNo", g_api.textSerial);
    ResolveOptional(dll, "GetPenModule", g_api.textModule);

    // 回调按名字逐个注册。缺一个只意味着少一项数据，不该让整个宿主起不来——不同版本的
    // PenService 导出并不完全一致。
    RegisterOptional(dll, "RegisterCallBackUpdateBatteryVolume", &OnBattery);
    RegisterOptional(dll, "RegisterCallBackUpdatePenChargingStatus", &OnCharging);
    RegisterOptional(dll, "RegisterCallBackUpdatePenConnectStatus", &OnConnect);
    RegisterOptional(dll, "RegisterCallBackUpdatePenFirmwareVersion", &OnFirmware);
    RegisterOptional(dll, "RegisterCallBackUpdatePenHardwareVersion", &OnHardware);
    RegisterOptional(dll, "RegisterCallBackUpdatePenSerialNo", &OnSerial);
    RegisterOptional(dll, "RegisterCallBackUpdatePenModule", &OnModule);
    RegisterOptional(dll, "RegisterCallBackUpdatePenKeySupport", &OnKeySupport);
    RegisterOptional(dll, "RegisterCallBackUpdatePenKeyFuncGet", &OnKeyFuncGet);
    RegisterOptional(dll, "RegisterCallBackUpdatePenKeyFuncSet", &OnKeyFuncSet);
    RegisterOptional(dll, "RegisterCallbackPenCurrentFunc", &OnCurrentFunc);
    RegisterOptional(dll, "RegisterCallBackNewPenConnectRequest", &OnConnectRequest);
    RegisterOptional(dll, "RegisterCallBackNewPenConnectResult", &OnConnectResult);
    RegisterOptional(dll, "RegisterCallbackPenTopBatteryWindow", &OnBatteryReminder);
    RegisterOptional(dll, "RegisterCallbackPenFirstBatAfterConn", &OnBatteryReminder);
    RegisterOptional(dll, "RegisterCallbackPenDeviationReminder", &OnDeviationReminder);
    RegisterOptional(dll, "RegisterCallbackPenCloseConnectWindow", &OnCloseConnectWindow);
    RegisterOptional(dll, "RegisterCallBackTransferPenMode", &OnTransferPenMode);

    m_procThread = CreateThread(nullptr, 0, ProcPipeThread, nullptr, 0, nullptr);
    m_interruptThread = CreateThread(nullptr, 0, InterruptPipeThread, nullptr, 0, nullptr);
    if (!m_procThread || !m_interruptThread) return false;

    return WaitUntilThreadBlocks(static_cast<HANDLE>(m_procThread)) &&
           WaitUntilThreadBlocks(static_cast<HANDLE>(m_interruptThread));
}

void Service::RequestRefresh() noexcept {
    if (!m_module) return;
    const VoidFn queries[] = {g_api.getConnectStatus, g_api.getBattery,   g_api.getChargingStatus,
                              g_api.getModule,        g_api.getKeySupport, g_api.getKeyFunc,
                              g_api.getFirmwareVersion, g_api.getHardwareVersion, g_api.getSerialNo};
    for (VoidFn query : queries) {
        if (query) query();
    }
}

Snapshot Service::GetSnapshot() const noexcept {
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_snapshot;
}

bool Service::PopEvent(Event &out) noexcept {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_events.empty()) return false;
    out = g_events.front();
    g_events.pop_front();
    return true;
}

void Service::SetKeyFunc(int32_t func) noexcept {
    if (g_api.setKeyFunc) g_api.setKeyFunc(func);
}

bool Service::HasCurrentFuncCommand() const noexcept { return g_api.setCurrentFunc != nullptr; }

void Service::SetCurrentFunc(int32_t func) noexcept {
    if (g_api.setCurrentFunc) g_api.setCurrentFunc(func);
}

} // namespace Gaokun::Pen
