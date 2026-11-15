#include "GaokunPower.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <winrt/Windows.System.Power.h>

#pragma comment(lib, "windowsapp.lib")

int main(int argc, char** argv) {
    using namespace Gaokun::Power;
    auto parse = [](const char* text, long maximum) {
        char* end = nullptr;
        const long value = std::strtol(text, &end, 10);
        return end != text && *end == '\0' && value > 0 && value <= maximum ? value : -1;
    };
    const long samples = argc > 1 ? parse(argv[1], 3600) : 5;
    const long intervalMs = argc > 2 ? parse(argv[2], 60000) : 1000;
    if (argc > 3 || samples < 0 || intervalMs < 0) {
        std::fprintf(stderr, "usage: BatteryLiveDiagnostic [samples 1..3600] [interval_ms 1..60000]\n");
        return 2;
    }
    winrt::init_apartment();
    int failures = 0;
    int compared = 0;
    for (int sample = 0; sample < samples; ++sample) {
        SYSTEM_POWER_STATUS before{}, after{};
        const bool beforeOk = GetSystemPowerStatus(&before) != FALSE;
        LiveState live;
        const Result result = ReadLiveState(live);
        const bool afterOk = GetSystemPowerStatus(&after) != FALSE;
        int winrtPercent = -1;
        try {
            winrtPercent = winrt::Windows::System::Power::PowerManager::RemainingChargePercent();
        } catch (const winrt::hresult_error& error) {
            std::printf("WinRT error=0x%08x\n", static_cast<unsigned>(error.code().value));
        }
        SYSTEMTIME utc{};
        GetSystemTime(&utc);
        const double ratio = live.fullChargedCapacityMWh
            ? 100.0 * live.remainingCapacityMWh / live.fullChargedCapacityMWh : -1.0;
        std::printf("utc=%04u-%02u-%02uT%02u:%02u:%02u.%03uZ sample=%d result=%u "
                    "api_before=%u/%u hal_percent=%u api_after=%u/%u "
                    "winrt_percent=%d capacity_percent=%.3f capacity_mWh=%u/%u "
                    "ac=%d charging=%d discharging=%d\n",
                    utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond,
                    utc.wMilliseconds, sample, static_cast<unsigned>(result), beforeOk,
                    before.BatteryLifePercent, live.percent, afterOk,
                    after.BatteryLifePercent, winrtPercent, ratio, live.remainingCapacityMWh,
                    live.fullChargedCapacityMWh, live.acOnline, live.charging, live.discharging);
        // Only compare stable, known bracketing samples; a changing battery is not a test failure.
        if (beforeOk && afterOk && before.BatteryLifePercent <= 100 &&
            before.BatteryLifePercent == after.BatteryLifePercent) {
            ++compared;
            if (result != Result::Ok || live.percent != before.BatteryLifePercent) ++failures;
        }
        if (sample + 1 < samples) Sleep(static_cast<DWORD>(intervalMs));
    }
    std::printf("stable_samples=%d stable_sample_mismatches=%d (read-only diagnostic)\n",
                compared, failures);
    return failures ? 1 : 0;
}
