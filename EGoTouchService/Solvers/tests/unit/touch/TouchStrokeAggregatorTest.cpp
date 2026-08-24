// 第 5 级(笔画)的接续与归属。这一级存在的全部理由是跨过跟踪级的碎裂:
// 一条真实的按压在跟踪级会碎成十几条短轨迹,每条各自积累、各自判定。
// 这里锁住的正是「轨迹会断,笔画不该断」,以及它的反面——不能什么都接上。

#include "TouchSolver/CoordinateFilter.hpp"
#include "TouchSolver/StrokeAggregator.hpp"
#include "TouchSolver/TouchGestureStateMachine.hpp"
#include "TouchSolver/TouchTracker.hpp"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {

using Solvers::HeatmapFrame;
using Solvers::TouchContact;
using Solvers::TouchReportUp;
using Solvers::Touch::CoordinateFilter;
using Solvers::Touch::StrokeAggregator;
using Solvers::Touch::TouchGestureStateMachine;
using Solvers::Touch::TouchTracker;

constexpr uint64_t kFrameIntervalUs = 8333;

// 与 TouchPipeline::ProcessTrackingAndGesture 同序:跟踪 → 滤波 → 笔画 → 手势 → 补盖。
struct Harness {
    TouchTracker tracker;
    CoordinateFilter filter;
    StrokeAggregator strokes;
    TouchGestureStateMachine gesture;
    uint64_t timestamp = 0;

    HeatmapFrame Step(std::initializer_list<std::pair<float, float>> points) {
        return StepWithPeak(points, 2000);
    }

    HeatmapFrame StepWithPeak(std::initializer_list<std::pair<float, float>> points,
                              int16_t peakSignal) {
        HeatmapFrame frame;
        timestamp += kFrameIntervalUs;
        frame.timestamp = timestamp;
        for (const auto& [x, y] : points) {
            TouchContact c;
            c.x = x;
            c.y = y;
            c.areaCells = 12;
            c.signalSum = 1200;
            c.sizeMm = 8.0f;
            c.peakSignal = peakSignal;
            frame.touch.output.contacts.push_back(c);
        }
        tracker.Process(frame);
        filter.Process(frame);
        strokes.Process(frame);
        gesture.Process(frame);
        strokes.StampLateContacts(frame);
        return frame;
    }

    HeatmapFrame StepEmpty(int frames) {
        HeatmapFrame frame;
        for (int i = 0; i < frames; ++i) frame = Step({});
        return frame;
    }
};

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int StrokeIdAt(const HeatmapFrame& frame, float x) {
    for (const auto& c : frame.touch.output.contacts) {
        if (c.x > x - 2.0f && c.x < x + 2.0f) return c.strokeId;
    }
    return 0;
}

// 轨迹被判终结、很短时间内在很近的位置又冒出一条新轨迹——同一条笔画。
void TestNewTrackNearbyContinuesStroke() {
    Harness h;
    const auto f1 = h.Step({{10.0f, 20.0f}});
    const int first = StrokeIdAt(f1, 10.0f);
    Require(first != 0, "first contact should get a stroke id");

    h.StepEmpty(5);  // 超过重连窗口(4 帧),跟踪级会另起一条轨迹
    const auto f2 = h.Step({{10.5f, 20.0f}});
    Require(StrokeIdAt(f2, 10.5f) == first, "a nearby track right after should continue the same stroke");
}

// 反面:同样的时间间隔,位置远,是两次按压。判据不能只看时间。
void TestNewTrackFarAwayStartsNewStroke() {
    Harness h;
    const auto f1 = h.Step({{10.0f, 20.0f}});
    const int first = StrokeIdAt(f1, 10.0f);

    h.StepEmpty(5);
    const auto f2 = h.Step({{30.0f, 20.0f}});
    const int second = StrokeIdAt(f2, 30.0f);
    Require(second != 0 && second != first, "a distant track should start a new stroke");
}

// 手指抬起后重新按下——两条笔画,哪怕位置一模一样。
void TestSamePlaceAfterLongGapStartsNewStroke() {
    Harness h;
    const auto f1 = h.Step({{10.0f, 20.0f}});
    const int first = StrokeIdAt(f1, 10.0f);

    h.StepEmpty(30);  // 250 ms,远超接续窗口
    const auto f2 = h.Step({{10.0f, 20.0f}});
    const int second = StrokeIdAt(f2, 10.0f);
    Require(second != 0 && second != first, "the same place after a long gap is a second stroke");
}

// 抬起事件是手势层在轨迹消失之后另建的接触点,不经过笔画层的主循环。漏掉它的话
// 每条笔画的最后一帧都会落在「没有归属」里,统计和 5.3 的终结判定都会错。
void TestSynthesizedUpEventCarriesStrokeId() {
    Harness h;
    int expected = 0;
    for (int i = 0; i < 6; ++i) {
        const auto f = h.Step({{10.0f, 20.0f}});
        expected = StrokeIdAt(f, 10.0f);
    }
    Require(expected != 0, "held contact should have a stroke id");

    for (int i = 0; i < 8; ++i) {
        const auto f = h.Step({});
        for (const auto& c : f.touch.output.contacts) {
            if (c.reportEvent != TouchReportUp) continue;
            Require(c.strokeId == expected, "the synthesized up event should carry the stroke id");
            return;
        }
    }
    throw std::runtime_error("no up event was emitted");
}

// 轨迹号抬起即回收,所以隔了很久回来的同一个号可能是另一根手指。这条锁住的是:
// 判「是不是同一条笔画」只能看时间与位置,不能看「上一帧还活着」——空闲帧不进管线,
// 笔画层的帧计数在那段时间根本不走,隔一秒的两次按压在它看来就是相邻两帧。
void TestReusedTrackIdAfterIdleStartsNewStroke() {
    // 直接驱动笔画层:跟踪级的 ClearLiveState 会保留 id 种子,从它那里拿不到复用的
    // 号,而这里要测的恰恰是「号相同」这一种输入。
    StrokeAggregator strokes;
    uint64_t timestamp = 0;

    auto feed = [&](int trackId) {
        HeatmapFrame frame;
        timestamp += kFrameIntervalUs;
        frame.timestamp = timestamp;
        TouchContact c;
        c.id = trackId;
        c.x = c.matchXCells = 10.0f;
        c.y = c.matchYCells = 20.0f;
        c.sizeMm = 8.0f;
        frame.touch.output.contacts.push_back(c);
        strokes.Process(frame);
        return frame.touch.output.contacts[0].strokeId;
    };

    const int first = feed(3);
    timestamp += kFrameIntervalUs * 120;  // 1 秒,管线在这段时间里不跑
    Require(feed(3) != first, "a reused track id after an idle second is a new stroke");
}

// hold 的两条:期间一个事件都不发,放行之后按下落在**起手位置**而不是判定那一帧。
// 后者是这条测试真正守的东西——弱接触点常常一边变强一边移动,拿判定那一帧的位置
// 发按下,起手点会偏出去。
void TestHeldStrokeReportsDownAtTheFirstPosition() {
    Harness h;
    h.strokes.m_holdMinPeakSignal = 1000;
    h.strokes.m_decideMaxSamples = 30;

    for (int i = 0; i < 3; ++i) {
        const auto f = h.StepWithPeak({{10.0f, 20.0f}}, 400);
        for (const auto& c : f.touch.output.contacts) {
            Require(!c.isReported, "a held stroke must not be reported");
        }
    }
    // 变强的同时挪了半格。按下应当落在 10.0,不是 10.5。
    const auto f = h.StepWithPeak({{10.5f, 20.0f}}, 1200);
    const TouchContact* down = nullptr;
    for (const auto& c : f.touch.output.contacts) {
        if (c.isReported) down = &c;
    }
    Require(down != nullptr, "the stroke should be released once the evidence arrives");
    Require(down->reportEvent == Solvers::TouchReportDown, "release should report Down, not Move");
    Require(down->x > 9.9f && down->x < 10.1f, "Down should land on the first position");
}

// 弱而孤立的接触点不是掌。实测按信号门槛单独判会把双指手势里较轻的那根压掉 47 帧,
// 而掌语料上弱误报身边中位有 10 条笔画同时存在,双指手势那根只有 2 条。
void TestLoneWeakStrokeIsReleasedNotCancelled() {
    Harness h;
    h.strokes.m_holdMinPeakSignal = 1000;
    h.strokes.m_decideMaxSamples = 3;
    h.strokes.m_palmMinConcurrentStrokes = 4;

    for (int i = 0; i < 8; ++i) h.StepWithPeak({{10.0f, 20.0f}}, 400);
    const auto f = h.StepWithPeak({{10.0f, 20.0f}}, 400);
    bool reported = false;
    for (const auto& c : f.touch.output.contacts) reported = reported || c.isReported;
    Require(reported, "a lone weak contact should be released once it has waited");
}

// 判为掌且从没发过按下:主机什么都不该看到,连抬起都不该有。
void TestCancelledStrokeNeverSeenByTheHostEmitsNothing() {
    Harness h;
    h.strokes.m_holdMinPeakSignal = 1000;
    h.strokes.m_decideMaxSamples = 3;
    h.strokes.m_palmMinConcurrentStrokes = 4;

    // 五个弱接触点同时进来,这才是掌的形状。
    for (int i = 0; i < 10; ++i) {
        const auto f = h.StepWithPeak(
            {{10.0f, 20.0f}, {13.0f, 20.0f}, {16.0f, 20.0f}, {19.0f, 20.0f}, {22.0f, 20.0f}},
            400);
        for (const auto& c : f.touch.output.contacts) {
            Require(!c.isReported, "a cancelled stroke must never be reported");
        }
    }
    for (int i = 0; i < 5; ++i) {
        const auto f = h.Step({});
        for (const auto& c : f.touch.output.contacts) {
            // 未上报的接触点身上留着跟踪级写的事件码,主机看不到它们;这里只管
            // 真正发出去的那些。
            Require(!(c.isReported && c.reportEvent == Solvers::TouchReportUp),
                    "no up may follow a stroke the host never saw");
        }
    }
}

// 判掌之后按重一点,就该回来。真机上出现过的:手掌把拥挤度抬起来,旁边真手指的轻点
// 被判成掌,而判定粘着,于是这根手指整个接触期间一直不响应。
void TestCancelledStrokeRecoversWhenItPressesHarder() {
    Harness h;
    h.strokes.m_holdMinPeakSignal = 1000;
    h.strokes.m_decideMaxSamples = 3;
    h.strokes.m_palmMinConcurrentStrokes = 4;

    for (int i = 0; i < 8; ++i) {
        h.StepWithPeak({{10.0f, 20.0f}, {13.0f, 20.0f}, {16.0f, 20.0f},
                        {19.0f, 20.0f}, {22.0f, 20.0f}}, 400);
    }
    // 按重之后(Step 的默认峰值 2000 高于门槛),被判掌的笔画该翻回有效。
    HeatmapFrame f;
    for (int i = 0; i < 3; ++i) {
        f = h.Step({{10.0f, 20.0f}, {13.0f, 20.0f}, {16.0f, 20.0f},
                    {19.0f, 20.0f}, {22.0f, 20.0f}});
    }
    bool revived = false;
    for (const auto& c : f.touch.output.contacts) {
        if (c.isReported && c.x > 9.0f && c.x < 11.0f) revived = true;
    }
    Require(revived, "a stroke called palm must come back once it presses hard enough");
}

}  // namespace

int main() {
    try {
        TestNewTrackNearbyContinuesStroke();
        TestNewTrackFarAwayStartsNewStroke();
        TestSamePlaceAfterLongGapStartsNewStroke();
        TestSynthesizedUpEventCarriesStrokeId();
        TestReusedTrackIdAfterIdleStartsNewStroke();
        TestHeldStrokeReportsDownAtTheFirstPosition();
        TestLoneWeakStrokeIsReleasedNotCancelled();
        TestCancelledStrokeNeverSeenByTheHostEmitsNothing();
        TestCancelledStrokeRecoversWhenItPressesHarder();
        std::cout << "[TEST] StrokeAggregator continuation tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
