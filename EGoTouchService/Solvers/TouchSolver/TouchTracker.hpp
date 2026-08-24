#pragma once

#include "SolverTypes.h"
#include "GhostSuppressor.hpp"
#include "StylusTouchSuppressor.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace Solvers { namespace Touch {

class TouchTracker {
public:
    bool m_enabled = true;
    int   m_maxTouchCount = 20;
    float m_maxTrackDistance = 4.985f;
    float m_alwaysMatchDistance = 2.0f;
    float m_edgeTrackBoost = 1.5f;
    float m_accThresholdBoost = 4.0f;
    float m_accBoostSizeMm = 1.6f;
    float m_predictionScale = 1.0f;
    bool  m_gapRelinkEnabled = true;
    int   m_gapRelinkWindowFrames = 4;
    // 空档期是否照常上报。见 BuildSilentGapContact 处的说明与实测数字。
    bool  m_reportDuringGap = true;
    int   m_touchDownDebounceFrames = 1;
    bool  m_dynamicDebounceEnabled = true;
    int   m_touchDownDebounceMaxExtra = 2;
    int   m_touchDownWeakSignalThreshold = 180;
    float m_touchDownSmallSizeThresholdMm = 1.3f;
    bool  m_touchDownRejectEnabled = true;
    int   m_touchDownRejectMinSignal = 55;
    float m_touchDownRejectMinSizeMm = 0.95f;
    int   m_touchDownEdgeRejectMinSignal = 90;
    float m_fallbackSizeMm = 1.0f;
    float m_sizeAreaScale = 0.22f;
    float m_sizeSignalScale = 0.35f;
    bool  m_rxGhostFilterEnabled = true;
    int   m_rxGhostLineDelta = 0;
    float m_rxGhostWeakRatio = 0.5f;
    bool  m_rxGhostOnlyNew = true;
    bool  m_useHungarian = true;

    bool  m_stylusSuppressGlobalEnabled = true;
    bool  m_stylusSuppressLocalEnabled = true;
    float m_stylusSuppressLocalDistance = 2.5f;
    int   m_stylusSuppressPenPeakThreshold = 1500;
    int   m_stylusSuppressTouchSignalKeep = 6000;
    int   m_stylusSuppressTouchAreaKeep = 12;
    bool  m_stylusAftEnabled = true;
    int   m_stylusAftRecentFrames = 24;
    float m_stylusAftRadius = 2.8f;
    // Screen-wide pen mode, driven by StylusTouchArbiter rather than by tip distance.
    // m_stylusAftRadius (~11 mm) can never reach a palm resting 30-80 mm from the tip,
    // which is why local suppression alone never behaved like a palm rejector.
    bool  m_penModeSuppressEnabled = true;
    int   m_stylusAftDebounceFrames = 3;
    int   m_stylusAftWeakSignalThreshold = 240;
    float m_stylusAftWeakSizeThresholdMm = 1.2f;
    int   m_stylusAftSuppressFrames = 40;
    int   m_stylusAftPalmSuppressFrames = 100;
    int   m_stylusAftPalmAreaThreshold = 20;
    float m_stylusAftPalmSizeThresholdMm = 2.5f;

    inline bool Process(HeatmapFrame& frame);
    inline bool HasLiveTracks() const { return m_trackCount > 0; }
    inline void ClearLiveState();

    // 本帧结束时活跃轨迹的位置。管线在下一帧的检测级之前把它挂进 runtime，
    // 检测级据此对已被跟踪的峰放宽维持判据。
    inline std::span<const TrackAnchor> GetTrackAnchors() const {
        return {m_trackAnchors, static_cast<size_t>(m_trackAnchorCount)};
    }

private:
    static constexpr int kMaxTracks = 20;
    static constexpr float kGapRelinkSecondBestMarginSq = 4.0f;
    static constexpr float kGapRelinkSecondBestRatio = 1.75f;
    static constexpr float kGapRelinkSpeedWindowScale = 0.75f;
    static constexpr float kGapRelinkHardCapScale = 2.0f;
    static constexpr float kActiveBootstrapGateScale = 1.25f;
    static constexpr float kStylusTouchCoordScale = 1.0f / 1024.0f;
    // A pen-mode episode survives this many consecutive frames without the flag before it
    // is considered over. The gap being tolerated is external: any frame that carries no
    // stylus payload leaves interop zeroed, and treating that as the end of the episode
    // restarts m_penModeFramesElapsed at 1, which hands every surviving track the
    // "predates the pen" exemption for good — a palm that was already being rejected turns
    // into reported input one frame later. A genuine pen departure never lands inside this
    // window: StylusTouchArbiter holds its flag for 40 linger frames after the tip leaves,
    // so anything shorter than that is a dropout, not an ending.
    static constexpr int kPenModeGapToleranceFrames = 5;
    static constexpr int kPenModeElapsedCap = 1000000;

    enum class TrackPhase : uint8_t { Active = 0, SilentGap = 1 };

    struct StylusNoiseEvidence {
        bool pointValid = false;
        bool writingLike = false;
        bool stable = false;
        bool active = false;
        bool tx2Strong = false;
        bool tx2Dominant = false;
        float x = 0.0f;
        float y = 0.0f;
        int signalX = 0;
        int signalY = 0;
        int maxRawPeak = 0;
    };

    struct TrackState {
        int id = 0;
        float x = 0, y = 0, vx = 0, vy = 0;
        int areaCells = 0, signalSum = 0;
        float sizeMm = 0;
        int age = 0, missed = 0, downDebounceFrames = 0;
        bool isEdge = false;
        uint32_t edgeFlags = 0;
        uint8_t centroidEdgeFlags = 0;
        uint32_t ecFlags = 0;
        float edgeDistXCells = 0.0f;
        float edgeDistYCells = 0.0f;
        // 匹配坐标与它自己的速度。与 x/y/vx/vy 分开维护：后者是补偿后的上报量，
        // 补偿量随 grip 比例逐帧抖动，拿它做配对等于给匹配器喂并不存在的位移。
        float matchXCells = 0.0f;
        float matchYCells = 0.0f;
        float matchVxCellsPerFrame = 0.0f;
        float matchVyCellsPerFrame = 0.0f;
        uint8_t ecWidthX = 0;
        uint8_t ecWidthY = 0;
        int stylusSuppressFrames = 0;
        uint8_t sourcePeakId = 0;
        uint8_t sourcePeakAge = 0;
        TrackPhase phase = TrackPhase::Active;
        int gapFrames = 0;
    };

    TrackState m_tracks[kMaxTracks];
    int m_trackCount = 0;
    TrackAnchor m_trackAnchors[kMaxTracks]{};
    int m_trackAnchorCount = 0;
    int m_nextIdSeed = 1;
    int m_stylusFramesSinceActive = 1000000;
    bool m_penModeActive = false;
    // How long the current pen-mode episode has lasted. A track older than this existed
    // before the pen arrived, so it is an in-progress gesture and must be left alone.
    // Counted in frames elapsed, not in frames the flag was asserted: see
    // kPenModeGapToleranceFrames.
    int m_penModeFramesElapsed = 0;
    int m_penModeGapFrames = 0;
    float m_lastStylusX = 0, m_lastStylusY = 0;

    static inline float DistanceSq(float x1, float y1, float x2, float y2);
    static inline bool IsEdgeTouch(float x, float y, int cols, int rows, float em);
    static inline bool IsEdgeContact(const TouchContact& touch, int cols, int rows, float em);
    static inline void StoreEdgeMetadata(TrackState& track, const TouchContact& touch);
    static inline void RestoreEdgeMetadata(TouchContact& touch, const TrackState& track);
    inline void GetMatchReference(const TrackState& track, float& outX, float& outY) const;
    inline float EstimateSizeMm(int areaCells, int signalSum) const;
    inline int ComputeTouchDownDebounceFrames(const TouchContact& touch) const;
    inline float ComputeTrackGateSq(const TrackState& track, const TouchContact& contact, int cols, int rows, float edgeMargin) const;
    inline float ComputeAssignmentCost(const TrackState& track, const TouchContact& contact, int cols, int rows, float edgeMargin) const;
    inline bool PassActiveBootstrapGate(const TrackState& track, const TouchContact& contact, int cols, int rows, float edgeMargin) const;
    inline void MatchAgainstSubset(std::span<const TouchContact> contacts, int curCount, const int* prevSubset, int subsetCount, int* curToPre) const;
    static inline float Orientation(float ax, float ay, float bx, float by, float cx, float cy);
    static inline bool SegmentsCross(float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy);
    inline void ApplyCrossingGuard(std::span<const TouchContact> contacts, int curCount, int* curToPre) const;
    static inline void UpdateBestCandidate(float distance, int candidateId, float& best, float& second, int& bestId);
    inline bool PassGapRelinkAmbiguity(float best, float second) const;
    inline TouchContact BuildSilentGapContact(const TrackState& track, int prevIndex, int cols, int rows, float edgeMargin) const;
    inline TouchContact BuildLiftOffContact(const TrackState& track, int prevIndex, int cols, int rows, float edgeMargin) const;
    inline int AllocateId(const TrackState* reservedNextTracks, int reservedCount) const;
    inline StylusNoiseEvidence BuildStylusNoiseEvidence(const HeatmapFrame& frame,
                                                        int recheckThreshold) const;
    inline bool IsStrongTouchCandidate(const TouchContact& touch, const StylusTouchSuppressor* ss) const;
    inline bool ResolveStylusAftContext(const HeatmapFrame& frame, float& outX, float& outY, const StylusTouchSuppressor* ss);
    inline bool ShouldPenModeSuppress(const TouchContact& touch, int touchAge) const;
    inline bool ShouldStylusAftSuppress(const TouchContact& touch, int touchAge, float stylusX, float stylusY, int& outHoldFrames, const StylusTouchSuppressor* ss) const;
    inline void SolveAssignment(const float* cost, int n, int m, int* rowToCol) const;
};

inline void TouchTracker::ClearLiveState() {
    for (auto& track : m_tracks) {
        track = TrackState{};
    }
    m_trackCount = 0;
    m_trackAnchorCount = 0;
    m_stylusFramesSinceActive = 1000000;
    m_penModeActive = false;
    m_penModeFramesElapsed = 0;
    m_penModeGapFrames = 0;
    m_lastStylusX = 0.0f;
    m_lastStylusY = 0.0f;
}

inline float TouchTracker::DistanceSq(float x1, float y1, float x2, float y2) {
    const float dx = x1 - x2;
    const float dy = y1 - y2;
    return dx * dx + dy * dy;
}

inline bool TouchTracker::IsEdgeTouch(float x, float y, int cols, int rows, float em) {
    return (x <= em) || (y <= em) || (x >= float(cols) - em) || (y >= float(rows) - em);
}

inline bool TouchTracker::IsEdgeContact(const TouchContact& touch, int cols, int rows, float em) {
    return touch.isEdge ||
           IsEdgeTouch(touch.matchXCells, touch.matchYCells, cols, rows, em) ||
           (touch.edgeFlags & (0x20 | 0x80000)) != 0 ||
           touch.centroidEdgeFlags != 0;
}

inline void TouchTracker::StoreEdgeMetadata(TrackState& track, const TouchContact& touch) {
    track.isEdge = touch.isEdge ||
                   (touch.edgeFlags & (0x20 | 0x80000)) != 0 ||
                   touch.centroidEdgeFlags != 0;
    track.edgeFlags = touch.edgeFlags;
    track.centroidEdgeFlags = touch.centroidEdgeFlags;
    track.ecFlags = touch.ecFlags;
    track.edgeDistXCells = touch.edgeDistXCells;
    track.edgeDistYCells = touch.edgeDistYCells;
    track.matchXCells = touch.matchXCells;
    track.matchYCells = touch.matchYCells;
    track.ecWidthX = touch.ecWidthX;
    track.ecWidthY = touch.ecWidthY;
}

inline void TouchTracker::RestoreEdgeMetadata(TouchContact& touch, const TrackState& track) {
    touch.isEdge = touch.isEdge || track.isEdge;
    touch.edgeFlags = track.edgeFlags;
    touch.centroidEdgeFlags = track.centroidEdgeFlags;
    touch.ecFlags = track.ecFlags;
    touch.edgeDistXCells = track.edgeDistXCells;
    touch.edgeDistYCells = track.edgeDistYCells;
    touch.matchXCells = track.matchXCells;
    touch.matchYCells = track.matchYCells;
    touch.ecWidthX = track.ecWidthX;
    touch.ecWidthY = track.ecWidthY;
}

inline void TouchTracker::GetMatchReference(const TrackState& track, float& outX, float& outY) const {
    float framesAhead = m_predictionScale;
    if (track.phase == TrackPhase::SilentGap) {
        framesAhead = m_predictionScale * static_cast<float>(std::max(1, track.gapFrames + 1));
    }
    outX = track.matchXCells + track.matchVxCellsPerFrame * framesAhead;
    outY = track.matchYCells + track.matchVyCellsPerFrame * framesAhead;
}

inline float TouchTracker::EstimateSizeMm(int areaCells, int signalSum) const {
    return EstimateContactSizeMm(areaCells, signalSum, m_fallbackSizeMm,
                                 m_sizeAreaScale, m_sizeSignalScale);
}

inline int TouchTracker::ComputeTouchDownDebounceFrames(const TouchContact& touch) const {
    int frames = m_touchDownDebounceFrames;
    if (!m_dynamicDebounceEnabled) return frames;
    int extra = 0;
    if (touch.signalSum > 0 && touch.signalSum < m_touchDownWeakSignalThreshold) extra += 1;
    if (touch.sizeMm > 0.0f && touch.sizeMm < m_touchDownSmallSizeThresholdMm) extra += 1;
    if (touch.isEdge) extra += 1;
    return frames + std::clamp(extra, 0, m_touchDownDebounceMaxExtra);
}

inline float TouchTracker::ComputeTrackGateSq(const TrackState& track,
                                              const TouchContact& contact,
                                              int cols,
                                              int rows,
                                              float edgeMargin) const {
    float gateDist = m_maxTrackDistance;
    const bool edge = track.isEdge ||
                      IsEdgeTouch(track.x, track.y, cols, rows, edgeMargin) ||
                      IsEdgeContact(contact, cols, rows, edgeMargin);
    const float speed = std::sqrt(track.vx * track.vx + track.vy * track.vy);
    if (edge) gateDist *= m_edgeTrackBoost;
    const float sizeMm = std::max(track.sizeMm, EstimateSizeMm(contact.areaCells, contact.signalSum));
    if (sizeMm <= m_accBoostSizeMm) {
        const bool young = track.age <= 2;
        const bool movingFast = speed > (m_maxTrackDistance * 0.5f);
        const float boost = (young || movingFast)
            ? m_accThresholdBoost
            : std::min(m_accThresholdBoost, 2.0f);
        gateDist *= boost;
    } else if (edge) {
        gateDist *= std::min(m_accThresholdBoost, 2.0f);
    }
    if (track.phase == TrackPhase::SilentGap) {
        const float extra = std::min(m_maxTrackDistance,
                                     speed * kGapRelinkSpeedWindowScale *
                                     static_cast<float>(std::max(1, track.gapFrames)));
        gateDist = std::min(gateDist + extra, m_maxTrackDistance * kGapRelinkHardCapScale);
    } else if (edge) {
        gateDist = std::min(gateDist, m_maxTrackDistance * 3.0f);
    } else {
        gateDist = std::min(gateDist, m_maxTrackDistance * 2.0f);
    }
    return gateDist * gateDist;
}

inline float TouchTracker::ComputeAssignmentCost(const TrackState& track,
                                                 const TouchContact& contact,
                                                 int cols,
                                                 int rows,
                                                 float edgeMargin) const {
    float refX = 0.0f, refY = 0.0f;
    GetMatchReference(track, refX, refY);
    const float distanceSq = DistanceSq(contact.matchXCells, contact.matchYCells, refX, refY);
    const float gateSq = ComputeTrackGateSq(track, contact, cols, rows, edgeMargin);
    if (distanceSq > gateSq) return 1.0e12f;

    float cost = distanceSq;
    const float dx = contact.matchXCells - track.matchXCells;
    const float dy = contact.matchYCells - track.matchYCells;
    const float moveSq = dx * dx + dy * dy;
    const float prevSpeedSq = track.matchVxCellsPerFrame * track.matchVxCellsPerFrame + track.matchVyCellsPerFrame * track.matchVyCellsPerFrame;
    if (track.age > 1 && moveSq > 0.01f && prevSpeedSq > 0.04f) {
        const float dot = dx * track.matchVxCellsPerFrame + dy * track.matchVyCellsPerFrame;
        if (dot < 0.0f) cost += std::min(25.0f, -dot * 1.5f);
        const float speedDelta = std::abs(std::sqrt(moveSq) - std::sqrt(prevSpeedSq));
        if (speedDelta > m_maxTrackDistance * 0.5f) {
            const float excess = speedDelta - m_maxTrackDistance * 0.5f;
            cost += std::min(25.0f, excess * excess * 0.35f);
        }
    }

    if (track.signalSum > 0 && contact.signalSum > 0) {
        const float denom = static_cast<float>(std::max(track.signalSum, contact.signalSum));
        cost += (std::abs(track.signalSum - contact.signalSum) / denom) * 6.0f;
    }
    if (track.areaCells > 0 && contact.areaCells > 0) {
        const float denom = static_cast<float>(std::max(track.areaCells, contact.areaCells));
        cost += (std::abs(track.areaCells - contact.areaCells) / denom) * 4.0f;
    }
    const float contactSize = EstimateSizeMm(contact.areaCells, contact.signalSum);
    if (track.sizeMm > 0.0f && contactSize > 0.0f) {
        const float diff = std::abs(track.sizeMm - contactSize);
        cost += std::min(9.0f, diff * diff * 0.5f);
    }
    if (track.sourcePeakId != 0 && contact.sourcePeakId != 0 && track.sourcePeakId != contact.sourcePeakId) {
        const float ageWeight = 1.0f + static_cast<float>(std::min<int>(track.sourcePeakAge, 20)) * 0.1f;
        cost += std::min(10.0f, 4.0f * ageWeight);
    }
    return cost;
}

inline bool TouchTracker::PassActiveBootstrapGate(const TrackState& track,
                                                  const TouchContact& contact,
                                                  int cols,
                                                  int rows,
                                                  float edgeMargin) const {
    if (track.phase != TrackPhase::Active || track.age > 1 || track.missed != 0) return false;
    if (track.isEdge || IsEdgeTouch(track.matchXCells, track.matchYCells, cols, rows, edgeMargin)) return false;
    if (IsEdgeContact(contact, cols, rows, edgeMargin)) return false;
    if (contact.signalSum < m_touchDownWeakSignalThreshold * 2) return false;
    if (EstimateSizeMm(contact.areaCells, contact.signalSum) <= m_touchDownSmallSizeThresholdMm) return false;

    float refX = 0.0f, refY = 0.0f;
    GetMatchReference(track, refX, refY);
    const float bootstrapGate = m_maxTrackDistance * kActiveBootstrapGateScale;
    return DistanceSq(contact.matchXCells, contact.matchYCells, refX, refY) <=
           bootstrapGate * bootstrapGate;
}

inline void TouchTracker::MatchAgainstSubset(std::span<const TouchContact> contacts,
                                             int curCount,
                                             const int* prevSubset,
                                             int subsetCount,
                                             int* curToPre) const {
    if (curCount <= 0 || subsetCount <= 0) return;
    constexpr int kRows = 40, kCols = 60;
    constexpr float kEdgeMargin = 2.0f;
    constexpr float kInvalidMatchCost = 1.0e12f;
    float cost[kMaxTracks * kMaxTracks];
    for (int c = 0; c < curCount; ++c) {
        for (int i = 0; i < subsetCount; ++i) {
            const int pre = prevSubset[i];
            const float assignmentCost = ComputeAssignmentCost(m_tracks[pre], contacts[c], kCols, kRows, kEdgeMargin);
            cost[c * kMaxTracks + i] = (assignmentCost < kInvalidMatchCost) ? assignmentCost : kInvalidMatchCost;
        }
    }

    int rowToSubset[kMaxTracks];
    for (int i = 0; i < curCount; ++i) rowToSubset[i] = -1;
    if (m_useHungarian) {
        SolveAssignment(cost, curCount, subsetCount, rowToSubset);
    } else {
        bool prevUsed[kMaxTracks] = {};
        for (int c = 0; c < curCount; ++c) {
            float best = std::numeric_limits<float>::max();
            int bestIdx = -1;
            for (int i = 0; i < subsetCount; ++i) {
                if (prevUsed[i]) continue;
                if (cost[c * kMaxTracks + i] < best) {
                    best = cost[c * kMaxTracks + i];
                    bestIdx = i;
                }
            }
            if (bestIdx >= 0 && best < kInvalidMatchCost) {
                prevUsed[bestIdx] = true;
                rowToSubset[c] = bestIdx;
            }
        }
    }

    for (int c = 0; c < curCount; ++c) {
        const int subset = rowToSubset[c];
        if (subset >= 0 && cost[c * kMaxTracks + subset] < kInvalidMatchCost) {
            curToPre[c] = prevSubset[subset];
        }
    }
    ApplyCrossingGuard(contacts, curCount, curToPre);
}

inline float TouchTracker::Orientation(float ax, float ay, float bx, float by, float cx, float cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

inline bool TouchTracker::SegmentsCross(float ax, float ay, float bx, float by,
                                        float cx, float cy, float dx, float dy) {
    const float o1 = Orientation(ax, ay, bx, by, cx, cy);
    const float o2 = Orientation(ax, ay, bx, by, dx, dy);
    const float o3 = Orientation(cx, cy, dx, dy, ax, ay);
    const float o4 = Orientation(cx, cy, dx, dy, bx, by);
    return (o1 * o2 < 0.0f) && (o3 * o4 < 0.0f);
}

inline void TouchTracker::ApplyCrossingGuard(std::span<const TouchContact> contacts,
                                             int curCount,
                                             int* curToPre) const {
    constexpr int kRows = 40, kCols = 60;
    constexpr float kEdgeMargin = 2.0f;
    constexpr float kInvalidMatchCost = 1.0e12f;
    for (int a = 0; a < curCount; ++a) {
        const int preA = curToPre[a];
        if (preA < 0) continue;
        for (int b = a + 1; b < curCount; ++b) {
            const int preB = curToPre[b];
            if (preB < 0 || preA == preB) continue;
            const TrackState& trackA = m_tracks[preA];
            const TrackState& trackB = m_tracks[preB];
            if (!SegmentsCross(trackA.matchXCells, trackA.matchYCells,
                               contacts[a].matchXCells, contacts[a].matchYCells,
                               trackB.matchXCells, trackB.matchYCells,
                               contacts[b].matchXCells, contacts[b].matchYCells)) {
                continue;
            }

            const float currentCost =
                ComputeAssignmentCost(trackA, contacts[a], kCols, kRows, kEdgeMargin) +
                ComputeAssignmentCost(trackB, contacts[b], kCols, kRows, kEdgeMargin);
            const float swappedA = ComputeAssignmentCost(trackA, contacts[b], kCols, kRows, kEdgeMargin);
            const float swappedB = ComputeAssignmentCost(trackB, contacts[a], kCols, kRows, kEdgeMargin);
            if (swappedA >= kInvalidMatchCost || swappedB >= kInvalidMatchCost) continue;
            const float swappedCost = swappedA + swappedB;
            if (swappedCost <= currentCost + 4.0f) {
                curToPre[a] = preB;
                curToPre[b] = preA;
            }
        }
    }
}

inline void TouchTracker::UpdateBestCandidate(float distance,
                                              int candidateId,
                                              float& best,
                                              float& second,
                                              int& bestId) {
    if (distance < best) {
        second = best;
        best = distance;
        bestId = candidateId;
    } else if (distance < second) {
        second = distance;
    }
}

inline bool TouchTracker::PassGapRelinkAmbiguity(float best, float second) const {
    if (!std::isfinite(best)) return false;
    if (!std::isfinite(second)) return true;
    return second >= best + kGapRelinkSecondBestMarginSq ||
           second >= best * kGapRelinkSecondBestRatio;
}

inline TouchContact TouchTracker::BuildSilentGapContact(const TrackState& track,
                                                        int prevIndex,
                                                        int cols,
                                                        int rows,
                                                        float edgeMargin) const {
    const bool predicted = m_predictionScale > 0.0f;

    TouchContact hidden;
    hidden.id = track.id;
    // 上报位置停在最后一个真实位置，不外推。空档的成因是信号丢了，不是手指一定还在动;
    // 真抬起时外推会让指针在手指离开之后继续滑出去几帧。外推只用于匹配（见下面对
    // matchXCells 的处理），那里越准越好，而这里越保守越好。
    hidden.x = track.x;
    hidden.y = track.y;
    hidden.state = TouchStateMove;
    hidden.areaCells = track.areaCells;
    hidden.signalSum = track.signalSum;
    hidden.sizeMm = track.sizeMm;
    hidden.sourcePeakId = track.sourcePeakId;
    hidden.sourcePeakAge = track.sourcePeakAge;
    RestoreEdgeMetadata(hidden, track);
    // RestoreEdgeMetadata 搬来的是上一次真实匹配的坐标。这一帧的位置是外推出来的，
    // 匹配坐标要按匹配空间自己的速度同样外推，否则两份坐标会越走越远。
    if (predicted) {
        const float framesAhead = m_predictionScale * static_cast<float>(std::max(1, track.gapFrames));
        hidden.matchXCells = std::clamp(track.matchXCells + track.matchVxCellsPerFrame * framesAhead,
                                         0.0f, static_cast<float>(cols));
        hidden.matchYCells = std::clamp(track.matchYCells + track.matchVyCellsPerFrame * framesAhead,
                                         0.0f, static_cast<float>(rows));
    }
    hidden.isEdge = hidden.isEdge || IsEdgeTouch(hidden.x, hidden.y, cols, rows, edgeMargin);
    // 空档期不上报，主机看到的就是接触点消失又出现，即一次断开——而重连窗口存在的
    // 全部意义正是不让短暂的信号丢失变成一次抬起。厂商在这几帧里照常上报：抬起防抖期
    // 的事件码是 8，而 TouchReport_ToBeReported 只排除 1（按下防抖）与 0x20（作废），
    // 8 不在其列。
    //
    // 实测四份语料上，中途断开从 11 / 62 / 65 / 746 次降到 0 / 0 / 0 / 14 次，
    // 厂商是 0 / 0 / 2 / 5。位置残差与接触点数一致率基本不动。数字见
    // docs/touch_stack.md。
    //
    // 起效还需要手势层不要再无条件压制空档接触点——那里曾经有一条
    // `lifeFlags & TouchLifeSilentGap` 的判据，盖过本级的决定，使整个重连窗口形同虚设。
    hidden.isReported = m_reportDuringGap;
    hidden.prevIndex = prevIndex;
    hidden.debugFlags = predicted ? 0x28 : 0x20;
    hidden.lifeFlags = TouchLifeMapped | TouchLifeSilentGap;
    if (hidden.isEdge) hidden.lifeFlags |= TouchLifeEdge;
    hidden.reportFlags = 0;
    hidden.reportEvent = TouchReportIdle;
    return hidden;
}

inline TouchContact TouchTracker::BuildLiftOffContact(const TrackState& track,
                                                      int prevIndex,
                                                      int cols,
                                                      int rows,
                                                      float edgeMargin) const {
    TouchContact up;
    up.id = track.id;
    up.x = track.x;
    up.y = track.y;
    up.state = TouchStateUp;
    up.areaCells = track.areaCells;
    up.signalSum = track.signalSum;
    up.sizeMm = track.sizeMm;
    up.sourcePeakId = track.sourcePeakId;
    up.sourcePeakAge = track.sourcePeakAge;
    RestoreEdgeMetadata(up, track);
    up.isEdge = up.isEdge || IsEdgeTouch(up.x, up.y, cols, rows, edgeMargin);
    up.isReported = true;
    up.prevIndex = prevIndex;
    up.debugFlags = 0x04;
    up.lifeFlags = TouchLifeLiftOff;
    if (up.isEdge) up.lifeFlags |= TouchLifeEdge;
    up.reportFlags = 0;
    up.reportEvent = TouchReportUp;
    return up;
}

inline int TouchTracker::AllocateId(const TrackState* reservedNextTracks, int reservedCount) const {
    for (int i = 0; i < kMaxTracks; ++i) {
        const int candidate = ((m_nextIdSeed - 1 + i) % kMaxTracks) + 1;
        bool used = false;
        for (int j = 0; j < reservedCount; ++j) {
            if (reservedNextTracks[j].id == candidate) {
                used = true;
                break;
            }
        }
        if (used) continue;
        for (int j = 0; j < m_trackCount; ++j) {
            if (m_tracks[j].id == candidate) {
                used = true;
                break;
            }
        }
        if (!used) return candidate;
    }
    return 0;
}

inline TouchTracker::StylusNoiseEvidence TouchTracker::BuildStylusNoiseEvidence(
    const HeatmapFrame& frame,
    int recheckThreshold) const {
    StylusNoiseEvidence evidence;
    const auto& output = frame.stylus.output;
    const auto& interop = frame.stylus.interop;
    evidence.signalX = static_cast<int>(interop.signalX);
    evidence.signalY = static_cast<int>(interop.signalY);
    evidence.maxRawPeak = std::max({
        evidence.signalX,
        evidence.signalY,
        static_cast<int>(interop.maxRawPeak)
    });

    if (!(output.inRange && output.point.valid)) {
        return evidence;
    }

    evidence.pointValid = true;
    evidence.x = output.point.x * kStylusTouchCoordScale;
    evidence.y = output.point.y * kStylusTouchCoordScale;

    evidence.writingLike =
        output.tipDown ||
        (output.pressure > 0);
    evidence.tx2Strong = evidence.signalY >= recheckThreshold;
    evidence.tx2Dominant =
        (evidence.signalY > 0) &&
        (evidence.signalY * 4 >= std::max(1, evidence.signalX) * 3);
    const int sustainThreshold = std::max(64, recheckThreshold / 2);
    evidence.stable =
        evidence.pointValid &&
        interop.recheckPassed &&
        (evidence.maxRawPeak >= sustainThreshold) &&
        (evidence.writingLike || evidence.tx2Strong || evidence.tx2Dominant);
    evidence.active =
        evidence.stable &&
        (evidence.tx2Strong ||
         ((evidence.signalY >= sustainThreshold) && evidence.tx2Dominant));
    return evidence;
}

inline bool TouchTracker::IsStrongTouchCandidate(const TouchContact& touch, const StylusTouchSuppressor* ss) const {
    const int keepSignal = ss ? ss->m_stylusSuppressTouchSignalKeep : m_stylusSuppressTouchSignalKeep;
    const int keepArea = ss ? ss->m_stylusSuppressTouchAreaKeep : m_stylusSuppressTouchAreaKeep;
    return (touch.signalSum >= keepSignal) &&
           (touch.areaCells >= keepArea);
}

inline bool TouchTracker::ResolveStylusAftContext(const HeatmapFrame& frame, float& outX, float& outY, const StylusTouchSuppressor* ss) {
    const bool globalEnabled = ss ? ss->m_stylusSuppressGlobalEnabled : m_stylusSuppressGlobalEnabled;
    const bool aftEnabled = ss ? ss->m_stylusAftEnabled : m_stylusAftEnabled;

    // Latch the arbiter's screen-wide verdict once per frame. StylusTouchSuppressor
    // preserves it through the touch stage precisely so it is readable here.
    m_penModeActive = globalEnabled && m_penModeSuppressEnabled &&
                      frame.stylus.interop.touchSuppressActive;
    if (m_penModeActive) {
        m_penModeGapFrames = 0;
    } else if (m_penModeFramesElapsed > 0) {
        ++m_penModeGapFrames;
    }
    // The episode outlives short dropouts (kPenModeGapToleranceFrames), and its length
    // keeps advancing across them. Counting only asserted frames would let track age
    // outrun the episode by exactly the number of dropped frames, which is the same
    // permanent exemption by a slower route.
    const bool episodeAlive =
        m_penModeActive ||
        (m_penModeFramesElapsed > 0 && m_penModeGapFrames <= kPenModeGapToleranceFrames);
    if (episodeAlive) {
        if (m_penModeFramesElapsed < kPenModeElapsedCap) ++m_penModeFramesElapsed;
    } else {
        m_penModeFramesElapsed = 0;
        m_penModeGapFrames = 0;
    }

    if (!globalEnabled || !aftEnabled) {
        m_stylusFramesSinceActive = 1000000;
        return false;
    }
    const auto& interop = frame.stylus.interop;
    const int penPeakThold = ss ? ss->m_stylusSuppressPenPeakThreshold : m_stylusSuppressPenPeakThreshold;
    const int baseThreshold =
        (interop.recheckThreshold > 0)
            ? static_cast<int>(interop.recheckThreshold)
            : penPeakThold;
    const int multiThreshold =
        (interop.recheckThresholdMulti > 0)
            ? static_cast<int>(interop.recheckThresholdMulti)
            : std::max(baseThreshold, 1200);
    const int finalThreshold =
        (frame.touch.output.contacts.size() > 2) ? multiThreshold : baseThreshold;
    const StylusNoiseEvidence evidence =
        BuildStylusNoiseEvidence(frame, finalThreshold);
    if (evidence.pointValid && evidence.stable) {
        m_lastStylusX = evidence.x;
        m_lastStylusY = evidence.y;
        m_stylusFramesSinceActive = 0;
    } else if (m_stylusFramesSinceActive < 1000000) {
        m_stylusFramesSinceActive += 1;
    }
    const int aftRecentFrames = m_stylusAftRecentFrames;
    if (m_stylusFramesSinceActive > aftRecentFrames) return false;
    outX = m_lastStylusX;
    outY = m_lastStylusY;
    return true;
}

// Screen-wide pen-mode rejection. Unlike the AFT path this is deliberately NOT gated on
// distance to the tip: a palm rests far outside m_stylusAftRadius, and during the linger
// tail there is no tip position at all.
//
// The age test is what keeps this from hijacking normal input. A track older than the
// current pen-mode episode was already being followed when the pen showed up, so it is an
// in-progress gesture (a scroll, a drag) and is left alone; only contacts born while the
// pen is active are candidates.
inline bool TouchTracker::ShouldPenModeSuppress(const TouchContact& touch, int touchAge) const {
    if (!m_penModeActive) return false;
    if (touchAge > m_penModeFramesElapsed) return false;

    // Area only, deliberately excluding the sizeMm branch the local AFT path uses.
    // sizeMm is not measured, it is fitted from signalSum as cbrt(signalSum) * 0.35, so
    // the 2.5 mm palm threshold is reached at signalSum >= 364 — a level essentially
    // every real contact clears, fingertips included. Screen-wide rejection keyed on
    // that would suppress all touch input whenever a pen is in range. Connected-pixel
    // areaCells is an actual measurement and separates a palm from a fingertip properly.
    return touch.areaCells >= m_stylusAftPalmAreaThreshold;
}

inline bool TouchTracker::ShouldStylusAftSuppress(const TouchContact& touch,
                                                  int touchAge,
                                                  float stylusX,
                                                  float stylusY,
                                                  int& outHoldFrames,
                                                  const StylusTouchSuppressor* ss) const {
    outHoldFrames = 0;
    const bool globalEnabled = ss ? ss->m_stylusSuppressGlobalEnabled : m_stylusSuppressGlobalEnabled;
    const bool aftEnabled = ss ? ss->m_stylusAftEnabled : m_stylusAftEnabled;
    if (!globalEnabled || !aftEnabled) return false;
    const float aftRadius = m_stylusAftRadius;
    if (DistanceSq(touch.x, touch.y, stylusX, stylusY) > aftRadius * aftRadius) return false;

    // The palm verdict is reached BEFORE the strong-touch exemption.
    //
    // Both branches were introduced together in 390d48d with the exemption first, so this
    // is not a regression — but on this panel that ordering makes the palm branch
    // unreachable for any contact it was written for. Reaching it requires areaCells >= 20 with
    // signalSum < 6000, i.e. a signal density below 300/unit. Replaying the recorded
    // fixture (Common/DVRCore/tests/fixtures/dvrbin/dataset.dvrbin) every contact measures
    // areaCells 31 / signalSum 18671 — density ~602, twice the ceiling. Only a hovering palm
    // shadow or a water film is that faint; a real resting palm always clears the
    // exemption and is waved through.
    //
    // Ordering palm first is what makes m_stylusAftPalm* mean anything. It only applies
    // within m_stylusAftRadius (~11 mm) of the pen tip, where a large contact is the
    // writing hand rather than deliberate input.
    //
    // Area only. sizeMm is not measured, it is fitted from signalSum as
    // cbrt(signalSum) * 0.35, so the 2.5 mm threshold is met at signalSum >= 364 — a
    // level any real contact clears, fingertips included. Keeping the sizeMm branch here
    // while palm is evaluated first would hold every contact near the tip for
    // m_stylusAftPalmSuppressFrames (~830 ms), which reads as touch latency and as
    // gestures refusing to start.
    const bool palm = touch.areaCells >= m_stylusAftPalmAreaThreshold;
    if (palm) {
        outHoldFrames = m_stylusAftPalmSuppressFrames;
        return true;
    }

    // Below the palm threshold the exemption does its intended job: protect a genuine
    // finger that happens to land near the pen tip.
    if (IsStrongTouchCandidate(touch, ss)) return false;

    const int weakSignalThold = ss ? ss->m_stylusAftWeakSignalThreshold : m_stylusAftWeakSignalThreshold;
    const float weakSizeThold = ss ? ss->m_stylusAftWeakSizeThresholdMm : m_stylusAftWeakSizeThresholdMm;
    const bool weak = (touch.signalSum < weakSignalThold) && (touch.sizeMm < weakSizeThold);
    const int debounceFrames = ss ? ss->m_stylusAftDebounceFrames : m_stylusAftDebounceFrames;
    const bool young = (touchAge <= debounceFrames);
    const int suppressFrames = ss ? ss->m_stylusAftSuppressFrames : m_stylusAftSuppressFrames;
    if (weak || young) {
        outHoldFrames = suppressFrames;
        return true;
    }
    return false;
}

inline void TouchTracker::SolveAssignment(const float* cost, int n, int m, int* rowToCol) const {
    if (n == 0 || m == 0) {
        for (int i = 0; i < n; ++i) rowToCol[i] = -1;
        return;
    }

    bool transposed = false;
    float matrix[kMaxTracks * kMaxTracks];
    int origN = n, origM = m;
    if (n > m) {
        transposed = true;
        for (int r = 0; r < n; ++r)
            for (int c2 = 0; c2 < m; ++c2)
                matrix[c2 * kMaxTracks + r] = cost[r * kMaxTracks + c2];
        std::swap(n, m);
    } else {
        for (int r = 0; r < n; ++r)
            for (int c2 = 0; c2 < m; ++c2)
                matrix[r * kMaxTracks + c2] = cost[r * kMaxTracks + c2];
    }

    const float kInf = std::numeric_limits<float>::max() / 8.0f;
    float u[kMaxTracks + 1] = {}, v[kMaxTracks + 1] = {};
    int p[kMaxTracks + 1] = {}, way[kMaxTracks + 1] = {};
    for (int i = 1; i <= n; ++i) {
        p[0] = i;
        int j0 = 0;
        float minv[kMaxTracks + 1];
        bool used[kMaxTracks + 1] = {};
        for (int j = 0; j <= m; ++j) minv[j] = kInf;
        do {
            used[j0] = true;
            const int i0 = p[j0];
            float delta = kInf;
            int j1 = 0;
            for (int j = 1; j <= m; ++j) {
                if (used[j]) continue;
                const float cur = matrix[(i0 - 1) * kMaxTracks + (j - 1)] - u[i0] - v[j];
                if (cur < minv[j]) {
                    minv[j] = cur;
                    way[j] = j0;
                }
                if (minv[j] < delta) {
                    delta = minv[j];
                    j1 = j;
                }
            }
            for (int j = 0; j <= m; ++j) {
                if (used[j]) {
                    u[p[j]] += delta;
                    v[j] -= delta;
                } else {
                    minv[j] -= delta;
                }
            }
            j0 = j1;
        } while (p[j0] != 0);
        do {
            const int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0 != 0);
    }

    if (!transposed) {
        for (int i = 0; i < n; ++i) rowToCol[i] = -1;
        for (int j = 1; j <= m; ++j)
            if (p[j] != 0) rowToCol[p[j] - 1] = j - 1;
    } else {
        for (int i = 0; i < origN; ++i) rowToCol[i] = -1;
        for (int prev = 0; prev < origM; ++prev) {
            int idx = -1;
            for (int j = 1; j <= m; ++j) {
                if (p[j] - 1 == prev) {
                    idx = j - 1;
                    break;
                }
            }
            if (idx >= 0 && idx < origN) rowToCol[idx] = prev;
        }
    }
}

inline bool TouchTracker::Process(HeatmapFrame& frame) {
    // 匹配坐标由 ZoneExpander 在接触点生成时填好，本级只读不写。上游没填过的一律
    // 退回 x/y——判据是两个分量同时恰为 0，而真实质心到不了这个值：它是至少一个格
    // 的加权平均，定点换算里还带 +0x80 的舍入项，补偿前的下限在 0.4 格上下。
    //
    // 这道兜底不是防御性编程的顺手一笔。改成按匹配坐标配对之后，只填 x/y 的调用方
    // 会拿到「所有接触点都在原点」的匹配结果，而且不报错、不崩溃，只是行为变了——
    // 这个错误已经发生过一次，被一个双指测试逮到，而在此之前它还蒙混过一轮全绿。
    for (auto& c : frame.touch.output.contacts) {
        if (c.matchXCells == 0.0f && c.matchYCells == 0.0f) {
            c.matchXCells = c.x;
            c.matchYCells = c.y;
        }
    }

    const auto* ss = frame.touch.runtime.stylusSuppress;
    const bool globalEnabled = ss ? ss->m_stylusSuppressGlobalEnabled : m_stylusSuppressGlobalEnabled;
    if (!globalEnabled) {
        m_stylusFramesSinceActive = 1000000;
        for (int i = 0; i < m_trackCount; ++i) {
            m_tracks[i].stylusSuppressFrames = 0;
        }
        frame.stylus.interop.touchSuppressActive = false;
        frame.stylus.interop.touchSuppressFrames = 0;
    }

    if (!m_enabled) {
        // Pass-through mode skips ResolveStylusAftContext, so pen-mode state would freeze
        // at whatever the last tracked frame left behind and re-enter the age test as a
        // stale episode length once tracking is switched back on.
        m_penModeActive = false;
        m_penModeFramesElapsed = 0;
        m_penModeGapFrames = 0;
        int nextId = 1;
        for (auto& c : frame.touch.output.contacts) {
            c.id = nextId++;
            c.state = TouchStateDown;
            c.isEdge = IsEdgeContact(c, 60, 40, 2.0f);
            // 直通模式没有轨迹，「只在落下时拒绝」无从表达，退化为逐帧压制。
            c.isReported = !c.edgeRejected;
            c.reportEvent = TouchReportIdle;
        }
        return true;
    }
    // Safety clamp: prevent out-of-bounds write if m_maxTouchCount exceeds fixed arrays
    const int effectiveMaxTouches = (std::min)(m_maxTouchCount, kMaxTracks);
    constexpr int effectiveMaxOutputContacts = kMaxTracks * 2;
    if (frame.touch.output.contacts.size() > static_cast<size_t>(effectiveMaxTouches))
        frame.touch.output.contacts.resize(static_cast<size_t>(effectiveMaxTouches));

    constexpr int kRows = 40, kCols = 60;
    constexpr float kEdgeMargin = 2.0f;
    float stylusAftX = 0, stylusAftY = 0;
    const bool stylusAftActive = ResolveStylusAftContext(frame, stylusAftX, stylusAftY, ss);
    const bool gapRelinkActive = m_gapRelinkEnabled && (m_gapRelinkWindowFrames > 0);

    const int curCount = static_cast<int>(std::min(frame.touch.output.contacts.size(), static_cast<size_t>(kMaxTracks)));
    const int preCount = m_trackCount;
    int curToPre[kMaxTracks];
    bool alwaysMatched[kMaxTracks] = {};
    bool gapRelinked[kMaxTracks] = {};
    for (int i = 0; i < curCount; ++i) curToPre[i] = -1;

    int activePrev[kMaxTracks], silentPrev[kMaxTracks];
    int activeCount = 0, silentCount = 0;
    for (int p = 0; p < preCount; ++p) {
        const auto& t = m_tracks[p];
        if (gapRelinkActive && t.phase == TrackPhase::SilentGap && t.gapFrames > 0 && t.gapFrames <= m_gapRelinkWindowFrames)
            silentPrev[silentCount++] = p;
        else
            activePrev[activeCount++] = p;
    }

    if (curCount > 0 && activeCount > 0) {
        MatchAgainstSubset(frame.touch.output.contacts.span(), curCount, activePrev, activeCount, curToPre);
        for (int c = 0; c < curCount; ++c) {
            const int pre = curToPre[c];
            if (pre < 0) continue;
            float refX = 0.0f, refY = 0.0f;
            GetMatchReference(m_tracks[pre], refX, refY);
            const float gateSq = ComputeTrackGateSq(m_tracks[pre], frame.touch.output.contacts[c], kCols, kRows, kEdgeMargin);
            if (DistanceSq(frame.touch.output.contacts[c].matchXCells,
                               frame.touch.output.contacts[c].matchYCells, refX, refY) > gateSq)
                curToPre[c] = -1;
        }

        if (gapRelinkActive && curCount == 1 && activeCount == 1 && silentCount == 0 && curToPre[0] < 0) {
            const int pre = activePrev[0];
            if (PassActiveBootstrapGate(m_tracks[pre], frame.touch.output.contacts[0], kCols, kRows, kEdgeMargin)) {
                curToPre[0] = pre;
            }
        }

        const float alwaysMatchSq = m_alwaysMatchDistance * m_alwaysMatchDistance;
        bool cUsed[kMaxTracks] = {}, pUsed[kMaxTracks] = {};
        for (int c = 0; c < curCount; ++c) {
            if (curToPre[c] >= 0) {
                cUsed[c] = true;
                pUsed[curToPre[c]] = true;
            }
        }
        for (int c = 0; c < curCount; ++c) {
            if (cUsed[c]) continue;
            float best = std::numeric_limits<float>::max();
            int bestPre = -1;
            for (int i = 0; i < activeCount; ++i) {
                const int pre = activePrev[i];
                if (pUsed[pre]) continue;
                float refX = 0.0f, refY = 0.0f;
                GetMatchReference(m_tracks[pre], refX, refY);
                const float d = DistanceSq(frame.touch.output.contacts[c].matchXCells,
                               frame.touch.output.contacts[c].matchYCells, refX, refY);
                if (d < best) { best = d; bestPre = pre; }
            }
            if (bestPre >= 0 && best <= alwaysMatchSq &&
                !IsEdgeContact(frame.touch.output.contacts[c], kCols, kRows, kEdgeMargin)) {
                curToPre[c] = bestPre;
                alwaysMatched[c] = true;
                cUsed[c] = true;
                pUsed[bestPre] = true;
            }
        }
    }

    if (gapRelinkActive && curCount > 0 && silentCount > 0) {
        float bestForCur[kMaxTracks], secondForCur[kMaxTracks];
        int bestTrackForCur[kMaxTracks];
        float bestForTrack[kMaxTracks], secondForTrack[kMaxTracks];
        int bestCurForTrack[kMaxTracks];
        for (int i = 0; i < kMaxTracks; ++i) {
            bestForCur[i] = secondForCur[i] = std::numeric_limits<float>::infinity();
            bestTrackForCur[i] = -1;
            bestForTrack[i] = secondForTrack[i] = std::numeric_limits<float>::infinity();
            bestCurForTrack[i] = -1;
        }
        for (int c = 0; c < curCount; ++c) {
            if (curToPre[c] >= 0) continue;
            for (int i = 0; i < silentCount; ++i) {
                const int pre = silentPrev[i];
                float refX = 0.0f, refY = 0.0f;
                GetMatchReference(m_tracks[pre], refX, refY);
                const float d = DistanceSq(frame.touch.output.contacts[c].matchXCells,
                               frame.touch.output.contacts[c].matchYCells, refX, refY);
                const float gateSq = ComputeTrackGateSq(m_tracks[pre], frame.touch.output.contacts[c], kCols, kRows, kEdgeMargin);
                if (d > gateSq) continue;
                UpdateBestCandidate(d, pre, bestForCur[c], secondForCur[c], bestTrackForCur[c]);
                UpdateBestCandidate(d, c, bestForTrack[pre], secondForTrack[pre], bestCurForTrack[pre]);
            }
        }
        for (int c = 0; c < curCount; ++c) {
            if (curToPre[c] >= 0) continue;
            const int pre = bestTrackForCur[c];
            if (pre < 0 || bestCurForTrack[pre] != c) continue;
            if (!PassGapRelinkAmbiguity(bestForCur[c], secondForCur[c])) continue;
            if (!PassGapRelinkAmbiguity(bestForTrack[pre], secondForTrack[pre])) continue;
            curToPre[c] = pre;
            gapRelinked[c] = true;
        }
    }

    bool preMatched[kMaxTracks] = {};
    TrackState nextTracks[kMaxTracks];
    int nextTrackCount = 0;
    TouchContact out[kMaxTracks * 2];
    int outCount = 0;

    for (int c = 0; c < curCount; ++c) {
        TouchContact o = frame.touch.output.contacts[c];
        const int pre = curToPre[c];
        if (pre < 0) continue;
        preMatched[pre] = true;
        TrackState t = m_tracks[pre];
        const bool wasSilentGap = (t.phase == TrackPhase::SilentGap);
        const int frameSpan = wasSilentGap ? std::max(1, t.gapFrames + 1) : 1;
        t.phase = TrackPhase::Active;
        t.gapFrames = 0;
        o.id = t.id;
        o.prevIndex = pre;
        o.isEdge = IsEdgeContact(o, kCols, kRows, kEdgeMargin);
        o.lifeFlags = TouchLifeMapped;
        if (o.isEdge) o.lifeFlags |= TouchLifeEdge;
        if (alwaysMatched[c]) o.lifeFlags |= TouchLifeAlwaysMatch;
        const float curSize = EstimateSizeMm(o.areaCells, o.signalSum);
        t.sizeMm = std::max({curSize, t.sizeMm, m_fallbackSizeMm});
        o.sizeMm = t.sizeMm;
        const bool emitDown = !wasSilentGap && (t.age <= 1 || t.downDebounceFrames > 0);
        o.state = emitDown ? TouchStateDown : TouchStateMove;
        if (emitDown && t.downDebounceFrames > 0) {
            o.lifeFlags |= TouchLifeDebounced;
            t.downDebounceFrames -= 1;
        }
        t.vx = (o.x - t.x) / static_cast<float>(frameSpan);
        t.vy = (o.y - t.y) / static_cast<float>(frameSpan);
        // 匹配速度走另一条坐标，StoreEdgeMetadata 随后才把新的匹配坐标搬进轨迹，
        // 所以这两行必须在它之前。
        t.matchVxCellsPerFrame = (o.matchXCells - t.matchXCells) / static_cast<float>(frameSpan);
        t.matchVyCellsPerFrame = (o.matchYCells - t.matchYCells) / static_cast<float>(frameSpan);
        t.x = o.x;
        t.y = o.y;
        t.areaCells = o.areaCells;
        t.signalSum = o.signalSum;
        if (o.sourcePeakId != 0) {
            t.sourcePeakId = o.sourcePeakId;
            t.sourcePeakAge = o.sourcePeakAge;
        }
        StoreEdgeMetadata(t, o);
        t.missed = 0;
        t.age += 1;
        // Either path can hold a track down: the local tip-radius AFT, or screen-wide pen
        // mode. Pen mode has to participate in the gate, otherwise the countdown would
        // drain during the linger tail, exactly when the tip signal is already gone.
        const bool penGate = stylusAftActive || m_penModeActive;
        if (!penGate && t.stylusSuppressFrames > 0) t.stylusSuppressFrames -= 1;
        bool aftSuppressed = false;
        if (penGate) {
            if (t.stylusSuppressFrames > 0) {
                aftSuppressed = true;
                t.stylusSuppressFrames -= 1;
            } else {
                int hold = 0;
                if (stylusAftActive &&
                    ShouldStylusAftSuppress(o, t.age, stylusAftX, stylusAftY, hold, ss)) {
                    aftSuppressed = true;
                    t.stylusSuppressFrames = std::max(0, hold - 1);
                } else if (ShouldPenModeSuppress(o, t.age)) {
                    aftSuppressed = true;
                    t.stylusSuppressFrames = std::max(0, m_stylusAftPalmSuppressFrames - 1);
                }
            }
        }
        o.isReported = !aftSuppressed;
        o.reportEvent = TouchReportIdle;
        o.reportFlags = 0;
        if (aftSuppressed) o.debugFlags = 0x101;
        else if (gapRelinked[c]) o.debugFlags = 0x21;
        else o.debugFlags = 0x01;
        if (outCount < effectiveMaxOutputContacts) out[outCount++] = o;
        if (nextTrackCount < effectiveMaxTouches) nextTracks[nextTrackCount++] = t;
    }

    for (int c = 0; c < curCount; ++c) {
        if (curToPre[c] >= 0) continue;
        TouchContact o = frame.touch.output.contacts[c];
        TrackState t;
        t.id = AllocateId(nextTracks, nextTrackCount);
        if (t.id == 0) continue;
        t.x = o.x; t.y = o.y; t.areaCells = o.areaCells; t.signalSum = o.signalSum;
        t.sourcePeakId = o.sourcePeakId;
        t.sourcePeakAge = o.sourcePeakAge;
        t.sizeMm = EstimateSizeMm(o.areaCells, o.signalSum);
        t.age = 1; t.missed = 0; t.phase = TrackPhase::Active; t.gapFrames = 0;
        o.isEdge = IsEdgeContact(o, kCols, kRows, kEdgeMargin);
        // EdgeRejector 的结论只在这里生效——建轨迹之前丢弃，而不是建好再置 isReported = false。
        // 后者只能压住第一帧：下一帧这个点就走已跟踪分支，isReported 被重新算出来，等于没拒。
        // 已跟踪分支不看这个标记，贴边滑动途中不会被中途掐断成一串 up/down。
        if (o.edgeRejected) continue;
        if (m_touchDownRejectEnabled) {
            const bool weak = (t.signalSum < m_touchDownRejectMinSignal);
            const bool tiny = (t.sizeMm < m_touchDownRejectMinSizeMm);
            const bool weakEdge = o.isEdge && (t.signalSum < m_touchDownEdgeRejectMinSignal);
            if ((weak && tiny) || weakEdge) continue;
        }
        t.downDebounceFrames = ComputeTouchDownDebounceFrames(o);
        if (stylusAftActive) {
            int hold = 0;
            if (ShouldStylusAftSuppress(o, t.age, stylusAftX, stylusAftY, hold, ss))
                t.stylusSuppressFrames = std::max(0, hold - 1);
        }
        // A track created while pen mode is asserted is by definition born during the
        // episode, so it never qualifies for the established-gesture exemption.
        if (t.stylusSuppressFrames <= 0 && ShouldPenModeSuppress(o, t.age)) {
            t.stylusSuppressFrames = std::max(0, m_stylusAftPalmSuppressFrames - 1);
        }
        m_nextIdSeed = (t.id % effectiveMaxTouches) + 1;
        o.id = t.id;
        o.state = TouchStateDown;
        o.sizeMm = t.sizeMm;
        o.isReported = (t.stylusSuppressFrames <= 0);
        o.prevIndex = -1;
        o.debugFlags = 0x02;
        o.lifeFlags = TouchLifeNew;
        if (o.isEdge) o.lifeFlags |= TouchLifeEdge;
        if (t.downDebounceFrames > 0) {
            o.lifeFlags |= TouchLifeDebounced;
            t.downDebounceFrames -= 1;
        }
        StoreEdgeMetadata(t, o);
        o.reportEvent = TouchReportIdle;
        o.reportFlags = 0;
        if (outCount < effectiveMaxOutputContacts) out[outCount++] = o;
        if (nextTrackCount < effectiveMaxTouches) nextTracks[nextTrackCount++] = t;
    }

    for (int p = 0; p < preCount; ++p) {
        if (preMatched[p]) continue;
        TrackState t = m_tracks[p];
        if (gapRelinkActive) {
            if (t.phase != TrackPhase::SilentGap) {
                t.phase = TrackPhase::SilentGap;
                t.gapFrames = 1;
            } else {
                t.gapFrames += 1;
            }
            t.missed = t.gapFrames;
            if (t.stylusSuppressFrames > 0) t.stylusSuppressFrames -= 1;
            if (t.gapFrames <= m_gapRelinkWindowFrames) {
                TouchContact hidden = BuildSilentGapContact(t, p, kCols, kRows, kEdgeMargin);
                if (outCount < effectiveMaxOutputContacts) out[outCount++] = hidden;
                if (nextTrackCount < effectiveMaxTouches) nextTracks[nextTrackCount++] = t;
            } else {
                TouchContact up = BuildLiftOffContact(t, p, kCols, kRows, kEdgeMargin);
                if (outCount < effectiveMaxOutputContacts) out[outCount++] = up;
            }
            continue;
        }

        t.missed += 1;
        if (t.stylusSuppressFrames > 0) t.stylusSuppressFrames -= 1;
        TouchContact up = BuildLiftOffContact(t, p, kCols, kRows, kEdgeMargin);
        if (outCount < effectiveMaxOutputContacts) out[outCount++] = up;
    }

    GhostSuppressor ghostSuppressor;
    ghostSuppressor.m_rxGhostFilterEnabled = m_rxGhostFilterEnabled;
    ghostSuppressor.m_rxGhostLineDelta = m_rxGhostLineDelta;
    ghostSuppressor.m_rxGhostWeakRatio = m_rxGhostWeakRatio;
    ghostSuppressor.m_rxGhostOnlyNew = m_rxGhostOnlyNew;
    ghostSuppressor.ProcessTracked(out, outCount, effectiveMaxTouches, nextTracks, nextTrackCount,
                                   TouchStateUp, TouchStateDown, TouchLifeSilentGap);

    frame.touch.output.contacts.assign(out, out + outCount);
    std::memcpy(m_tracks, nextTracks, sizeof(TrackState) * nextTrackCount);
    m_trackCount = nextTrackCount;

    m_trackAnchorCount = 0;
    for (int i = 0; i < m_trackCount; ++i) {
        m_trackAnchors[m_trackAnchorCount++] = {m_tracks[i].x, m_tracks[i].y};
    }

    int activeSuppressFrames = 0;
    for (int i = 0; i < m_trackCount; ++i) {
        activeSuppressFrames = std::max(activeSuppressFrames,
                                        m_tracks[i].stylusSuppressFrames);
    }
    frame.stylus.interop.touchSuppressFrames = static_cast<uint8_t>(
        std::clamp(std::max(activeSuppressFrames,
                            static_cast<int>(frame.stylus.interop.touchSuppressFrames)),
                   0, 255));
    if (activeSuppressFrames > 0) {
        frame.stylus.interop.touchSuppressActive = true;
    }
    return true;
}

}} // namespace Solvers::Touch
