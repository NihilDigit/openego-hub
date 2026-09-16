#pragma once

#include <chrono>
#include <string>

// 逆向层对外的唯一接口：启动与停止 ARM64EC 的原厂 THP 宿主。
//
// 这个头本身不接触任何华为 DLL，只用 Win32 的服务与进程原语，因此可以编译进原生 ARM64 的
// 调用方。需要 ARM64EC 的只有 GaokunThpHost.exe，它在自己的进程里加载 x64 的 THP_Service.dll
// 及其依赖。进程边界同时也是架构边界，调用方不必为了这条链路改变自身的目标架构。
//
// **宿主必须作为服务运行，不能用 CreateProcess 拉起。** THP_Service.dll 内部另有一个
// ServiceMain，它要先向 SCM 注册控制处理器才会订阅电源通知；注册失败时原厂那条链拿不到
// 电源状态，ApDaemon 把系统当成没通电并挂起，触摸帧一帧都不处理。非服务进程注册必然失败。
// 零售版 ApDaemon 在缺少电源通知时默认认为有电，所以这个前提长期没有暴露；工程版默认相反，
// 换上就是整条链停摆。来龙去脉见 hal/docs/thp-power-gate.md。
//
// 服务名不必是 HuaweiThpService：RegisterServiceCtrlHandlerExW 对 SERVICE_WIN32_OWN_PROCESS
// 忽略服务名，原厂硬编码的那个名字会绑到当前进程所属的服务上。
//
// 调用方需要知道的全部前提：机器上注册着 HuaweiThpService（可以处于停止或禁用状态，宿主
// 只读它的 ImagePath 来定位原厂目录），且在启动宿主之前该服务已经停止——设备同一时刻只能
// 由一个实现持有。
namespace Gaokun::Thp {

enum class StartResult {
    Started = 0,
    AlreadyRunning,
    HostNotFound,      ///< hostExePath 指向的文件不存在
    LaunchFailed,      ///< 注册或启动宿主服务失败
    ExitedImmediately, ///< 服务起来了但很快停止，通常是设备仍被占用或原厂目录定位失败
};

class HostController {
public:
    HostController() noexcept = default;
    ~HostController() noexcept;

    HostController(const HostController &) = delete;
    HostController &operator=(const HostController &) = delete;

    // 注册（或对齐）宿主服务并启动它接管触控。宿主会等待本进程的句柄，因此调用方即便崩溃，
    // 宿主也会自行走完 ThpFuncStop 并把原厂服务请回来，不会留下占着设备的孤儿——SCM 不做
    // 这件事，服务之间没有父子关系。
    // extraArgs 原样写进服务的 ImagePath，调用方用它把自己的日志级别传下去。
    [[nodiscard]] StartResult Start(const std::wstring &hostExePath,
                                    const std::wstring &extraArgs = {}) noexcept;

    // 请求宿主停止并等待它退出。超时后强制终止并返回 false——那种情况下设备可能停在
    // 中间状态，调用方应当在交还原厂服务前把这一点记进日志。
    [[nodiscard]] bool Stop(std::chrono::milliseconds timeout = std::chrono::seconds(15)) noexcept;

    [[nodiscard]] bool IsRunning() const noexcept;

    // 宿主最后一次停止时向 SCM 报告的退出码，仍在运行时为 -1。
    [[nodiscard]] int ExitCode() const noexcept;

private:
    int m_lastExitCode = -1;
};

} // namespace Gaokun::Thp
