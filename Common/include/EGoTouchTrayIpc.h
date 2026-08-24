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
};

} // namespace EGoTouchTrayIpc
