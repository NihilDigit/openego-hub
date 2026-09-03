#include "ServiceEntry.h"
#include "TestAssert.h"

namespace {

// FakeActions 与恢复厂商服务的路由无条件编译。EGOTOUCH_SERVICE_ENABLE_IPC 在当前构建里
// 没有定义，整份测试原先都在它里面，等于一条都不跑；而卸载要走的那条路恰恰在宏外。
struct FakeActions final : Service::IServiceEntryActions {
    bool installResult = true;
    bool uninstallResult = true;
    bool dispatcherResult = true;
    DWORD lastError = ERROR_SUCCESS;

    bool restoreVendorResult = true;

    int installCalls = 0;
    int uninstallCalls = 0;
    int restoreVendorCalls = 0;
    int initializeCalls = 0;
    int consoleCalls = 0;
    int dispatcherCalls = 0;

#if EGOTOUCH_SERVICE_ENABLE_IPC
    bool InstallService() override { ++installCalls; return installResult; }
    bool UninstallService() override { ++uninstallCalls; return uninstallResult; }
#endif
    bool RestoreVendorServices() override {
        ++restoreVendorCalls;
        return restoreVendorResult;
    }
    void InitializeServiceProcess() override { ++initializeCalls; }
#if EGOTOUCH_SERVICE_ENABLE_IPC
    // 与 InstallService 同理：接口里这一项也在宏内，宏关掉时基类根本没有它可重写。
    void RunConsole() override { ++consoleCalls; }
#endif
    bool StartScmDispatcher() override { ++dispatcherCalls; return dispatcherResult; }
    DWORD LastErrorCode() const override { return lastError; }
};

int Invoke(FakeActions& actions, std::initializer_list<const wchar_t*> args) {
    wchar_t* argv[4]{};
    int argc = 0;
    for (const wchar_t* arg : args) {
        argv[argc++] = const_cast<wchar_t*>(arg);
    }
    return Service::ServiceEntryMain(argc, argv, actions);
}

// 安装包用 ServiceControl 原生删除服务，走不到 --uninstall，所以恢复厂商服务必须能被单独
// 调用：这条路上不能顺手把服务注册也动了。
bool RestoreVendorRouteOnlyRestores() {
    FakeActions actions;
    REQUIRE_EQ(Invoke(actions, {L"OpenEGoHubService.exe", L"--restore-vendor-services"}), 0);
    REQUIRE_EQ(actions.restoreVendorCalls, 1);
    REQUIRE_EQ(actions.uninstallCalls, 0);
    REQUIRE_EQ(actions.installCalls, 0);
    REQUIRE_EQ(actions.initializeCalls, 0);
    REQUIRE_EQ(actions.dispatcherCalls, 0);
    return true;
}

bool RestoreVendorFailureReturnsFailure() {
    FakeActions actions;
    actions.restoreVendorResult = false;
    REQUIRE_EQ(Invoke(actions, {L"OpenEGoHubService.exe", L"--restore-vendor-services"}), 1);
    return true;
}

#if EGOTOUCH_SERVICE_ENABLE_IPC
bool InstallRouteDoesNotInitializeRuntime() {
    FakeActions actions;
    REQUIRE_EQ(Invoke(actions, {L"OpenEGoHubService.exe", L"--install"}), 0);
    REQUIRE_EQ(actions.installCalls, 1);
    REQUIRE_EQ(actions.initializeCalls, 0);
    REQUIRE_EQ(actions.dispatcherCalls, 0);
    REQUIRE_EQ(actions.consoleCalls, 0);
    return true;
}

bool UninstallFailureReturnsFailure() {
    FakeActions actions;
    actions.uninstallResult = false;
    REQUIRE_EQ(Invoke(actions, {L"OpenEGoHubService.exe", L"--uninstall"}), 1);
    REQUIRE_EQ(actions.uninstallCalls, 1);
    REQUIRE_EQ(actions.initializeCalls, 0);
    REQUIRE_EQ(actions.dispatcherCalls, 0);
    return true;
}

bool ConsoleRouteSkipsScmDispatcher() {
    FakeActions actions;
    REQUIRE_EQ(Invoke(actions, {L"OpenEGoHubService.exe", L"--console"}), 0);
    REQUIRE_EQ(actions.initializeCalls, 1);
#if EGOTOUCH_SERVICE_ENABLE_IPC
    REQUIRE_EQ(actions.consoleCalls, 1);
    REQUIRE_EQ(actions.dispatcherCalls, 0);
#else
    REQUIRE_EQ(actions.consoleCalls, 0);
    REQUIRE_EQ(actions.dispatcherCalls, 1);
#endif
    return true;
}

bool ScmSuccessDoesNotFallbackToConsole() {
    FakeActions actions;
    actions.dispatcherResult = true;
    REQUIRE_EQ(Invoke(actions, {L"OpenEGoHubService.exe"}), 0);
    REQUIRE_EQ(actions.initializeCalls, 1);
    REQUIRE_EQ(actions.dispatcherCalls, 1);
    REQUIRE_EQ(actions.consoleCalls, 0);
    return true;
}

#endif  // EGOTOUCH_SERVICE_ENABLE_IPC

bool ScmControllerConnectFailureFallsBackToConsole() {
    FakeActions actions;
    actions.dispatcherResult = false;
    actions.lastError = ERROR_FAILED_SERVICE_CONTROLLER_CONNECT;
    REQUIRE_EQ(Invoke(actions, {L"OpenEGoHubService.exe"}), 0);
    REQUIRE_EQ(actions.initializeCalls, 1);
    REQUIRE_EQ(actions.dispatcherCalls, 1);
#if EGOTOUCH_SERVICE_ENABLE_IPC
    REQUIRE_EQ(actions.consoleCalls, 1);
#else
    REQUIRE_EQ(actions.consoleCalls, 0);
#endif
    return true;
}

bool OtherScmFailureDoesNotFallbackToConsole() {
    FakeActions actions;
    actions.dispatcherResult = false;
    actions.lastError = ERROR_ACCESS_DENIED;
    REQUIRE_EQ(Invoke(actions, {L"OpenEGoHubService.exe"}), 0);
    REQUIRE_EQ(actions.initializeCalls, 1);
    REQUIRE_EQ(actions.dispatcherCalls, 1);
    REQUIRE_EQ(actions.consoleCalls, 0);
    return true;
}

} // namespace

int main() {
    int failures = 0;
    failures += RunTest(&RestoreVendorRouteOnlyRestores, "RestoreVendorRouteOnlyRestores");
    failures += RunTest(&RestoreVendorFailureReturnsFailure, "RestoreVendorFailureReturnsFailure");
    failures += RunTest(&ScmControllerConnectFailureFallsBackToConsole, "ScmControllerConnectFailureFallsBackToConsole");
    failures += RunTest(&OtherScmFailureDoesNotFallbackToConsole, "OtherScmFailureDoesNotFallbackToConsole");
#if EGOTOUCH_SERVICE_ENABLE_IPC
    failures += RunTest(&InstallRouteDoesNotInitializeRuntime, "InstallRouteDoesNotInitializeRuntime");
    failures += RunTest(&UninstallFailureReturnsFailure, "UninstallFailureReturnsFailure");
    failures += RunTest(&ConsoleRouteSkipsScmDispatcher, "ConsoleRouteSkipsScmDispatcher");
    failures += RunTest(&ScmSuccessDoesNotFallbackToConsole, "ScmSuccessDoesNotFallbackToConsole");
#endif
    return failures == 0 ? 0 : 1;
}
