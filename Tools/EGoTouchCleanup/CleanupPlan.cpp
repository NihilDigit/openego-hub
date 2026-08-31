#include "CleanupPlan.h"

#include <algorithm>

namespace Cleanup {
namespace {

std::wstring ToLower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return (c >= L'A' && c <= L'Z') ? wchar_t(c - L'A' + L'a') : c; });
    return text;
}

bool StartsWith(const std::wstring &text, const std::wstring &prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// 末尾反斜杠去掉，否则 "C:\X\" 与 "C:\X" 比不相等。
std::wstring Normalize(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    std::replace(path.begin(), path.end(), L'/', L'\\');
    return ToLower(std::move(path));
}

std::wstring Join(const std::wstring &base, const std::wstring &leaf) {
    if (base.empty()) return leaf;
    std::wstring result = base;
    if (result.back() != L'\\') result.push_back(L'\\');
    result += leaf;
    return result;
}

}  // namespace

Options ParseCommandLine(int argc, const wchar_t *const *argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--dry-run") {
            options.dryRun = true;
        } else if (argument == L"--log") {
            if (i + 1 >= argc) {
                options.valid = false;
                options.error = L"--log needs a path";
                return options;
            }
            options.logPath = argv[++i];
        } else {
            options.valid = false;
            options.error = L"unknown argument: " + argument;
            return options;
        }
    }
    return options;
}

std::vector<PathTarget> BuildRemovalPlan(const KnownFolders &folders) {
    std::vector<PathTarget> plan;

    plan.push_back({TargetKind::Directory, Join(folders.programFiles, L"OpenEGoHub"),
                    L"install directory; MSI has already removed its own files"});
    plan.push_back({TargetKind::Directory, Join(folders.programFiles, L"OpenEGoHub.backup"),
                    L"backup left behind by deploy.ps1"});
    plan.push_back({TargetKind::Directory, Join(folders.programFiles, L"EGoTouchRev"),
                    L"install directory from before the rename"});
    plan.push_back({TargetKind::Directory, Join(folders.programData, L"EGoTouchRev"),
                    L"application data from before the rename"});
    plan.push_back({TargetKind::Directory, Join(folders.commonPrograms, L"EGoTouch"),
                    L"start menu folder from before the rename"});
    plan.push_back({TargetKind::File,
                    Join(folders.commonPrograms, L"OpenEGo Hub (Test Version).lnk"),
                    L"start menu shortcut left by the v0.1.x test packages"});

    return plan;
}

std::vector<std::wstring> UserFacingProcesses() {
    return {L"OpenEGoHubTray.exe", L"OpenEGoHubSettings.exe", L"OpenEGoHubApp.exe",
            L"EGoTouchApp.exe", L"BtMcuTestTool.exe"};
}

std::vector<std::wstring> HalHostProcesses() {
    return {L"GaokunThpHost.exe", L"GaokunPenHost.exe", L"GaokunKeyboardHost.exe",
            L"GaokunPower.exe", L"GaokunDisplay.exe"};
}

std::vector<std::wstring> ServiceRemovalPlan() {
    return {L"EGoTouchService", L"OpenEGoHubServiceDebug", L"OpenEGoHubService"};
}

bool IsProtectedService(const std::wstring &name) {
    const std::wstring lowered = ToLower(name);
    // 前缀而不是精确匹配：原厂和 PC Manager 装出来的服务不止一个，名字也随版本变，
    // 而我们要删的三个服务没有一个用这些前缀，误伤的方向比漏判的方向危险得多。
    for (const wchar_t *prefix : {L"huawei", L"hw", L"hi"}) {
        if (StartsWith(lowered, prefix)) return true;
    }
    // 事件源与原厂共用，删掉之后原厂那一侧写事件日志会失败。
    for (const wchar_t *exact : {L"thplog", L"thpevent"}) {
        if (lowered == exact) return true;
    }
    return false;
}

bool IsProtectedPath(const std::wstring &path, const std::wstring &programData) {
    const std::wstring target = Normalize(path);
    const std::wstring preserved = Normalize(Join(programData, L"OpenEGoHub"));
    if (preserved.empty()) return false;
    if (target == preserved) return true;
    return StartsWith(target, preserved + L"\\");
}

std::vector<std::wstring> PreservedItems(const KnownFolders &folders) {
    return {Join(folders.programData, L"OpenEGoHub") + L" (settings and logs)",
            L"HKLM\\SOFTWARE\\OpenEGoHub\\VendorServiceBackup",
            L"service HuaweiThpService and every other Huawei service",
            L"event sources THPLog and THPEvent"};
}

}  // namespace Cleanup
