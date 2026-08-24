#include "PenInkShortcut.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace PenInk {

bool InjectDoubleClickShortcut() {
    // F19 是实测得到的，不是从文档抄的：在本机发 Win+F19 会拉起截图工具，与当时
    // HKCU\...\ClickNote\UserCustomization\DoubleClickBelowLock 配置的动作一致。
    // 此前代码里发的是 Win+F22，没有任何来源，系统里也没有接收方。
    INPUT inputs[4]{};

    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_LWIN;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_F19;

    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = VK_F19;
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;

    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_LWIN;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    return SendInput(4, inputs, sizeof(INPUT)) == 4;
}

} // namespace PenInk
