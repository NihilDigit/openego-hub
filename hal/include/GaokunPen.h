#pragma once

#include <chrono>
#include <cstdint>
#include <new>
#include <string>

// 笔的状态与事件通道。宿主进程 GaokunPenHost.exe 是 ARM64EC（它加载 x64 的 PenService.dll）,
// 本头文件与其读者侧实现是原生 ARM64，调用方不必改变自身架构。
//
// 两类数据分两条通道，因为它们的语义不同：
//   状态快照走共享内存，可重复读，读者随时取到当前值即可，漏读一次没有影响；
//   离散事件走命名管道，每一次都是一条边沿，漏掉就没有了。
// 把边沿压进状态位会丢失「又发生了一次」，把快照塞进管道则会在读者慢时堆积。
namespace Gaokun::Pen {

// ---- 状态快照 ----

inline constexpr int kVersionCapacity = 32;
inline constexpr int kSerialCapacity = 40;
// 产品名是 UTF-8，「HUAWEI M-Pencil（第一代）」这类带全角括号的串一个字符占三字节，
// 容量要按最长的那个留够：截断会切断多字节序列，渲染侧解码直接失败。
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
    HasKeySupport = 1u << 9,
    HasKeyFunc = 1u << 10,
};

// 跨进程共享的载荷。写者是 ARM64EC，读者是原生 ARM64，因此只用固定宽度类型并显式补齐，
// 不依赖任何一侧编译器的填充规则。
struct Snapshot {
    uint32_t flags = 0;
    uint32_t moduleId = 0;   // MCU 报告的笔模块 ID，见 docs/pen.md 的型号表
    uint8_t battery = 0;     // 百分比
    uint8_t keySupport = 0;  // 按键能力掩码
    uint8_t keyFunc = 0;     // 当前按键功能
    uint8_t _pad0 = 0;
    uint32_t _pad1 = 0;
    uint64_t updatedAtUnixMs = 0;
    char firmware[kVersionCapacity]{};
    char hardware[kVersionCapacity]{};
    char serial[kSerialCapacity]{};
    // 可直接显示的产品名，由 hal 按模组 ID 查表填好。上层不必知道模组 ID 的含义，
    // 也不必维护一份自己的型号表——那份表正是先前把 M-Pen 2 认成初代 M-Pencil 的地方。
    char modelName[kModelNameCapacity]{};
};
static_assert(sizeof(Snapshot) == 176, "Snapshot layout must stay fixed across both sides");

// ---- 离散事件 ----

enum class EventKind : uint32_t {
    None = 0,
    ConnectRequest,      ///< 笔请求连接，UI 需要弹确认
    ConnectResult,       ///< 连接结果
    KeyFuncChanged,      ///< 按键功能被改变
    CurrentFunc,         ///< 侧键触发，value 是功能编号
    BatteryReminder,     ///< 低电量提醒
    DeviationReminder,   ///< 笔尖偏移提醒
    CloseConnectWindow,  ///< 请求关闭连接窗口
    TransferPenMode,     ///< 笔模式切换
};

struct Event {
    uint32_t kind = 0;   // EventKind
    int32_t value = 0;   // 回调带回的整型实参，含义随 kind
    uint64_t atUnixMs = 0;
};
static_assert(sizeof(Event) == 16, "Event layout must stay fixed across both sides");

// ---- 下行命令 ----

enum class CommandKind : uint32_t {
    None = 0,
    SetCurrentFunc,
};

struct Command {
    uint32_t kind = 0;
    int32_t value = 0;
};
static_assert(sizeof(Command) == 8, "Command layout must stay fixed across both sides");

// ---- 读者侧 ----

// 状态快照的读取。宿主未运行时 Read 返回 false，调用方据此显示「未知」而不是零值。
class SnapshotReader {
public:
    SnapshotReader() noexcept = default;
    ~SnapshotReader() noexcept;

    SnapshotReader(const SnapshotReader &) = delete;
    SnapshotReader &operator=(const SnapshotReader &) = delete;

    [[nodiscard]] bool Open() noexcept;
    [[nodiscard]] bool Read(Snapshot &out) const noexcept;

private:
    // 指向内部的 seqlock 读者。用不透明指针是为了让通道实现留在 src 里，对外头文件
    // 不必暴露共享内存的线路格式。
    void *m_impl = nullptr;
};

// 事件流的读取。每次 Poll 取一条，没有则返回 false，不阻塞。
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

// 向常驻 ARM64EC 宿主发送厂商命令。当前只开放 PenCurrentFunc，避免让服务直接加载 x64 DLL。
class CommandWriter {
public:
    CommandWriter() noexcept = default;
    ~CommandWriter() noexcept;

    CommandWriter(const CommandWriter &) = delete;
    CommandWriter &operator=(const CommandWriter &) = delete;

    [[nodiscard]] bool Open() noexcept;
    [[nodiscard]] bool SetCurrentFunc(bool eraser) noexcept;

private:
    void *m_impl = nullptr;
};

// ---- 宿主生命周期 ----

enum class StartResult {
    Started = 0,
    AlreadyRunning,
    HostNotFound,
    LaunchFailed,
    ExitedImmediately,
};

// 与 Gaokun::Thp::HostController 同形：宿主等待本进程句柄，调用方崩溃时它自行收尾。
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

} // namespace Gaokun::Pen
