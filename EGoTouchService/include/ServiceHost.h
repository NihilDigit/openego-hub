#pragma once

#include "ConfigRuntime.h"
#include "PenButtonConfig.h"
#include "ServiceConfigCore.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

class DeviceRuntime;
struct RuntimePenState;

namespace Config {
class ConfigStore;
}

namespace Himax::Pen {
struct PenPressureStats;
}

namespace Ipc {
struct DebugFieldSchemaWire;
struct IpcRequest;
struct IpcResponse;
}

namespace Solvers {
struct HeatmapFrame;
}

namespace PenControl {
struct Command;
}

namespace PenStatus {
enum class TouchProviderState : uint8_t;
}

namespace Service {

class ServiceLifecycleCoordinator;

/// 模块加载器：负责创建、连接、启停所有子模块。
/// 不知道 SCM 的存在，可以独立测试。
class ServiceHost {
public:
    ServiceHost();
    ~ServiceHost();

    ServiceHost(const ServiceHost&) = delete;
    ServiceHost& operator=(const ServiceHost&) = delete;

    /// 按依赖顺序启动所有模块
    bool Start();

    /// 逆序停止所有模块
    void Stop();

    ServiceMode GetMode() const { return m_runtimeMode; }

private:
    friend class ServiceLifecycleCoordinator;
    struct Impl;

    ServiceConfigState m_configState{};
    ServiceMode m_runtimeMode = ServiceMode::Full;
    // ConfigRuntime holds the code-defined defaults and has no IPC dependency of its
    // own (see ConfigRuntime.h). It must exist in every build: it is the only source of
    // the startup ConfigStore, and without it Release would fall back to whatever the
    // pipeline member initializers happen to hold.
    ConfigRuntime m_configRuntime;

    std::unique_ptr<DeviceRuntime> m_deviceRuntime;
    std::unique_ptr<Impl> m_impl;

    // 与 IPC 无关，所以不在守卫里：PublishPenStatus 在所有配置里都要编。
    static void CopyCString(char* dst, size_t dstSize, std::string_view src);

#if EGOTOUCH_SERVICE_ENABLE_IPC
    static uint32_t HashDebugSchema(const std::vector<Ipc::DebugFieldSchemaWire>& defs);
    static uint16_t DeriveDebugSchemaVersion(uint32_t schemaHash);
    static uint64_t EncodeDebugValue(const Solvers::HeatmapFrame& frame,
                                     const Ipc::DebugFieldSchemaWire& def,
                                     bool& valid);
    static uint64_t EncodePenValue(const Himax::Pen::PenPressureStats& s,
                                   bool evtRunning,
                                   bool pressRunning,
                                   int16_t sourceIndex,
                                   bool& valid);
    void BuildDebugSchema();
#endif

    bool InitializeConfigStores();
    void PublishPenStatus(const RuntimePenState& state);
    void ReplayLastWakeEvent();
    void ApplyServiceConfigToRuntime(const ServiceConfigState& config);

    // 托盘控制通道：起停线程、消费一条提交、把新模式落到运行时并持久化。
    void StartPenControlChannel();
    void StopPenControlChannel();
    void PenControlThreadMain();
    void HandlePenControlCommand(const PenControl::Command& command);
    void ApplyPenButtonMode(PenButtonMode mode, const char* source, bool persist);
    bool StartEGoTouchProvider();
    bool StopEGoTouchProvider();
    void SetInputSuppressed(bool suppressed);
    void TickInputSuppressionTimeout();
    void PublishTouchProviderState(PenStatus::TouchProviderState state,
                                   uint8_t error);
    void RepublishPenStatus();
#if EGOTOUCH_SERVICE_ENABLE_IPC
    ReloadServiceConfigResult HandleReloadServiceConfig(const ServiceConfigState& reloadedConfig);
#endif
    bool ValidateStartupConfig(const Config::ConfigStore& store) const;
    bool StartRuntimeAndPipeline();
    bool StartSystemStateMonitor();
    bool StartIpcSubsystem();
    bool StartPenSubsystem();

    void StopIpcServer();
    void CloseIpcResources();
    void StopPenSubsystem();
    void StopSystemStateMonitor();
    void StopRuntimeSubsystem();

#if EGOTOUCH_SERVICE_ENABLE_IPC
    void HandleIpcEnterDebugMode(Ipc::IpcResponse& resp);
    void HandleIpcExitDebugMode(Ipc::IpcResponse& resp);
    void HandleIpcGetConfigCatalogV3(const Ipc::IpcRequest& req, Ipc::IpcResponse& resp);
    void HandleIpcGetConfigV3Snapshot(const Ipc::IpcRequest& req, Ipc::IpcResponse& resp);
    void HandleIpcConfigV3ApplyPatch(const Ipc::IpcRequest& req, Ipc::IpcResponse& resp);
    void HandleIpcConfigV3Persist(Ipc::IpcResponse& resp);
    void HandleIpcGetLogs(Ipc::IpcResponse& resp);
    void HandleIpcGetPenBridgeStatus(Ipc::IpcResponse& resp);
    void HandleIpcGetPenIdentityStatus(Ipc::IpcResponse& resp);
    void HandleIpcGetDebugSchema(const Ipc::IpcRequest& req, Ipc::IpcResponse& resp);
    void HandleIpcGetDebugSnapshot(Ipc::IpcResponse& resp);

    Ipc::IpcResponse HandleIpcCommand(const Ipc::IpcRequest& req);
#endif
};

} // namespace Service

