#include "PenButtonConfig.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void RequireString(const char* actual, std::string_view expected, const char* message) {
    Require(std::string_view(actual) == expected, message);
}

void TestPenButtonModeStrings() {
    RequireString(ToString(PenButtonMode::OemCustom), "OEM Custom", "OemCustom should map to OEM Custom");
    RequireString(ToString(PenButtonMode::NativeBarrel), "Native Barrel", "NativeBarrel should map to Native Barrel");
    RequireString(ToString(PenButtonMode::NativeEraser), "Native Eraser", "NativeEraser should map to Native Eraser");
    RequireString(ToString(static_cast<PenButtonMode>(0x7F)), "Unknown", "Invalid PenButtonMode should map to Unknown");
}

void TestPenButtonRouteStrings() {
    RequireString(ToString(PenButtonRoute::VhfOnly), "VHF Only", "VhfOnly should map to VHF Only");
    RequireString(ToString(PenButtonRoute::Win32Only), "Win32 Only", "Win32Only should map to Win32 Only");
    RequireString(ToString(PenButtonRoute::VhfAndWin32), "VHF + Win32", "VhfAndWin32 should map to VHF + Win32");
    RequireString(ToString(static_cast<PenButtonRoute>(0x7F)), "Unknown", "Invalid PenButtonRoute should map to Unknown");
}

void TestEnumStorageValues() {
    Require(static_cast<uint8_t>(PenButtonMode::OemCustom) == 0, "OemCustom storage value should remain 0");
    Require(static_cast<uint8_t>(PenButtonMode::NativeBarrel) == 1, "NativeBarrel storage value should remain 1");
    Require(static_cast<uint8_t>(PenButtonMode::NativeEraser) == 2, "NativeEraser storage value should remain 2");
    Require(static_cast<uint8_t>(PenButtonRoute::VhfOnly) == 0, "VhfOnly storage value should remain 0");
    Require(static_cast<uint8_t>(PenButtonRoute::Win32Only) == 1, "Win32Only storage value should remain 1");
    Require(static_cast<uint8_t>(PenButtonRoute::VhfAndWin32) == 2, "VhfAndWin32 storage value should remain 2");
}

// 映射表是解析、校验、binder 和托盘菜单共用的唯一真相，所以它自身的两条不变量要守住：
// 下标即枚举值（诊断面板的 Combo 直接把下标当模式用），以及 token 与数值双向可逆
// （配置里写 token 和写数字必须落到同一个模式上）。
void TestPenButtonModeMappingIsTheSingleSource() {
    int32_t expectedValue = 0;
    for (const auto& [mode, token] : PenButtonModeMapping()) {
        Require(static_cast<int32_t>(mode) == expectedValue,
                "PenButtonModeMapping must stay ordered by enum value with no gaps");
        Require(PenButtonModeFromToken(token) == mode,
                "every mapped token must parse back to its own mode");
        Require(PenButtonModeFromNumeric(static_cast<int32_t>(mode)) == mode,
                "every mapped numeric value must parse back to its own mode");
        RequireString(ToPenButtonModeToken(mode), token,
                      "ToPenButtonModeToken must return the mapped token");
        ++expectedValue;
    }

    Require(PenButtonModeFromToken("toggle_eraser") == PenButtonMode::ToggleEraser,
            "toggle_eraser is the configuration token for ToggleEraser");
    Require(!PenButtonModeFromNumeric(expectedValue).has_value(),
            "a value past the end of the mapping must be rejected, not clamped");
    Require(!PenButtonModeFromToken("windows ink").has_value(),
            "unnormalized text is the caller's job to normalize first");
}

} // namespace

int main() {
    try {
        TestPenButtonModeStrings();
        TestPenButtonRouteStrings();
        TestEnumStorageValues();
        TestPenButtonModeMappingIsTheSingleSource();
        std::cout << "[TEST] CommonPenButtonConfigTest passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] CommonPenButtonConfigTest failed: " << ex.what() << '\n';
        return 1;
    }
}
