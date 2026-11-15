#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace DisplayModeHelper {

struct PanelModeInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t currentHz = 0;
    std::vector<uint32_t> supportedHz;
};

inline PanelModeInfo QueryPrimary() noexcept {
    PanelModeInfo info{};
    DEVMODEW cur{};
    cur.dmSize = sizeof(cur);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &cur)) {
        return info;
    }

    info.width = cur.dmPelsWidth;
    info.height = cur.dmPelsHeight;
    info.currentHz = cur.dmDisplayFrequency;

    DEVMODEW dm{};
    for (DWORD i = 0;; ++i) {
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(nullptr, i, &dm)) break;

        if (dm.dmPelsWidth != cur.dmPelsWidth || dm.dmPelsHeight != cur.dmPelsHeight ||
            dm.dmBitsPerPel != cur.dmBitsPerPel) {
            continue;
        }
        if ((cur.dmFields & DM_DISPLAYORIENTATION) && (dm.dmFields & DM_DISPLAYORIENTATION) &&
            dm.dmDisplayOrientation != cur.dmDisplayOrientation) {
            continue;
        }
        const uint32_t hz = dm.dmDisplayFrequency;
        if (hz >= 30 &&
            std::find(info.supportedHz.begin(), info.supportedHz.end(), hz) ==
                info.supportedHz.end()) {
            info.supportedHz.push_back(hz);
        }
    }

    if (info.currentHz >= 30 &&
        std::find(info.supportedHz.begin(), info.supportedHz.end(), info.currentHz) ==
            info.supportedHz.end()) {
        info.supportedHz.push_back(info.currentHz);
    }

    std::sort(info.supportedHz.begin(), info.supportedHz.end());
    return info;
}

inline bool SetPrimaryRefreshRate(uint32_t targetHz) noexcept {
    if (targetHz < 30) return false;

    DEVMODEW cur{};
    cur.dmSize = sizeof(cur);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &cur)) {
        return false;
    }
    if (cur.dmDisplayFrequency == targetHz) {
        return true;
    }

    cur.dmDisplayFrequency = targetHz;
    cur.dmFields |= DM_DISPLAYFREQUENCY;
    return ChangeDisplaySettingsExW(nullptr, &cur, nullptr, CDS_UPDATEREGISTRY, nullptr) ==
           DISP_CHANGE_SUCCESSFUL;
}

} // namespace DisplayModeHelper
