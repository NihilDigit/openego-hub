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

} // namespace

int main() {
    try {
        TestAcquireStartsEGoBeforeDisablingHuawei();
        TestStartFailureRestoresHuawei();
        TestDisableFailureStopsEGoAndRestoresHuawei();
        TestLeaseTimeoutReturnsToHuawei();
        TestHuaweiFailureRollsBackToEGo();
        std::cout << "[TEST] Touch provider coordinator tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[TEST] " << error.what() << "\n";
        return 1;
    }
}
