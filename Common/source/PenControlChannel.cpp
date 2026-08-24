#include "PenControlChannel.h"

#include <aclapi.h>
#include <sddl.h>

#include <atomic>
#include <chrono>
#include <cstring>

namespace PenControl {
namespace {

// SYSTEM and Administrators get full control; Interactive Users get read *and* write.
// 这是与 PenStatusChannel 的唯一实质差异：那条通道是服务广播、谁都只读，这条是托盘提交、
// 服务消费，交互用户必须能写。section 的 generic mapping 下 GW 是 SECTION_MAP_WRITE、
// GR 是 SECTION_MAP_READ，两者合起来才够 MapViewOfFile 拿到一个可读写的视图。
constexpr wchar_t kChannelSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GWGR;;;IU)";

// 提交事件：交互用户要能置位（GW → EVENT_MODIFY_STATE），也要能等待（GX →
// STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE）。等待权限不是给托盘的，是给未提权环境下的
// Host——它此时也只是个 IU，没有 GX 就等不了自己建的事件。
//
// GX 与 GW 都是权限记号，SY / BA / IU 是受托者记号，两类不能混写。PenStatusChannel.cpp
// 里记着这个教训：SDDL 里写错一个记号，整个描述符解析失败，事件静默地没被创建，而通道
// 其余部分照常工作，直到某个依赖信号的功能失效才暴露。
constexpr wchar_t kEventSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GWGX;;;IU)";

struct ScopedSd {
    PSECURITY_DESCRIPTOR value = nullptr;
    ~ScopedSd() { if (value) LocalFree(value); }
};

bool MakeAttributes(const wchar_t* sddl, SECURITY_ATTRIBUTES& sa, ScopedSd& sd) {
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &sd.value, nullptr)) {
        return false;
    }
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd.value;
    sa.bInheritHandle = FALSE;
    return true;
}

// 当前进程的用户 SID 与给定 SID 是否相同。
bool IsCurrentUser(PSID sid) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size) &&
                    EqualSid(sid, reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid);
    CloseHandle(token);
    return ok;
}

// 既有命名对象是不是「本可以由我们自己建出来的那一个」——属主为 SYSTEM、Administrators
// 或当前用户。与 PenStatusChannel.cpp 里那份相同，两个通道之间没有共享的私有头。
bool IsAdoptableOwner(HANDLE object) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(object, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION,
                        &owner, nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS) {
        return false;
    }

    bool adoptable = false;
    if (owner) {
        for (const WELL_KNOWN_SID_TYPE wellKnown :
             {WinLocalSystemSid, WinBuiltinAdministratorsSid}) {
            BYTE expected[SECURITY_MAX_SID_SIZE];
            DWORD size = sizeof(expected);
            if (CreateWellKnownSid(wellKnown, nullptr, expected, &size) &&
                EqualSid(owner, expected)) {
                adoptable = true;
                break;
            }
        }
        if (!adoptable) adoptable = IsCurrentUser(owner);
    }
    if (sd) LocalFree(sd);
    return adoptable;
}

// 建一个命名对象，并对「名字已被占用」表态。CreateFileMapping / CreateEvent 此时仍返回
// 有效句柄，只看返回值永远发现不了；无条件附着等于把通道交给先建者——ACL 来自它那份描述
// 符而不是上面这两条，服务会在一个攻击者可读可写的映射上收命令。
//
// 按属主认领：SYSTEM / Administrators / 当前用户建的对象等价于我们自己建的，认领；其余
// 放弃，换命名空间。一律拒绝是不行的——服务重启时托盘还握着映射句柄，对象不会消失，重启
// 后的服务会在 Global 上撞到已存在而退到 Local，托盘却还连在 Global 上，通道静默失效。
//
// 「同时有两个 Host」是另一回事，靠对象是否存在推断不出来（重启的 Host 和并存的第二个
// Host 看到的现象完全相同）。PenStatusChannel 那边用一个命名信号量把它变成可执行检查，
// 这里没有：那需要在 Host 里多存一个句柄，而这个类的成员由 PenControlChannel.h 定死。
// 生产环境里 Host 的唯一性由 SCM 的单实例保证。
template <typename CreateFn>
HANDLE CreateOwned(CreateFn create) {
    HANDLE h = create();
    if (!h) return nullptr;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return h;
    if (IsAdoptableOwner(h)) return h;
    CloseHandle(h);
    return nullptr;
}

// 单 Host 闸门。名字不在头文件里：只有 Host 碰它，托盘从不打开。
constexpr wchar_t kHostLockName[]      = L"Global\\OpenEGoHubPenControlHostLock";
constexpr wchar_t kHostLockNameLocal[] = L"Local\\OpenEGoHubPenControlHostLock";

// 「同一命名空间只能有一个活着的 Host」判不了对象是否存在——重启的 Host 和并存的第二个
// Host 看到的现象完全相同（托盘握着句柄，映射不会随旧 Host 消失）。
//
// 计数为 1 的信号量分得开：它由 Host 独占持有，进程退出时句柄全关、对象随之销毁，重启的
// Host 建到的是全新的计数 1。用信号量而不是互斥量，是因为互斥量按线程记归属，同一线程里
// 开两个 Host 会递归获取成功，而那正是测试要抓的情形。
//
// 与 PenStatusChannel 的 WriterLock 是同一套写法，注释见那边。
HANDLE AcquireHostLock(SECURITY_ATTRIBUTES& sa, const wchar_t* name) {
    HANDLE h = CreateSemaphoreW(&sa, 1, 1, name);
    if (!h) return nullptr;
    if (GetLastError() == ERROR_ALREADY_EXISTS && !IsAdoptableOwner(h)) {
        CloseHandle(h);
        return nullptr;
    }
    if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0) {
        CloseHandle(h);   // 另一个 Host 活着
        return nullptr;
    }
    return h;
}

// 归还闸门。先补回计数再关句柄：进程还活着的时候关句柄不会自动补回计数，同一进程里重开
// Host 会永远拿不到闸门。
void ReleaseGate(HANDLE& gate) {
    if (!gate) return;
    ReleaseSemaphore(gate, 1, nullptr);
    CloseHandle(gate);
    gate = nullptr;
}

// Local 命名空间下的认领。CreateFileMapping / CreateEvent 打开既有对象时请求的是 ALL
// ACCESS，而上面那两条 ACL 只给 IU 读写、不给完全控制——未提权的 Host 因此连自己上一次
// 建的对象都 Create 不回来（ERROR_ACCESS_DENIED）。按实际需要的权限 Open 就没这个问题。
//
// 只用于 Local。Global 下 CreateFileMapping 的 ERROR_ACCESS_DENIED 通常意味着缺
// SeCreateGlobalPrivilege 而不是对象已存在，此时改用 Open 会让一个未提权进程接管运行中
// 服务的控制通道——正是这个通道最不该发生的事。
//
// READ_CONTROL 是给属主校验用的；IU 的 GR / GX 都含 STANDARD_RIGHTS_*，本来就带着它。
HANDLE OpenAdoptable(const wchar_t* name, bool isEvent) {
    const DWORD access = isEvent ? (EVENT_MODIFY_STATE | SYNCHRONIZE | READ_CONTROL)
                                 : (FILE_MAP_READ | FILE_MAP_WRITE | READ_CONTROL);
    HANDLE h = isEvent ? OpenEventW(access, FALSE, name)
                       : OpenFileMappingW(access, FALSE, name);
    if (!h) return nullptr;
    if (IsAdoptableOwner(h)) return h;
    CloseHandle(h);
    return nullptr;
}

uint64_t NowUnixMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// seqlock 读。与 PenStatus::Reader::Read 同构，注释见那边：奇数序号表示写入进行中，
// fence 保证载荷拷贝不会被重排到两次序号读之外。
bool ReadPayload(const Snapshot* view, Payload& out) {
    for (int attempt = 0; attempt < 64; ++attempt) {
        const uint32_t before = view->sequence.load(std::memory_order_acquire);
        if (before & 1u) continue;

        Payload copy{};
        std::memcpy(&copy, &view->payload, sizeof(Payload));

        std::atomic_thread_fence(std::memory_order_acquire);
        if (view->sequence.load(std::memory_order_relaxed) == before) {
            out = copy;
            return true;
        }
    }
    return false;
}

} // namespace

// ── Host ─────────────────────────────────────────────────────────────────────

Host::~Host() { Close(); }

bool Host::Open(Scope scope) {
    if (m_view) return true;

    SECURITY_ATTRIBUTES mapSa{};
    ScopedSd mapSd;
    if (!MakeAttributes(kChannelSddl, mapSa, mapSd)) return false;

    // 闸门与提交事件共用这份描述符。它建不出来时不降级继续：闸门没了就没法保证单 Host，
    // 而单 Host 是「一份提交只被消费一次」的前提。
    SECURITY_ATTRIBUTES evtSa{};
    ScopedSd evtSd;
    if (!MakeAttributes(kEventSddl, evtSa, evtSd)) return false;

    // 先占闸门再建映射：反过来的话，映射已经建好而闸门被别人持有时要回滚，而回滚会销毁
    // 一份托盘可能已经映射上的对象。
    auto tryNamespace = [&](bool local) {
        m_hostLock = AcquireHostLock(evtSa, local ? kHostLockNameLocal : kHostLockName);
        if (!m_hostLock) return false;

        const wchar_t* const name = local ? kSharedMemoryNameLocal : kSharedMemoryName;
        m_mapping = CreateOwned([&] {
            return CreateFileMappingW(INVALID_HANDLE_VALUE, &mapSa, PAGE_READWRITE,
                                      0, sizeof(Snapshot), name);
        });
        if (!m_mapping && local) m_mapping = OpenAdoptable(name, /*isEvent=*/false);
        if (!m_mapping) {
            ReleaseGate(m_hostLock);
            return false;
        }
        m_usingLocalNamespace = local;
        return true;
    };

    // Global\ 建不起来有两种原因，处理相同：没有 SeCreateGlobalPrivilege（测试、手工运
    // 行），或者命名空间被别的账户占着。后者下退到 Local 是有意的——宁可让托盘找不到通道，
    // 也不接管一个别人建的对象。
    if (scope == Scope::Local) {
        if (!tryNamespace(true)) return false;
    } else if (!tryNamespace(false)) {
        if (scope == Scope::Global || !tryNamespace(true)) return false;
    }

    m_view = static_cast<Snapshot*>(
        MapViewOfFile(m_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Snapshot)));
    if (!m_view) {
        Close();
        return false;
    }

    // 自动重置：等待者只有服务一个，一次提交唤醒它一次即可。事件被别的账户占着时不附着，
    // Host 退化为纯轮询——PollCommand 照样能看到新 revision，只是延迟一个轮询周期。
    const wchar_t* const eventName =
        m_usingLocalNamespace ? kSubmitEventNameLocal : kSubmitEventName;
    m_submitEvent = CreateOwned([&] {
        return CreateEventW(&evtSa, FALSE, FALSE, eventName);
    });
    if (!m_submitEvent && m_usingLocalNamespace) {
        m_submitEvent = OpenAdoptable(eventName, /*isEvent=*/true);
    }

    m_view->abiVersion = kAbiVersion;

    // 把映射里已有的 revision 记为基线。服务重启后不该把上一次会话里早已生效的提交再应用
    // 一遍——那会覆盖掉这期间从配置文件或别处改过的模式。新建的映射是零填充，基线为 0。
    Payload existing{};
    m_lastRevision = ReadPayload(m_view, existing) ? existing.revision : 0;
    return true;
}

void Host::Close() {
    if (m_view) {
        UnmapViewOfFile(m_view);
        m_view = nullptr;
    }
    if (m_mapping) {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
    }
    if (m_submitEvent) {
        CloseHandle(m_submitEvent);
        m_submitEvent = nullptr;
    }
    ReleaseGate(m_hostLock);
    m_lastRevision = 0;
    m_usingLocalNamespace = false;
}

bool Host::PollCommand(Command& out) {
    if (!m_view) return false;
    if (m_view->abiVersion != kAbiVersion) return false;

    Payload copy{};
    if (!ReadPayload(m_view, copy)) return false;
    if (copy.revision == m_lastRevision) return false;

    m_lastRevision = copy.revision;

    Command command{};
    command.hasPenButtonMode = (copy.flags & kFlagHasPenButtonMode) != 0;
    command.penButtonMode = copy.penButtonMode;
    command.hasProviderLease = (copy.flags & kFlagHasProviderLease) != 0;
    command.providerLease = static_cast<ProviderLeaseCommand>(copy.providerLease);
    command.hasInputSuppression = (copy.flags & kFlagHasInputSuppression) != 0;
    command.inputSuppression =
        static_cast<InputSuppressionCommand>(copy.inputSuppression);
    command.hasKbdDetachSupport = (copy.flags & kFlagHasKbdDetachSupport) != 0;
    command.kbdDetachSupport =
        static_cast<KbdDetachSupportCommand>(copy.kbdDetachSupport);
    command.revision = copy.revision;
    command.submittedAtUnixMs = copy.submittedAtUnixMs;
    out = command;
    return true;
}

bool Host::WaitForSubmit(DWORD timeoutMs) const {
    if (!m_submitEvent) {
        // 等满超时。立刻返回会让循环等待的调用方空转，而这个接口就是给循环用的。
        Sleep(timeoutMs);
        return false;
    }
    return WaitForSingleObject(m_submitEvent, timeoutMs) == WAIT_OBJECT_0;
}

// ── Client ───────────────────────────────────────────────────────────────────

Client::~Client() { Close(); }

bool Client::Open(Scope scope) {
    // 只打开，不创建。服务不在时这里失败，正是想要的结果——托盘建出来的对象服务也用不上，
    // 反而会挡住服务之后的独占创建。
    constexpr DWORD kAccess = FILE_MAP_READ | FILE_MAP_WRITE;

    if (m_view) return true;

    bool local = (scope == Scope::Local);
    if (local) {
        m_mapping = OpenFileMappingW(kAccess, FALSE, kSharedMemoryNameLocal);
    } else {
        m_mapping = OpenFileMappingW(kAccess, FALSE, kSharedMemoryName);
        if (!m_mapping && scope == Scope::Auto) {
            m_mapping = OpenFileMappingW(kAccess, FALSE, kSharedMemoryNameLocal);
            local = true;
        }
    }
    if (!m_mapping) return false;

    m_view = static_cast<Snapshot*>(
        MapViewOfFile(m_mapping, kAccess, 0, 0, sizeof(Snapshot)));
    if (!m_view) {
        Close();
        return false;
    }

    // abiVersion 是普通读，与 Host 在 Open 时的普通写构成形式上的数据竞争。它只被写一次
    // 且值恒定，读到的不是 kAbiVersion 就是零，两种都会被下面这一步拒掉。
    if (m_view->abiVersion != kAbiVersion) {
        Close();
        return false;
    }

    m_submitEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE,
                               local ? kSubmitEventNameLocal : kSubmitEventName);
    return true;
}

void Client::Close() {
    if (m_view) {
        UnmapViewOfFile(m_view);
        m_view = nullptr;
    }
    if (m_mapping) {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
    }
    if (m_submitEvent) {
        CloseHandle(m_submitEvent);
        m_submitEvent = nullptr;
    }
}

bool Client::SubmitPenButtonMode(uint8_t mode) {
    if (!m_view) return false;

    Payload staged{};
    staged.flags = kFlagHasPenButtonMode;
    staged.penButtonMode = mode;
    return Submit(staged);
}

bool Client::SubmitProviderLease(ProviderLeaseCommand command) {
    if (!m_view || command == ProviderLeaseCommand::None) return false;

    Payload staged{};
    staged.flags = kFlagHasProviderLease;
    staged.providerLease = static_cast<uint8_t>(command);
    return Submit(staged);
}

bool Client::SubmitInputSuppression(InputSuppressionCommand command) {
    if (!m_view || command == InputSuppressionCommand::None) return false;

    Payload staged{};
    staged.flags = kFlagHasInputSuppression;
    staged.inputSuppression = static_cast<uint8_t>(command);
    return Submit(staged);
}

bool Client::SubmitKbdDetachSupport(KbdDetachSupportCommand command) {
    if (!m_view || command == KbdDetachSupportCommand::None) return false;

    Payload staged{};
    staged.flags = kFlagHasKbdDetachSupport;
    staged.kbdDetachSupport = static_cast<uint8_t>(command);
    return Submit(staged);
}

bool Client::Submit(const Payload& requested) {
    if (!m_view) return false;

    Payload staged = requested;
    // 接着映射里的当前值递增，而不是从自己的计数器起步：托盘重启后如果从 0 重来，Host 记
    // 下的基线会大于新值，第一批提交就都被当成旧的丢掉。
    staged.revision = m_view->payload.revision + 1u;
    staged.submittedAtUnixMs = NowUnixMs();

    const uint32_t start = m_view->sequence.load(std::memory_order_relaxed) + 1u;
    m_view->sequence.store(start, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);

    // 单次不透明拷贝：编译器拆不开也挪不到两侧的序号存储之外。
    std::memcpy(&m_view->payload, &staged, sizeof(Payload));

    m_view->sequence.store(start + 1u, std::memory_order_release);

    if (m_submitEvent) {
        SetEvent(m_submitEvent);
    }
    return true;
}

} // namespace PenControl
