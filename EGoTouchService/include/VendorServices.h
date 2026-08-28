#pragma once

#include <cstdint>

// 华为的后台服务开关。
//
// 这些服务的职责本程序都已经承担，或者对本机没有用处；把它们停掉能省下常驻内存与开机时间。
// 操作是可逆的：禁用前把每个服务原来的启动类型记进注册表，恢复时按记录写回，而不是一律
// 设成自动——有些服务出厂就是手动启动，一律改自动等于替用户改了配置。
//
// 有三类东西刻意不在名单里：
//
//   HuaweiThpService  本程序要从它的 ImagePath 推导原厂安装目录，托盘退出后也要把设备交还
//                     给它。服务项必须留在 SCM 中，禁用启动无妨，删掉就找不回来了。
//   HWVEAudioService  音频，本程序不涉及。
//   内核驱动          HidInjectorThp、SpbThpTool、HwOs2ECx64、HWAudioOs2Ec。虚拟 HID、SPI
//                     通道、EC 全在这一层，禁用等于把触控、笔、键盘和充电控制一起关掉。
namespace Service::VendorServices {

struct Status {
    uint32_t total = 0;     ///< 名单里在本机实际存在的服务数
    uint32_t disabled = 0;  ///< 其中已被禁用的
    // 其中进程仍在运行的。启动类型与运行状态是两回事：改了类型而进程还在，说明这次改动
    // 还没有完全落地。界面靠这个差别提示用户，而不是笼统地说「需要重启」。
    uint32_t running = 0;
};

// 名单里有多少服务存在、多少已禁用、多少仍在运行。
[[nodiscard]] Status Query() noexcept;

// 全部禁用，并记下每个服务原来的启动类型。已在禁用状态的跳过，不覆盖已有记录。
//
// 两个方向都是幂等的，也都立即生效：禁用会停掉正在运行的实例，恢复会把它们起回来。
[[nodiscard]] bool DisableAll() noexcept;

// 按记录恢复，并把服务启动起来。没有记录的服务恢复为自动启动：那说明它不是被本程序禁用的，
// 但「按了恢复却什么都没发生」比多启动一个服务更糟。
[[nodiscard]] bool RestoreAll() noexcept;

} // namespace Service::VendorServices
