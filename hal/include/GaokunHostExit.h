#pragma once

// gaokun-hal 宿主进程的退出码约定。服务侧的 HostSupervisor 据此区分「重启可能好转」
// 与「重装电脑管家之前每次重启都得到同一个结果」，后者不该进 5 秒一轮的重启循环。
//
// 0/1/2 沿用各宿主已有的含义（正常退出 / 初始化失败 / 环境错误），这里只登记需要跨
// 进程识别的值。追加，不复用。

namespace Gaokun {

// PC Manager 的 Plugins\Depend 目录不存在（PenService.dll / KeyboardService.dll 找不到）。
// 确定性失败：目录被清理工具删过或电脑管家未装全，宿主起多少次都立即退出。
inline constexpr int kHostExitVendorComponentsMissing = 4;

} // namespace Gaokun
