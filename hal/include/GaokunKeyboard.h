#pragma once

#include <chrono>
#include <cstdint>
#include <new>
#include <string>

// 键盘的状态与事件通道。结构与 GaokunPen.h 刻意保持同构：两者的厂商 DLL 调用模型相同,
// 上层的消费方式也就相同，读一个会读另一个。
//
// 宿主 GaokunKeyboardHost.exe 是 ARM64EC（它加载 x64 的 KeyboardService.dll），本头文件与
// 读者侧实现是原生 ARM64。
namespace Gaokun::Keyboard {

inline constexpr int kVersionCapacity = 32;
inline constexpr int kSerialCapacity = 40;
// 产品名是 UTF-8，容量按最长的那个留够；截断会切断多字节序列，渲染侧解码直接失败。
inline constexpr int kModelNameCapacity = 48;

enum class Flag : uint32_t {
    HasBattery = 1u << 0,
    Charging = 1u << 1,
    HasCharging = 1u << 2,
    Connected = 1u << 3,
    HasConnected = 1u << 4,
    HasFirmware = 1u << 5,
    HasHardware = 1u << 6,
    HasSerial = 1u << 7,
    HasModule = 1u << 8,
    Detached = 1u << 9,        ///< 键盘已分离
    HasDetached = 1u << 10,
    DetachSupport = 1u << 11,  ///< 分离后允许无线连接
    HasDetachSupport = 1u << 12,
    SMode = 1u << 13,          ///< IsSmode 的回报
    HasSMode = 1u << 14,
};

struct Snapshot {
    uint32_t flags = 0;
    uint32_t moduleId = 0;
    uint8_t battery = 0;
    uint8_t _pad0 = 0;
    uint16_t _pad1 = 0;
    // 宿主还活着的判据。updatedAtUnixMs 只在有字段变化时前移，宿主死后 seqlock 停在最后
    // 一帧，读者拿到的仍是一份自洽的旧快照而无从分辨。心跳每秒无条件自增一次，读者比对
    // 两次采样即可。占的是原来的 _pad2，旧读者当填充跳过，176 字节布局不变。
    uint32_t heartbeat = 0;
    uint64_t updatedAtUnixMs = 0;
    char firmware[kVersionCapacity]{};
    char hardware[kVersionCapacity]{};
    char serial[kSerialCapacity]{};
    // 可直接显示的产品名，由 hal 按固件串的平台前缀判定后填好。
    char modelName[kModelNameCapacity]{};
};
static_assert(sizeof(Snapshot) == 176, "Snapshot layout must stay fixed across both sides");

// 只追加，不插入也不重排：写者在宿主里、读者在服务里，两个可执行分开构建，插一个值会
// 让不匹配的一对错位解释所有后续事件。
enum class EventKind : uint32_t {
    None = 0,
    ConnectRequest,
    ConnectResult,
    DetachChanged,        ///< 插拔，value 非零表示已分离
    DetachSupportChanged, ///< 开关被改动，value 是 MCU 的回显
    FirstBatteryAfterConnect,
    // 命令通道上一条 SetDetachSupport 的落地结果。与 DetachSupportChanged 分开，是因为
    // 后者由厂商回调产生，无从区分「谁改的」和「改成功没有」。
    DetachSupportResult,  ///< value: 1 已启用 / 0 已停用 / -1 DLL 不支持 / -2 读回超时
};

struct Event {
    uint32_t kind = 0;
    int32_t value = 0;
    uint64_t atUnixMs = 0;
};
static_assert(sizeof(Event) == 16, "Event layout must stay fixed across both sides");

// ---- 下行命令 ----

// 同样只追加。与 Gaokun::Pen::CommandKind 同构。
enum class CommandKind : uint32_t {
    None = 0,
    SetDetachSupport,
};

struct Command {
    uint32_t kind = 0;
    int32_t value = 0;
};
static_assert(sizeof(Command) == 8, "Command layout must stay fixed across both sides");

class SnapshotReader {
public:
    SnapshotReader() noexcept = default;
    ~SnapshotReader() noexcept;

    SnapshotReader(const SnapshotReader &) = delete;
    SnapshotReader &operator=(const SnapshotReader &) = delete;

    [[nodiscard]] bool Open() noexcept;
    [[nodiscard]] bool Read(Snapshot &out) const noexcept;

private:
    // 不透明指针，指向内部的 seqlock 读者，见 GaokunPen.h 的同一处说明。
    void *m_impl = nullptr;
};

class EventReader {
public:
    EventReader() noexcept = default;
    ~EventReader() noexcept;

    EventReader(const EventReader &) = delete;
    EventReader &operator=(const EventReader &) = delete;

    [[nodiscard]] bool Open() noexcept;
    [[nodiscard]] bool Poll(Event &out) noexcept;

private:
    void *m_impl = nullptr;
};

// 向常驻宿主下发命令。与 Gaokun::Pen::CommandWriter 同构。
class CommandWriter {
public:
    CommandWriter() noexcept = default;
    ~CommandWriter() noexcept;

    CommandWriter(const CommandWriter &) = delete;
    CommandWriter &operator=(const CommandWriter &) = delete;

    // 宿主未运行时返回 false，调用方据此退到 SetDetachSupport 那条一次性路径。
    [[nodiscard]] bool Open() noexcept;
    [[nodiscard]] bool SetDetachSupport(bool enable) noexcept;

private:
    void *m_impl = nullptr;
};

enum class StartResult {
    Started = 0,
    AlreadyRunning,
    HostNotFound,
    LaunchFailed,
    ExitedImmediately,
};

class HostController {
public:
    HostController() noexcept = default;
    ~HostController() noexcept;

    HostController(const HostController &) = delete;
    HostController &operator=(const HostController &) = delete;

    // extraArgs 原样附在命令行末尾，调用方用它把自己的日志级别传下去。
    [[nodiscard]] StartResult Start(const std::wstring &hostExePath,
                                    const std::wstring &extraArgs = {}) noexcept;
    [[nodiscard]] bool Stop(std::chrono::milliseconds timeout = std::chrono::seconds(10)) noexcept;
    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] int ExitCode() const noexcept;

private:
    void CloseHandles() noexcept;

    // 不透明指针，指向内部的宿主进程对象；停止之后仍要能回答退出码，所以额外留一份。
    void *m_process = nullptr;
    int m_lastExitCode = -1;
};

// 分离后无线连接开关的一次性设置：另起一个宿主进程跑一次命令行。
//
// 常驻宿主在跑的时候不要走这条路，用 CommandWriter。这里拉起的第二个实例会和常驻宿主
// 争抢同一个 MCU 端点，本身就容易失败；即使成功，回显也发生在一次性实例里，常驻宿主的
// 快照最快要等下一次整表刷新才翻转，界面按回读判定就会弹回。留着它是因为服务在宿主没起
// 来时仍要能改这个开关。
[[nodiscard]] bool SetDetachSupport(const std::wstring &hostExePath, bool enable) noexcept;

} // namespace Gaokun::Keyboard
