#pragma once
// PenStatusChannel — a small, read-only broadcast of pen power/attach state.
//
// Why this exists separately from IPCCore:
//   * The service runs in session 0 and cannot draw UI, so a companion process in the
//     user's session has to render anything the user sees.
//   * That companion must not require administrator rights (a UAC prompt at every logon
//     is not acceptable), but the IPCCore control pipe is deliberately admin-only —
//     it can rewrite configuration and stop the runtime.
//   * IPCCore is only linked into the service in Debug builds, so nothing on that path
//     exists in a shipping build at all.
//
// So this is a deliberately tiny, one-way channel: the service writes, everyone reads,
// and there is no command surface to abuse. It depends on nothing but Win32, which is
// what lets it be compiled into every configuration.

#include <atomic>
#include <cstdint>
#include <type_traits>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace PenStatus {

inline constexpr wchar_t kSharedMemoryName[] = L"Global\\OpenEGoHubPenStatus";
inline constexpr wchar_t kUpdateEventName[]  = L"Global\\OpenEGoHubPenStatusUpdated";

// Creating a Global\ object requires SeCreateGlobalPrivilege, which the service has and
// an ordinary process does not. Falling back to a session-local name keeps the channel
// exercisable outside a service context (tests, manual runs) without weakening the
// production path, which always tries Global first.
inline constexpr wchar_t kSharedMemoryNameLocal[] = L"Local\\OpenEGoHubPenStatus";
inline constexpr wchar_t kUpdateEventNameLocal[]  = L"Local\\OpenEGoHubPenStatusUpdated";

// 侧键双击。与状态更新分开是因为语义不同：状态是一份可重复读的快照，手势是一次边沿。
// 自动重置事件正好表达这一点——每次 SetEvent 只唤醒一个等待者，连按也不会堆积成一串
// 补发的注入。
inline constexpr wchar_t kGestureEventName[]      = L"Global\\OpenEGoHubPenDoubleClick";
inline constexpr wchar_t kGestureEventNameLocal[] = L"Local\\OpenEGoHubPenDoubleClick";

// Bump when the Snapshot layout changes. A reader that sees a different version treats
// the mapping as unusable rather than misinterpreting the bytes.
inline constexpr uint32_t kAbiVersion = 7;

// 瞬时 UI 通知不能只压成状态位：吸附异常和连接提示都是边沿，读者需要知道「又发生了一次」。
// sequence 单调递增，kind 表示最近一次边沿；共享状态的普通重发不会改变它们。
enum class NotificationKind : uint8_t {
    None = 0,
    PenConnected,
    PenDeviation,
    KeyboardConnected,
    // 键盘「分离后无线连接」没能落地。分成两条是因为用户能做的事不同：固件不支持这个开关
    // 时再点多少次都一样，键盘没有应答则再试一次就可能成功。只追加，不插入。
    KbdDetachSupportFailed,
    KbdDetachSupportUnsupported,
};

// hostHealth 的取值。gaokun-hal 的宿主死掉时它的快照并不消失，seqlock 停在最后一帧，读者
// 拿到的是一份自洽的旧状态。服务据心跳判定宿主是否还在工作，把结论一并发布，读者才能把
// 「键盘拔了」和「读键盘的那个进程没了」区分开。
//
// 这三位占的是 Payload 原有的填充字节，共享内存布局与 ABI 版本因此不变；旧读者读到 0，
// 表现为「未发布这一项」，与本来就没有这个字段时一致。
inline constexpr uint8_t kHostHealthValid = 1u << 0;
inline constexpr uint8_t kHostHealthPen   = 1u << 1;
inline constexpr uint8_t kHostHealthKbd   = 1u << 2;

inline constexpr uint32_t kFlagHasBatteryLevel   = 1u << 0;
inline constexpr uint32_t kFlagHasChargingState  = 1u << 1;
inline constexpr uint32_t kFlagCharging          = 1u << 2;
inline constexpr uint32_t kFlagHasDeviceAttached = 1u << 3;
inline constexpr uint32_t kFlagDeviceAttached    = 1u << 4;
inline constexpr uint32_t kFlagHasStylusLink     = 1u << 5;
inline constexpr uint32_t kFlagStylusLinked      = 1u << 6;
// 服务当前生效的侧键模式。托盘菜单要显示哪一项被选中，靠的是服务回报的这个值而不是它
// 自己提交过什么——提交可能被服务按枚举校验拒绝，也可能被别处的配置改写。
inline constexpr uint32_t kFlagHasPenButtonMode  = 1u << 7;
// ToggleEraser 的当前工具状态。沿用 flags 的空闲位，不改变共享内存 payload 布局；旧托盘会
// 忽略它，新托盘则能在收到双击边沿后确定这次应该进入还是退出 OneNote 橡皮擦。
inline constexpr uint32_t kFlagHasEraserActive   = 1u << 8;
inline constexpr uint32_t kFlagEraserActive      = 1u << 9;
inline constexpr uint32_t kFlagHasTouchProvider  = 1u << 10;
inline constexpr uint32_t kFlagHasProviderError  = 1u << 11;
inline constexpr uint32_t kFlagHasInputSuppressed = 1u << 12;
inline constexpr uint32_t kFlagInputSuppressed    = 1u << 13;
// 键盘「分离后无线连接」开关。发布的是 MCU 回报的实际值而不是本地记下的意图：这个设置存在
// 键盘自己那边，换一块键盘、或者别的程序改过它，本地记忆就是错的。MCU 没答复过时两位都为 0，
// 面板据此显示为未知而不是「已关闭」。
inline constexpr uint32_t kFlagHasKbdDetachSupport = 1u << 14;
inline constexpr uint32_t kFlagKbdDetachSupport    = 1u << 15;

// 笔身份。这三项在一次连接内不变，与电量那类每秒都动的字段共用一份快照只是因为读者相同，
// 没必要为「几乎不变」单开一条通道。
inline constexpr uint32_t kFlagHasPenFirmware = 1u << 16;
inline constexpr uint32_t kFlagHasPenHardware = 1u << 17;
inline constexpr uint32_t kFlagHasPenSerial   = 1u << 18;

// 键盘。kFlagKbdPresent 表示 MCU 键盘子系统有应答——第三方键盘不注册这个接口，走不到这里，
// 所以它同时也是「这是华为一体化键盘」的判据（见 docs/KEYBOARD_IDENTITY.md）。
// 「已分离且无线未连上」与「完全没有键盘」在 MCU 上不可分，原厂同样没分。
inline constexpr uint32_t kFlagHasKbdPresent  = 1u << 19;
inline constexpr uint32_t kFlagKbdPresent     = 1u << 20;
inline constexpr uint32_t kFlagHasKbdDetached = 1u << 21;
inline constexpr uint32_t kFlagKbdDetached    = 1u << 22;
inline constexpr uint32_t kFlagHasKbdBattery  = 1u << 23;
inline constexpr uint32_t kFlagHasKbdCharging = 1u << 24;
inline constexpr uint32_t kFlagKbdCharging    = 1u << 25;
// 华为后台服务是否已被本程序禁用。沿用 flags 的空闲位，共享内存布局因此不变，旧读者会
// 忽略它而不是错读。判据由服务给出：只有它有权查 SCM 的启动类型。
inline constexpr uint32_t kFlagHasVendorServices      = 1u << 26;
inline constexpr uint32_t kFlagVendorServicesDisabled = 1u << 27;
// 电池充电阈值的回读。读它要管理员权限，设置窗是中完整性进程，只有服务读得到，所以必须经
// 这条通道回传——在此之前设置窗没有任何真实值可用，滑块只能停在 Minimum，看起来像是用户
// 自己设过 50%。
//
// kFlagChargeLimitManual 区分两种状态：置位是用户设定的阈值；未置位说明还在华为的智能充电
// 模式下，此时阈值由系统动态调整、并不硬性生效（实测阈值写着 70 而电池充到了 100%），
// 界面不该把它显示成一个确定的设定值。
inline constexpr uint32_t kFlagHasChargeLimit    = 1u << 28;
inline constexpr uint32_t kFlagChargeLimitManual = 1u << 29;
// 名单里还有服务的进程在运行。启动类型与运行状态是两回事：都改成禁用了而进程还在，说明
// 这次改动还没完全落地。界面据此提示，比笼统地说「需要重启」准确——多数情况下不需要。
inline constexpr uint32_t kFlagVendorServicesRunning = 1u << 30;
// 名单里的服务全部在运行。恢复方向要靠它判断「做完了」——启停都要时间，每个服务最多等
// 十秒，界面得知道什么时候才算稳定下来。
inline constexpr uint32_t kFlagVendorServicesAllRunning = 1u << 31;

enum class TouchProviderState : uint8_t {
    Unknown = 0,
    Huawei,
    SwitchingToEGo,
    EGoTouch,
    SwitchingToHuawei,
    Error,
};

inline constexpr int kModelNameCapacity = 32;
// 实测串形如 "GAOKUN_KBD_BD 1.0.0.39"、"1.0.0.40"；序列号更长，单独给一档。
inline constexpr int kVersionCapacity = 32;
inline constexpr int kSerialCapacity  = 40;

// The seqlock-protected payload, kept as its own trivially copyable struct so both sides
// can move it with a single memcpy. Reading the fields individually would let the
// compiler cache or reorder them — a fence orders atomics, not plain loads — and a
// reader could then validate a mixture of two different publishes.
struct Payload {
    uint32_t flags = 0;
    // MCU 上报的模组 ID（0 表示未知）。发布它而不是只发名字，是因为伴随进程要按 ID 找华为
    // 选件中心里以十进制 ID 命名的产品图；从名字反推会在改文案时悄悄失配。
    uint32_t modelId = 0;
    uint32_t notificationSequence = 0;
    uint8_t  batteryLevel = 0;      // percent, valid with kFlagHasBatteryLevel
    uint8_t  penButtonMode = 0;     // PenButtonMode 的数值，valid with kFlagHasPenButtonMode
    uint8_t  touchProvider = 0;     // TouchProviderState，valid with kFlagHasTouchProvider
    uint8_t  providerError = 0;     // Win32/内部错误摘要，valid with kFlagHasProviderError
    uint8_t  kbdBatteryLevel = 0;   // percent, valid with kFlagHasKbdBattery
    uint8_t  notificationKind = 0;  // NotificationKind
    uint8_t  chargeLimit = 0;       // 停充百分比，valid with kFlagHasChargeLimit
    uint8_t  hostHealth = 0;        // kHostHealth* 的位组合
    uint8_t  _pad[4]{};             // 显式补齐，避免下一个字段的偏移随编译器的填充规则漂移
    uint64_t updatedAtUnixMs = 0;
    char     modelName[kModelNameCapacity]{};   // UTF-8, NUL-terminated
    char     penFirmware[kVersionCapacity]{};   // valid with kFlagHasPenFirmware
    char     penHardware[kVersionCapacity]{};   // valid with kFlagHasPenHardware
    char     penSerial[kSerialCapacity]{};      // valid with kFlagHasPenSerial
    char     kbdModelName[kModelNameCapacity]{}; // 空串表示型号未定，仍可显示通用产品图
    char     kbdFirmware[kVersionCapacity]{};   // 型号由它的平台前缀推出，见 KEYBOARD_IDENTITY.md
};
static_assert(std::is_trivially_copyable_v<Payload>,
              "Payload must be memcpy-able for the seqlock to be correct");

struct Snapshot {
    // atomic 只为消除形式上的数据竞争：写者在 Open 时写一次，读者随时读。值恒定，实践中
    // 撕不了，但 UB 不看实践。lock-free 的 uint32 原子读写与普通读写同代价，布局也不变。
    std::atomic<uint32_t> abiVersion{kAbiVersion};
    // Seqlock counter: odd while a write is in progress. Readers retry until they observe
    // the same even value on both sides of the copy, so a torn read is never returned.
    //
    // This must be std::atomic, not volatile. volatile orders nothing against the
    // surrounding plain stores, and std::atomic_thread_fence only orders atomic
    // operations — so on a weakly-ordered target (this device is ARM64) the payload
    // writes could become visible before the odd counter, and a reader would accept a
    // half-written snapshot. Its lock-free guarantee is what makes it valid across a
    // shared mapping, since the two processes share no lock.
    std::atomic<uint32_t> sequence{0};
    Payload payload{};
};

static_assert(sizeof(Snapshot) == 240, "PenStatus::Snapshot layout must stay fixed");
// Cross-process seqlock only works if the counter needs no lock the other process cannot
// see. If this ever fails the channel must be redesigned, not patched.
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "PenStatus::Snapshot::sequence must be lock-free to work across processes");

// 命名空间选择。生产路径永远是 Auto：服务建 Global，伴随进程先找 Global。
// 需要钉死的是测试——服务正在运行时，一个未提权的 Writer 会退到 Local，而 Auto 的 Reader
// 会连上服务的 Global 通道，于是测试读到的是真实设备状态，成败取决于服务当时开没开。
enum class Scope {
    Auto,
    Global,
    Local,
};

// Plain value type handed to callers; never references shared memory directly.
struct State {
    bool hasBatteryLevel = false;
    uint8_t batteryLevel = 0;
    bool hasChargingState = false;
    bool charging = false;
    bool hasDeviceAttached = false;
    bool deviceAttached = false;
    bool hasStylusLink = false;
    bool stylusLinked = false;
    bool hasPenButtonMode = false;
    uint8_t penButtonMode = 0;
    bool hasEraserActive = false;
    bool eraserActive = false;
    bool hasTouchProvider = false;
    TouchProviderState touchProvider = TouchProviderState::Unknown;
    bool hasProviderError = false;
    uint8_t providerError = 0;
    bool hasInputSuppressed = false;
    bool inputSuppressed = false;
    bool hasKbdDetachSupport = false;
    bool kbdDetachSupport = false;
    // hal 宿主是否还在工作，见 kHostHealthValid。未发布时三项都为假，读者按「未知」处理。
    bool hasHostHealth = false;
    bool penHostHealthy = false;
    bool kbdHostHealthy = false;
    bool hasVendorServices = false;
    bool vendorServicesDisabled = false;
    bool vendorServicesRunning = false;
    bool vendorServicesAllRunning = false;
    // chargeLimitManual 为假时 chargeLimit 是华为智能充电模式的当前取值，不是用户的设定，
    // 界面应当据此区分显示，见 kFlagChargeLimitManual。
    bool hasChargeLimit = false;
    bool chargeLimitManual = false;
    uint8_t chargeLimit = 0;
    bool hasPenFirmware = false;
    bool hasPenHardware = false;
    bool hasPenSerial = false;
    bool hasKbdPresent = false;
    bool kbdPresent = false;
    bool hasKbdDetached = false;
    bool kbdDetached = false;
    bool hasKbdBattery = false;
    uint8_t kbdBatteryLevel = 0;
    bool hasKbdCharging = false;
    bool kbdCharging = false;
    uint32_t notificationSequence = 0;
    NotificationKind notificationKind = NotificationKind::None;
    uint64_t updatedAtUnixMs = 0;
    uint32_t modelId = 0;
    char modelName[kModelNameCapacity]{};
    char penFirmware[kVersionCapacity]{};
    char penHardware[kVersionCapacity]{};
    char penSerial[kSerialCapacity]{};
    char kbdModelName[kModelNameCapacity]{};
    char kbdFirmware[kVersionCapacity]{};
};

// Service side. Creates the mapping with an ACL that grants full control to SYSTEM and
// administrators, and read-only access to interactive users.
class Writer {
public:
    Writer() = default;
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // 每个命名空间只允许一个活着的写者，由一个命名信号量把关。第二个写者会让两份状态互踩
    // 同一个 seqlock；提权环境下跑测试时，那第二个写者就是测试本身，还会把假状态灌进托盘
    // 真正在读的通道。闸门被占时按 Auto 规则回落，Local 也被占则 Open 失败。
    //
    // scope 存在就是为了让测试钉死 Local，不去碰运行中服务的 Global 通道。
    bool Open(Scope scope = Scope::Auto);
    void Close();
    [[nodiscard]] bool IsOpen() const noexcept { return m_view != nullptr; }

    // Publishes the state and signals the update event. Returns false if not open.
    bool Publish(const State& state);

    // 通知伴随进程：侧键被双击了一次。服务自己注入不了输入（会话 0），只能转交。
    bool SignalDoubleClick();

    // 事件是否真的建起来了。建不起来时 Open() 仍然成功——共享内存可用，面板靠轮询照常
    // 工作——但一切依赖信号的功能都会无声地失效。调用方应当把它记进日志。
    [[nodiscard]] bool HasEventChannel() const noexcept {
        return m_updateEvent != nullptr && m_gestureEvent != nullptr;
    }

    // 实际落在哪个命名空间。把它传给 Reader::Open 就能确定地连到同一个通道。
    [[nodiscard]] Scope OpenedScope() const noexcept {
        return m_usingLocalNamespace ? Scope::Local : Scope::Global;
    }

private:
    HANDLE m_mapping = nullptr;
    HANDLE m_updateEvent = nullptr;
    HANDLE m_gestureEvent = nullptr;
    // 命名信号量，写者活着期间一直持有。seqlock 的正确性建立在单写者之上，这是把那个假设
    // 变成可执行检查的地方。
    HANDLE m_writerLock = nullptr;
    Snapshot* m_view = nullptr;
    bool m_usingLocalNamespace = false;
};

// Companion-process side. Read-only; works without elevation.
class Reader {
public:
    Reader() = default;
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    bool Open(Scope scope = Scope::Auto);
    void Close();
    [[nodiscard]] bool IsOpen() const noexcept { return m_view != nullptr; }

    // Returns false when the mapping is absent, the ABI does not match, or the writer
    // kept the snapshot torn for the whole retry budget.
    bool Read(State& out) const;

    // Blocks until the service publishes an update, or the timeout elapses.
    // Returns true when an update was signalled.
    //
    // 事件句柄缺失时会顺带重试一次 OpenEvent：写者先建映射后建事件，在这个窗口里打开的
    // 读者会拿到「映射可用、信号永久失效」的状态，而它自己看起来一切正常。补不上时等满
    // 超时返回 false，与 WaitForDoubleClick 一致，循环调用不会空转。
    bool WaitForUpdate(DWORD timeoutMs) const;

    // Blocks until the service reports a side-key double click, or the timeout elapses.
    // Returns true when a gesture was signalled. When the gesture channel is unavailable
    // it waits out the timeout and returns false, so a caller looping on it cannot spin.
    bool WaitForDoubleClick(DWORD timeoutMs) const;

private:
    void OpenEventsIfNeeded() const;

    HANDLE m_mapping = nullptr;
    // mutable：等待接口对调用方是只读的（不改变通道内容），惰性补开事件句柄只是让这个
    // 只读操作真的能用。把接口改成非 const 会传染到所有持有 const Reader& 的调用点。
    mutable HANDLE m_updateEvent = nullptr;
    mutable HANDLE m_gestureEvent = nullptr;
    // 命名空间在 Open 时定下，惰性重试要按它挑事件名。
    bool m_usingLocalNamespace = false;
    const Snapshot* m_view = nullptr;
};

} // namespace PenStatus
