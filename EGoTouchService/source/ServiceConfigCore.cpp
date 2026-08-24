#include "ServiceConfigCore.h"

#include "config/ConfigBinder.h"
#include "config/ConfigStore.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>

namespace Service {

namespace {

std::string Normalize(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    for (char& ch : value) {
        if (ch == ' ' || ch == '+' || ch == '-') {
            ch = '_';
        }
    }
    while (value.find("__") != std::string::npos) {
        value.replace(value.find("__"), 2, "_");
    }
    return value;
}

PenButtonMode ParsePenButtonMode(const Config::ConfigStore& store, PenButtonMode fallback) {
    if (store.has("service.pen_button_mode")) {
        const auto value = store.get<Config::ConfigValue>("service.pen_button_mode");
        if (const auto parsed = ParsePenButtonModeValue(value)) {
            return *parsed;
        }
    }
    return fallback;
}

PenButtonRoute ParsePenButtonRoute(const Config::ConfigStore& store,
                                   PenButtonRoute fallback,
                                   bool& explicitRoute) {
    if (store.has("service.pen_button_route")) {
        explicitRoute = true;
        const auto value = store.get<Config::ConfigValue>("service.pen_button_route");
        if (const auto parsed = ParsePenButtonRouteValue(value)) {
            return *parsed;
        }
        explicitRoute = false;
    }
    return fallback;
}

} // namespace

void RegisterServiceConfigBindings(Config::ConfigBinder& binder, ServiceConfigState& state) {
    static const std::array<std::pair<ServiceMode, std::string>, 2> kModeMapping{{
        {ServiceMode::Full, "full"},
        {ServiceMode::TouchOnly, "touch_only"},
    }};

    constexpr auto runtimeBinding = Config::ConfigRuntimeBinding::ManualLiveApply;
    binder.bindEnum("service.mode", &ServiceConfigState::mode, state,
                    ServiceMode::Full, std::span<const std::pair<ServiceMode, std::string>>(kModeMapping), "Service runtime topology", runtimeBinding);
    binder.bind("service.auto_mode", &ServiceConfigState::autoMode, state,
                true, {}, "Enable automatic runtime start/init", runtimeBinding);
    binder.bind("service.stylus_vhf_enabled", &ServiceConfigState::stylusVhfEnabled, state,
                true, {}, "Enable stylus VHF output", runtimeBinding);
    binder.bindEnum("service.pen_button_mode", &ServiceConfigState::penButtonMode, state,
                    PenButtonMode::WindowsInk, PenButtonModeMapping(), "Pen button semantic mode", runtimeBinding);
    binder.bindEnum("service.pen_button_route", &ServiceConfigState::penButtonRoute, state,
                    PenButtonRoute::VhfOnly, PenButtonRouteMapping(), "Pen button injection route", runtimeBinding);
}

const char* ServiceModeToConfig(ServiceMode mode) {
    return mode == ServiceMode::Full ? "full" : "touch_only";
}

std::optional<PenButtonMode> ParsePenButtonModeValue(const Config::ConfigValue& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return PenButtonModeFromToken(Normalize(*text));
    }
    if (const auto* numeric = std::get_if<int32_t>(&value)) {
        return PenButtonModeFromNumeric(*numeric);
    }
    return std::nullopt;
}

std::optional<PenButtonRoute> ParsePenButtonRouteValue(const Config::ConfigValue& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        auto normalized = Normalize(*text);
        // 归一化把显示写法 "VHF + Win32" 压成 vhf_win32，而表里的规范名是 vhf_and_win32。
        // 这是唯一一个两边对不上的 token。
        if (normalized == "vhf_win32") {
            normalized = "vhf_and_win32";
        }
        return PenButtonRouteFromToken(normalized);
    }
    if (const auto* numeric = std::get_if<int32_t>(&value)) {
        return PenButtonRouteFromNumeric(*numeric);
    }
    return std::nullopt;
}

void ApplyLegacyServiceModeMigration(Config::ConfigStore& target, const Config::ConfigStore& source) {
    if (source.has("service.mode") || !source.has("service.mode.full")) {
        return;
    }

    target.set<std::string>(
        "service.mode",
        source.getOr<bool>("service.mode.full", true) ? "full" : "touch_only");
}

void ApplyConfig(ServiceConfigState& state, const Config::ConfigStore& store) {
    if (store.has("service.mode")) {
        const auto mode = Normalize(store.getOr<std::string>("service.mode", ServiceModeToConfig(state.mode)));
        if (mode == "full") {
            state.mode = ServiceMode::Full;
        } else if (mode == "touch_only") {
            state.mode = ServiceMode::TouchOnly;
        }
    } else if (store.has("service.mode.full")) {
        state.mode = store.getOr<bool>("service.mode.full", state.mode == ServiceMode::Full)
            ? ServiceMode::Full
            : ServiceMode::TouchOnly;
    }

    state.autoMode = store.getOr<bool>("service.auto_mode", state.autoMode);
    state.stylusVhfEnabled = store.getOr<bool>("service.stylus_vhf_enabled", state.stylusVhfEnabled);
    state.penButtonMode = ParsePenButtonMode(store, state.penButtonMode);
    state.penButtonRoute = ParsePenButtonRoute(store, state.penButtonRoute, state.penButtonRouteExplicit);
}

ReloadServiceConfigResult DiffServiceConfig(const ServiceConfigState& current,
                                            const ServiceConfigState& reloaded,
                                            bool runtimeAvailable) {
    ReloadServiceConfigResult result{};

    const bool modeChanged = (current.mode != reloaded.mode);
    const bool autoModeChanged = (current.autoMode != reloaded.autoMode);
    const bool stylusVhfChanged = (current.stylusVhfEnabled != reloaded.stylusVhfEnabled);
    const bool penButtonModeChanged = (current.penButtonMode != reloaded.penButtonMode);
    const bool penButtonRouteChanged =
        (current.penButtonRoute != reloaded.penButtonRoute) ||
        (current.penButtonRouteExplicit != reloaded.penButtonRouteExplicit);

    if (modeChanged) {
        result.changedFields |= ToServiceConfigFieldBit(ServiceConfigField::Mode);
        result.restartRequiredFields |= ToServiceConfigFieldBit(ServiceConfigField::Mode);
    }
    if (autoModeChanged) {
        result.changedFields |= ToServiceConfigFieldBit(ServiceConfigField::AutoMode);
    }
    if (stylusVhfChanged) {
        result.changedFields |= ToServiceConfigFieldBit(ServiceConfigField::StylusVhfEnabled);
    }
    if (penButtonModeChanged) {
        result.changedFields |= ToServiceConfigFieldBit(ServiceConfigField::PenButtonMode);
    }
    if (penButtonRouteChanged) {
        result.changedFields |= ToServiceConfigFieldBit(ServiceConfigField::PenButtonRoute);
    }

    if (runtimeAvailable) {
        result.appliedFields |= static_cast<uint8_t>(
            (autoModeChanged ? ToServiceConfigFieldBit(ServiceConfigField::AutoMode) : 0u) |
            (stylusVhfChanged ? ToServiceConfigFieldBit(ServiceConfigField::StylusVhfEnabled) : 0u) |
            (penButtonModeChanged ? ToServiceConfigFieldBit(ServiceConfigField::PenButtonMode) : 0u) |
            (penButtonRouteChanged ? ToServiceConfigFieldBit(ServiceConfigField::PenButtonRoute) : 0u));
    }

    return result;
}

} // namespace Service
