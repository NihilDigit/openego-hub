#pragma once
// ── TouchPipeline Module: PeakDetector ──
// Header-only. Faithful replica of TouchSolver/PeakDetector.{h,cpp}.
// TSACore Peak_Process: detect local maxima, filter, sort, track IDs.

#include "SolverTypes.h"
#include <array>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <span>

namespace Solvers { namespace Touch {

class PeakDetector {
public:
    static constexpr int kMaxStoredPeaks = 100;

    int  m_threshold = 280;
    int  m_sigTholdLimit = 1000;
    bool m_z8Filter = true;
    bool m_z1Filter = true;
    bool m_pressureDriftFilter = true;
    bool m_edgePeakFilter = true;
    bool m_edgeThresholdEnabled = true;
    int  m_edgeThreshold = 300;
    int  m_z8Radius = 1;
    int  m_localMaxRadius = 1;
    bool m_closePeakSaddleFilter = true;
    // 同一个宏区里挨得这么近的两个峰,谷不够深就并成一个。
    //
    // 半径 3、比例 0.50 是在「掌搭着 + 同手拇指滑动」的录制上标定的
    // (`dvr20260824_170138`)。拇指的接触斑不是一个圆点,滑动时会分出两三个脊,原来的
    // 0.08 太松:两个 1520 的峰只要谷降到 1398 就都留下,于是同一根拇指出两三个接触点,
    // 而且轨迹在它们之间来回换号——主机看到的是几根手指在原地抖。
    //
    // 0.50 之后拇指语料的双点帧 88 → 12、断连 2 → 0,`dvr20260824_152555` 的断连
    // 15 → 0。真双指不受影响:`dvr20260824_012458` 的双点帧 2081 → 2074、上报帧数
    // 与首次出现数一格未变,而那份录制里两点最近到 0.7 格,并非只有远距样本。
    int  m_closePeakRadius = 3;
    int  m_closePeakMinSaddleDrop = 80;
    float m_closePeakMinSaddleRatio = 0.50f;
    int  m_maxPeaks = 20;
    // 宏区面积的起始判据。手指滑向面板边缘时超阈格数会从 3 掉到 2 再到 1，
    // 硬阈值在这里没有迟滞，每掉一帧峰就整个消失，轨迹随之进 SilentGap 不上报，
    // OS 看到触点消失、重连后再发一次 down。
    int  m_macroZoneMinArea = 3;
    // 维持判据：上一帧已经有轨迹压在这个峰上时改用它。起始严、维持松，
    // 边缘那几格的抖动就不再打断已经成立的笔画。
    int  m_macroZoneHoldArea = 1;
    // 峰要落在轨迹的这个半径内才算「同一个接触点」，单位是传感器格。
    // 报点率 122 Hz 下手指一帧走不到两格，所以 2 格足够而且不会误留邻近噪声。
    float m_trackHoldRadius = 2.0f;

    // ── 统一签名入口：从 frame.touch.runtime 读取参数，结果写回 runtime ──
    inline void Process(HeatmapFrame& frame) {
        if (frame.touch.runtime.macroZones) {
            Detect(frame, *frame.touch.runtime.macroZones,
                   frame.touch.runtime.prevTrackAnchors);
        }
        frame.touch.runtime.peaks = GetPeaks();
    }

    // ────────────────────────────────────────────────────────
    // Public entry — mirrors TSACore Peak_Process()
    // ────────────────────────────────────────────────────────
    inline void Detect(const HeatmapFrame& frame,
                       const std::vector<MacroZone>& macroZones,
                       std::span<const TrackAnchor> prevTrackAnchors = {}) {
        // Step 1: DetectInRange — asymmetric local max
        DetectInRange(frame, macroZones);

        if (m_closePeakSaddleFilter) ApplyClosePeakSaddleFilter(frame);

        // Step 2: Z8 Filter — signal >> 5 > neighborSignalSum → remove
        if (m_z8Filter) ApplyZ8Filter(std::clamp(m_z8Radius, 1, 5), prevTrackAnchors);

        // Step 3: Z1 Filter — signal < threshold → remove
        if (m_z1Filter) ApplyZ1Filter(m_threshold);

        // Step 3.5: MacroZone areaCells filter — 起始判据 m_macroZoneMinArea，
        // 上一帧已有轨迹压着的峰改用维持判据 m_macroZoneHoldArea。
        if (m_macroZoneMinArea > 1) {
            const int holdArea = std::clamp(m_macroZoneHoldArea, 1, m_macroZoneMinArea);
            const float holdRadiusSq = m_trackHoldRadius * m_trackHoldRadius;
            CompactPeaks([&](const Peak& p) {
                const int required =
                    HasTrackWithin(prevTrackAnchors, p, holdRadiusSq) ? holdArea
                                                                      : m_macroZoneMinArea;
                return p.macroZoneArea < required;
            });
        }

        // Step 4: Edge peak filter — weak edge peaks < maxSig*5/8
        if (m_edgePeakFilter) ApplyEdgePeakFilter(frame);

        // Step 5: Sort ascending by signal (TSACore default)
        SortPeaks();

        // Step 6: Cap to max
        m_peakCount = std::min(m_peakCount, EffectiveMaxPeaks());

        // Step 7: Peak_IDTracking — assign persistent IDs
        TrackPeakIDs();
    }

    std::span<const Peak> GetPeaks() const {
        return std::span<const Peak>(m_peaks.data(),
                                     static_cast<size_t>(m_peakCount));
    }

private:
    static constexpr int kRows = 40;
    static constexpr int kCols = 60;

    struct PressureDriftRowStats {
        int gradientSum = 0;
        int signalSum = 0;
        bool hasSharpSpike = false;
    };

    std::array<Peak, kMaxStoredPeaks> m_peaks{};
    std::array<Peak, kMaxStoredPeaks> m_prevPeaks{};
    std::array<PressureDriftRowStats, kRows> m_pressureRows{};
    std::array<uint8_t, kRows> m_pressureRowsReady{};
    int m_peakCount = 0;
    int m_prevPeakCount = 0;
    uint8_t m_nextPeakId = 1;

    inline int EffectiveMaxPeaks() const {
        return std::clamp(m_maxPeaks, 1, kMaxStoredPeaks);
    }

    // 轨迹位置是 (x=列, y=行)，峰是 (r=行, c=列)，两边的轴序相反。
    static inline bool HasTrackWithin(std::span<const TrackAnchor> anchors,
                                      const Peak& p, float radiusSq) {
        for (const auto& a : anchors) {
            const float dc = a.x - static_cast<float>(p.c);
            const float dr = a.y - static_cast<float>(p.r);
            if (dc * dc + dr * dr <= radiusSq) return true;
        }
        return false;
    }

    template <typename Pred>
    inline void CompactPeaks(Pred&& pred) {
        int writeIdx = 0;
        for (int i = 0; i < m_peakCount; ++i) {
            if (pred(m_peaks[static_cast<size_t>(i)])) {
                continue;
            }
            if (writeIdx != i) {
                m_peaks[static_cast<size_t>(writeIdx)] =
                    m_peaks[static_cast<size_t>(i)];
            }
            ++writeIdx;
        }
        m_peakCount = writeIdx;
    }

    // ────────────────────────────────────────────────────────
    // Peak_DetectInRange — TSACore asymmetric 8-neighbor
    // Down neighbors: signal[n] <= peak  (allows equal)
    // Up+Left neighbors: signal[n] < peak (strict)
    // ────────────────────────────────────────────────────────
    inline void DetectInRange(const HeatmapFrame& frame,
                              const std::vector<MacroZone>& macroZones) {
        m_peakCount = 0;
        const int maxPeaks = EffectiveMaxPeaks();
        int weakIdx = -1;
        if (m_pressureDriftFilter) {
            m_pressureRowsReady.fill(0);
        }

        auto refreshWeakIdx = [&]() {
            weakIdx = 0;
            for (int k = 1; k < maxPeaks; ++k) {
                if (m_peaks[static_cast<size_t>(k)].z <
                    m_peaks[static_cast<size_t>(weakIdx)].z) {
                    weakIdx = k;
                }
            }
        };

        const int colEnd = kCols - 1;
        const int localRadius = std::clamp(m_localMaxRadius, 1, 5);
        const int z8Radius = std::clamp(m_z8Radius, 1, 5);

        auto val = [&](int r, int c) -> int16_t {
            return frame.touch.conditioned[r][c];
        };

        for (int zi = 0; zi < static_cast<int>(macroZones.size()); ++zi) {
            const auto& zone = macroZones[static_cast<size_t>(zi)];
            for (int idx : zone.pixels) {
                int r = idx / kCols;
                int c = idx % kCols;

                const int16_t v = val(r, c);

                // Edge-specific threshold (TSACore: g_toeSigThold)
                int16_t thold = m_threshold;
                if (m_edgeThresholdEnabled &&
                    (c == 1 || c == colEnd - 1 || r == kRows - 1)) {
                    thold = m_edgeThreshold;
                }
                if (v < thold) continue;

                bool isPeak = true;
                for (int dr = -localRadius; dr <= localRadius && isPeak; ++dr) {
                    for (int dc = -localRadius; dc <= localRadius; ++dc) {
                        if (dr == 0 && dc == 0) continue;
                        int nr = r + dr;
                        int nc = c + dc;
                        if (nr >= 0 && nr < kRows && nc >= 0 && nc < kCols) {
                            int16_t nv = val(nr, nc);
                            bool after = (dr > 0) || (dr == 0 && dc > 0);
                            if (after) {
                                if (nv > v) { isPeak = false; break; }
                            } else {
                                if (nv >= v) { isPeak = false; break; }
                            }
                        }
                    }
                }
                if (!isPeak) continue;

                int nbrSigSum = 0;
                int nbrCellCount = 0;
                for (int dr = -z8Radius; dr <= z8Radius; ++dr) {
                    for (int dc = -z8Radius; dc <= z8Radius; ++dc) {
                        if (dr == 0 && dc == 0) continue;
                        int nr = r + dr;
                        int nc = c + dc;
                        if (nr >= 0 && nr < kRows && nc >= 0 && nc < kCols) {
                            nbrSigSum += val(nr, nc);
                            ++nbrCellCount;
                        }
                    }
                }

                // PressureDrift check
                if (m_pressureDriftFilter && DetectPressureDrift(frame, c, r)) {
                    continue;
                }

                // TSACore Peak_Insert: cap at m_maxPeaks, replace weakest
                Peak peak;
                peak.r = r;
                peak.c = c;
                peak.z = v;
                peak.neighborSignalSum = nbrSigSum;
                peak.neighborCellCount = nbrCellCount;
                peak.macroZoneIndex = zi;
                peak.macroZoneArea = zone.areaCells;
                if (m_peakCount < maxPeaks) {
                    m_peaks[static_cast<size_t>(m_peakCount++)] = peak;
                    if (m_peakCount == maxPeaks) refreshWeakIdx();
                } else if (m_peaks[static_cast<size_t>(weakIdx)].z < v) {
                    m_peaks[static_cast<size_t>(weakIdx)] = peak;
                    refreshWeakIdx();
                }
            }
        }
    }

    inline int EstimateSaddleSignal(const HeatmapFrame& frame,
                                    const Peak& a,
                                    const Peak& b) const {
        const int rowDelta = b.r - a.r;
        const int colDelta = b.c - a.c;
        const int steps = std::max(std::abs(rowDelta), std::abs(colDelta));
        auto roundStep = [steps](int delta, int step) {
            const int numerator = delta * step;
            return numerator >= 0
                ? (numerator + steps / 2) / steps
                : (numerator - steps / 2) / steps;
        };
        int saddle = 0;
        for (int step = 1; step < steps; ++step) {
            const int r = a.r + roundStep(rowDelta, step);
            const int c = a.c + roundStep(colDelta, step);
            if (r < 0 || r >= kRows || c < 0 || c >= kCols) continue;
            if ((r == a.r && c == a.c) || (r == b.r && c == b.c)) continue;
            saddle = std::max(saddle, static_cast<int>(frame.touch.conditioned[r][c]));
        }
        return saddle;
    }

    inline void CompactPeaksBySuppression(const std::array<bool, kMaxStoredPeaks>& suppress) {
        int writeIdx = 0;
        for (int i = 0; i < m_peakCount; ++i) {
            if (suppress[static_cast<size_t>(i)]) continue;
            if (writeIdx != i) {
                m_peaks[static_cast<size_t>(writeIdx)] = m_peaks[static_cast<size_t>(i)];
            }
            ++writeIdx;
        }
        m_peakCount = writeIdx;
    }

    inline void ApplyClosePeakSaddleFilter(const HeatmapFrame& frame) {
        if (m_peakCount <= 1) return;
        const int closeRadius = std::max(1, m_closePeakRadius);
        std::array<bool, kMaxStoredPeaks> suppress{};
        suppress.fill(false);

        for (int i = 0; i < m_peakCount; ++i) {
            if (suppress[static_cast<size_t>(i)]) continue;
            const auto& a = m_peaks[static_cast<size_t>(i)];
            for (int j = i + 1; j < m_peakCount; ++j) {
                if (suppress[static_cast<size_t>(j)]) continue;
                const auto& b = m_peaks[static_cast<size_t>(j)];
                if (a.macroZoneIndex != b.macroZoneIndex) continue;
                const int rowDist = std::abs(a.r - b.r);
                const int colDist = std::abs(a.c - b.c);
                if (std::max(rowDist, colDist) > closeRadius) continue;

                const int weakerIdx = (a.z <= b.z) ? i : j;
                const int weakerSignal = static_cast<int>(m_peaks[static_cast<size_t>(weakerIdx)].z);
                const int saddle = EstimateSaddleSignal(frame, a, b);
                const int requiredDrop = std::max(
                    m_closePeakMinSaddleDrop,
                    static_cast<int>(static_cast<float>(weakerSignal) * m_closePeakMinSaddleRatio));
                if (weakerSignal - saddle < requiredDrop) {
                    suppress[static_cast<size_t>(weakerIdx)] = true;
                    if (weakerIdx == i) break;
                }
            }
        }

        CompactPeaksBySuppression(suppress);
    }

    // ────────────────────────────────────────────────────────
    // PressureDrift_Detect — TSACore gradient-based palm press
    // Returns true if the peak looks like a flat palm press.
    // ────────────────────────────────────────────────────────
    inline void BuildPressureDriftRowStats(
            const HeatmapFrame& frame,
            int row,
            PressureDriftRowStats& stats) const {
        stats = {};
        const int sharpSpikeLimit = m_sigTholdLimit / 3;
        for (int c = 1; c < kCols - 1; ++c) {
            const int grad = std::abs(static_cast<int>(frame.touch.conditioned[row][c + 1])
                                    - static_cast<int>(frame.touch.conditioned[row][c - 1]));
            stats.hasSharpSpike = stats.hasSharpSpike || grad > sharpSpikeLimit;
            stats.gradientSum += grad;
            if (frame.touch.conditioned[row][c] > 0) {
                stats.signalSum += frame.touch.conditioned[row][c];
            }
        }
    }

    inline bool DetectPressureDrift(const HeatmapFrame& frame,
                                    int col,
                                    int row) {
        const int16_t peakSig = frame.touch.conditioned[row][col];
        const int16_t limit3_4 = static_cast<int16_t>(m_sigTholdLimit * 3 / 4);
        const int16_t limit3_8 = static_cast<int16_t>(m_sigTholdLimit * 3 / 8);

        if (peakSig > limit3_4 || peakSig < limit3_8) return false;

        if (m_pressureRowsReady[static_cast<size_t>(row)] == 0) {
            BuildPressureDriftRowStats(frame, row, m_pressureRows[static_cast<size_t>(row)]);
            m_pressureRowsReady[static_cast<size_t>(row)] = 1;
        }
        const auto& stats = m_pressureRows[static_cast<size_t>(row)];
        if (stats.hasSharpSpike) return false;
        return (stats.signalSum >= peakSig * 9 / 2) &&
               (peakSig * 6 >= stats.gradientSum);
    }

    // ────────────────────────────────────────────────────────
    // Peak_Z8Filter — signal >> 5 > neighborSignalSum → remove
    // Isolated spikes: strong peak but neighbors sum is small
    // ────────────────────────────────────────────────────────
    // 孤峰判据:峰值的 1/32 都比周围一圈的和还大,说明它没有支撑,是噪声尖刺。
    //
    // 贴角的接触点过不了这一关:邻域被面板边界切掉大半(角上只剩三格),而露在面板内的
    // 那几格本身也几乎没信号——手指的其余部分压根不在面板上。实测四边语料上最后那 67 帧
    // 漏报正是死在这里,信号一直在、宏区也一直在,就是被这条当噪声删了。
    //
    // 先试过「按实际邻居数把和折算回一整圈」,没用:和本来就接近零,乘个 8/3 还是够不着。
    // 所以改成迟滞——邻域被切、且上一帧确实有轨迹压在这里,就不当噪声。新出现的孤立
    // 尖刺没有轨迹,照样被删。
    inline void ApplyZ8Filter(int z8Radius, std::span<const TrackAnchor> prevTrackAnchors) {
        const int fullNeighborCount = (2 * z8Radius + 1) * (2 * z8Radius + 1) - 1;
        const float holdRadiusSq = m_trackHoldRadius * m_trackHoldRadius;
        CompactPeaks([&](const Peak& p) {
            if ((p.z >> 5) <= p.neighborSignalSum) return false;
            // 邻域被面板边界切掉了一部分,而上一帧确实有轨迹压在这里:那就不是噪声尖刺,
            // 是一个只剩一格露在面板内的真接触点。判据与宏区面积那条用同一个半径。
            const bool clipped = p.neighborCellCount > 0 &&
                                 p.neighborCellCount < fullNeighborCount;
            if (clipped && HasTrackWithin(prevTrackAnchors, p, holdRadiusSq)) return false;
            return true;
        });
    }

    // ────────────────────────────────────────────────────────
    // Peak_Z1Filter — remove peaks with signal < threshold
    // ────────────────────────────────────────────────────────
    inline void ApplyZ1Filter(int16_t thold) {
        CompactPeaks([thold](const Peak& p) { return p.z < thold; });
    }

    // ────────────────────────────────────────────────────────
    // EdgePeakFilter_WorkAround — remove weak edge peaks
    // W273AS2700 uses the first/last column path.
    // ────────────────────────────────────────────────────────
    inline void ApplyEdgePeakFilter(const HeatmapFrame& frame) {
        auto filterEdgeColumn = [this, &frame](int col) {
            int16_t maxSig = 0;
            for (int row = 0; row < kRows; ++row) {
                maxSig = std::max(maxSig, frame.touch.conditioned[row][col]);
            }
            if (maxSig == 0) return;
            const int16_t cutoff = static_cast<int16_t>((maxSig >> 3) * 5);
            CompactPeaks([&](const Peak& p) {
                return p.c == col && p.z < cutoff;
            });
        };

        filterEdgeColumn(0);
        filterEdgeColumn(kCols - 1);
    }

    // ────────────────────────────────────────────────────────
    // Peak sort — ascending by signal (TSACore default)
    // ────────────────────────────────────────────────────────
    inline void SortPeaks() {
        std::sort(m_peaks.begin(), m_peaks.begin() + m_peakCount,
            [](const Peak& a, const Peak& b) { return a.z < b.z; });
    }

    // ────────────────────────────────────────────────────────
    // Peak_IDTracking — assign persistent IDs across frames.
    // TSACore uses IDT_Process(1) (Hungarian); we use greedy
    // nearest-neighbor which is sufficient for peak-level tracking.
    // ────────────────────────────────────────────────────────
    inline void TrackPeakIDs() {
        if (m_prevPeakCount == 0) {
            // First frame: assign fresh IDs
            for (int i = 0; i < m_peakCount; ++i)
                m_peaks[static_cast<size_t>(i)].id = m_nextPeakId++;
            std::copy_n(m_peaks.begin(), m_peakCount, m_prevPeaks.begin());
            m_prevPeakCount = m_peakCount;
            return;
        }

        // Mark which prev peaks have been matched
        std::array<bool, kMaxStoredPeaks> prevUsed{};
        prevUsed.fill(false);

        for (int i = 0; i < m_peakCount; ++i) {
            auto& pk = m_peaks[static_cast<size_t>(i)];
            int bestDist = 9999;
            int bestIdx = -1;
            for (int j = 0; j < m_prevPeakCount; ++j) {
                if (prevUsed[static_cast<size_t>(j)]) continue;
                const auto& prevPk = m_prevPeaks[static_cast<size_t>(j)];
                int dist = std::abs(pk.r - prevPk.r)
                         + std::abs(pk.c - prevPk.c);
                if (dist < bestDist) {
                    bestDist = dist;
                    bestIdx = j;
                }
            }
            // Match if within 3 cells Manhattan distance
            if (bestIdx >= 0 && bestDist <= 3) {
                const auto& prevPk = m_prevPeaks[static_cast<size_t>(bestIdx)];
                pk.id = prevPk.id;
                pk.tzAge = prevPk.tzAge + 1; // TZ_UpdatePeakTzAge
                prevUsed[static_cast<size_t>(bestIdx)] = true;
            } else {
                pk.id = m_nextPeakId++;
                pk.tzAge = 0;
            }
        }
        std::copy_n(m_peaks.begin(), m_peakCount, m_prevPeaks.begin());
        m_prevPeakCount = m_peakCount;
    }
};

}} // namespace Solvers::Touch
