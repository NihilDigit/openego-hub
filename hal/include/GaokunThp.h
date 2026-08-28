#pragma once

#include <chrono>
#include <string>

// 逆向层对外的唯一接口：启动与停止 ARM64EC 的原厂 THP 宿主进程。
//
// 这个头本身不接触任何华为 DLL，只用 Win32 的进程与同步原语，因此可以编译进原生 ARM64 的
// 调用方。需要 ARM64EC 的只有 GaokunThpHost.exe，它在自己的进程里加载 x64 的 THP_Service.dll
// 及其依赖。进程边界同时也是架构边界，调用方不必为了这条链路改变自身的目标架构。
//
// 调用方需要知道的全部前提：机器上注册着 HuaweiThpService（可以处于停止或禁用状态，宿主
// 只读它的 ImagePath 来定位原厂目录），且在启动宿主之前该服务已经停止——设备同一时刻只能
// 由一个实现持有。
namespace Gaokun::Thp {

enum class StartResult {
    Started = 0,
    AlreadyRunning,
    HostNotFound,      ///< hostExePath 指向的文件不存在
    LaunchFailed,      ///< CreateProcess 失败
    ExitedImmediately, ///< 进程起来了但很快退出，通常是设备仍被占用或原厂目录定位失败
};

class HostController {
public:
    HostController() noexcept = default;
    ~HostController() noexcept;

    HostController(const HostController &) = delete;
    HostController &operator=(const HostController &) = delete;

    // 拉起宿主并接管触控。宿主会等待本进程的句柄，因此调用方即便崩溃，宿主也会自行
    // 走完 ThpFuncStop 再退出，不会留下占着设备的孤儿。
    [[nodiscard]] StartResult Start(const std::wstring &hostExePath) noexcept;

    // 请求宿主停止并等待它退出。超时后强制终止并返回 false——那种情况下设备可能停在
    // 中间状态，调用方应当在交还原厂服务前把这一点记进日志。
    [[nodiscard]] bool Stop(std::chrono::milliseconds timeout = std::chrono::seconds(15)) noexcept;

    [[nodiscard]] bool IsRunning() const noexcept;

    // 宿主的退出码，仍在运行时为 -1。Stop 之后依然可读：句柄在那里已经关闭，退出码是在
    // 关闭之前取下来留存的，否则调用方在停止后就再也拿不到它。
    [[nodiscard]] int ExitCode() const noexcept;

private:
    void CloseHandles() noexcept;

    void *m_process = nullptr;   // HANDLE
    void *m_stopEvent = nullptr; // HANDLE
    unsigned long m_pid = 0;
    int m_lastExitCode = -1;
};

} // namespace Gaokun::Thp
