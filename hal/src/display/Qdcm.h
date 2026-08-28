#pragma once

#include <cstdint>
#include <vector>

// qdcmlib.dll 的绑定。该 DLL 是 x64，所以本组件必须编译为 ARM64EC。
//
// 接口是两个工厂函数各自返回一个带虚表的对象。这里把虚表写成显式的函数指针结构，而不是
// 声明成 C++ 抽象类：抽象类要求编译器的虚表布局与 DLL 完全一致，那是一条没有写进任何
// 契约的假设；显式结构则把布局摆在明处，与逆向记录一一对应。
namespace Gaokun::Display {

// 17^3 的 3D LUT，通道值为 0..4095 的定点数。
inline constexpr int kLutSize = 17;
inline constexpr int kLutEntries = kLutSize * kLutSize * kLutSize; // 4913
inline constexpr uint32_t kLutMax = 4095;

// IGC（输入整形）是 3x1D LUT，固定 257 项。
inline constexpr int kIgcEntries = 257;

struct RgbTable {
    std::vector<uint32_t> red;
    std::vector<uint32_t> green;
    std::vector<uint32_t> blue;
};

struct IgcData {
    int32_t enable;
    uint32_t *channel1;
    uint32_t *channel2;
    uint32_t *channel3;
    int32_t numEntries;
};

struct Lut3dData {
    int32_t enable;
    uint32_t *channel1;
    uint32_t *channel2;
    uint32_t *channel3;
    int32_t numFlattenEntries;
};

class Qdcm {
public:
    Qdcm() noexcept = default;
    ~Qdcm() noexcept;

    Qdcm(const Qdcm &) = delete;
    Qdcm &operator=(const Qdcm &) = delete;

    [[nodiscard]] bool Load() noexcept;

    [[nodiscard]] bool EnumDisplays(std::vector<uint32_t> &ids) noexcept;
    [[nodiscard]] bool QueryCaps(uint32_t display, int32_t &caps) noexcept;

    // 诊断用：同时给出 out 参数与函数返回值，用来判断能力位到底从哪一侧回来。
    [[nodiscard]] int QueryCapsRaw(uint32_t display, int32_t &caps) noexcept;

    // 3x3 色彩校正矩阵，行优先。
    [[nodiscard]] bool SetPcc(uint32_t display, const float matrix[9]) noexcept;

    // table 为空表示关闭该级。3D LUT 的扁平索引是 blue*289 + green*17 + red，
    // 与固件表的读取顺序相反，见 FactoryLut.h。
    [[nodiscard]] bool SetIgc(uint32_t display, const RgbTable *table) noexcept;
    [[nodiscard]] bool SetLut3d(uint32_t display, const RgbTable *table) noexcept;

private:
    void *m_module = nullptr;
    void *m_factory1 = nullptr;
    void *m_factory2 = nullptr;
};

} // namespace Gaokun::Display
