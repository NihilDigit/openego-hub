#include "TouchSolver/CoordinateFilter.hpp"
#include "TouchSolver/TouchGestureStateMachine.hpp"
#include "TouchSolver/TouchTracker.hpp"
#include "TouchSolver/TouchPipeline.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using Solvers::HeatmapFrame;
using Solvers::TouchContact;
using Solvers::TouchLifeMapped;
using Solvers::TouchLifeSilentGap;
using Solvers::TouchReportDown;
using Solvers::TouchReportIdle;
using Solvers::TouchReportMove;
using Solvers::TouchReportUp;
using Solvers::TouchStateDown;
using Solvers::TouchStateMove;
using Solvers::Touch::CoordinateFilter;
using Solvers::Touch::TouchGestureStateMachine;
using Solvers::Touch::TouchTracker;

struct PipelineHarness {
    TouchTracker tracker;
    CoordinateFilter filter;
    TouchGestureStateMachine gesture;
    uint64_t timestamp = 0;

    PipelineHarness() = default;

    explicit PipelineHarness(int gapRelinkWindowFrames) {
        tracker.m_gapRelinkWindowFrames = gapRelinkWindowFrames;
    }

    HeatmapFrame Run(std::initializer_list<std::pair<float, float>> points) {
        return RunFlagged(points, false);
    }

    // edgeRejected 由 EdgeRejector 在 tracker 之前写入，这里直接置位以隔离 tracker 的取用逻辑。
    HeatmapFrame RunFlagged(std::initializer_list<std::pair<float, float>> points,
                            bool edgeRejected) {
        HeatmapFrame frame;
        timestamp += 8;
        frame.timestamp = timestamp;
        for (const auto& [x, y] : points) {
            TouchContact c;
            c.x = x;
            c.y = y;
            c.areaCells = 12;
            c.signalSum = 1200;
            c.edgeRejected = edgeRejected;
            frame.touch.output.contacts.push_back(c);
        }
        tracker.Process(frame);
        filter.Process(frame);
        gesture.Process(frame);
        return frame;
    }
};

std::vector<const TouchContact*> VisibleContacts(const HeatmapFrame& frame) {
    std::vector<const TouchContact*> out;
    for (const auto& c : frame.touch.output.contacts) {
        if (c.isReported) out.push_back(&c);
    }
    return out;
}

const TouchContact* FindContactById(const HeatmapFrame& frame, int id) {
    for (const auto& c : frame.touch.output.contacts) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

const TouchContact* FindVisibleById(const HeatmapFrame& frame, int id) {
    for (const auto& c : frame.touch.output.contacts) {
        if (c.id == id && c.isReported) return &c;
    }
    return nullptr;
}

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// EdgeRejector 只写标记，真正丢弃发生在 tracker。这条测试锁住那段接线：判定正确但没人取用，
// 正是这个功能长期形同虚设的原因。
void TestEdgeRejectedContactNeverBecomesTrack() {
    PipelineHarness h;
    const auto f1 = h.RunFlagged({{1.0f, 20.0f}}, true);
    Require(VisibleContacts(f1).empty(), "edge-rejected contact should not be reported");
    Require(f1.touch.output.contacts.empty(), "edge-rejected contact should not create a track");

    // 拒绝不是黏性的：同一位置不再带标记时，正常走触摸落下。
    const auto f2 = h.RunFlagged({{1.0f, 20.0f}}, false);
    const auto v2 = VisibleContacts(f2);
    Require(v2.size() == 1 && v2[0]->reportEvent == TouchReportDown, "unflagged contact should report Down");
}

// 已建立的轨迹不看这个标记，否则贴边滑动会被中途掐断成一串 up/down。
void TestEdgeRejectedFlagDoesNotKillEstablishedTrack() {
    PipelineHarness h;
    const int id = VisibleContacts(h.Run({{10.0f, 20.0f}}))[0]->id;
    const auto f2 = h.RunFlagged({{11.0f, 20.0f}}, true);
    const auto* moved = FindVisibleById(f2, id);
    Require(moved != nullptr, "established track should survive a later edge rejection");
    Require(moved->reportEvent == TouchReportMove, "established track should continue as Move");
}

// PressCandidate 阶段输出钉在 anchor 上，越过 m_dragThreshold 才开始跟随。断言的是「越界那一帧
// 输出的位移不超过手指的位移」——不复述补偿公式，换个补偿方式仍应通过，而直接跳到指尖真实位置
// 必然失败。按默认阈值那一跳约合屏幕上三十多个像素。
// 越界之后的契约见函数尾部：输出逐帧追上指尖，而不是永久落后一个阈值。
void TestDragStartDoesNotJumpOutput() {
    PipelineHarness h;
    h.filter.m_enabled = false;  // 1-Euro 会改写坐标，这里要比对的是状态机自己的输出

    const auto f1 = h.Run({{10.0f, 10.0f}});
    const auto v1 = VisibleContacts(f1);
    Require(v1.size() == 1, "frame1 should report one contact");
    const float anchorX = v1[0]->x;

    // 阈值内：输出应当纹丝不动。
    const auto f2 = h.Run({{10.5f, 10.0f}});
    const auto v2 = VisibleContacts(f2);
    Require(v2.size() == 1 && v2[0]->x == anchorX, "sub-threshold move should not move the output");

    // 越过阈值的那一帧：手指走了 0.5，输出不该走得更多。
    const auto f3 = h.Run({{11.0f, 10.0f}});
    const auto v3 = VisibleContacts(f3);
    Require(v3.size() == 1, "frame3 should still report one contact");
    const float outputStep = v3[0]->x - anchorX;
    Require(outputStep > 0.0f, "crossing the threshold should start moving the output");
    Require(outputStep <= 0.5f + 1e-4f, "output must not outrun the finger when dragging starts");

    // 补偿不是只补一帧，也不是永久保留：后续每帧输出追上一点点，追赶量不超过
    // 阈值/消解帧数。永久保留会让指针够不到起手方向那一侧的屏幕边缘，见
    // SolversUnit_TouchDragReach。
    const float catchUpPerFrame = h.gesture.m_dragThreshold /
                                  static_cast<float>(h.gesture.m_dragOffsetDecayFrames);
    const float beforeX = v3[0]->x;
    const auto f4 = h.Run({{12.0f, 10.0f}});
    const auto v4 = VisibleContacts(f4);
    Require(v4.size() == 1, "frame4 should still report one contact");
    const float steadyStep = v4[0]->x - beforeX;
    Require(steadyStep >= 1.0f - 1e-4f, "an established drag must not fall further behind the finger");
    Require(steadyStep <= 1.0f + catchUpPerFrame + 1e-4f,
            "catch-up must not exceed one decay step per frame");
}

void TestSingleFingerSilentGapRelink() {
    PipelineHarness h;
    const auto f1 = h.Run({{10.0f, 10.0f}});
    const auto v1 = VisibleContacts(f1);
    Require(v1.size() == 1 && v1[0]->reportEvent == TouchReportDown, "frame1 should report one Down");
    const int id = v1[0]->id;

    const auto f2 = h.Run({{12.0f, 10.0f}});
    const auto v2 = VisibleContacts(f2);
    Require(v2.size() == 1 && v2[0]->id == id && v2[0]->reportEvent == TouchReportMove, "frame2 should keep same id as Move");

    // 空档帧照常上报，而且位置停在最后一个真实位置。不报的话主机看到的是接触点消失
    // 又出现，即一次断开——重连窗口存在的意义正是不让短暂的信号丢失变成一次抬起。
    const auto f3 = h.Run({});
    const auto* hidden = FindContactById(f3, id);
    Require(hidden != nullptr, "silent gap contact should stay in frame");
    Require((hidden->lifeFlags & TouchLifeSilentGap) != 0, "gap frame should be marked as silent gap");
    Require(hidden->isReported, "a track inside the relink window must stay reported");
    // 手指从 10 走到 12。位置保持的话空档帧仍在 12 附近；按速度外推会走到 14 上下。
    // 界限放在 13 是因为这里读到的是手势层改写过的坐标，带着拖动偏移的消解量。
    Require(hidden->x < 13.0f, "gap position should hold, not extrapolate past the finger");

    const auto f4 = h.Run({{14.0f, 10.0f}});
    const auto* resumed = FindVisibleById(f4, id);
    Require(resumed != nullptr, "relinked finger should recover original id");
    Require(resumed->reportEvent == TouchReportMove, "relinked finger should resume as Move");
}

void TestFastSingleFingerGapRelinkUsesPrediction() {
    PipelineHarness h;
    const int id = VisibleContacts(h.Run({{10.0f, 10.0f}}))[0]->id;
    const auto f2 = h.Run({{16.0f, 10.0f}});
    const auto v2 = VisibleContacts(f2);
    Require(v2.size() == 1 && v2[0]->id == id, "fast frame2 should keep original id to seed prediction");

    const auto gap = h.Run({});
    Require(FindVisibleById(gap, id) != nullptr, "fast gap frame should keep reporting the track");
    Require(FindContactById(gap, id) != nullptr, "fast gap should keep hidden contact");

    const auto resume = h.Run({{28.0f, 10.0f}});
    const auto* relinked = FindVisibleById(resume, id);
    Require(relinked != nullptr, "fast relink should keep original id after one missing frame");
    Require(relinked->reportEvent == TouchReportMove, "fast relink should resume as Move");
}

void TestFastSingleFingerTwoGapRelinkUsesPrediction() {
    PipelineHarness h;
    const int id = VisibleContacts(h.Run({{10.0f, 10.0f}}))[0]->id;
    const auto f2 = h.Run({{16.0f, 10.0f}});
    const auto v2 = VisibleContacts(f2);
    Require(v2.size() == 1 && v2[0]->id == id, "fast two-gap frame2 should keep original id to seed prediction");

    const auto gap1 = h.Run({});
    const auto gap2 = h.Run({});
    Require(FindContactById(gap1, id) != nullptr, "first fast gap should keep hidden contact");
    Require(FindContactById(gap2, id) != nullptr, "second fast gap should keep hidden contact");

    const auto resume = h.Run({{34.0f, 10.0f}});
    const auto* relinked = FindVisibleById(resume, id);
    Require(relinked != nullptr, "fast relink should keep original id after two missing frames");
    Require(relinked->reportEvent == TouchReportMove, "two-gap relink should resume as Move");
}

void TestSingleFingerGapTimeout() {
    // Use a two-frame window here to make the timeout boundary explicit and keep this test short.
    // Other relink tests use the production default four-frame window.
    PipelineHarness h(2);
    const int id = VisibleContacts(h.Run({{10.0f, 10.0f}}))[0]->id;
    h.Run({{12.0f, 10.0f}});

    const auto gap1 = h.Run({});
    Require(FindContactById(gap1, id) != nullptr, "first gap frame should keep hidden contact");
    const auto gap2 = h.Run({});
    Require(FindContactById(gap2, id) != nullptr, "second gap frame should keep hidden contact");

    const auto timeout = h.Run({});
    const auto* up = FindVisibleById(timeout, id);
    Require(up != nullptr && up->reportEvent == TouchReportUp, "timeout frame should emit Up");
    const auto* stale = FindContactById(timeout, id);
    Require(stale != nullptr && (stale->lifeFlags & TouchLifeSilentGap) == 0, "timeout frame should no longer expose silent gap state");
}

void TestTwoFingerRelinkKeepsOtherFinger() {
    PipelineHarness h;
    const auto first = h.Run({{10.0f, 10.0f}, {30.0f, 10.0f}});
    const auto vis1 = VisibleContacts(first);
    Require(vis1.size() == 2, "frame1 should report two fingers");
    const int idA = vis1[0]->id;
    const int idB = vis1[1]->id;

    h.Run({{12.0f, 10.0f}, {28.0f, 10.0f}});

    const auto gap = h.Run({{26.0f, 10.0f}});
    Require(FindVisibleById(gap, idB) != nullptr, "remaining finger should keep visible id");
    const auto* hiddenA = FindContactById(gap, idA);
    // 空档期照常上报，主机才看不到断开。所以这里断言的是「进入了空档状态」，
    // 不是「被藏起来了」——后者正是重连窗口失效的表现。
    Require(hiddenA != nullptr && (hiddenA->lifeFlags & TouchLifeSilentGap) != 0,
            "missing finger should enter silent gap");
    Require(hiddenA->isReported, "a track inside the relink window must stay reported");

    const auto resume = h.Run({{14.0f, 10.0f}, {24.0f, 10.0f}});
    Require(FindVisibleById(resume, idA) != nullptr, "recovered finger should relink to original id");
    Require(FindVisibleById(resume, idB) != nullptr, "other finger should keep original id");
}

void TestAmbiguousSilentGapDoesNotHijackOldIds() {
    PipelineHarness h;
    const auto first = h.Run({{10.0f, 10.0f}, {20.0f, 10.0f}});
    const auto vis1 = VisibleContacts(first);
    Require(vis1.size() == 2, "frame1 should report two fingers");
    const int oldA = vis1[0]->id;
    const int oldB = vis1[1]->id;

    h.Run({{12.0f, 10.0f}, {18.0f, 10.0f}});
    const auto bothGap = h.Run({});
    Require(FindContactById(bothGap, oldA) != nullptr && FindContactById(bothGap, oldB) != nullptr, "both fingers should enter silent gap");

    const auto ambiguous = h.Run({{15.0f, 10.0f}, {15.0f, 10.0f}});
    const auto visible = VisibleContacts(ambiguous);
    size_t fresh = 0;
    for (const auto* c : visible) {
        if (c->id != oldA && c->id != oldB) ++fresh;
    }
    // 这个用例守的是「歧义时不要把旧 id 安到新接触点头上」，不是「旧轨迹被藏起来」。
    // 旧轨迹仍在重连窗口内，按新规则照常上报;它们与两个新接触点同时出现在输出里，
    // 这是重连窗口的已知代价，记在 docs/touch_stack.md。
    Require(fresh == 2, "ambiguous frame should output two contacts carrying new ids");
    Require(FindContactById(ambiguous, oldA) != nullptr, "oldA should still be alive in its relink window");
    Require(FindContactById(ambiguous, oldB) != nullptr, "oldB should still be alive in its relink window");
}

TouchContact MakeGestureContact(int id, float x, float y, int state, bool reported) {
    TouchContact c;
    c.id = id;
    c.x = x;
    c.y = y;
    c.state = state;
    c.areaCells = 12;
    c.signalSum = 1200;
    c.sizeMm = 2.0f;
    c.isReported = reported;
    c.lifeFlags = TouchLifeMapped;
    return c;
}

void TestHiddenNonSilentContactDoesNotBecomeVisibleInGesture() {
    TouchGestureStateMachine gesture;

    HeatmapFrame down;
    down.touch.output.contacts.push_back(MakeGestureContact(1, 10.0f, 10.0f, TouchStateDown, true));
    gesture.Process(down);
    Require(VisibleContacts(down).size() == 1 && down.touch.output.contacts[0].reportEvent == TouchReportDown,
            "visible contact should enter gesture as Down");

    HeatmapFrame move;
    move.touch.output.contacts.push_back(MakeGestureContact(1, 12.0f, 10.0f, TouchStateMove, true));
    gesture.Process(move);
    Require(VisibleContacts(move).size() == 1 && move.touch.output.contacts[0].reportEvent == TouchReportMove,
            "visible moved contact should enter dragging as Move");

    HeatmapFrame hidden;
    hidden.touch.output.contacts.push_back(MakeGestureContact(1, 13.0f, 10.0f, TouchStateMove, false));
    gesture.Process(hidden);
    Require(!hidden.touch.output.contacts[0].isReported && hidden.touch.output.contacts[0].reportEvent == TouchReportIdle,
            "non-silent hidden contact should not be resurrected by gesture output rewrite");
    const auto* up = FindVisibleById(hidden, 1);
    Require(up != nullptr && up->reportEvent == TouchReportUp,
            "previously visible contact should be released when the replacement contact is hidden");
}

void TestTrackerClearLiveStateKeepsIdSeed() {
    TouchTracker tracker;

    HeatmapFrame first;
    first.touch.output.contacts.push_back(MakeGestureContact(0, 10.0f, 10.0f, TouchStateDown, true));
    tracker.Process(first);
    Require(tracker.HasLiveTracks(), "tracker should have live tracks after a contact");
    Require(first.touch.output.contacts.size() == 1 && first.touch.output.contacts[0].id == 1,
            "first allocated tracker id should be 1");

    tracker.ClearLiveState();
    Require(!tracker.HasLiveTracks(), "tracker clear should remove live tracks");

    HeatmapFrame second;
    second.touch.output.contacts.push_back(MakeGestureContact(0, 20.0f, 20.0f, TouchStateDown, true));
    tracker.Process(second);
    Require(second.touch.output.contacts.size() == 1 && second.touch.output.contacts[0].id == 2,
            "tracker clear should preserve next id seed");
}

void TestGestureClearLiveState() {
    TouchGestureStateMachine gesture;

    HeatmapFrame down;
    down.touch.output.contacts.push_back(MakeGestureContact(1, 10.0f, 10.0f, TouchStateDown, true));
    gesture.Process(down);
    Require(gesture.HasLiveState(), "gesture should have live state after a contact");

    gesture.ClearLiveState();
    Require(!gesture.HasLiveState(), "gesture clear should remove live slots");
}

void TestHiddenNewContactReportsDownWhenSuppressionEnds() {
    TouchGestureStateMachine gesture;

    HeatmapFrame hidden;
    hidden.touch.output.contacts.push_back(MakeGestureContact(1, 10.0f, 10.0f, TouchStateMove, false));
    gesture.Process(hidden);
    Require(VisibleContacts(hidden).empty(),
            "hidden new contact should not create a visible gesture slot");
    Require(hidden.touch.output.contacts[0].reportEvent == TouchReportIdle,
            "hidden new contact should remain idle");

    HeatmapFrame visible;
    visible.touch.output.contacts.push_back(MakeGestureContact(1, 10.1f, 10.0f, TouchStateMove, true));
    gesture.Process(visible);
    const auto visibleContacts = VisibleContacts(visible);
    Require(visibleContacts.size() == 1 && visibleContacts[0]->reportEvent == TouchReportDown,
            "first visible frame after suppression should report Down, not Move");
}

TouchContact MakeTrackerContact(float x, float y, uint8_t sourcePeakId) {
    TouchContact c;
    c.x = x;
    c.y = y;
    c.areaCells = 12;
    c.signalSum = 1200;
    c.sizeMm = 2.0f;
    c.sourcePeakId = sourcePeakId;
    c.sourcePeakAge = 10;
    return c;
}

HeatmapFrame RunTrackerFrame(TouchTracker& tracker, std::initializer_list<TouchContact> contacts) {
    HeatmapFrame frame;
    for (const auto& c : contacts) frame.touch.output.contacts.push_back(c);
    tracker.Process(frame);
    return frame;
}

void TestGestureRetainsPendingUpWhenOutputIsFull() {
    TouchGestureStateMachine gesture;

    HeatmapFrame down;
    down.touch.output.contacts.push_back(MakeGestureContact(1, 10.0f, 10.0f, TouchStateDown, true));
    Require(gesture.Process(down), "initial gesture down should process");

    HeatmapFrame full;
    for (size_t i = 0; i < Solvers::kMaxTouchContacts; ++i) {
        full.touch.output.contacts.push_back(
            MakeGestureContact(static_cast<int>(i) + 2, 20.0f + static_cast<float>(i), 20.0f, TouchStateMove, true));
    }
    Require(!gesture.Process(full), "gesture should report failure when Up cannot be appended");
    Require(FindContactById(full, 1) == nullptr, "full frame should not silently drop a visible Up contact");
    Require(gesture.HasLiveState(), "pending Up slot should be retained after append failure");

    HeatmapFrame retry;
    Require(gesture.Process(retry), "gesture should retry pending Up on a later frame");
    const auto* up = FindContactById(retry, 1);
    Require(up != nullptr && up->reportEvent == TouchReportUp,
            "pending Up should be emitted once output capacity is available");
}

void TestTouchPipelinePropagatesGestureCapacityFailure() {
    Solvers::TouchPipeline pipeline;

    HeatmapFrame down;
    down.touch.output.contacts.push_back(MakeGestureContact(1, 10.0f, 10.0f, TouchStateDown, true));
    Require(pipeline.ProcessGestureOutput(down), "pipeline gesture should process initial down frame");

    HeatmapFrame full;
    for (size_t i = 0; i < Solvers::kMaxTouchContacts; ++i) {
        full.touch.output.contacts.push_back(
            MakeGestureContact(static_cast<int>(i) + 2, 20.0f + static_cast<float>(i), 20.0f, TouchStateMove, true));
    }
    Require(!pipeline.ProcessGestureOutput(full), "pipeline should fail when gesture output is capacity-limited");
    Require(FindContactById(full, 1) == nullptr, "pipeline should not dispatch incomplete touch output");

    HeatmapFrame retry;
    Require(pipeline.ProcessGestureOutput(retry), "pipeline should recover on the next frame");
    const auto* up = FindContactById(retry, 1);
    Require(up != nullptr && up->reportEvent == TouchReportUp,
            "pipeline should flush the pending Up once output capacity is available");
}

void TestSourcePeakIdentityKeepsCrossingTracks() {
    TouchTracker tracker;
    const auto first = RunTrackerFrame(tracker, {
        MakeTrackerContact(10.0f, 10.0f, 1),
        MakeTrackerContact(20.0f, 10.0f, 2),
    });
    const int idA = first.touch.output.contacts[0].id;
    const int idB = first.touch.output.contacts[1].id;

    RunTrackerFrame(tracker, {
        MakeTrackerContact(12.0f, 10.0f, 1),
        MakeTrackerContact(18.0f, 10.0f, 2),
    });

    const auto crossed = RunTrackerFrame(tracker, {
        MakeTrackerContact(16.0f, 10.0f, 1),
        MakeTrackerContact(14.0f, 10.0f, 2),
    });
    const auto* peakA = FindContactById(crossed, idA);
    const auto* peakB = FindContactById(crossed, idB);
    Require(peakA != nullptr && peakA->sourcePeakId == 1,
            "source peak identity should keep first crossing track id");
    Require(peakB != nullptr && peakB->sourcePeakId == 2,
            "source peak identity should keep second crossing track id");
}

void TestTrackerKeepsLifecycleEventsBeyondReportCapacity() {
    TouchTracker tracker;
    tracker.m_maxTouchCount = 5;
    RunTrackerFrame(tracker, {
        MakeTrackerContact(5.0f, 20.0f, 1),
        MakeTrackerContact(10.0f, 20.0f, 2),
        MakeTrackerContact(15.0f, 20.0f, 3),
        MakeTrackerContact(20.0f, 20.0f, 4),
        MakeTrackerContact(25.0f, 20.0f, 5),
    });

    const auto mixed = RunTrackerFrame(tracker, {
        MakeTrackerContact(5.5f, 20.0f, 1),
        MakeTrackerContact(10.5f, 20.0f, 2),
        MakeTrackerContact(15.5f, 20.0f, 3),
        MakeTrackerContact(20.5f, 20.0f, 4),
        MakeTrackerContact(35.0f, 20.0f, 6),
    });
    Require(mixed.touch.output.contacts.size() > 5,
            "tracker should keep hidden lifecycle contact beyond live report capacity");
    bool hasSilentGap = false;
    for (const auto& c : mixed.touch.output.contacts) {
        hasSilentGap = hasSilentGap || ((c.lifeFlags & TouchLifeSilentGap) != 0);
    }
    Require(hasSilentGap, "tracker should output the missing old finger as silent gap");
}

} // namespace

int main() {
    try {
        TestEdgeRejectedContactNeverBecomesTrack();
        TestEdgeRejectedFlagDoesNotKillEstablishedTrack();
        TestDragStartDoesNotJumpOutput();
        TestSingleFingerSilentGapRelink();
        TestFastSingleFingerGapRelinkUsesPrediction();
        TestFastSingleFingerTwoGapRelinkUsesPrediction();
        TestSingleFingerGapTimeout();
        TestTwoFingerRelinkKeepsOtherFinger();
        TestAmbiguousSilentGapDoesNotHijackOldIds();
        TestHiddenNonSilentContactDoesNotBecomeVisibleInGesture();
        TestTrackerClearLiveStateKeepsIdSeed();
        TestGestureClearLiveState();
        TestHiddenNewContactReportsDownWhenSuppressionEnds();
        TestGestureRetainsPendingUpWhenOutputIsFull();
        TestTouchPipelinePropagatesGestureCapacityFailure();
        TestSourcePeakIdentityKeepsCrossingTracks();
        TestTrackerKeepsLifecycleEventsBeyondReportCapacity();
        std::cout << "[TEST] TouchTracker silent-gap relink tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
