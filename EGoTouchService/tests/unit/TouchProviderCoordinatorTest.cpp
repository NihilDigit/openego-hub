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
    int egoAliveQueries = 0;
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
        operations.egoAlive = [this] { ++egoAliveQueries; return egoAlive; };
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

// 接管失败之后托盘并不知情，它每秒照常续租一次。冷却挡住的是「停原厂、起宿主、再把原厂
// 请回来」这一整套动作被每秒重演一遍——那段时间里触控时有时无。
void TestAcquireFailureCoolsDown() {
    Fixture f;
    f.startEGo = false;
    auto coordinator = f.Make();
    auto now = TouchProviderCoordinator::Clock::time_point{} + std::chrono::hours(1);

    Require(!coordinator.AcquireOrRenew(now), "precondition: the acquire fails");
    const auto callsAfterFailure = f.calls.size();

    now += std::chrono::seconds(1);
    Require(!coordinator.AcquireOrRenew(now), "renewals stay refused during the cooldown");
    Require(f.calls.size() == callsAfterFailure,
            "no second switch attempt is made while cooling down");

    now += TouchProviderCoordinator::kAcquireFailureCooldown;
    f.startEGo = true;
    Require(coordinator.AcquireOrRenew(now), "the cooldown expires and takeover is retried");
}

// 宿主里的 THP_Service 收不到电源通知，面板断电之后它内部就死了而进程还在。挂起时由服务
// 主动停掉、唤醒时重启，是这条链上唯一能观察到电源状态的一环。
void TestSuspendStopsTheHostAndResumeStartsIt() {
    Fixture f;
    auto coordinator = f.Make();
    const auto start = TouchProviderCoordinator::Clock::time_point{};
    Require(coordinator.AcquireOrRenew(start), "precondition: EGo active");

    f.calls.clear();
    coordinator.OnSuspend(start + std::chrono::seconds(1));
    Require(f.calls == std::vector<std::string>{"stopEGo"},
            "suspend stops the host and leaves Huawei alone: the machine is going to sleep");
    Require(coordinator.State() == PenStatus::TouchProviderState::EGoSuspended,
            "suspend publishes EGoSuspended");
    Require(coordinator.HasLease(), "the lease is frozen, not dropped");

    coordinator.OnResume(start + std::chrono::seconds(2));
    Require(f.calls == std::vector<std::string>{"stopEGo", "startEGo"},
            "resume restarts the host exactly once");
    Require(coordinator.State() == PenStatus::TouchProviderState::EGoTouch,
            "resume returns to EGoTouch");
}

// 息屏并不冻结托盘，它照常每秒续租一次。续租不该把刚停下的宿主拉起来。
void TestSuspendedStateIgnoresTicksAndRenewals() {
    Fixture f;
    auto coordinator = f.Make(std::chrono::milliseconds(100));
    const auto start = TouchProviderCoordinator::Clock::time_point{};
    Require(coordinator.AcquireOrRenew(start), "precondition: EGo active");
    coordinator.OnSuspend(start);

    f.calls.clear();
    f.egoAliveQueries = 0;
    coordinator.Tick(start + std::chrono::hours(1));
    Require(f.calls.empty(), "a frozen lease does not expire while the machine sleeps");
    Require(f.egoAliveQueries == 0,
            "the host is not probed for life: we stopped it ourselves");
    Require(coordinator.AcquireOrRenew(start + std::chrono::hours(1)),
            "renewals are accepted while suspended");
    Require(f.calls.empty(), "a renewal does not restart the host before the resume");
    Require(coordinator.State() == PenStatus::TouchProviderState::EGoSuspended,
            "the state stays suspended until the resume arrives");
}

void TestResumeStartFailureFallsBackToHuawei() {
    Fixture f;
    auto coordinator = f.Make();
    const auto start = TouchProviderCoordinator::Clock::time_point{};
    Require(coordinator.AcquireOrRenew(start), "precondition: EGo active");
    coordinator.OnSuspend(start + std::chrono::seconds(1));

    f.calls.clear();
    f.startEGo = false;
    coordinator.OnResume(start + std::chrono::seconds(2));
    Require(f.calls == std::vector<std::string>{
                "startEGo", "stopHuawei", "startEGo", "restoreHuawei"},
            "a host that will not come back takes the existing crash path");
    Require(coordinator.State() == PenStatus::TouchProviderState::Huawei,
            "Huawei ends up owning touch");
    Require(coordinator.Error() == TouchProviderError::EGoHostDied,
            "the published error names the dead host");
    Require(!coordinator.HasLease(), "giving up drops the lease");
}

// 重启预算已经用尽时 HandleEGoHostDeath 根本不会再试 SwitchToEGo，状态还停在挂起。
// 这一路必须把原厂请回来，否则宿主停着、原厂也停着，一个提供方都不剩。
void TestResumeWithExhaustedBudgetRestoresHuawei() {
    Fixture f;
    auto coordinator = f.Make();
    auto now = TouchProviderCoordinator::Clock::time_point{};
    Require(coordinator.AcquireOrRenew(now), "precondition: EGo active");

    f.egoAlive = false;
    for (int i = 0; i < TouchProviderCoordinator::kMaxRestartsPerWindow; ++i) {
        now += std::chrono::milliseconds(500);
        coordinator.Tick(now);
    }
    f.egoAlive = true;
    Require(coordinator.State() == PenStatus::TouchProviderState::EGoTouch,
            "precondition: the restart budget is spent but EGo still owns touch");

    now += std::chrono::milliseconds(500);
    coordinator.OnSuspend(now);
    f.calls.clear();
    f.startEGo = false;
    now += std::chrono::milliseconds(500);
    coordinator.OnResume(now);
    Require(f.calls == std::vector<std::string>{"startEGo", "stopEGo", "restoreHuawei"},
            "the suspended state hands touch back instead of being left with no provider");
    Require(coordinator.State() == PenStatus::TouchProviderState::Huawei,
            "Huawei owns touch again");
}

void TestSuspendOutsideEGoIsANoOp() {
    Fixture f;
    auto coordinator = f.Make();
    const auto start = TouchProviderCoordinator::Clock::time_point{};

    coordinator.OnSuspend(start);
    Require(f.calls.empty(), "nothing to suspend while Huawei owns touch");
    coordinator.OnResume(start);
    Require(f.calls.empty(), "and nothing to resume either");
    Require(!coordinator.HasLease(), "a power event never grants a lease");
}

// 唤醒之后托盘的心跳恢复得比服务晚。按冻结时剩下的时间计租约，第一拍心跳还没到租约就过期
// 了，触控白白交还原厂再抢回来一次。
void TestResumeExtendsAShortLeaseToTheGrace() {
    Fixture f;
    auto coordinator = f.Make(std::chrono::milliseconds(5000));
    const auto start = TouchProviderCoordinator::Clock::time_point{};
    Require(coordinator.AcquireOrRenew(start), "precondition: EGo active");

    const auto suspendAt = start + std::chrono::milliseconds(4900);
    coordinator.OnSuspend(suspendAt);
    const auto resumeAt = suspendAt + std::chrono::hours(3);
    coordinator.OnResume(resumeAt);

    f.calls.clear();
    coordinator.Tick(resumeAt + TouchProviderCoordinator::kResumeLeaseGrace -
                     std::chrono::milliseconds(1));
    Require(f.calls.empty(), "the grace outlives the 100 ms that were left on the lease");
    coordinator.Tick(resumeAt + TouchProviderCoordinator::kResumeLeaseGrace);
    Require(f.calls == std::vector<std::string>{"stopEGo", "restoreHuawei"},
            "a tray that never comes back still loses the lease");
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
        TestAcquireFailureCoolsDown();
        TestSuspendStopsTheHostAndResumeStartsIt();
        TestSuspendedStateIgnoresTicksAndRenewals();
        TestResumeStartFailureFallsBackToHuawei();
        TestResumeWithExhaustedBudgetRestoresHuawei();
        TestSuspendOutsideEGoIsANoOp();
        TestResumeExtendsAShortLeaseToTheGrace();
        std::cout << "[TEST] Touch provider coordinator tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[TEST] " << error.what() << "\n";
        return 1;
    }
}
