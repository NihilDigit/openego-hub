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
    uint32_t _pad2 = 0;
    uint64_t updatedAtUnixMs = 0;
    char firmware[kVersionCapacity]{};
    char hardware[kVersionCapacity]{};
    char serial[kSerialCapacity]{};
    // 可直接显示的产品名，由 hal 按固件串的平台前缀判定后填好。
    char modelName[kModelNameCapacity]{};
};
static_assert(sizeof(Snapshot) == 176, "Snapshot layout must stay fixed across both sides");

enum class EventKind : uint32_t {
    None = 0,
    ConnectRequest,
    ConnectResult,
    DetachChanged,        ///< 插拔，value 非零表示已分离
    DetachSupportChanged, ///< 开关被改动
    FirstBatteryAfterConnect,
};

struct Event {
    uint32_t kind = 0;
    int32_t value = 0;
    uint64_t atUnixMs = 0;
};
static_assert(sizeof(Event) == 16, "Event layout must stay fixed across both sides");

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

    [[nodiscard]] StartResult Start(const std::wstring &hostExePath) noexcept;
    [[nodiscard]] bool Stop(std::chrono::milliseconds timeout = std::chrono::seconds(10)) noexcept;
    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] int ExitCode() const noexcept;

private:
    void CloseHandles() noexcept;

    // 不透明指针，指向内部的宿主进程对象；停止之后仍要能回答退出码，所以额外留一份。
    void *m_process = nullptr;
    int m_lastExitCode = -1;
};

// 分离后无线连接开关的一次性设置。宿主不必常驻：命令发出后由随后的
// DetachSupportChanged 事件或下一次快照确认。
[[nodiscard]] bool SetDetachSupport(const std::wstring &hostExePath, bool enable) noexcept;

} // namespace Gaokun::Keyboard
