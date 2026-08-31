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

/// 启动阶段编号。启动失败时经 SCM 的 dwServiceSpecificExitCode 上报，值本身会被
/// 事件日志和 `sc.exe query` 原样显示出来，所以只增不改：改了以后旧日志里的数字就
/// 指向另一个阶段了。
enum class ServiceStartPhase : uint32_t {
    None = 0,
    Config = 1,
    RuntimeAndPipeline = 2,
    IpcSubsystem = 3,
    PenSubsystem = 4,
    SystemStateMonitor = 5,
    Exception = 6,
};

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

    /// Start() 返回 false 时停在哪个阶段。记录的是最后一个进入过的阶段，成功路径不清零，
    /// 所以只在 Start() 失败之后读它有意义。
    ServiceStartPhase LastFailedPhase() const {
        return m_startPhase.load(std::memory_order_acquire);
    }

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

    // 各 Start 阶段入口处写入。初始化跑在后台线程上，读取方是 SCM 线程，因此用原子。
    std::atomic<ServiceStartPhase> m_startPhase{ServiceStartPhase::None};

    std::unique_ptr<DeviceRuntime> m_deviceRuntime;
    std::unique_ptr<Impl> m_impl;

    // 触控由 gaokun-hal 的 ARM64EC 宿主进程提供，这里只持有它的生命周期。声明放在 Impl
    // 之外是因为 provider 的启停要直接用到它，而 Impl 是不完全类型。
    Gaokun::Thp::HostController m_thpHost;

    // 笔与键盘的 MCU 通道同样来自 gaokun-hal：那边直接驱动厂商的 PenService.dll 与
    // KeyboardService.dll，型号识别与按键语义都留在厂商实现里，不必在本仓库重新推导一遍。
    //
    // 六条通道都用 unique_ptr 持有：宿主重启之后旧句柄指着已经死掉的那个进程，读永远读不到
    // 新数据，必须整体丢掉重开。这些通道类没有 Close()，析构才关句柄，直接再 Open 一次会
    // 把旧句柄漏掉。空指针即「未打开」，AccessoryLoop 每轮据此补开一次。
    Gaokun::Pen::HostController m_penHost;
    std::unique_ptr<Gaokun::Pen::SnapshotReader> m_penSnapshots;
    std::unique_ptr<Gaokun::Pen::EventReader> m_penEvents;
    std::unique_ptr<Gaokun::Pen::CommandWriter> m_penCommands;
    Gaokun::Keyboard::HostController m_kbdHost;
    std::unique_ptr<Gaokun::Keyboard::SnapshotReader> m_kbdSnapshots;
    std::unique_ptr<Gaokun::Keyboard::EventReader> m_kbdEvents;
    // 常驻键盘宿主的命令管道。分离开关经它下发，不再另起一个一次性实例去抢同一个 MCU 端点。
    std::unique_ptr<Gaokun::Keyboard::CommandWriter> m_kbdCommands;

    // 轮询两条通道并转发到 PenStatusChannel。共享内存快照本身就是可重复读的，所以这里
    // 用轮询而不是等通知：错过一轮没有代价，而少一个跨进程的唤醒路径就少一处可能卡住的地方。
    std::thread m_accessoryThread;
    std::atomic<bool> m_accessoryStop{false};

    // 宿主可执行文件的位置：优先本服务同目录（部署形态），其次 hal 的构建目录（开发形态）。
    [[nodiscard]] static std::wstring ResolveHostPath(const wchar_t* exeName);
    [[nodiscard]] static std::wstring ResolveThpHostPath();

    void AccessoryLoop();
    // 巡检一个 hal 宿主：判死、按预算重启、重启后重开它的通道。
    void SuperviseAccessoryHosts();
    // 未打开的通道补开一次。宿主刚拉起时映射和管道还没建好，打不开是常态。
    void OpenAccessoryChannels();
    void CloseKeyboardChannels();
    void ClosePenChannels();
    // 状态快照的唯一构造点，见实现处的说明。
    void PublishStatusSnapshot();

    // 与 IPC 无关，所以不在守卫里：状态发布在所有配置里都要编。
    static void CopyCString(char* dst, size_t dstSize, std::string_view src);

    bool InitializeConfigStores();
    void ReplayLastWakeEvent();
    void ApplyServiceConfigToRuntime(const ServiceConfigState& config);

    // 托盘控制通道：起停线程、消费一条提交、把新模式落到运行时并持久化。
    void StartPenControlChannel();
    void StopPenControlChannel();
    void PenControlThreadMain();
    void DispatchPendingPowerEvent();
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
    void StartPenEventBridge();

    void StopIpcServer();
    void CloseIpcResources();
    void StopPenSubsystem();
    void StopSystemStateMonitor();
    void StopRuntimeSubsystem();
};

} // namespace Service
