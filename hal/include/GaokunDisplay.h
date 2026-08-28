#pragma once

#include <cstdint>

// 屏幕色彩能力的对外接口。
//
// 进程与架构的分工：
//
//   GaokunDisplay.exe   ARM64EC。凡是要下发到面板的（色域 3D LUT、色温 / 护眼 /
//                       自然色彩合成出的 PCC 矩阵）都在这个进程里，因为它加载 x64 的
//                       qdcmlib.dll。
//   本头文件            纯算术，无任何依赖，原生 ARM64 的上层可直接包含，用来在不启动
//                       子进程的前提下预览一组设置会得到什么增益。
//
// 环境光传感器是纯 COM，在 ARM64EC 下与原生 ARM64 行为一致，因此没有为它单开进程。真正
// 需要常驻的只有自然色彩，它以 GaokunDisplay.exe --natural-color-daemon 的形式存在，
// 上层负责拉起与终止。
//
// 不做 ICC 关联。切色域时同步操作系统的 ICC 是原厂的做法，也实现过，但 mscms 的逐显示器
// 关联在本机收下写入却不落地，管理员身份下同样如此；实测记录在 docs/display-manage.md
// 6.5。后果是 Photoshop 一类做色彩管理的应用仍按旧色域解释画面。
//
// 命令行契约（上层即以此驱动）：
//
//   --preset <Native|sRGB|DisplayP3>  下发出厂 3D LUT，Native 即清空
//   --temperature <off|2000..10000|warm|neutral|cool>
//   --eye-comfort <on|off>
//   --natural-color <on|off>       只记录状态并立即按当前环境光下发一次
//   --natural-color-daemon         常驻，读传感器并做渐变，Ctrl+C 或终止进程即停
//   --status                       打印三项的当前状态与合成后的增益
//   --reset                        清 3D LUT、清 PCC、清三项状态
//
// 色温、护眼、自然色彩写的是同一个 PCC 寄存器，不能各写各的。三者的状态持久化在
// HKCU\Software\gaokun-hal\Display，任何一项变化都重新读全部三项、重算合成结果、整体
// 下发一次。上层不要试图自己叠加两条命令的效果。
namespace Gaokun::Display {

// 逐通道增益。PCC 通道上没有色域矩阵（本机色域走 3D LUT），所以下发的矩阵就是
// diag(r, g, b)，与厂商 TransferPccMatrixColorInfoToPccMatrix 在基矩阵为单位阵时的
// 结果相同。
struct RgbGain {
    double r = 1.0;
    double g = 1.0;
    double b = 1.0;
};

[[nodiscard]] inline RgbGain Multiply(const RgbGain &a, const RgbGain &b) noexcept {
    return RgbGain{a.r * b.r, a.g * b.g, a.b * b.b};
}

enum class Gamut : uint32_t {
    Native = 0,
    Srgb = 1,
    DisplayP3 = 2,
};

// 护眼模式的蓝通道系数。取自 devices.xml 的 GaoKun3 条目（莱茵认证值），与注册表
// EyeProtect\RhineValue = 0.720000 一致。厂商的 GetColorBlueValue 只返回 0 或 255，
// 即这是一个纯开关，没有强度档位；界面上那条强度滑条走的是面板厂商专属 CCT 表，
// GaoKun3 没有这张表，那条路在本机是空转的。
inline constexpr double kEyeComfortBlue = 0.72;

// 色温可调区间。上下界即下面那张出厂标定表的两端，外推没有数据支撑。
inline constexpr int kTemperatureMinK = 2000;
inline constexpr int kTemperatureMaxK = 10000;

// 三档预设。中性档等于关闭，增益为单位值而非表内某一行——表里没有任何一行是 (1,1,1)。
inline constexpr int kTemperatureWarmK = 4000;
inline constexpr int kTemperatureCoolK = 8000;

// CCT 到 RGB 增益的出厂标定表，取自 devices.xml 的 GaoKun3 <naturalColor><algoInfo>，
// 九个锚点覆盖 2000K 至 10000K。
//
// 厂商只把它用于自然色彩（按环境光查表），手动色温另走一套半径 80 的二维色轮。这里
// 手动色温也用这张表：色轮的六段插值锚点顺序在逆向记录里标注为未逐段验证，自己重写
// 等于凭空发明一条曲线，而这张表是同一块面板的出厂标定数据，方向也一致（环境光越暖，
// 增益越偏暖）。代价是色轮上那些偏离冷暖轴的点无法表达，本机没有需要它们的场景。
inline constexpr int kCctTableSize = 9;
inline constexpr double kCctTable[kCctTableSize][4] = {
    {2000.0, 1.0, 0.9130434782608695, 0.8188405797101449},
    {3000.0, 1.0, 0.9264705882352942, 0.8566176470588235},
    {4000.0, 1.0, 0.9368029739776952, 0.8884758364312267},
    {5000.0, 1.0, 0.9402985074626866, 0.9067164179104478},
    {6000.0, 1.0, 0.9547169811320755, 0.9358490566037736},
    {7000.0, 1.0, 0.9619771863117871, 0.9581749049429658},
    {8000.0, 1.0, 0.9693486590038314, 0.9923371647509579},
    {9000.0, 0.9772727272727273, 0.9583333333333334, 1.0},
    {10000.0, 0.9553903345724907, 0.9405204460966543, 1.0},
};

// 在标定表上按 CCT 线性插值，区间外夹到两端。
[[nodiscard]] inline RgbGain GainForCct(double cct) noexcept {
    if (cct <= kCctTable[0][0]) return RgbGain{kCctTable[0][1], kCctTable[0][2], kCctTable[0][3]};
    const int last = kCctTableSize - 1;
    if (cct >= kCctTable[last][0]) {
        return RgbGain{kCctTable[last][1], kCctTable[last][2], kCctTable[last][3]};
    }
    for (int i = 0; i < last; ++i) {
        const double lo = kCctTable[i][0];
        const double hi = kCctTable[i + 1][0];
        if (cct > hi) continue;
        const double a = (cct - lo) / (hi - lo);
        return RgbGain{kCctTable[i][1] + a * (kCctTable[i + 1][1] - kCctTable[i][1]),
                       kCctTable[i][2] + a * (kCctTable[i + 1][2] - kCctTable[i][2]),
                       kCctTable[i][3] + a * (kCctTable[i + 1][3] - kCctTable[i][3])};
    }
    return RgbGain{};
}

// 三项能力的完整状态。持久化在注册表，每次下发前整体读出。
struct ColorState {
    // 0 表示关闭。非 0 时取值在 kTemperatureMinK..kTemperatureMaxK 之间。
    int temperatureK = 0;
    bool eyeComfort = false;
    bool naturalColor = false;
    // 自然色彩上一次采到的环境光色温，仅在 naturalColor 为真时参与合成。守护进程写入，
    // 一次性命令读出，用来在没有守护进程时也能保持住最后一次的效果。
    double sensorCct = 0.0;
};

[[nodiscard]] inline RgbGain TemperatureGain(const ColorState &s) noexcept {
    if (s.temperatureK <= 0) return RgbGain{};
    return GainForCct(static_cast<double>(s.temperatureK));
}

[[nodiscard]] inline RgbGain EyeComfortGain(const ColorState &s) noexcept {
    if (!s.eyeComfort) return RgbGain{};
    return RgbGain{1.0, 1.0, kEyeComfortBlue};
}

[[nodiscard]] inline RgbGain NaturalColorGain(const ColorState &s) noexcept {
    if (!s.naturalColor || s.sensorCct <= 0.0) return RgbGain{};
    return GainForCct(s.sensorCct);
}

// 三者的合成。厂商的 Fusion 没有逐条反汇编，从形参与 SetColorDll 侧同逻辑的
// MergeCircleCtAndSensorCt 看是逐通道相乘，此处按相乘实现。
//
// 注意色温与自然色彩共用同一张标定表，两者同时开启时增益相乘，偏暖会叠加两次。厂商
// 那边色温走色轮、自然色彩走标定表，两条曲线不同，但同样是相乘，同样会叠加。
[[nodiscard]] inline RgbGain Compose(const ColorState &s) noexcept {
    return Multiply(Multiply(TemperatureGain(s), EyeComfortGain(s)), NaturalColorGain(s));
}

// 合成结果到 PCC 矩阵，行优先，与 Qdcm::SetPcc 的入参一致。
inline void ToPccMatrix(const RgbGain &gain, float matrix[9]) noexcept {
    matrix[0] = static_cast<float>(gain.r);
    matrix[1] = 0.0f;
    matrix[2] = 0.0f;
    matrix[3] = 0.0f;
    matrix[4] = static_cast<float>(gain.g);
    matrix[5] = 0.0f;
    matrix[6] = 0.0f;
    matrix[7] = 0.0f;
    matrix[8] = static_cast<float>(gain.b);
}

} // namespace Gaokun::Display
