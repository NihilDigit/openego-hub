#pragma once

#include "PenButtonConfig.h"
#include "config/ConfigValue.h"

#include <cstdint>
#include <optional>

namespace Config {
class ConfigBinder;
class ConfigStore;
}

namespace Service {

/// Service runtime topology selected by [Service].mode.
enum class ServiceMode {
    Full,
    TouchOnly,
};

struct ServiceConfigState {
    ServiceMode mode = ServiceMode::Full;
    bool autoMode = true;
    bool stylusVhfEnabled = true;
    PenButtonMode penButtonMode = PenButtonMode::WindowsInk;
    PenButtonRoute penButtonRoute = PenButtonRoute::VhfOnly;
    bool penButtonRouteExplicit = false;
    // 关掉之后只是不再按周期自动查更新；托盘送来的 CheckNow 仍然照做——用户点了「检查
    // 更新」还不查，那个开关就不是「自动」的意思了。
    bool autoUpdateCheck = true;
};

struct ReloadServiceConfigResult {
    uint8_t changedFields = 0;
    uint8_t appliedFields = 0;
    uint8_t restartRequiredFields = 0;
};

enum class ServiceConfigField : uint8_t {
    Mode = 0,
    AutoMode = 1,
    StylusVhfEnabled = 2,
    PenButtonMode = 3,
    PenButtonRoute = 4,
    AutoUpdateCheck = 5,
};

constexpr uint8_t ToServiceConfigFieldBit(ServiceConfigField field) {
    return static_cast<uint8_t>(1u << static_cast<uint8_t>(field));
}

void RegisterServiceConfigBindings(Config::ConfigBinder& binder, ServiceConfigState& state);
const char* ServiceModeToConfig(ServiceMode mode);
std::optional<PenButtonMode> ParsePenButtonModeValue(const Config::ConfigValue& value);
std::optional<PenButtonRoute> ParsePenButtonRouteValue(const Config::ConfigValue& value);
void ApplyConfig(ServiceConfigState& state, const Config::ConfigStore& store);
void ApplyLegacyServiceModeMigration(Config::ConfigStore& target, const Config::ConfigStore& source);
ReloadServiceConfigResult DiffServiceConfig(const ServiceConfigState& current,
                                            const ServiceConfigState& reloaded,
                                            bool runtimeAvailable);

} // namespace Service
