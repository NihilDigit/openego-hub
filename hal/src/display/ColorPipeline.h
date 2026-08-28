#pragma once

#include "GaokunDisplay.h"
#include "Qdcm.h"

#include <cstdint>
#include <vector>

// 色温、护眼、自然色彩三项的合成层。
//
// 三者写的是同一个 PCC 寄存器，硬件上只有一份，谁后写谁生效。因此这里不提供「设置色温」
// 「设置护眼」这样各自下发的入口——唯一的下发入口是 Apply，它先取回三项的完整状态，
// 合成出一组增益，再整体写一次。改任意一项都走 Store + Apply 这一对。
//
// 状态必须持久化：命令行是一次性进程，下一次调用无从知道另外两项开着还是关着，只按本次
// 参数下发就会把另外两项静默清掉。存放位置是 HKCU（每用户，不需要管理员）；厂商存在
// HKLM 下，但那是服务在写，我们没有常驻服务。
namespace Gaokun::Display {

// 读取持久化的三项状态。键不存在时返回全关，这与出厂状态一致。
[[nodiscard]] ColorState LoadColorState() noexcept;

// 写回。失败只影响下一次调用能否看到本次的改动，不影响本次下发。
bool StoreColorState(const ColorState &state) noexcept;

// 合成并下发。gain 非空时回填实际下发的增益，便于调用方打印。
[[nodiscard]] bool ApplyColorState(Qdcm &qdcm, const std::vector<uint32_t> &targets,
                                   const ColorState &state, RgbGain *applied = nullptr) noexcept;

// 直接下发一组增益，跳过持久化状态。自然色彩守护进程做帧间渐变时用它，逐帧写注册表
// 是没有必要的开销。
[[nodiscard]] bool ApplyGain(Qdcm &qdcm, const std::vector<uint32_t> &targets,
                            const RgbGain &gain) noexcept;

} // namespace Gaokun::Display
