#pragma once
// ── TouchPipeline Module: StrokeAggregator (第 5 级 —— 笔画) ──
// Header-only. 设计与取证见 docs/touch_stack.md 第三节。
//
// 厂商没有这一级：GripFilter_GetStage 每帧重算，只靠状态机与一道回退闸门约束跳变，
// 没有任何笔画级锁定（出处 SPEC_grip 七）。这是有意分歧——逐帧翻转正是「写着写着
// 跳一下」的来源，而掌抑制、按下侧迟滞、上报语义三件事都堵在这一级后面。
//
// 本级只聚合与累积证据，不决定上报。判定（verdict）与 hold / cancel 在 5.3、5.4
// 接上；当前 verdict 恒为 Valid、phase 恒为 Active，上报路径逐位不变。

#include "SolverTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Solvers { namespace Touch {

class StrokeAggregator {
public:
    static constexpr int kMaxStrokes = 32;
    static constexpr int kMaxTrackIdsPerStroke = 8;

    struct Stroke {
        bool inUse = false;
        bool claimedThisFrame = false;   // 只在一帧之内有意义
        int  id = 0;

        // ── 身份 ──
        // 轨迹会断，笔画不该断：跟踪级的 id 在失配超过重连窗口之后就释放并复用，
        // 所以笔画自己编号，并记下它由哪几条轨迹接起来的。
        std::array<int, kMaxTrackIdsPerStroke> trackIds{};
        int  trackIdCount = 0;

        // ── 时间 ──
        uint64_t firstSampleUs = 0;
        uint64_t lastSampleUs = 0;
        int  sampleCount = 0;
        int  gapCount = 0;            // 接续过几次（换了几回轨迹）
        int  lastFrameIndex = -1;

        // ── 位置（格，用契约 1 的匹配坐标）──
        float firstXCells = 0.0f, firstYCells = 0.0f;
        float lastXCells = 0.0f, lastYCells = 0.0f;
        float pathLengthCells = 0.0f;

        // 按下该落在哪里。记的是**上报坐标**：匹配坐标是补偿之前的量，拿它当按下位置
        // 等于把边缘补偿撤销掉（契约 1）。
        float downXCells = 0.0f, downYCells = 0.0f;

        // ── 形态：一律取迄今最大，不取当前帧值 ──
        // 实测我们的掌误报比真接触点更小（最大峰值信号中位 508 对 1916），掌是碎成
        // 小片进来的。取当前值会让同一条笔画在强弱之间反复翻转，取最大值才有单调性。
        int     maxAreaCells = 0;
        float   maxAreaMm2 = 0.0f;
        float   maxSizeMm = 0.0f;
        int16_t maxPeakSignal = 0;
        // maxGripRatio 等第 3 级把 gripRatio 挂成接触点字段之后再加（5.4 之前）。

        // ── 邻居与边缘 ──
        int  maxConcurrentStrokes = 0;
        bool everLeftEdge = false;

        // ── 判定 ──
        bool          decided = false;
        StrokeVerdict verdict = StrokeVerdict::Pending;
        int           decidedAtSample = 0;
        StrokePhase   phase = StrokePhase::Holding;

        [[nodiscard]] float StraightLengthCells() const {
            const float dx = lastXCells - firstXCells;
            const float dy = lastYCells - firstYCells;
            return std::sqrt(dx * dx + dy * dy);
        }
    };

    bool  m_enabled = true;
    // 接续判据：三条同时成立才算同一条笔画。
    //
    // 100 ms 是量出来的,不是猜的:手势语料上碎裂的四对轨迹间隔是 50/58/67/75 ms,
    // 而真正的两次点击间隔在 200 ms 以上,75 与 125 之间没有样本。扫参也验证过这道
    // 边界——400 ms 那一档开始把四边语料角上的重复点击并掉(37 条并成 30 条),
    // 那正是不该并的。距离与尺寸比仍是未标定的初值。
    float m_continueMaxGapMs = 100.0f;
    float m_continueMaxDistanceCells = 2.0f;
    float m_continueMinSizeRatio = 0.5f;
    float m_continueMaxSizeRatio = 2.0f;
    // 拿不到任何戳时按标称帧率推进内部时钟。这是个坏兜底,只是不让判据变成未定义:
    // 空闲帧根本不进管线(ProcessSignalConditioning 提前返回),本级不被调用,时钟
    // 就停在那里——手指抬起 600 ms 会被数成一帧,两次点击于是并成一条笔画。实测过。
    int   m_nominalFrameIntervalUs = 8333;
    // 判据：整条笔画的最大峰值信号够不够。0 表示关闭。
    //
    // 800 是在**掌 + 另一只手点击**的录制上标定的(`dvr20260824_145431`)。那份录音里
    // 掌不是一大块,而是同一小片区域里反复生灭的碎片——98 条笔画中 43 条峰值 < 1000,
    // 而真正的点击全部 ≥ 1000。主机看到的就是这堆碎片在不停按下抬起,手指怎么点都不稳。
    //
    // 800 这一档:掌碎片清掉 20/43,而轻触语料 15 次轻点一次不丢、手势语料 58 条笔画
    // 一条不丢。1000 能把 43 条全清掉,代价是轻触丢 3/15——那一档留给真机去定。
    int   m_holdMinPeakSignal = 800;
    // 证据一直不够时，等到第几个采样就判掌。这个数同时是**很轻的接触点要等多久**的
    // 上界:轻触语料上够不到峰值门槛的那两条,扣住的帧数恰好就是它。
    //
    // 4 个采样(约 33 ms)是量出来的:8 → 4 之后掌语料「只有我们报」1134 → 1109
    // (略好,不是略差),手势语料「只有厂商报」仍是 0,而轻触的最长等待从 66 ms 减半。
    // 再往下到 3 收益已经很薄,而拥挤度判据的证据也随之变薄。
    int   m_decideMaxSamples = 4;
    // 判掌之前还要求身边有几条笔画同时存在。**1 表示不要求**,也就是只看强弱。
    //
    // 这条判据原先是 4,从掌语料(`012833`)标出来的——那份录音里掌一次会冒十几个接触点。
    // 「掌 + 另一只手点击」的录制推翻了它:那里掌一次只冒 1..3 个碎片,够不到 4,于是
    // 全被放行,保护方向正好反了。同一份语料上,把它设成 1 之后掌碎片才压得下去。
    //
    // 留着这个旋钮是因为它在多指同时按的场景里仍有意义,但默认不要求。
    int   m_palmMinConcurrentStrokes = 1;

    inline bool Process(HeatmapFrame& frame) {
        if (!m_enabled) return true;

        ++m_frameIndex;
        const uint64_t nowUs = AdvanceClock(frame);

        // 先按时间退休,再配对。反过来做会漏一种情况:轨迹号抬起即回收,一次长空档
        // 之后新手指可能拿到同一个号,而 m_frameIndex 在空闲帧上不增加(那些帧根本
        // 不进管线),于是「上一帧还活着」这个判据对隔了一秒的两次按压照样成立。
        for (auto& s : m_strokes) s.claimedThisFrame = false;
        Retire(nowUs);

        auto& contacts = frame.touch.output.contacts;
        // 归属每帧重算。接触点由第 3 级逐帧新建，这里不依赖它一定是 0。
        for (auto& c : contacts) c.strokeId = 0;

        // 一遍：轨迹 id 直接命中上一帧还活着的笔画。id 抬起即回收，所以只认上一帧
        // 就活着的笔画——隔了几帧的同号轨迹多半是另一根手指，交给接续判据去查。
        for (auto& c : contacts) {
            if (c.id <= 0) continue;
            Stroke* s = FindByTrackId(c.id);
            if (s != nullptr && s->lastFrameIndex == m_frameIndex - 1 && !s->claimedThisFrame) {
                Append(*s, c, nowUs);
            }
        }
        // 二遍：没归属的接触点找可接续的笔画，找不到就新建。
        for (auto& c : contacts) {
            if (c.id <= 0 || c.strokeId != 0) continue;
            Stroke* s = FindContinuation(c, nowUs);
            if (s != nullptr) {
                AdoptTrackId(*s, c.id);
                Append(*s, c, nowUs);
            } else {
                Stroke* fresh = Allocate(c, nowUs);
                if (fresh != nullptr) Append(*fresh, c, nowUs);
            }
        }

        int aliveCount = 0;
        for (const auto& s : m_strokes) {
            if (s.inUse && s.claimedThisFrame) ++aliveCount;
        }
        for (auto& s : m_strokes) {
            if (!s.inUse || !s.claimedThisFrame) continue;
            s.maxConcurrentStrokes = std::max(s.maxConcurrentStrokes, aliveCount);
            Decide(s);
        }

        // 回填给第 6 级:阶段决定发什么事件,按下位置决定发在哪里。
        for (auto& c : contacts) {
            const Stroke* s = FindById(c.strokeId);
            c.strokePhase = static_cast<uint8_t>(s != nullptr ? s->phase : StrokePhase::Active);
            if (s != nullptr) {
                c.strokeDownX = s->downXCells;
                c.strokeDownY = s->downYCells;
            }
        }
        return true;
    }

    // 第 6 级的抬起事件是合成出来的:轨迹已经不在输出里了,手势层按槽位状态另建一个
    // 接触点补发抬起(TouchGestureStateMachine.hpp Phase 3)。它没经过上面的 Process,
    // 所以归属要在手势层跑完之后补盖一次。这里只盖章,不记证据——那一帧的位置与面积
    // 来自槽位里的陈旧副本,当成新采样会污染笔画的最大值。
    //
    // 5.3 之后这个补盖点会变成笔画终结的记录点:抬起发不发由笔画阶段决定。
    inline void StampLateContacts(HeatmapFrame& frame) {
        if (!m_enabled) return;
        for (auto& c : frame.touch.output.contacts) {
            if (c.id <= 0 || c.strokeId != 0) continue;
            const Stroke* s = FindByTrackId(c.id);
            if (s == nullptr) continue;
            c.strokeId = s->id;
            c.strokePhase = static_cast<uint8_t>(s->phase);
        }
    }

    inline void ClearLiveState() {
        for (auto& s : m_strokes) s = Stroke{};
        m_nextStrokeId = 1;
        m_frameIndex = -1;
        m_clockUs = 0;
    }

    [[nodiscard]] std::array<Stroke, kMaxStrokes> GetStrokes() const { return m_strokes; }

private:
    inline uint64_t AdvanceClock(const HeatmapFrame& frame) {
        // 优先用主机读到帧的时刻:它在空闲帧上照样走,而本级在空闲帧上不被调用。
        // frame.timestamp 现在恒为 0(它曾经取自 master 后缀 word 9,而那个字是
        // 频点码不是戳),留着这一支是为了设备侧真打上戳之后自动切过去。
        if (frame.receiveSystemEpochUs != 0) {
            m_clockUs = frame.receiveSystemEpochUs;
        } else if (frame.timestamp != 0) {
            m_clockUs = frame.timestamp;
        } else {
            m_clockUs += static_cast<uint64_t>(std::max(1, m_nominalFrameIntervalUs));
        }
        return m_clockUs;
    }

    inline Stroke* FindById(int strokeId) {
        if (strokeId <= 0) return nullptr;
        for (auto& s : m_strokes) {
            if (s.inUse && s.id == strokeId) return &s;
        }
        return nullptr;
    }

    inline Stroke* FindByTrackId(int trackId) {
        for (auto& s : m_strokes) {
            if (!s.inUse) continue;
            for (int i = 0; i < s.trackIdCount; ++i) {
                if (s.trackIds[static_cast<size_t>(i)] == trackId) return &s;
            }
        }
        return nullptr;
    }

    inline Stroke* FindContinuation(const TouchContact& c, uint64_t nowUs) {
        const float maxGapUs = m_continueMaxGapMs * 1000.0f;
        const float maxDistSq = m_continueMaxDistanceCells * m_continueMaxDistanceCells;
        Stroke* best = nullptr;
        float bestDistSq = maxDistSq;
        for (auto& s : m_strokes) {
            if (!s.inUse || s.claimedThisFrame) continue;
            if (static_cast<float>(nowUs - s.lastSampleUs) > maxGapUs) continue;
            const float dx = MatchX(c) - s.lastXCells;
            const float dy = MatchY(c) - s.lastYCells;
            const float distSq = dx * dx + dy * dy;
            if (distSq > bestDistSq) continue;
            // 尺寸比：任一侧为 0 就不判（sizeMm 在极弱接触点上会拟合出 0），
            // 剩下两条判据仍然要过。
            if (s.maxSizeMm > 0.0f && c.sizeMm > 0.0f) {
                const float ratio = c.sizeMm / s.maxSizeMm;
                if (ratio < m_continueMinSizeRatio || ratio > m_continueMaxSizeRatio) continue;
            }
            best = &s;
            bestDistSq = distSq;
        }
        return best;
    }

    inline Stroke* Allocate(const TouchContact& c, uint64_t nowUs) {
        for (auto& s : m_strokes) {
            if (s.inUse) continue;
            s = Stroke{};
            s.inUse = true;
            s.id = m_nextStrokeId++;
            if (m_nextStrokeId <= 0) m_nextStrokeId = 1;
            s.trackIds[0] = c.id;
            s.trackIdCount = 1;
            s.firstSampleUs = nowUs;
            s.firstXCells = MatchX(c);
            s.firstYCells = MatchY(c);
            s.lastXCells = s.firstXCells;
            s.lastYCells = s.firstYCells;
            return &s;
        }
        return nullptr;  // 槽位耗尽：接触点上限 20，笔画上限 32，正常到不了这里
    }

    inline void AdoptTrackId(Stroke& s, int trackId) {
        for (int i = 0; i < s.trackIdCount; ++i) {
            if (s.trackIds[static_cast<size_t>(i)] == trackId) return;
        }
        if (s.trackIdCount < kMaxTrackIdsPerStroke) {
            s.trackIds[static_cast<size_t>(s.trackIdCount++)] = trackId;
        }
        ++s.gapCount;
    }

    inline void Append(Stroke& s, TouchContact& c, uint64_t nowUs) {
        const float x = MatchX(c);
        const float y = MatchY(c);
        if (s.sampleCount > 0) {
            const float dx = x - s.lastXCells;
            const float dy = y - s.lastYCells;
            s.pathLengthCells += std::sqrt(dx * dx + dy * dy);
        }
        if (s.sampleCount == 0) {
            s.downXCells = c.x; s.downYCells = c.y;
        }
        s.lastXCells = x;
        s.lastYCells = y;
        s.lastSampleUs = nowUs;
        s.lastFrameIndex = m_frameIndex;
        ++s.sampleCount;
        s.claimedThisFrame = true;

        s.maxAreaCells = std::max(s.maxAreaCells, c.areaCells);
        s.maxAreaMm2 = std::max(s.maxAreaMm2, c.areaMm2);
        s.maxSizeMm = std::max(s.maxSizeMm, c.sizeMm);
        s.maxPeakSignal = std::max(s.maxPeakSignal, c.peakSignal);
        if (!c.isEdge) s.everLeftEdge = true;

        c.strokeId = s.id;
    }

    // 判定只做一次，判完粘着。时序抄 Chromium 的 ShouldDecideStroke，判据不抄——
    // 它判「大 ⇒ 掌」（长轴 20 mm、面积 400 mm²），而我们的误报比真接触点更小，
    // 那条规则一次都不会触发。
    //
    // 「证据一够就立刻放行」不是设计偏好，是量出来的：真笔画的峰值在**第一个采样**
    // 就够门槛（中位 0、p90 0，62 条里只有 1 条要等到第 14 个）。写成「先等 N 帧再判」
    // 等于对每条笔画白收一次延迟，上次失败的变体正是死在这里（手势语料 0 帧漏报变 89）。
    inline void Decide(Stroke& s) {
        if (s.decided) {
            // 判掌之后仍然允许翻回有效,但**只允许这一个方向**。证据是「迄今最大峰值」,
            // 单调不减,所以这一翻最多发生一次,不会来回跳——契约 3 要禁的是反复改判,
            // 不是永不认错。
            //
            // 不留这条口子的后果是真机上出现过的:手掌搭在屏幕上把拥挤度抬起来,旁边
            // 真手指的一次轻点被判成掌,而判定粘着,于是这根手指在整个接触期间**一直
            // 不响应**,直到抬起重按。
            if (s.verdict == StrokeVerdict::Palm && m_holdMinPeakSignal > 0 &&
                s.maxPeakSignal >= m_holdMinPeakSignal) {
                s.verdict = StrokeVerdict::Valid;
                s.decidedAtSample = s.sampleCount;
            }
            s.phase = s.verdict == StrokeVerdict::Valid ? StrokePhase::Active
                                                        : StrokePhase::Cancelled;
            return;
        }
        if (m_holdMinPeakSignal <= 0 || s.maxPeakSignal >= m_holdMinPeakSignal) {
            s.decided = true;
            s.verdict = StrokeVerdict::Valid;
            s.decidedAtSample = s.sampleCount;
            s.phase = StrokePhase::Active;
            return;
        }
        if (s.sampleCount >= m_decideMaxSamples) {
            const bool crowded = s.maxConcurrentStrokes >= m_palmMinConcurrentStrokes;
            s.decided = true;
            s.verdict = crowded ? StrokeVerdict::Palm : StrokeVerdict::Valid;
            s.decidedAtSample = s.sampleCount;
            s.phase = crowded ? StrokePhase::Cancelled : StrokePhase::Active;
            return;
        }
        s.phase = StrokePhase::Holding;
    }

    inline void Retire(uint64_t nowUs) {
        const float maxGapUs = m_continueMaxGapMs * 1000.0f;
        for (auto& s : m_strokes) {
            if (!s.inUse || s.claimedThisFrame) continue;
            if (static_cast<float>(nowUs - s.lastSampleUs) > maxGapUs) s = Stroke{};
        }
    }

    static inline float MatchX(const TouchContact& c) { return c.matchXCells; }
    static inline float MatchY(const TouchContact& c) { return c.matchYCells; }

    std::array<Stroke, kMaxStrokes> m_strokes{};
    int m_nextStrokeId = 1;
    int m_frameIndex = -1;
    uint64_t m_clockUs = 0;
};

}} // namespace Solvers::Touch
