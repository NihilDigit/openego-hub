#pragma once

#include "ConfigRuntime.h"
#include "PenButtonConfig.h"
#include "ServiceConfigCore.h"

#include "GaokunKeyboard.h"
#include "GaokunPen.h"
#include "GaokunThp.h"

#include <atomic>
#include <thread>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class DeviceRuntime;
struct RuntimePenState;

namespace Config {
class ConfigStore;
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

    // 触控由 gaokun-hal 的 ARM64EC 宿主进程提供，这里只持有它的生命周期。声明放在 Impl
    // 之外是因为 provider 的启停要直接用到它，而 Impl 是不完全类型。
    Gaokun::Thp::HostController m_thpHost;

    // 笔与键盘的 MCU 通道同样来自 gaokun-hal：那边直接驱动厂商的 PenService.dll 与
    // KeyboardService.dll，型号识别与按键语义都留在厂商实现里，不必在本仓库重新推导一遍。
    Gaokun::Pen::HostController m_penHost;
    Gaokun::Pen::SnapshotReader m_penSnapshots;
    Gaokun::Pen::EventReader m_penEvents;
    Gaokun::Keyboard::HostController m_kbdHost;
    Gaokun::Keyboard::SnapshotReader m_kbdSnapshots;
    Gaokun::Keyboard::EventReader m_kbdEvents;

    // 轮询两条通道并转发到 PenStatusChannel。共享内存快照本身就是可重复读的，所以这里
    // 用轮询而不是等通知：错过一轮没有代价，而少一个跨进程的唤醒路径就少一处可能卡住的地方。
    std::thread m_accessoryThread;
    std::atomic<bool> m_accessoryStop{false};

    // 宿主可执行文件的位置：优先本服务同目录（部署形态），其次 hal 的构建目录（开发形态）。
    [[nodiscard]] static std::wstring ResolveHostPath(const wchar_t* exeName);
    [[nodiscard]] static std::wstring ResolveThpHostPath();

    void AccessoryLoop();
    void PublishAccessoryStatus();

    // 与 IPC 无关，所以不在守卫里：PublishPenStatus 在所有配置里都要编。
    static void CopyCString(char* dst, size_t dstSize, std::string_view src);

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
};

} // namespace Service

