#pragma once
// PenControlChannel — 托盘到服务的小型命令通道，只运载「用户在托盘菜单里选了什么」。
//
// 方向与 PenStatusChannel 相反，创建者却相同：服务创建并持有映射——它有
// SeCreateGlobalPrivilege，也由它决定 ACL——托盘只是打开并写入。交互用户拿到映射的
// 写权限和提交事件的置位权限。让托盘自己创建走不通：未提权进程建不了 Global\ 对象，
// 而它建在会话命名空间里的对象，会话 0 的服务反过来找不到。
//
// 载荷只有枚举值加一个 revision 计数，没有任何会被解释执行的内容。同会话的任意进程
// 都写得进来，这是接受的风险：能改的只是双击手势映射到什么行为，且服务侧仍按枚举
// 映射校验，非法值不落地。
//
// seqlock 与 PenStatusChannel 同构，但假设是「单写者」——两个进程同时提交会互踩序号。
// 正常部署里写者只有托盘一个实例（它有单实例互斥），不为这个场景加锁。

#include <atomic>
#include <cstdint>
#include <type_traits>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace PenControl {

inline constexpr wchar_t kSharedMemoryName[]      = L"Global\\OpenEGoHubPenControl";
inline constexpr wchar_t kSubmitEventName[]       = L"Global\\OpenEGoHubPenControlSubmit";
inline constexpr wchar_t kSharedMemoryNameLocal[] = L"Local\\OpenEGoHubPenControl";
inline constexpr wchar_t kSubmitEventNameLocal[]  = L"Local\\OpenEGoHubPenControlSubmit";

// Snapshot 布局变化时递增。版本不符的一方把映射当作不可用，而不是错读字节。
inline constexpr uint32_t kAbiVersion = 3;

inline constexpr uint32_t kFlagHasPenButtonMode = 1u << 0;
inline constexpr uint32_t kFlagHasProviderLease = 1u << 1;
inline constexpr uint32_t kFlagHasInputSuppression = 1u << 2;
inline constexpr uint32_t kFlagHasKbdDetachSupport = 1u << 3;

enum class ProviderLeaseCommand : uint8_t {
    None = 0,
    AcquireOrRenew,
    Release,
};

enum class InputSuppressionCommand : uint8_t {
    None = 0,
    BeginOrRenew,
    End,
};

// 键盘「分离后无线连接」开关。用三态而不是 bool，是为了让 None 表示「这条提交与键盘无关」，
// 与 flags 位一起构成双重判据，误置一个字节不会静默改掉用户的键盘设置。
enum class KbdDetachSupportCommand : uint8_t {
    None = 0,
    Disable,
    Enable,
};

struct Payload {
    uint32_t flags = 0;
    // 写者每次提交递增。Host 只在看到新值时应用一次，重启后的服务把当前值当作已消费,
    // 所以一份陈旧的提交不会在每次服务启动时重放。
    uint32_t revision = 0;
    uint8_t  penButtonMode = 0;   // PenButtonMode 的数值，valid with kFlagHasPenButtonMode
    uint8_t  providerLease = 0;   // ProviderLeaseCommand，valid with kFlagHasProviderLease
    uint8_t  inputSuppression = 0; // InputSuppressionCommand
    // 占用原先的填充字节，Snapshot 布局因此不变，kAbiVersion 无需递增：旧 Host 看不懂新
    // 标志位，会把这条提交当作一条没有任何已知字段的空提交忽略掉，而不是错读。
    uint8_t  kbdDetachSupport = 0; // KbdDetachSupportCommand
    uint64_t submittedAtUnixMs = 0;
};
static_assert(std::is_trivially_copyable_v<Payload>,
              "Payload must be memcpy-able for the seqlock to be correct");

struct Snapshot {
    uint32_t abiVersion = kAbiVersion;
    // 奇数表示写入进行中。语义与 PenStatusChannel::Snapshot::sequence 相同，那边的
    // 注释解释了为什么必须是 std::atomic 而不是 volatile。
    std::atomic<uint32_t> sequence{0};
    Payload payload{};
};
static_assert(sizeof(Snapshot) == 32, "PenControl::Snapshot layout must stay fixed");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "PenControl::Snapshot::sequence must be lock-free to work across processes");

enum class Scope {
    Auto,
    Global,
    Local,
};

// 交给调用方的普通值类型，不引用共享内存。
struct Command {
    bool     hasPenButtonMode = false;
    uint8_t  penButtonMode = 0;
    bool     hasProviderLease = false;
    ProviderLeaseCommand providerLease = ProviderLeaseCommand::None;
    bool     hasInputSuppression = false;
    InputSuppressionCommand inputSuppression = InputSuppressionCommand::None;
    bool     hasKbdDetachSupport = false;
    KbdDetachSupportCommand kbdDetachSupport = KbdDetachSupportCommand::None;
    uint32_t revision = 0;
    uint64_t submittedAtUnixMs = 0;
};

// 服务侧：创建映射与事件，读取提交。ACL 给 SYSTEM/管理员完全控制，交互用户读写。
class Host {
public:
    Host() = default;
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // 测试必须钉 Scope::Local：提权环境下 Auto 会建到或认领 Global，即运行中服务的
    // 那条通道，测试提交会真的送进服务。
    bool Open(Scope scope = Scope::Auto);
    void Close();
    [[nodiscard]] bool IsOpen() const noexcept { return m_view != nullptr; }

    // 读出当前提交。仅当 revision 相对上次 PollCommand 变化时返回 true。
    // Open() 时把已有的 revision 记为基线，服务重启不会重放旧提交。
    bool PollCommand(Command& out);

    // 等待写者置位提交事件。事件通道缺失时等满超时返回 false，不空转。
    bool WaitForSubmit(DWORD timeoutMs) const;

    // 事件是否建成。缺失时 Open() 仍成功，Host 退化为纯轮询，调用方应记日志。
    [[nodiscard]] bool HasEventChannel() const noexcept { return m_submitEvent != nullptr; }

    [[nodiscard]] Scope OpenedScope() const noexcept {
        return m_usingLocalNamespace ? Scope::Local : Scope::Global;
    }

private:
    HANDLE m_mapping = nullptr;
    HANDLE m_submitEvent = nullptr;
    // 写者闸门的对偶：同一命名空间下第二个 Host 必须被拒，语义与 PenStatusChannel
    // 的 WriterLock 相同，由 .cpp 内的命名信号量实现。
    HANDLE m_hostLock = nullptr;
    Snapshot* m_view = nullptr;
    uint32_t m_lastRevision = 0;
    bool m_usingLocalNamespace = false;
};

// 托盘侧：打开既有通道并提交。不创建对象，服务不在时 Open 失败。
class Client {
public:
    Client() = default;
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool Open(Scope scope = Scope::Auto);
    void Close();
    [[nodiscard]] bool IsOpen() const noexcept { return m_view != nullptr; }

    // seqlock 写入一份提交并置位事件。返回 false 表示通道未打开。
    bool SubmitPenButtonMode(uint8_t mode);
    bool SubmitProviderLease(ProviderLeaseCommand command);
    bool SubmitInputSuppression(InputSuppressionCommand command);
    bool SubmitKbdDetachSupport(KbdDetachSupportCommand command);

private:
    HANDLE m_mapping = nullptr;
    HANDLE m_submitEvent = nullptr;
    Snapshot* m_view = nullptr;

    bool Submit(const Payload& payload);
};

} // namespace PenControl
