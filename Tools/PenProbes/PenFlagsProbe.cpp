// PenFlagsProbe — 看 Windows 到底从这支笔收到了什么。
//
// 诊断问题：CommandSendPenCurrentFunc(1) 之后笔停止书写，但橡皮在 OneNote 里也不生效。
// 断点可能在两层：厂商链没把 eraser/invert 位放进 HID 报告，或者放了而 OneNote 不消费。
// 这个探针只回答第一层——Windows 的 pointer 输入栈收到了什么。
//
// Surface Pen 的橡皮端走的是 HID 的 Invert + Eraser usage，Windows 呈现为
// PEN_FLAG_INVERTED / PEN_FLAG_ERASER，OneNote 消费的就是这个。若这里看不到那两位，
// 问题就不在 OneNote 一侧。

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <cstdio>

namespace {

const wchar_t *PointerTypeName(POINTER_INPUT_TYPE type) {
    switch (type) {
    case PT_POINTER:  return L"POINTER";
    case PT_TOUCH:    return L"TOUCH";
    case PT_PEN:      return L"PEN";
    case PT_MOUSE:    return L"MOUSE";
    case PT_TOUCHPAD: return L"TOUCHPAD";
    default:          return L"?";
    }
}

void PrintPenFlags(UINT32 flags) {
    if (flags == PEN_FLAG_NONE) { wprintf(L"NONE"); return; }
    bool first = true;
    auto bit = [&](UINT32 mask, const wchar_t *name) {
        if (!(flags & mask)) return;
        wprintf(L"%ls%ls", first ? L"" : L"|", name);
        first = false;
    };
    bit(PEN_FLAG_BARREL, L"BARREL");
    bit(PEN_FLAG_INVERTED, L"INVERTED");
    bit(PEN_FLAG_ERASER, L"ERASER");
}

void Report(const wchar_t *what, UINT32 pointerId) {
    POINTER_INFO info{};
    if (!GetPointerInfo(pointerId, &info)) return;

    wprintf(L"%-10ls type=%-5ls", what, PointerTypeName(info.pointerType));

    if (info.pointerType == PT_PEN) {
        POINTER_PEN_INFO pen{};
        if (GetPointerPenInfo(pointerId, &pen)) {
            wprintf(L"  penFlags=");
            PrintPenFlags(pen.penFlags);
            wprintf(L"(0x%02x) mask=0x%02x pressure=%u tilt=%d,%d rot=%u",
                    pen.penFlags, pen.penMask, pen.pressure,
                    pen.tiltX, pen.tiltY, pen.rotation);
        } else {
            wprintf(L"  GetPointerPenInfo failed (err=%lu)", GetLastError());
        }
    }
    wprintf(L"\n");
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_POINTERDOWN:   Report(L"DOWN", GET_POINTERID_WPARAM(wp)); return 0;
    // UPDATE 会刷屏，只在 flags 变化时打印：要看的是状态翻转，不是轨迹。
    case WM_POINTERUPDATE: {
        static UINT32 lastFlags = 0xffffffff;
        POINTER_PEN_INFO pen{};
        if (GetPointerPenInfo(GET_POINTERID_WPARAM(wp), &pen) && pen.penFlags != lastFlags) {
            lastFlags = pen.penFlags;
            Report(L"UPDATE*", GET_POINTERID_WPARAM(wp));
        }
        return 0;
    }
    case WM_POINTERUP:     Report(L"UP", GET_POINTERID_WPARAM(wp)); return 0;
    case WM_POINTERENTER:  Report(L"ENTER", GET_POINTERID_WPARAM(wp)); return 0;
    case WM_POINTERLEAVE:  Report(L"LEAVE", GET_POINTERID_WPARAM(wp)); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
        const wchar_t *text = L"用笔在这个窗口里划几下，然后看控制台输出。ESC 退出。";
        DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int wmain() {
    // 不声明 DPI 感知就拿不到物理坐标，但这里只看 flags，不看坐标，够用。
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PenFlagsProbe";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"PenFlagsProbe",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                900, 600, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { wprintf(L"CreateWindow failed (err=%lu)\n", GetLastError()); return 1; }

    ShowWindow(hwnd, SW_SHOW);
    wprintf(L"PenFlagsProbe: 用笔在窗口里划。PEN_FLAG_INVERTED/ERASER 是橡皮的判据。\n");

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
