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
    int stopEGoFailuresRemaining = 0;
    bool restoreHuawei = true;
    bool egoAlive = true;
    int egoAliveQueries = 0;
    std::vector<std::string> calls;
    std::vector<PenStatus::TouchProviderState> states;

    // 注入的时钟。真机上每一次 SCM 操作都要花掉可观的时间——停原厂 1.6~3.4 秒、起原厂
    // 0.32 秒、一次完整接管 4.9 秒——而协调器的正确性恰恰取决于这段时间算在谁头上。
    // 每次操作推进 stepCost，测试因此能表达「切换本身比整个租期还长」。
    TouchProviderCoordinator::Clock::time_point clock{};
    std::chrono::milliseconds stepCost{0};

    TouchProviderCoordinator Make(std::chrono::milliseconds timeout =
                                      std::chrono::milliseconds(5000)) {
        Service::TouchProviderOperations operations{};
        const auto step = [this](const char* name) {
            calls.push_back(name);
            clock += stepCost;
        };
        operations.stopHuawei = [this, step] { step("stopHuawei"); return stopHuawei; };
        operations.disableHuawei = [this, step] { step("disableHuawei"); return disableHuawei; };
        operations.startEGo = [this, step] { step("startEGo"); return startEGo; };
        operations.stopEGo = [this, step] {
            step("stopEGo");
            if (stopEGoFailuresRemaining > 0) {
                --stopEGoFailuresRemaining;
                return false;
            }
            return stopEGo;
        };
        operations.restoreHuawei = [this, step] { step("restoreHuawei"); return restoreHuawei; };
        operations.egoAlive = [this] { ++egoAliveQueries; return egoAlive; };
        operations.now = [this] { return clock; };
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

// 宿主里的 THP_Service 收不到电源通知，面板断电之后它内部就死了而进程还在（唤醒瞬间实测
// running=1, exit=-1）。挂起时只停宿主，避免亮屏后再等原厂停止。
void TestSuspendStopsOnlyEGoAndResumeRetakesTouch() {
    Fixture f;
    auto coordinator = f.Make();
    const auto start = TouchProviderCoordinator::Clock::time_point{};
    Require(coordinator.AcquireOrRenew(start), "precondition: EGo active");

    f.calls.clear();
    coordinator.OnSuspend(start + std::chrono::seconds(1));
    Require(f.calls == std::vector<std::string>{"stopEGo"},
            "suspend stops only EGo and never starts Huawei");
    Require(coordinator.State() == PenStatus::TouchProviderState::EGoSuspended,
            "suspend publishes the intentionally stopped provider state");
    Require(coordinator.Error() == TouchProviderError::None,
            "a clean suspension is not an error");
    Require(coordinator.IsSuspended(), "suspend is queryable by the display watchdog");
    Require(coordinator.HasLease(), "the tray never gave the lease back");
    coordinator.OnSuspend(start + std::chrono::seconds(1));
    Require(f.calls == std::vector<std::string>{"stopEGo"},
            "duplicate suspend does not stop the host twice");

    f.calls.clear();
    coordinator.OnResume(start + std::chrono::seconds(2));
    Require(f.calls == std::vector<std::string>{"stopHuawei", "startEGo", "disableHuawei"},
            "resume takes touch back through the ordinary takeover path");
    Require(coordinator.State() == PenStatus::TouchProviderState::EGoTouch,
            "resume returns to EGoTouch");
    Require(!coordinator.IsSuspended(), "resume clears the suspended flag");
}

// 息屏并不冻结托盘，它照常每秒续租一次。续租必须延租约却不能把宿主拉起来，否则代管等于
// 没做过；同时挂起期间不得去问宿主死活——它是我们自己停的。
void TestSuspendedRenewalsExtendTheLeaseWithoutTakingOver() {
    Fixture f;
    auto coordinator = f.Make(std::chrono::milliseconds(100));
    const auto start = TouchProviderCoordinator::Clock::time_point{};
    Require(coordinator.AcquireOrRenew(start), "precondition: EGo active");
    coordinator.OnSuspend(start);

    f.calls.clear();
    f.egoAliveQueries = 0;
    auto now = start;
    for (int i = 0; i < 5; ++i) {
        now += std::chrono::milliseconds(50);
        Require(coordinator.AcquireOrRenew(now), "renewals are accepted while suspended");
        coordinator.Tick(now);
    }
    Require(f.calls.empty(), "no switch of any kind happens while the machine sleeps");
    Require(f.egoAliveQueries == 0, "the host is not probed: we stopped it ourselves");
    Require(coordinator.State() == PenStatus::TouchProviderState::EGoSuspended,
            "both providers stay stopped until the display resumes");
    Require(coordinator.HasLease(), "the renewals kept the lease alive");
}

// 用户会话在深度睡眠期间冻结，Tick 即使先于唤醒心跳也不能误判租约超时。
void TestSuspendedLeaseSurvivesAnHourWithoutRenewal() {
    Fixture f;
    auto coordinator = f.Make(std::chrono::milliseconds(100));
    const auto start = TouchProviderCoordinator::Clock::time_point{};
    Require(coordinator.AcquireOrRenew(start), "precondition: EGo active");
    coordinator.OnSuspend(start);

    f.calls.clear();
    f.egoAliveQueries = 0;
    f.egoAlive = false;
    coordinator.Tick(start + std::chrono::hours(1));
    Require(coordinator.HasLease(), "suspension preserves even an unrenewed lease");
    Require(coordinator.IsSuspended(), "sleeping for an hour does not clear suspension");
    Require(f.calls.empty(), "suspended tick never starts Huawei or EGo");
    Require(f.egoAliveQueries == 0, "suspended tick never probes the stopped host");
    Require(coordinator.AcquireOrRenew(start + std::chrono::hours(1)),
            "renewal remains accepted after an hour asleep");
    Require(f.calls.empty(), "suspended renewal only extends the lease");

    f.clock = start + std::chrono::hours(1);
    coordinator.OnResume(f.clock);
    Require(f.calls == std::vector<std::string>{"stopHuawei", "startEGo", "disableHuawei"},
            "resume still retakes touch after the original lease deadline");
    Require(coordinator.State() == PenStatus::TouchProviderState::EGoTouch,
            "EGo is restored even if the tray has not yet resumed");
}

// 唤醒后宿主起不来，走的是普通接管的失败路径：SwitchToEGo 自带回退，原厂留在原地，
// 并进入接管失败冷却。这里不该再动用「跑着跑着崩掉」那套重启预算，因为根本还没有宿主在跑。
void TestResumeStartFailureLeavesHuaweiOwningTouch() {
    Fixture f;
    auto coordinator = f.Make();
    const auto start = TouchProviderCoordinator::Clock::time_point{};
    Require(coordinator.AcquireOrRenew(start), "precondition: EGo active");
    coordinator.OnSuspend(start + std::chrono::seconds(1));

    f.calls.clear();
    f.startEGo = false;
    coordinator.OnResume(start + std::chrono::seconds(2));
    Require(f.calls == std::vector<std::string>{"stopHuawei", "startEGo", "restoreHuawei"},
            "a failed retake restores Huawei on the spot");
    Require(coordinator.State() == PenStatus::TouchProviderState::Huawei,
            "Huawei ends up owning touch");
    Require(!coordinator.HasLease(), "a failed takeover drops the lease");
    Require(!coordinator.IsSuspended(), "failed resume leaves suspension");
    Require(coordinator.Error() == TouchProviderError::StartEGoFailed,
            "failed resume records the start failure");
    f.calls.clear();
    Require(!coordinator.AcquireOrRenew(f.clock + std::chrono::seconds(1)),
            "failed resume enters the acquire cooldown");
    Require(f.calls.empty(), "cooldown does not disturb the restored Huawei provider");
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

// 唤醒之后托盘的心跳恢复得比服务晚，接管又可能很快，租期不足以撑到第一拍心跳。宽限是
// 唤醒后的租期下限，不是替换值。
void TestResumeLeaseIsAtLeastTheGrace() {
    Fixture f;
    auto coordinator = f.Make(std::chrono::milliseconds(1000));
    const auto start = TouchProviderCoordinator::Clock::time_point{};
    Require(coordinator.AcquireOrRenew(start), "precondition: EGo active");
    coordinator.OnSuspend(start + std::chrono::seconds(1));

    const auto resumeAt = start + std::chrono::hours(3);
    f.clock = resumeAt;
    // 恢复过程本身也耗时，宽限必须从切换完成而非 OnResume 入口起算。
    f.stepCost = std::chrono::milliseconds(100);
    coordinator.OnResume(resumeAt);
    const auto resumedAt = f.clock;
    Require(coordinator.State() == PenStatus::TouchProviderState::EGoTouch,
            "precondition: the retake succeeded");

    f.calls.clear();
    coordinator.Tick(resumedAt + TouchProviderCoordinator::kResumeLeaseGrace -
                     std::chrono::milliseconds(1));
    Require(f.calls.empty(), "the grace outlives the 1 s lease the takeover would have given");
    coordinator.Tick(resumedAt + TouchProviderCoordinator::kResumeLeaseGrace);
    Require(f.calls == std::vector<std::string>{"stopEGo", "restoreHuawei"},
            "a tray that never comes back still loses the lease");
}

void TestShortDisplayOffStillGrantsResumeGrace() {
    Fixture f;
    auto coordinator = f.Make(std::chrono::milliseconds(100));
    Require(coordinator.AcquireOrRenew(f.clock), "precondition: EGo active");
    f.calls.clear();
    const auto resumeAt = f.clock + std::chrono::milliseconds(50);
    coordinator.OnResume(resumeAt);
    coordinator.Tick(resumeAt + TouchProviderCoordinator::kResumeLeaseGrace -
                     std::chrono::milliseconds(1));
    Require(f.calls.empty(), "short display-off grants grace without restarting the host");
    coordinator.Tick(resumeAt + TouchProviderCoordinator::kResumeLeaseGrace);
    Require(f.calls == std::vector<std::string>{"stopEGo", "restoreHuawei"},
            "short display-off grace still eventually expires");
}

void TestResumeDoesNotShortenALongerLease() {
    Fixture f;
    auto coordinator = f.Make(std::chrono::hours(1));
    Require(coordinator.AcquireOrRenew(f.clock), "precondition: EGo active");
    coordinator.OnSuspend(f.clock);
    coordinator.OnResume(f.clock);
    f.calls.clear();
    coordinator.Tick(f.clock + TouchProviderCoordinator::kResumeLeaseGrace);
    Require(f.calls.empty(), "resume grace never shortens a longer lease");
    Require(coordinator.HasLease(), "the longer lease remains held");
}

void TestSuspendedReleaseAndOrdinaryShutdownRestoreHuawei() {
    for (const bool shutdown : {false, true}) {
        Fixture f;
        auto coordinator = f.Make();
        Require(coordinator.AcquireOrRenew(f.clock), "precondition: EGo active");
        coordinator.OnSuspend(f.clock);
        f.calls.clear();
        if (shutdown) coordinator.Shutdown(false);
        else Require(coordinator.Release(), "release while suspended succeeds");
        Require(f.calls == std::vector<std::string>{"stopEGo", "restoreHuawei"},
                "release and ordinary shutdown restore Huawei even from EGoSuspended");
        Require(coordinator.State() == PenStatus::TouchProviderState::Huawei,
                "Huawei owns touch after leaving suspension");
        Require(!coordinator.HasLease() && !coordinator.IsSuspended(),
                "release and shutdown clear both the lease and suspension");
    }
}

void TestSuspendStopFailureFallsBackToHuawei() {
    Fixture f;
    auto coordinator = f.Make();
    Require(coordinator.AcquireOrRenew(f.clock), "precondition: EGo active");
    f.calls.clear();
    f.stopEGoFailuresRemaining = 1;
    coordinator.OnSuspend(f.clock);
    Require(f.calls == std::vector<std::string>{"stopEGo", "stopEGo", "restoreHuawei"},
            "a failed initial stop retries through the Huawei fallback transaction");
    Require(coordinator.State() == PenStatus::TouchProviderState::Huawei,
            "successful suspend fallback leaves Huawei owning touch");
    Require(coordinator.IsSuspended() && coordinator.HasLease(),
            "fallback retains suspension and lease for resume");
    f.calls.clear();
    coordinator.Tick(f.clock + std::chrono::hours(1));
    Require(f.calls.empty(), "fallback suspension also ignores lease expiry");
    coordinator.OnResume(f.clock);
    Require(f.calls == std::vector<std::string>{"stopHuawei", "startEGo", "disableHuawei"},
            "resume retakes touch even after the suspend fallback");
}

void TestSuspendStopAndFallbackFailureRemainRecoverable() {
    Fixture f;
    auto coordinator = f.Make();
    Require(coordinator.AcquireOrRenew(f.clock), "precondition: EGo active");
    f.calls.clear();
    f.stopEGo = false;
    coordinator.OnSuspend(f.clock);
    Require(f.calls == std::vector<std::string>{"stopEGo", "stopEGo"},
            "unconfirmed host stop must not start Huawei concurrently");
    Require(coordinator.State() == PenStatus::TouchProviderState::EGoTouch &&
                coordinator.Error() == TouchProviderError::StopEGoFailed,
            "failed suspend fallback publishes EGoTouch and StopEGoFailed");
    Require(coordinator.IsSuspended() && coordinator.HasLease(),
            "failed fallback retains the resume path");
    f.calls.clear();
    Require(coordinator.AcquireOrRenew(f.clock), "suspended error state accepts renewal");
    coordinator.Tick(f.clock + std::chrono::hours(1));
    Require(f.calls.empty() && f.egoAliveQueries == 0,
            "suspended error state neither retries takeover nor probes the host");
    f.stopEGo = true;
    coordinator.OnResume(f.clock);
    Require(f.calls == std::vector<std::string>{"stopEGo", "stopHuawei", "startEGo", "disableHuawei"},
            "resume stops the stale EGo host first before starting a fresh one");
    Require(coordinator.State() == PenStatus::TouchProviderState::EGoTouch &&
                coordinator.Error() == TouchProviderError::None,
            "resume after stop recovery returns cleanly to EGoTouch");
}

// 一次接管同步占住控制线程 4888~5129 毫秒，而租期就是 5000 毫秒。租期若按进入时刻起算，
// 接管成功后紧跟的那一拍 Tick 立刻判它过期，把刚拿到的触控又交还回去：真机日志上是接管
// 成功 4 毫秒后自己撤销，宿主起停各两次，白折腾 15 秒。
void TestSlowTakeoverDoesNotExpireItsOwnLease() {
    Fixture f;
    f.stepCost = std::chrono::milliseconds(1800);  // 三步共 5400 ms，比整个租期还长
    auto coordinator = f.Make(std::chrono::milliseconds(5000));

    const auto enteredAt = f.clock;
    Require(coordinator.AcquireOrRenew(enteredAt), "precondition: the takeover succeeds");
    Require(f.clock - enteredAt > std::chrono::milliseconds(5000),
            "precondition: the switch itself outlasts the whole lease");

    f.calls.clear();
    // 控制线程的循环紧接着就跑这一拍，用的是切换之后的时刻。
    coordinator.Tick(f.clock);
    Require(f.calls.empty(),
            "the tick right after a slow takeover must not expire the lease it just created");
    Require(coordinator.State() == PenStatus::TouchProviderState::EGoTouch,
            "touch stays with EGo instead of bouncing back to Huawei");
    Require(coordinator.HasLease(), "the lease survives its own takeover");
}

// 关机时把原厂请回来是白等：它是 AUTO_START，下次开机自己会起，而此刻 StartService 必然
// 失败。实测正常停止要 6331 毫秒，多出来的那几秒正是花在这一趟上，而 STOP_PENDING 只报了
// 5000 毫秒 hint，超时之后 SCM 直接终止进程，终止点可能落在两个提供方都停着的那一刻。
void TestShutdownDuringSystemShutdownDoesNotWaitForHuawei() {
    Fixture f;
    auto coordinator = f.Make();
    Require(coordinator.AcquireOrRenew(TouchProviderCoordinator::Clock::time_point{}),
            "precondition: EGo active");

    f.calls.clear();
    coordinator.Shutdown(true);
    Require(f.calls == std::vector<std::string>{"stopEGo"},
            "a system shutdown only hands the device back, it does not start Huawei");
}

// 普通停止（服务被停掉、升级、卸载）机器还要接着用，触控必须交还原厂。
void TestOrdinaryShutdownStillRestoresHuawei() {
    Fixture f;
    auto coordinator = f.Make();
    Require(coordinator.AcquireOrRenew(TouchProviderCoordinator::Clock::time_point{}),
            "precondition: EGo active");

    f.calls.clear();
    coordinator.Shutdown(false);
    Require(f.calls == std::vector<std::string>{"stopEGo", "restoreHuawei"},
            "an ordinary stop hands touch back to Huawei");
    Require(coordinator.State() == PenStatus::TouchProviderState::Huawei,
            "and says so");
}

// 看护宿主的判据是「原厂正被我们停着」，不是「我们有没有租约」。租约超时那条路先清掉
// m_hasLease 再切换，这次切换若失败并回滚成 EGoTouch，宿主就跑在一个没人看着的状态里，
// 而托盘此时通常已经不在了——宿主再死就是彻底无触控，状态却仍显示 EGoTouch。
void TestHostStaysWatchedAfterTheLeaseIsGone() {
    Fixture f;
    auto coordinator = f.Make(std::chrono::milliseconds(100));
    const auto start = TouchProviderCoordinator::Clock::time_point{};
    Require(coordinator.AcquireOrRenew(start), "precondition: EGo active");

    // 租约到期，交还原厂失败，回滚到 EGoTouch：宿主还在跑，原厂仍被我们停着。
    f.restoreHuawei = false;
    coordinator.Tick(start + std::chrono::seconds(1));
    Require(!coordinator.HasLease(), "precondition: the lease is gone");
    Require(coordinator.State() == PenStatus::TouchProviderState::EGoTouch,
            "precondition: the rollback left EGo driving touch");
    Require(coordinator.Error() == TouchProviderError::RestoreHuaweiFailed,
            "precondition: the published error records why Huawei never came back");

    f.calls.clear();
    f.egoAliveQueries = 0;
    f.egoAlive = false;
    coordinator.Tick(start + std::chrono::seconds(2));
    Require(f.egoAliveQueries > 0, "a host with no lease behind it is still probed for life");
    Require(!f.calls.empty(), "and a dead one is still acted upon");
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
        TestSuspendStopsOnlyEGoAndResumeRetakesTouch();
        TestSuspendedRenewalsExtendTheLeaseWithoutTakingOver();
        TestSuspendedLeaseSurvivesAnHourWithoutRenewal();
        TestResumeStartFailureLeavesHuaweiOwningTouch();
        TestSuspendOutsideEGoIsANoOp();
        TestResumeLeaseIsAtLeastTheGrace();
        TestShortDisplayOffStillGrantsResumeGrace();
        TestResumeDoesNotShortenALongerLease();
        TestSuspendedReleaseAndOrdinaryShutdownRestoreHuawei();
        TestSuspendStopFailureFallsBackToHuawei();
        TestSuspendStopAndFallbackFailureRemainRecoverable();
        TestSlowTakeoverDoesNotExpireItsOwnLease();
        TestHostStaysWatchedAfterTheLeaseIsGone();
        TestShutdownDuringSystemShutdownDoesNotWaitForHuawei();
        TestOrdinaryShutdownStillRestoresHuawei();
        std::cout << "[TEST] Touch provider coordinator tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[TEST] " << error.what() << "\n";
        return 1;
    }
}
