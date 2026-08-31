// gaokun-power 的命令行。三个子命令按代价分开，与 GaokunPower.h 的两组接口对应：
// --info 会付一次 30 ms 量级的 ACPI 事务，--query 走 WMI 且需要管理员权限。

#include "GaokunPower.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

#include "power/ChargeLimit.h"
#include "power/OemWmi.h"

namespace {

using namespace Gaokun::Power;

void TraceStep(const wchar_t *what, HRESULT hr) noexcept {
    wprintf(L"  %-24ls 0x%08lx\n", what, static_cast<unsigned long>(hr));
}

void ReportFailure(const wchar_t *what, Result result, HRESULT failure) noexcept {
    wprintf(L"%ls failed: %ls (0x%08lx)\n", what, ToString(result),
            static_cast<unsigned long>(failure));
    if (result == Result::AccessDenied) {
        wprintf(L"this needs an elevated process.\n");
    }
}

[[nodiscard]] int PrintThreshold() {
    ChargeThreshold threshold;
    HRESULT failure = S_OK;
    const Result result = Detail::ReadChargeThreshold(threshold, failure);
    if (result != Result::Ok) {
        ReportFailure(L"reading the charge threshold", result, failure);
        return 1;
    }

    wprintf(L"charge   : start %u%%, stop %u%%\n", threshold.startPercent,
            threshold.stopPercent);
    // 智能模式下阈值要等连续接电满 delay 小时才生效，在那之前照常充到 100%。只印百分比
    // 会让人以为设置没生效。
    wprintf(L"mode     : %u (%ls), delay %u h\n", threshold.mode,
            threshold.IsManual() ? L"manual, enforced now"
                                 : L"smart charging, enforced after the delay on AC",
            threshold.delay);
    return 0;
}

[[nodiscard]] int PrintInfo() {
    LiveState live;
    const Result liveResult = ReadLiveState(live);
    if (liveResult != Result::Ok) {
        wprintf(L"live state failed: %ls\n", ToString(liveResult));
        return 1;
    }

    wprintf(L"percent  : %u%%\n", live.percent);
    wprintf(L"remaining: %lu mWh of %lu mWh\n",
            static_cast<unsigned long>(live.remainingCapacityMWh),
            static_cast<unsigned long>(live.fullChargedCapacityMWh));
    wprintf(L"ac line  : %ls\n", live.acOnline ? L"online" : L"offline");
    wprintf(L"flow     : %ls %ld mW\n",
            live.charging ? L"charging" : (live.discharging ? L"discharging" : L"idle"),
            static_cast<long>(live.powerMilliWatt));
    // 印出量的是哪一头。放电时是固件报的可用时间，充电时是本层算出的充满时间，两者不是
    // 同一个量，只印分钟数会看不出来。
    switch (live.remainingKind) {
    case TimeKind::ToEmpty:
        wprintf(L"runtime  : %lu min to empty\n",
                static_cast<unsigned long>(live.remainingSeconds / 60));
        break;
    case TimeKind::ToFull:
        wprintf(L"runtime  : %lu min to full (computed)\n",
                static_cast<unsigned long>(live.remainingSeconds / 60));
        break;
    case TimeKind::Unknown:
        wprintf(L"runtime  : unknown\n");
        break;
    }

    BatteryInfo info;
    const Result infoResult = ReadBatteryInfo(info);
    if (infoResult != Result::Ok) {
        wprintf(L"battery info failed: %ls\n", ToString(infoResult));
        return 1;
    }

    wprintf(L"model    : %hs\n", info.deviceName);
    wprintf(L"vendor   : %hs\n", info.manufacturer);
    wprintf(L"serial   : %hs\n", info.serialNumber);
    wprintf(L"chemistry: %hs\n", info.chemistry);
    wprintf(L"design   : %lu mWh\n", static_cast<unsigned long>(info.designCapacityMWh));
    wprintf(L"full     : %lu mWh\n", static_cast<unsigned long>(info.fullChargedCapacityMWh));
    wprintf(L"health   : %.1f%%\n", HealthPercent(info));
    wprintf(L"cycles   : %lu\n", static_cast<unsigned long>(info.cycleCount));
    wprintf(L"voltage  : %lu mV\n", static_cast<unsigned long>(info.voltageMilliVolt));

    // 阈值读不回来不算 --info 失败：它需要管理员权限，其余各项不需要。
    ChargeThreshold threshold;
    HRESULT failure = S_OK;
    const Result thresholdResult = Detail::ReadChargeThreshold(threshold, failure);
    if (thresholdResult == Result::Ok) {
        wprintf(L"limit    : start %u%%, stop %u%%, mode %u (%ls)\n", threshold.startPercent,
                threshold.stopPercent, threshold.mode,
                threshold.IsManual() ? L"manual" : L"smart charging");
        // 充电会在停充阈值停下，所以真正会兑现的是充到阈值的时间，不是上面那行 runtime 里
        // 的充满时间。上层拿得到阈值时应当照这样显示。
        const uint32_t toLimit = SecondsToPercent(live, threshold.stopPercent);
        if (toLimit != kSecondsUnknown) {
            wprintf(L"to limit : %lu min\n", static_cast<unsigned long>(toLimit / 60));
        }
    } else {
        wprintf(L"limit    : %ls\n", ToString(thresholdResult));
    }
    return 0;
}

[[nodiscard]] int SetLimit(const wchar_t *argument, bool dryRun) {
    const int requested = _wtoi(argument);
    if (requested < kMinChargeLimit || requested > kMaxChargeLimit) {
        wprintf(L"limit must be between %d and %d\n", kMinChargeLimit, kMaxChargeLimit);
        return 1;
    }

    if (dryRun) {
        Oem::SetTrace(TraceStep);
        wprintf(L"request  : %02x %02x %02x %02x %02x %02x (start %d%%, stop %d%%)\n", 0x03,
                0x15, 0x01, 0x18, requested - kChargeStartOffset, requested,
                requested - kChargeStartOffset, requested);
    }

    HRESULT failure = S_OK;
    const Result result = Detail::SetChargeLimit(requested, dryRun, failure);
    Oem::SetTrace(nullptr);
    if (result != Result::Ok) {
        ReportFailure(L"setting the charge limit", result, failure);
        return 1;
    }

    if (dryRun) {
        wprintf(L"dry run: everything resolved, ExecMethod not issued\n");
    } else {
        wprintf(L"charge limit set: start %d%%, stop %d%%\n", requested - kChargeStartOffset,
                requested);
    }
    return 0;
}

int SetSmart(bool dryRun) {
    if (dryRun) Oem::SetTrace(TraceStep);

    HRESULT failure = S_OK;
    const Result result = Detail::SetSmartCharge(dryRun, failure);
    Oem::SetTrace(nullptr);
    if (result != Result::Ok) {
        ReportFailure(L"handing charging back to the vendor", result, failure);
        return 1;
    }

    if (dryRun) {
        wprintf(L"dry run: everything resolved, ExecMethod not issued\n");
    } else {
        wprintf(L"smart charging set: %u%%/%u%% after %u h on AC\n",
                kSmartChargeStartPercent, kSmartChargeStopPercent, kSmartChargeDelayHours);
    }
    return 0;
}

void PrintUsage() {
    wprintf(L"gaokun-power -- battery information for the MateBook E Go\n\n"
            L"  --info             print everything, including the current threshold\n"
            L"  --query            print the charge threshold and its mode\n"
            L"  --limit <%d-%d>   stop charging at the given percentage\n"
            L"  --smart            switch to smart charging (%d%%/%d%% after %d h on AC)\n\n"
            L"Charging resumes %d points below the limit, as the vendor tool does.\n"
            L"Smart charging holds the battery at its threshold only once the machine has\n"
            L"been on AC for %d hours without interruption; before that it charges to\n"
            L"100%%. The EC enforces both modes on its own.\n"
            L"--query, --limit and --smart need an elevated process; --info degrades\n"
            L"without one.\n",
            kMinChargeLimit, kMaxChargeLimit, kSmartChargeStartPercent,
            kSmartChargeStopPercent, kSmartChargeDelayHours, kChargeStartOffset,
            kSmartChargeDelayHours);
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc >= 2 && _wcsicmp(argv[1], L"--info") == 0) return PrintInfo();
    if (argc >= 2 && _wcsicmp(argv[1], L"--query") == 0) return PrintThreshold();
    if (argc >= 3 && _wcsicmp(argv[1], L"--limit") == 0) {
        const bool dryRun = argc >= 4 && _wcsicmp(argv[3], L"--dry-run") == 0;
        return SetLimit(argv[2], dryRun);
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"--smart") == 0) {
        const bool dryRun = argc >= 3 && _wcsicmp(argv[2], L"--dry-run") == 0;
        return SetSmart(dryRun);
    }

    PrintUsage();
    return 1;
}
