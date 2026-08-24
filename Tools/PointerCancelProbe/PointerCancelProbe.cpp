// PointerCancelProbe — 真机验证 Windows 对触摸 confidence 位的处理方式。
//
// 背景:VHF 注入驱动(HidInjectorThp.sys)的触摸描述符状态字节 bit0 = Tip Switch、
// bit1 = Confidence。本程序向 VHF 设备注入一段合成拖拽,中途把 confidence 降为 0,
// 在自己的观察窗里记录 WM_POINTER* 消息与 pointerFlags,回答一个问题:
// confidence 1→0 时,Windows 是回溯撤销整段指针流(出现 POINTER_FLAG_CANCELED),
// 还是仅停止后续投递。答案决定触控栈重构中 cancel 通路的形态。
//
// 用法(管理员运行;先停掉服务注入,避免真实触摸帧交错):
//   sc stop OpenEGoHubServiceDebug
//   PointerCancelProbe.exe          正式序列:拖拽 20 帧 → 0x01 → 0x00 → 空报文
//   PointerCancelProbe.exe --low    对照组:起手即 confidence=0
//   sc start OpenEGoHubServiceDebug
//
// 输出为逐条指针消息:时间、消息名、pointer id、坐标、flags 解码。

#include <Windows.h>
#include <SetupAPI.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

// 与 EGoTouchService/Device/vhf/VhfReporter.cpp:15-17 相同的设备接口 GUID
const GUID kVhfGuid = {0x59819b74, 0xf102, 0x469a,
                       {0x90, 0x09, 0x3c, 0xaf, 0x35, 0xfd, 0x46, 0x86}};

// 触摸报文布局与描述符一致:report id 0x01,每接触 6 字节
// (状态 1B + id 1B + 字段1 u16 [0..16000] + 字段2 u16 [0..25600]),offset 31 为接触计数
constexpr size_t kPacketSize = 32;
constexpr uint16_t kField1Center = 8000;
constexpr uint16_t kField2Start = 10000;
constexpr uint16_t kField2StepPerFrame = 200;

HANDLE OpenVhfDevice() {
    HDEVINFO devInfo = SetupDiGetClassDevsW(&kVhfGuid, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    HANDLE handle = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(ifData);
    for (DWORD i = 0;
         SetupDiEnumDeviceInterfaces(devInfo, nullptr, &kVhfGuid, i, &ifData); ++i) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &required, nullptr);
        if (required == 0) continue;
        auto* detail = static_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(malloc(required));
        if (!detail) break;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, required,
                                             nullptr, nullptr)) {
            handle = CreateFileW(detail->DevicePath, GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
        }
        free(detail);
        if (handle != INVALID_HANDLE_VALUE) break;
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return handle;
}

bool WriteTouchPacket(HANDLE dev, uint8_t state, uint8_t contactId,
                      uint16_t field1, uint16_t field2, uint8_t count) {
    uint8_t bytes[kPacketSize] = {};
    bytes[0] = 0x01;
    if (count != 0) {
        bytes[1] = state;
        bytes[2] = contactId;
        bytes[3] = static_cast<uint8_t>(field1 & 0xFF);
        bytes[4] = static_cast<uint8_t>(field1 >> 8);
        bytes[5] = static_cast<uint8_t>(field2 & 0xFF);
        bytes[6] = static_cast<uint8_t>(field2 >> 8);
    }
    bytes[31] = count;
    DWORD written = 0;
    return WriteFile(dev, bytes, kPacketSize, &written, nullptr) &&
           written == kPacketSize;
}

const char* MessageName(UINT msg) {
    switch (msg) {
    case WM_POINTERDOWN: return "DOWN  ";
    case WM_POINTERUPDATE: return "UPDATE";
    case WM_POINTERUP: return "UP    ";
    case WM_POINTERCAPTURECHANGED: return "CAPCHG";
    case WM_POINTERLEAVE: return "LEAVE ";
    default: return "OTHER ";
    }
}

void AppendFlag(std::string& out, bool set, const char* name) {
    if (!set) return;
    if (!out.empty()) out += '|';
    out += name;
}

DWORD g_startTick = 0;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_POINTERDOWN:
    case WM_POINTERUPDATE:
    case WM_POINTERUP:
    case WM_POINTERCAPTURECHANGED:
    case WM_POINTERLEAVE: {
        POINTER_INFO info{};
        const UINT32 pointerId = GET_POINTERID_WPARAM(wp);
        std::string flags;
        LONG x = -1, y = -1;
        if (GetPointerInfo(pointerId, &info)) {
            x = info.ptPixelLocation.x;
            y = info.ptPixelLocation.y;
            AppendFlag(flags, info.pointerFlags & POINTER_FLAG_CANCELED, "CANCELED");
            AppendFlag(flags, info.pointerFlags & POINTER_FLAG_INCONTACT, "INCONTACT");
            AppendFlag(flags, info.pointerFlags & POINTER_FLAG_INRANGE, "INRANGE");
            AppendFlag(flags, info.pointerFlags & POINTER_FLAG_CONFIDENCE, "CONFIDENCE");
            AppendFlag(flags, info.pointerFlags & POINTER_FLAG_UP, "UP");
            AppendFlag(flags, info.pointerFlags & POINTER_FLAG_DOWN, "DOWN");
            AppendFlag(flags, info.pointerFlags & POINTER_FLAG_UPDATE, "UPDATE");
        } else {
            flags = "<GetPointerInfo failed>";
        }
        std::printf("%6lu ms  %s id=%u pos=(%ld,%ld) flags=%s\n",
                    GetTickCount() - g_startTick, MessageName(msg), pointerId, x, y,
                    flags.c_str());
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void PumpMessagesFor(DWORD ms) {
    const DWORD deadline = GetTickCount() + ms;
    MSG m;
    for (;;) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        const DWORD now = GetTickCount();
        if (now >= deadline) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, deadline - now, QS_ALLINPUT);
    }
}

} // namespace

int main(int argc, char** argv) {
    const bool lowConfidenceFromStart = argc > 1 && std::strcmp(argv[1], "--low") == 0;
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    HANDLE dev = OpenVhfDevice();
    if (dev == INVALID_HANDLE_VALUE) {
        std::printf("cannot open VHF device (run as administrator; is the driver present?)\n");
        return 2;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PointerCancelProbe";
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    RegisterClassW(&wc);
    // 全屏置顶,保证合成接触落在本窗口内
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName,
                                L"PointerCancelProbe - do not touch the screen",
                                WS_POPUP | WS_VISIBLE, 0, 0,
                                GetSystemMetrics(SM_CXSCREEN),
                                GetSystemMetrics(SM_CYSCREEN),
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        std::printf("cannot create observer window\n");
        CloseHandle(dev);
        return 2;
    }
    SetForegroundWindow(hwnd);
    g_startTick = GetTickCount();

    std::printf("mode: %s\n", lowConfidenceFromStart
                                  ? "control (confidence=0 from the first frame)"
                                  : "main (drag 20 frames, then confidence drop)");
    std::printf("settling 2s - keep hands off the screen\n");
    PumpMessagesFor(2000);

    const uint8_t dragState = lowConfidenceFromStart ? 0x01 : 0x03;
    uint16_t field2 = kField2Start;
    for (int i = 0; i < 20; ++i) {
        WriteTouchPacket(dev, dragState, 1, kField1Center, field2, 1);
        field2 = static_cast<uint16_t>(field2 + kField2StepPerFrame);
        PumpMessagesFor(10);
    }

    std::printf("-- injecting state 0x01 (tip=1, confidence=0) --\n");
    WriteTouchPacket(dev, 0x01, 1, kField1Center, field2, 1);
    PumpMessagesFor(10);

    std::printf("-- injecting state 0x00 (tip=0, confidence=0) --\n");
    WriteTouchPacket(dev, 0x00, 1, kField1Center, field2, 1);
    PumpMessagesFor(10);

    std::printf("-- injecting empty report (count=0) --\n");
    WriteTouchPacket(dev, 0, 0, 0, 0, 0);

    std::printf("draining messages for 3s...\n");
    PumpMessagesFor(3000);

    std::printf("done. verdict: CANCELED flag after the 0x01 frame means Windows\n"
                "retroactively cancels the pointer stream; a silent end means it only\n"
                "stops further delivery.\n");
    CloseHandle(dev);
    DestroyWindow(hwnd);
    return 0;
}
