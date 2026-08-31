#include "CleanupPlan.h"

#include <iostream>
#include <stdexcept>

namespace {

using Cleanup::KnownFolders;
using Cleanup::Options;
using Cleanup::TargetKind;

void Require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

// 测试用的假路径，与本机是否真有这些目录无关。
KnownFolders Folders() {
    return {L"C:\\Program Files", L"C:\\ProgramData",
            L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs"};
}

bool PlanContains(const std::wstring &path, TargetKind kind) {
    for (const auto &target : Cleanup::BuildRemovalPlan(Folders())) {
        if (target.path == path && target.kind == kind) return true;
    }
    return false;
}

Options Parse(std::initializer_list<const wchar_t *> arguments) {
    std::vector<const wchar_t *> argv{L"OpenEGoHubCleanup.exe"};
    argv.insert(argv.end(), arguments);
    return Cleanup::ParseCommandLine(static_cast<int>(argv.size()), argv.data());
}

void TestCommandLine() {
    Require(!Parse({}).dryRun, "a bare invocation really does the work");
    Require(Parse({}).valid, "and is valid");

    Require(Parse({L"--dry-run"}).dryRun, "--dry-run is recognised");
    Require(Parse({L"--log", L"C:\\x.log"}).logPath == L"C:\\x.log", "--log takes the next word");
    Require(Parse({L"--log", L"C:\\x.log", L"--dry-run"}).dryRun,
            "the flag after --log's value is still parsed");

    Require(!Parse({L"--log"}).valid, "--log without a path is rejected");
    Require(!Parse({L"--force"}).valid, "an unknown flag is rejected rather than ignored");
}

void TestRemovalPlan() {
    Require(PlanContains(L"C:\\Program Files\\OpenEGoHub", TargetKind::Directory),
            "the install directory is in the plan");
    Require(PlanContains(L"C:\\Program Files\\OpenEGoHub.backup", TargetKind::Directory),
            "deploy.ps1's backup is in the plan");
    Require(PlanContains(L"C:\\Program Files\\EGoTouchRev", TargetKind::Directory),
            "the pre-rename install directory is in the plan");
    Require(PlanContains(L"C:\\ProgramData\\EGoTouchRev", TargetKind::Directory),
            "the pre-rename data directory is in the plan");
    Require(PlanContains(
                L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\EGoTouch",
                TargetKind::Directory),
            "the old start menu folder is in the plan");
    Require(PlanContains(L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\OpenEGo "
                         L"Hub (Test Version).lnk",
                         TargetKind::File),
            "the test-package shortcut is in the plan as a file, not a directory");

    // 配置与日志留下，是这个工具唯一的保留承诺。
    Require(!PlanContains(L"C:\\ProgramData\\OpenEGoHub", TargetKind::Directory),
            "the settings directory is never in the removal plan");

    for (const auto &target : Cleanup::BuildRemovalPlan(Folders())) {
        Require(!target.reason.empty(), "every entry says why it is being removed");
        Require(!Cleanup::IsProtectedPath(target.path, L"C:\\ProgramData"),
                "no entry in the removal plan is also protected");
    }
}

void TestProtectedPaths() {
    Require(Cleanup::IsProtectedPath(L"C:\\ProgramData\\OpenEGoHub", L"C:\\ProgramData"),
            "the settings root is protected");
    Require(Cleanup::IsProtectedPath(L"C:\\ProgramData\\OpenEGoHub\\logs\\service.log",
                                     L"C:\\ProgramData"),
            "and so is everything under it");
    Require(Cleanup::IsProtectedPath(L"c:\\programdata\\openegohub\\", L"C:\\ProgramData"),
            "case and a trailing backslash do not get past the check");

    // 相邻的名字不能被前缀匹配吃掉——这两个正是要删的。
    Require(!Cleanup::IsProtectedPath(L"C:\\ProgramData\\OpenEGoHub.backup", L"C:\\ProgramData"),
            "a sibling whose name merely starts the same is not protected");
    Require(!Cleanup::IsProtectedPath(L"C:\\ProgramData\\EGoTouchRev", L"C:\\ProgramData"),
            "the pre-rename data directory is not protected");
    Require(!Cleanup::IsProtectedPath(L"C:\\Program Files\\OpenEGoHub", L"C:\\ProgramData"),
            "the install directory of the same name is not protected");
}

void TestProtectedServices() {
    Require(Cleanup::IsProtectedService(L"HuaweiThpService"), "the vendor touch service");
    Require(Cleanup::IsProtectedService(L"huaweithpservice"), "matched case-insensitively");
    Require(Cleanup::IsProtectedService(L"HwDeviceService"), "Hw-prefixed vendor services");
    Require(Cleanup::IsProtectedService(L"HiSuiteService"), "Hi-prefixed vendor services");
    Require(Cleanup::IsProtectedService(L"THPLog"), "the shared event source");
    Require(Cleanup::IsProtectedService(L"THPEvent"), "the other shared event source");

    for (const auto &service : Cleanup::ServiceRemovalPlan()) {
        Require(!Cleanup::IsProtectedService(service),
                "nothing we intend to delete is on the preserve list");
    }
}

void TestProcessLists() {
    const auto users = Cleanup::UserFacingProcesses();
    Require(users.size() == 5, "five user-facing processes");
    for (const auto &name : users) {
        Require(name.size() > 4 && name.compare(name.size() - 4, 4, L".exe") == 0,
                "Toolhelp32 compares image names, so every entry carries .exe");
    }
    for (const auto &name : Cleanup::HalHostProcesses()) {
        Require(name.size() > 4 && name.compare(name.size() - 4, 4, L".exe") == 0,
                "the hal hosts too");
    }
}

}  // namespace

int main() {
    try {
        TestCommandLine();
        TestRemovalPlan();
        TestProtectedPaths();
        TestProtectedServices();
        TestProcessLists();
        std::cout << "[TEST] Cleanup plan tests passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "[TEST] " << error.what() << "\n";
        return 1;
    }
}
