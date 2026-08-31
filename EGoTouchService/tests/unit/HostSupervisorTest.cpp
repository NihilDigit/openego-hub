#include "HostSupervisor.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {

using Service::HostAction;
using Service::HostSupervisor;
using Clock = HostSupervisor::Clock;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// 启动宽限期一过就能开始判定的一个监督器，省得每个用例重写这四行。
HostSupervisor MakeRunning(Clock::time_point& now) {
    HostSupervisor supervisor("test");
    supervisor.SetDesiredRunning(true, now);
    now += HostSupervisor::kStartGrace + std::chrono::seconds(1);
    return supervisor;
}

void TestLiveHostIsLeftAlone() {
    auto now = Clock::time_point{};
    auto supervisor = MakeRunning(now);

    uint32_t heartbeat = 1;
    for (int i = 0; i < 20; ++i) {
        now += std::chrono::milliseconds(250);
        if (i % 4 == 0) ++heartbeat;   // 宿主每秒推进一次，巡检每 250 毫秒一轮
        Require(supervisor.Tick(now, true, heartbeat) == HostAction::None,
                "a live host is never restarted");
    }
    Require(supervisor.Healthy(), "a live host is healthy");
}

void TestDeadProcessIsRestarted() {
    auto now = Clock::time_point{};
    auto supervisor = MakeRunning(now);
    Require(supervisor.Tick(now, true, 1) == HostAction::None, "precondition: alive");

    now += std::chrono::milliseconds(250);
    Require(supervisor.Tick(now, false, 1) == HostAction::Restart,
            "a host whose process is gone is restarted");
    Require(!supervisor.Healthy(), "a dead host is not healthy");
}

// 宿主进程还在、内部的循环已经停住：快照的 seqlock 停在最后一帧，读者拿到的仍是一份
// 自洽的旧状态，只有心跳分辨得出来。
void TestFrozenHostIsRestarted() {
    auto now = Clock::time_point{};
    auto supervisor = MakeRunning(now);
    Require(supervisor.Tick(now, true, 7) == HostAction::None, "precondition: alive");

    now += HostSupervisor::kHeartbeatTimeout;
    Require(supervisor.Tick(now, true, 7) == HostAction::None,
            "the heartbeat is only stale once the timeout is exceeded");

    now += std::chrono::seconds(1);
    Require(supervisor.Tick(now, true, 7) == HostAction::Restart,
            "a frozen heartbeat is treated as death even while the process lives");
}

void TestRestartGraceSuppressesASecondJudgement() {
    auto now = Clock::time_point{};
    auto supervisor = MakeRunning(now);
    Require(supervisor.Tick(now, false, std::nullopt) == HostAction::Restart,
            "precondition: the first tick restarts");
    supervisor.NoteRestartResult(true, now);

    // 宿主要先加载厂商 DLL 再建映射，这段时间读不到心跳是正常的。
    now += std::chrono::seconds(1);
    Require(supervisor.Tick(now, true, std::nullopt) == HostAction::None,
            "a freshly restarted host is given time to publish");
    // 宽限期只压住重启判断。进程活着就照常算可用，否则托盘会在服务刚启动的头几秒
    // 把配件那几项灰掉。
    Require(supervisor.Healthy(), "a live host inside the grace period is still usable");
    Require(supervisor.Tick(now, false, std::nullopt) == HostAction::None,
            "but a host that did not come up is not restarted again inside the grace");
    Require(!supervisor.Healthy(), "and a host whose process is gone is never usable");

    now += HostSupervisor::kStartGrace;
    Require(supervisor.Tick(now, true, 1) == HostAction::None, "the host came back");
    Require(supervisor.Healthy(), "and is healthy once it publishes again");
}

void TestBudgetEndsInCooldown() {
    auto now = Clock::time_point{};
    auto supervisor = MakeRunning(now);

    for (int i = 0; i < HostSupervisor::kMaxRestartsPerWindow; ++i) {
        Require(supervisor.Tick(now, false, std::nullopt) == HostAction::Restart,
                "restarts inside the budget are attempted");
        supervisor.NoteRestartResult(true, now);
        now += HostSupervisor::kStartGrace + std::chrono::seconds(1);
    }

    Require(supervisor.Tick(now, false, std::nullopt) == HostAction::EnterCooldown,
            "the budget runs out and the supervisor stops trying");

    now += std::chrono::seconds(1);
    Require(supervisor.Tick(now, false, std::nullopt) == HostAction::None,
            "nothing is attempted during the cooldown");

    now += HostSupervisor::kRestartCooldown;
    Require(supervisor.Tick(now, false, std::nullopt) == HostAction::Restart,
            "the cooldown expires and the host is given another chance");
}

// 偶发崩溃彼此隔得足够远时不该累积，与 TouchProviderCoordinator 的窗口语义一致。
void TestBudgetIsPerWindow() {
    auto now = Clock::time_point{};
    auto supervisor = MakeRunning(now);

    for (int i = 0; i < HostSupervisor::kMaxRestartsPerWindow + 2; ++i) {
        Require(supervisor.Tick(now, false, std::nullopt) == HostAction::Restart,
                "crashes spread beyond the window are always restarted");
        supervisor.NoteRestartResult(true, now);
        now += HostSupervisor::kRestartWindow + std::chrono::seconds(1);
    }
}

void TestStoppedHostIsNotSupervised() {
    auto now = Clock::time_point{};
    auto supervisor = MakeRunning(now);
    supervisor.SetDesiredRunning(false, now);

    now += std::chrono::seconds(10);
    Require(supervisor.Tick(now, false, std::nullopt) == HostAction::None,
            "a host nobody asked for is not restarted");
    Require(!supervisor.Healthy(), "and is never reported healthy");
}

} // namespace

int main() {
    try {
        TestLiveHostIsLeftAlone();
        TestDeadProcessIsRestarted();
        TestFrozenHostIsRestarted();
        TestRestartGraceSuppressesASecondJudgement();
        TestBudgetEndsInCooldown();
        TestBudgetIsPerWindow();
        TestStoppedHostIsNotSupervised();
        std::cout << "[TEST] Host supervisor tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[TEST] " << error.what() << "\n";
        return 1;
    }
}
