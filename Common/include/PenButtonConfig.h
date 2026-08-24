#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

/// 笔按键语义模式 — 决定双击触控手势映射到什么行为
enum class PenButtonMode : uint8_t {
    OemCustom    = 0,  ///< OEM 自定义按键码 (VHF HID keycode 0x14B)
    NativeBarrel = 1,  ///< 原生笔杆按键 (Win32 PEN_FLAG_BARREL)
    NativeEraser = 2,  ///< 原生橡皮擦   (Win32 PEN_FLAG_ERASER)
    WindowsInk   = 3,  ///< 交给系统笔菜单 (Win+F19 → ClickNote)
    // 双击在正常书写与橡皮擦之间切换。整条链路都在服务内闭环——翻转的是 VHF 报文里的
    // 橡皮擦位，不依赖用户会话里的任何进程，托盘没跑也照样生效。
    ToggleEraser = 4,
};

/// 笔按键注入路由 — 决定哪些后端参与按键注入
enum class PenButtonRoute : uint8_t {
    VhfOnly     = 0,  ///< 仅 VHF 注入
    Win32Only   = 1,  ///< 仅 Win32 虚拟笔 API 注入
    VhfAndWin32 = 2,  ///< VHF + Win32 双路由（诊断用）
};

inline const char* ToString(PenButtonMode m) {
    switch (m) {
    case PenButtonMode::OemCustom:    return "OEM Custom";
    case PenButtonMode::NativeBarrel: return "Native Barrel";
    case PenButtonMode::NativeEraser: return "Native Eraser";
    case PenButtonMode::WindowsInk:   return "Windows Ink";
    case PenButtonMode::ToggleEraser: return "Toggle Eraser";
    default:                          return "Unknown";
    }
}

inline const char* ToString(PenButtonRoute r) {
    switch (r) {
    case PenButtonRoute::VhfOnly:     return "VHF Only";
    case PenButtonRoute::Win32Only:   return "Win32 Only";
    case PenButtonRoute::VhfAndWin32: return "VHF + Win32";
    default:                          return "Unknown";
    }
}

// 枚举与配置 token 的唯一映射表。服务端 binder、App 侧 proxy、SchemaValidator 和所有
// 解析函数都从这里取。
//
// 之前每处各抄一份，且数值路径手写成 0..2：新增 WindowsInk(3) 后，配置里写 3 能过
// SchemaValidator（它已改为查 binder 的 enumMapping），却在 ParsePenButtonModeValue 里
// 落到 nullopt 被静默换成默认值——「校验通过」与「实际生效」不是同一张表的后果。
// 数值合法域现在也由这张表决定，不再有手写上限。
//
// 用 std::string 而不是 string_view，是因为 ConfigBinder::bindEnum 的签名要的是
// std::pair<T, std::string>；换成 string_view 要动整个 binder。
inline std::span<const std::pair<PenButtonMode, std::string>> PenButtonModeMapping() {
    static const std::array<std::pair<PenButtonMode, std::string>, 5> kMapping{{
        {PenButtonMode::OemCustom,    "oem_custom"},
        {PenButtonMode::NativeBarrel, "native_barrel"},
        {PenButtonMode::NativeEraser, "native_eraser"},
        {PenButtonMode::WindowsInk,   "windows_ink"},
        {PenButtonMode::ToggleEraser, "toggle_eraser"},
    }};
    return kMapping;
}

inline std::span<const std::pair<PenButtonRoute, std::string>> PenButtonRouteMapping() {
    static const std::array<std::pair<PenButtonRoute, std::string>, 3> kMapping{{
        {PenButtonRoute::VhfOnly,     "vhf_only"},
        {PenButtonRoute::Win32Only,   "win32_only"},
        {PenButtonRoute::VhfAndWin32, "vhf_and_win32"},
    }};
    return kMapping;
}

/// 规范 token → 枚举。传入的必须是已归一化的 token（小写、分隔符统一为下划线）。
inline std::optional<PenButtonMode> PenButtonModeFromToken(std::string_view token) {
    for (const auto& [mode, name] : PenButtonModeMapping()) {
        if (name == token) return mode;
    }
    return std::nullopt;
}

inline std::optional<PenButtonRoute> PenButtonRouteFromToken(std::string_view token) {
    for (const auto& [route, name] : PenButtonRouteMapping()) {
        if (name == token) return route;
    }
    return std::nullopt;
}

/// 数值 → 枚举。合法域就是映射表里出现过的数值，没有手写上限。
inline std::optional<PenButtonMode> PenButtonModeFromNumeric(int32_t value) {
    for (const auto& [mode, name] : PenButtonModeMapping()) {
        if (static_cast<int32_t>(mode) == value) return mode;
    }
    return std::nullopt;
}

inline std::optional<PenButtonRoute> PenButtonRouteFromNumeric(int32_t value) {
    for (const auto& [route, name] : PenButtonRouteMapping()) {
        if (static_cast<int32_t>(route) == value) return route;
    }
    return std::nullopt;
}

/// 枚举 → 规范 token。表里必然命中，兜底只为满足返回类型。
inline const char* ToPenButtonModeToken(PenButtonMode mode) {
    for (const auto& [candidate, name] : PenButtonModeMapping()) {
        if (candidate == mode) return name.c_str();
    }
    return "windows_ink";
}

inline const char* ToPenButtonRouteToken(PenButtonRoute route) {
    for (const auto& [candidate, name] : PenButtonRouteMapping()) {
        if (candidate == route) return name.c_str();
    }
    return "vhf_only";
}
