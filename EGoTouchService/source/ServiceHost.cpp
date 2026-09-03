#include "ServiceHost.h"

#include "ServiceLifecycleCoordinator.h"
#include "ConfigRuntime.h"
#include "HostSupervisor.h"
#include "SystemStateMonitor.h"
#include "runtime/DeviceRuntime.h"
#include "penevt/PenEventBridge.h"
#include "penpress/PenPressureReader.h"
#include "ServiceConfigCore.h"
#include "PenSettingsStore.h"
#include "TouchProviderCoordinator.h"
#include "VendorServices.h"
#include "PenControlChannel.h"
#include "PenStatusChannel.h"
// gaokun-hal 的电池读取。充电阈值只有服务读得到——那条 WMI 通道要管理员权限，而设置窗
// 是中完整性进程，自己读会拿到「拒绝访问」。
#include "GaokunPower.h"
#include "GaokunHostExit.h"

#include "Logger.h"
#include "config/ConfigBinder.h"
#include "config/ConfigKeyMap.h"
#include "config/ConfigStore.h"
#include "config/SchemaValidator.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <exception>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace Service {

// 电源事件从监视线程投递到控制线程时的取值。息屏与挂起归为同一件事：现代待机下面板断电
// 未必伴随 PBT_APMSUSPEND，而对宿主来说两者的后果一样。
constexpr int kPowerEventNone = 0;
constexpr int kPowerEventSuspend = 1;
constexpr int kPowerEventResume = 2;
// 息屏之后等这么久再停宿主。息屏和唤醒之间常常只隔一瞬——一条通知、一次误触都够了——
// 停一次再起一次反倒是一段实打实的触控中断。这段时间里来的唤醒把这次息屏一并抵消。
constexpr auto kPowerSuspendDebounce = std::chrono::seconds(2);

struct ServiceHost::Impl {
    ServiceLifecycleStateMachine m_lifecycle;
    // Read-only pen state broadcast for the tray companion. Present in every build:
    // unlike the IPC control surface, a shipping service still has to publish this.
    PenStatus::Writer m_penStatusWriter;
    // 反方向的小通道：托盘菜单选了哪个侧键模式。服务创建，托盘只写。
    PenControl::Host m_penControlHost;
    std::thread m_penControlThread;
    std::atomic<bool> m_penControlStop{false};
    // 托盘要显示当前哪一项生效，而状态发布跑在 DeviceRuntime 的回调线程上，
    // 不能去读会被配置路径改写的 m_configState。应用路径写这份原子镜像，发布路径只读它。
    std::atomic<PenButtonMode> m_effectivePenButtonMode{PenButtonMode::WindowsInk};
    std::atomic<PenStatus::TouchProviderState> m_touchProvider{
        PenStatus::TouchProviderState::Unknown};
    std::atomic<TouchProviderError> m_touchProviderError{TouchProviderError::None};
    std::unique_ptr<TouchProviderCoordinator> m_touchProviderCoordinator;
    // 待处理的电源事件，见 kPowerEvent*。监视线程写、控制线程取走，后写覆盖先写：只有
    // 最后那个状态是真的，中间的翻转没有意义。coordinator 只能在控制线程上被碰。
    std::atomic<int> m_pendingPowerEvent{kPowerEventNone};
    // 息屏之后推迟动手的时刻。只有控制线程读写，不需要同步。
    std::chrono::steady_clock::time_point m_powerSuspendDueAt{};
    std::atomic<bool> m_inputSuppressed{false};
    std::atomic<uint32_t> m_notificationSequence{0};
    std::atomic<PenStatus::NotificationKind> m_notificationKind{
        PenStatus::NotificationKind::None};
    // 一次瞬时通知。kind 先落定再递增序号：托盘只在序号变化时才去读 kind，反过来写会让它
    // 读到上一条的类型。
    void RaiseNotification(PenStatus::NotificationKind kind) {
        m_notificationKind.store(kind, std::memory_order_release);
        m_notificationSequence.fetch_add(1, std::memory_order_acq_rel);
    }
    // 键盘「分离后无线连接」的镜像。真值在 MCU 侧，服务只缓存最近一次应答：写入方是
    // PenEventBridge 的读线程，读取方是 DeviceRuntime 回调线程上的状态发布。
    // 未收到过应答时 known 为 false，此时状态通道不置 has 位，托盘据此把该项显示为不可用。
    std::atomic<bool> m_kbdDetachSupportKnown{false};
    std::atomic<bool> m_kbdDetachSupport{false};

    // 键盘接上的边沿检测。键盘状态源从 PenEventBridge 换成 hal 宿主时，这段一并被删掉，
    // 顶替它的是厂商 DLL 的连接结果回调——那两条对应 MCU 的 0x16 / 0x39，只在分离后的无线
    // 配对上报，键盘吸附回机身走的是 0x12 和 0x31，一条事件都不产生，弹窗于是在最常见的那次
    // 操作上从不出现。边沿只能自己在状态位上求，判据沿用 PenEventBridge::IsKbdConnectionEdge。
    struct KeyboardArrivalDetector {
        // AccessoryLoop 每 250 毫秒采一次。
        static constexpr int kPresenceStableTicks = 8;  // 2 秒
        static constexpr int kDuplicateTicks = 6;       // 1.5 秒

        bool m_havePresence = false;
        bool m_present = false;
        bool m_candidatePresent = false;
        int m_presenceTicks = 0;

        bool m_haveAttached = false;
        bool m_attached = false;

        int m_ticksSinceNotify = kDuplicateTicks;

        // 快照读不到时作废基线：宿主重启后键盘多半还在原位，重建基线的那一轮不该弹窗。
        void Invalidate() noexcept {
            m_havePresence = false;
            m_haveAttached = false;
            m_presenceTicks = 0;
        }

        // 返回真表示这一轮出现了一次「键盘接上了」。
        [[nodiscard]] bool Sample(bool present, bool hasAttached, bool attached) noexcept {
            if (m_ticksSinceNotify < kDuplicateTicks) ++m_ticksSinceNotify;

            bool arrived = false;

            // 0x12 在息屏和唤醒前后会抖出一两秒的 0（docs/KEYBOARD_IDENTITY.md 3.4），照边沿
            // 直接弹会变成每次唤醒都弹一次。取值稳定住才更新基线，抖动因此两头都留不下痕迹。
            if (present != m_candidatePresent) {
                m_candidatePresent = present;
                m_presenceTicks = 0;
            } else if (m_presenceTicks < kPresenceStableTicks &&
                       ++m_presenceTicks == kPresenceStableTicks) {
                arrived = m_havePresence && present && !m_present;
                m_present = present;
                m_havePresence = true;
            }

            // 吸附是一次明确的物理动作，不等稳定窗口——等两秒的弹窗已经错过了用户看它的时刻。
            if (hasAttached) {
                arrived = arrived || (m_haveAttached && attached && !m_attached);
                m_attached = attached;
                m_haveAttached = true;
            } else {
                m_haveAttached = false;
            }

            // 0x12 与 0x31 通常紧挨着到达，表达的是同一次物理动作，合并成一条。
            if (!arrived || m_ticksSinceNotify < kDuplicateTicks) return false;
            m_ticksSinceNotify = 0;
            return true;
        }
    };
    // 只被 AccessoryLoop 那个线程碰，不需要同步。
    KeyboardArrivalDetector m_kbdArrival;
    // 充电阈值的缓存。读一次要走 WMI 往返约 6 毫秒，而状态发布跟着设备回调走、一秒可能
    // 好几次，每次都读会把这条 ACPI 通道占满，还要和别的 WMI 消费者抢。阈值只在用户提交
    // 之后变化，所以启动时读一次、每次提交后重读。
    //
    // 打包进一个原子而不是拆成三个：拆开会读到「已就绪」配上还没更新的百分比，界面上
    // 表现为滑块先跳到旧值再跳到新值。
    //   bit 0-7   停充百分比
    //   bit 8     手动模式（未置位说明还在华为的智能充电模式下）
    //   bit 16    这份缓存有效
    static constexpr uint32_t kChargeLimitValid = 1u << 16;
    static constexpr uint32_t kChargeLimitManual = 1u << 8;
    std::atomic<uint32_t> m_chargeLimitCache{0};

    // 重读充电阈值并更新缓存。读不到时清空而不是保留旧值：读不到的常见原因是权限或固件
    // 不支持，这两种情况下继续显示上一次的数字会让界面看起来还在正常工作。
    void RefreshChargeLimit() {
        Gaokun::Power::ChargeThreshold threshold{};
        if (Gaokun::Power::ReadChargeThreshold(threshold) != Gaokun::Power::Result::Ok) {
            m_chargeLimitCache.store(0, std::memory_order_relaxed);
            return;
        }
        uint32_t packed = kChargeLimitValid | threshold.stopPercent;
        if (threshold.IsManual()) packed |= kChargeLimitManual;
        m_chargeLimitCache.store(packed, std::memory_order_relaxed);
    }
    // 两个 hal 宿主各自的「应当在运行」意图与重启预算。只由 AccessoryLoop 这一个线程读写，
    // 结论经下面两个原子发布给别的线程。
    HostSupervisor m_penSupervisor{"pen"};
    HostSupervisor m_kbdSupervisor{"keyboard"};
    // 宿主此刻可不可信。写入方是 AccessoryLoop，读取方是状态发布和控制线程。
    //
    // 宿主死掉时它的快照并不消失：seqlock 停在最后一帧，读者拿到的是一份自洽的旧状态，
    // 于是托盘继续显示键盘的电量和开关，用户点开关也毫无反应。宁可发布「未知」。
    std::atomic<bool> m_penHostHealthy{false};
    std::atomic<bool> m_kbdHostHealthy{false};
    // 不健康的原因是 PC Manager 的配件组件缺失。健康位只说「现在能不能用」，界面要把「等它
    // 重启」和「用户得去重装电脑管家」分开说，靠的是这两个。
    std::atomic<bool> m_penVendorMissing{false};
    std::atomic<bool> m_kbdVendorMissing{false};

    // 键盘状态镜像。含字符串，用不了原子，改由互斥保护：写入方是 MCU 读线程，
    // 读取方是 DeviceRuntime 回调线程上的状态发布。
    std::mutex m_kbdStateMutex;
    Himax::Pen::PenEventBridge::KbdState m_kbdState;
    std::chrono::steady_clock::time_point m_lastKbdConnectionNotification{};
    std::chrono::steady_clock::time_point m_inputSuppressionStarted{};
    std::chrono::steady_clock::time_point m_inputSuppressionDeadline{};
    // 侧键模式有 IPC 配置重载和托盘控制通道两个写入方，串行化它们对 m_configState 的读改写。
    std::mutex m_penButtonApplyMutex;
    // 六条 hal 通道的指针本身。只有 AccessoryLoop 改它们（宿主重启后整体换掉），而状态发布
    // 在 DeviceRuntime 回调线程上读快照、控制线程写命令，指针必须串行化。锁内的动作都是
    // 一次 memcpy 或一次非阻塞管道写，不会把持有者拖住。
    std::mutex m_accessoryChannelMutex;
    std::unique_ptr<Host::SystemStateMonitor> m_sysMonitor;
    // Serializes startup-time IPC access with Pen object publication.
    std::mutex m_penSubsystemMutex;
    // BT MCU 事件通道 (col00)：设备发现 + 握手 + ACK + 0x7D01 回显 —— 仅 Full 模式
    std::unique_ptr<Himax::Pen::PenEventBridge> m_penEventBridge;
    // BT MCU 压力通道 (col01)：'U' 报文读取 + 频率 / 压感数据 —— 仅 Full 模式
    std::unique_ptr<Himax::Pen::PenPressureReader> m_penPressureReader;

    // 最后一次唤醒类系统事件,用于运行时起来之后补投。见 ReplayLastWakeEvent。
    // 不在 IPC 守卫内:补投是生命周期修复,Release(IPC 关闭)同样需要,而使用点本来
    // 就是无条件的。放进去会让 Release 编译不过。
    std::mutex m_lastWakeEventMutex;
    std::optional<Host::SystemStateEvent> m_lastWakeEvent;
};

// ── 设备路径 ──
static const std::wstring kDevicePathMaster    = L"\\\\.\\Global\\SPBTESTTOOL_MASTER";
static const std::wstring kDevicePathSlave     = L"\\\\.\\Global\\SPBTESTTOOL_SLAVE";
static const std::wstring kDevicePathInterrupt = L"\\\\.\\Global\\SPBTESTTOOL_MASTER";

namespace {

constexpr wchar_t kHuaweiThpServiceName[] = L"HuaweiThpService";
constexpr DWORD kProviderServiceWaitMs = 15000;
constexpr auto kInputSuppressionTimeout = std::chrono::milliseconds(1000);

bool WaitForServiceState(SC_HANDLE service, DWORD desiredState, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    do {
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) {
            return false;
        }
        if (status.dwCurrentState == desiredState) return true;
        if (status.dwCurrentState != SERVICE_START_PENDING &&
            status.dwCurrentState != SERVICE_STOP_PENDING) {
            return false;
        }
        Sleep(100);
    } while (GetTickCount64() < deadline);
    SetLastError(ERROR_TIMEOUT);
    return false;
}

bool WithHuaweiService(DWORD access, const std::function<bool(SC_HANDLE)>& operation) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return false;
    SC_HANDLE service = OpenServiceW(manager, kHuaweiThpServiceName, access);
    if (!service) {
        const DWORD error = GetLastError();
        CloseServiceHandle(manager);
        SetLastError(error);
        return false;
    }
    const bool result = operation(service);
    const DWORD error = result ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    SetLastError(error);
    return result;
}

bool StopHuaweiThpService() {
    return WithHuaweiService(SERVICE_STOP | SERVICE_QUERY_STATUS,
        [](SC_HANDLE service) {
            SERVICE_STATUS_PROCESS status{};
            DWORD needed = 0;
            if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                      reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) {
                return false;
            }
            if (status.dwCurrentState == SERVICE_STOPPED) return true;
            if (status.dwCurrentState != SERVICE_STOP_PENDING) {
                SERVICE_STATUS legacy{};
                if (!ControlService(service, SERVICE_CONTROL_STOP, &legacy) &&
                    GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
                    return false;
                }
            }
            return WaitForServiceState(service, SERVICE_STOPPED, kProviderServiceWaitMs);
        });
}

// 保持 HuaweiThpService 的启动类型为自动，只是让它此刻不在运行。
//
// 这里曾经把它设为 SERVICE_DISABLED，理由是防止 SCM 把它拉起来抢设备。代价是「我们没能
// 接管」和「用户没有触控」变成了同一件事：本服务崩溃、被手工停掉、或者干脆没随开机启动，
// 禁用状态都会留在注册表里，重启后原厂服务也不会自启，机器就没有触摸屏了——实测踩到过。
//
// 只停不禁的代价是重启后有一小段时间由原厂提供触控，直到本服务起来把它换掉。那是个可见
// 但无害的交接，比丢掉触控好得多。
// 只问一次状态，不等待、不发停止命令。
bool IsHuaweiThpServiceStopped() {
    return WithHuaweiService(SERVICE_QUERY_STATUS,
        [](SC_HANDLE service) {
            SERVICE_STATUS_PROCESS status{};
            DWORD needed = 0;
            if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                      reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) {
                return false;
            }
            return status.dwCurrentState == SERVICE_STOPPED;
        });
}

bool EnsureHuaweiThpAutoStart() {
    return WithHuaweiService(SERVICE_CHANGE_CONFIG,
        [](SC_HANDLE service) {
            return ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                                        SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
                                        nullptr, nullptr, nullptr, nullptr) != FALSE;
        });
}

bool RestoreHuaweiThpService() {
    return WithHuaweiService(SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_QUERY_STATUS,
        [](SC_HANDLE service) {
            if (!ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                                      SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
                                      nullptr, nullptr, nullptr, nullptr)) {
                return false;
            }

            SERVICE_STATUS_PROCESS status{};
            DWORD needed = 0;
            if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                      reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) {
                return false;
            }
            if (status.dwCurrentState == SERVICE_RUNNING) return true;
            if (status.dwCurrentState != SERVICE_START_PENDING &&
                !StartServiceW(service, 0, nullptr) &&
                GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
                return false;
            }
            return WaitForServiceState(service, SERVICE_RUNNING, kProviderServiceWaitMs);
        });
}

enum class DebugDerivedSourceIndex : int16_t {
    MasterWasRead = 0,
    ContactCount = 1,
    PeakCount = 2,
    FrameTimestamp = 3,
};

std::size_t Utf8TruncatedLength(std::string_view text, std::size_t capacity) noexcept {
    std::size_t i = 0;
    std::size_t lastGood = 0;
    while (i < text.size() && i < capacity) {
        const auto ch = static_cast<uint8_t>(text[i]);
        std::size_t width = 1;
        if ((ch & 0x80u) == 0) {
            width = 1;
        } else if ((ch & 0xE0u) == 0xC0u) {
            width = 2;
        } else if ((ch & 0xF0u) == 0xE0u) {
            width = 3;
        } else if ((ch & 0xF8u) == 0xF0u) {
            width = 4;
        } else {
            break;
        }
        if (i + width > text.size() || i + width > capacity) {
            break;
        }
        i += width;
        lastGood = i;
    }
    return lastGood;
}

constexpr std::array<std::string_view, 4> kStylusIirCoefficientPaths{
    "stylus.sp.iir_coef_low_hover",
    "stylus.sp.iir_coef_high_hover",
    "stylus.sp.iir_coef_low_writing",
    "stylus.sp.iir_coef_high_writing",
};

bool StylusIirCoefficientsWithinMax(const Config::ConfigStore& store) {
    const int32_t maxCoef = store.getOr<int32_t>("stylus.sp.iir_max_coef", 32);
    if (maxCoef < 1) {
        return false;
    }

    for (const auto path : kStylusIirCoefficientPaths) {
        const int32_t coef = store.getOr<int32_t>(path, 0);
        if (coef < 0 || coef > maxCoef) {
            return false;
        }
    }
    return true;
}

void ClampStylusIirCoefficients(Config::ConfigStore& store) {
    const int32_t maxCoef = std::clamp(store.getOr<int32_t>("stylus.sp.iir_max_coef", 32), int32_t{1}, int32_t{255});
    store.set<int32_t>("stylus.sp.iir_max_coef", maxCoef);
    for (const auto path : kStylusIirCoefficientPaths) {
        if (store.has(path)) {
            store.set<int32_t>(path, std::clamp(store.get<int32_t>(path), int32_t{0}, maxCoef));
        }
    }
}

bool ConfigValueAllowedBySchema(std::string_view path,
                                const Config::ConfigValue& value,
                                const Config::ConfigSchemaSnapshot& schema) {
    const auto it = std::find_if(schema.entries.begin(), schema.entries.end(),
        [path](const Config::ConfigSchemaEntry& entry) { return entry.yamlPath == path; });
    if (it == schema.entries.end()) {
        return false;
    }
    if (!it->boundToRuntime ||
        (it->runtimeBinding != Config::ConfigRuntimeBinding::LiveSetter &&
         it->runtimeBinding != Config::ConfigRuntimeBinding::ManualLiveApply) ||
        !Config::isLiveApplyTiming(it->applyTiming)) {
        return false;
    }

    if (path == "service.mode") {
        const auto str = Config::tryGetValue<std::string>(value);
        return str.has_value() && (*str == "full" || *str == "touch_only");
    }
    if (path == "service.pen_button_mode") {
        return Service::ParsePenButtonModeValue(value).has_value();
    }
    if (path == "service.pen_button_route") {
        return Service::ParsePenButtonRouteValue(value).has_value();
    }


    switch (it->uiType) {
    case Config::ConfigUiType::Bool:
        return Config::tryGetValue<bool>(value).has_value();
    case Config::ConfigUiType::Int32: {
        const auto v = Config::tryGetValue<int32_t>(value);
        if (!v.has_value()) return false;
        return !it->range.has_value() || (*v >= it->range->min && *v <= it->range->max);
    }
    case Config::ConfigUiType::Float: {
        const auto v = Config::tryGetValue<float>(value);
        if (!v.has_value()) return false;
        return !it->range.has_value() || (*v >= it->range->min && *v <= it->range->max);
    }
    case Config::ConfigUiType::Enum:
    case Config::ConfigUiType::String:
        return Config::tryGetValue<std::string>(value).has_value();
    }
    return false;
}

uint64_t EncodeU32(uint32_t value) {
    return static_cast<uint64_t>(value);
}

uint64_t EncodeI32(int32_t value) {
    return static_cast<uint64_t>(static_cast<uint32_t>(value));
}

uint64_t EncodeBool(bool value) {
    return value ? 1ull : 0ull;
}

uint64_t EncodeF32(float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float/u32 size mismatch");
    std::memcpy(&bits, &value, sizeof(bits));
    return static_cast<uint64_t>(bits);
}

template <size_t N>
std::string_view CStrArrayView(const char (&value)[N]) {
    const auto* end = std::find(value, value + N, '\0');
    return std::string_view(value, static_cast<size_t>(end - value));
}

// 宿主被 CREATE_NO_WINDOW 拉起，没有控制台，日志只能落盘，级别跟随服务。首次启动与巡检
// 里的重启必须传同一份参数，否则重启之后宿主的日志级别会悄悄变回默认值。
std::wstring HostLogArgs() {
    return std::wstring(L"--log-level ") + Common::Logger::MinLevelName();
}

RuntimePolicyEvent TranslateSystemStateEvent(const Host::SystemStateEvent& event) {
    RuntimePolicyEvent translated{};
    translated.timestamp = event.timestamp;
    translated.rawIndex = event.raw_index;

    switch (event.type) {
    case Host::SystemStateEventType::DisplayOn:
        translated.type = RuntimePolicyEvent::Type::DisplayOn;
        break;
    case Host::SystemStateEventType::DisplayOff:
        translated.type = RuntimePolicyEvent::Type::DisplayOff;
        break;
    case Host::SystemStateEventType::LidOn:
        translated.type = RuntimePolicyEvent::Type::LidOn;
        break;
    case Host::SystemStateEventType::LidOff:
        translated.type = RuntimePolicyEvent::Type::LidOff;
        break;
    case Host::SystemStateEventType::Suspend:
        translated.type = RuntimePolicyEvent::Type::Suspend;
        break;
    case Host::SystemStateEventType::Shutdown:
        translated.type = RuntimePolicyEvent::Type::Shutdown;
        break;
    case Host::SystemStateEventType::ResumeAutomatic:
        translated.type = RuntimePolicyEvent::Type::ResumeAutomatic;
        break;
    default:
        translated.type = RuntimePolicyEvent::Type::Unknown;
        break;
    }

    return translated;
}

} // namespace

ServiceHost::ServiceHost()
    : m_impl(std::make_unique<Impl>()) {}

ServiceHost::~ServiceHost() {
    Stop();
}

bool ServiceHost::InitializeConfigStores() {
    // Runs in every build. Config v3 defaults come from code, not from a file or from
    // IPC, so gating this on EGOTOUCH_SERVICE_ENABLE_IPC used to leave Release running
    // the pipelines' in-class initializers instead of the declared defaults.
    const bool ok = m_configRuntime.Initialize(
        [this](const Config::ConfigStore& store) { return ValidateStartupConfig(store); });
    if (!ok) {
        return false;
    }
    m_configState = m_configRuntime.ServiceState();
    m_configRuntime.WriteServiceState(m_configState);
    return true;
}
// ── 模式解析 ──────────────────────────────────────────
void ServiceHost::ApplyServiceConfigToRuntime(const ServiceConfigState& config) {
    if (!m_deviceRuntime) return;

    m_deviceRuntime->ApplyServicePolicy(
        config.autoMode, config.stylusVhfEnabled,
        config.penButtonMode, config.penButtonRoute,
        config.penButtonRouteExplicit);
}

// Validates the startup store in every build: it guards the same defaults that
// InitializeConfigStores() now injects unconditionally.
bool ServiceHost::ValidateStartupConfig(const Config::ConfigStore& store) const {
    ServiceConfigState schemaState{};
    Config::ConfigBinder serviceBinder;
    RegisterServiceConfigBindings(serviceBinder, schemaState);

    auto serviceValidation = Config::SchemaValidator::validate(store, serviceBinder);
    serviceValidation.logAll();
    if (!serviceValidation.ok()) {
        return false;
    }

    if (m_deviceRuntime) {
        auto pipelineValidation = m_deviceRuntime->ValidateConfigStore(store);
        pipelineValidation.logAll();
        if (!pipelineValidation.ok()) {
            return false;
        }
    }

    return true;
}

bool ServiceHost::StartRuntimeAndPipeline() {
    m_startPhase.store(ServiceStartPhase::RuntimeAndPipeline, std::memory_order_release);
    m_deviceRuntime = std::make_unique<DeviceRuntime>(
        kDevicePathMaster, kDevicePathSlave, kDevicePathInterrupt);

    Config::ConfigStore startupConfig = m_configRuntime.SnapshotStore();
    // Phase 2 runs after DeviceRuntime is constructed: validate pipeline keys against
    // the runtime-owned binder before applying the config store.
    if (!ValidateStartupConfig(startupConfig)) {
        LOG_ERROR("Service", __func__, "Boot", "Startup config validation failed; runtime start blocked.");
        m_deviceRuntime.reset();
        return false;
    }

    // 配置注入已完成，这里再让持久化的侧键模式覆盖它：托盘上一次选的那一项优先于配置默认值。
    // 文件缺失或 token 非法时 LoadPenButtonMode 返回 nullopt 并已记 WARN，走配置里的值。
    if (const auto persisted = PenSettingsStore::LoadPenButtonMode()) {
        m_configState.penButtonMode = *persisted;
        m_configRuntime.WriteServiceState(m_configState);
        LOG_INFO("Service", __func__, "Boot",
                 "Restored persisted pen button mode: {}.", ToString(*persisted));
    }
    m_impl->m_effectivePenButtonMode.store(m_configState.penButtonMode,
                                           std::memory_order_release);

    ApplyServiceConfigToRuntime(m_configState);

    if (m_impl->m_penStatusWriter.Open()) {
        m_deviceRuntime->SetPenStateChangedCallback(
            [this](const RuntimePenState&) { PublishStatusSnapshot(); });
        m_deviceRuntime->SetPenDoubleClickCallback([this] {
            return m_impl->m_penStatusWriter.SignalDoubleClick();
        });
        LOG_INFO("Service", __func__, "Boot", "Pen status channel published.");
        if (!m_impl->m_penStatusWriter.HasEventChannel()) {
            // 共享内存可用而事件没建成时，面板靠轮询照常显示，但双击转发会无声失效。
            // 不打错误码：事件创建失败发生在 Open() 内部，等日志写完 last error 早被覆盖了。
            LOG_WARN("Service", __func__, "Boot",
                     "Pen status events unavailable; side-key double click will "
                     "not reach the tray.");
        }
    } else {
        // Non-fatal: losing the tray display must never keep touch from starting.
        // 同上，Open() 有多条失败路径，带不出可信的错误码，只陈述失败事实。
        LOG_WARN("Service", __func__, "Boot",
                 "Pen status channel unavailable; tray display disabled.");
    }

    TouchProviderOperations providerOps{};
    providerOps.stopHuawei = [] {
        const bool ok = StopHuaweiThpService();
        if (!ok) {
            LOG_ERROR("Service", "StopHuawei", "Provider",
                      "Failed to stop HuaweiThpService (err={}).", GetLastError());
        }
        return ok;
    };
    providerOps.disableHuawei = [] {
        // 名字叫 disable，实际只保证它不在运行、同时留着自动启动。原因见
        // EnsureHuaweiThpAutoStart 的注释：真禁用会在本服务缺席时连带丢掉触控。
        //
        // 常路只查一次状态。这一步的调用点前面刚刚等到过 SERVICE_STOPPED，再走一遍完整的
        // 停止流程最坏要在控制线程上白等 15 秒，而租约到期、托盘的命令和宿主的存活检查都
        // 排在这个线程后面。确实还在跑时才真去停它——回滚路径上原厂可能起了一半。
        const bool ok = EnsureHuaweiThpAutoStart() &&
                        (IsHuaweiThpServiceStopped() || StopHuaweiThpService());
        if (!ok) {
            LOG_ERROR("Service", "DisableHuawei", "Provider",
                      "Failed to stand HuaweiThpService down (err={}).", GetLastError());
        }
        return ok;
    };
    providerOps.startEGo = [this] { return StartEGoTouchProvider(); };
    providerOps.stopEGo = [this] { return StopEGoTouchProvider(); };
    // 只有 coordinator 认定触控归 EGo 时才会问到这里，所以返回 false 就是宿主意外没了。
    // 退出码在这一刻还取得到，交还原厂之后 Start 会把句柄换掉，那时再查就是下一条命的了。
    providerOps.egoAlive = [this] {
        if (m_thpHost.IsRunning()) return true;
        LOG_ERROR("Service", "EGoAlive", "Provider",
                  "THP host is gone (exit={}); restarting it or handing touch back to Huawei.",
                  m_thpHost.ExitCode());
        return false;
    };
    providerOps.restoreHuawei = [] {
        const bool ok = RestoreHuaweiThpService();
        if (!ok) {
            LOG_ERROR("Service", "RestoreHuawei", "Provider",
                      "Failed to restore HuaweiThpService (err={}).", GetLastError());
        }
        return ok;
    };
    m_impl->m_touchProviderCoordinator = std::make_unique<TouchProviderCoordinator>(
        std::move(providerOps),
        [this](PenStatus::TouchProviderState state, TouchProviderError error) {
            PublishTouchProviderState(state, static_cast<uint8_t>(error));
        },
        std::chrono::seconds(5));

    // 服务本身是常驻监督器，启动时先确保 Huawei 提供触控。托盘登录后取得租约，才切换到
    // EGo runtime；这样登录界面和托盘未运行的场景始终有 OEM 回退。
    const bool huaweiReady = m_impl->m_touchProviderCoordinator->Release();
    if (!huaweiReady &&
        m_impl->m_touchProviderCoordinator->State() == PenStatus::TouchProviderState::Error) {
        LOG_ERROR("Service", __func__, "Boot",
                  "Neither HuaweiTHP nor EGoTouch could be made active.");
        m_impl->m_touchProviderCoordinator.reset();
        m_deviceRuntime.reset();
        return false;
    }

    // 控制线程在 runtime 对象和 provider coordinator 都发布之后才启动。
    StartPenControlChannel();

    LOG_INFO("Service", __func__, "Boot",
             "Touch provider supervisor ready; awaiting tray lease.");
    return true;
}

bool ServiceHost::StartSystemStateMonitor() {
    m_startPhase.store(ServiceStartPhase::SystemStateMonitor, std::memory_order_release);
    m_impl->m_sysMonitor = std::make_unique<Host::SystemStateMonitor>();
    const bool monitorOk = m_impl->m_sysMonitor->Start(
        [this](const Host::SystemStateEvent& ev) {
            LOG_INFO("Service", __func__, "Event", "System event: type={}", Host::ToString(ev.type));
            // Windows 在注册电源通知的那一刻就把当前的显示/盖子状态送过来一次,而运行时
            // 那时通常还没准备好,会把它丢掉;此后显示与盖子不再变化,不会有第二次,运行时
            // 于是永远进不了 streaming。记下最后一次唤醒状态,运行时起来之后补投一次。
            if (Host::IsPenStatusWakeEvent(ev.type)) {
                std::lock_guard<std::mutex> lk(m_impl->m_lastWakeEventMutex);
                m_impl->m_lastWakeEvent = ev;
            }
            if (m_deviceRuntime) {
                m_deviceRuntime->IngestPolicyEvent(TranslateSystemStateEvent(ev));
            }

            // 触控提供方要跟着睡和醒，但 coordinator 只能在控制线程上被碰，这里只留个记号。
            // 盖子事件不接：合盖本身不断面板电，接了只会多出一次没必要的宿主起停。
            switch (ev.type) {
            case Host::SystemStateEventType::Suspend:
            case Host::SystemStateEventType::DisplayOff:
                m_impl->m_pendingPowerEvent.store(kPowerEventSuspend, std::memory_order_release);
                break;
            case Host::SystemStateEventType::ResumeAutomatic:
            case Host::SystemStateEventType::DisplayOn:
                m_impl->m_pendingPowerEvent.store(kPowerEventResume, std::memory_order_release);
                break;
            default:
                break;
            }

            if (!Host::IsPenStatusWakeEvent(ev.type) ||
                m_runtimeMode != ServiceMode::Full) {
                return;
            }

            std::lock_guard<std::mutex> penLock(m_impl->m_penSubsystemMutex);
            auto* penEventBridge = m_impl->m_penEventBridge.get();
            if (!penEventBridge || !penEventBridge->IsRunning()) {
                LOG_INFO("Service", __func__, "MCU",
                         "Wake status query skipped because PenEventBridge is not running.");
                return;
            }

            if (!penEventBridge->SendQueryPenStatus()) {
                LOG_WARN("Service", __func__, "MCU",
                         "Wake status query failed for event {}.", Host::ToString(ev.type));
            }
        });

    if (!monitorOk) {
        LOG_ERROR("Service", __func__, "Monitor", "SystemStateMonitor failed to start; service startup will roll back.");
        m_impl->m_sysMonitor.reset();
        return false;
    }

    LOG_INFO("Service", __func__, "Monitor", "SystemStateMonitor started.");
    return true;
}

// IPC 控制面已经撤掉：调试与配置下发都不再经过命名管道。这三个入口留着是因为
// ServiceLifecycleCoordinator 按固定顺序调用它们，删掉要动生命周期的阶段表。
bool ServiceHost::StartIpcSubsystem() {
    m_startPhase.store(ServiceStartPhase::IpcSubsystem, std::memory_order_release);
    return true;
}

bool ServiceHost::StartPenSubsystem() {
    m_startPhase.store(ServiceStartPhase::PenSubsystem, std::memory_order_release);
    if (m_runtimeMode != ServiceMode::Full) {
        LOG_INFO("Service", __func__, "MCU", "Pen modules skipped (touch_only mode).");
        // 充电阈值的常规首读在 AccessoryLoop 里，touch_only 不启动那个循环，只能在
        // 这里读一次。整个 Start 已在后台线程且 WMI 有 5 秒上限，不再牵连 SCM。
        m_impl->RefreshChargeLimit();
        return true;
    }

    try {
        // 笔与键盘的 MCU 数据来自 gaokun-hal 的两个宿主进程，它们直接驱动厂商的
        // PenService.dll 与 KeyboardService.dll。型号识别、按键语义、固件串格式因此都留在
        // 厂商实现里，本仓库不再维护一份自己的协议解析——那份解析曾把 modelId 282 认成
        // CD52，而它其实是 M-Pen 2。
        //
        // 这一段的每条失败路径都只降级，不再让 Start 返回 false。缺一个 KeyboardService.dll
        // 就整个服务回滚的话，用户丢的是触控、笔手势和托盘，而不只是键盘那一项。
        const std::wstring penHostPath = ResolveHostPath(L"GaokunPenHost.exe");
        const std::wstring kbdHostPath = ResolveHostPath(L"GaokunKeyboardHost.exe");
        if (penHostPath.empty() || kbdHostPath.empty()) {
            LOG_ERROR("Service", __func__, "MCU",
                      "gaokun-hal accessory hosts not found next to the service "
                      "(pen={}, keyboard={}); accessory state will be unavailable.",
                      !penHostPath.empty(), !kbdHostPath.empty());
        }

        const std::wstring hostLogArgs = HostLogArgs();
        const auto now = HostSupervisor::Clock::now();

        if (!penHostPath.empty()) {
            const auto penStart = m_penHost.Start(penHostPath, hostLogArgs);
            const bool penVendorMissing =
                penStart == Gaokun::Pen::StartResult::ExitedImmediately &&
                m_penHost.ExitCode() == Gaokun::kHostExitVendorComponentsMissing;
            if (penVendorMissing) {
                LOG_ERROR("Service", __func__, "MCU",
                          "Pen host exited immediately: the PC Manager accessory components "
                          "are missing. Reinstalling PC Manager is what fixes it; until then "
                          "the host is probed once every {} s instead of being restarted.",
                          HostSupervisor::kVendorMissingReprobe.count());
            } else if (penStart != Gaokun::Pen::StartResult::Started &&
                       penStart != Gaokun::Pen::StartResult::AlreadyRunning) {
                LOG_ERROR("Service", __func__, "MCU",
                          "Pen host failed to start (result={}, exit={}); the supervisor "
                          "will retry it.",
                          static_cast<int>(penStart), m_penHost.ExitCode());
            }
            // 意图与「这一次起没起来」无关：起不来的宿主同样归巡检管，预算内它会被重试。
            m_impl->m_penSupervisor.SetDesiredRunning(true, now);
            // 顺序不能反：SetDesiredRunning 会复位状态，其中就包括 vendorMissing。
            if (penVendorMissing) m_impl->m_penSupervisor.NoteVendorMissing(now);
        }

        if (!kbdHostPath.empty()) {
            const auto kbdStart = m_kbdHost.Start(kbdHostPath, hostLogArgs);
            const bool kbdVendorMissing =
                kbdStart == Gaokun::Keyboard::StartResult::ExitedImmediately &&
                m_kbdHost.ExitCode() == Gaokun::kHostExitVendorComponentsMissing;
            if (kbdVendorMissing) {
                LOG_ERROR("Service", __func__, "MCU",
                          "Keyboard host exited immediately: the PC Manager accessory "
                          "components are missing. Reinstalling PC Manager is what fixes it; "
                          "until then the host is probed once every {} s instead of being "
                          "restarted.",
                          HostSupervisor::kVendorMissingReprobe.count());
            } else if (kbdStart != Gaokun::Keyboard::StartResult::Started &&
                       kbdStart != Gaokun::Keyboard::StartResult::AlreadyRunning) {
                LOG_ERROR("Service", __func__, "MCU",
                          "Keyboard host failed to start (result={}, exit={}); the supervisor "
                          "will retry it.",
                          static_cast<int>(kbdStart), m_kbdHost.ExitCode());
            }
            m_impl->m_kbdSupervisor.SetDesiredRunning(true, now);
            if (kbdVendorMissing) m_impl->m_kbdSupervisor.NoteVendorMissing(now);
        }

        // 宿主刚拉起来，映射与管道未必已经建好，所以这六条通道此刻打不开是常态。这里只试
        // 一次，退让留给 AccessoryLoop 每轮的补开：在启动路径上逐条重试会把 ServiceMain
        // 报 RUNNING 的时刻推后几十秒，安装程序于是停在「正在启动服务」。
        //
        // 打不开不算致命：快照读不到时上层显示「未知」，比让整个服务起不来要好。
        OpenAccessoryChannels();

        m_deviceRuntime->SetPenCurrentFuncCommandCallback([this](bool eraser) {
            // 通道可能刚被补开、也可能刚随宿主重启换掉，所以每次调用现取。
            std::lock_guard<std::mutex> lk(m_impl->m_accessoryChannelMutex);
            return m_penCommands && m_penCommands->SetCurrentFunc(eraser);
        });

        m_accessoryStop.store(false, std::memory_order_release);
        m_accessoryThread = std::thread([this] { AccessoryLoop(); });

        StartPenEventBridge();

        LOG_INFO("Service", __func__, "MCU",
                 "Accessory subsystem started (penHost={}, kbdHost={}); the supervisor now "
                 "owns both.",
                 m_penHost.IsRunning(), m_kbdHost.IsRunning());
        return true;
    } catch (...) {
        LOG_ERROR("Service", __func__, "MCU", "Pen subsystem startup threw an exception.");
        return false;
    }
}

bool ServiceHost::Start() {
    return m_impl->m_lifecycle.RunStart([this] {
        try {
            m_startPhase.store(ServiceStartPhase::Config, std::memory_order_release);
            if (!InitializeConfigStores()) {
                LOG_ERROR("Service", "Start", "Boot", "Startup config load/validation failed; service start blocked.");
                return false;
            }

            m_runtimeMode = m_configState.mode;
            LOG_INFO("Service", "Start", "Boot", "Service mode: {}, AutoMode: {}",
                     ServiceModeToConfig(m_configState.mode), m_configState.autoMode);

            // 厂商服务的现状记一条。「上次禁用过、这次重启后被谁恢复了」和「本来就没禁用」
            // 在日志里长得一样，事后只能靠这条基线区分。
            const auto vendor = VendorServices::Query();
            LOG_INFO("Service", "Start", "Vendor",
                     "Vendor services: {} present, {} disabled, {} running.",
                     vendor.total, vendor.disabled, vendor.running);

            if (!ServiceLifecycleCoordinator::Start(*this)) {
                return false;
            }

            LOG_INFO("Service", "Start", "Boot", "All modules started.");
            return true;
        } catch (const std::exception& error) {
            m_startPhase.store(ServiceStartPhase::Exception, std::memory_order_release);
            ServiceLifecycleCoordinator::Stop(*this);
            LOG_ERROR("Service", "Start", "Boot",
                      "Unhandled startup exception; service lifecycle rolled back: {}",
                      error.what());
            return false;
        } catch (...) {
            m_startPhase.store(ServiceStartPhase::Exception, std::memory_order_release);
            ServiceLifecycleCoordinator::Stop(*this);
            LOG_ERROR("Service", "Start", "Boot",
                      "Unhandled non-standard startup exception; service lifecycle rolled back.");
            return false;
        }
    });
}

void ServiceHost::StopIpcServer() {
}

void ServiceHost::CloseIpcResources() {
}

// 轮询 hal 的两条通道并转发到 PenStatusChannel。
//
// 用轮询而不是等宿主的通知：快照本身可重复读，错过一轮没有代价，而少一条跨进程唤醒路径
// 就少一处可能卡住的地方。事件那侧是管道，Poll 不阻塞，同一个循环里一并取走。
void ServiceHost::AccessoryLoop() {
    // 充电阈值的首读挪到了这里。它走 WMI，一次往返在实测里可以花掉几秒，放在启动路径上
    // 就把 ServiceMain 报 RUNNING 的时刻一起推后，安装程序于是停在「正在启动服务」。
    // 设置窗第一次打开时滑块仍能停在真实位置：这一轮在 250 毫秒内就跑到了。
    bool chargeLimitRead = false;

    while (!m_accessoryStop.load(std::memory_order_acquire)) {
        if (!chargeLimitRead) {
            m_impl->RefreshChargeLimit();
            chargeLimitRead = true;
        }

        SuperviseAccessoryHosts();
        OpenAccessoryChannels();

        // 通道指针只有本线程改，读它们不必上锁；别的线程用同一批指针时会去拿那把互斥。
        Gaokun::Pen::Event penEvent{};
        while (m_penEvents && m_penEvents->Poll(penEvent)) {
            using K = Gaokun::Pen::EventKind;
            const auto kind = static_cast<K>(penEvent.kind);
            if (kind == K::BatteryReminder) {
                m_impl->RaiseNotification(PenStatus::NotificationKind::PenConnected);
            } else if (kind == K::DeviationReminder) {
                m_impl->RaiseNotification(PenStatus::NotificationKind::PenDeviation);
            } else if (kind == K::CurrentFunc) {
                // 这是 CommandSendPenCurrentFunc 的 MCU 回显，不是物理双击。物理手势只由
                // PenEventBridge 分派；把回显再送给托盘会让一次双击产生两个 UI 边沿。
                LOG_INFO("Service", __func__, "MCU",
                         "PenCurrentFunc command echoed value={}.", penEvent.value);
            }
        }

        Gaokun::Keyboard::Event kbdEvent{};
        while (m_kbdEvents && m_kbdEvents->Poll(kbdEvent)) {
            using K = Gaokun::Keyboard::EventKind;
            const auto kind = static_cast<K>(kbdEvent.kind);
            if (kind == K::ConnectResult) {
                m_impl->RaiseNotification(PenStatus::NotificationKind::KeyboardConnected);
            } else if (kind == K::DetachSupportResult) {
                // 命令有没有落地只有这条事件说得准。DetachSupportChanged 是厂商回调的回显，
                // 分不出「谁改的」和「改成了没有」。
                if (kbdEvent.value >= 0) {
                    LOG_INFO("Service", __func__, "MCU",
                             "Keyboard detach support is now {}.",
                             kbdEvent.value != 0 ? "enabled" : "disabled");
                } else {
                    LOG_WARN("Service", __func__, "MCU",
                             "Keyboard detach support command failed (result={}).",
                             kbdEvent.value);
                    m_impl->RaiseNotification(
                        kbdEvent.value == -1
                            ? PenStatus::NotificationKind::KbdDetachSupportUnsupported
                            : PenStatus::NotificationKind::KbdDetachSupportFailed);
                }
            }
        }

        DetectKeyboardArrival();
        PublishStatusSnapshot();
        Sleep(250);
    }
}

// 键盘接上时提示一次。判据取自快照的状态位而不是厂商的连接结果回调，理由写在
// KeyboardArrivalDetector 的说明里。
void ServiceHost::DetectKeyboardArrival() {
    Gaokun::Keyboard::Snapshot kbd{};
    bool read = false;
    if (m_impl->m_kbdHostHealthy.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(m_impl->m_accessoryChannelMutex);
        read = m_kbdSnapshots && m_kbdSnapshots->Read(kbd);
    }
    if (!read) {
        m_impl->m_kbdArrival.Invalidate();
        return;
    }

    using F = Gaokun::Keyboard::Flag;
    const auto has = [&](F f) { return (kbd.flags & static_cast<uint32_t>(f)) != 0; };
    const bool present = has(F::HasConnected) && has(F::Connected);
    const bool hasAttached = has(F::HasDetached);
    const bool attached = hasAttached && !has(F::Detached);

    if (m_impl->m_kbdArrival.Sample(present, hasAttached, attached)) {
        LOG_INFO("Service", __func__, "MCU",
                 "Keyboard connected (present={}, attached={}).", present, attached);
        m_impl->RaiseNotification(PenStatus::NotificationKind::KeyboardConnected);
    }
}

// 巡检两个 hal 宿主。判死的证据有两条：进程没了，或者心跳停了——宿主进程还在而它内部的
// MCU 循环已经停住时，快照的 seqlock 停在最后一帧，只有心跳分辨得出来。
void ServiceHost::SuperviseAccessoryHosts() {
    const auto now = HostSupervisor::Clock::now();

    std::optional<uint32_t> penHeartbeat;
    std::optional<uint32_t> kbdHeartbeat;
    {
        std::lock_guard<std::mutex> lk(m_impl->m_accessoryChannelMutex);
        Gaokun::Pen::Snapshot pen{};
        if (m_penSnapshots && m_penSnapshots->Read(pen)) penHeartbeat = pen.heartbeat;
        Gaokun::Keyboard::Snapshot kbd{};
        if (m_kbdSnapshots && m_kbdSnapshots->Read(kbd)) kbdHeartbeat = kbd.heartbeat;
    }

    const auto penAction =
        m_impl->m_penSupervisor.Tick(now, m_penHost.IsRunning(), penHeartbeat);
    m_impl->m_penHostHealthy.store(m_impl->m_penSupervisor.Healthy(),
                                  std::memory_order_release);
    if (penAction == HostAction::EnterCooldown) {
        LOG_ERROR("Service", __func__, "MCU",
                  "Pen host restarted too often; leaving it down for {} s.",
                  HostSupervisor::kRestartCooldown.count());
    } else if (penAction == HostAction::Restart) {
        // 已知是组件缺失时这只是一次定期重探，不是新故障，别按错误报。
        if (m_impl->m_penSupervisor.VendorMissing()) {
            LOG_INFO("Service", __func__, "MCU",
                     "Probing the pen host again in case PC Manager was reinstalled.");
        } else {
            LOG_ERROR("Service", __func__, "MCU",
                      "Pen host is not responding (running={}, exit={}); restarting it.",
                      m_penHost.IsRunning(), m_penHost.ExitCode());
        }
        const std::wstring path = ResolveHostPath(L"GaokunPenHost.exe");
        bool started = false;
        bool vendorMissing = false;
        if (path.empty()) {
            LOG_ERROR("Service", __func__, "MCU", "GaokunPenHost.exe is no longer where it was.");
        } else {
            // 僵住但没退出的宿主必须先停掉，否则 Start 只会回一句 AlreadyRunning。
            (void)m_penHost.Stop();
            const auto result = m_penHost.Start(path, HostLogArgs());
            started = result == Gaokun::Pen::StartResult::Started ||
                      result == Gaokun::Pen::StartResult::AlreadyRunning;
            vendorMissing = result == Gaokun::Pen::StartResult::ExitedImmediately &&
                            m_penHost.ExitCode() == Gaokun::kHostExitVendorComponentsMissing;
        }
        // 旧句柄对着上一条命的宿主：映射和管道的名字虽然相同，句柄却仍指向已经消失的那份
        // 内核对象，读永远读不到新数据。整体丢掉，下一轮补开。
        if (started) ClosePenChannels();
        if (vendorMissing) {
            // 只在转入这个状态的那一轮报错。后续每次重探都得到同一个结果，逐轮报错就是这条
            // 改动要消掉的那 500 多条日志。
            if (!m_impl->m_penSupervisor.VendorMissing()) {
                LOG_ERROR("Service", __func__, "MCU",
                          "Pen host cannot start: the PC Manager accessory components are "
                          "missing. Reinstall PC Manager to restore it; probing every {} s.",
                          HostSupervisor::kVendorMissingReprobe.count());
            } else {
                LOG_INFO("Service", __func__, "MCU",
                         "Pen host still reports missing PC Manager components.");
            }
            m_impl->m_penSupervisor.NoteVendorMissing(now);
        } else {
            m_impl->m_penSupervisor.NoteRestartResult(started, now);
        }
    }

    const auto kbdAction =
        m_impl->m_kbdSupervisor.Tick(now, m_kbdHost.IsRunning(), kbdHeartbeat);
    m_impl->m_kbdHostHealthy.store(m_impl->m_kbdSupervisor.Healthy(),
                                  std::memory_order_release);
    if (kbdAction == HostAction::EnterCooldown) {
        LOG_ERROR("Service", __func__, "MCU",
                  "Keyboard host restarted too often; leaving it down for {} s.",
                  HostSupervisor::kRestartCooldown.count());
    } else if (kbdAction == HostAction::Restart) {
        if (m_impl->m_kbdSupervisor.VendorMissing()) {
            LOG_INFO("Service", __func__, "MCU",
                     "Probing the keyboard host again in case PC Manager was reinstalled.");
        } else {
            LOG_ERROR("Service", __func__, "MCU",
                      "Keyboard host is not responding (running={}, exit={}); restarting it.",
                      m_kbdHost.IsRunning(), m_kbdHost.ExitCode());
        }
        const std::wstring path = ResolveHostPath(L"GaokunKeyboardHost.exe");
        bool started = false;
        bool vendorMissing = false;
        if (path.empty()) {
            LOG_ERROR("Service", __func__, "MCU",
                      "GaokunKeyboardHost.exe is no longer where it was.");
        } else {
            (void)m_kbdHost.Stop();
            const auto result = m_kbdHost.Start(path, HostLogArgs());
            started = result == Gaokun::Keyboard::StartResult::Started ||
                      result == Gaokun::Keyboard::StartResult::AlreadyRunning;
            vendorMissing = result == Gaokun::Keyboard::StartResult::ExitedImmediately &&
                            m_kbdHost.ExitCode() == Gaokun::kHostExitVendorComponentsMissing;
        }
        if (started) CloseKeyboardChannels();
        if (vendorMissing) {
            if (!m_impl->m_kbdSupervisor.VendorMissing()) {
                LOG_ERROR("Service", __func__, "MCU",
                          "Keyboard host cannot start: the PC Manager accessory components "
                          "are missing. Reinstall PC Manager to restore it; probing every "
                          "{} s.",
                          HostSupervisor::kVendorMissingReprobe.count());
            } else {
                LOG_INFO("Service", __func__, "MCU",
                         "Keyboard host still reports missing PC Manager components.");
            }
            m_impl->m_kbdSupervisor.NoteVendorMissing(now);
        } else {
            m_impl->m_kbdSupervisor.NoteRestartResult(started, now);
        }
    }

    // 状态发布在别的线程上，两个原因位在这一轮的判定全部落定之后再发布。
    m_impl->m_penVendorMissing.store(m_impl->m_penSupervisor.VendorMissing(),
                                     std::memory_order_release);
    m_impl->m_kbdVendorMissing.store(m_impl->m_kbdSupervisor.VendorMissing(),
                                     std::memory_order_release);
}

void ServiceHost::OpenAccessoryChannels() {
    std::lock_guard<std::mutex> lk(m_impl->m_accessoryChannelMutex);
    const auto tryOpen = [](auto& channel, const char* name) {
        if (channel) return;
        using Channel = typename std::remove_reference_t<decltype(channel)>::element_type;
        auto fresh = std::make_unique<Channel>();
        if (!fresh->Open()) return;
        channel = std::move(fresh);
        LOG_INFO("Service", "OpenAccessoryChannels", "MCU", "Accessory channel {} opened.", name);
    };
    tryOpen(m_penSnapshots, "penSnapshots");
    tryOpen(m_penEvents, "penEvents");
    tryOpen(m_penCommands, "penCommands");
    tryOpen(m_kbdSnapshots, "kbdSnapshots");
    tryOpen(m_kbdEvents, "kbdEvents");
    tryOpen(m_kbdCommands, "kbdCommands");
}

void ServiceHost::ClosePenChannels() {
    std::lock_guard<std::mutex> lk(m_impl->m_accessoryChannelMutex);
    m_penSnapshots.reset();
    m_penEvents.reset();
    m_penCommands.reset();
}

void ServiceHost::CloseKeyboardChannels() {
    std::lock_guard<std::mutex> lk(m_impl->m_accessoryChannelMutex);
    m_kbdSnapshots.reset();
    m_kbdEvents.reset();
    m_kbdCommands.reset();
}

// 侧键双击的事件源。厂商的 PenService.dll 提供不了它：那边的 0x2F 回调只是
// CommandSendPenCurrentFunc 的回显，物理双击不经过它——实测独占 MCU 端点双击 45 秒，
// 一条 0x2F 都收不到。MCU 要先被握手（0x7101 + 0x7701 + 0x7701）、并对每一帧回 ACK
// 才会持续上报事件，而那套只有 PenEventBridge 实现。
//
// 上一次重构把它的实例化删掉了（成员声明还留着），双击于是对任何绑定都没有反应，
// 连系统笔菜单也调不起来。
//
// 起不来只丢双击，不该让整个服务起不来，所以失败只记一条 WARN。
void ServiceHost::StartPenEventBridge() {
    auto bridge = std::make_unique<Himax::Pen::PenEventBridge>();

    // 只取侧键。电量、连接、笔尖偏移这些通知已经由 hal 的宿主经 AccessoryLoop 发布，
    // 两边都发会让 notificationSequence 翻倍，托盘就弹两次。
    bridge->SetEventCallback([this](const Himax::Pen::PenEvent& ev) {
        if (m_deviceRuntime) m_deviceRuntime->IngestPenEvent(ev);
    });

    if (!bridge->Start()) {
        LOG_WARN("Service", __func__, "MCU",
                 "PenEventBridge failed to start; the pen side button will not work.");
        return;
    }
    m_impl->m_penEventBridge = std::move(bridge);
    LOG_INFO("Service", __func__, "MCU", "PenEventBridge started (side-button events).");
}

void ServiceHost::StopPenSubsystem() {
    if (m_deviceRuntime) m_deviceRuntime->SetPenCurrentFuncCommandCallback(nullptr);
    if (m_impl->m_penEventBridge) {
        // 先摘回调再 Stop：setter 只换指针，不等在飞的调用结束，真正 join 掉读线程的是
        // Stop()。顺序反过来会让一个晚到的事件打在正在析构的 runtime 上。
        m_impl->m_penEventBridge->SetEventCallback(nullptr);
        m_impl->m_penEventBridge->Stop();
        m_impl->m_penEventBridge.reset();
    }
    if (m_accessoryThread.joinable()) {
        m_accessoryStop.store(true, std::memory_order_release);
        m_accessoryThread.join();
    }
    // 意图先撤，否则这一轮停机会被巡检当成宿主意外死亡。巡检线程此刻已经退出，写它是安全的。
    const auto now = HostSupervisor::Clock::now();
    m_impl->m_penSupervisor.SetDesiredRunning(false, now);
    m_impl->m_kbdSupervisor.SetDesiredRunning(false, now);
    m_impl->m_penHostHealthy.store(false, std::memory_order_release);
    m_impl->m_kbdHostHealthy.store(false, std::memory_order_release);
    m_impl->m_penVendorMissing.store(false, std::memory_order_release);
    m_impl->m_kbdVendorMissing.store(false, std::memory_order_release);
    ClosePenChannels();
    CloseKeyboardChannels();
    (void)m_penHost.Stop();
    (void)m_kbdHost.Stop();
    LOG_INFO("Service", __func__, "MCU", "gaokun-hal accessory hosts stopped.");
}

void ServiceHost::StopSystemStateMonitor() {
    if (!m_impl->m_sysMonitor) {
        return;
    }

    m_impl->m_sysMonitor->Stop();
    m_impl->m_sysMonitor.reset();
    LOG_INFO("Service", __func__, "Monitor", "SystemStateMonitor stopped.");
}

// 状态快照只有一个构造点。此处曾经还有一个 PublishPenStatus，各自填一份 State 再发布，
// 于是两者交替覆盖同一块共享内存：它比这里少了充电上限和厂商服务三个字段，键盘那一块又取自
// PenEventBridge 的缓存而非 hal 快照，笔状态一变，面板上那几项就闪一下。
//
// 少一个来源比让两个来源保持同步可靠。DeviceRuntime 状态变化时只触发重新发布，字段仍由
// 这里统一取——每一项都取权威来源：笔与键盘的身份、电量、固件来自 hal 的宿主快照，橡皮态
// 来自 DeviceRuntime，其余来自服务自己的原子量。

void ServiceHost::PublishTouchProviderState(
        PenStatus::TouchProviderState state,
        uint8_t error) {
    m_impl->m_touchProvider.store(state, std::memory_order_release);
    m_impl->m_touchProviderError.store(
        static_cast<TouchProviderError>(error), std::memory_order_release);
    RepublishPenStatus();
    LOG_INFO("Service", __func__, "Provider",
             "Touch provider state={} error={}.",
             static_cast<unsigned>(state), static_cast<unsigned>(error));
}

// hal 宿主的位置。部署时它们与本服务放在一起；开发时经 hal 符号链接指向 gaokun-hal 的
// 构建目录。不去 PATH 里找：拉起错误的可执行文件会直接抢设备，而现象与「宿主起不来」不同,
// 排查方向也不同。
std::wstring ServiceHost::ResolveHostPath(const wchar_t* exeName) {
    wchar_t self[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};

    std::wstring dir(self, length);
    const size_t slash = dir.find_last_of(L'\\');
    if (slash == std::wstring::npos) return {};
    dir.resize(slash);

    const std::wstring candidates[] = {
        dir + L"\\" + exeName,
        dir + L"\\..\\..\\hal\\build\\Release\\" + exeName,
    };
    for (const auto &candidate : candidates) {
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate;
    }
    return {};
}

std::wstring ServiceHost::ResolveThpHostPath() { return ResolveHostPath(L"GaokunThpHost.exe"); }

namespace {
// 同步跑一个 hal 的一次性组件。色域与充电阈值都是一次动作，没有常驻状态，用进程调用即可；
// 服务是 LocalSystem，充电阈值需要的提权由此而来，托盘自己做不到。
[[nodiscard]] bool RunHalTool(const std::wstring& exePath, const wchar_t* arguments) {
    std::wstring commandLine = L"\"" + exePath + L"\" " + arguments;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(exePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) {
        return false;
    }
    CloseHandle(info.hThread);

    const bool finished = WaitForSingleObject(info.hProcess, 15000) == WAIT_OBJECT_0;
    DWORD code = 1;
    if (finished) (void)GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return finished && code == 0;
}
} // namespace

// 把 hal 的两份快照组装成托盘要读的那一份。
//
// 型号名直接用厂商固件串里带的那个，不再按 modelId 查本地表：本地表维护不动，也曾出过错。
void ServiceHost::PublishStatusSnapshot() {
    PenStatus::State out{};

    // 宿主死了它的快照并不会消失：seqlock 停在最后一帧，读者拿到的是一份自洽的旧状态。
    // 转发它等于告诉托盘键盘还在、电量还是那么多、开关还是那个值，而实际上没有人在读 MCU。
    // 巡检判定宿主不健康时这一整段就不填，has 位保持为假，托盘据此显示为未知。
    const bool penHealthy = m_impl->m_penHostHealthy.load(std::memory_order_acquire);
    const bool kbdHealthy = m_impl->m_kbdHostHealthy.load(std::memory_order_acquire);

    Gaokun::Pen::Snapshot pen{};
    Gaokun::Keyboard::Snapshot kbd{};
    bool penRead = false;
    bool kbdRead = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->m_accessoryChannelMutex);
        penRead = penHealthy && m_penSnapshots && m_penSnapshots->Read(pen);
        kbdRead = kbdHealthy && m_kbdSnapshots && m_kbdSnapshots->Read(kbd);
    }

    if (penRead) {
        using F = Gaokun::Pen::Flag;
        const auto has = [&](F f) { return (pen.flags & static_cast<uint32_t>(f)) != 0; };
        out.hasBatteryLevel = has(F::HasBattery);
        out.batteryLevel = pen.battery;
        out.hasChargingState = has(F::HasCharging);
        out.charging = has(F::Charging);
        out.hasStylusLink = has(F::HasConnected);
        out.stylusLinked = has(F::Connected);
        out.hasDeviceAttached = has(F::HasConnected);
        out.deviceAttached = has(F::Connected);
        out.modelId = pen.moduleId;
        out.hasPenFirmware = has(F::HasFirmware);
        CopyCString(out.penFirmware, sizeof(out.penFirmware), pen.firmware);
        out.hasPenHardware = has(F::HasHardware);
        CopyCString(out.penHardware, sizeof(out.penHardware), pen.hardware);

        out.hasPenSerial = has(F::HasSerial);
        CopyCString(out.penSerial, sizeof(out.penSerial), pen.serial);
        // 产品名由 hal 按模组 ID 查表填好，这里直接转发。本仓库不再自带型号表：先前那份
        // 把 282 当成 CD52，因而把 M-Pen 2 显示为「第一代 M-Pencil」。
        CopyCString(out.modelName, sizeof(out.modelName), pen.modelName);
    }

    if (kbdRead) {
        using F = Gaokun::Keyboard::Flag;
        const auto has = [&](F f) { return (kbd.flags & static_cast<uint32_t>(f)) != 0; };
        out.hasKbdPresent = has(F::HasConnected);
        out.kbdPresent = has(F::Connected);
        out.hasKbdDetached = has(F::HasDetached);
        out.kbdDetached = has(F::Detached);
        out.hasKbdBattery = has(F::HasBattery);
        out.kbdBatteryLevel = kbd.battery;
        out.hasKbdCharging = has(F::HasCharging);
        out.kbdCharging = has(F::Charging);
        out.hasKbdDetachSupport = has(F::HasDetachSupport);
        out.kbdDetachSupport = has(F::DetachSupport);
        CopyCString(out.kbdFirmware, sizeof(out.kbdFirmware), kbd.firmware);
        CopyCString(out.kbdModelName, sizeof(out.kbdModelName), kbd.modelName);
    }

    out.notificationSequence =
        m_impl->m_notificationSequence.load(std::memory_order_acquire);
    out.notificationKind = m_impl->m_notificationKind.load(std::memory_order_acquire);

    // 托盘菜单靠这个值决定哪一项打勾，而不是靠它自己提交过什么。
    out.hasPenButtonMode = true;
    out.penButtonMode = static_cast<uint8_t>(
        m_impl->m_effectivePenButtonMode.load(std::memory_order_acquire));
    out.hasTouchProvider = true;
    out.touchProvider = m_impl->m_touchProvider.load(std::memory_order_acquire);
    const auto providerError = m_impl->m_touchProviderError.load(std::memory_order_acquire);
    out.hasProviderError = providerError != TouchProviderError::None;
    out.providerError = static_cast<uint8_t>(providerError);
    out.hasInputSuppressed = true;
    out.inputSuppressed = m_impl->m_inputSuppressed.load(std::memory_order_acquire);

    // 宿主健康与否本身也发布出去。托盘要区分「键盘不在」和「读键盘的那个进程没了」：前者
    // 该把开关显示成不可用，后者还要连带说明为什么点了没反应。touch_only 不启动这两个宿主，
    // 也就没有可发布的判定。
    out.hasHostHealth = m_runtimeMode == ServiceMode::Full;
    out.penHostHealthy = penHealthy;
    out.kbdHostHealthy = kbdHealthy;
    // 不健康时还要说明是不是「电脑管家的配件组件缺失」这种得由用户去修的确定性失败，否则
    // 界面只能笼统地说宿主没起来，而它其实永远不会自己起来。
    out.penVendorMissing = m_impl->m_penVendorMissing.load(std::memory_order_acquire);
    out.kbdVendorMissing = m_impl->m_kbdVendorMissing.load(std::memory_order_acquire);

    // 橡皮态只有 DeviceRuntime 知道：它由 MCU 的 0x7F EraserToggle 和 ToggleEraser 模式下的
    // 双击共同维护，而厂商的 PenService.dll 根本不暴露 EraserToggle。这里一度改用
    // 「keyFunc == 4」顶替，那是「侧键绑定的是不是橡皮」而不是「当前是不是橡皮」——一个随
    // 用户设置固定不变的值，托盘据它决定切哪个工具，于是永远切不对。
    if (m_deviceRuntime) {
        const auto pen = m_deviceRuntime->GetPenStateSnapshot();
        out.hasEraserActive = pen.hasEraserToggle;
        out.eraserActive = pen.eraserToggle != 0;
    }

    // 只有服务有权查 SCM 的启动类型，所以判据由这里给出。名单里的服务全部禁用才算「已禁用」,
    // 部分禁用仍显示为未禁用：那种状态下再点一次开关会把剩下的补上，比显示成已完成有用。
    const auto vendor = VendorServices::Query();
    out.hasVendorServices = vendor.total > 0;
    out.vendorServicesDisabled = vendor.total > 0 && vendor.disabled == vendor.total;
    out.vendorServicesRunning = vendor.running > 0;
    out.vendorServicesAllRunning = vendor.total > 0 && vendor.running == vendor.total;

    // 充电阈值读缓存，不在这条路径上走 WMI，理由见 m_chargeLimitCache 的说明。
    const uint32_t charge = m_impl->m_chargeLimitCache.load(std::memory_order_relaxed);
    if ((charge & Impl::kChargeLimitValid) != 0) {
        out.hasChargeLimit = true;
        out.chargeLimit = static_cast<uint8_t>(charge & 0xFF);
        out.chargeLimitManual = (charge & Impl::kChargeLimitManual) != 0;
    }

    m_impl->m_penStatusWriter.Publish(out);
}

// 触控由 gaokun-hal 的 ARM64EC 宿主提供，本服务只负责它的生死。自研管线不再参与触控,
// DeviceRuntime 留下来是因为笔与键盘的 MCU 通道仍挂在它上面，而那条通道与触控无关。
bool ServiceHost::StartEGoTouchProvider() {
    const std::wstring host = ResolveThpHostPath();
    if (host.empty()) {
        LOG_ERROR("Service", __func__, "Provider",
                  "GaokunThpHost.exe not found next to the service or under hal/build/Release.");
        return false;
    }

    SetInputSuppressed(false);

    // 同 StartPenSubsystem：宿主没有控制台，日志只能落盘，级别跟随服务。
    const std::wstring hostLogArgs =
        std::wstring(L"--log-level ") + Common::Logger::MinLevelName();

    switch (m_thpHost.Start(host, hostLogArgs)) {
    case Gaokun::Thp::StartResult::Started:
    case Gaokun::Thp::StartResult::AlreadyRunning:
        LOG_INFO("Service", __func__, "Provider", "ARM64EC THP host started.");
        return true;
    case Gaokun::Thp::StartResult::ExitedImmediately:
        // 宿主起来了又马上退出，几乎总是因为 HuaweiThpService 仍持有设备。
        LOG_ERROR("Service", __func__, "Provider",
                  "THP host exited on startup (code={}); is HuaweiThpService still running?",
                  m_thpHost.ExitCode());
        return false;
    case Gaokun::Thp::StartResult::HostNotFound:
        LOG_ERROR("Service", __func__, "Provider", "THP host missing at the resolved path.");
        return false;
    default:
        LOG_ERROR("Service", __func__, "Provider",
                  "Failed to launch the THP host (err={}).", GetLastError());
        return false;
    }
}

// 补投启动竞态里被运行时丢掉的那次唤醒事件。放在这里而不是 DeviceRuntime::Start 内部:
// 那里补投会把「Start 发布 running 之后到达的 Shutdown」盖掉(SystemPowerPolicy 守着
// 这条边界),而这里已经在 Start 之外,且服务本来就知道最后一次的显示/盖子状态。
void ServiceHost::ReplayLastWakeEvent() {
    if (!m_deviceRuntime) return;
    std::optional<Host::SystemStateEvent> pending;
    {
        std::lock_guard<std::mutex> lk(m_impl->m_lastWakeEventMutex);
        pending = m_impl->m_lastWakeEvent;
    }
    if (!pending) return;
    LOG_INFO("Service", __func__, "Policy", "Replaying last wake event ({}) after runtime start.",
             Host::ToString(pending->type));
    m_deviceRuntime->IngestPolicyEvent(TranslateSystemStateEvent(*pending));
}

bool ServiceHost::StopEGoTouchProvider() {
    SetInputSuppressed(false);
    if (!m_thpHost.IsRunning()) return true;

    // 宿主收到停止事件后自行走完 ThpFuncStop 再退出。超时意味着收尾没走通，此时设备可能
    // 停在中间状态；交还原厂服务时它会自己复位 AFE，但这里必须留下记录。
    const bool clean = m_thpHost.Stop(std::chrono::seconds(15));
    if (!clean) {
        LOG_ERROR("Service", __func__, "Provider",
                  "THP host did not exit within the timeout and was terminated.");
    } else {
        LOG_INFO("Service", __func__, "Provider", "ARM64EC THP host stopped.");
    }
    return clean;
}

void ServiceHost::SetInputSuppressed(bool suppressed) {
    if (m_deviceRuntime) {
        m_deviceRuntime->SetInputSuppressed(suppressed);
    }
    const bool wasSuppressed =
        m_impl->m_inputSuppressed.exchange(suppressed, std::memory_order_acq_rel);
    const auto now = std::chrono::steady_clock::now();
    if (suppressed) {
        if (!wasSuppressed) m_impl->m_inputSuppressionStarted = now;
        m_impl->m_inputSuppressionDeadline = now + kInputSuppressionTimeout;
    } else {
        m_impl->m_inputSuppressionDeadline = {};
    }
    if (suppressed != wasSuppressed) {
        if (suppressed) {
            LOG_INFO("Service", __func__, "PenControl",
                     "OneNote input suppression enabled (watchdog={} ms).",
                     kInputSuppressionTimeout.count());
        } else {
            const auto elapsed = m_impl->m_inputSuppressionStarted ==
                                     std::chrono::steady_clock::time_point{}
                ? std::chrono::milliseconds(0)
                : std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - m_impl->m_inputSuppressionStarted);
            LOG_INFO("Service", __func__, "PenControl",
                     "OneNote input suppression released after {} ms.",
                     elapsed.count());
            m_impl->m_inputSuppressionStarted = {};
        }
    }
    RepublishPenStatus();
}

void ServiceHost::TickInputSuppressionTimeout() {
    if (!m_impl->m_inputSuppressed.load(std::memory_order_acquire)) return;
    const auto deadline = m_impl->m_inputSuppressionDeadline;
    if (deadline == std::chrono::steady_clock::time_point{} ||
        std::chrono::steady_clock::now() < deadline) {
        return;
    }
    LOG_WARN("Service", __func__, "PenControl",
             "OneNote input suppression timed out; releasing the VHF gate.");
    SetInputSuppressed(false);
}

void ServiceHost::RepublishPenStatus() {
    PublishStatusSnapshot();
}

void ServiceHost::ApplyPenButtonMode(PenButtonMode mode, const char* source, bool persist) {
    {
        std::lock_guard<std::mutex> lk(m_impl->m_penButtonApplyMutex);
        if (m_configState.penButtonMode == mode) {
            LOG_INFO("Service", __func__, "PenControl",
                     "{}: pen button mode already {}; nothing to apply.", source, ToString(mode));
        } else {
            m_configState.penButtonMode = mode;
            // 经现行的策略应用路径生效，与 IPC 配置重载走的是同一个入口。
            m_configRuntime.WriteServiceState(m_configState);
            ApplyServiceConfigToRuntime(m_configState);
            m_impl->m_effectivePenButtonMode.store(mode, std::memory_order_release);
            LOG_INFO("Service", __func__, "PenControl",
                     "{}: pen button mode applied: {}.", source, ToString(mode));
        }

        if (persist && !PenSettingsStore::SavePenButtonMode(mode)) {
            // 落盘失败不回滚：本次会话里模式已经生效，重启后退回默认值好过让选择当场失效。
            LOG_WARN("Service", __func__, "PenControl",
                     "{}: pen button mode {} applied but not persisted.", source, ToString(mode));
        }
    }

    RepublishPenStatus();
}

void ServiceHost::HandlePenControlCommand(const PenControl::Command& command) {
    if (command.hasPenButtonMode) {
        // 同会话的任意进程都写得进这个通道，所以提交的值一律按枚举映射校验，非法值不落地。
        const auto mode = PenButtonModeFromNumeric(static_cast<int32_t>(command.penButtonMode));
        if (!mode) {
            LOG_WARN("Service", __func__, "PenControl",
                     "Rejecting pen button mode {} from the control channel: not a known enum value.",
                     static_cast<unsigned>(command.penButtonMode));
        } else {
            ApplyPenButtonMode(*mode, "PenControl", true);
        }
    }

    if (command.hasProviderLease && m_impl->m_touchProviderCoordinator) {
        switch (command.providerLease) {
        case PenControl::ProviderLeaseCommand::AcquireOrRenew:
            (void)m_impl->m_touchProviderCoordinator->AcquireOrRenew(
                TouchProviderCoordinator::Clock::now());
            break;
        case PenControl::ProviderLeaseCommand::Release:
            (void)m_impl->m_touchProviderCoordinator->Release();
            break;
        default:
            LOG_WARN("Service", __func__, "PenControl",
                     "Rejecting unknown provider lease command {}.",
                     static_cast<unsigned>(command.providerLease));
            break;
        }
    }

    if (command.hasInputSuppression) {
        switch (command.inputSuppression) {
        case PenControl::InputSuppressionCommand::BeginOrRenew:
            SetInputSuppressed(true);
            break;
        case PenControl::InputSuppressionCommand::End:
            SetInputSuppressed(false);
            break;
        default:
            LOG_WARN("Service", __func__, "PenControl",
                     "Rejecting unknown input suppression command {}.",
                     static_cast<unsigned>(command.inputSuppression));
            break;
        }
    }

    // 一条提交可以同时带多个字段，所以每个分支都只处理自己那一段、不提前 return——
    // 早返回会让一个非法的键盘值顺带丢掉同一条提交里其余合法的命令。
    if (command.hasKbdDetachSupport) {
        const bool enable =
            command.kbdDetachSupport == PenControl::KbdDetachSupportCommand::Enable;
        const bool known =
            enable || command.kbdDetachSupport == PenControl::KbdDetachSupportCommand::Disable;

        if (!known) {
            LOG_WARN("Service", __func__, "PenControl",
                     "Rejecting unknown keyboard detach support command {}.",
                     static_cast<unsigned>(command.kbdDetachSupport));
        } else {
            // 常驻宿主的命令管道，写一次就返回。这里不能等：本线程同时在给触控租约打点，
            // 阻塞十秒会让租约被判过期，触控当场交还原厂。
            //
            // 一次性宿主那条退路也因此不再走。它要另起一个进程、最长等十秒，而那个第二实例
            // 还会和常驻宿主抢同一个 MCU 端点。宿主起不来时改这个开关本来就不会成功，
            // 不如当场告诉用户。
            bool sent = false;
            if (m_impl->m_kbdHostHealthy.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lk(m_impl->m_accessoryChannelMutex);
                sent = m_kbdCommands && m_kbdCommands->SetDetachSupport(enable);
            }
            if (sent) {
                // 这里不补发 Get：宿主内部会读回确认，实际值经快照回流到状态通道，落地结果
                // 另经 DetachSupportResult 事件回来。本次调用返回时通道还是旧值，不是漏更新。
                LOG_INFO("Service", __func__, "PenControl",
                         "Keyboard detach support command sent ({}).",
                         enable ? "enable" : "disable");
            } else {
                LOG_WARN("Service", __func__, "PenControl",
                         "Dropping keyboard detach support command: the keyboard host is not "
                         "available.");
                m_impl->RaiseNotification(PenStatus::NotificationKind::KbdDetachSupportFailed);
                RepublishPenStatus();
            }
        }
    }

    // 充电阈值。范围在提交侧已经挡过，这里再挡一次：控制通道是共享内存，写者不止一个版本。
    // 0 是「交还厂商智能充电」的哨兵，不是一个百分比。
    if (command.hasChargeLimit) {
        const bool handBack = command.chargeLimit == 0;
        if (!handBack && (command.chargeLimit < 50 || command.chargeLimit > 100)) {
            LOG_WARN("Service", __func__, "PenControl",
                     "Rejecting out-of-range charge limit {}.",
                     static_cast<unsigned>(command.chargeLimit));
        } else {
            const std::wstring host = ResolveHostPath(L"GaokunPower.exe");
            wchar_t args[64];
            if (handBack) {
                swprintf_s(args, L"--smart");
            } else {
                swprintf_s(args, L"--limit %u", static_cast<unsigned>(command.chargeLimit));
            }
            if (host.empty() || !RunHalTool(host, args)) {
                if (handBack) {
                    LOG_WARN("Service", __func__, "PenControl",
                             "Handing charging back to the vendor failed.");
                } else {
                    LOG_WARN("Service", __func__, "PenControl",
                             "Charge limit {} failed to apply.",
                             static_cast<unsigned>(command.chargeLimit));
                }
            } else if (handBack) {
                LOG_INFO("Service", __func__, "PenControl",
                         "Charging handed back to the vendor's smart mode.");
            } else {
                LOG_INFO("Service", __func__, "PenControl", "Charge limit set to {}.",
                         static_cast<unsigned>(command.chargeLimit));
            }
            // 无论成功与否都重读一次。失败时缓存要跟上硬件的真实值，否则界面会一直显示
            // 用户刚才拖到的那个数字，而机器根本没有按它执行。
            m_impl->RefreshChargeLimit();
        }
    }

    // 华为后台服务的总开关。只有服务有权改 SCM 配置，托盘做不到，所以这一步必须在这里做。
    if (command.hasVendorServices) {
        switch (command.vendorServices) {
        case PenControl::VendorServicesCommand::Disable: {
            const bool ok = VendorServices::DisableAll();
            const auto status = VendorServices::Query();
            LOG_INFO("Service", __func__, "Vendor",
                     "Vendor services disabled ({}/{}), complete={}.",
                     status.disabled, status.total, ok);
            break;
        }
        case PenControl::VendorServicesCommand::Restore: {
            const bool ok = VendorServices::RestoreAll();
            LOG_INFO("Service", __func__, "Vendor", "Vendor services restored, complete={}.", ok);
            break;
        }
        default:
            LOG_WARN("Service", __func__, "Vendor", "Rejecting unknown vendor services command {}.",
                     static_cast<unsigned>(command.vendorServices));
            break;
        }
    }

    // 色域。
    if (command.hasColorMode) {
        const wchar_t* args = nullptr;
        switch (command.colorMode) {
        case PenControl::ColorModeCommand::Srgb: args = L"--preset sRGB"; break;
        case PenControl::ColorModeCommand::DisplayP3: args = L"--preset DisplayP3"; break;
        case PenControl::ColorModeCommand::Reset: args = L"--reset"; break;
        default: break;
        }
        if (!args) {
            LOG_WARN("Service", __func__, "PenControl", "Rejecting unknown colour mode {}.",
                     static_cast<unsigned>(command.colorMode));
        } else {
            const std::wstring host = ResolveHostPath(L"GaokunDisplay.exe");
            if (host.empty() || !RunHalTool(host, args)) {
                LOG_WARN("Service", __func__, "PenControl", "Colour mode command failed.");
            } else {
                LOG_INFO("Service", __func__, "PenControl", "Colour mode applied.");
            }
        }
    }
}

namespace {
// 等待切片。有提交事件时它只决定 Stop 的响应上限（事件一置位就返回）；事件缺失时它同时
// 是轮询周期——读一次 32 字节的 seqlock 比这次等待本身还便宜，没必要为省这点开销把停机
// 延迟拉长到秒级。
constexpr DWORD kPenControlWaitSliceMs = 250;
} // namespace

// 把监视线程记下的电源事件交给 coordinator。只在控制线程上调用。
void ServiceHost::DispatchPendingPowerEvent() {
    if (!m_impl->m_touchProviderCoordinator) return;

    const auto now = TouchProviderCoordinator::Clock::now();
    const int event = m_impl->m_pendingPowerEvent.exchange(kPowerEventNone,
                                                           std::memory_order_acq_rel);
    if (event == kPowerEventSuspend) {
        if (m_impl->m_powerSuspendDueAt == std::chrono::steady_clock::time_point{}) {
            m_impl->m_powerSuspendDueAt = now + kPowerSuspendDebounce;
        }
    } else if (event == kPowerEventResume) {
        // 还没动手就醒了，这次息屏当没发生过。OnResume 照常调用：宿主没停时它只延租约，
        // 而唤醒后本来就该给托盘的心跳留出宽限。
        m_impl->m_powerSuspendDueAt = {};
        m_impl->m_touchProviderCoordinator->OnResume(now);
        return;
    }

    if (m_impl->m_powerSuspendDueAt != std::chrono::steady_clock::time_point{} &&
        now >= m_impl->m_powerSuspendDueAt) {
        m_impl->m_powerSuspendDueAt = {};
        m_impl->m_touchProviderCoordinator->OnSuspend(now);
    }
}

void ServiceHost::PenControlThreadMain() {
    while (!m_impl->m_penControlStop.load(std::memory_order_acquire)) {
        (void)m_impl->m_penControlHost.WaitForSubmit(kPenControlWaitSliceMs);
        if (m_impl->m_penControlStop.load(std::memory_order_acquire)) {
            break;
        }

        DispatchPendingPowerEvent();

        PenControl::Command command{};
        if (m_impl->m_penControlHost.PollCommand(command)) {
            HandlePenControlCommand(command);
        }
        if (m_impl->m_touchProviderCoordinator) {
            m_impl->m_touchProviderCoordinator->Tick(
                TouchProviderCoordinator::Clock::now());
        }
        TickInputSuppressionTimeout();
    }
}

void ServiceHost::StartPenControlChannel() {
    if (!m_impl->m_penControlHost.Open()) {
        // Non-fatal: 托盘改不了模式而已，配置和默认值仍然生效。
        LOG_WARN("Service", __func__, "PenControl",
                 "Pen control channel unavailable; tray cannot change the pen button mode.");
        return;
    }

    if (!m_impl->m_penControlHost.HasEventChannel()) {
        LOG_WARN("Service", __func__, "PenControl",
                 "Pen control submit event unavailable; falling back to polling.");
    }

    m_impl->m_penControlStop.store(false, std::memory_order_release);
    m_impl->m_penControlThread = std::thread([this] { PenControlThreadMain(); });
    LOG_INFO("Service", __func__, "PenControl", "Pen control channel opened.");
}

void ServiceHost::StopPenControlChannel() {
    // 先退线程再关 Host：线程正在 WaitForSubmit 里持有 Host 的事件句柄，反过来关会让它
    // 等在一个已经关闭的句柄上。
    m_impl->m_penControlStop.store(true, std::memory_order_release);
    if (m_impl->m_penControlThread.joinable()) {
        m_impl->m_penControlThread.join();
    }
    m_impl->m_penControlHost.Close();
}

void ServiceHost::StopRuntimeSubsystem() {
    // 控制通道的线程会调进 DeviceRuntime，必须先于它退出，且与 StartRuntimeAndPipeline
    // 里的创建位置对称。DeviceRuntime 不在时这里是空操作。
    StopPenControlChannel();

    // 先交还 Huawei 并等到 Running，再拆掉 runtime 对象。正常停止、卸载和服务关闭都走
    // 这里；托盘被强杀则由上面的租约超时走同一个 coordinator 分支。
    if (m_impl->m_touchProviderCoordinator) {
        m_impl->m_touchProviderCoordinator->Shutdown();
        m_impl->m_touchProviderCoordinator.reset();
    }

    if (!m_deviceRuntime) {
        return;
    }
    // 拆 runtime 之前先摘掉两个回调，缩小晚到的笔事件打到即将析构的 Impl 上的窗口。
    // 摘除本身不构成安全保证：setter 只换指针，不等待已经在飞的调用；真正让在飞调用结束的
    // 是随后的 Stop() 与 reset()——它们 join 掉产生回调的线程。
    m_deviceRuntime->SetPenStateChangedCallback(nullptr);
    m_deviceRuntime->SetPenDoubleClickCallback(nullptr);

    m_deviceRuntime->Stop();
    m_deviceRuntime.reset();
    LOG_INFO("Service", __func__, "Device", "DeviceRuntime stopped.");
}

void ServiceHost::Stop() {
    m_impl->m_lifecycle.RunStop([this] {
        // Quiesce every callback producer before destroying its downstream objects.
        ServiceLifecycleCoordinator::Stop(*this);
        LOG_INFO("Service", "Stop", "Shutdown", "All modules stopped.");
    });
}

// 与 IPC 无关的字符串助手，因此不在下面那个守卫里：状态发布在所有配置里都要编，
// 而笔状态通道在 Release 中同样要工作。
void ServiceHost::CopyCString(char* dst, size_t dstSize, std::string_view src) {
    if (!dst || dstSize == 0) return;
    std::memset(dst, 0, dstSize);
    if (src.empty()) return;
    const size_t n = std::min(dstSize - 1, src.size());
    std::memcpy(dst, src.data(), n);
}

} // namespace Service
