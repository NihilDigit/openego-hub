// 键盘通道的读者侧与宿主控制器。原生 ARM64，不接触任何厂商 DLL；与 PenClient.cpp 同构,
// 底层实现共用 shared/ 下的 seqlock 与进程管理。

#include "GaokunKeyboard.h"

#include "keyboard/KbdChannelLayout.h"
#include "shared/HostProcess.h"

namespace Gaokun::Keyboard {

using namespace Gaokun::Keyboard::Wire;

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

bool CommandWriter::SetDetachSupport(bool enable) noexcept {
    if (!m_impl) return false;
    const Command command{static_cast<uint32_t>(CommandKind::SetDetachSupport), enable ? 1 : 0};
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

// 一次性设置。上层改一个开关不必为此常驻一个宿主进程，直接把命令行用法包起来。
bool SetDetachSupport(const std::wstring &hostExePath, bool enable) noexcept {
    std::wstring commandLine = L"\"" + hostExePath + L"\" --detach-support " +
                               (enable ? L"enable" : L"disable");

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(hostExePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) {
        return false;
    }
    CloseHandle(info.hThread);

    const bool finished = WaitForSingleObject(info.hProcess, 10000) == WAIT_OBJECT_0;
    DWORD code = 1;
    if (finished) (void)GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return finished && code == 0;
}

} // namespace Gaokun::Keyboard
