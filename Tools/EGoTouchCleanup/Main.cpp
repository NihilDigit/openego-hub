#include <windows.h>

#include "CleanupActions.h"
#include "CleanupLog.h"
#include "CleanupPlan.h"

// OpenEGoHubCleanup.exe —— 安装前的残留清理。
//
// 由 MSI 的 Binary 表携带，在 RemoveExistingProducts 之后、InstallFiles 之前以 deferred
// custom action 运行；也能单独跑，`--dry-run` 只打印将做什么。
//
// MSI 那侧配的是 Return="ignore"，这里的退出码恒为 0 是同一个决定的另一半：清理是尽力
// 而为，任何一项失败都不该让整个安装回滚——回滚之后用户既没装上新版，残留也还在。做不成
// 的事情留在日志里。
int wmain(int argc, wchar_t **argv) {
    const Cleanup::Options options = Cleanup::ParseCommandLine(argc, argv);
    Cleanup::OpenLog(options.logPath);

    if (!options.valid) {
        Cleanup::Log(L"bad command line: %s", options.error.c_str());
        Cleanup::Log(L"usage: OpenEGoHubCleanup.exe [--dry-run] [--log <path>]");
        Cleanup::CloseLog();
        return 0;
    }

    Cleanup::Log(L"=== OpenEGoHub cleanup %s ===", options.dryRun ? L"(dry run)" : L"");
    Cleanup::Log(L"log: %s", Cleanup::LogPath().c_str());

    const Cleanup::KnownFolders folders = Cleanup::ResolveKnownFolders();
    Cleanup::Log(L"ProgramFiles: %s", folders.programFiles.c_str());
    Cleanup::Log(L"ProgramData: %s", folders.programData.c_str());
    Cleanup::Log(L"CommonPrograms: %s", folders.commonPrograms.c_str());

    Cleanup::Log(L"[keep] the following are never touched:");
    for (const auto &item : Cleanup::PreservedItems(folders)) {
        Cleanup::Log(L"  %s", item.c_str());
    }

    Cleanup::Log(L"[1/4] user-facing processes");
    Cleanup::KillProcesses(Cleanup::UserFacingProcesses(), options.dryRun);

    Cleanup::Log(L"[2/4] orphaned services");
    for (const auto &service : Cleanup::ServiceRemovalPlan()) {
        Cleanup::RemoveService(service, options.dryRun);
    }

    Cleanup::Log(L"[3/4] vendor handover and leftover hal hosts");
    Cleanup::WaitForVendorTouchService(15000, options.dryRun);
    // 宿主靠 --parent 机制跟着服务退出，但 GaokunThpHost 还要等厂商 DLL 收尾，给 5 秒。
    Cleanup::KillProcessesAfterGrace(Cleanup::HalHostProcesses(), 5000, options.dryRun);

    Cleanup::Log(L"[4/4] files and directories");
    for (const auto &target : Cleanup::BuildRemovalPlan(folders)) {
        Cleanup::RemovePath(target, folders.programData, options.dryRun);
    }

    Cleanup::Log(L"=== cleanup finished ===");
    Cleanup::CloseLog();
    return 0;
}
