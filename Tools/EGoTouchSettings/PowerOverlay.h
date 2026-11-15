#pragma once

#include <windows.h>

#include <winrt/Windows.System.Power.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace PowerOverlay {

enum class Mode : uint8_t {
    BestEfficiency = 0,
    Balanced = 1,
    BestPerformance = 2,
};

struct State {
    bool supported = false;
    Mode mode = Mode::Balanced;
    bool energySaverActive = false;
};

struct EnergySaverState {
    bool supported = false;
    bool active = false;
    bool alwaysOn = false;
    uint32_t autoThresholdPercent = 20;  // 0（从不）, 10, 20, 30, 40, 50, 100（始终）
    uint32_t dcThresholdPercent = 20;    // 电源方案底层 DC 阈值（避免关闭“始终”时覆盖用户原阈值）
    bool lowerBrightness = false;        // ESBRIGHTNESS < 100
};

namespace detail {

constexpr GUID kBestEfficiencyGuid{
    0x961cc777, 0x2547, 0x4f9d, {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}};
constexpr GUID kBalancedGuid{
    0x00000000, 0x0000, 0x0000, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
constexpr GUID kBalancedLegacyGuid{
    0x381b4222, 0xf694, 0x41f0, {0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e}};
constexpr GUID kBestPerformanceGuid{
    0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}};
constexpr GUID kBestPerformanceLegacyGuid{
    0xe9a42b02, 0xd5df, 0x448d, {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};

// Windows SDK 公开电源设置 GUID（winnt.h）：节能模式子组、自动电量阈值与屏幕降亮。
constexpr GUID kEnergySaverSubgroupGuid{
    0xDE830923, 0xA562, 0x41AF, {0xA0, 0x86, 0xE3, 0xA2, 0xC6, 0xBA, 0xD2, 0xDA}};
constexpr GUID kEnergySaverBatteryThresholdGuid{
    0xE69653CA, 0xCF7F, 0x4F05, {0xAA, 0x73, 0xCB, 0x83, 0x3F, 0xA9, 0x0A, 0xD4}};
constexpr GUID kEnergySaverBrightnessGuid{
    0x13D09884, 0xF74E, 0x474A, {0xA8, 0x52, 0xB6, 0xBD, 0xE8, 0xAD, 0x03, 0xA8}};

struct WnfStateName {
    ULONG Data[2];
};

// Windows 11 24H2/26H2 节能模式 WNF 状态通道：
// - kWnfPoEnergySaverState: 内核发布的实时激活状态（接电/电池通用，首 DWORD bits 14..21 非零表示已开启）
// - kWnfPoEnergySaverOverride: 节能模式状态覆盖（0=使用电池低于阈值时手动关闭且接电时恢复为 2, 1=强制始终开启, 2=按电池阈值自动开启）
constexpr WnfStateName kWnfPoEnergySaverState{{0xA3BDF075u, 0x41840B3Eu}};
constexpr WnfStateName kWnfPoEnergySaverOverride{{0xA3BC3075u, 0x41C6013Du}};

inline bool GuidEquals(const GUID& a, const GUID& b) noexcept {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

inline uint32_t SnapThresholdStep(uint32_t value) noexcept {
    if (value == 0) return 0;
    if (value >= 100) return 100;
    constexpr std::array<uint32_t, 5> kSteps{10, 20, 30, 40, 50};
    uint32_t best = kSteps[0];
    uint32_t bestDist = value > best ? value - best : best - value;
    for (const uint32_t step : kSteps) {
        const uint32_t dist = value > step ? value - step : step - value;
        if (dist < bestDist) {
            best = step;
            bestDist = dist;
        }
    }
    return best;
}

struct Api {
    using GetActualFn = DWORD(WINAPI*)(GUID*);
    using GetEffectiveFn = DWORD(WINAPI*)(GUID*);
    using SetActiveFn = DWORD(WINAPI*)(const GUID*);
    using GetUserModeFn = DWORD(WINAPI*)(GUID*);
    using SetUserModeFn = DWORD(WINAPI*)(const GUID*);

    using GetActiveSchemeFn = DWORD(WINAPI*)(HKEY, GUID**);
    using SetActiveSchemeFn = DWORD(WINAPI*)(HKEY, const GUID*);
    using ReadValueIndexFn = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, LPDWORD);
    using WriteValueIndexFn = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, DWORD);

    using NtQueryWnfFn = LONG(NTAPI*)(
        const WnfStateName*, const void*, const void*, ULONG*, void*, ULONG*);
    using RtlPublishWnfFn = LONG(NTAPI*)(
        WnfStateName, const void*, const void*, ULONG, const void*);

    HMODULE module = nullptr;
    GetActualFn getActual = nullptr;
    GetEffectiveFn getEffective = nullptr;
    SetActiveFn setActive = nullptr;
    GetUserModeFn getUserAc = nullptr;
    GetUserModeFn getUserDc = nullptr;
    SetUserModeFn setUserAc = nullptr;
    SetUserModeFn setUserDc = nullptr;
    GetActiveSchemeFn getActiveScheme = nullptr;
    SetActiveSchemeFn setActiveScheme = nullptr;
    ReadValueIndexFn readAcValue = nullptr;
    ReadValueIndexFn readDcValue = nullptr;
    WriteValueIndexFn writeAcValue = nullptr;
    WriteValueIndexFn writeDcValue = nullptr;
    NtQueryWnfFn queryWnf = nullptr;
    RtlPublishWnfFn publishWnf = nullptr;
    bool loaded = false;

    static const Api& Instance() noexcept {
        static Api api = [] {
            Api res{};
            res.module = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (res.module) {
                res.getActual = reinterpret_cast<GetActualFn>(
                    GetProcAddress(res.module, "PowerGetActualOverlayScheme"));
                res.getEffective = reinterpret_cast<GetEffectiveFn>(
                    GetProcAddress(res.module, "PowerGetEffectiveOverlayScheme"));
                res.setActive = reinterpret_cast<SetActiveFn>(
                    GetProcAddress(res.module, "PowerSetActiveOverlayScheme"));
                res.getUserAc = reinterpret_cast<GetUserModeFn>(
                    GetProcAddress(res.module, "PowerGetUserConfiguredACPowerMode"));
                res.getUserDc = reinterpret_cast<GetUserModeFn>(
                    GetProcAddress(res.module, "PowerGetUserConfiguredDCPowerMode"));
                res.setUserAc = reinterpret_cast<SetUserModeFn>(
                    GetProcAddress(res.module, "PowerSetUserConfiguredACPowerMode"));
                res.setUserDc = reinterpret_cast<SetUserModeFn>(
                    GetProcAddress(res.module, "PowerSetUserConfiguredDCPowerMode"));
                res.getActiveScheme = reinterpret_cast<GetActiveSchemeFn>(
                    GetProcAddress(res.module, "PowerGetActiveScheme"));
                res.setActiveScheme = reinterpret_cast<SetActiveSchemeFn>(
                    GetProcAddress(res.module, "PowerSetActiveScheme"));
                res.readAcValue = reinterpret_cast<ReadValueIndexFn>(
                    GetProcAddress(res.module, "PowerReadACValueIndex"));
                res.readDcValue = reinterpret_cast<ReadValueIndexFn>(
                    GetProcAddress(res.module, "PowerReadDCValueIndex"));
                res.writeAcValue = reinterpret_cast<WriteValueIndexFn>(
                    GetProcAddress(res.module, "PowerWriteACValueIndex"));
                res.writeDcValue = reinterpret_cast<WriteValueIndexFn>(
                    GetProcAddress(res.module, "PowerWriteDCValueIndex"));
                res.loaded = (res.getActual != nullptr || res.getEffective != nullptr) &&
                             res.setActive != nullptr;
            }
            if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
                res.queryWnf = reinterpret_cast<NtQueryWnfFn>(
                    GetProcAddress(ntdll, "NtQueryWnfStateData"));
                res.publishWnf = reinterpret_cast<RtlPublishWnfFn>(
                    GetProcAddress(ntdll, "RtlPublishWnfStateData"));
            }
            return res;
        }();
        return api;
    }
};

inline bool IsEnergySaverActiveNow() noexcept {
    const auto& api = Api::Instance();
    if (api.queryWnf) {
        ULONG stamp = 0;
        std::array<uint32_t, 2> stateWords{};
        ULONG cb = static_cast<ULONG>(sizeof(stateWords));
        const LONG ntStatus = api.queryWnf(
            &kWnfPoEnergySaverState, nullptr, nullptr, &stamp, stateWords.data(), &cb);
        if (ntStatus >= 0 && cb >= sizeof(uint32_t)) {
            return (stateWords[0] & 0x003FC000u) != 0;
        }
    }
    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps) && (sps.SystemStatusFlag & 1) != 0) {
        return true;
    }
    try {
        return winrt::Windows::System::Power::PowerManager::EnergySaverStatus() ==
               winrt::Windows::System::Power::EnergySaverStatus::On;
    } catch (...) {
        return false;
    }
}

inline bool QueryEnergySaverOverride(DWORD& outVal) noexcept {
    const auto& api = Api::Instance();
    if (!api.queryWnf) return false;
    ULONG stamp = 0;
    DWORD val = 0;
    ULONG cb = sizeof(val);
    const LONG ntStatus = api.queryWnf(
        &kWnfPoEnergySaverOverride, nullptr, nullptr, &stamp, &val, &cb);
    if (ntStatus >= 0 && cb >= sizeof(DWORD)) {
        outVal = val;
        return true;
    }
    return false;
}

inline bool IsEnergySaverGroupPolicyForcedOn() noexcept {
    DWORD policyVal = 0;
    DWORD cb = sizeof(policyVal);
    const LSTATUS rc = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        L"Software\\Policies\\Microsoft\\Power\\EnergySaver",
        L"EnableEnergySaver",
        RRF_RT_REG_DWORD,
        nullptr,
        &policyVal,
        &cb);
    return rc == ERROR_SUCCESS && policyVal == 1u;
}

inline bool IsEnergySaverAlwaysOnNow() noexcept {
    if (IsEnergySaverGroupPolicyForcedOn()) {
        return true;
    }
    DWORD overrideVal = 0;
    return QueryEnergySaverOverride(overrideVal) && overrideVal == 1u;
}

inline void WaitEnergySaverState(bool expectedActive) noexcept {
    // RtlPublishWnfStateData 经 umpo 异步刷新内核状态，短暂轮询可避免调用方立即读到旧值。
    for (int i = 0; i < 30; ++i) {
        if (IsEnergySaverActiveNow() == expectedActive) return;
        Sleep(2);
    }
}

inline Mode ModeFromGuid(const GUID& guid) noexcept {
    if (GuidEquals(guid, kBestEfficiencyGuid)) return Mode::BestEfficiency;
    if (GuidEquals(guid, kBestPerformanceGuid) ||
        GuidEquals(guid, kBestPerformanceLegacyGuid)) {
        return Mode::BestPerformance;
    }
    if (GuidEquals(guid, kBalancedGuid) || GuidEquals(guid, kBalancedLegacyGuid)) {
        return Mode::Balanced;
    }
    return Mode::Balanced;
}

inline const GUID& GuidForMode(Mode mode) noexcept {
    switch (mode) {
    case Mode::BestEfficiency: return kBestEfficiencyGuid;
    case Mode::BestPerformance: return kBestPerformanceGuid;
    default: return kBalancedGuid;
    }
}

} // namespace detail

inline State Query() noexcept {
    State state{};
    state.energySaverActive = detail::IsEnergySaverActiveNow();

    const auto& api = detail::Api::Instance();
    if (!api.loaded) return state;

    GUID guid{};
    DWORD rc = ERROR_CALL_NOT_IMPLEMENTED;
    SYSTEM_POWER_STATUS sps{};
    const bool havePowerStatus = GetSystemPowerStatus(&sps) != FALSE;
    const bool acOnline = havePowerStatus ? (sps.ACLineStatus == 1) : true;
    if (!state.energySaverActive) {
        if (acOnline && api.getUserAc) {
            rc = api.getUserAc(&guid);
        } else if (!acOnline && api.getUserDc) {
            rc = api.getUserDc(&guid);
        }
    }
    if (rc != ERROR_SUCCESS && api.getActual) {
        rc = api.getActual(&guid);
    }
    if (rc != ERROR_SUCCESS && api.getEffective) {
        rc = api.getEffective(&guid);
    }
    if (rc != ERROR_SUCCESS) return state;

    state.supported = true;
    state.mode = detail::ModeFromGuid(guid);
    return state;
}

inline bool Apply(Mode mode) noexcept {
    const auto& api = detail::Api::Instance();
    if (!api.loaded || !api.setActive) return false;
    const GUID& target = detail::GuidForMode(mode);
    bool ok = false;
    SYSTEM_POWER_STATUS sps{};
    const bool acOnline = GetSystemPowerStatus(&sps) ? (sps.ACLineStatus == 1) : true;
    if (acOnline && api.setUserAc && api.setUserAc(&target) == ERROR_SUCCESS) {
        ok = true;
    } else if (!acOnline && api.setUserDc && api.setUserDc(&target) == ERROR_SUCCESS) {
        ok = true;
    }
    if (api.setActive(&target) == ERROR_SUCCESS) {
        ok = true;
    }
    return ok;
}

inline EnergySaverState QueryEnergySaver() noexcept {
    EnergySaverState state{};
    state.active = detail::IsEnergySaverActiveNow();
    state.alwaysOn = detail::IsEnergySaverAlwaysOnNow();

    const auto& api = detail::Api::Instance();
    if (api.getActiveScheme && api.readDcValue) {
        GUID* scheme = nullptr;
        if (api.getActiveScheme(nullptr, &scheme) == ERROR_SUCCESS && scheme != nullptr) {
            DWORD dcThreshold = 20;
            if (api.readDcValue(nullptr, scheme, &detail::kEnergySaverSubgroupGuid,
                                &detail::kEnergySaverBatteryThresholdGuid, &dcThreshold) == ERROR_SUCCESS) {
                state.supported = true;
                if (dcThreshold >= 100) {
                    state.alwaysOn = true;
                }
                state.dcThresholdPercent = detail::SnapThresholdStep(dcThreshold);
                state.autoThresholdPercent = state.dcThresholdPercent;
            }
            DWORD dcBrightness = 100;
            if (api.readDcValue(nullptr, scheme, &detail::kEnergySaverSubgroupGuid,
                                &detail::kEnergySaverBrightnessGuid, &dcBrightness) == ERROR_SUCCESS) {
                state.lowerBrightness = dcBrightness < 100;
            }
            LocalFree(scheme);
        }
    }
    if (state.alwaysOn) {
        state.autoThresholdPercent = 100;
    }
    return state;
}

inline bool SetEnergySaverActive(bool enabled) noexcept {
    if (!enabled && detail::IsEnergySaverGroupPolicyForcedOn()) {
        return false;
    }

    const auto& api = detail::Api::Instance();
    const EnergySaverState cur = QueryEnergySaver();
    const uint32_t effectiveDcThreshold = cur.dcThresholdPercent >= 100 ? 20u : cur.dcThresholdPercent;

    auto writeThresholds = [&](DWORD acVal, DWORD dcVal) -> bool {
        if (!api.getActiveScheme || !api.writeDcValue || !api.setActiveScheme) return false;
        GUID* scheme = nullptr;
        if (api.getActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return false;
        bool wrote = (api.writeDcValue(
                          nullptr, scheme, &detail::kEnergySaverSubgroupGuid,
                          &detail::kEnergySaverBatteryThresholdGuid, dcVal) == ERROR_SUCCESS);
        if (api.writeAcValue) {
            wrote = (api.writeAcValue(
                         nullptr, scheme, &detail::kEnergySaverSubgroupGuid,
                         &detail::kEnergySaverBatteryThresholdGuid, acVal) == ERROR_SUCCESS) || wrote;
        }
        if (wrote) {
            (void)api.setActiveScheme(nullptr, scheme);
        }
        LocalFree(scheme);
        return wrote;
    };

    if (!enabled && cur.dcThresholdPercent >= 100) {
        (void)writeThresholds(0u, 20u);
    }

    if (api.publishWnf) {
        SYSTEM_POWER_STATUS sps{};
        const bool haveSps = GetSystemPowerStatus(&sps) != FALSE;
        const bool onDcBelowThreshold =
            haveSps && sps.ACLineStatus == 0 && sps.BatteryLifePercent <= 100 &&
            effectiveDcThreshold > 0 && sps.BatteryLifePercent <= effectiveDcThreshold;

        DWORD overrideVal = 1u;
        if (enabled) {
            DWORD curOverride = 2u;
            if (onDcBelowThreshold && detail::QueryEnergySaverOverride(curOverride) && curOverride == 0u) {
                overrideVal = 2u;
            } else {
                overrideVal = 1u;
            }
        } else {
            overrideVal = onDcBelowThreshold ? 0u : 2u;
        }
        if (api.publishWnf(detail::kWnfPoEnergySaverOverride, nullptr,
                           &overrideVal, sizeof(overrideVal), nullptr) >= 0) {
            detail::WaitEnergySaverState(enabled);
            return true;
        }
    }

    const DWORD fallbackDc = enabled ? 100u : effectiveDcThreshold;
    const DWORD fallbackAc = enabled ? 100u : 0u;
    if (!writeThresholds(fallbackAc, fallbackDc)) return false;
    detail::WaitEnergySaverState(enabled);
    return true;
}

inline bool SetEnergySaverAutoThreshold(uint32_t thresholdPercent) noexcept {
    const auto& api = detail::Api::Instance();
    const uint32_t snapped = detail::SnapThresholdStep(thresholdPercent);

    if (snapped >= 100) {
        if (api.publishWnf) {
            const DWORD forceOn = 1u;
            if (api.publishWnf(detail::kWnfPoEnergySaverOverride, nullptr,
                               &forceOn, sizeof(forceOn), nullptr) >= 0) {
                detail::WaitEnergySaverState(true);
                return true;
            }
        }
        if (!api.getActiveScheme || !api.writeDcValue || !api.setActiveScheme) return false;
        GUID* scheme = nullptr;
        if (api.getActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return false;
        bool ok = api.writeDcValue(nullptr, scheme, &detail::kEnergySaverSubgroupGuid,
                                   &detail::kEnergySaverBatteryThresholdGuid, 100u) == ERROR_SUCCESS;
        if (api.writeAcValue) {
            ok = (api.writeAcValue(nullptr, scheme, &detail::kEnergySaverSubgroupGuid,
                                   &detail::kEnergySaverBatteryThresholdGuid, 100u) == ERROR_SUCCESS) && ok;
        }
        if (ok) ok = (api.setActiveScheme(nullptr, scheme) == ERROR_SUCCESS);
        LocalFree(scheme);
        if (ok) detail::WaitEnergySaverState(true);
        return ok;
    }

    if (!api.getActiveScheme || !api.writeDcValue || !api.setActiveScheme) return false;
    GUID* scheme = nullptr;
    if (api.getActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return false;
    bool ok = api.writeDcValue(nullptr, scheme, &detail::kEnergySaverSubgroupGuid,
                               &detail::kEnergySaverBatteryThresholdGuid, snapped) == ERROR_SUCCESS;
    if (api.writeAcValue) {
        (void)api.writeAcValue(nullptr, scheme, &detail::kEnergySaverSubgroupGuid,
                               &detail::kEnergySaverBatteryThresholdGuid, 0u);
    }
    if (ok) ok = (api.setActiveScheme(nullptr, scheme) == ERROR_SUCCESS);
    LocalFree(scheme);
    if (ok && api.publishWnf) {
        const DWORD autoPolicy = 2u;
        (void)api.publishWnf(detail::kWnfPoEnergySaverOverride, nullptr,
                             &autoPolicy, sizeof(autoPolicy), nullptr);
        SYSTEM_POWER_STATUS sps{};
        const bool expectActive =
            GetSystemPowerStatus(&sps) != FALSE && sps.ACLineStatus == 0 &&
            sps.BatteryLifePercent <= 100 && snapped > 0 &&
            sps.BatteryLifePercent <= snapped;
        detail::WaitEnergySaverState(expectActive);
    }
    return ok;
}

inline bool SetEnergySaverLowerBrightness(bool enabled) noexcept {
    const auto& api = detail::Api::Instance();
    if (!api.getActiveScheme || !api.writeDcValue || !api.setActiveScheme) return false;
    GUID* scheme = nullptr;
    if (api.getActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return false;
    const DWORD val = enabled ? 70u : 100u;
    bool ok = api.writeDcValue(nullptr, scheme, &detail::kEnergySaverSubgroupGuid,
                               &detail::kEnergySaverBrightnessGuid, val) == ERROR_SUCCESS;
    if (api.writeAcValue) {
        ok = (api.writeAcValue(nullptr, scheme, &detail::kEnergySaverSubgroupGuid,
                               &detail::kEnergySaverBrightnessGuid, val) == ERROR_SUCCESS) && ok;
    }
    if (ok) ok = (api.setActiveScheme(nullptr, scheme) == ERROR_SUCCESS);
    LocalFree(scheme);
    return ok;
}

} // namespace PowerOverlay
