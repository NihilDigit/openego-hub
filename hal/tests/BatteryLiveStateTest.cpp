#include <windows.h>
#include <powrprof.h>
#include <cstdio>
#include <initializer_list>

namespace {
SYSTEM_BATTERY_STATE battery{};
SYSTEM_POWER_STATUS fakePower{};
NTSTATUS batteryResult = 0;
BOOL powerResult = TRUE;

NTSTATUS WINAPI ReadBattery(POWER_INFORMATION_LEVEL, PVOID, ULONG, PVOID out, ULONG) {
    *static_cast<SYSTEM_BATTERY_STATE*>(out) = battery;
    return batteryResult;
}
BOOL WINAPI ReadPower(LPSYSTEM_POWER_STATUS out) {
    *out = fakePower;
    return powerResult;
}
}

// Exercise the production reader with deterministic OS responses; idle samples avoid IOCTLs.
#define CallNtPowerInformation ReadBattery
#define GetSystemPowerStatus ReadPower
#include "../src/power/BatteryInfo.cpp"
#undef GetSystemPowerStatus
#undef CallNtPowerInformation

int main() {
    using namespace Gaokun::Power;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    battery.BatteryPresent = TRUE;
    battery.MaxCapacity = 40000;
    battery.RemainingCapacity = 24000;
    LiveState live;
    for (unsigned value : {0u, 1u, 59u, 100u}) {
        fakePower.BatteryLifePercent = static_cast<BYTE>(value);
        check(ReadLiveState(live) == Result::Ok && live.percent == value,
              "system percent preserved even when capacity ratio differs");
    }
    for (unsigned value : {101u, 254u, 255u}) {
        fakePower.BatteryLifePercent = static_cast<BYTE>(value);
        check(ReadLiveState(live) == Result::Ok && live.percent == kPercentUnknown,
              "invalid or unknown system percent does not become capacity estimate");
    }
    powerResult = FALSE;
    fakePower.BatteryLifePercent = 80;
    check(ReadLiveState(live) == Result::Ok && live.percent == kPercentUnknown,
          "failed API cannot leak plausible output or previous percent");
    battery.MaxCapacity = 1;
    battery.RemainingCapacity = 0xffffffff;
    check(ReadLiveState(live) == Result::Ok && live.percent == kPercentUnknown,
          "invalid capacities cannot wrap into a plausible percent");
    powerResult = TRUE;
    fakePower.BatteryLifePercent = 42;
    battery.MaxCapacity = 0;
    check(ReadLiveState(live) == Result::Ok && live.percent == 42,
          "system percent remains usable without capacity");
    battery.BatteryPresent = FALSE;
    check(ReadLiveState(live) == Result::NoBattery && !live.batteryPresent &&
          live.percent == kPercentUnknown, "missing battery clears previous sample");
    batteryResult = static_cast<NTSTATUS>(0xc0000001);
    check(ReadLiveState(live) == Result::Failed && live.percent == kPercentUnknown,
          "failed battery query clears previous sample");
    return failures ? 1 : 0;
}
