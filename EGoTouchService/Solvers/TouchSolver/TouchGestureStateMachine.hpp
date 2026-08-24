#pragma once
// ── TouchPipeline Module: TouchGestureStateMachine ──
// Header-only. Converted from Reporting/TouchGestureStateMachine.{h,cpp}.
// 5-Phase gesture lifecycle per slot.

#include "SolverTypes.h"
#include <array>
#include <cmath>
#include <cstdint>

namespace Solvers { namespace Touch {

enum class GesturePhase : uint8_t {
    Idle = 0, PressCandidate, Dragging, LongPressHold, ReleasePending,
};

struct GestureSlot {
    GesturePhase phase = GesturePhase::Idle;
    GesturePhase prevPhase = GesturePhase::Idle;
    float anchorX = 0, anchorY = 0;
    float lastTrackedX = 0, lastTrackedY = 0;
    float lastOutputX = 0, lastOutputY = 0;
    // 拖动期间输出坐标相对指尖真实位置的固定偏移，见 EnterDragging。
    float dragOffsetX = 0, dragOffsetY = 0;
    uint16_t ageFrames = 0, missingFrames = 0, stableFrames = 0;
    float sizeMm = 0; int signalSum = 0, areaCells = 0; bool isEdge = false;
    bool upEmitted = false;
    // 这个槽位有没有真的发出过按下。作废一条笔画时要靠它区分两种情形:发过按下的
    // 必须补一次抬起,否则主机那边指针一直按着;从没发过的什么都不能发,发了就是
    // 凭空一次点击。
    bool downEmitted = false;
    void Reset() {
        phase = prevPhase = GesturePhase::Idle;
        anchorX = anchorY = lastTrackedX = lastTrackedY = lastOutputX = lastOutputY = 0;
        dragOffsetX = dragOffsetY = 0;
        ageFrames = missingFrames = stableFrames = 0;
        sizeMm = 0; signalSum = areaCells = 0; isEdge = false;
        upEmitted = false;
        downEmitted = false;
    }
};

class TouchGestureStateMachine {
public:
    static constexpr int kMaxSlots = 20;
    bool  m_enabled = true;
    int   m_pressCandidateFrames = 1;
    int   m_pressCandidateMinSignal = 0;
    float m_pressCandidateMinSizeMm = 0.0f;
    float m_dragThreshold = 0.8f;
    int   m_longPressFrames = 46;
    float m_longPressMoveTolerance = 0.8f;
    int   m_releasePendingFrames = 0;
    bool  m_bypassStateMachine = false;
    // 拖动偏移消解到零所用的帧数。见 EnterDragging。
    int   m_dragOffsetDecayFrames = 10;

    // 进入拖动时把输出坐标沿起手方向回退一个 m_dragThreshold。越过阈值之前输出一直钉在
    // anchor 上，不做这个回退的话，越过阈值那一帧输出会从 anchor 直接跳到指尖真实位置，
    // 跳变幅度恰好是一个阈值——按默认的 0.8 格、4.5mm 间距算约 3.6mm，屏幕上三十多个像素。
    //
    // 这个偏移曾经固定不变，理由是照搬 Chromium 扣掉首个 scroll delta 中 slop 的做法。
    // 那个类比不成立：Chromium 那边是内容相对滚动，永久偏移看不出来；这里是绝对指针定位，
    // 永久偏移意味着指针永远够不到起手方向那一侧的屏幕边缘——手指压在物理边框上，选区还差
    // 一个阈值。缺口在两轴上按格数缩放（横 0.8/60、纵 0.8/40），实测比例 1.48 对上了 1.5。
    //
    // 改为在 m_dragOffsetDecayFrames 帧内线性消解：起手那一帧照样不跳，之后指针以每帧
    // 阈值/N 的速度追上指尖，追平后输出即真实位置，边缘可达。追赶量每帧约 0.36 mm，
    // 拖动时指尖本身每帧走得更多，看不出来。
    inline void EnterDragging(GestureSlot& slot, const TouchContact& contact) {
        const float dx = contact.x - slot.anchorX;
        const float dy = contact.y - slot.anchorY;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > m_dragThreshold) {
            const float k = m_dragThreshold / dist;
            slot.dragOffsetX = -dx * k;
            slot.dragOffsetY = -dy * k;
        } else {
            slot.dragOffsetX = slot.dragOffsetY = 0.0f;
        }
        slot.phase = GesturePhase::Dragging;
        slot.lastOutputX = contact.x + slot.dragOffsetX;
        slot.lastOutputY = contact.y + slot.dragOffsetY;
    }

    // 按固定步长缩短偏移的模长，方向不变。分量各自衰减会让指针在追赶途中拐向坐标轴。
    inline void DecayDragOffset(GestureSlot& slot) const {
        if (m_dragOffsetDecayFrames <= 0) {
            slot.dragOffsetX = slot.dragOffsetY = 0.0f;
            return;
        }
        const float step = m_dragThreshold / static_cast<float>(m_dragOffsetDecayFrames);
        const float mag = std::sqrt(slot.dragOffsetX * slot.dragOffsetX +
                                    slot.dragOffsetY * slot.dragOffsetY);
        if (mag <= step) {
            slot.dragOffsetX = slot.dragOffsetY = 0.0f;
            return;
        }
        const float scale = (mag - step) / mag;
        slot.dragOffsetX *= scale;
        slot.dragOffsetY *= scale;
    }

    TouchGestureStateMachine() { ClearLiveState(); }
    inline void ClearLiveState() {
        for (auto& slot : m_slots) slot.Reset();
    }
    inline bool HasLiveState() const {
        for (const auto& slot : m_slots) {
            if (slot.phase != GesturePhase::Idle || slot.upEmitted) {
                return true;
            }
        }
        return false;
    }

    inline bool Process(HeatmapFrame& frame) {
        if (!m_enabled) return true;

        // Bypass mode
        if (m_bypassStateMachine) {
            for (auto& c : frame.touch.output.contacts) {
                if (c.id <= 0 || !c.isReported) continue;
                switch (c.state) {
                case TouchStateDown: c.reportEvent = TouchReportDown; break;
                case TouchStateMove: c.reportEvent = TouchReportMove; break;
                case TouchStateUp:   c.reportEvent = TouchReportUp;   break;
                default: break;
                }
            }
            return true;
        }

        // Build slot→contact mapping
        std::array<TouchContact*, kMaxSlots> contactForSlot{};
        contactForSlot.fill(nullptr);
        for (auto& c : frame.touch.output.contacts) {
            const bool hiddenContinuation = (c.lifeFlags & TouchLifeSilentGap) != 0;
            if (!c.isReported && !hiddenContinuation) continue;
            const int idx = c.id - 1;
            if (idx >= 0 && idx < kMaxSlots) contactForSlot[idx] = &c;
        }
        for (int i = 0; i < kMaxSlots; ++i) {
            if (contactForSlot[i] && contactForSlot[i]->state == TouchStateUp)
                contactForSlot[i] = nullptr;
        }

        // 第 5 级判为掌的笔画在这里落地。本级**不判掌**,只执行判定(契约 3)。
        // 判错了回第 5 级修证据或阈值,不要在这里加补丁。
        for (int i = 0; i < kMaxSlots; ++i) {
            TouchContact* contact = contactForSlot[i];
            if (contact == nullptr) continue;
            if (contact->strokePhase != static_cast<uint8_t>(StrokePhase::Cancelled)) continue;
            contact->isReported = false;
            contact->reportEvent = TouchReportIdle;
            if (m_slots[i].downEmitted) {
                // 已经发过按下:当作接触点消失,走正常的抬起路径把它收掉。
                contactForSlot[i] = nullptr;
            } else {
                // 从没发过按下:整条笔画对主机而言没发生过,连抬起都不该有。
                m_slots[i].Reset();
                contactForSlot[i] = nullptr;
            }
        }

        // Phase 1: Update slots
        for (int i = 0; i < kMaxSlots; ++i) {
            auto& slot = m_slots[i];
            TouchContact* contact = contactForSlot[i];
            if (slot.phase == GesturePhase::Idle && contact == nullptr) {
                if (slot.upEmitted) slot.upEmitted = false;
                continue;
            }
            if (slot.phase == GesturePhase::Idle && slot.upEmitted) {
                slot.upEmitted = false;
                if (contact) { contact->isReported = false; contact->reportEvent = TouchReportIdle; }
                continue;
            }
            UpdateSlot(slot, contact, i);
        }

        // Phase 2: Rewrite output fields
        for (auto& c : frame.touch.output.contacts) {
            const int idx = c.id - 1;
            if (idx < 0 || idx >= kMaxSlots) continue;
            if (c.state == TouchStateUp) { c.isReported = false; continue; }
            // 这里曾经额外写着 `|| (c.lifeFlags & TouchLifeSilentGap) != 0`，把静默空档
            // 的接触点无条件压成不上报，盖过跟踪级已经做过的同一个决定。后果是重连窗口
            // 形同虚设：轨迹保住了，主机看到的仍是接触点消失又出现，即一次断开。
            // 「报不报」只由跟踪级决定，本级只按手势阶段改写坐标与事件。
            if (!c.isReported) {
                c.reportEvent = TouchReportIdle;
                continue;
            }
            auto& slot = m_slots[idx];
            switch (slot.phase) {
            case GesturePhase::Idle:
                c.isReported = false; c.reportEvent = TouchReportIdle; break;
            case GesturePhase::PressCandidate:
                if (slot.stableFrames >= static_cast<uint16_t>(m_pressCandidateFrames)) {
                    c.isReported = true; c.reportEvent = TouchReportDown;
                    c.x = slot.lastOutputX; c.y = slot.lastOutputY;
                    slot.downEmitted = true;
                } else { c.isReported = false; c.reportEvent = TouchReportIdle; }
                break;
            case GesturePhase::Dragging:
            case GesturePhase::LongPressHold:
            case GesturePhase::ReleasePending:
                c.isReported = true; c.reportEvent = TouchReportMove;
                c.x = slot.lastOutputX; c.y = slot.lastOutputY;
                slot.downEmitted = true; break;
            }
        }

        // Phase 3: ReleasePending → Idle (emit Up)
        for (int i = 0; i < kMaxSlots; ++i) {
            auto& slot = m_slots[i];
            if (slot.phase != GesturePhase::ReleasePending) continue;
            if (slot.missingFrames <= static_cast<uint16_t>(m_releasePendingFrames)) continue;
            TouchContact upEvent;
            upEvent.id = i + 1; upEvent.x = slot.lastOutputX; upEvent.y = slot.lastOutputY;
            upEvent.state = TouchStateUp; upEvent.areaCells = slot.areaCells;
            upEvent.signalSum = slot.signalSum; upEvent.sizeMm = slot.sizeMm;
            upEvent.isEdge = slot.isEdge; upEvent.isReported = true;
            upEvent.reportEvent = TouchReportUp;
            upEvent.lifeFlags = TouchLifeLiftOff; upEvent.reportFlags = 0;
            if (!frame.touch.output.contacts.try_push_back(upEvent)) {
                return false;
            }
            slot.Reset(); slot.upEmitted = true;
        }
        return true;
    }

private:
    std::array<GestureSlot, kMaxSlots> m_slots{};

    inline void UpdateSlot(GestureSlot& slot, const TouchContact* contact, int) {
        switch (slot.phase) {
        case GesturePhase::Idle:
            if (contact) {
                slot.phase = GesturePhase::PressCandidate;
                slot.anchorX = contact->x; slot.anchorY = contact->y;
                slot.lastTrackedX = contact->x; slot.lastTrackedY = contact->y;
                slot.lastOutputX = contact->x; slot.lastOutputY = contact->y;
                slot.ageFrames = 1; slot.missingFrames = 0;
                // 起手这一帧就可能还在 hold 里,那就不算稳定帧,按下等判定下来再发。
                slot.stableFrames =
                    contact->strokePhase == static_cast<uint8_t>(StrokePhase::Holding) ? 0 : 1;
                slot.sizeMm = contact->sizeMm; slot.signalSum = contact->signalSum;
                slot.areaCells = contact->areaCells; slot.isEdge = contact->isEdge;
            }
            break;
        case GesturePhase::PressCandidate:
            if (!contact) {
                slot.prevPhase = GesturePhase::PressCandidate;
                slot.phase = GesturePhase::ReleasePending; slot.missingFrames = 1; return;
            }
            slot.ageFrames += 1; slot.missingFrames = 0;
            slot.lastTrackedX = contact->x; slot.lastTrackedY = contact->y;
            slot.sizeMm = contact->sizeMm; slot.signalSum = contact->signalSum;
            slot.areaCells = contact->areaCells; slot.isEdge = contact->isEdge;
            // 笔画还没判下来的时候不放行。锚点保持在起手那一帧,所以随后判为有效时
            // 补发的按下落在**手指最初落下的位置**,不是判定那一帧的位置。
            { const bool holding =
                  contact->strokePhase == static_cast<uint8_t>(StrokePhase::Holding);
              bool stable = !holding;
              if (m_pressCandidateMinSignal > 0 && contact->signalSum < m_pressCandidateMinSignal) stable = false;
              if (m_pressCandidateMinSizeMm > 0.0f && contact->sizeMm < m_pressCandidateMinSizeMm) stable = false;
              slot.stableFrames = stable ? (slot.stableFrames + 1) : 0;
              // 按下还没发出去之前,起手点由第 5 级说了算:移入修正会把它挪到边界上。
              // 发出去之后就不能再动,那是拖动的事。
              if (contact->strokeId != 0 && !slot.downEmitted) {
                  slot.anchorX = contact->strokeDownX;
                  slot.anchorY = contact->strokeDownY;
              }
              slot.lastOutputX = slot.anchorX; slot.lastOutputY = slot.anchorY;
              // hold 期间不许滑进拖动:那条路直接发移动,主机从没收到过按下。
              // 按下还没发出去时同样不许——扣住的几帧里手指可能已经走过了拖动阈值,
              // 放行那一帧若直接进拖动,这个槽位对主机而言就只有移动没有按下。
              if (holding || !slot.downEmitted) break;
            }
            { float dx = contact->x - slot.anchorX, dy = contact->y - slot.anchorY;
              if (dx*dx+dy*dy > m_dragThreshold*m_dragThreshold) {
                  EnterDragging(slot, *contact); return;
              }
            }
            if (slot.ageFrames >= static_cast<uint16_t>(m_longPressFrames)) {
                float dx = contact->x - slot.anchorX, dy = contact->y - slot.anchorY;
                if (dx*dx+dy*dy <= m_longPressMoveTolerance*m_longPressMoveTolerance) {
                    slot.phase = GesturePhase::LongPressHold;
                }
            }
            break;
        case GesturePhase::Dragging:
            if (!contact) {
                slot.prevPhase = GesturePhase::Dragging;
                slot.phase = GesturePhase::ReleasePending;
                slot.missingFrames = static_cast<uint16_t>(m_releasePendingFrames + 1); return;
            }
            slot.ageFrames += 1; slot.missingFrames = 0;
            slot.lastTrackedX = contact->x; slot.lastTrackedY = contact->y;
            DecayDragOffset(slot);
            slot.lastOutputX = contact->x + slot.dragOffsetX;
            slot.lastOutputY = contact->y + slot.dragOffsetY;
            slot.sizeMm = contact->sizeMm; slot.signalSum = contact->signalSum;
            slot.areaCells = contact->areaCells; slot.isEdge = contact->isEdge;
            break;
        case GesturePhase::LongPressHold:
            if (!contact) {
                slot.prevPhase = GesturePhase::LongPressHold;
                slot.phase = GesturePhase::ReleasePending;
                slot.missingFrames = static_cast<uint16_t>(m_releasePendingFrames + 1); return;
            }
            slot.ageFrames += 1; slot.missingFrames = 0;
            slot.lastTrackedX = contact->x; slot.lastTrackedY = contact->y;
            slot.sizeMm = contact->sizeMm; slot.signalSum = contact->signalSum;
            slot.areaCells = contact->areaCells; slot.isEdge = contact->isEdge;
            { float dx = contact->x - slot.anchorX, dy = contact->y - slot.anchorY;
              if (dx*dx+dy*dy > m_dragThreshold*m_dragThreshold) {
                  EnterDragging(slot, *contact); return;
              }
            }
            slot.lastOutputX = slot.anchorX; slot.lastOutputY = slot.anchorY;
            break;
        case GesturePhase::ReleasePending:
            if (contact) {
                slot.phase = slot.prevPhase; slot.missingFrames = 0;
                slot.lastTrackedX = contact->x; slot.lastTrackedY = contact->y;
                slot.sizeMm = contact->sizeMm; slot.signalSum = contact->signalSum;
                slot.areaCells = contact->areaCells; slot.isEdge = contact->isEdge;
                if (slot.phase == GesturePhase::Dragging) {
                    slot.lastOutputX = contact->x + slot.dragOffsetX;
                    slot.lastOutputY = contact->y + slot.dragOffsetY;
                }
                return;
            }
            slot.missingFrames += 1;
            break;
        }
    }
};

}} // namespace Solvers::Touch
