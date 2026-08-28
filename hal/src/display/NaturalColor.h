#pragma once

#include "GaokunDisplay.h"
#include "Qdcm.h"

#include <cstdint>
#include <vector>

// 自然色彩显示。读环境光传感器的色温，查出厂标定表插值出 RGB 增益，按 50 帧、每帧
// 100 ms 平滑过渡后下发。
//
// 参数取自 devices.xml 的 GaoKun3 <naturalColor><algoInfo>：frameCount=50、
// frameTime=100、luminanceThreshold=30。厂商还有 dimmingThreshold=500、minLuma=2000.0
// 两个量，语义未从反汇编中读出，这里没有使用——按猜出来的语义加分支只会让行为不可解释。
//
// 这是四项能力里唯一「一次性下发」解决不了的：它需要常驻监听。命令行入口是
// --natural-color-daemon，前台运行，Ctrl+C 或终止进程即停。
namespace Gaokun::Display {

inline constexpr int kNcFrameCount = 50;
inline constexpr int kNcFrameMs = 100;

// 照度低于此值时不更新目标增益。暗环境下传感器给出的色度噪声很大，跟着它走会看到画面
// 缓慢来回偏色。
inline constexpr double kNcLuminanceThreshold = 30.0;

// 一次性读取环境光并直接下发（不做渐变）。--natural-color on 与 --status 用它。
// 成功时回填读到的色温。
[[nodiscard]] bool ReadAmbientCct(double &kelvin, const wchar_t *&error) noexcept;

// 诊断：把传感器报告里的字段逐行交给 sink。传感器声称支持色温、读出来却是空值时，
// 只有摊开整份报告才看得出它实际在报什么。
void ProbeSensor(void (*sink)(const wchar_t *)) noexcept;

// 常驻循环。直到收到 Ctrl+C 或控制台关闭事件才返回。
[[nodiscard]] int RunNaturalColorDaemon(Qdcm &qdcm, const std::vector<uint32_t> &targets) noexcept;

} // namespace Gaokun::Display
