#pragma once

#include <cstdint>

// 充电阈值的「用户意图」，以及它在厂商那侧的镜像。
//
// 单独成一个编译单元而不是留在 ServiceHost 里：这里三件事全部只与注册表打交道，与服务的
// 生命周期无关，而 ServiceHost.cpp 已经是这棵树上最长的文件。
namespace Service::ChargePolicy {

// 用户最后一次设定。valid 为假说明他从未设过——这种机器上不要替他做任何决定，EC 是什么就
// 是什么。
struct Intent {
    bool valid = false;
    bool manual = false;  // 假为交还厂商的智能充电
    uint8_t limit = 0;    // 仅 manual 为真时有意义
};

[[nodiscard]] Intent Load() noexcept;
void Store(const Intent& intent) noexcept;

// 把同一份意图写进 PC Manager 自己的档位记录。
//
// 厂商的 SmartChargePlugin.dll 在自己启动与系统唤醒时重写 EC，写什么完全由
// HKLM\Software\PCManager\MBAPowerManager 下的几个值决定，它连 EC 当前是什么都不看
// （hal/docs/charge-control.md）。把那份记录改成与用户的选择一致，它下一次运行就写出和
// 我们相同的字节，两边不再互相覆盖——这比在唤醒时和它抢先后可靠。
//
// PC Manager 没装时什么也不做：那台机器上没有人会读这份记录，凭空建出一个厂商的键只是
// 留垃圾。
void MirrorToVendor(const Intent& intent) noexcept;

} // namespace Service::ChargePolicy
