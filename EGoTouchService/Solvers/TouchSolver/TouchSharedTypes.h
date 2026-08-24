#pragma once
// ══════════════════════════════════════════════════════════════════════
// TouchSharedTypes — 触摸流水线各阶段共享的纯数据类型。
//
// 从 MSType.hpp 和 EdgeCompensation.hpp 提取而来，使
// TouchFrameTypes.h 的 TouchRuntimeState 可以无循环依赖地引用
// Peak, PeakEvaluation, ZoneEdgeInfo, EdgeBounds 等类型。
// ══════════════════════════════════════════════════════════════════════

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Solvers {

// ── 传感器网格物理边界（来自 EdgeCompensation.hpp）──────────────────
struct EdgeBounds {
    float colMin = 0.0f;     // 左侧物理边缘
    float colMax = 60.0f;    // 右侧物理边缘 (kCols)
    float rowMin = 0.0f;     // 顶部物理边缘
    float rowMax = 40.0f;    // 底部物理边缘 (kRows)
};

// ── BFS 展开期间每 zone 收集的边缘信息（来自 EdgeCompensation.hpp）──
struct ZoneEdgeInfo {
    int outerColSigSum = 0;
    int innerColSigSum = 0;
    int outerRowSigSum = 0;
    int innerRowSigSum = 0;
    int16_t outerColMax = 0;
    int16_t innerColMax = 0;
    int16_t outerRowMax = 0;
    int16_t innerRowMax = 0;

    uint8_t minCol = 255, maxCol = 0;
    uint8_t minRow = 255, maxRow = 0;
    uint8_t minRowOnOuterCol = 255, maxRowOnOuterCol = 0;
    uint8_t colAtMinOuterRow = 0, colAtMaxOuterRow = 0;
    uint8_t minColOnOuterRow = 255, maxColOnOuterRow = 0;
    uint8_t rowAtMinOuterCol = 0, rowAtMaxOuterCol = 0;
    uint8_t colEdgeWidth = 0;
    uint8_t rowEdgeWidth = 0;

    uint32_t edgeFlags = 0;
};

namespace Touch {

// ── Peak 及其分类相关类型（来自 MSType.hpp）──────────────────────────
struct Peak {
    int r = 0, c = 0;
    int16_t z = 0;
    int neighborSignalSum = 0;
    // 上一行那个和是在**面板内**的邻居上求的。贴边、尤其贴角的峰,邻居有一多半在
    // 面板外,和天然就小,拿它跟一个固定门槛比等于按位置歧视。所以把实际参与的邻居
    // 个数一并记下来,判据里按它折算。
    int neighborCellCount = 0;
    uint8_t id = 0;
    int tzAge = 0;
    int macroZoneIndex = -1;
    int macroZoneArea = 0;
};

// ── 上一帧活跃轨迹的位置（网格单位，x 为列、y 为行）────────────────────
// 跟踪级产出，供下一帧的检测级做迟滞判断用：已经被轨迹跟着的峰，允许用比
// 起始判据宽的维持判据留下来。
struct TrackAnchor {
    float x = 0.0f;
    float y = 0.0f;
};

enum class PalmClass : uint8_t {
    Unknown = 0,
    FingerLikely,
    Ambiguous,
    PalmCandidate,
    PalmLikely
};

enum PalmReasonFlags : uint32_t {
    PalmReasonLargeArea = 0x0001,
    PalmReasonLargeSignalSum = 0x0002,
    PalmReasonLowDensity = 0x0004,
    PalmReasonElongated = 0x0008,
    PalmReasonHighFillRatio = 0x0010,
    PalmReasonEdgeWideContact = 0x0020,
    PalmReasonFlatSignalShape = 0x0040,
    PalmReasonStrongSharpPeakPresent = 0x0080,
    PalmReasonPalmBoxSuppressed = 0x0100
};

struct MacroZoneFeature {
    int zoneIndex = -1;
    int areaCells = 0;
    int signalSum = 0;
    float density = 0.0f;
    int bboxW = 0;
    int bboxH = 0;
    int bboxArea = 0;
    float aspectRatio = 1.0f;
    float fillRatio = 0.0f;
    int maxSignal = 0;
    float meanSignal = 0.0f;
    float signalVariance = 0.0f;
    int edgeTouchMask = 0;
    PalmClass palmClass = PalmClass::Unknown;
    float palmScore = 0.0f;
    float fingerScore = 0.0f;
    uint32_t reasonFlags = 0;
};

struct PeakEvaluation {
    PalmClass palmClass = PalmClass::Unknown;
    float palmScore = 0.0f;
    float fingerScore = 0.0f;
    bool allowContact = true;
    bool palmEvidenceOnly = false;
    int mergeTarget = -1;
    float localMean3x3 = 0.0f;
    float localMean5x5 = 0.0f;
    float prominence = 0.0f;
    float sharpness = 0.0f;
    PalmClass zonePalmClass = PalmClass::Unknown;
    uint32_t evalFlags = 0;
};

// ── 接触点尺寸估计 ────────────────────────────────────────────────
// sizeMm 不是测量值，是从信号总量拟合出来的：cbrt(signalSum) * signalScale。信号总量同时
// 受接触面积和按压力度影响，所以它区分不开「用力点的指尖」和「轻搭的掌根」。下游所有 sizeMm
// 阈值都是围绕这个拟合调出来的，改变换算方式等于让那些阈值集体失去意义。
//
// 需要真正的物理尺度时用 TouchContact::areaMm2，那个由传感器格数乘间距得到，是测量值。
//
// 这个公式此前在 TouchTracker、StylusTouchSuppressor 和 ContactExtractor 各写了一遍，
// 且第三份用的是另一套会在 signalSum 达到 368 时饱和到 15mm 的算法。
inline float EstimateContactSizeMm(int areaCells, int signalSum,
                                   float fallbackMm, float areaScale, float signalScale) {
    if (signalSum > 0)
        return (std::max)(fallbackMm, std::cbrt(static_cast<float>(signalSum)) * signalScale);
    if (areaCells > 0)
        return (std::max)(fallbackMm, std::sqrt(static_cast<float>(areaCells)) * areaScale);
    return fallbackMm;
}

}} // namespace Solvers::Touch
