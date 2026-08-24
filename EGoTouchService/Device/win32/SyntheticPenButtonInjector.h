#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

/// Win32 虚拟笔按键注入器 — 使用 CreateSyntheticPointerDevice +
/// InjectSyntheticPointerInput 注入笔杆按键或橡皮擦脉冲。
///
/// 所有注入均为边沿触发（瞬时），不锁存。适合笔硬件只发单次手势事件的场景。
class SyntheticPenButtonInjector {
public:
    SyntheticPenButtonInjector() = default;
    ~SyntheticPenButtonInjector();

    SyntheticPenButtonInjector(const SyntheticPenButtonInjector&) = delete;
    SyntheticPenButtonInjector& operator=(const SyntheticPenButtonInjector&) = delete;

    /// 确保虚拟笔设备已创建。首次调用时初始化，后续调用立即返回。
    bool EnsureDevice();

    /// 注入一次笔杆按键脉冲
    bool InjectBarrelPulse(POINT screenPt);

    /// 注入一次橡皮擦脉冲
    bool InjectEraserPulse(POINT screenPt);

    // 这里没有键盘快捷键注入。服务在会话 0，SendInput 拿不到可交互的输入桌面，必然以
    // ERROR_ACCESS_DENIED 失败；笔按键双击 → Windows Ink 的注入在托盘进程里，见
    // Common/include/PenInkShortcut.h。下面两个走的是合成指针设备，不受此限。

    /// 设备是否已就绪
    bool IsReady() const { return m_device != nullptr; }

private:
    bool InjectPenPulse(POINT screenPt, UINT32 penFlags, UINT32 extraPointerFlags);

    HSYNTHETICPOINTERDEVICE m_device = nullptr;
};
