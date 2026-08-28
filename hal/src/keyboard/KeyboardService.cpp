#include "KeyboardService.h"

#include <windows.h>

#include <cstdlib>
#include <deque>
#include <mutex>
#include <string_view>

#include "shared/ModelNames.h"

namespace Gaokun::Keyboard {

namespace {

using CallbackFn = int(__cdecl *)(int);
using VoidFn = void(__cdecl *)();
using SetU8Fn = void(__cdecl *)(uint8_t);
using RegisterFn = void(__cdecl *)(CallbackFn);
using GetTextFn = void(__cdecl *)(char *, int32_t *);

struct Api {
    VoidFn procPipeMsg = nullptr;
    VoidFn getInterruptPipeMsg = nullptr;

    VoidFn getBattery = nullptr;
    VoidFn getChargingStatus = nullptr;
    VoidFn getConnectStatus = nullptr;
    VoidFn getDetachStatus = nullptr;
    VoidFn getFirmwareVersion = nullptr;
    VoidFn getHardwareVersion = nullptr;
    VoidFn getModule = nullptr;
    VoidFn getSerialNo = nullptr;
    VoidFn detachSupportGet = nullptr;
    SetU8Fn detachSupportSet = nullptr;

    GetTextFn textFirmware = nullptr;
    GetTextFn textHardware = nullptr;
    GetTextFn textModule = nullptr;
    GetTextFn textSerial = nullptr;
};

Api g_api{};

std::mutex g_mutex;
Snapshot g_snapshot{};
std::deque<Event> g_events;
HANDLE g_detachReply = nullptr;
volatile LONG g_detachValue = 0;

// 队列有界。读者未连接或读得慢时丢最旧的：键盘事件都是即时语义，补投一条几秒前的插拔
// 没有意义，反而会让界面在恢复连接的瞬间连闪几下。
constexpr size_t kMaxQueued = 64;

[[nodiscard]] uint64_t NowUnixMs() noexcept {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER v{};
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
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

void CopyText(GetTextFn fn, char *dst, int32_t capacity) noexcept {
    if (!fn) return;
    int32_t length = capacity;
    fn(dst, &length);
    dst[capacity - 1] = '\0';
}

// ---- 回调 ----

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

int __cdecl OnDetach(int value) noexcept {
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        SetFlag(Flag::HasDetached, true);
        SetFlag(Flag::Detached, value != 0);
        Touch();
    }
    PushEvent(EventKind::DetachChanged, value);
    return 0;
}

int __cdecl OnFirmware(int) noexcept {
    std::lock_guard<std::mutex> guard(g_mutex);
    CopyText(g_api.textFirmware, g_snapshot.firmware, kVersionCapacity);
    SetFlag(Flag::HasFirmware, g_snapshot.firmware[0] != '\0');

    // 键盘的产品名由固件串的平台前缀判定，不是模组 ID——本机的模组 ID 回报为 0。
    const std::string_view name = Models::KeyboardDisplayName(g_snapshot.firmware);
    const size_t copied = name.copy(g_snapshot.modelName, kModelNameCapacity - 1);
    g_snapshot.modelName[copied] = '\0';

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
    if (value != 0) {
        g_snapshot.moduleId = static_cast<uint32_t>(value);
    } else {
        char text[kVersionCapacity]{};
        CopyText(g_api.textModule, text, kVersionCapacity);
        g_snapshot.moduleId = static_cast<uint32_t>(strtoul(text, nullptr, 10));
    }
    SetFlag(Flag::HasModule, g_snapshot.moduleId != 0);
    Touch();
    return 0;
}

int __cdecl OnDetachSupportGet(int value) noexcept {
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        SetFlag(Flag::HasDetachSupport, true);
        SetFlag(Flag::DetachSupport, value != 0);
        Touch();
    }
    InterlockedExchange(&g_detachValue, value);
    if (g_detachReply) (void)SetEvent(g_detachReply);
    return 0;
}

int __cdecl OnDetachSupportSet(int value) noexcept {
    PushEvent(EventKind::DetachSupportChanged, value);
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

int __cdecl OnFirstBattery(int value) noexcept {
    PushEvent(EventKind::FirstBatteryAfterConnect, value);
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
    if (m_procThread) CloseHandle(static_cast<HANDLE>(m_procThread));
    if (m_interruptThread) CloseHandle(static_cast<HANDLE>(m_interruptThread));
    if (g_detachReply) {
        CloseHandle(g_detachReply);
        g_detachReply = nullptr;
    }
}

bool Service::Start() noexcept {
    if (m_module) return true;

    m_module = LoadLibraryW(L"KeyboardService.dll");
    if (!m_module) return false;
    auto *dll = static_cast<HMODULE>(m_module);

    if (!Resolve(dll, "ProcPipeMsg", g_api.procPipeMsg) ||
        !Resolve(dll, "GetInterruptPipeMsg", g_api.getInterruptPipeMsg)) {
        return false;
    }

    ResolveOptional(dll, "CommandSendGetKeyboardBattery", g_api.getBattery);
    ResolveOptional(dll, "CommandSendGetKeyboardChargingStatus", g_api.getChargingStatus);
    ResolveOptional(dll, "CommandSendGetKeyboardConnectStatus", g_api.getConnectStatus);
    ResolveOptional(dll, "CommandSendGetKeyboardDetachStatus", g_api.getDetachStatus);
    ResolveOptional(dll, "CommandSendGetKeyboardFirmwareVersion", g_api.getFirmwareVersion);
    ResolveOptional(dll, "CommandSendGetKeyboardHardwareVersion", g_api.getHardwareVersion);
    ResolveOptional(dll, "CommandSendGetKeyboardModule", g_api.getModule);
    ResolveOptional(dll, "CommandSendGetKeyboardSerialNo", g_api.getSerialNo);
    ResolveOptional(dll, "CommandSendKbdDetachSupportGet", g_api.detachSupportGet);
    ResolveOptional(dll, "CommandSendKbdDetachSupportSet", g_api.detachSupportSet);

    ResolveOptional(dll, "GetKeyboardFirmwareVersion", g_api.textFirmware);
    ResolveOptional(dll, "GetKeyboardHardwareVersion", g_api.textHardware);
    ResolveOptional(dll, "GetKeyboardModule", g_api.textModule);
    ResolveOptional(dll, "GetKeyboardSerialNo", g_api.textSerial);

    // 逐个注册。缺一个只意味着少一项数据，不该让整个宿主起不来——不同版本的
    // KeyboardService 导出并不完全一致。
    RegisterOptional(dll, "RegisterCallBackUpdateBatteryVolume", &OnBattery);
    RegisterOptional(dll, "RegisterCallBackUpdateKeyboardChargingStatus", &OnCharging);
    RegisterOptional(dll, "RegisterCallBackUpdateKeyboardConnectStatus", &OnConnect);
    RegisterOptional(dll, "RegisterCallBackUpdateDetachStatus", &OnDetach);
    RegisterOptional(dll, "RegisterCallBackUpdateKeyboardFirmwareVersion", &OnFirmware);
    RegisterOptional(dll, "RegisterCallBackUpdateKeyboardHardwareVersion", &OnHardware);
    RegisterOptional(dll, "RegisterCallBackUpdateKeyboardSerialNo", &OnSerial);
    RegisterOptional(dll, "RegisterCallBackUpdateKeyboardModule", &OnModule);
    RegisterOptional(dll, "RegisterCallBackKbdDetachSupportGet", &OnDetachSupportGet);
    RegisterOptional(dll, "RegisterCallBackKbdDetachSupportSet", &OnDetachSupportSet);
    RegisterOptional(dll, "RegisterCallBackNewKeyboardConnectRequest", &OnConnectRequest);
    RegisterOptional(dll, "RegisterCallBackNewKeyboardConnectResult", &OnConnectResult);
    RegisterOptional(dll, "RegisterCallBackKbdConnectResult", &OnConnectResult);
    RegisterOptional(dll, "RegisterCallbackKeyboardFirstBatAfterConn", &OnFirstBattery);

    g_detachReply = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    m_procThread = CreateThread(nullptr, 0, ProcPipeThread, nullptr, 0, nullptr);
    m_interruptThread = CreateThread(nullptr, 0, InterruptPipeThread, nullptr, 0, nullptr);
    if (!m_procThread || !m_interruptThread) return false;

    return WaitUntilThreadBlocks(static_cast<HANDLE>(m_procThread)) &&
           WaitUntilThreadBlocks(static_cast<HANDLE>(m_interruptThread));
}

void Service::RequestRefresh() noexcept {
    if (!m_module) return;
    const VoidFn queries[] = {g_api.getConnectStatus,    g_api.getBattery,
                              g_api.getChargingStatus,   g_api.getDetachStatus,
                              g_api.getModule,           g_api.detachSupportGet,
                              g_api.getFirmwareVersion,  g_api.getHardwareVersion,
                              g_api.getSerialNo};
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

void Service::SetDetachSupport(bool enable) noexcept {
    if (g_api.detachSupportSet) g_api.detachSupportSet(enable ? 1 : 0);
}

bool Service::QueryDetachSupport(bool &enabled) noexcept {
    if (!m_module || !g_api.detachSupportGet || !g_detachReply) return false;

    for (int attempt = 0; attempt < 3; ++attempt) {
        (void)ResetEvent(g_detachReply);
        g_api.detachSupportGet();
        if (WaitForSingleObject(g_detachReply, 100) == WAIT_OBJECT_0) {
            enabled = InterlockedCompareExchange(&g_detachValue, 0, 0) != 0;
            return true;
        }
    }
    return false;
}

} // namespace Gaokun::Keyboard
