#include "TouchProviderCoordinator.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Service::TouchProviderCoordinator;
using Service::TouchProviderError;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    bool stopHuawei = true;
    bool disableHuawei = true;
    bool startEGo = true;
    bool stopEGo = true;
    bool restoreHuawei = true;
    bool egoAlive = true;
    std::vector<std::string> calls;
    std::vector<PenStatus::TouchProviderState> states;

    TouchProviderCoordinator Make(std::chrono::milliseconds timeout =
                                      std::chrono::milliseconds(5000)) {
        Service::TouchProviderOperations operations{};
        operations.stopHuawei = [this] { calls.push_back("stopHuawei"); return stopHuawei; };
        operations.disableHuawei = [this] { calls.push_back("disableHuawei"); return disableHuawei; };
        operations.startEGo = [this] { calls.push_back("startEGo"); return startEGo; };
        operations.stopEGo = [this] { calls.push_back("stopEGo"); return stopEGo; };
        operations.restoreHuawei = [this] { calls.push_back("restoreHuawei"); return restoreHuawei; };
        operations.egoAlive = [this] { return egoAlive; };
        return TouchProviderCoordinator(
            std::move(operations),
            [this](PenStatus::TouchProviderState state, TouchProviderError) {
                states.push_back(state);
            },
            timeout);
    }
};

void TestAcquireStartsEGoBeforeDisablingHuawei() {
    Fixture f;
    auto coordinator = f.Make();
    Require(coordinator.AcquireOrRenew(TouchProviderCoordinator::Clock::time_point{}),
            "acquire should succeed");
    Require(f.calls == std::vector<std::string>{"stopHuawei", "startEGo", "disableHuawei"},
            "Huawei must only be disabled after EGo is running");
    Require(coordinator.State() == PenStatus::TouchProviderState::EGoTouch,
            "successful acquire publishes EGoTouch");
    Require(coordinator.HasLease(), "successful acquire holds a lease");
}

void TestStartFailureRestoresHuawei() {
    Fixture f;
    f.startEGo = false;
    auto coordinator = f.Make();
    Require(!coordinator.AcquireOrRenew(TouchProviderCoordinator::Clock::time_point{}),
            "failed EGo start rejects the lease");
    Require(f.calls == std::vector<std::string>{"stopHuawei", "startEGo", "restoreHuawei"},
            "start failure immediately restores Huawei");
    Require(coordinator.State() == PenStatus::TouchProviderState::Huawei,
            "Huawei remains the active provider");
    Require(!coordinator.HasLease(), "failed acquire drops the lease");
}

void TestDisableFailureStopsEGoAndRestoresHuawei() {
    Fixture f;
    f.disableHuawei = false;
    auto coordinator = f.Make();
    Require(!coordinator.AcquireOrRenew(TouchProviderCoordinator::Clock::time_point{}),
            "disable failure rejects takeover");
    Require(f.calls == std::vector<std::string>{
                "stopHuawei", "startEGo", "disableHuawei", "stopEGo", "restoreHuawei"},
            "disable failure rolls back in safe order");
    Require(coordinator.State() == PenStatus::TouchProviderState::Huawei,
            "rollback returns to Huawei");
}

void TestLeaseTimeoutReturnsToHuawei() {
    Fixture f;
    auto coordinator = f.Make(std::chrono::milliseconds(100));
    const auto start = TouchProviderCoordinator::Clock::time_point{};
    Require(coordinator.AcquireOrRenew(start), "acquire succeeds");
    f.calls.clear();
    coordinator.Tick(start + std::chrono::milliseconds(99));
    Require(f.calls.empty(), "lease remains valid before its deadline");
    coordinator.Tick(start + std::chrono::milliseconds(100));
    Require(f.calls == std::vector<std::string>{"stopEGo", "restoreHuawei"},
            "expired lease hands control back to Huawei");
    Require(coordinator.State() == PenStatus::TouchProviderState::Huawei,
            "timeout publishes Huawei state");
}

void TestHuaweiFailureRollsBackToEGo() {
    Fixture f;
    auto coordinator = f.Make();
    Require(coordinator.AcquireOrRenew(TouchProviderCoordinator::Clock::time_point{}),
            "precondition: EGo active");
    f.calls.clear();
    f.restoreHuawei = false;
    Require(!coordinator.Release(), "release reports Huawei failure");
    Require(f.calls == std::vector<std::string>{
                "stopEGo", "restoreHuawei", "startEGo", "disableHuawei"},
            "failed Huawei restore rolls back to EGo");
    Require(coordinator.State() == PenStatus::TouchProviderState::EGoTouch,
            "rollback keeps a touch provider active");
    Require(coordinator.Error() == TouchProviderError::RestoreHuaweiFailed,
            "rollback exposes the Huawei failure");
}

void TestDeadHostIsRestartedInPlace() {
    Fixture f;
    auto coordinator = f.Make();
    const auto start = TouchProviderCoordinator::Clock::time_point{};
    Require(coordinator.AcquireOrRenew(start), "precondition: EGo active");

    f.calls.clear();
    f.egoAlive = false;
    coordinator.Tick(start + std::chrono::seconds(1));
    Require(f.calls == std::vector<std::string>{"stopHuawei", "startEGo", "disableHuawei"},
            "a dead host is restarted in place, not handed back to Huawei");
    Require(coordinator.State() == PenStatus::TouchProviderState::EGoTouch,
            "a successful restart keeps EGoTouch active");
    Require(coordinator.HasLease(), "a successful restart keeps the lease");
}

void TestLiveHostIsLeftAlone() {
    Fixture f;
    auto coordinator = f.Make();
    const auto start = TouchProviderCoordinator::Clock::time_point{};
    Require(coordinator.AcquireOrRenew(start), "precondition: EGo active");

    f.calls.clear();
    coordinator.Tick(start + std::chrono::seconds(1));
    Require(f.calls.empty(), "a live host is not touched");
}

void TestRepeatedDeathFallsBackToHuaweiAndCoolsDown() {
    Fixture f;
    auto coordinator = f.Make();
    auto now = TouchProviderCoordinator::Clock::time_point{};
    Require(coordinator.AcquireOrRenew(now), "precondition: EGo active");

    // 宿主每次都在重启后立刻再死。允许的重启次数用尽之后就该交还原厂。
    f.egoAlive = false;
    for (int i = 0; i < TouchProviderCoordinator::kMaxRestartsPerWindow; ++i) {
        now += std::chrono::seconds(1);
        coordinator.Tick(now);
        Require(coordinator.State() == PenStatus::TouchProviderState::EGoTouch,
                "restarts within the window keep trying EGoTouch");
    }

    f.calls.clear();
    now += std::chrono::seconds(1);
    coordinator.Tick(now);
    Require(f.calls == std::vector<std::string>{"stopEGo", "restoreHuawei"},
            "the restart budget runs out and Huawei takes over");
    Require(coordinator.State() == PenStatus::TouchProviderState::Huawei,
            "Huawei becomes the active provider");
    Require(coordinator.Error() == TouchProviderError::EGoHostDied,
            "the published error names the root cause, not the fallback path");
    Require(!coordinator.HasLease(), "giving up drops the lease");

    // 托盘不知道刚才发生了什么，照常每秒续租。冷却期内这些续租必须全部被挡住,
    // 否则就是每秒一轮的提供方来回切换。
    f.calls.clear();
    now += std::chrono::seconds(1);
    Require(!coordinator.AcquireOrRenew(now), "renewal is refused during the cooldown");
    Require(f.calls.empty(), "a refused renewal touches neither provider");

    // 冷却结束后允许再试一次。这一次宿主是好的。
    f.egoAlive = true;
    now += TouchProviderCoordinator::kRestartCooldown;
    Require(coordinator.AcquireOrRenew(now), "takeover resumes once the cooldown expires");
    Require(coordinator.State() == PenStatus::TouchProviderState::EGoTouch,
            "EGoTouch comes back after the cooldown");
}

void TestRestartBudgetIsPerWindow() {
    Fixture f;
    auto coordinator = f.Make(std::chrono::hours(1));
    auto now = TouchProviderCoordinator::Clock::time_point{};
    Require(coordinator.AcquireOrRenew(now), "precondition: EGo active");

    // 偶发崩溃彼此隔得足够远时不该累积。窗口过期后计数重置，宿主永远只是被重启。
    for (int i = 0; i < TouchProviderCoordinator::kMaxRestartsPerWindow + 2; ++i) {
        f.egoAlive = false;
        now += TouchProviderCoordinator::kRestartWindow + std::chrono::seconds(1);
        coordinator.Tick(now);
        f.egoAlive = true;
        Require(coordinator.State() == PenStatus::TouchProviderState::EGoTouch,
                "crashes spread beyond the window are always restarted");
        Require(coordinator.HasLease(), "the lease survives an isolated crash");
    }
}

void TestReleaseClearsTheCooldown() {
    Fixture f;
    auto coordinator = f.Make();
    auto now = TouchProviderCoordinator::Clock::time_point{};
    Require(coordinator.AcquireOrRenew(now), "precondition: EGo active");

    f.egoAlive = false;
    for (int i = 0; i <= TouchProviderCoordinator::kMaxRestartsPerWindow; ++i) {
        now += std::chrono::seconds(1);
        coordinator.Tick(now);
    }
    Require(coordinator.Error() == TouchProviderError::EGoHostDied,
            "precondition: the coordinator gave up and is cooling down");

    // 用户在托盘里手动关掉再打开，是一次全新的接管请求，不该继承上一轮的冷却。
    f.egoAlive = true;
    (void)coordinator.Release();
    now += std::chrono::seconds(1);
    Require(coordinator.AcquireOrRenew(now),
            "an explicit release clears the cooldown");
}

} // namespace

int main() {
    try {
        TestAcquireStartsEGoBeforeDisablingHuawei();
        TestStartFailureRestoresHuawei();
        TestDisableFailureStopsEGoAndRestoresHuawei();
        TestLeaseTimeoutReturnsToHuawei();
        TestHuaweiFailureRollsBackToEGo();
        TestDeadHostIsRestartedInPlace();
        TestLiveHostIsLeftAlone();
        TestRepeatedDeathFallsBackToHuaweiAndCoolsDown();
        TestRestartBudgetIsPerWindow();
        TestReleaseClearsTheCooldown();
        std::cout << "[TEST] Touch provider coordinator tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[TEST] " << error.what() << "\n";
        return 1;
    }
}
