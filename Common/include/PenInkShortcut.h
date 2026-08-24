#pragma once
// PenInkShortcut — 把笔的侧键手势交还给 Windows 自己的笔菜单。
//
// Windows 的 ClickNote 用三个组合键接收笔按键手势：单击 Win+F20、双击 Win+F19、长按
// Win+F18，收到后按“设置 → 笔和 Windows Ink”里的配置执行动作。走这条路等于复用系统菜单，
// 不需要我们自己做一套映射 UI。
//
// 这里不放在服务里，是因为服务放不下：服务跑在会话 0，线程没有可交互的输入桌面，SendInput
// 会直接以 ERROR_ACCESS_DENIED 失败（实测 sent=0 err=5），连“发出去但没人收”都不是。所以
// 注入只能由用户会话里的伴随进程执行，这个头文件因此属于 Common 而不是 Device。

namespace PenInk {

/// 注入一次“笔按键双击”= Win+F19。必须在用户会话的进程里调用，服务进程调用必然失败。
/// 返回 false 表示 SendInput 没有完整投递，调用方可据此记日志。
bool InjectDoubleClickShortcut();

} // namespace PenInk
