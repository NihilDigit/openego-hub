// 笔通道的读者侧与宿主控制器。这一侧不接触任何厂商 DLL，是原生 ARM64，调用方（界面层）
// 因此不必改变自身架构；需要 ARM64EC 的只有 GaokunPenHost.exe。
//
// seqlock 与进程管理的实现都在 shared/ 下，与键盘共用。这里只是把它们绑到笔的名字上,
// 并藏在对外头文件的不透明指针后面。

#include "GaokunPen.h"

#include "pen/PenChannelLayout.h"
#include "shared/HostProcess.h"

namespace Gaokun::Pen {

using namespace Gaokun::Pen::Wire;

namespace {

[[nodiscard]] StartResult Translate(Host::LaunchResult result) noexcept {
    switch (result) {
    case Host::LaunchResult::Started: return StartResult::Started;
    case Host::LaunchResult::AlreadyRunning: return StartResult::AlreadyRunning;
    case Host::LaunchResult::NotFound: return StartResult::HostNotFound;
    case Host::LaunchResult::ExitedImmediately: return StartResult::ExitedImmediately;
    default: return StartResult::LaunchFailed;
    }
}

} // namespace

SnapshotReader::~SnapshotReader() noexcept { delete static_cast<SnapshotReaderImpl *>(m_impl); }

bool SnapshotReader::Open() noexcept {
    if (!m_impl) m_impl = new (std::nothrow) SnapshotReaderImpl();
    if (!m_impl) return false;
    return static_cast<SnapshotReaderImpl *>(m_impl)->Open(kSnapshotName);
}

bool SnapshotReader::Read(Snapshot &out) const noexcept {
    if (!m_impl) return false;
    return static_cast<const SnapshotReaderImpl *>(m_impl)->Read(out);
}

EventReader::~EventReader() noexcept { delete static_cast<EventReaderImpl *>(m_impl); }

bool EventReader::Open() noexcept {
    if (!m_impl) m_impl = new (std::nothrow) EventReaderImpl();
    if (!m_impl) return false;
    return static_cast<EventReaderImpl *>(m_impl)->Open(kEventPipeName);
}

bool EventReader::Poll(Event &out) noexcept {
    if (!m_impl) return false;
    return static_cast<EventReaderImpl *>(m_impl)->Poll(out);
}

CommandWriter::~CommandWriter() noexcept { delete static_cast<CommandWriterImpl *>(m_impl); }

bool CommandWriter::Open() noexcept {
    if (!m_impl) m_impl = new (std::nothrow) CommandWriterImpl();
    if (!m_impl) return false;
    return static_cast<CommandWriterImpl *>(m_impl)->Open(kCommandPipeName);
}

bool CommandWriter::SetCurrentFunc(bool eraser) noexcept {
    if (!m_impl) return false;
    const Command command{static_cast<uint32_t>(CommandKind::SetCurrentFunc), eraser ? 1 : 0};
    return static_cast<CommandWriterImpl *>(m_impl)->Send(command);
}

HostController::~HostController() noexcept { delete static_cast<Host::Process *>(m_process); }

void HostController::CloseHandles() noexcept {
    delete static_cast<Host::Process *>(m_process);
    m_process = nullptr;
}

StartResult HostController::Start(const std::wstring &hostExePath,
                                  const std::wstring &extraArgs) noexcept {
    if (!m_process) m_process = new (std::nothrow) Host::Process();
    if (!m_process) return StartResult::LaunchFailed;
    return Translate(
        static_cast<Host::Process *>(m_process)->Start(hostExePath, kStopEventPrefix, extraArgs));
}

bool HostController::Stop(std::chrono::milliseconds timeout) noexcept {
    if (!m_process) return true;
    auto *process = static_cast<Host::Process *>(m_process);
    const bool clean = process->Stop(timeout);
    m_lastExitCode = process->ExitCode();
    return clean;
}

bool HostController::IsRunning() const noexcept {
    return m_process && static_cast<const Host::Process *>(m_process)->IsRunning();
}

int HostController::ExitCode() const noexcept {
    if (!m_process) return m_lastExitCode;
    const int code = static_cast<const Host::Process *>(m_process)->ExitCode();
    return code == -1 ? m_lastExitCode : code;
}

} // namespace Gaokun::Pen
