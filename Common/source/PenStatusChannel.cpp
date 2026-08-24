#include "PenStatusChannel.h"

#include <aclapi.h>
#include <sddl.h>

#include <atomic>
#include <chrono>
#include <cstring>

namespace PenStatus {
namespace {

// SYSTEM and Administrators get full control; Interactive Users get read only.
// The companion process needs nothing more than GR, and giving it nothing more is the
// point — this channel must never become a way to influence the service.
constexpr wchar_t kChannelSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;IU)";

// Same ACL for the events, minus write: readers only wait on them.
//
// GX, not SY. SY is a *trustee* abbreviation (Local System) and is not a valid rights
// token, so "GRSY" made the whole descriptor fail to parse and no event was ever created
// — silently, because event creation is non-fatal here and the tray polls on a timer, so
// nothing looked broken until a feature actually needed to be signalled. For event
// objects the generic mapping turns GENERIC_EXECUTE into STANDARD_RIGHTS_EXECUTE |
// SYNCHRONIZE, which is exactly the right to wait.
constexpr wchar_t kEventSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;IU)";

struct ScopedSd {
    PSECURITY_DESCRIPTOR value = nullptr;
    ~ScopedSd() { if (value) LocalFree(value); }
};

bool MakeAttributes(const wchar_t* sddl, SECURITY_ATTRIBUTES& sa, ScopedSd& sd) {
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &sd.value, nullptr)) {
        return false;
    }
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd.value;
    sa.bInheritHandle = FALSE;
    return true;
}

// 当前进程的用户 SID 与给定 SID 是否相同。
bool IsCurrentUser(PSID sid) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size) &&
                    EqualSid(sid, reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid);
    CloseHandle(token);
    return ok;
}

// 既有命名对象是不是「本可以由我们自己建出来的那一个」——属主为 SYSTEM、Administrators
// 或当前用户。同一份实现也在 PenControlChannel.cpp 里；两个通道之间没有共享的私有头，为
// 这几十行新开一个不值得。
bool IsAdoptableOwner(HANDLE object) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(object, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION,
                        &owner, nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS) {
        return false;
    }

    bool adoptable = false;
    if (owner) {
        for (const WELL_KNOWN_SID_TYPE wellKnown :
             {WinLocalSystemSid, WinBuiltinAdministratorsSid}) {
            BYTE expected[SECURITY_MAX_SID_SIZE];
            DWORD size = sizeof(expected);
            if (CreateWellKnownSid(wellKnown, nullptr, expected, &size) &&
                EqualSid(owner, expected)) {
                adoptable = true;
                break;
            }
        }
        if (!adoptable) adoptable = IsCurrentUser(owner);
    }
    if (sd) LocalFree(sd);
    return adoptable;
}

// 建映射，并对「名字已存在」表态。CreateFileMapping 此时仍返回有效句柄，只看返回值发现
// 不了；无条件附着意味着 ACL 来自先建者而不是这里这份，服务会在一个别人能写的映射上广播。
//
// 一律拒绝同样不行：服务重启时伴随进程还握着映射句柄，对象不会消失，重启后的服务就会在
// Global 上撞到已存在，退到 Local，而伴随进程还连在 Global 上——通道从此静默失效，直到伴
// 随进程也重启。所以按属主判断：SYSTEM / Administrators / 当前用户建的对象等价于我们自己
// 建的（非特权进程根本建不了 Global\ 对象），认领；其余放弃，换命名空间。
//
// 这不覆盖「已提权的攻击者预先抢注」——那种情形下机器已经失守，不是这条通道能解决的。
HANDLE CreateOwnedMapping(SECURITY_ATTRIBUTES& sa, const wchar_t* name) {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                  0, sizeof(Snapshot), name);
    if (!h) return nullptr;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return h;
    if (IsAdoptableOwner(h)) return h;
    CloseHandle(h);
    return nullptr;
}

// 单写者闸门。名字不在头文件里：只有写者碰它，读者不需要知道它存在。
constexpr wchar_t kWriterLockName[]      = L"Global\\OpenEGoHubPenStatusWriterLock";
constexpr wchar_t kWriterLockNameLocal[] = L"Local\\OpenEGoHubPenStatusWriterLock";

// 「同一命名空间只能有一个活着的写者」不能靠映射的 ERROR_ALREADY_EXISTS 判断——那分不出
// 「另一个写者正在写」和「写者已经退出、只是读者还握着句柄让对象活着」。后者恰恰是服务
// 重启的常态，拒绝它会让重启后的服务退到 Local 而伴随进程还连在 Global 上。
//
// 计数为 1 的信号量分得开：它由写者独占持有，进程退出时句柄全关、对象随之销毁，重启的写者
// 建到的是一个全新的、计数为 1 的信号量。读者从不打开它，所以它的存活期就是写者的存活期。
// 用信号量而不是互斥量，是因为互斥量按线程记归属，同一线程里开两个 Writer 会递归获取成功,
// 这正是测试要抓的那种情形。
HANDLE AcquireWriterLock(SECURITY_ATTRIBUTES& sa, const wchar_t* name) {
    HANDLE h = CreateSemaphoreW(&sa, 1, 1, name);
    if (!h) return nullptr;
    if (GetLastError() == ERROR_ALREADY_EXISTS && !IsAdoptableOwner(h)) {
        CloseHandle(h);
        return nullptr;
    }
    if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0) {
        CloseHandle(h);   // 另一个写者活着
        return nullptr;
    }
    return h;
}

// 同上，用于两个事件对象。
HANDLE CreateOwnedEvent(SECURITY_ATTRIBUTES& sa, BOOL manualReset, const wchar_t* name) {
    HANDLE h = CreateEventW(&sa, manualReset, FALSE, name);
    if (!h) return nullptr;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return h;
    if (IsAdoptableOwner(h)) return h;
    CloseHandle(h);
    return nullptr;
}

uint64_t NowUnixMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

uint32_t PackFlags(const State& s) {
    uint32_t f = 0;
    if (s.hasBatteryLevel)   f |= kFlagHasBatteryLevel;
    if (s.hasChargingState)  f |= kFlagHasChargingState;
    if (s.charging)          f |= kFlagCharging;
    if (s.hasDeviceAttached) f |= kFlagHasDeviceAttached;
    if (s.deviceAttached)    f |= kFlagDeviceAttached;
    if (s.hasStylusLink)     f |= kFlagHasStylusLink;
    if (s.stylusLinked)      f |= kFlagStylusLinked;
    if (s.hasPenButtonMode)  f |= kFlagHasPenButtonMode;
    if (s.hasEraserActive)   f |= kFlagHasEraserActive;
    if (s.eraserActive)      f |= kFlagEraserActive;
    if (s.hasTouchProvider)  f |= kFlagHasTouchProvider;
    if (s.hasProviderError)  f |= kFlagHasProviderError;
    if (s.hasInputSuppressed) f |= kFlagHasInputSuppressed;
    if (s.inputSuppressed)    f |= kFlagInputSuppressed;
    if (s.hasKbdDetachSupport) f |= kFlagHasKbdDetachSupport;
    if (s.kbdDetachSupport)    f |= kFlagKbdDetachSupport;
    if (s.hasPenFirmware) f |= kFlagHasPenFirmware;
    if (s.hasPenHardware) f |= kFlagHasPenHardware;
    if (s.hasPenSerial)   f |= kFlagHasPenSerial;
    if (s.hasKbdPresent)  f |= kFlagHasKbdPresent;
    if (s.kbdPresent)     f |= kFlagKbdPresent;
    if (s.hasKbdDetached) f |= kFlagHasKbdDetached;
    if (s.kbdDetached)    f |= kFlagKbdDetached;
    if (s.hasKbdBattery)  f |= kFlagHasKbdBattery;
    if (s.hasKbdCharging) f |= kFlagHasKbdCharging;
    if (s.kbdCharging)    f |= kFlagKbdCharging;
    return f;
}

void UnpackFlags(uint32_t f, State& s) {
    s.hasBatteryLevel   = (f & kFlagHasBatteryLevel) != 0;
    s.hasChargingState  = (f & kFlagHasChargingState) != 0;
    s.charging          = (f & kFlagCharging) != 0;
    s.hasDeviceAttached = (f & kFlagHasDeviceAttached) != 0;
    s.deviceAttached    = (f & kFlagDeviceAttached) != 0;
    s.hasStylusLink     = (f & kFlagHasStylusLink) != 0;
    s.stylusLinked      = (f & kFlagStylusLinked) != 0;
    s.hasPenButtonMode  = (f & kFlagHasPenButtonMode) != 0;
    s.hasEraserActive   = (f & kFlagHasEraserActive) != 0;
    s.eraserActive      = (f & kFlagEraserActive) != 0;
    s.hasTouchProvider  = (f & kFlagHasTouchProvider) != 0;
    s.hasProviderError  = (f & kFlagHasProviderError) != 0;
    s.hasInputSuppressed = (f & kFlagHasInputSuppressed) != 0;
    s.inputSuppressed    = (f & kFlagInputSuppressed) != 0;
    s.hasKbdDetachSupport = (f & kFlagHasKbdDetachSupport) != 0;
    s.kbdDetachSupport    = (f & kFlagKbdDetachSupport) != 0;
    s.hasPenFirmware = (f & kFlagHasPenFirmware) != 0;
    s.hasPenHardware = (f & kFlagHasPenHardware) != 0;
    s.hasPenSerial   = (f & kFlagHasPenSerial) != 0;
    s.hasKbdPresent  = (f & kFlagHasKbdPresent) != 0;
    s.kbdPresent     = (f & kFlagKbdPresent) != 0;
    s.hasKbdDetached = (f & kFlagHasKbdDetached) != 0;
    s.kbdDetached    = (f & kFlagKbdDetached) != 0;
    s.hasKbdBattery  = (f & kFlagHasKbdBattery) != 0;
    s.hasKbdCharging = (f & kFlagHasKbdCharging) != 0;
    s.kbdCharging    = (f & kFlagKbdCharging) != 0;
}

} // namespace

// ── Writer ───────────────────────────────────────────────────────────────────

Writer::~Writer() { Close(); }

bool Writer::Open(Scope scope) {
    if (m_view) return true;

    SECURITY_ATTRIBUTES mapSa{};
    ScopedSd mapSd;
    if (!MakeAttributes(kChannelSddl, mapSa, mapSd)) return false;

    SECURITY_ATTRIBUTES evtSa{};
    ScopedSd evtSd;
    // 闸门与两个事件共用这份描述符。它建不出来时不再像从前那样降级继续：闸门没了就没法
    // 保证单写者，而单写者是 seqlock 正确性的前提。
    if (!MakeAttributes(kEventSddl, evtSa, evtSd)) return false;

    // 先占闸门再建映射：反过来的话，映射已经建好而闸门被别人持有时要回滚，而回滚会销毁一份
    // 读者可能已经映射上的对象。
    auto tryNamespace = [&](bool local) {
        m_writerLock = AcquireWriterLock(evtSa, local ? kWriterLockNameLocal : kWriterLockName);
        if (!m_writerLock) return false;
        m_mapping = CreateOwnedMapping(mapSa, local ? kSharedMemoryNameLocal
                                                    : kSharedMemoryName);
        if (!m_mapping) {
            ReleaseSemaphore(m_writerLock, 1, nullptr);
            CloseHandle(m_writerLock);
            m_writerLock = nullptr;
            return false;
        }
        m_usingLocalNamespace = local;
        return true;
    };

    // Only the service holds SeCreateGlobalPrivilege. Anything else (tests, a manual run)
    // still gets a working channel, just confined to its own session. 命名空间被别人占着
    // 时也落到这里：抢不得，宁可退到自己的会话里。
    if (scope == Scope::Local) {
        if (!tryNamespace(true)) return false;
    } else if (!tryNamespace(false)) {
        if (scope == Scope::Global || !tryNamespace(true)) return false;
    }

    m_view = static_cast<Snapshot*>(
        MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Snapshot)));
    if (!m_view) {
        Close();
        return false;
    }

    // 手动重置，但 Publish 用的是脉冲语义（SetEvent 紧接 ResetEvent），所以此刻不在等待中
    // 的读者会错过这一次更新。兜底是面板本来就在定时轮询共享内存，事件只用来把延迟从一个
    // 轮询周期压到即时。
    //
    // 不做成真手动重置：那要求读者自己 ResetEvent，而读者可以有多个，谁来 Reset 说不清——
    // 先醒来的那个一 Reset，其余读者就再也等不到；都不 Reset 则事件恒有信号，等待退化成
    // 空转。
    m_updateEvent = CreateOwnedEvent(evtSa, TRUE,
                                     m_usingLocalNamespace ? kUpdateEventNameLocal
                                                           : kUpdateEventName);
    // Auto-reset: a gesture is an edge, not a state. Auto-reset also collapses a burst
    // into one wake-up, so a stuck reader cannot come back to a queue of pending
    // injections.
    m_gestureEvent = CreateOwnedEvent(evtSa, FALSE,
                                      m_usingLocalNamespace ? kGestureEventNameLocal
                                                            : kGestureEventName);

    // A fresh mapping is zero-filled, which is already a valid even sequence. Stamp the
    // ABI so readers can reject a stale layout. Deliberately not placement-new'ing a
    // Snapshot here: a reader may already have the page mapped, and reconstructing it
    // would momentarily reset the counter under them.
    m_view->abiVersion.store(kAbiVersion, std::memory_order_release);
    return true;
}

void Writer::Close() {
    if (m_view) {
        UnmapViewOfFile(m_view);
        m_view = nullptr;
    }
    if (m_mapping) {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
    }
    if (m_updateEvent) {
        CloseHandle(m_updateEvent);
        m_updateEvent = nullptr;
    }
    if (m_gestureEvent) {
        CloseHandle(m_gestureEvent);
        m_gestureEvent = nullptr;
    }
    if (m_writerLock) {
        // 先归还计数再关句柄：进程活着的时候句柄关闭不会自动补回计数，同一进程里重开
        // Writer 会永远拿不到闸门。
        ReleaseSemaphore(m_writerLock, 1, nullptr);
        CloseHandle(m_writerLock);
        m_writerLock = nullptr;
    }
    m_usingLocalNamespace = false;
}

bool Writer::SignalDoubleClick() {
    if (!m_gestureEvent) return false;
    return SetEvent(m_gestureEvent) != FALSE;
}

bool Writer::Publish(const State& state) {
    if (!m_view) return false;

    // Seqlock: odd sequence marks the window in which the payload is inconsistent.
    const uint32_t start = m_view->sequence.load(std::memory_order_relaxed) + 1u;

    // Release: the odd counter must be visible before any payload store that follows.
    m_view->sequence.store(start, std::memory_order_release);
    // ...and the payload stores must not sink above it.
    std::atomic_thread_fence(std::memory_order_release);

    Payload staged{};
    staged.flags = PackFlags(state);
    staged.modelId = state.modelId;
    staged.notificationSequence = state.notificationSequence;
    staged.batteryLevel = state.batteryLevel;
    staged.penButtonMode = state.penButtonMode;
    staged.touchProvider = static_cast<uint8_t>(state.touchProvider);
    staged.providerError = state.providerError;
    staged.kbdBatteryLevel = state.kbdBatteryLevel;
    staged.notificationKind = static_cast<uint8_t>(state.notificationKind);
    staged.updatedAtUnixMs = NowUnixMs();
    std::strncpy(staged.modelName, state.modelName, sizeof(staged.modelName) - 1);
    std::strncpy(staged.penFirmware, state.penFirmware, sizeof(staged.penFirmware) - 1);
    std::strncpy(staged.penHardware, state.penHardware, sizeof(staged.penHardware) - 1);
    std::strncpy(staged.penSerial, state.penSerial, sizeof(staged.penSerial) - 1);
    std::strncpy(staged.kbdModelName, state.kbdModelName, sizeof(staged.kbdModelName) - 1);
    std::strncpy(staged.kbdFirmware, state.kbdFirmware, sizeof(staged.kbdFirmware) - 1);
    // Single opaque copy: the compiler cannot split it or move it across the counter
    // stores that bracket it.
    std::memcpy(&m_view->payload, &staged, sizeof(Payload));

    // Release: every payload store above is visible before the counter turns even.
    m_view->sequence.store(start + 1u, std::memory_order_release);

    if (m_updateEvent) {
        // Pulse: wake anyone waiting, then re-arm for the next publish.
        SetEvent(m_updateEvent);
        ResetEvent(m_updateEvent);
    }
    return true;
}

// ── Reader ───────────────────────────────────────────────────────────────────

Reader::~Reader() { Close(); }

bool Reader::Open(Scope scope) {
    if (m_view) return true;

    bool local = (scope == Scope::Local);
    if (scope == Scope::Local) {
        m_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kSharedMemoryNameLocal);
    } else {
        m_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kSharedMemoryName);
        if (!m_mapping && scope == Scope::Auto) {
            m_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kSharedMemoryNameLocal);
            local = true;
        }
    }
    if (!m_mapping) return false;

    m_view = static_cast<const Snapshot*>(
        MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, sizeof(Snapshot)));
    if (!m_view) {
        Close();
        return false;
    }

    if (m_view->abiVersion.load(std::memory_order_acquire) != kAbiVersion) {
        Close();
        return false;
    }

    m_usingLocalNamespace = local;
    // 这里失败不算致命：写者先建映射后建事件，读者可能正落在那个窗口里。等待接口会补开。
    OpenEventsIfNeeded();
    return true;
}

void Reader::OpenEventsIfNeeded() const {
    if (!m_updateEvent) {
        m_updateEvent = OpenEventW(SYNCHRONIZE, FALSE,
                                   m_usingLocalNamespace ? kUpdateEventNameLocal
                                                         : kUpdateEventName);
    }
    if (!m_gestureEvent) {
        m_gestureEvent = OpenEventW(SYNCHRONIZE, FALSE,
                                    m_usingLocalNamespace ? kGestureEventNameLocal
                                                          : kGestureEventName);
    }
}

void Reader::Close() {
    if (m_view) {
        UnmapViewOfFile(const_cast<Snapshot*>(m_view));
        m_view = nullptr;
    }
    if (m_mapping) {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
    }
    if (m_updateEvent) {
        CloseHandle(m_updateEvent);
        m_updateEvent = nullptr;
    }
    if (m_gestureEvent) {
        CloseHandle(m_gestureEvent);
        m_gestureEvent = nullptr;
    }
}

bool Reader::Read(State& out) const {
    if (!m_view) return false;
    if (m_view->abiVersion.load(std::memory_order_acquire) != kAbiVersion) return false;

    // Bounded retry: a writer only holds the odd window for a few stores, so failing
    // here means something is wrong rather than merely contended.
    for (int attempt = 0; attempt < 64; ++attempt) {
        // Acquire: nothing below may be hoisted above this load.
        const uint32_t before = m_view->sequence.load(std::memory_order_acquire);
        if (before & 1u) continue;   // writer is mid-update

        Payload copy{};
        std::memcpy(&copy, &m_view->payload, sizeof(Payload));

        // The payload copy must complete before the counter is re-checked, otherwise a
        // stale read could be validated by a counter load that ran early.
        std::atomic_thread_fence(std::memory_order_acquire);
        if (m_view->sequence.load(std::memory_order_relaxed) == before) {
            State candidate{};
            UnpackFlags(copy.flags, candidate);
            candidate.batteryLevel = copy.batteryLevel;
            candidate.penButtonMode = copy.penButtonMode;
            candidate.touchProvider = static_cast<TouchProviderState>(copy.touchProvider);
            candidate.providerError = copy.providerError;
            candidate.kbdBatteryLevel = copy.kbdBatteryLevel;
            candidate.modelId = copy.modelId;
            candidate.notificationSequence = copy.notificationSequence;
            candidate.notificationKind = static_cast<NotificationKind>(copy.notificationKind);
            candidate.updatedAtUnixMs = copy.updatedAtUnixMs;
            std::memcpy(candidate.modelName, copy.modelName, sizeof(candidate.modelName));
            candidate.modelName[kModelNameCapacity - 1] = '\0';
            // 写侧可能截断，共享内存里也可能是别的进程写进来的内容；读侧一律强制收尾，
            // 后面每一处把它当 C 字符串用的地方才不会读越界。
            std::memcpy(candidate.penFirmware, copy.penFirmware, sizeof(candidate.penFirmware));
            candidate.penFirmware[kVersionCapacity - 1] = '\0';
            std::memcpy(candidate.penHardware, copy.penHardware, sizeof(candidate.penHardware));
            candidate.penHardware[kVersionCapacity - 1] = '\0';
            std::memcpy(candidate.penSerial, copy.penSerial, sizeof(candidate.penSerial));
            candidate.penSerial[kSerialCapacity - 1] = '\0';
            std::memcpy(candidate.kbdModelName, copy.kbdModelName, sizeof(candidate.kbdModelName));
            candidate.kbdModelName[kModelNameCapacity - 1] = '\0';
            std::memcpy(candidate.kbdFirmware, copy.kbdFirmware, sizeof(candidate.kbdFirmware));
            candidate.kbdFirmware[kVersionCapacity - 1] = '\0';
            out = candidate;
            return true;
        }
    }
    return false;
}

bool Reader::WaitForUpdate(DWORD timeoutMs) const {
    // 每次调用最多补开一次，不做退避也不加计时器：这个函数本身就是阻塞等待，调用频率被
    // 超时长度限住了，一次 OpenEvent 的开销可以忽略。句柄一旦拿到就不再走这条路。
    if (!m_updateEvent) OpenEventsIfNeeded();
    if (!m_updateEvent) {
        // 与 WaitForDoubleClick 同理：立刻返回会让轮询这个接口的调用方空转。
        Sleep(timeoutMs);
        return false;
    }
    return WaitForSingleObject(m_updateEvent, timeoutMs) == WAIT_OBJECT_0;
}

bool Reader::WaitForDoubleClick(DWORD timeoutMs) const {
    if (!m_gestureEvent) OpenEventsIfNeeded();
    if (!m_gestureEvent) {
        // Still honour the timeout. Returning immediately would spin a caller that loops
        // on this call — which is exactly how it is meant to be used.
        Sleep(timeoutMs);
        return false;
    }
    return WaitForSingleObject(m_gestureEvent, timeoutMs) == WAIT_OBJECT_0;
}

} // namespace PenStatus
