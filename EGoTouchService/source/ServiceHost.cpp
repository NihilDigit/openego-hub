#include "ServiceHost.h"

#include "ServiceLifecycleCoordinator.h"
#include "ConfigRuntime.h"
#include "SystemStateMonitor.h"
#include "runtime/DeviceRuntime.h"
#include "penevt/PenEventBridge.h"
#include "penpress/PenPressureReader.h"
#include "SolverBuildConfig.h"
#include "SolverTypes.h"
#include "ServiceConfigCore.h"
#include "PenSettingsStore.h"
#include "TouchProviderCoordinator.h"
#include "PenControlChannel.h"
#include "PenStatusChannel.h"

#if EGOTOUCH_SERVICE_ENABLE_IPC
#include "GuiLogSink.h"
#include "Ipc/IpcPipeServer.h"
#include "Ipc/IpcSecurity.h"
#include "Ipc/SharedFrameBuffer.h"
#include "Ipc/ConfigSync.h"
#include "Ipc/IpcProtocol.h"
#endif

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
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Service {

struct ServiceHost::Impl {
    ServiceLifecycleStateMachine m_lifecycle;
    // Read-only pen state broadcast for the tray companion. Present in every build:
    // unlike the IPC control surface, a shipping service still has to publish this.
    PenStatus::Writer m_penStatusWriter;
    // 反方向的小通道：托盘菜单选了哪个侧键模式。服务创建，托盘只写。
    PenControl::Host m_penControlHost;
    std::thread m_penControlThread;
    std::atomic<bool> m_penControlStop{false};
    // 托盘要显示当前哪一项生效，而 PublishPenStatus 跑在 DeviceRuntime 的回调线程上，
    // 不能去读会被配置路径改写的 m_configState。应用路径写这份原子镜像，发布路径只读它。
    std::atomic<PenButtonMode> m_effectivePenButtonMode{PenButtonMode::WindowsInk};
    std::atomic<PenStatus::TouchProviderState> m_touchProvider{
        PenStatus::TouchProviderState::Unknown};
    std::atomic<TouchProviderError> m_touchProviderError{TouchProviderError::None};
    std::unique_ptr<TouchProviderCoordinator> m_touchProviderCoordinator;
    std::atomic<bool> m_inputSuppressed{false};
    std::atomic<uint32_t> m_notificationSequence{0};
    std::atomic<PenStatus::NotificationKind> m_notificationKind{
        PenStatus::NotificationKind::None};
    // 键盘「分离后无线连接」的镜像。真值在 MCU 侧，服务只缓存最近一次应答：写入方是
    // PenEventBridge 的读线程，读取方是 DeviceRuntime 回调线程上的 PublishPenStatus。
    // 未收到过应答时 known 为 false，此时状态通道不置 has 位，托盘据此把该项显示为不可用。
    std::atomic<bool> m_kbdDetachSupportKnown{false};
    std::atomic<bool> m_kbdDetachSupport{false};
    // 键盘状态镜像。含字符串，用不了原子，改由互斥保护：写入方是 MCU 读线程，
    // 读取方是 DeviceRuntime 回调线程上的 PublishPenStatus。
    std::mutex m_kbdStateMutex;
    Himax::Pen::PenEventBridge::KbdState m_kbdState;
    std::chrono::steady_clock::time_point m_lastKbdConnectionNotification{};
    std::chrono::steady_clock::time_point m_inputSuppressionStarted{};
    std::chrono::steady_clock::time_point m_inputSuppressionDeadline{};
    // 侧键模式有 IPC 配置重载和托盘控制通道两个写入方，串行化它们对 m_configState 的读改写。
    std::mutex m_penButtonApplyMutex;
    std::unique_ptr<Host::SystemStateMonitor> m_sysMonitor;
    // Serializes startup-time IPC access with Pen object publication.
    std::mutex m_penSubsystemMutex;
    // BT MCU 事件通道 (col00)：设备发现 + 握手 + ACK + 0x7D01 回显 —— 仅 Full 模式
    std::unique_ptr<Himax::Pen::PenEventBridge> m_penEventBridge;
    // BT MCU 压力通道 (col01)：'U' 报文读取 + 频率 / 压感数据 —— 仅 Full 模式
    std::unique_ptr<Himax::Pen::PenPressureReader> m_penPressureReader;

#if EGOTOUCH_SERVICE_ENABLE_IPC
    // IPC
    Ipc::IpcPipeServer m_ipcServer;
    Ipc::SharedFrameWriter m_frameWriter;
    Ipc::ConfigDirtyFlag m_configDirty;
    bool m_debugMode = false;
    HANDLE m_logEvent = nullptr;
    HANDLE m_penEvent = nullptr;

    std::vector<Ipc::DebugFieldSchemaWire> m_debugSchema;
    uint16_t m_debugSchemaVersion = 0;
    uint32_t m_debugSchemaHash = 0;
    std::mutex m_debugFrameMutex;
    Solvers::HeatmapFrame m_latestDebugFrame;
    Solvers::HeatmapFrame m_latestMasterTouchFrame;
    Ipc::SharedFrameData m_latestMasterSharedFrame{};
    bool m_hasLatestDebugFrame = false;
    bool m_hasLatestMasterTouchFrame = false;
    bool m_hasLatestMasterSharedFrame = false;
#endif

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

bool SetHuaweiThpDisabled() {
    return WithHuaweiService(SERVICE_CHANGE_CONFIG,
        [](SC_HANDLE service) {
            return ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_DISABLED,
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

#if EGOTOUCH_SERVICE_ENABLE_IPC
void PreserveMasterTouchDebugState(Solvers::HeatmapFrame& dst,
                                   const Solvers::HeatmapFrame& cached) {
    std::memcpy(dst.heatmapMatrix, cached.heatmapMatrix, sizeof(dst.heatmapMatrix));
    dst.masterSuffix = cached.masterSuffix;
    dst.masterSuffixValid = cached.masterSuffixValid;
    dst.touch.output = cached.touch.output;
#if EGOTOUCH_DIAG
    dst.touch.debug = cached.touch.debug;
#endif
}

Ipc::IpcStatusCode ToIpcStatus(ServiceRuntimeStatusCode status) noexcept {
    return static_cast<Ipc::IpcStatusCode>(static_cast<uint8_t>(status));
}

Ipc::ConfigV3MutationStatus ToIpcMutationStatus(ConfigV3MutationStatus status) noexcept {
    return static_cast<Ipc::ConfigV3MutationStatus>(static_cast<uint8_t>(status));
}

Ipc::PenIdentityProtocolHint ToIpcPenIdentityProtocolHint(
    Solvers::StylusProtocolHint hint) noexcept {
    switch (hint) {
    case Solvers::StylusProtocolHint::Hpp2:
        return Ipc::PenIdentityProtocolHint::Hpp2;
    case Solvers::StylusProtocolHint::Hpp3:
        return Ipc::PenIdentityProtocolHint::Hpp3;
    default:
        return Ipc::PenIdentityProtocolHint::Auto;
    }
}

bool BuildConfigV3PageResponse(Ipc::IpcCommand command,
                               const Ipc::IpcRequest& req,
                               const ConfigRuntime::ConfigV3Blob& blob,
                               Ipc::IpcResponse& resp) {
    if (req.paramLen != sizeof(Ipc::ConfigV3PageRequestWire)) {
        Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidRequest);
        return false;
    }

    Ipc::ConfigV3PageRequestWire pageRequest{};
    std::memcpy(&pageRequest, req.param, sizeof(pageRequest));
    if (pageRequest.wireVersion != Ipc::kIpcProtocolVersion || pageRequest.flags != 0) {
        Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidRequest);
        return false;
    }

    const uint8_t expectedKind = command == Ipc::IpcCommand::GetConfigCatalogV3
        ? static_cast<uint8_t>(Ipc::ConfigV3PayloadKind::Catalog)
        : static_cast<uint8_t>(Ipc::ConfigV3PayloadKind::Snapshot);
    if (pageRequest.payloadKind != expectedKind) {
        Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidRequest);
        return false;
    }

    const uint32_t totalBytes = static_cast<uint32_t>(blob.bytes.size());
    if (pageRequest.offset > totalBytes) {
        Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidRequest);
        return false;
    }

    const uint32_t capacity = Ipc::ConfigV3PageCapacityBytes();
    uint32_t requestedBytes = pageRequest.maxBytes == 0 ? capacity : pageRequest.maxBytes;
    requestedBytes = std::min<uint32_t>(requestedBytes, capacity);
    const uint32_t availableBytes = totalBytes - pageRequest.offset;
    const uint32_t pageBytes = std::min<uint32_t>(requestedBytes, availableBytes);

    Ipc::ConfigV3PageResponseHeaderWire header{};
    header.wireVersion = Ipc::kIpcProtocolVersion;
    header.payloadKind = expectedKind;
    header.flags = 0;
    header.headerBytes = static_cast<uint16_t>(sizeof(Ipc::ConfigV3PageResponseHeaderWire));
    header.pageBytes = static_cast<uint16_t>(pageBytes);
    header.totalBytes = totalBytes;
    header.schemaVersion = blob.schemaVersion;
    header.snapshotVersion = blob.snapshotVersion;
    header.offset = pageRequest.offset;
    header.checksum = blob.checksum;

    const uint32_t dataLen = static_cast<uint32_t>(header.headerBytes) + pageBytes;
    if (!Ipc::IsValidConfigV3PageResponse(header, dataLen)) {
        Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InternalError);
        return false;
    }

    std::memcpy(resp.data, &header, sizeof(header));
    if (pageBytes != 0) {
        std::memcpy(resp.data + sizeof(header), blob.bytes.data() + pageRequest.offset, pageBytes);
    }
    resp.dataLen = static_cast<uint16_t>(dataLen);
    Ipc::MarkSuccess(resp);
    return true;
}

void MarkLegacyConfigCommandUnsupported(Ipc::IpcCommand command, Ipc::IpcResponse& resp) {
    Ipc::MarkFailure(resp, Ipc::IpcStatusCode::UnsupportedCommand);
    LOG_WARN("Service", __func__, "IPC", "Legacy config IPC command {} is unsupported; use config v3 IPC.",
             static_cast<unsigned int>(command));
}
#endif

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

#if EGOTOUCH_SERVICE_ENABLE_IPC
ReloadServiceConfigResult ServiceHost::HandleReloadServiceConfig(
    const ServiceConfigState& reloadedConfig) {
    const bool modeChanged = (m_configState.mode != reloadedConfig.mode);
    const bool autoModeChanged = (m_configState.autoMode != reloadedConfig.autoMode);
    const bool stylusVhfChanged = (m_configState.stylusVhfEnabled != reloadedConfig.stylusVhfEnabled);
    const bool penButtonModeChanged = (m_configState.penButtonMode != reloadedConfig.penButtonMode);
    const bool penButtonRouteChanged =
        (m_configState.penButtonRoute != reloadedConfig.penButtonRoute) ||
        (m_configState.penButtonRouteExplicit != reloadedConfig.penButtonRouteExplicit);
    const bool policyChanged =
        autoModeChanged || stylusVhfChanged ||
        penButtonModeChanged || penButtonRouteChanged;

    ReloadServiceConfigResult result = DiffServiceConfig(m_configState, reloadedConfig, m_deviceRuntime && policyChanged);

    if (modeChanged) {
        result.changedFields |= ToServiceConfigFieldBit(ServiceConfigField::Mode);
        result.restartRequiredFields |= ToServiceConfigFieldBit(ServiceConfigField::Mode);
        LOG_WARN("Service", __func__, "IPC",
                 "[Service].mode changed from {} to {}; runtime topology remains {} until service restart.",
                 ServiceModeToConfig(m_configState.mode),
                 ServiceModeToConfig(reloadedConfig.mode),
                 ServiceModeToConfig(m_runtimeMode));
    }

    if (autoModeChanged) {
        result.changedFields |= ToServiceConfigFieldBit(ServiceConfigField::AutoMode);
        LOG_INFO("Service", __func__, "IPC",
                 "[Service].auto_mode reloaded to {} (immediate apply).",
                 reloadedConfig.autoMode ? 1 : 0);
    }

    if (stylusVhfChanged) {
        result.changedFields |= ToServiceConfigFieldBit(ServiceConfigField::StylusVhfEnabled);
        LOG_INFO("Service", __func__, "IPC",
                 "[Service].stylus_vhf_enabled reloaded to {} (immediate apply).",
                 reloadedConfig.stylusVhfEnabled ? 1 : 0);
    }

    if (penButtonModeChanged) {
        result.changedFields |= ToServiceConfigFieldBit(ServiceConfigField::PenButtonMode);
        LOG_INFO("Service", __func__, "IPC",
                 "[Service].pen_button_mode reloaded to {} (immediate apply).",
                 static_cast<int>(reloadedConfig.penButtonMode));
    }

    if (penButtonRouteChanged) {
        result.changedFields |= ToServiceConfigFieldBit(ServiceConfigField::PenButtonRoute);
        LOG_INFO("Service", __func__, "IPC",
                 "[Service].pen_button_route reloaded to {} (immediate apply).",
                 static_cast<int>(reloadedConfig.penButtonRoute));
    }

    if (m_deviceRuntime && policyChanged) {
        m_deviceRuntime->ApplyServicePolicy(
            reloadedConfig.autoMode, reloadedConfig.stylusVhfEnabled,
            reloadedConfig.penButtonMode, reloadedConfig.penButtonRoute,
            reloadedConfig.penButtonRouteExplicit);
        result.appliedFields |= static_cast<uint8_t>(
            (autoModeChanged ? ToServiceConfigFieldBit(ServiceConfigField::AutoMode) : 0u) |
            (stylusVhfChanged ? ToServiceConfigFieldBit(ServiceConfigField::StylusVhfEnabled) : 0u) |
            (penButtonModeChanged ? ToServiceConfigFieldBit(ServiceConfigField::PenButtonMode) : 0u) |
            (penButtonRouteChanged ? ToServiceConfigFieldBit(ServiceConfigField::PenButtonRoute) : 0u));
    }

    ServiceConfigState activeConfig = reloadedConfig;
    if (modeChanged) {
        activeConfig.mode = m_configState.mode;
    }
    {
        std::lock_guard<std::mutex> lk(m_impl->m_penButtonApplyMutex);
        m_configState = activeConfig;
        // 托盘读的是这份镜像，IPC 改了模式也要同步过去。
        m_impl->m_effectivePenButtonMode.store(activeConfig.penButtonMode,
                                               std::memory_order_release);
    }
    if (penButtonModeChanged) {
        RepublishPenStatus();
    }
    return result;
}
#endif

bool ServiceHost::StartRuntimeAndPipeline() {
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
    m_deviceRuntime->ApplyConfigStore(startupConfig);

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
#if EGOTOUCH_SERVICE_ENABLE_IPC
    BuildDebugSchema();
#endif

    if (m_impl->m_penStatusWriter.Open()) {
        m_deviceRuntime->SetPenStateChangedCallback(
            [this](const RuntimePenState& state) { PublishPenStatus(state); });
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
        const bool ok = SetHuaweiThpDisabled();
        if (!ok) {
            LOG_ERROR("Service", "DisableHuawei", "Provider",
                      "Failed to disable HuaweiThpService (err={}).", GetLastError());
        }
        return ok;
    };
    providerOps.startEGo = [this] { return StartEGoTouchProvider(); };
    providerOps.stopEGo = [this] { return StopEGoTouchProvider(); };
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

bool ServiceHost::StartIpcSubsystem() {
#if EGOTOUCH_SERVICE_ENABLE_IPC
#ifdef _DEBUG
    // Service creates Global\\ mapping in debug builds.
    if (!m_impl->m_frameWriter.Create(Ipc::kSharedFrameName)) {
        LOG_WARN("Service", __func__, "IPC", "Failed to create shared memory; App debug will be disabled.");
    } else {
        LOG_INFO("Service", __func__, "IPC", "Shared memory created for App connection.");
    }
#endif

    {
        Ipc::ScopedSecurityDescriptor sd;
        SECURITY_ATTRIBUTES sa{};
        if (!Ipc::BuildAdminOnlySecurityAttributes(sa, sd)) {
            LOG_WARN("Service", __func__, "IPC", "Build event security descriptor failed: {}", GetLastError());
        } else {
            m_impl->m_logEvent = CreateEventW(&sa, FALSE, FALSE, Ipc::kLogReadyEventName);
            if (!m_impl->m_logEvent) {
                LOG_WARN("Service", __func__, "IPC", "CreateEvent failed for LogReadyEvent: {}", GetLastError());
            } else {
                Common::GuiLogSink::Instance()->SetNotifyEvent(m_impl->m_logEvent);
            }

            m_impl->m_penEvent = CreateEventW(&sa, FALSE, FALSE, Ipc::kPenReadyEventName);
            if (!m_impl->m_penEvent) {
                LOG_WARN("Service", __func__, "IPC", "CreateEvent failed for PenReadyEvent: {}", GetLastError());
            }
        }
    }

    if (!m_impl->m_configDirty.Open()) {
        LOG_WARN("Service", __func__, "IPC", "Legacy config dirty flag unavailable.");
    }
    m_impl->m_ipcServer.SetCommandHandler(
        [this](const Ipc::IpcRequest& req) {
            return HandleIpcCommand(req);
        });
    if (!m_impl->m_ipcServer.Start()) {
        LOG_ERROR("Service", __func__, "Boot", "IPC pipe server failed readiness handshake.");
        return false;
    }
    LOG_INFO("Service", __func__, "Boot", "IPC pipe server started and ready.");
#endif
    return true;
}

bool ServiceHost::StartPenSubsystem() {
    if (m_runtimeMode != ServiceMode::Full) {
        LOG_INFO("Service", __func__, "MCU", "Pen modules skipped (touch_only mode).");
        return true;
    }

    try {
        auto eventBridge = std::make_unique<Himax::Pen::PenEventBridge>();
#if EGOTOUCH_SERVICE_ENABLE_IPC
        if (m_impl->m_penEvent) {
            eventBridge->SetNotifyEvent(m_impl->m_penEvent);
        }
#endif
        eventBridge->SetEventCallback(
            [this](const Himax::Pen::PenEvent& ev) {
                if (m_deviceRuntime) {
                    m_deviceRuntime->IngestPenEvent(ev);
                }
                PenStatus::NotificationKind notification = PenStatus::NotificationKind::None;
                if (ev.code == Himax::Pen::PenUsbEventCode::PenTopBatteryWindow ||
                    ev.code == Himax::Pen::PenUsbEventCode::PenBatteryAfterConn) {
                    notification = PenStatus::NotificationKind::PenConnected;
                } else if (ev.code == Himax::Pen::PenUsbEventCode::PenDeviationReminder) {
                    notification = PenStatus::NotificationKind::PenDeviation;
                }
                if (notification != PenStatus::NotificationKind::None) {
                    m_impl->m_notificationKind.store(notification, std::memory_order_release);
                    m_impl->m_notificationSequence.fetch_add(1, std::memory_order_acq_rel);
                    RepublishPenStatus();
                }
            });
        // 回调跑在 PenEventBridge 的 MCU 读线程上，不能久阻。RepublishPenStatus 只读一份
        // 快照再写共享内存，不碰 m_penSubsystemMutex，所以与本函数末尾发布桥对象的那段
        // 临界区不构成互锁。
        eventBridge->SetKbdStateCallback(
            [this](const Himax::Pen::PenEventBridge::KbdState& kbd) {
                bool notifyConnected = false;
                {
                    std::lock_guard<std::mutex> lk(m_impl->m_kbdStateMutex);
                    notifyConnected = Himax::Pen::PenEventBridge::IsKbdConnectionEdge(
                        m_impl->m_kbdState, kbd);
                    if (notifyConnected) {
                        // 0x12（连接）与 0x31（吸附）通常紧挨着到达，都是同一次物理动作。
                        // 在 Producer 侧合并，所有 Consumer 看到的 notificationSequence 都一致。
                        constexpr auto kDuplicateWindow = std::chrono::milliseconds(1500);
                        const auto now = std::chrono::steady_clock::now();
                        if (m_impl->m_lastKbdConnectionNotification !=
                                std::chrono::steady_clock::time_point{} &&
                            now - m_impl->m_lastKbdConnectionNotification < kDuplicateWindow) {
                            notifyConnected = false;
                        } else {
                            m_impl->m_lastKbdConnectionNotification = now;
                        }
                    }
                    m_impl->m_kbdState = kbd;
                }
                if (notifyConnected) {
                    m_impl->m_notificationKind.store(
                        PenStatus::NotificationKind::KeyboardConnected,
                        std::memory_order_release);
                    m_impl->m_notificationSequence.fetch_add(1, std::memory_order_acq_rel);
                }
                RepublishPenStatus();
            });
        eventBridge->SetKbdDetachSupportCallback(
            [this](bool enabled) {
                m_impl->m_kbdDetachSupport.store(enabled, std::memory_order_release);
                m_impl->m_kbdDetachSupportKnown.store(true, std::memory_order_release);
                RepublishPenStatus();
            });
        if (!eventBridge->Start()) {
            LOG_ERROR("Service", __func__, "MCU", "PenEventBridge failed to start.");
            return false;
        }

        auto pressureReader = std::make_unique<Himax::Pen::PenPressureReader>();
#if EGOTOUCH_SERVICE_ENABLE_IPC
        if (m_impl->m_penEvent) {
            pressureReader->SetNotifyEvent(m_impl->m_penEvent);
        }
#endif
        pressureReader->SetPressureCallback(
            [this](const Himax::Pen::PenPressureStats& stats) {
                if (m_deviceRuntime) {
                    m_deviceRuntime->IngestBtMcuPressurePacket(
                        std::array<uint16_t, 4>{stats.press[0], stats.press[1], stats.press[2], stats.press[3]},
                        std::array<uint16_t, 4>{stats.rawPress[0], stats.rawPress[1], stats.rawPress[2], stats.rawPress[3]},
                        stats.freq1,
                        stats.freq2);
                }
            });
        if (!pressureReader->Start()) {
            LOG_ERROR("Service", __func__, "MCU", "PenPressureReader failed to start.");
            return false;
        }

        {
            std::lock_guard<std::mutex> penLock(m_impl->m_penSubsystemMutex);
            m_impl->m_penEventBridge = std::move(eventBridge);
            m_impl->m_penPressureReader = std::move(pressureReader);
        }
        LOG_INFO("Service", __func__, "MCU", "PenEventBridge and PenPressureReader started.");
        return true;
    } catch (...) {
        LOG_ERROR("Service", __func__, "MCU", "Pen subsystem startup threw an exception.");
        return false;
    }
}

bool ServiceHost::Start() {
    return m_impl->m_lifecycle.RunStart([this] {
        try {
            if (!InitializeConfigStores()) {
                LOG_ERROR("Service", "Start", "Boot", "Startup config load/validation failed; service start blocked.");
                return false;
            }

            m_runtimeMode = m_configState.mode;
            LOG_INFO("Service", "Start", "Boot", "Service mode: {}, AutoMode: {}",
                     ServiceModeToConfig(m_configState.mode), m_configState.autoMode);

            if (!ServiceLifecycleCoordinator::Start(*this)) {
                return false;
            }

            LOG_INFO("Service", "Start", "Boot", "All modules started.");
            return true;
        } catch (const std::exception& error) {
            ServiceLifecycleCoordinator::Stop(*this);
            LOG_ERROR("Service", "Start", "Boot",
                      "Unhandled startup exception; service lifecycle rolled back: {}",
                      error.what());
            return false;
        } catch (...) {
            ServiceLifecycleCoordinator::Stop(*this);
            LOG_ERROR("Service", "Start", "Boot",
                      "Unhandled non-standard startup exception; service lifecycle rolled back.");
            return false;
        }
    });
}

void ServiceHost::StopIpcServer() {
#if EGOTOUCH_SERVICE_ENABLE_IPC
    // IpcPipeServer::Stop() closes the listener and joins all handler activity.
    // Pen objects remain alive until this gate has completed.
    m_impl->m_ipcServer.Stop();
#endif
}

void ServiceHost::CloseIpcResources() {
#if EGOTOUCH_SERVICE_ENABLE_IPC
#ifdef _DEBUG
    if (m_deviceRuntime) {
        m_deviceRuntime->SetFramePushCallback(nullptr);
    }
#endif
    {
        std::lock_guard<std::mutex> lk(m_impl->m_debugFrameMutex);
        m_impl->m_hasLatestDebugFrame = false;
        m_impl->m_hasLatestMasterTouchFrame = false;
        m_impl->m_hasLatestMasterSharedFrame = false;
        m_impl->m_latestDebugFrame = Solvers::HeatmapFrame{};
        m_impl->m_latestMasterTouchFrame = Solvers::HeatmapFrame{};
        m_impl->m_latestMasterSharedFrame = Ipc::SharedFrameData{};
    }
    m_impl->m_debugMode = false;

    m_impl->m_frameWriter.Close();
    m_impl->m_configDirty.Close();

    if (m_impl->m_logEvent) {
        Common::GuiLogSink::Instance()->SetNotifyEvent(nullptr);
        CloseHandle(m_impl->m_logEvent);
        m_impl->m_logEvent = nullptr;
    }

    if (m_impl->m_penEvent) {
        // The IPC thread is joined above, so no handler can race pen subsystem
        // access. Detach both producers before closing their shared event handle.
        if (m_impl->m_penEventBridge) {
            m_impl->m_penEventBridge->SetNotifyEvent(nullptr);
        }
        if (m_impl->m_penPressureReader) {
            m_impl->m_penPressureReader->SetNotifyEvent(nullptr);
        }
        // Keep the handle valid until StopPenSubsystem() joins both producer
        // threads; a producer may already have loaded the previous atomic value.
    }
#endif
}

void ServiceHost::StopPenSubsystem() {
    std::lock_guard<std::mutex> penLock(m_impl->m_penSubsystemMutex);
    if (m_impl->m_penPressureReader) {
        m_impl->m_penPressureReader->SetNotifyEvent(nullptr);
        m_impl->m_penPressureReader->Stop();
        m_impl->m_penPressureReader.reset();
        LOG_INFO("Service", __func__, "MCU", "PenPressureReader stopped.");
    }

    if (m_impl->m_penEventBridge) {
        m_impl->m_penEventBridge->SetNotifyEvent(nullptr);
        m_impl->m_penEventBridge->Stop();
        m_impl->m_penEventBridge.reset();
        LOG_INFO("Service", __func__, "MCU", "PenEventBridge stopped.");
    }

#if EGOTOUCH_SERVICE_ENABLE_IPC
    if (m_impl->m_penEvent) {
        CloseHandle(m_impl->m_penEvent);
        m_impl->m_penEvent = nullptr;
    }
#endif
}

void ServiceHost::StopSystemStateMonitor() {
    if (!m_impl->m_sysMonitor) {
        return;
    }

    m_impl->m_sysMonitor->Stop();
    m_impl->m_sysMonitor.reset();
    LOG_INFO("Service", __func__, "Monitor", "SystemStateMonitor stopped.");
}

void ServiceHost::PublishPenStatus(const RuntimePenState& state) {
    PenStatus::State out{};
    out.hasBatteryLevel = state.hasBatteryLevel;
    out.batteryLevel = state.batteryLevel;
    out.hasChargingState = state.hasChargingState;
    out.charging = state.charging;
    out.hasDeviceAttached = state.hasDeviceConnected;
    out.deviceAttached = state.deviceConnected;
    out.hasStylusLink = state.hasConnection;
    out.stylusLinked = state.connected;
    out.notificationSequence =
        m_impl->m_notificationSequence.load(std::memory_order_acquire);
    out.notificationKind = m_impl->m_notificationKind.load(std::memory_order_acquire);

    // 通道里放产品名而不是内部代号：读它的是面板。CopyCString 会截断，而截断一个 UTF-8
    // 多字节序列会得到无效字节、在渲染侧解码失败，所以最长的名字必须编译期就装得下。
    static_assert(sizeof("HUAWEI M-Pencil（第一代）") <= PenStatus::kModelNameCapacity,
                  "product display name must fit the channel's modelName field");
    out.modelId = state.penModuleModelId;
    const char* name = Himax::Pen::ToDisplayName(state.penModuleModel);
    CopyCString(out.modelName, sizeof(out.modelName), name ? name : "");

    // 笔身份三项：MCU 在一次连接内只答复一次，DeviceRuntime 已经缓存好，这里只是转发。
    out.hasPenFirmware = state.hasFirmwareVersion;
    CopyCString(out.penFirmware, sizeof(out.penFirmware), state.firmwareVersion);
    out.hasPenHardware = state.hasHardwareVersion;
    CopyCString(out.penHardware, sizeof(out.penHardware), state.hardwareVersion);
    out.hasPenSerial = state.hasSerialNumber;
    CopyCString(out.penSerial, sizeof(out.penSerial), state.serialNumber);

    {
        std::lock_guard<std::mutex> lk(m_impl->m_kbdStateMutex);
        const auto& kbd = m_impl->m_kbdState;
        out.hasKbdPresent = kbd.hasPresent;
        out.kbdPresent = kbd.present;
        out.hasKbdDetached = kbd.hasDetached;
        out.kbdDetached = kbd.detached;
        out.hasKbdBattery = kbd.hasBattery;
        out.kbdBatteryLevel = kbd.battery;
        out.hasKbdCharging = kbd.hasCharging;
        out.kbdCharging = kbd.charging;
        CopyCString(out.kbdModelName, sizeof(out.kbdModelName), kbd.modelName);
        CopyCString(out.kbdFirmware, sizeof(out.kbdFirmware), kbd.firmware);
    }

    // 托盘菜单靠这个值决定哪一项打勾，而不是靠它自己提交过什么——提交可能被服务按枚举
    // 校验拒绝，也可能被 IPC 配置改写。
    out.hasPenButtonMode = true;
    out.penButtonMode = static_cast<uint8_t>(
        m_impl->m_effectivePenButtonMode.load(std::memory_order_acquire));
    out.hasEraserActive = state.hasEraserToggle;
    out.eraserActive = state.eraserToggle != 0;
    out.hasTouchProvider = true;
    out.touchProvider = m_impl->m_touchProvider.load(std::memory_order_acquire);
    const auto providerError =
        m_impl->m_touchProviderError.load(std::memory_order_acquire);
    out.hasProviderError = providerError != TouchProviderError::None;
    out.providerError = static_cast<uint8_t>(providerError);
    out.hasInputSuppressed = true;
    out.inputSuppressed =
        m_impl->m_inputSuppressed.load(std::memory_order_acquire);
    out.hasKbdDetachSupport =
        m_impl->m_kbdDetachSupportKnown.load(std::memory_order_acquire);
    out.kbdDetachSupport =
        m_impl->m_kbdDetachSupport.load(std::memory_order_acquire);

    m_impl->m_penStatusWriter.Publish(out);
}

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

bool ServiceHost::StartEGoTouchProvider() {
    if (!m_deviceRuntime) return false;
    SetInputSuppressed(false);
    switch (m_deviceRuntime->RequestStart()) {
    case DeviceRuntime::StartRequestResult::Started:
    case DeviceRuntime::StartRequestResult::AlreadyRunning:
        ReplayLastWakeEvent();
        return true;
    default:
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
    if (!m_deviceRuntime) return true;
    SetInputSuppressed(false);
    (void)m_deviceRuntime->RequestStop();
    return !m_deviceRuntime->IsRunning();
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
    if (!m_deviceRuntime) {
        return;
    }
    PublishPenStatus(m_deviceRuntime->GetPenStateSnapshot());
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
            // 持锁调用：锁挡住的是 StopPenSubsystem 销毁桥对象，不能只取裸指针就放手。
            std::lock_guard<std::mutex> penLock(m_impl->m_penSubsystemMutex);
            auto* bridge = m_impl->m_penEventBridge.get();
            if (!bridge) {
                // touch_only 模式或笔子系统尚未起来：没有 MCU 可写，丢弃而不是崩。
                LOG_WARN("Service", __func__, "PenControl",
                         "Dropping keyboard detach support command: pen event bridge is not available.");
            } else if (!bridge->SendKbdDetachSupportSet(enable)) {
                LOG_WARN("Service", __func__, "PenControl",
                         "Keyboard detach support set to {} failed on the MCU channel.",
                         enable ? "enabled" : "disabled");
            }
            // 这里不补发 Get：SendKbdDetachSupportSet 内部已经重发一次，实际值经 detach
            // support 回调回流到状态通道。本次调用返回时状态通道还是旧值，这不是漏更新。
        }
    }
}

namespace {
// 等待切片。有提交事件时它只决定 Stop 的响应上限（事件一置位就返回）；事件缺失时它同时
// 是轮询周期——读一次 32 字节的 seqlock 比这次等待本身还便宜，没必要为省这点开销把停机
// 延迟拉长到秒级。
constexpr DWORD kPenControlWaitSliceMs = 250;
} // namespace

void ServiceHost::PenControlThreadMain() {
    while (!m_impl->m_penControlStop.load(std::memory_order_acquire)) {
        (void)m_impl->m_penControlHost.WaitForSubmit(kPenControlWaitSliceMs);
        if (m_impl->m_penControlStop.load(std::memory_order_acquire)) {
            break;
        }

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

// 与 IPC 无关的字符串助手，因此不在下面那个守卫里：PublishPenStatus 在所有配置里都要编，
// 而笔状态通道在 Release 中同样要工作。
void ServiceHost::CopyCString(char* dst, size_t dstSize, std::string_view src) {
    if (!dst || dstSize == 0) return;
    std::memset(dst, 0, dstSize);
    if (src.empty()) return;
    const size_t n = std::min(dstSize - 1, src.size());
    std::memcpy(dst, src.data(), n);
}

#if EGOTOUCH_SERVICE_ENABLE_IPC
// ── Pipeline 构建 ──────────────────────────────
uint32_t ServiceHost::HashDebugSchema(const std::vector<Ipc::DebugFieldSchemaWire>& defs) {
    uint32_t h = 2166136261u;
    auto hashBytes = [&h](const void* p, size_t n) {
        const auto* b = reinterpret_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 16777619u;
        }
    };

    for (const auto& d : defs) {
        hashBytes(&d.fieldId, sizeof(d.fieldId));
        hashBytes(&d.valueType, sizeof(d.valueType));
        hashBytes(&d.sourceKind, sizeof(d.sourceKind));
        hashBytes(&d.sourceIndex, sizeof(d.sourceIndex));
        hashBytes(&d.uiOrder, sizeof(d.uiOrder));
        hashBytes(&d.dvrTarget, sizeof(d.dvrTarget));
        hashBytes(&d.dvrPositionMode, sizeof(d.dvrPositionMode));
        hashBytes(&d.dvrIndex, sizeof(d.dvrIndex));

        const std::string_view key = CStrArrayView(d.key);
        const std::string_view displayName = CStrArrayView(d.displayName);
        const std::string_view unit = CStrArrayView(d.unit);
        const std::string_view uiGroup = CStrArrayView(d.uiGroup);
        const std::string_view dvrColumnName = CStrArrayView(d.dvrColumnName);
        const std::string_view dvrAnchor = CStrArrayView(d.dvrAnchor);

        hashBytes(key.data(), key.size());
        hashBytes(displayName.data(), displayName.size());
        hashBytes(unit.data(), unit.size());
        hashBytes(uiGroup.data(), uiGroup.size());
        hashBytes(dvrColumnName.data(), dvrColumnName.size());
        hashBytes(dvrAnchor.data(), dvrAnchor.size());
    }

    return h;
}

uint16_t ServiceHost::DeriveDebugSchemaVersion(uint32_t schemaHash) {
    // Update policy: schemaVersion is deterministically derived from descriptor content.
    // Any descriptor change that affects schemaHash automatically changes schemaVersion.
    constexpr uint16_t kVersionSalt = 0xA5C3u;
    const uint16_t folded = static_cast<uint16_t>((schemaHash & 0xFFFFu) ^ (schemaHash >> 16));
    const uint16_t version = static_cast<uint16_t>(folded ^ kVersionSalt);
    return version == 0 ? 1 : version;
}

uint64_t ServiceHost::EncodePenValue(const Himax::Pen::PenPressureStats& s,
                                     bool evtRunning,
                                     bool pressRunning,
                                     int16_t sourceIndex,
                                     bool& valid) {
    valid = true;
    switch (static_cast<Ipc::DebugPenSourceIndex>(sourceIndex)) {
    case Ipc::DebugPenSourceIndex::EvtRunning: return EncodeBool(evtRunning);
    case Ipc::DebugPenSourceIndex::PressRunning: return EncodeBool(pressRunning);
    case Ipc::DebugPenSourceIndex::ReportType: return EncodeU32(s.reportType);
    case Ipc::DebugPenSourceIndex::Freq1: return EncodeU32(s.freq1);
    case Ipc::DebugPenSourceIndex::Freq2: return EncodeU32(s.freq2);
    case Ipc::DebugPenSourceIndex::Press0: return EncodeU32(s.press[0]);
    case Ipc::DebugPenSourceIndex::Press1: return EncodeU32(s.press[1]);
    case Ipc::DebugPenSourceIndex::Press2: return EncodeU32(s.press[2]);
    case Ipc::DebugPenSourceIndex::Press3: return EncodeU32(s.press[3]);
    default:
        valid = false;
        return 0;
    }
}

uint64_t ServiceHost::EncodeDebugValue(const Solvers::HeatmapFrame& frame,
                                       const Ipc::DebugFieldSchemaWire& def,
                                       bool& valid) {
    valid = true;
    const auto sourceKind = static_cast<Ipc::DebugSourceKind>(def.sourceKind);

    switch (sourceKind) {
    case Ipc::DebugSourceKind::MasterSuffixWord:
        if (!frame.masterSuffixValid || def.sourceIndex < 0 || def.sourceIndex >= Frame::kMasterSuffixWords) {
            valid = false;
            return 0;
        }
        return EncodeU32(frame.masterSuffix.words[def.sourceIndex]);
    case Ipc::DebugSourceKind::SlaveSuffixWord:
        if (!frame.slaveSuffixValid || def.sourceIndex < 0 || def.sourceIndex >= Frame::kSlaveSuffixWords) {
            valid = false;
            return 0;
        }
        return EncodeU32(frame.slaveSuffix.words[def.sourceIndex]);
    case Ipc::DebugSourceKind::StylusField: {
        const auto& s = frame.stylus;
        const auto& point = s.output.point;
        const auto& press = s.runtime.Active().pressure;
        switch (static_cast<Ipc::DebugStylusSourceIndex>(def.sourceIndex)) {
        case Ipc::DebugStylusSourceIndex::Pressure: return EncodeU32(s.output.pressure);
        case Ipc::DebugStylusSourceIndex::SignalX: return EncodeU32(s.interop.signalX);
        case Ipc::DebugStylusSourceIndex::SignalY: return EncodeU32(s.interop.signalY);
        case Ipc::DebugStylusSourceIndex::MaxRawPeak: return EncodeU32(s.interop.maxRawPeak);
        case Ipc::DebugStylusSourceIndex::Status: return EncodeU32(s.input.status);
        case Ipc::DebugStylusSourceIndex::PipelineStage: return EncodeU32(s.output.pipelineStage);
        case Ipc::DebugStylusSourceIndex::PointX: return EncodeF32(point.x);
        case Ipc::DebugStylusSourceIndex::PointY: return EncodeF32(point.y);
        case Ipc::DebugStylusSourceIndex::RawPressure: return EncodeU32(point.rawPressure);
        case Ipc::DebugStylusSourceIndex::MappedPressure: return EncodeU32(point.mappedPressure);
        case Ipc::DebugStylusSourceIndex::NoPressInkActive:
            valid = false;
            return 0;
        case Ipc::DebugStylusSourceIndex::TouchSuppressActive: return EncodeBool(s.interop.touchSuppressActive);
        case Ipc::DebugStylusSourceIndex::BtSeq: return EncodeU32(press.btSeq);
        case Ipc::DebugStylusSourceIndex::PressureIsReal: return EncodeBool(press.pressureIsReal);
        default:
            valid = false;
            return 0;
        }
    }
    case Ipc::DebugSourceKind::PenBridgeField:
        valid = false;
        return 0;
    case Ipc::DebugSourceKind::DerivedField:
        switch (static_cast<DebugDerivedSourceIndex>(def.sourceIndex)) {
        case DebugDerivedSourceIndex::MasterWasRead:
            return EncodeBool(frame.masterWasRead);
        case DebugDerivedSourceIndex::ContactCount:
            return EncodeU32(static_cast<uint32_t>(frame.touch.output.contacts.size()));
        case DebugDerivedSourceIndex::PeakCount:
#if EGOTOUCH_DIAG
            return EncodeU32(static_cast<uint32_t>(frame.touch.debug.peaks.size()));
#else
            return EncodeU32(0);
#endif
        case DebugDerivedSourceIndex::FrameTimestamp:
            return frame.timestamp;
        default:
            valid = false;
            return 0;
        }
    default:
        valid = false;
        return 0;
    }
}

void ServiceHost::BuildDebugSchema() {
    m_impl->m_debugSchema.clear();

    auto add = [this](uint16_t fieldId,
                      Ipc::DebugValueType valueType,
                      Ipc::DebugSourceKind sourceKind,
                      int16_t sourceIndex,
                      uint8_t uiOrder,
                      Ipc::DebugDvrTarget dvrTarget,
                      Ipc::DebugDvrPositionMode dvrPositionMode,
                      int16_t dvrIndex,
                      std::string_view key,
                      std::string_view displayName,
                      std::string_view unit,
                      std::string_view uiGroup,
                      std::string_view dvrColumnName,
                      std::string_view dvrAnchor) {
        Ipc::DebugFieldSchemaWire w{};
        w.fieldId = fieldId;
        w.valueType = static_cast<uint8_t>(valueType);
        w.sourceKind = static_cast<uint8_t>(sourceKind);
        w.sourceIndex = sourceIndex;
        w.uiOrder = uiOrder;
        w.dvrTarget = static_cast<uint8_t>(dvrTarget);
        w.dvrPositionMode = static_cast<uint8_t>(dvrPositionMode);
        w.dvrIndex = dvrIndex;
        CopyCString(w.key, sizeof(w.key), key);
        CopyCString(w.displayName, sizeof(w.displayName), displayName);
        CopyCString(w.unit, sizeof(w.unit), unit);
        CopyCString(w.uiGroup, sizeof(w.uiGroup), uiGroup);
        CopyCString(w.dvrColumnName, sizeof(w.dvrColumnName), dvrColumnName);
        CopyCString(w.dvrAnchor, sizeof(w.dvrAnchor), dvrAnchor);
        m_impl->m_debugSchema.push_back(w);
    };

    add(1, Ipc::DebugValueType::UInt32, Ipc::DebugSourceKind::MasterSuffixWord,
        static_cast<int16_t>(Frame::MasterWord::kTpFreq1), 1,
        Ipc::DebugDvrTarget::MasterStatus, Ipc::DebugDvrPositionMode::AfterAnchor, -1,
        "master_tp_freq1", "Master TpFreq1", "", "MasterSuffix",
        "DBG_MasterTpFreq1", "MasterSuffixValid");

    add(2, Ipc::DebugValueType::UInt32, Ipc::DebugSourceKind::MasterSuffixWord,
        static_cast<int16_t>(Frame::MasterWord::kTpFreq2), 2,
        Ipc::DebugDvrTarget::MasterStatus, Ipc::DebugDvrPositionMode::AfterAnchor, -1,
        "master_tp_freq2", "Master TpFreq2", "", "MasterSuffix",
        "DBG_MasterTpFreq2", "DBG_MasterTpFreq1");

    add(3, Ipc::DebugValueType::UInt32, Ipc::DebugSourceKind::StylusField,
        static_cast<int16_t>(Ipc::DebugStylusSourceIndex::Pressure), 1,
        Ipc::DebugDvrTarget::SlaveSuffix, Ipc::DebugDvrPositionMode::AfterAnchor, -1,
        "stylus_pressure", "Stylus Pressure", "", "Stylus",
        "DBG_StylusPressure", "Pressure");

    add(4, Ipc::DebugValueType::Float32, Ipc::DebugSourceKind::StylusField,
        static_cast<int16_t>(Ipc::DebugStylusSourceIndex::PointX), 2,
        Ipc::DebugDvrTarget::SlaveSuffix, Ipc::DebugDvrPositionMode::AfterAnchor, -1,
        "stylus_point_x", "Stylus Point X", "grid", "Stylus",
        "DBG_StylusPointX", "PointX");

    add(5, Ipc::DebugValueType::Float32, Ipc::DebugSourceKind::StylusField,
        static_cast<int16_t>(Ipc::DebugStylusSourceIndex::PointY), 3,
        Ipc::DebugDvrTarget::SlaveSuffix, Ipc::DebugDvrPositionMode::AfterAnchor, -1,
        "stylus_point_y", "Stylus Point Y", "grid", "Stylus",
        "DBG_StylusPointY", "DBG_StylusPointX");

    add(13, Ipc::DebugValueType::UInt32, Ipc::DebugSourceKind::StylusField,
        static_cast<int16_t>(Ipc::DebugStylusSourceIndex::BtSeq), 4,
        Ipc::DebugDvrTarget::SlaveSuffix, Ipc::DebugDvrPositionMode::AfterAnchor, -1,
        "stylus_bt_seq", "Stylus BT Seq", "", "Stylus",
        "DBG_StylusBtSeq", "DBG_StylusPointY");

    add(15, Ipc::DebugValueType::Bool, Ipc::DebugSourceKind::StylusField,
        static_cast<int16_t>(Ipc::DebugStylusSourceIndex::PressureIsReal), 5,
        Ipc::DebugDvrTarget::SlaveSuffix, Ipc::DebugDvrPositionMode::AfterAnchor, -1,
        "stylus_pressure_is_real", "Pressure Is Real", "", "Stylus",
        "DBG_StylusPressureIsReal", "DBG_StylusBtSeq");

    add(6, Ipc::DebugValueType::UInt32, Ipc::DebugSourceKind::PenBridgeField,
        static_cast<int16_t>(Ipc::DebugPenSourceIndex::Freq1), 1,
        Ipc::DebugDvrTarget::DynamicDebug, Ipc::DebugDvrPositionMode::Append, -1,
        "pen_freq1", "Pen Freq1", "", "PenBridge",
        "DBG_PenFreq1", "");

    add(7, Ipc::DebugValueType::UInt32, Ipc::DebugSourceKind::PenBridgeField,
        static_cast<int16_t>(Ipc::DebugPenSourceIndex::Freq2), 2,
        Ipc::DebugDvrTarget::DynamicDebug, Ipc::DebugDvrPositionMode::Append, -1,
        "pen_freq2", "Pen Freq2", "", "PenBridge",
        "DBG_PenFreq2", "");

    add(8, Ipc::DebugValueType::UInt32, Ipc::DebugSourceKind::PenBridgeField,
        static_cast<int16_t>(Ipc::DebugPenSourceIndex::Press0), 3,
        Ipc::DebugDvrTarget::DynamicDebug, Ipc::DebugDvrPositionMode::Append, -1,
        "pen_press0", "Pen Press0", "", "PenBridge",
        "DBG_PenPress0", "");

    add(9, Ipc::DebugValueType::UInt32, Ipc::DebugSourceKind::PenBridgeField,
        static_cast<int16_t>(Ipc::DebugPenSourceIndex::Press1), 4,
        Ipc::DebugDvrTarget::DynamicDebug, Ipc::DebugDvrPositionMode::Append, -1,
        "pen_press1", "Pen Press1", "", "PenBridge",
        "DBG_PenPress1", "");

    add(16, Ipc::DebugValueType::UInt32, Ipc::DebugSourceKind::PenBridgeField,
        static_cast<int16_t>(Ipc::DebugPenSourceIndex::Press2), 5,
        Ipc::DebugDvrTarget::DynamicDebug, Ipc::DebugDvrPositionMode::Append, -1,
        "pen_press2", "Pen Press2", "", "PenBridge",
        "DBG_PenPress2", "");

    add(17, Ipc::DebugValueType::UInt32, Ipc::DebugSourceKind::PenBridgeField,
        static_cast<int16_t>(Ipc::DebugPenSourceIndex::Press3), 6,
        Ipc::DebugDvrTarget::DynamicDebug, Ipc::DebugDvrPositionMode::Append, -1,
        "pen_press3", "Pen Press3", "", "PenBridge",
        "DBG_PenPress3", "");

    add(10, Ipc::DebugValueType::UInt32, Ipc::DebugSourceKind::DerivedField,
        static_cast<int16_t>(DebugDerivedSourceIndex::ContactCount), 1,
        Ipc::DebugDvrTarget::MasterStatus, Ipc::DebugDvrPositionMode::AfterAnchor, -1,
        "contact_count", "Contact Count", "", "Frame",
        "DBG_ContactCount", "ContactCount");

    add(11, Ipc::DebugValueType::UInt32, Ipc::DebugSourceKind::DerivedField,
        static_cast<int16_t>(DebugDerivedSourceIndex::PeakCount), 2,
        Ipc::DebugDvrTarget::MasterStatus, Ipc::DebugDvrPositionMode::AfterAnchor, -1,
        "peak_count", "Peak Count", "", "Frame",
        "DBG_PeakCount", "DBG_ContactCount");

    add(12, Ipc::DebugValueType::Bool, Ipc::DebugSourceKind::DerivedField,
        static_cast<int16_t>(DebugDerivedSourceIndex::MasterWasRead), 3,
        Ipc::DebugDvrTarget::MasterStatus, Ipc::DebugDvrPositionMode::AfterAnchor, -1,
        "master_was_read", "Master Was Read", "", "Frame",
        "DBG_MasterWasRead", "DBG_PeakCount");

    m_impl->m_debugSchemaHash = HashDebugSchema(m_impl->m_debugSchema);
    m_impl->m_debugSchemaVersion = DeriveDebugSchemaVersion(m_impl->m_debugSchemaHash);
}

// ── IPC helpers ──────────────────────────────
void ServiceHost::HandleIpcEnterDebugMode(Ipc::IpcResponse& resp) {
#ifdef _DEBUG
    // Shared memory is already created at startup.
    // Just activate the frame push callback.
    if (m_impl->m_frameWriter.IsOpen()) {
        m_deviceRuntime->SetFramePushCallback(
            [this](const Solvers::HeatmapFrame& f) {
                Ipc::SharedFrameData sharedFrame{};
                Ipc::PopulateSharedFrameDataFromSolverFrame(sharedFrame, f);

                Solvers::HeatmapFrame debugFrame = f;
                {
                    std::lock_guard<std::mutex> lk(m_impl->m_debugFrameMutex);
                    if (f.masterWasRead) {
                        m_impl->m_latestMasterTouchFrame = f;
                        m_impl->m_latestMasterSharedFrame = sharedFrame;
                        m_impl->m_hasLatestMasterTouchFrame = true;
                        m_impl->m_hasLatestMasterSharedFrame = true;
                    } else {
                        if (m_impl->m_hasLatestMasterTouchFrame) {
                            PreserveMasterTouchDebugState(debugFrame, m_impl->m_latestMasterTouchFrame);
                        }
                        if (m_impl->m_hasLatestMasterSharedFrame) {
                            Ipc::PreserveMasterTouchVisualizationFromCachedFrame(
                                sharedFrame,
                                m_impl->m_latestMasterSharedFrame);
                        }
                    }
                    m_impl->m_latestDebugFrame = debugFrame;
                    m_impl->m_hasLatestDebugFrame = true;
                }

                const RuntimeSnapshot runtime = m_deviceRuntime->GetSnapshot();
                sharedFrame.workerState = static_cast<int8_t>(runtime.state);
                sharedFrame.streaming = runtime.state == workerState::streaming;
                sharedFrame.lastFrameProcessUs = -1;
                sharedFrame.avgFrameProcessUs = -1;
                sharedFrame.acquisitionFps = -1;
                sharedFrame.slaveAcquisitionFps = -1;
                sharedFrame.vhfEnabled = m_deviceRuntime->IsVhfEnabled();
                sharedFrame.vhfDeviceOpen = m_deviceRuntime->IsVhfDeviceOpen();
                sharedFrame.vhfTranspose = m_deviceRuntime->IsVhfTransposeEnabled();
                m_impl->m_frameWriter.Write(sharedFrame);
            });
        m_impl->m_debugMode = true;
        Ipc::MarkSuccess(resp);
        LOG_INFO("Service", __func__, "IPC", "Entered debug mode.");
    } else {
        Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidState);
        LOG_ERROR("Service", __func__, "IPC", "EnterDebugMode rejected: shared memory not available.");
    }
#else
    Ipc::MarkFailure(resp, Ipc::IpcStatusCode::UnsupportedCommand);
    LOG_WARN("Service", __func__, "IPC", "EnterDebugMode not available in release build.");
#endif
}

void ServiceHost::HandleIpcExitDebugMode(Ipc::IpcResponse& resp) {
#ifdef _DEBUG
    m_deviceRuntime->SetFramePushCallback(nullptr);
    {
        std::lock_guard<std::mutex> lk(m_impl->m_debugFrameMutex);
        m_impl->m_hasLatestDebugFrame = false;
        m_impl->m_hasLatestMasterTouchFrame = false;
        m_impl->m_hasLatestMasterSharedFrame = false;
        m_impl->m_latestDebugFrame = Solvers::HeatmapFrame{};
        m_impl->m_latestMasterTouchFrame = Solvers::HeatmapFrame{};
        m_impl->m_latestMasterSharedFrame = Ipc::SharedFrameData{};
    }
    m_impl->m_debugMode = false;
#endif

    Ipc::MarkSuccess(resp);
    LOG_INFO("Service", __func__, "IPC", "Exited debug mode.");
}

void ServiceHost::HandleIpcGetConfigCatalogV3(const Ipc::IpcRequest& req, Ipc::IpcResponse& resp) {
    const auto blob = m_configRuntime.BuildCatalogV3Blob();
    BuildConfigV3PageResponse(Ipc::IpcCommand::GetConfigCatalogV3, req, blob, resp);
}

void ServiceHost::HandleIpcGetConfigV3Snapshot(const Ipc::IpcRequest& req, Ipc::IpcResponse& resp) {
    const auto blob = m_configRuntime.BuildSnapshotV3Blob();
    BuildConfigV3PageResponse(Ipc::IpcCommand::GetConfigSnapshotV3, req, blob, resp);
}

void ServiceHost::HandleIpcConfigV3ApplyPatch(const Ipc::IpcRequest& req, Ipc::IpcResponse& resp) {
    if (!m_deviceRuntime) {
        Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidState);
        return;
    }
    if (req.paramLen < sizeof(Ipc::ApplyConfigPatchV3RequestWire)) {
        Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidRequest);
        return;
    }

    Ipc::ApplyConfigPatchV3RequestWire request{};
    std::memcpy(&request, req.param, sizeof(request));
    if (!Ipc::IsValidApplyConfigPatchV3Request(request)) {
        Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidRequest);
        return;
    }

    const auto apply = m_configRuntime.ApplyConfigPatchV3(
        request.baseSchemaVersion,
        request.baseSnapshotVersion,
        request.bytes,
        request.payloadBytes);

    if (apply.runtimeStatus != ServiceRuntimeStatusCode::Ok) {
        Ipc::MarkFailure(resp, ToIpcStatus(apply.runtimeStatus));
        return;
    }

    for (const auto& action : apply.applyActions) {
        switch (action.kind) {
        case ConfigApplyActionKind::ServicePolicy:
            (void)HandleReloadServiceConfig(action.serviceConfig);
            break;
        case ConfigApplyActionKind::PipelineRuntime:
            m_deviceRuntime->ApplyPipelineConfig(action.configStore);
            break;
        }
    }

    Ipc::ConfigV3ApplyResultWire result{};
    result.status = static_cast<uint8_t>(ToIpcMutationStatus(apply.status));
    result.changedCount = static_cast<uint16_t>(std::min<size_t>(apply.changedCount, UINT16_MAX));
    result.appliedCount = static_cast<uint16_t>(std::min<size_t>(apply.appliedCount, UINT16_MAX));
    result.restartRequiredCount = static_cast<uint16_t>(std::min<size_t>(apply.restartRequiredCount, UINT16_MAX));
    result.rejectedCount = static_cast<uint16_t>(std::min<size_t>(apply.rejectedCount, UINT16_MAX));
    result.failedKeyId = static_cast<uint16_t>(apply.failedKeyId);
    result.failedValueType = static_cast<uint8_t>(apply.failedValueType);
    std::memcpy(resp.data, &result, sizeof(result));
    resp.dataLen = static_cast<uint16_t>(sizeof(result));
    Ipc::MarkSuccess(resp);
    LOG_INFO("Service", __func__, "Config", "Applied config v3 patch entries={} changed={} applied={} restartRequired={} status={}",
             apply.entryCount, apply.changedCount, apply.appliedCount, apply.restartRequiredCount,
             static_cast<unsigned int>(apply.status));
}

void ServiceHost::HandleIpcConfigV3Persist(Ipc::IpcResponse& resp) {
    const auto persist = m_configRuntime.PersistConfigV3();
    if (persist.runtimeStatus != ServiceRuntimeStatusCode::Ok) {
        Ipc::MarkFailure(resp, ToIpcStatus(persist.runtimeStatus));
        return;
    }

    Ipc::PersistConfigV3ResponseWire result{};
    result.status = static_cast<uint8_t>(ToIpcMutationStatus(persist.status));
    result.persistedCount = static_cast<uint16_t>(std::min<size_t>(persist.persistedCount, UINT16_MAX));
    result.skippedCount = static_cast<uint16_t>(std::min<size_t>(persist.skippedCount, UINT16_MAX));
    result.failedCount = static_cast<uint16_t>(std::min<size_t>(persist.failedCount, UINT16_MAX));
    std::memcpy(resp.data, &result, sizeof(result));
    resp.dataLen = static_cast<uint16_t>(sizeof(result));
    Ipc::MarkSuccess(resp);
    LOG_INFO("Service", __func__, "IPC", "PersistConfigV3 completed persisted={} skipped={} failed={}",
             persist.persistedCount, persist.skippedCount, persist.failedCount);
}

void ServiceHost::HandleIpcGetLogs(Ipc::IpcResponse& resp) {
    auto lines = Common::GuiLogSink::Instance()->DrainNewLines();
    std::string packed;
    for (const auto& l : lines) {
        if (packed.size() + l.size() + 1 > sizeof(resp.data)) {
            break;
        }
        packed += l;
        packed += '\n';
    }

    resp.dataLen = static_cast<uint16_t>(std::min(packed.size(), sizeof(resp.data)));
    std::memcpy(resp.data, packed.data(), resp.dataLen);
    Ipc::MarkSuccess(resp);
}

void ServiceHost::HandleIpcGetPenBridgeStatus(Ipc::IpcResponse& resp) {
    // Pack: [evtRunning:1][pressRunning:1][reportType:1][freq1:1][freq2:1]
    //       [p0..p3 u16 scaled][mode:1][max u16][raw0..raw3 u16]
    // Total: 24 bytes
    uint8_t buf[24] = {};
    std::lock_guard<std::mutex> penLock(m_impl->m_penSubsystemMutex);
    buf[0] = (m_impl->m_penEventBridge && m_impl->m_penEventBridge->IsRunning()) ? 1 : 0;
    buf[1] = (m_impl->m_penPressureReader && m_impl->m_penPressureReader->IsRunning()) ? 1 : 0;
    if (m_impl->m_penPressureReader) {
        auto s = m_impl->m_penPressureReader->GetPressureStats();
        buf[2] = s.reportType;
        buf[3] = s.freq1;
        buf[4] = s.freq2;
        for (int k = 0; k < 4; ++k) {
            buf[5 + k * 2] = static_cast<uint8_t>(s.press[k] & 0xFF);
            buf[5 + k * 2 + 1] = static_cast<uint8_t>(s.press[k] >> 8);
        }
        buf[13] = static_cast<uint8_t>(s.pressureMode);
        buf[14] = static_cast<uint8_t>(s.pressureMax & 0xFF);
        buf[15] = static_cast<uint8_t>(s.pressureMax >> 8);
        for (int k = 0; k < 4; ++k) {
            buf[16 + k * 2] = static_cast<uint8_t>(s.rawPress[k] & 0xFF);
            buf[16 + k * 2 + 1] = static_cast<uint8_t>(s.rawPress[k] >> 8);
        }
    }

    std::memcpy(resp.data, buf, sizeof(buf));
    resp.dataLen = sizeof(buf);
    Ipc::MarkSuccess(resp);
}

void ServiceHost::HandleIpcGetPenIdentityStatus(Ipc::IpcResponse& resp) {
    if (!m_deviceRuntime) {
        Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidState);
        return;
    }

    const auto state = m_deviceRuntime->GetPenStateSnapshot();
    Ipc::PenIdentityStatusWire wire{};
    if (state.hasConnection) {
        wire.flags |= Ipc::kPenIdentityHasConnectionState;
        if (state.connected) {
            wire.flags |= Ipc::kPenIdentityConnected;
        }
    }
    wire.flags |= Ipc::kPenIdentityHasProtocolHint;
    wire.protocolHint = static_cast<uint8_t>(ToIpcPenIdentityProtocolHint(state.protocolHint));
    if (state.protocolHintFromPenModule) {
        wire.protocolFlags |= Ipc::kPenIdentityProtocolFromPenModule;
    }
    if (state.hasPairStatus) {
        Ipc::SetPenIdentityPairStatus(wire, state.pairStatus);
    }
    if (state.hasBatteryLevel) {
        Ipc::SetPenIdentityBatteryLevel(wire, state.batteryLevel);
    }
    if (state.hasChargingState) {
        Ipc::SetPenIdentityChargingState(wire, state.charging);
    }
    if (state.hasDeviceConnected) {
        Ipc::SetPenIdentityDeviceConnected(wire, state.deviceConnected);
    }
    wire.factoryStatusFlags = state.factoryStatusFlags;
    if (state.hasStylusId) {
        wire.flags |= Ipc::kPenIdentityHasStylusId;
        wire.stylusId = state.stylusId;
    }
    if (state.hasPenModuleModelId) {
        wire.flags |= Ipc::kPenIdentityHasPenModuleModelId;
        wire.penModuleModelId = state.penModuleModelId;
    }
    auto copyUtf8Field = [&](const std::string& text,
                              uint8_t flag,
                              uint16_t& textLenWire,
                              auto& textBuffer) {
        if (text.empty()) {
            return;
        }

        const std::size_t maxTextBytes = sizeof(textBuffer) - 1;
        const std::size_t textLen = Utf8TruncatedLength(text, maxTextBytes);
        if (textLen > 0) {
            wire.flags |= flag;
            textLenWire = static_cast<uint16_t>(textLen);
            std::memcpy(textBuffer, text.data(), textLen);
        }
    };

    if (state.hasHardwareVersion) {
        copyUtf8Field(state.hardwareVersion,
                      Ipc::kPenIdentityHasHardwareVersion,
                      wire.hardwareVersionUtf8Len,
                      wire.hardwareVersionUtf8);
    }
    if (state.hasSerialNumber) {
        copyUtf8Field(state.serialNumber,
                      Ipc::kPenIdentityHasSerialNumber,
                      wire.serialNumberUtf8Len,
                      wire.serialNumberUtf8);
    }
    if (state.hasFirmwareVersion) {
        copyUtf8Field(state.firmwareVersion,
                      Ipc::kPenIdentityHasFirmwareVersion,
                      wire.firmwareVersionUtf8Len,
                      wire.firmwareVersionUtf8);
    }

    std::memcpy(resp.data, &wire, sizeof(wire));
    resp.dataLen = static_cast<uint16_t>(sizeof(wire));
    Ipc::MarkSuccess(resp);
}

void ServiceHost::HandleIpcGetDebugSchema(const Ipc::IpcRequest& req, Ipc::IpcResponse& resp) {
    Ipc::DebugSchemaRequest reqSchema{};
    if (req.paramLen >= sizeof(Ipc::DebugSchemaRequest)) {
        std::memcpy(&reqSchema, req.param, sizeof(Ipc::DebugSchemaRequest));
    }

    const uint16_t total = static_cast<uint16_t>(m_impl->m_debugSchema.size());
    const uint16_t offset = std::min<uint16_t>(reqSchema.offset, total);
    const uint16_t maxByPayload = static_cast<uint16_t>(
        (sizeof(resp.data) - sizeof(Ipc::DebugSchemaResponseHeader)) / sizeof(Ipc::DebugFieldSchemaWire));
    uint16_t requested = reqSchema.limit == 0 ? maxByPayload : reqSchema.limit;
    requested = std::min<uint16_t>(requested, maxByPayload);
    const uint16_t available = static_cast<uint16_t>(total - offset);
    const uint16_t take = std::min<uint16_t>(requested, available);

    Ipc::DebugSchemaResponseHeader hdr{};
    hdr.schemaVersion = m_impl->m_debugSchemaVersion;
    hdr.totalFields = total;
    hdr.returnedFields = take;
    hdr.recordSize = static_cast<uint16_t>(sizeof(Ipc::DebugFieldSchemaWire));
    hdr.schemaHash = m_impl->m_debugSchemaHash;

    std::memcpy(resp.data, &hdr, sizeof(hdr));
    size_t cursor = sizeof(hdr);
    for (uint16_t i = 0; i < take; ++i) {
        const auto& def = m_impl->m_debugSchema[offset + i];
        std::memcpy(resp.data + cursor, &def, sizeof(def));
        cursor += sizeof(def);
    }

    resp.dataLen = static_cast<uint16_t>(cursor);
    Ipc::MarkSuccess(resp);
}

void ServiceHost::HandleIpcGetDebugSnapshot(Ipc::IpcResponse& resp) {
    Solvers::HeatmapFrame frame{};
    bool hasFrame = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->m_debugFrameMutex);
        if (m_impl->m_hasLatestDebugFrame) {
            frame = m_impl->m_latestDebugFrame;
            hasFrame = true;
        }
    }

    bool evtRunning = false;
    bool pressRunning = false;
    Himax::Pen::PenPressureStats penStats{};
    {
        std::lock_guard<std::mutex> penLock(m_impl->m_penSubsystemMutex);
        evtRunning = m_impl->m_penEventBridge && m_impl->m_penEventBridge->IsRunning();
        pressRunning = m_impl->m_penPressureReader && m_impl->m_penPressureReader->IsRunning();
        if (m_impl->m_penPressureReader) {
            penStats = m_impl->m_penPressureReader->GetPressureStats();
        }
    }

    const uint16_t take = static_cast<uint16_t>(std::min<size_t>(
        m_impl->m_debugSchema.size(),
        Ipc::kDebugSnapshotMaxValues));

    Ipc::DebugSnapshotHeader hdr{};
    hdr.schemaVersion = m_impl->m_debugSchemaVersion;
    hdr.fieldCount = take;
    hdr.recordSize = static_cast<uint16_t>(sizeof(Ipc::DebugSnapshotValueWire));
    std::memcpy(resp.data, &hdr, sizeof(hdr));

    size_t cursor = sizeof(hdr);
    for (uint16_t i = 0; i < take; ++i) {
        const auto& def = m_impl->m_debugSchema[i];
        bool valid = true;
        uint64_t raw = 0;

        const auto sourceKind = static_cast<Ipc::DebugSourceKind>(def.sourceKind);
        if (sourceKind == Ipc::DebugSourceKind::PenBridgeField) {
            raw = EncodePenValue(penStats, evtRunning, pressRunning, def.sourceIndex, valid);
        } else if (hasFrame) {
            raw = EncodeDebugValue(frame, def, valid);
        } else {
            valid = false;
        }

        Ipc::DebugSnapshotValueWire v{};
        v.fieldId = def.fieldId;
        v.valueType = def.valueType;
        v.flags = valid ? 0x1 : 0x0;
        v.rawValue = raw;
        std::memcpy(resp.data + cursor, &v, sizeof(v));
        cursor += sizeof(v);
    }

    if (hasFrame && cursor + sizeof(Ipc::DebugSnapshotMetadataWire) <= sizeof(resp.data)) {
        Ipc::DebugSnapshotMetadataWire meta{};
        meta.frameIdentityFlags = Ipc::kDebugSnapshotHasFrameTimestamp;
        meta.frameTimestamp = frame.timestamp;
        std::memcpy(resp.data + cursor, &meta, sizeof(meta));
        cursor += sizeof(meta);
    }

    resp.dataLen = static_cast<uint16_t>(cursor);
    Ipc::MarkSuccess(resp);
}

// ── IPC Command Handler ──────────────────────────────
Ipc::IpcResponse ServiceHost::HandleIpcCommand(const Ipc::IpcRequest& req) {
    Ipc::IpcResponse resp{};
    const auto withRunningPenEventBridge = [this](const auto& operation) {
        std::lock_guard<std::mutex> penLock(m_impl->m_penSubsystemMutex);
        auto* bridge = m_impl->m_penEventBridge.get();
        return bridge && bridge->IsRunning() && operation(*bridge);
    };

    if (Ipc::IsLegacyConfigTombstoneCommand(req.command)) {
        MarkLegacyConfigCommandUnsupported(req.command, resp);
        return resp;
    }

    switch (req.command) {
    case Ipc::IpcCommand::Ping:
        Ipc::MarkSuccess(resp);
        break;

    case Ipc::IpcCommand::EnterDebugMode:
        HandleIpcEnterDebugMode(resp);
        break;

    case Ipc::IpcCommand::ExitDebugMode:
        HandleIpcExitDebugMode(resp);
        break;

    case Ipc::IpcCommand::AfeCommand:
        if (req.paramLen < 2) {
            Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidRequest);
        } else if (!m_deviceRuntime) {
            Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidState);
        } else if (m_deviceRuntime->SubmitExternalAfeCommand(static_cast<AFE_Command>(req.param[0]), req.param[1])) {
            Ipc::MarkSuccess(resp);
        } else {
            Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidState);
        }
        break;

    case Ipc::IpcCommand::StartRuntime:
        if (m_deviceRuntime) {
            switch (m_deviceRuntime->RequestStart()) {
            case DeviceRuntime::StartRequestResult::Started:
                // 与 StartEGoTouchProvider 同理:运行时刚起来,把启动竞态里被丢掉的
                // 那次显示/盖子状态补投一遍,否则它永远等不到第二次。
                ReplayLastWakeEvent();
                Ipc::MarkSuccess(resp);
                LOG_INFO("Service", __func__, "IPC", "StartRuntime accepted: runtime started.");
                break;
            case DeviceRuntime::StartRequestResult::AlreadyRunning:
                Ipc::MarkSuccess(resp);
                LOG_INFO("Service", __func__, "IPC", "StartRuntime accepted: runtime already running (idempotent no-op).");
                break;
            case DeviceRuntime::StartRequestResult::Failed:
            default:
                Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InternalError);
                LOG_WARN("Service", __func__, "IPC", "StartRuntime failed: runtime did not start.");
                break;
            }
        } else {
            Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidState);
        }
        break;

    case Ipc::IpcCommand::StopRuntime:
        if (m_deviceRuntime) {
            switch (m_deviceRuntime->RequestStop()) {
            case DeviceRuntime::StopRequestResult::Stopped:
                Ipc::MarkSuccess(resp);
                LOG_INFO("Service", __func__, "IPC", "StopRuntime accepted: runtime stopped.");
                break;
            case DeviceRuntime::StopRequestResult::AlreadyStopped:
            default:
                Ipc::MarkSuccess(resp);
                LOG_INFO("Service", __func__, "IPC", "StopRuntime accepted: runtime already stopped (idempotent no-op).");
                break;
            }
        } else {
            Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidState);
        }
        break;

    case Ipc::IpcCommand::GetConfigCatalogV3:
        HandleIpcGetConfigCatalogV3(req, resp);
        break;

    case Ipc::IpcCommand::GetConfigSnapshotV3:
        HandleIpcGetConfigV3Snapshot(req, resp);
        break;

    case Ipc::IpcCommand::ApplyConfigPatchV3:
        HandleIpcConfigV3ApplyPatch(req, resp);
        break;

    case Ipc::IpcCommand::PersistConfigV3:
        HandleIpcConfigV3Persist(resp);
        break;

    case Ipc::IpcCommand::SetVhfEnabled:
        if (req.paramLen < 1) {
            Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidRequest);
        } else if (!m_deviceRuntime) {
            Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidState);
        } else {
            m_deviceRuntime->SetVhfEnabled(req.param[0] != 0);
            Ipc::MarkSuccess(resp);
        }
        break;

    case Ipc::IpcCommand::SetVhfTranspose:
        if (req.paramLen < 1) {
            Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidRequest);
        } else if (!m_deviceRuntime) {
            Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidState);
        } else {
            m_deviceRuntime->SetVhfTransposeEnabled(req.param[0] != 0);
            Ipc::MarkSuccess(resp);
        }
        break;

    case Ipc::IpcCommand::GetLogs:
        HandleIpcGetLogs(resp);
        break;

    case Ipc::IpcCommand::GetPenBridgeStatus:
        HandleIpcGetPenBridgeStatus(resp);
        break;

    case Ipc::IpcCommand::GetPenIdentityStatus:
        HandleIpcGetPenIdentityStatus(resp);
        break;

    case Ipc::IpcCommand::GetRuntimeStatus:
        if (!m_deviceRuntime) {
            Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidState);
            break;
        } else {
            const RuntimeSnapshot runtime = m_deviceRuntime->GetSnapshot();
            Ipc::RuntimeStatusWire wire{};
            wire.workerState = static_cast<int8_t>(runtime.state);
            if (runtime.state == workerState::streaming) {
                wire.flags |= Ipc::kRuntimeStatusStreaming;
            }
            if (m_deviceRuntime->IsVhfEnabled()) {
                wire.flags |= Ipc::kRuntimeStatusVhfEnabled;
            }
            if (m_deviceRuntime->IsVhfDeviceOpen()) {
                wire.flags |= Ipc::kRuntimeStatusVhfDeviceOpen;
            }
            if (m_deviceRuntime->IsVhfTransposeEnabled()) {
                wire.flags |= Ipc::kRuntimeStatusVhfTranspose;
            }
            wire.recoverCount = runtime.recover_count;
            wire.queueDepth = static_cast<uint16_t>(std::min<std::size_t>(runtime.queue_depth, UINT16_MAX));
            wire.lastCommandId = runtime.last_command_id;
            CopyCString(wire.lastNoteUtf8, sizeof(wire.lastNoteUtf8), runtime.last_note);
            wire.lastNoteUtf8Len = static_cast<uint16_t>(std::min<std::size_t>(runtime.last_note.size(), sizeof(wire.lastNoteUtf8) - 1));
            std::memcpy(resp.data, &wire, sizeof(wire));
            resp.dataLen = static_cast<uint16_t>(sizeof(wire));
            Ipc::MarkSuccess(resp);
            break;
        }

    case Ipc::IpcCommand::TriggerQueryHardwareVersion:
        if (withRunningPenEventBridge([](auto& bridge) {
                return bridge.SendQueryHardwareVersion();
            })) {
            Ipc::MarkSuccess(resp);
        } else {
            Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidState);
        }
        break;

    case Ipc::IpcCommand::TriggerQueryPenStatus:
        if (withRunningPenEventBridge([](auto& bridge) {
                return bridge.SendQueryPenStatus();
            })) {
            Ipc::MarkSuccess(resp);
        } else {
            Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidState);
        }
        break;

    case Ipc::IpcCommand::TriggerQueryPenInfo:
        if (withRunningPenEventBridge([](auto& bridge) {
                return bridge.SendFirstMcuStatusQuery();
            })) {
            Ipc::MarkSuccess(resp);
        } else {
            Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidState);
        }
        break;

    case Ipc::IpcCommand::TriggerSendScanMode:
        if (req.paramLen >= 3 &&
            withRunningPenEventBridge([&](auto& bridge) {
                return bridge.SendScanMode(req.param[0], req.param[1], req.param[2]);
            })) {
            Ipc::MarkSuccess(resp);
        } else {
            Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidState);
        }
        break;

    case Ipc::IpcCommand::TriggerSendFactoryInitParams:
        if (req.paramLen == 0 &&
            withRunningPenEventBridge([](auto& bridge) {
                return bridge.SendFactoryInitProtocolParams();
            })) {
            Ipc::MarkSuccess(resp);
        } else {
            Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidState);
        }
        break;

    case Ipc::IpcCommand::TriggerSendPairInfoSet:
        if (req.paramLen >= 1 &&
            withRunningPenEventBridge([&](auto& bridge) {
                return bridge.SendPairInfoSet(req.param[0]);
            })) {
            Ipc::MarkSuccess(resp);
        } else {
            Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidState);
        }
        break;


    case Ipc::IpcCommand::SetMasterParserOnly:
        if (req.paramLen >= 1 && m_deviceRuntime) {
            m_deviceRuntime->SetMasterParserOnlyMode(req.param[0] != 0);
            Ipc::MarkSuccess(resp);
            LOG_INFO("Service", __func__, "IPC", "Master parser only mode {}.", req.param[0] != 0 ? "enabled" : "disabled");
        } else {
            Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidState);
        }
        break;

    case Ipc::IpcCommand::SetPenPressureMode: {
        std::lock_guard<std::mutex> penLock(m_impl->m_penSubsystemMutex);
        if (req.paramLen >= 1 && m_impl->m_penPressureReader) {
            const auto mode = req.param[0] == 0
                ? Himax::Pen::PenPressureRangeMode::Raw12Bit4096
                : Himax::Pen::PenPressureRangeMode::Raw14Bit16382;
            m_impl->m_penPressureReader->SetPressureRangeMode(mode);
            Ipc::MarkSuccess(resp);
            LOG_INFO("Service", __func__, "MCU", "Pen pressure mode set to {}.", req.param[0] == 0 ? "4096" : "16382/4");
        } else {
            Ipc::MarkFailure(resp, Ipc::IpcStatusCode::InvalidState);
        }
        break;
    }

    case Ipc::IpcCommand::GetDebugSchema:
        HandleIpcGetDebugSchema(req, resp);
        break;

    case Ipc::IpcCommand::GetDebugSnapshot:
        HandleIpcGetDebugSnapshot(resp);
        break;

    default:
        break;
    }

    return resp;
}

#endif

} // namespace Service
