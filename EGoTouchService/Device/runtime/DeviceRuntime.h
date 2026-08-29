#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "PenButtonConfig.h"
#include "btmcu/PenUsbTypes.h"
#include "win32/SyntheticPenButtonInjector.h"

namespace Config {
class ConfigStore;
struct ValidationResult;
}

#include "BtPenInputLatch.h"

// 笔的协议代次。由笔身份或 MCU 上报的模块号推断。采集与解帧都已经交给 GaokunThpHost 里的
// 原厂算法链，这里只剩一个随笔身份一起发布的状态字段。
enum class StylusProtocolHint : uint8_t {
    Auto = 0,
    Hpp2,
    Hpp3,
};

// --------------- 按键与状态动作辅助类型 ---------------
struct PenStateUpdateResult {
    bool stateChanged = false;
    bool applyToPipeline = true;
    bool stylusIdChanged = false;
    uint8_t nextStylusId = 0;
};

struct PenButtonAction {
    enum class Type {
        Barrel,
        Eraser,
        DoubleClick,   ///< 侧键双击。离散手势，pressed 对它没有意义
    };
    Type type;
    // 只对 Barrel/Eraser 有效。DoubleClick 是一次性事件，没有对应的松开——此前把它当成
    // 「按下且永不释放的 barrel」，那个「不释放的 bug」其实是建模错位，不是 MCU 的问题。
    bool pressed = false;
    uint8_t rawPayload = 0;
};

// --------------- 基础类型 ---------------

class RuntimePolicyEvent {
public:
    enum class Type : uint8_t {
        Unknown = 0,
        DisplayOn,
        DisplayOff,
        LidOn,
        LidOff,
        Suspend,
        Shutdown,
        ResumeAutomatic,
    };

    enum class Source : uint8_t {
        HostSystemState = 0,
    };

    Type type = Type::Unknown;
    Source source = Source::HostSystemState;
    std::chrono::system_clock::time_point timestamp{};
    uint32_t rawIndex = 0;
};

const char* ToString(RuntimePolicyEvent::Type type) noexcept;

struct RuntimePenState {
    uint16_t factoryStatusFlags = 0;

    bool hasConnection = false;
    bool connected = false;

    bool hasPairStatus = false;
    uint8_t pairStatus = 0;

    bool hasStylusId = false;
    uint8_t stylusId = 0;

    StylusProtocolHint protocolHint = StylusProtocolHint::Auto;
    bool protocolHintFromPenModule = false;
    // 任何一次提交的状态变化都让 penRevision 前进一格，电量、充电、笔身份不作区分。
    // 没有消费方需要区分：生产代码只读快照本身，只有单元测试断言这个计数。真正要分辨
    // 「身份变了」的是 pipelineRevision——它单独计数，且只在 applyToPipeline 时前进。
    // 代价是拿 penRevision 当「笔换了」的判据会误判成电量刷新，谁要这么用先拆开它。
    uint32_t penRevision = 0;
    uint32_t pipelineRevision = 0;

    bool hasPenModuleModelId = false;
    uint32_t penModuleModelId = 0;
    Himax::Pen::PenModuleModel penModuleModel = Himax::Pen::PenModuleModel::Unknown;

    bool hasSerialNumber = false;
    std::string serialNumber;

    bool hasHardwareVersion = false;
    std::string hardwareVersion;

    bool hasFirmwareVersion = false;
    std::string firmwareVersion;

    bool hasCurrentMode = false;
    Himax::Pen::PenCurrentMode currentMode = Himax::Pen::PenCurrentMode::Unknown;
    uint8_t currentModeRaw = 0;

    bool hasEraserToggle = false;
    uint8_t eraserToggle = 0;

    bool hasCurrentFunc = false;
    uint8_t currentFunc = 0;

    bool hasBatteryLevel = false;
    uint8_t batteryLevel = 0;

    bool hasChargingState = false;
    bool charging = false;

    bool hasDeviceConnected = false;
    bool deviceConnected = false;
};

struct PenAfeReplayState {
    uint64_t generation = 0;
    bool pending = false;

    void BeginInitCycle() noexcept {
        ++generation;
        if (generation == 0) {
            ++generation;
        }
        pending = true;
    }

    void CompleteInitCycle() noexcept { pending = false; }

    bool IsCurrent(uint64_t commandGeneration) const noexcept {
        return commandGeneration != 0 &&
               commandGeneration == generation &&
               !pending;
    }
};

// --------------- DeviceRuntime ---------------

struct DeviceRuntimePenStateTestAccess;

/// 笔与键盘 MCU 事件的状态机。
///
/// 触控的采集、解算与注入都在 GaokunThpHost 里，由原厂 THP_Service.dll 完成，本类不再碰帧、
/// 不再注入 HID、也不再驱动芯片。留下的是三件事：把 MCU 事件归并成一份可发布的笔状态、
/// 按当前的侧键模式分派按键动作、以及缓存蓝牙笔的压力样本。
class DeviceRuntime {
public:
    DeviceRuntime(const std::wstring& master,
                  const std::wstring& slave,
                  const std::wstring& interrupt);
    ~DeviceRuntime();
    DeviceRuntime(const DeviceRuntime&) = delete;
    DeviceRuntime& operator=(const DeviceRuntime&) = delete;

    void Stop();

    // Auto/Manual 模式
    void SetAutoMode(bool enabled) { m_autoMode.store(enabled); }
    bool IsAutoMode() const { return m_autoMode.load(); }

    void ApplyServicePolicy(bool autoMode, bool stylusVhfEnabled,
                            PenButtonMode penButtonMode = PenButtonMode::WindowsInk,
                            PenButtonRoute penButtonRoute = PenButtonRoute::VhfOnly,
                            bool penButtonRouteExplicit = false);
    // Config-only, no IPC types involved. Available in every build so Release applies
    // the declared defaults instead of falling back to member initializers.
    Config::ValidationResult ValidateConfigStore(const Config::ConfigStore& store) const;

    void SetPenButtonMode(PenButtonMode m) { m_penButtonMode.store(m, std::memory_order_release); }
    PenButtonMode GetPenButtonMode() const { return m_penButtonMode.load(std::memory_order_acquire); }
    void SetPenButtonRoute(PenButtonRoute r, bool explicitRoute = true) {
        m_penButtonRoute.store(r, std::memory_order_release);
        m_penButtonRouteExplicit.store(explicitRoute, std::memory_order_release);
    }
    PenButtonRoute GetPenButtonRoute() const { return m_penButtonRoute.load(std::memory_order_acquire); }

    // 抑制状态仍然发布——托盘的 ScopedOneNoteInputSuppression 阻塞等待 PenStatusChannel
    // 里的 hasInputSuppressed 变化，状态位没了它会一直等——但实际的输入闸门已经不在这一侧：
    // 实时笔输入来自 GaokunThpHost 里的原厂 VHF，这个开关目前挡不住它。
    void SetInputSuppressed(bool suppressed);
    bool IsInputSuppressed() const;

    /// 注入 BT MCU 压感值（由 PenBridge 线程写入）
    void IngestBtMcuPressure(uint16_t p);
    void IngestBtMcuPressurePacket(const std::array<uint16_t, 4>& pressure,
                                   const std::array<uint16_t, 4>& rawPressure,
                                   uint8_t freq1,
                                   uint8_t freq2);

    void IngestPolicyEvent(const RuntimePolicyEvent& ev);

    RuntimePenState GetPenStateSnapshot() const;
    PenAfeReplayState GetPenAfeReplayStateSnapshot() const;

    /// MCU 事件 ingress（runtime 内部完成状态分派）
    void IngestPenEvent(const Himax::Pen::PenEvent& ev);

    /// Invoked after any pen-state change commits, with the lock released. Lets the host
    /// republish state without polling. Available in every configuration: the pen status
    /// broadcast has to work in a shipping build, where IPC is compiled out.
    using PenStateChangedCallback = std::function<void(const RuntimePenState&)>;
    void SetPenStateChangedCallback(PenStateChangedCallback cb);

    /// 侧键双击。需要用户会话的行为（Windows Ink 快捷键、OneNote 工具同步）只由运行时
    /// 报告手势，不在服务里执行——服务在会话 0，没有可交互输入桌面。
    /// 返回值是「手势真的送达了伴随进程」：托盘没在跑、通知事件没建成时返回 false，
    /// 否则日志会把无声失效记成注入成功。
    using PenDoubleClickCallback = std::function<bool()>;
    void SetPenDoubleClickCallback(PenDoubleClickCallback cb);

    // ToggleEraser 需要让 ARM64EC 宿主调用厂商 PenService.dll。返回值只用于日志；即使厂商
    // 命令暂时失败，用户会话里的 OneNote workaround 仍照常收到这次边沿。
    using PenCurrentFuncCommandCallback = std::function<bool(bool)>;
    void SetPenCurrentFuncCommandCallback(PenCurrentFuncCommandCallback cb);

private:
    friend struct DeviceRuntimePenStateTestAccess;
    void HandlePenButtonStatusCode(uint8_t statusCode,
                                   uint8_t rawEventPayload,
                                   const char* source);
    void ApplyPenSessionChange();
    void UpdatePenState(std::function<void(RuntimePenState&, PenStateUpdateResult&)> updateFn);
    void DispatchPenButtonAction(const PenButtonAction& action, const char* source);
    // 橡皮擦开关的唯一状态源，硬件事件与 ToggleEraser 双击共用。
    bool IsEraserActive() const;
    void UpdateEraserState(bool active);

    RuntimePenState m_penState{};
    mutable std::mutex m_penStateMu;
    mutable std::mutex m_penIngressMu;
    PenAfeReplayState m_penReplay{};
    std::atomic<bool> m_autoMode{false};
    std::atomic<bool> m_inputSuppressed{false};
    std::atomic<PenButtonMode> m_penButtonMode{PenButtonMode::WindowsInk};
    std::atomic<PenButtonRoute> m_penButtonRoute{PenButtonRoute::VhfOnly};
    std::atomic<bool> m_penButtonRouteExplicit{false};
    SyntheticPenButtonInjector m_synthPenButton;
    Device::BtPenInputLatch m_btPenLatch;

    // 配置校验与策略应用共用一把锁，二者都不在 MCU 事件路径上。
    mutable std::mutex m_policyMu;

    // 电源事件的去抖窗口。索引是 RuntimePolicyEvent::Type。
    mutable std::mutex m_mu;
    std::array<std::chrono::steady_clock::time_point, 16> m_lastEventByType{};
    std::atomic<bool> m_shutdownRequested{false};

    mutable std::mutex m_penStateChangedCbMu;
    PenStateChangedCallback m_penStateChangedCb;
    mutable std::mutex m_penDoubleClickCbMu;
    PenDoubleClickCallback m_penDoubleClickCb;
    mutable std::mutex m_penCurrentFuncCommandCbMu;
    PenCurrentFuncCommandCallback m_penCurrentFuncCommandCb;
};
