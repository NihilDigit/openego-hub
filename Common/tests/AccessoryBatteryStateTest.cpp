#include "AccessoryBatteryState.h"

#include <iostream>
#include <stdexcept>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void TestBatteryValidity() {
    PenStatus::State state{};
    Require(!AccessoryBattery::HasPenBattery(state), "missing pen level");
    Require(!AccessoryBattery::HasKeyboardBattery(state), "missing keyboard level");
    state.hasBatteryLevel = state.hasKbdBattery = true;
    for (unsigned level = 0; level <= 255; ++level) {
        state.batteryLevel = state.kbdBatteryLevel = static_cast<uint8_t>(level);
        Require(AccessoryBattery::HasPenBattery(state) == (level <= 100),
                "pen percentage range including real zero");
        Require(AccessoryBattery::HasKeyboardBattery(state) == (level <= 100),
                "keyboard percentage range including real zero");
    }
}

void TestDisconnectedAccessories() {
    PenStatus::State state{};
    state.hasBatteryLevel = state.hasKbdBattery = true;
    state.batteryLevel = state.kbdBatteryLevel = 72;
    state.hasDeviceAttached = state.hasKbdPresent = true;
    Require(!AccessoryBattery::HasPenBattery(state), "detached pen stale level");
    Require(!AccessoryBattery::HasKeyboardBattery(state), "absent keyboard stale level");
    state.hasStylusLink = state.stylusLinked = true;
    Require(AccessoryBattery::HasPenBattery(state), "writing away from charger");
    state.stylusLinked = false;
    state.hasChargingState = state.charging = true;
    Require(AccessoryBattery::HasPenBattery(state), "charging before link event");
    state.kbdPresent = true;
    state.hasKbdDetached = state.kbdDetached = true;
    Require(AccessoryBattery::HasKeyboardBattery(state), "wireless keyboard still present");
}

void TestHostHealth() {
    PenStatus::State state{};
    state.hasBatteryLevel = state.hasKbdBattery = true;
    state.batteryLevel = state.kbdBatteryLevel = 72;
    Require(AccessoryBattery::HasPenBattery(state), "unknown pen health is not failure");
    Require(AccessoryBattery::HasKeyboardBattery(state), "unknown keyboard health is not failure");
    state.hasHostHealth = true;
    state.hasStylusLink = state.stylusLinked = true;
    state.hasKbdPresent = state.kbdPresent = true;
    Require(!AccessoryBattery::HasPenBattery(state), "dead pen host with stale connection");
    Require(!AccessoryBattery::HasKeyboardBattery(state), "dead keyboard host with stale connection");
    state.penHostHealthy = state.kbdHostHealthy = true;
    Require(AccessoryBattery::HasPenBattery(state), "pen host recovered");
    Require(AccessoryBattery::HasKeyboardBattery(state), "keyboard host recovered");
    state.penVendorMissing = state.kbdVendorMissing = true;
    Require(!AccessoryBattery::HasPenBattery(state), "missing pen vendor component");
    Require(!AccessoryBattery::HasKeyboardBattery(state), "missing keyboard vendor component");
}
} // namespace

int main() {
    try {
        TestBatteryValidity();
        TestDisconnectedAccessories();
        TestHostHealth();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
