#pragma once
// 可执行文件里那张品牌图标的资源 id。
//
// 三处要用同一个数：EGoTouchSettings 的 App.rc、EGoTouchTray 的 Tray.rc，以及设置窗口
// 启动时用 WM_SETICON 显式挂图标的那段代码。各写一个字面量的话，改了 .rc 而漏掉代码那边
// 就会静默退回系统默认图标，没有任何报错。
//
// .rc 与 .cpp 都能包含这个头：资源编译器只认预处理指令，所以这里除了 #define 什么都不能有。

#define IDI_APP_ICON 101

// 屏幕页的色彩对照图，只有设置窗用。放在这里而不是另开一个头文件，理由与上面相同：
// .rc 与读取它的 .cpp 必须用同一个数。
#define IDR_COLOR_REFERENCE 102

// 标题栏的品牌标记，只有设置窗用。
#define IDR_BRAND_MARK 103
