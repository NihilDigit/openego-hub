#include "ColorPipeline.h"

#include <windows.h>

namespace Gaokun::Display {

namespace {

constexpr const wchar_t *kStateKey = L"Software\\gaokun-hal\\Display";

[[nodiscard]] bool ReadDword(HKEY key, const wchar_t *name, uint32_t &out) noexcept {
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(&value), &size) !=
        ERROR_SUCCESS) {
        return false;
    }
    if (type != REG_DWORD) return false;
    out = value;
    return true;
}

void WriteDword(HKEY key, const wchar_t *name, uint32_t value) noexcept {
    (void)RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value),
                         sizeof(value));
}

} // namespace

ColorState LoadColorState() noexcept {
    ColorState state;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStateKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return state;
    }

    uint32_t value = 0;
    if (ReadDword(key, L"TemperatureK", value)) state.temperatureK = static_cast<int>(value);
    if (ReadDword(key, L"EyeComfort", value)) state.eyeComfort = value != 0;
    if (ReadDword(key, L"NaturalColor", value)) state.naturalColor = value != 0;
    // 传感器色温以整数开尔文存放。这个量本身的精度远低于 1K，没有理由为它引入 REG_BINARY
    // 的浮点存储。
    if (ReadDword(key, L"SensorCct", value)) state.sensorCct = static_cast<double>(value);
    RegCloseKey(key);

    // 越界值一律当作关闭。注册表是用户可改的，读到 30000K 就照着外推会得到一张没有标定
    // 数据支撑的曲线。
    if (state.temperatureK != 0 &&
        (state.temperatureK < kTemperatureMinK || state.temperatureK > kTemperatureMaxK)) {
        state.temperatureK = 0;
    }
    return state;
}

bool StoreColorState(const ColorState &state) noexcept {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kStateKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    WriteDword(key, L"TemperatureK", static_cast<uint32_t>(state.temperatureK));
    WriteDword(key, L"EyeComfort", state.eyeComfort ? 1u : 0u);
    WriteDword(key, L"NaturalColor", state.naturalColor ? 1u : 0u);
    WriteDword(key, L"SensorCct", static_cast<uint32_t>(state.sensorCct < 0.0 ? 0.0
                                                                             : state.sensorCct));
    RegCloseKey(key);
    return true;
}

bool ApplyGain(Qdcm &qdcm, const std::vector<uint32_t> &targets, const RgbGain &gain) noexcept {
    float matrix[9];
    ToPccMatrix(gain, matrix);
    bool ok = true;
    for (uint32_t id : targets) {
        if (!qdcm.SetPcc(id, matrix)) ok = false;
    }
    return ok;
}

bool ApplyColorState(Qdcm &qdcm, const std::vector<uint32_t> &targets, const ColorState &state,
                     RgbGain *applied) noexcept {
    const RgbGain gain = Compose(state);
    if (applied) *applied = gain;
    return ApplyGain(qdcm, targets, gain);
}

} // namespace Gaokun::Display
