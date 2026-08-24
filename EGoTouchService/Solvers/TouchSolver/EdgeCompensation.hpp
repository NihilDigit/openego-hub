#pragma once

#include "SolverTypes.h"
#include "TouchFrameTypes.h"
#include "TouchSharedTypes.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Solvers {

// EdgeBounds 和 ZoneEdgeInfo 已迁移到 TouchSharedTypes.h

// BFS-level grid limits (node indices, not physical edges)
static constexpr int kGridColMin = 0, kGridColMax = 59;
static constexpr int kGridRowMin = 0, kGridRowMax = 39;

// TZ_UpdateEdgeInfo — called per pixel during BFS
inline void TZ_UpdateEdgeInfo(ZoneEdgeInfo& ei,
                              int16_t signal, int col, int row,
                              uint8_t flagMask) {
    if (col == kGridColMin || col == kGridColMax) {
        ei.outerColSigSum += signal;
        if (flagMask & 1)
            ei.outerColMax = std::max(ei.outerColMax, signal);
        if (flagMask & 4) {
            const bool firstOuterCol = ei.minRowOnOuterCol == 255;
            if (firstOuterCol || row < ei.minRowOnOuterCol) {
                ei.minRowOnOuterCol = static_cast<uint8_t>(row);
                ei.colAtMinOuterRow = static_cast<uint8_t>(col);
            }
            if (firstOuterCol || row > ei.maxRowOnOuterCol) {
                ei.maxRowOnOuterCol = static_cast<uint8_t>(row);
                ei.colAtMaxOuterRow = static_cast<uint8_t>(col);
            }
        }
    } else if (col == kGridColMin + 1 || col == kGridColMax - 1) {
        ei.innerColSigSum += signal;
        if (flagMask & 2)
            ei.innerColMax = std::max(ei.innerColMax, signal);
    }
    if (row == kGridRowMin || row == kGridRowMax) {
        ei.outerRowSigSum += signal;
        if (flagMask & 1)
            ei.outerRowMax = std::max(ei.outerRowMax, signal);
        if (flagMask & 4) {
            const bool firstOuterRow = ei.minColOnOuterRow == 255;
            if (firstOuterRow || col < ei.minColOnOuterRow) {
                ei.minColOnOuterRow = static_cast<uint8_t>(col);
                ei.rowAtMinOuterCol = static_cast<uint8_t>(row);
            }
            if (firstOuterRow || col > ei.maxColOnOuterRow) {
                ei.maxColOnOuterRow = static_cast<uint8_t>(col);
                ei.rowAtMaxOuterCol = static_cast<uint8_t>(row);
            }
        }
    } else if (row == kGridRowMin + 1 || row == kGridRowMax - 1) {
        ei.innerRowSigSum += signal;
        if (flagMask & 2)
            ei.innerRowMax = std::max(ei.innerRowMax, signal);
    }
    if (flagMask & 4) {
        ei.minCol = std::min(ei.minCol, static_cast<uint8_t>(col));
        ei.maxCol = std::max(ei.maxCol, static_cast<uint8_t>(col));
        ei.minRow = std::min(ei.minRow, static_cast<uint8_t>(row));
        ei.maxRow = std::max(ei.maxRow, static_cast<uint8_t>(row));
    }
}

// TZ_GetEdgeTouchedFlag — call after BFS completes
inline void TZ_GetEdgeTouchedFlag(ZoneEdgeInfo& ei) {
    ei.edgeFlags = 0;
    if (ei.minCol <= kGridColMin || ei.maxCol >= kGridColMax ||
        ei.minRow <= kGridRowMin || ei.maxRow >= kGridRowMax) {
        ei.edgeFlags |= 0x20;   // touches boundary
    }
    if (ei.minCol < kGridColMin + 2 || ei.maxCol > kGridColMax - 2 ||
        ei.minRow < kGridRowMin + 2 || ei.maxRow > kGridRowMax - 2) {
        ei.edgeFlags |= 0x80000; // within 2px of boundary
    }
}

inline uint8_t TZ_GetCentroidEdgeFlags(const ZoneEdgeInfo& ei, float col, float row) {
    uint8_t flags = 0;
    // 四条边必须对称。远端曾写作 col > kGridColMax / row > kGridRowMax，而质心是 0..N-1
    // 号格的加权平均，数学上到不了 N-1 以上，于是右边和下边的补偿一次也没触发过。
    // 原实现（TSACore Peak_IsOnOuterEdgeDim1Near/Far）按峰的格索引判，near 是 index==0、
    // far 是 index==N-1，严格镜像；换算成质心就是近端 < min+1、远端 > max-1。
    if (ei.minCol <= kGridColMin && col < static_cast<float>(kGridColMin + 1)) flags |= 0x01;
    if (ei.maxCol >= kGridColMax && col > static_cast<float>(kGridColMax - 1)) flags |= 0x02;
    if (ei.minRow <= kGridRowMin && row < static_cast<float>(kGridRowMin + 1)) flags |= 0x04;
    if (ei.maxRow >= kGridRowMax && row > static_cast<float>(kGridRowMax - 1)) flags |= 0x08;
    return flags;
}

template <size_t Rows, size_t Cols>
inline void TZ_GetEdgeWidth(ZoneEdgeInfo& ei,
                            const int16_t (&heatmap)[Rows][Cols],
                            int16_t threshold) {
    if (ei.minColOnOuterRow <= ei.maxColOnOuterRow) {
        int left = ei.minColOnOuterRow;
        int right = ei.maxColOnOuterRow;
        int scannedLeft = left;
        int scannedRight = right;
        while (right < static_cast<int>(Cols) && heatmap[ei.rowAtMaxOuterCol][right] >= threshold) {
            scannedRight = right;
            ++right;
        }
        while (left >= 0 && heatmap[ei.rowAtMinOuterCol][left] >= threshold) {
            scannedLeft = left;
            --left;
        }
        ei.rowEdgeWidth = static_cast<uint8_t>(std::clamp(scannedRight - scannedLeft + 1, 0, 255));
    } else {
        ei.rowEdgeWidth = 0;
    }

    if (ei.minRowOnOuterCol <= ei.maxRowOnOuterCol) {
        int top = ei.minRowOnOuterCol;
        int bottom = ei.maxRowOnOuterCol;
        int scannedTop = top;
        int scannedBottom = bottom;
        while (bottom < static_cast<int>(Rows) && heatmap[bottom][ei.colAtMaxOuterRow] >= threshold) {
            scannedBottom = bottom;
            ++bottom;
        }
        while (top >= 0 && heatmap[top][ei.colAtMinOuterRow] >= threshold) {
            scannedTop = top;
            --top;
        }
        ei.colEdgeWidth = static_cast<uint8_t>(std::clamp(scannedBottom - scannedTop + 1, 0, 255));
    } else {
        ei.colEdgeWidth = 0;
    }
}

// ── CTD_EC LUT and helpers (from firmware) ──

struct ECSegment {
    int touchSizeThreshold;
    int lutIdxLow;
    int lutIdxHigh;
};
struct ECProfile {
    int numSegments;
    ECSegment segments[4];
};

namespace Touch {

// ── g_ctd256Ln[256]: official LUT from firmware ──
static const uint16_t g_ctd256Ln[256] = {
    0x0000, 0x0000, 0x00B1, 0x0119, 0x0162, 0x019C, 0x01CA, 0x01F2,
    0x0214, 0x0232, 0x024D, 0x0265, 0x027C, 0x0290, 0x02A3, 0x02B5,
    0x02C5, 0x02D5, 0x02E3, 0x02F1, 0x02FE, 0x030B, 0x0317, 0x0322,
    0x032D, 0x0338, 0x0342, 0x034B, 0x0355, 0x035E, 0x0366, 0x036F,
    0x0377, 0x037F, 0x0386, 0x038E, 0x0395, 0x039C, 0x03A3, 0x03A9,
    0x03B0, 0x03B6, 0x03BC, 0x03C2, 0x03C8, 0x03CE, 0x03D4, 0x03D9,
    0x03DF, 0x03E4, 0x03E9, 0x03EE, 0x03F3, 0x03F8, 0x03FD, 0x0401,
    0x0406, 0x040B, 0x040F, 0x0413, 0x0418, 0x041C, 0x0420, 0x0424,
    0x0428, 0x042C, 0x0430, 0x0434, 0x0438, 0x043B, 0x043F, 0x0443,
    0x0446, 0x044A, 0x044D, 0x0451, 0x0454, 0x0458, 0x045B, 0x045E,
    0x0461, 0x0464, 0x0468, 0x046B, 0x046E, 0x0471, 0x0474, 0x0477,
    0x047A, 0x047D, 0x047F, 0x0482, 0x0485, 0x0488, 0x048B, 0x048D,
    0x0490, 0x0493, 0x0495, 0x0498, 0x049A, 0x049D, 0x049F, 0x04A2,
    0x04A4, 0x04A7, 0x04A9, 0x04AC, 0x04AE, 0x04B0, 0x04B3, 0x04B5,
    0x04B7, 0x04BA, 0x04BC, 0x04BE, 0x04C0, 0x04C3, 0x04C5, 0x04C7,
    0x04C9, 0x04CB, 0x04CD, 0x04CF, 0x04D1, 0x04D4, 0x04D6, 0x04D8,
    0x04DA, 0x04DC, 0x04DE, 0x04E0, 0x04E1, 0x04E3, 0x04E5, 0x04E7,
    0x04E9, 0x04EB, 0x04ED, 0x04EF, 0x04F1, 0x04F2, 0x04F4, 0x04F6,
    0x04F8, 0x04FA, 0x04FB, 0x04FD, 0x04FF, 0x0501, 0x0502, 0x0504,
    0x0506, 0x0507, 0x0509, 0x050B, 0x050C, 0x050E, 0x0510, 0x0511,
    0x0513, 0x0514, 0x0516, 0x0518, 0x0519, 0x051B, 0x051C, 0x051E,
    0x051F, 0x0521, 0x0522, 0x0524, 0x0525, 0x0527, 0x0528, 0x052A,
    0x052B, 0x052D, 0x052E, 0x052F, 0x0531, 0x0532, 0x0534, 0x0535,
    0x0537, 0x0538, 0x0539, 0x053B, 0x053C, 0x053D, 0x053F, 0x0540,
    0x0541, 0x0543, 0x0544, 0x0545, 0x0547, 0x0548, 0x0549, 0x054B,
    0x054C, 0x054D, 0x054E, 0x0550, 0x0551, 0x0552, 0x0553, 0x0555,
    0x0556, 0x0557, 0x0558, 0x055A, 0x055B, 0x055C, 0x055D, 0x055E,
    0x0560, 0x0561, 0x0562, 0x0563, 0x0564, 0x0565, 0x0567, 0x0568,
    0x0569, 0x056A, 0x056B, 0x056C, 0x056D, 0x056F, 0x0570, 0x0571,
    0x0572, 0x0573, 0x0574, 0x0575, 0x0576, 0x0577, 0x0578, 0x0579,
    0x057B, 0x057C, 0x057D, 0x057E, 0x057F, 0x0580, 0x0581, 0x0582,
    0x0583, 0x0584, 0x0585, 0x0586, 0x0587, 0x0588, 0x0589, 0x058A
};

// 本机面板 W273AS2700 的真表，从 TSAPrmt.dll 读出（docs/tsacore_ground_truth.md）。
// 四张表在 flash+0x840 / +0x851 / +0x862 / +0x873，各 0x11 字节，格式
// {段数, 尺寸阈, ?, lutLo, lutHi}。分法是按**轴**不是按远近端：Dim1 的两张上界
// 都是 224，Dim2 的两张都是 96。此前四份是同一个占位符。
static const ECProfile g_defaultECProfiles[4] = {
    { 1, { {7, 16, 224}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0} } },  // Dim1 近端
    { 1, { {7, 16, 224}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0} } },  // Dim1 远端
    { 1, { {7, 16,  96}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0} } },  // Dim2 近端
    { 1, { {7, 16,  96}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0} } },  // Dim2 远端
};

// TZ_CentroidGripRatio —— 最外一圈信号和相对次外一圈的陡峭程度。
// 接触点被边界切得越深，最外一圈相对次外一圈越强，比值越大；这正是「陷进去多少」
// 的可观测量，也是边缘补偿真正的输入。分母为零时视作比例拉满，不是视作零——
// 只有最外圈有信号意味着陷得最深。
static inline uint8_t TZ_CentroidGripRatio(int outerSigSum, int innerSigSum) {
    if (outerSigSum <= 0) return 0;
    if (innerSigSum <= 0) return 0xFF;
    const int ratio = (outerSigSum << 4) / innerSigSum;
    return static_cast<uint8_t>(std::min(ratio, 0xFF));
}

static inline int ECGetOffset(uint8_t gripRatio, uint8_t touchSize,
                               const ECProfile& prof) {
    int si = 0;
    const int segmentCount = std::clamp(prof.numSegments, 1, 4);
    while (si < segmentCount - 1 &&
           prof.segments[si].touchSizeThreshold < touchSize)
        si++;
    auto& s = prof.segments[si];
    const int hiIdx = std::clamp(s.lutIdxHigh, 0, 255);
    const int loIdx = std::clamp(s.lutIdxLow, 0, 255);
    uint16_t hi = g_ctd256Ln[hiIdx];
    uint16_t lo = g_ctd256Ln[loIdx];
    if (hi == lo) return 0;
    // 只钳上界。下界不钳是原实现的行为：比例低于 lutLo 时 offset 为负，compOff 因而
    // 大于一格，接触点被放得比一格更靠里。钳到 0 会把这一段行为抹平。
    int r = (int(g_ctd256Ln[gripRatio] - lo) * 0x100) / int(hi - lo);
    return std::min(r, 0xFF);
}

static inline int ECGetFinalOffset(int rawDist, int compOff,
                                   int blendStart, int blendWidth) {
    if (rawDist < 0) return 0;
    const int start = std::max(1, blendStart);
    const int width = std::max(1, blendWidth);
    if (rawDist <= start) return compOff;
    if (rawDist >= start + width) return rawDist;
    const int t = ((rawDist - start) * 0x100) / width;
    int blended = rawDist * t + compOff * (0x100 - t);
    if (blended < 0) blended += 0xff;
    return blended >> 8;
}

enum class ECEdge : uint8_t {
    Dim1Near = 0,
    Dim1Far = 1,
    Dim2Near = 2,
    Dim2Far = 3,
};

struct ECDimResult {
    bool active = false;
    bool corrected = false;
    bool nearEdge = false;
    bool farEdge = false;
    uint8_t edgeWidth = 0;
    int rawDistQ8 = 0;
    int finalOffQ8 = 0;
    float correctedDistance = 0.0f;
};

class EdgeCompensator {
public:
    bool m_enabled = true;
    EdgeBounds m_bounds;
    float m_ecStrength = 1.0f;
    // TSACore 的 CTD_ECGetFinalOffset 是硬编码的：
    //
    //     t = rawDist - 0x100;                    // 0x100 = 1 格 (Q8)
    //     if (t <= 0)    return compOff;          // 全量区：距边 ≤ 1.0 格
    //     if (t >= 0x40) return rawDist;          // 恒等区：距边 ≥ 1.25 格
    //     return (rawDist*t*4 + (0x100 - 4*t)*compOff) >> 8;   // 0.25 格线性过渡
    //
    // 我们的 ECGetFinalOffset 与它逐句等价，代入 start=0x100、width=0x40 后
    // t 的算式恰好化成同一个 4*t，所以这两个常量取真值即复现原实现。
    //
    // 这两个值曾被反向调过两次（full 收到 0.08、blend 放到 2.0），因为当时 compOff
    // 的输入喂错了：喂的是次轴归一化位置，沿被补偿轴移动不变，全量区退化成常量输出，
    // 于是坐标在边缘冻住。输入已改回 grip 比例（见 ProcessDim），常量随之回到真值。
    float m_ecFullCompRange = 1.0f;
    float m_ecBlendRange = 0.25f;
    ECProfile m_profiles[4] = {
        g_defaultECProfiles[0],
        g_defaultECProfiles[1],
        g_defaultECProfiles[2],
        g_defaultECProfiles[3],
    };

    // ── 统一签名入口：从 frame.touch.runtime 读取 edgeInfos/edgeBounds ──
    inline void Process(HeatmapFrame& frame) {
        const auto& rt = frame.touch.runtime;
        if (rt.edgeBounds) {
            Process(frame.touch.output.contacts.span(), rt.edgeInfos, *rt.edgeBounds);
        }
    }

    inline void Process(std::span<TouchContact> contacts,
                        std::span<const ZoneEdgeInfo> edgeInfos,
                        const EdgeBounds& bounds) {
        if (!m_enabled) return;
        for (int i = 0; i < static_cast<int>(contacts.size()); ++i) {
            if (i >= static_cast<int>(edgeInfos.size())) break;
            auto& tc = contacts[static_cast<size_t>(i)];
            const auto& ei = edgeInfos[static_cast<size_t>(i)];

            // matchXCells/matchYCells 由 ZoneExpander::ApplyEdgeInfo 在接触点生成时写好，
            // 这里只改 x/y。两份坐标的分工：前者供跟踪级配对，后者是对外上报的量。
            tc.edgeFlags |= ei.edgeFlags;
            tc.centroidEdgeFlags |= TZ_GetCentroidEdgeFlags(ei, tc.x, tc.y);

            // Centroid-distance fallback: only arm compensation inside the outermost sensor cell.
            constexpr float kEdgeDistFallbackCell = 1.0f;
            const float dLeft   = tc.x - bounds.colMin;
            const float dRight  = bounds.colMax - tc.x;
            const float dTop    = tc.y - bounds.rowMin;
            const float dBottom = bounds.rowMax - tc.y;
            if (tc.centroidEdgeFlags == 0) {
                if (dLeft   < kEdgeDistFallbackCell) tc.centroidEdgeFlags |= 0x01;
                if (dRight  < kEdgeDistFallbackCell) tc.centroidEdgeFlags |= 0x02;
                if (dTop    < kEdgeDistFallbackCell) tc.centroidEdgeFlags |= 0x04;
                if (dBottom < kEdgeDistFallbackCell) tc.centroidEdgeFlags |= 0x08;
            }

            const bool edge = (tc.edgeFlags & (0x20 | 0x80000)) != 0 ||
                              tc.centroidEdgeFlags != 0;
            if (!edge) continue;

            tc.isEdge = true;
            tc.ecFlags &= ~(0x100u | 0x200u);

            ECDimResult xResult = ProcessDim(tc.x, bounds.colMin, bounds.colMax,
                                             ei, tc.centroidEdgeFlags,
                                             0x01, 0x02,
                                             ECEdge::Dim1Near, ECEdge::Dim1Far,
                                             tc.sizeMm,
                                             tc.gripRatioXQ4);
            ECDimResult yResult = ProcessDim(tc.y, bounds.rowMin, bounds.rowMax,
                                             ei, tc.centroidEdgeFlags,
                                             0x04, 0x08,
                                             ECEdge::Dim2Near, ECEdge::Dim2Far,
                                             tc.sizeMm,
                                             tc.gripRatioYQ4);

            if (xResult.active) {
                tc.ecWidthX = xResult.edgeWidth;
                tc.edgeDistXCells = xResult.correctedDistance;
                if (xResult.corrected) tc.ecFlags |= 0x100;
            }
            if (yResult.active) {
                tc.ecWidthY = yResult.edgeWidth;
                tc.edgeDistYCells = yResult.correctedDistance;
                if (yResult.corrected) tc.ecFlags |= 0x200;
            }
        }
    }

private:
    inline ECDimResult ProcessDim(float& coord,
                                  float boundNear,
                                  float boundFar,
                                  const ZoneEdgeInfo& ei,
                                  uint8_t centroidFlags,
                                  uint8_t nearMask,
                                  uint8_t farMask,
                                  ECEdge nearProfile,
                                  ECEdge farProfile,
                                  float touchSizeMm,
                                  uint8_t gripRatio) const {
        ECDimResult result;
        result.nearEdge = (centroidFlags & nearMask) != 0;
        result.farEdge = (centroidFlags & farMask) != 0;
        if (!result.nearEdge && !result.farEdge) return result;

        const int q8coord = static_cast<int>(coord * 256.0f);
        const int nearDist = std::max(0, q8coord - static_cast<int>(boundNear * 256.0f));
        const int farDist = std::max(0, static_cast<int>(boundFar * 256.0f) - q8coord);
        const bool useFar = result.farEdge && (!result.nearEdge || farDist < nearDist);
        result.rawDistQ8 = useFar ? farDist : nearDist;
        result.active = true;

        const bool isDimX = nearMask == 0x01;
        result.edgeWidth = isDimX ? ei.colEdgeWidth : ei.rowEdgeWidth;

        // LUT 索引是 grip 比例，不是位置。原实现 CTD_ECGetOffset 的第一个参数取自
        // 触点结构 +0x196（Dim1）/ +0x197（Dim2），而那两个字节由 TZ_RegisterTouch
        // 从 TZ_GetGripRatio 算出的比例搬过来。近端与远端共用同一个比例——原实现
        // 两个分支传的是同一个字段，累加器也把两侧边缘算进同一个和。
        const int profileIndex = static_cast<int>(useFar ? farProfile : nearProfile);
        const uint8_t touchSizeByte = static_cast<uint8_t>(std::min(touchSizeMm, 255.0f));
        const int offset = ECGetOffset(gripRatio, touchSizeByte, m_profiles[profileIndex]);
        const int compOff = 256 - offset;
        const int blendStart = std::max(1, static_cast<int>(m_ecFullCompRange * 256.0f));
        const int blendWidth = std::max(1, static_cast<int>(m_ecBlendRange * 256.0f));
        const int targetOffQ8 = ECGetFinalOffset(result.rawDistQ8, compOff, blendStart, blendWidth);
        const float strength = std::clamp(m_ecStrength, 0.0f, 1.0f);
        const int deltaQ8 = targetOffQ8 - result.rawDistQ8;
        result.finalOffQ8 = result.rawDistQ8 + static_cast<int>(static_cast<float>(deltaQ8) * strength);
        result.correctedDistance = static_cast<float>(result.finalOffQ8) / 256.0f;

        const int oldQ8 = q8coord;
        if (useFar) {
            coord = boundFar - result.correctedDistance;
        } else {
            coord = boundNear + result.correctedDistance;
        }
        const int newQ8 = static_cast<int>(coord * 256.0f);
        result.corrected = newQ8 != oldQ8;
        return result;
    }
};

// 判定「接触点贴在边界上，且 EC 没能把坐标从边界拉开」——这种点的坐标是钉死的，不可信。
// 这里只写 TouchContact::edgeRejected，后果交给 TouchTracker：拒绝只在建轨迹那一帧生效，
// 已经在报的轨迹不中途掐断，否则贴边滑动会碎成一串 up/down。
class EdgeRejector {
public:
    bool m_enabled = true;
    int  m_edgeMargin = 2;

    // ── 统一签名入口：从 frame.touch.runtime 读取 edgeInfos/edgeBounds ──
    inline void Process(HeatmapFrame& frame) {
        const auto& rt = frame.touch.runtime;
        if (rt.edgeBounds) {
            Process(frame.touch.output.contacts.span(), rt.edgeInfos, *rt.edgeBounds);
        } else {
            for (auto& tc : frame.touch.output.contacts) tc.edgeRejected = false;
        }
    }

    inline void Process(std::span<TouchContact> contacts,
                        std::span<const ZoneEdgeInfo> edgeInfos,
                        const EdgeBounds& bounds) {
        // edgeRejected 每帧对每个接触点无条件写一次，包括本级停用时。FixedVector::clear()
        // 只重置长度，底层数组里的旧对象原样留着，漏写一个槽位就会把上一帧的拒绝结论带进来。
        for (auto& tc : contacts) tc.edgeRejected = false;
        if (!m_enabled) return;

        for (int i = 0; i < static_cast<int>(contacts.size()); ++i) {
            if (i >= static_cast<int>(edgeInfos.size())) break;
            auto& tc = contacts[static_cast<size_t>(i)];
            const auto& ei = edgeInfos[static_cast<size_t>(i)];
            const uint8_t centroidFlags = tc.centroidEdgeFlags | TZ_GetCentroidEdgeFlags(ei, tc.x, tc.y);
            const uint32_t edgeFlags = tc.edgeFlags | ei.edgeFlags;
            if ((edgeFlags & 0x20) == 0 && centroidFlags == 0) continue;

            const bool xEdge = (centroidFlags & 0x03) != 0;
            const bool yEdge = (centroidFlags & 0x0c) != 0;
            const float fallbackXDist = std::min(tc.x - bounds.colMin, bounds.colMax - tc.x);
            const float fallbackYDist = std::min(tc.y - bounds.rowMin, bounds.rowMax - tc.y);
            const float xDist = tc.edgeDistXCells > 0.0f ? tc.edgeDistXCells : fallbackXDist;
            const float yDist = tc.edgeDistYCells > 0.0f ? tc.edgeDistYCells : fallbackYDist;
            const bool uncorrectedX = xEdge && (tc.ecFlags & 0x100) == 0;
            const bool uncorrectedY = yEdge && (tc.ecFlags & 0x200) == 0;
            const bool stillPinned = (uncorrectedX && xDist <= static_cast<float>(m_edgeMargin)) ||
                                     (uncorrectedY && yDist <= static_cast<float>(m_edgeMargin));

            // 这里曾经写作 `if (tc.state == 0 && stillPinned)`，本意是「只在落下那一帧拒绝」。
            // 但本级跑在 tracker 之前，state 还没有任何上游赋值，恒为默认的 0，条件实际恒真；
            // 而紧接着的 isReported = false 又被 tracker 无条件重写，两个错误相互抵消，整个
            // EdgeRejector 是死代码。判定移到这里、后果移到 tracker，两处都不再依赖 state。
            tc.edgeRejected = stillPinned;
        }
    }
};

}} // namespace Solvers::Touch
