// HidCapsProbe — 列出本机所有 digitizer HID 集合声明了哪些 usage。
//
// 要回答的问题：厂商 VHF 的 report descriptor 里有没有 Invert(0x3C) 与 Eraser(0x45)。
// 探针实测 penFlags 恒为 NONE，若这两个 usage 根本没被声明，那 Windows 就不可能给出
// PEN_FLAG_INVERTED/ERASER，橡皮必须换一条路，而不是去 OneNote 那头找原因。
//
// 只读：打开设备时不要 GENERIC_READ/WRITE。独占打开会把正在用的数字化仪抢掉，
// 而 HidD_GetPreparsedData 用 0 访问权限就够。

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <setupapi.h>
#include <cstdio>
#include <vector>

extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace {

const wchar_t *UsagePageName(USAGE page) {
    switch (page) {
    case 0x01: return L"GenericDesktop";
    case 0x0D: return L"Digitizer";
    case 0x09: return L"Button";
    default:   return L"";
    }
}

// 数字化仪 usage 里与本次诊断相关的那几个。
const wchar_t *DigitizerUsageName(USAGE usage) {
    switch (usage) {
    case 0x01: return L"Digitizer";
    case 0x02: return L"Pen";
    case 0x04: return L"TouchScreen";
    case 0x05: return L"TouchPad";
    case 0x20: return L"Stylus";
    case 0x22: return L"Finger";
    case 0x30: return L"TipPressure";
    case 0x31: return L"BarrelPressure";
    case 0x32: return L"InRange";
    case 0x33: return L"Touch";
    case 0x3C: return L"Invert  <<<";
    case 0x3D: return L"XTilt";
    case 0x3E: return L"YTilt";
    case 0x42: return L"TipSwitch";
    case 0x44: return L"BarrelSwitch";
    case 0x45: return L"Eraser  <<<";
    case 0x46: return L"TabletPick";
    case 0x47: return L"Confidence";
    case 0x48: return L"Width";
    case 0x49: return L"Height";
    case 0x51: return L"ContactId";
    case 0x54: return L"ContactCount";
    case 0x55: return L"ContactCountMax";
    case 0x5B: return L"TransducerSerial";
    default:   return L"";
    }
}

void DumpCaps(const wchar_t *path) {
    // 0 访问权限：只取描述符，不碰数据流，不会把设备从正在用它的进程手里抢走。
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    PHIDP_PREPARSED_DATA pp = nullptr;
    if (!HidD_GetPreparsedData(h, &pp)) { CloseHandle(h); return; }

    HIDP_CAPS caps{};
    if (HidP_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS) {
        HidD_FreePreparsedData(pp);
        CloseHandle(h);
        return;
    }

    // 只看数字化仪，其余（键盘、鼠标、消费键）与本次诊断无关。
    if (caps.UsagePage != 0x0D) {
        HidD_FreePreparsedData(pp);
        CloseHandle(h);
        return;
    }

    HIDD_ATTRIBUTES attrs{};
    attrs.Size = sizeof(attrs);
    HidD_GetAttributes(h, &attrs);

    wchar_t product[256]{};
    HidD_GetProductString(h, product, sizeof(product));

    wprintf(L"\n=== VID_%04X&PID_%04X  %ls\n", attrs.VendorID, attrs.ProductID,
            product[0] ? product : L"(no product string)");
    wprintf(L"    %ls\n", path);
    wprintf(L"    top-level: page=0x%02X(%ls) usage=0x%02X(%ls)\n",
            caps.UsagePage, UsagePageName(caps.UsagePage),
            caps.Usage, DigitizerUsageName(caps.Usage));

    // Button caps 里才会出现 Invert / Eraser / TipSwitch 这类开关量。
    USHORT count = caps.NumberInputButtonCaps;
    if (count) {
        std::vector<HIDP_BUTTON_CAPS> buttons(count);
        if (HidP_GetButtonCaps(HidP_Input, buttons.data(), &count, pp) == HIDP_STATUS_SUCCESS) {
            wprintf(L"    input buttons:\n");
            for (USHORT i = 0; i < count; ++i) {
                const auto &b = buttons[i];
                if (b.IsRange) {
                    wprintf(L"      report=0x%02X page=0x%02X usage 0x%02X-0x%02X\n",
                            b.ReportID, b.UsagePage, b.Range.UsageMin, b.Range.UsageMax);
                } else {
                    wprintf(L"      report=0x%02X page=0x%02X usage=0x%02X %ls\n",
                            b.ReportID, b.UsagePage, b.NotRange.Usage,
                            b.UsagePage == 0x0D ? DigitizerUsageName(b.NotRange.Usage) : L"");
                }
            }
        }
    }

    // Value caps：压力、倾角、坐标。用来确认这个集合就是我们看到的那支笔。
    count = caps.NumberInputValueCaps;
    if (count) {
        std::vector<HIDP_VALUE_CAPS> values(count);
        if (HidP_GetValueCaps(HidP_Input, values.data(), &count, pp) == HIDP_STATUS_SUCCESS) {
            wprintf(L"    input values:\n");
            for (USHORT i = 0; i < count; ++i) {
                const auto &v = values[i];
                if (v.IsRange) continue;
                wprintf(L"      report=0x%02X page=0x%02X usage=0x%02X %ls (bits=%u, %d..%d)\n",
                        v.ReportID, v.UsagePage, v.NotRange.Usage,
                        v.UsagePage == 0x0D ? DigitizerUsageName(v.NotRange.Usage) : L"",
                        v.BitSize, v.LogicalMin, v.LogicalMax);
            }
        }
    }

    HidD_FreePreparsedData(pp);
    CloseHandle(h);
}

} // namespace

int wmain() {
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        wprintf(L"SetupDiGetClassDevs failed (err=%lu)\n", GetLastError());
        return 1;
    }

    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof(iface);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &iface); ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &needed, nullptr);
        if (!needed) continue;
        std::vector<BYTE> buffer(needed);
        auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, needed, nullptr, nullptr)) {
            DumpCaps(detail->DevicePath);
        }
    }
    SetupDiDestroyDeviceInfoList(set);

    wprintf(L"\n带 <<< 的是本次要找的：Invert(0x3C) 与 Eraser(0x45)。\n");
    return 0;
}
