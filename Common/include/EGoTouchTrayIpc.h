#pragma once

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace EGoTouchTrayIpc {

inline constexpr wchar_t kTrayWindowClass[] = L"OpenEGoHubTrayPanel";
inline constexpr wchar_t kSettingsBridgeWindowClass[] = L"OpenEGoHubSettingsBridge";
inline constexpr wchar_t kSettingsMutexName[] = L"Local\\OpenEGoHubSettingsSingleton";

// Both processes live in the same interactive session. Keeping the command surface on
// the tray HWND preserves PenControlChannel's single-writer invariant: the WinUI process
// never writes the service's shared command mapping directly.
inline constexpr UINT kCommandMessage = WM_APP + 20;
inline constexpr UINT kActivateMessage = WM_APP + 21;
inline constexpr UINT kNotificationMessage = WM_APP + 22;

enum class Notification : uint32_t {
    PenConnected = 1,
    PenDeviation,
    // 只追加，不能插入：托盘和 Settings 可能短时间运行不同版本。
    KeyboardConnected,
};

enum class Command : uint32_t {
    SetProviderEnabled = 1,
    SetPenButtonMode,
    SetOneNoteCompatibility,
    SetAutoStart,
    RequestSafeExit,
    // 键盘「分离后无线连接」。追加在末尾而不是插进中间：设置窗与托盘是两个独立编译的可执行
    // 文件，版本不一致时插入会让既有命令整体错位。
    SetKeyboardWirelessOnDetach,
    // 设备接入时的弹窗开关。
    SetDeviceNotifications,
    // 电池充电阈值，lParam 取 50..100 的百分比；0 表示交还厂商的智能充电模式。
    SetChargeLimit,
    // 色域，lParam 取 PenControl::ColorModeCommand 的数值。
    SetColorMode,
    // 华为后台服务的总开关，lParam 非零表示禁用。
    SetVendorServicesDisabled,
    // 以下三项由托盘直接执行，不经服务转发。它们要落的色彩状态存在 HKCU 下，而服务跑在
    // LocalSystem，那是另一个 hive——写进去用户会话读不到，自然色彩的守护进程更是会在
    // 第一帧读到「已关闭」然后立刻退出。色域不受此限，它只改 3D LUT 不碰这份状态。
    //
    // 色温，lParam 取开尔文值，0 表示关闭。
    SetColorTemperature,
    // 护眼模式，lParam 非零表示开启。
    SetEyeComfort,
    // 自然色彩显示，lParam 非零表示开启。
    SetNaturalColor,
};

} // namespace EGoTouchTrayIpc
