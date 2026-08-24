#pragma once
// ══════════════════════════════════════════════════════════════════════
// BaselineTracker — Per-cell adaptive baseline subtraction with
// Q8.24 fixed-point IIR tracking, freeze/release-hold state machine,
// and aggressive recovery on finger-state transitions.
//
// Finger state is a simple bool (hasFinger), determined externally from
// the master suffix. The module makes no distinction between Unknown
// and NoFinger — it only cares whether a finger is present right now.
//
// Baseline is INHERITED across lid/display/idle state changes; only
// the very first frame or an explicit Reset() reinitializes from the
// configured default value.
// ══════════════════════════════════════════════════════════════════════

#include "SolverTypes.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace Solvers { namespace Touch {

class BaselineTracker {
public:
    static constexpr int kRows = 40;
    static constexpr int kCols = 60;
    static constexpr int kCellCount = kRows * kCols;

    // ── Public configurable parameters ────────────────────────────

    bool m_enabled = true;

    // Initial baseline value in ADC units (0..65535).
    // Applied only on Initialize() or Reset().
    int  m_baseline = 0x7FEE;               // 32750

    // Deadband width: |delta| ≤ this is treated as noise.
    int  m_noiseDeadband = 90;              // 0..200

    // Thresholds for the three-tier IIR classification.
    // positiveDeadband: delta above this enters fast-positive tier.
    // negativeDeadband: delta below -this enters fast-negative tier;
    //                   also gates negative escape during release hold.
    int  m_positiveDeadband = 14;            // 0..200
    int  m_negativeDeadband = 13;            // 0..200

    // Freeze threshold: localDiff above this locks the cell's baseline.
    int  m_peakThreshold = 305;              // 1..2000

    // 维持门槛:落在已有轨迹附近的格用这个更低的门槛冻结,其余格仍用 m_peakThreshold。
    //
    // 为什么需要:被边界切掉的接触点信号只有正常按压的几分之一。实测「手指拖到角上
    // 停住」时,四边语料有 676 帧我们一个接触点都不报,而厂商一直在报——那几格的
    // localDiff 落在 305 以下,冻结判据够不着,于是被背景更新吸收成 0。
    //
    // 为什么按轨迹位置而不是按「这个格自己冻结过」:手指移动时不断进入新的格,新格
    // 没有冻结史,逐格迟滞对移动中的接触点不起作用。实测逐格迟滞只把 676 降到 496,
    // 而全局降门槛到 240 能降到 386——差距正是这个。
    //
    // 噪声进不来:低门槛只在上一帧确实有轨迹的地方生效,而轨迹本身要先越过完整的
    // 检测链才建得起来。
    int  m_sustainThreshold = 100;           // 1..2000

    // 轨迹锚点周围多少格内适用维持门槛。
    float m_sustainRadiusCells = 2.5f;       // 0..10

    // 只有距边界这么近的轨迹才降低门槛。物理理由是边界把接触点切掉了一部分,信号
    // 因此偏低;屏幕中间的接触点没有这个问题,不该跟着放宽——掌就落在中间,放宽它
    // 只会让本来就偏多的掌上报更多。设为负数则对所有轨迹生效。
    float m_sustainEdgeOnlyCells = 2.0f;     // -1..10

    // Number of frames to hold after a freeze cell is released,
    // protecting against baseline absorbing the negative rebound.
    int  m_releaseHoldFrames = 60;           // 0..255

    // ── IIR alpha-shift parameters (effective alpha = 2^(-shift)) ──
    // Higher shift = slower convergence.

    int  m_positiveAlphaShift = 7;           // Temperature drift (slow)
    int  m_negativeAlphaShift = 5;           // Release artifact (medium)
    int  m_noiseAlphaShift = 6;              // Within-noise tracking (slow)
    int  m_backgroundAlphaShift = 3;         // Background cell normal (moderate)
    int  m_noFingerAlphaShift = 3;           // No-finger mode (aggressive)

    // ── Per-frame step clamps (in ADC units, applied after Q8 conversion) ──
    int  m_positiveMaxStep = 20;             // 0..200
    int  m_negativeMaxStep = 20;             // 0..200
    int  m_backgroundMaxStep = 512;          // 1..2048
    int  m_noFingerMaxStep = 512;            // 1..2048

    // ── Aggressive recovery mode (false→true hasFinger transition) ──
    int  m_recoveryAlphaShift = 2;           // Recovery IIR speed (very fast)
    int  m_recoveryMaxStep = 256;            // Recovery step clamp
    int  m_recoveryMaxFrames = 30;           // Hard frame limit for recovery

    // Whether to apply minimal IIR within the deadband zone.
    bool m_noiseTrackingEnabled = true;

    // 上一帧还有轨迹时,忽略固件的 hasFinger 翻假。见 Process 里的说明。
    bool m_trustTracksOverFirmwareFlag = true;
    // 轨迹消失之后再宽限几帧。轨迹会因为一次歧义配对掉一帧,而断崖只要触发一次就不可逆。
    int  m_noFingerGraceFrames = 8;

    // 本帧自己的信号极值(已扣共模)超过这个数就当有触摸,不管固件标志说什么。
    // 0 表示只信固件标志。见 Process 里的出处说明。
    int  m_noFingerMaxSignal = 200;

    // ═══════════════════════════════════════════════════════════════
    // Public API
    // ═══════════════════════════════════════════════════════════════

    /// Process one frame of heatmap data.
    /// @param frame   读 frame.heatmapMatrix(原始),写 frame.touch.conditioned(调理后)。
    /// @param hasFinger  true when master suffix reports finger present.
    /// @return always true (reserved for future error reporting).
    inline bool Process(HeatmapFrame& frame, bool hasFinger) {
        if (!m_enabled) {
            // 关掉这一级是「不做调理」,不是「没有信号」。原先它就地改写热图,关掉时
            // 下游读到的自然还是原始图;两张图分开之后要显式直通,否则一关就整条链断。
            std::memcpy(&frame.touch.conditioned[0][0], &frame.heatmapMatrix[0][0],
                        sizeof(frame.touch.conditioned));
            return true;
        }

        if (!m_initialized) {
            Initialize();
        }

        // ── Guard: invalid master data → safe zero output, reset transient state ──
        const bool masterValid = frame.masterWasRead && frame.masterSuffixValid;
        if (!masterValid) {
            ZeroOutput(&frame.touch.conditioned[0][0]);
            m_releaseHold.fill(0);
            m_prevHadFinger = false;
            m_hadFreezeLastFrame = false;
            m_recoveryFrameCounter = 0;
            m_tracksAliveGrace = 0;
            return true;
        }

        // ── Dispatch ──
        // 固件的 hasFinger 不是整帧门控。它翻假时 ProcessNoFinger 会把调理后的热图
        // **整张清零**并让基线快速吸收,而手指停在角上不动时固件自己的检测器会先放弃
        // ——实测四边语料上 152 帧因此一个接触点都不报,厂商在同样的帧里正常上报,
        // 而且信号在峰检测之前就没了,下游任何阈值都救不回来。
        //
        // 上一帧还有轨迹就照常调理。这个覆盖是自限的,不需要计时器:ProcessFinger
        // 的背景分支与 ProcessNoFinger 用的是同一档速度(alpha shift 3、步长 512),
        // 手指真的抬起之后没有格子够得着冻结门槛,基线照样以原速吸收,只是不再有那一下
        // 断崖。
        // 轨迹掉一帧就够把断崖重新触发一次:实测四边语料上剩下的那 67 帧漏报,起点正是
        // 同一帧里多出一个接触点、原轨迹被顶掉,下一帧没有锚点 → 整张清零 → 信号没了 →
        // 再也建不回轨迹。所以看的不是「此刻有没有锚点」,而是「最近还有没有」。
        if (m_trustTracksOverFirmwareFlag && !frame.touch.runtime.prevTrackAnchors.empty()) {
            m_tracksAliveGrace = std::clamp(m_noFingerGraceFrames, 0, 255);
        } else if (m_tracksAliveGrace > 0) {
            --m_tracksAliveGrace;
        }
        // 厂商判「本帧无触摸」用的是**自己算的帧极值**,硬件检测器只是一张否决票:
        //   maxSig < dynamic[0x42] && !AFE_IsCoreTouchDetected() && ...
        // 检测器只要还报有触摸,就绝不会走进无触摸分支。(出处:`SPEC_baseline` §3.2)
        // 我们此前把它当唯一依据,这是缺的那一半——手指还压着而检测器先放弃时,
        // 无触摸分支一帧就能把基线推 512 个单位,足够把一个 500 量级的信号整个吸收掉。
        const bool signalSaysTouch =
            m_noFingerMaxSignal > 0 &&
            FrameSignalExtreme(&frame.heatmapMatrix[0][0]) >= m_noFingerMaxSignal;
        const bool treatAsFinger = hasFinger || m_tracksAliveGrace > 0 || signalSaysTouch;
        if (treatAsFinger) {
            ProcessFinger(frame);
        } else {
            ProcessNoFinger(frame);
        }

        m_prevHadFinger = treatAsFinger;
        return true;
    }

    /// Reset all state. Next Process() call will re-Initialize from m_baseline.
    inline void Reset() {
        m_initialized = false;
        m_prevHadFinger = false;
        m_hadFreezeLastFrame = false;
        m_recoveryFrameCounter = 0;
        m_tracksAliveGrace = 0;
        m_baselineQ8.fill(0);
        m_releaseHold.fill(0);
    }

private:
    static constexpr int kBaselineFractionBits = 8;

    // ── Persistent state ──
    bool m_initialized = false;
    bool m_prevHadFinger = false;
    bool m_hadFreezeLastFrame = false;
    int  m_recoveryFrameCounter = 0;
    int  m_tracksAliveGrace = 0;

    // ── SoA data arrays (cache-friendly linear traversal) ──
    // baselineQ8: Q8.24 fixed-point  (int32_t, 8 fractional bits)
    // releaseHold: remaining hold frames per cell
    std::array<int32_t, kCellCount> m_baselineQ8{};
    std::array<uint8_t, kCellCount> m_releaseHold{};

    // ═══════════════════════════════════════════════════════════════
    // Initialization
    // ═══════════════════════════════════════════════════════════════

    inline void Initialize() {
        const int initialBaseline = std::clamp(m_baseline, 0, 0xFFFF);
        m_baselineQ8.fill(static_cast<int32_t>(initialBaseline) << kBaselineFractionBits);
        m_releaseHold.fill(0);
        m_prevHadFinger = false;
        m_hadFreezeLastFrame = false;
        m_recoveryFrameCounter = 0;
        m_initialized = true;
    }

    // ═══════════════════════════════════════════════════════════════
    // ProcessNoFinger — all cells update baseline, all output zero
    // ═══════════════════════════════════════════════════════════════

    inline void ProcessNoFinger(HeatmapFrame& frame) {
        const int16_t* const raws = &frame.heatmapMatrix[0][0];
        int16_t* const out = &frame.touch.conditioned[0][0];

        for (int i = 0; i < kCellCount; ++i) {
            m_releaseHold[i] = 0;

            const int raw   = static_cast<int>(RawCell(raws[i]));
            const int baseline = m_baselineQ8[i] >> kBaselineFractionBits;
            const int delta = raw - baseline;

            if (std::abs(delta) <= m_noiseDeadband) {
                if (m_noiseTrackingEnabled && delta != 0) {
                    UpdateBaseline(i, delta, m_noiseAlphaShift, 1);
                }
            } else {
                UpdateBaseline(i, delta, m_noFingerAlphaShift, m_noFingerMaxStep);
            }

            out[i] = 0;
        }

        m_hadFreezeLastFrame = false;
        m_recoveryFrameCounter = 0;
    }

    // ═══════════════════════════════════════════════════════════════
    // BuildFreezeThresholds — 轨迹附近降低冻结门槛
    // ═══════════════════════════════════════════════════════════════

    // 轨迹锚点是上一帧的位置(本级跑在 tracker 之前),这正是迟滞需要的输入。
    // 锚点是 (x=列, y=行),而格下标按 行*列数+列 排,两边的轴序相反。
    inline void BuildFreezeThresholds(const HeatmapFrame& frame,
                                      std::array<int16_t, kCellCount>& out) const {
        const int enterThold = m_peakThreshold;
        const int sustain = std::min(m_sustainThreshold, m_peakThreshold);
        out.fill(static_cast<int16_t>(enterThold));

        const auto anchors = frame.touch.runtime.prevTrackAnchors;
        if (anchors.empty() || sustain >= enterThold) return;

        const float radius = std::clamp(m_sustainRadiusCells, 0.0f, 10.0f);
        if (radius <= 0.0f) return;
        const int span = static_cast<int>(radius) + 1;
        const float radiusSq = radius * radius;

        for (const auto& a : anchors) {
            if (m_sustainEdgeOnlyCells >= 0.0f) {
                const float distToEdge = std::min({a.x, a.y,
                                                   static_cast<float>(kCols) - a.x,
                                                   static_cast<float>(kRows) - a.y});
                if (distToEdge > m_sustainEdgeOnlyCells) continue;
            }
            const int centreCol = static_cast<int>(a.x);
            const int centreRow = static_cast<int>(a.y);
            for (int r = centreRow - span; r <= centreRow + span; ++r) {
                if (r < 0 || r >= kRows) continue;
                for (int c = centreCol - span; c <= centreCol + span; ++c) {
                    if (c < 0 || c >= kCols) continue;
                    const float dc = a.x - (static_cast<float>(c) + 0.5f);
                    const float dr = a.y - (static_cast<float>(r) + 0.5f);
                    if (dc * dc + dr * dr > radiusSq) continue;
                    out[static_cast<size_t>(r) * kCols + static_cast<size_t>(c)] =
                        static_cast<int16_t>(sustain);
                }
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════
    // ProcessFinger — per-cell freeze vs background dispatch
    // ═══════════════════════════════════════════════════════════════

    inline void ProcessFinger(HeatmapFrame& frame) {
        const int16_t* const raws = &frame.heatmapMatrix[0][0];
        int16_t* const out = &frame.touch.conditioned[0][0];

        // ── Common-mode rejection: median of all (raw - baseline) ──
        const int commonDiff = EstimateDiffMedian(raws);

        // ── Determine recovery mode ──
        // Recovery activates on:
        //   1. false→true hasFinger transition (baseline needs fast adaptation)
        //   2. No freeze cells in previous frame (finger present but signal weak)
        // Recovery deactivates when:
        //   - A freeze cell appears (next frame)
        //   - Frame counter exceeds m_recoveryMaxFrames
        bool inRecovery = false;

        if (!m_prevHadFinger) {
            inRecovery = true;
            m_recoveryFrameCounter = 0;
        } else if (!m_hadFreezeLastFrame) {
            inRecovery = true;
            ++m_recoveryFrameCounter;
            if (m_recoveryFrameCounter >= m_recoveryMaxFrames) {
                inRecovery = false;
            }
        }

        bool foundFreezeThisFrame = false;

        // ── 逐格的冻结门槛:轨迹附近用维持门槛,其余用进入门槛 ──
        std::array<int16_t, kCellCount> freezeThold;
        BuildFreezeThresholds(frame, freezeThold);

        // ── Per-cell dispatch ──
        for (int i = 0; i < kCellCount; ++i) {
            const int raw      = static_cast<int>(RawCell(raws[i]));
            const int baseline = m_baselineQ8[i] >> kBaselineFractionBits;
            const int delta    = raw - baseline;
            const int localDiff = delta - commonDiff;

            if (localDiff > freezeThold[i]) {
                // ── FREEZE ──
                foundFreezeThisFrame = true;
                m_releaseHold[i] = static_cast<uint8_t>(
                    std::clamp(m_releaseHoldFrames, 0, 255));

                // Track common-mode shift for frozen cell (keep baseline
                // aligned with global panel changes)
                if (commonDiff != 0) {
                    UpdateBaseline(i, commonDiff,
                                   m_backgroundAlphaShift,
                                   m_backgroundMaxStep);
                }

                out[i] = SaturateInt16(localDiff);
            } else {
                // ── BACKGROUND ──
                if (m_releaseHold[i] > 0) {
                    --m_releaseHold[i];

                    // Negative escape during release hold:
                    // If the local signal drops sharply (finger lifting),
                    // pass the negative value through instead of absorbing
                    // it into the baseline.
                    if (localDiff < -m_negativeDeadband) {
                        out[i] = SaturateInt16(localDiff);
                        continue;
                    }
                }

                BackgroundBaselineUpdate(i, delta, inRecovery);
                out[i] = 0;
            }
        }

        m_hadFreezeLastFrame = foundFreezeThisFrame;
    }

    // ═══════════════════════════════════════════════════════════════
    // BackgroundBaselineUpdate — three-tier adaptive IIR
    // ═══════════════════════════════════════════════════════════════

    inline void BackgroundBaselineUpdate(int index, int delta, bool inRecovery) {
        // Recovery mode: bypass tier classification, use uniform fast alpha
        if (inRecovery) {
            UpdateBaseline(index, delta, m_recoveryAlphaShift, m_recoveryMaxStep);
            return;
        }

        const int absDelta = std::abs(delta);

        // Tier 1: Deadband — skip or minimal noise tracking
        if (absDelta <= m_noiseDeadband) {
            if (m_noiseTrackingEnabled && delta != 0) {
                UpdateBaseline(index, delta, m_noiseAlphaShift, 1);
            }
            return;
        }

        // Tier 2+3: Beyond deadband — classify by delta direction and magnitude
        if (delta > m_positiveDeadband) {
            // Large positive: temperature / ambient drift (slow, clamped)
            UpdateBaseline(index, delta, m_positiveAlphaShift, m_positiveMaxStep);
        } else if (delta < -m_negativeDeadband) {
            // Large negative: finger release artifact (medium speed)
            UpdateBaseline(index, delta, m_negativeAlphaShift, m_negativeMaxStep);
        } else {
            // Normal range: moderate noise-level tracking
            UpdateBaseline(index, delta, m_noiseAlphaShift, m_backgroundMaxStep);
        }
    }

    // ═══════════════════════════════════════════════════════════════
    // UpdateBaseline — Q8.24 fixed-point IIR core
    // ═══════════════════════════════════════════════════════════════

    inline void UpdateBaseline(int index, int delta, int alphaShift, int maxStep) {
        const int shift = std::clamp(alphaShift, 0, 15);
        const int32_t maxStepQ8 = static_cast<int32_t>(std::max(0, maxStep)) << kBaselineFractionBits;

        int32_t updateQ8 = (static_cast<int32_t>(delta) << kBaselineFractionBits) >> shift;

        if (maxStepQ8 > 0) {
            updateQ8 = std::clamp(updateQ8, -maxStepQ8, maxStepQ8);
        }

        auto& baseline = m_baselineQ8[index];
        baseline = std::clamp(baseline + updateQ8,
                              0,
                              0xFFFF << kBaselineFractionBits);
    }

    // ═══════════════════════════════════════════════════════════════
    // Common-mode estimation
    // ═══════════════════════════════════════════════════════════════

    // Computes the median of (raw - baseline) across all cells.
    // Used as commonDiff to remove global panel-wide shifts (temperature,
    // display state, VCOM noise) from the freeze threshold comparison.
    // 本帧最强的一格,扣掉共模。用中位数当共模基准,与 ProcessFinger 里一致——
    // 不扣的话一次全局漂移就会被当成有手指。
    inline int FrameSignalExtreme(const int16_t* cells) const {
        const int commonDiff = EstimateDiffMedian(cells);
        int extreme = 0;
        for (int i = 0; i < kCellCount; ++i) {
            const int baseline = m_baselineQ8[i] >> kBaselineFractionBits;
            const int localDiff = static_cast<int>(RawCell(cells[i])) - baseline - commonDiff;
            if (localDiff > extreme) extreme = localDiff;
        }
        return extreme;
    }

    inline int EstimateDiffMedian(const int16_t* cells) const {
        std::array<int, kCellCount> diffs{};
        for (int i = 0; i < kCellCount; ++i) {
            const int baseline = m_baselineQ8[i] >> kBaselineFractionBits;
            diffs[i] = static_cast<int>(RawCell(cells[i])) - baseline;
        }

        auto mid = diffs.begin() + (kCellCount / 2);
        std::nth_element(diffs.begin(), mid, diffs.end());
        return *mid;
    }

    // ═══════════════════════════════════════════════════════════════
    // Static helpers
    // ═══════════════════════════════════════════════════════════════

    static inline void ZeroOutput(int16_t* cells) {
        for (int i = 0; i < kCellCount; ++i) {
            cells[i] = 0;
        }
    }

    static inline int16_t SaturateInt16(int value) {
        return static_cast<int16_t>(std::clamp(value,
                                               static_cast<int>(INT16_MIN),
                                               static_cast<int>(INT16_MAX)));
    }

    static inline uint16_t RawCell(int16_t value) {
        return static_cast<uint16_t>(value);
    }
};

}} // namespace Solvers::Touch
