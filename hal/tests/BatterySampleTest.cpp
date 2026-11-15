#include "GaokunPen.h"
#include "GaokunKeyboard.h"
#include "shared/BatterySample.h"

#include <cstdio>
#include <limits>

template <typename Snapshot, typename Flag>
bool CheckAccessory() {
    Snapshot state{};
    const auto batteryFlag = static_cast<uint32_t>(Flag::HasBattery);
    const auto unrelated = static_cast<uint32_t>(Flag::HasCharging) |
                           static_cast<uint32_t>(Flag::Charging) |
                           static_cast<uint32_t>(Flag::HasConnected);
    state.flags = unrelated;
    auto apply = [&](int value) {
        Gaokun::ApplyBatterySample(value, state.battery, state.flags, batteryFlag);
    };
    const int invalid[] = {-1, 101, 255, 256, std::numeric_limits<int>::min(),
                          std::numeric_limits<int>::max()};
    for (int value : invalid) {
        apply(73);
        if (state.battery != 73 || state.flags != (unrelated | batteryFlag)) return false;
        apply(value);
        if (state.battery != 0 || state.flags != unrelated) return false;
        apply(value);
        if (state.battery != 0 || state.flags != unrelated) return false;
        apply(100);
        if (state.battery != 100 || state.flags != (unrelated | batteryFlag)) return false;
    }
    for (int value = 0; value <= 100; ++value) {
        apply(value);
        if (state.battery != value || state.flags != (unrelated | batteryFlag)) return false;
    }
    return true;
}

int main() {
    const bool pen = CheckAccessory<Gaokun::Pen::Snapshot, Gaokun::Pen::Flag>();
    const bool keyboard = CheckAccessory<Gaokun::Keyboard::Snapshot, Gaokun::Keyboard::Flag>();
    std::printf("%s pen battery validity transitions\n", pen ? "PASS" : "FAIL");
    std::printf("%s keyboard battery validity transitions\n", keyboard ? "PASS" : "FAIL");
    return pen && keyboard ? 0 : 1;
}
