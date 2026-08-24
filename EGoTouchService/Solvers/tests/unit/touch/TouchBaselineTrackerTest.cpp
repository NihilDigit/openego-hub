#include "TouchSolver/BaselineTracker.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

constexpr uint16_t kRawBaseline = 1000;
constexpr uint16_t kRawHighCell = 1400;
constexpr int kPeakRow = 20;
constexpr int kPeakCol = 20;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int16_t PeakValue(const Solvers::HeatmapFrame& frame) {
    return frame.touch.conditioned[kPeakRow][kPeakCol];
}

int16_t BackgroundValue(const Solvers::HeatmapFrame& frame) {
    return frame.touch.conditioned[0][0];
}

void FillRaw(Solvers::HeatmapFrame& frame, uint16_t value) {
    for (auto& row : frame.heatmapMatrix) {
        for (auto& cell : row) {
            cell = static_cast<int16_t>(value);
        }
    }
}

void FillRawWithHighCell(Solvers::HeatmapFrame& frame) {
    FillRaw(frame, kRawBaseline);
    frame.heatmapMatrix[kPeakRow][kPeakCol] = static_cast<int16_t>(kRawHighCell);
}

void PrimeBaseline(Solvers::Touch::BaselineTracker& tracker) {
    tracker.m_baseline = kRawBaseline;
    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;
    FillRaw(frame, kRawBaseline);
    tracker.Process(frame, false);
    Require(PeakValue(frame) == 0, "baseline prime frame should subtract to zero");
}

// ──── Initialization ────

void TestInitFromDefaultBaseline() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_baseline = 0x7FEE;
    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;
    FillRaw(frame, 0x7FEE);
    tracker.Process(frame, false);
    Require(PeakValue(frame) == 0, "uniform raw matching default baseline should output zero");
}

void TestDisabledPassesThrough() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_enabled = false;
    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;
    FillRaw(frame, 5000);
    tracker.Process(frame, false);
    Require(PeakValue(frame) == 5000, "disabled tracker should not modify output");
}

void TestInvalidMasterZeroOutput() {
    Solvers::Touch::BaselineTracker tracker;
    PrimeBaseline(tracker);
    Solvers::HeatmapFrame frame;
    frame.masterWasRead = false;
    frame.masterSuffixValid = false;
    FillRawWithHighCell(frame);
    tracker.Process(frame, false);
    Require(PeakValue(frame) == 0, "invalid master should produce zero output");
    Require(BackgroundValue(frame) == 0, "invalid master should produce zero output (bg)");
}

void TestResetDropsPreviousDynamicBaseline() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_baseline = kRawBaseline;
    PrimeBaseline(tracker);

    // Change the "hardware" baseline and reset
    tracker.m_baseline = 2000;
    tracker.Reset();

    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;
    FillRaw(frame, 2000);
    tracker.Process(frame, false);
    Require(PeakValue(frame) == 0, "reset should re-initialize from new BaselineValue");
}

// ──── NoFinger mode ────

void TestNoFingerUpdatesAllCells() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_noFingerAlphaShift = 0;
    tracker.m_noFingerMaxStep = 2000;
    PrimeBaseline(tracker);

    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;
    FillRaw(frame, kRawBaseline + 200);
    tracker.Process(frame, false);
    Require(PeakValue(frame) == 0, "no-finger mode always outputs zero");

    FillRaw(frame, kRawBaseline + 200);
    tracker.Process(frame, false);
    Require(PeakValue(frame) == 0, "no-finger mode always outputs zero after convergence");
}

void TestNoFingerDeadbandSkipsUpdate() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_noiseTrackingEnabled = false;
    PrimeBaseline(tracker);

    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;
    FillRaw(frame, kRawBaseline + 50); // within deadband (90)
    tracker.Process(frame, false);
    Require(PeakValue(frame) == 0, "deadband should skip update");
    // Baseline should be unchanged after deadband skip
    tracker.Reset();
    tracker.m_baseline = kRawBaseline;
    // Verify the no-track path produces zero output regardless
}

void TestNoFingerNoiseTrackingStillOutputsZero() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_noiseTrackingEnabled = true;
    PrimeBaseline(tracker);

    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;
    FillRaw(frame, kRawBaseline + 50);
    tracker.Process(frame, false);
    Require(PeakValue(frame) == 0, "no-finger with noise tracking still outputs zero");
}

// 固件说没手指、而本帧自己还有强信号时,不许把它吸收掉。
//
// 这条测试原先断言的正好相反(「无手指模式把高信号吸进基线」),那正是真机上
// 「手指停在角上就消失」的来源:无手指分支一帧能把基线推 512 个单位,一个 500 量级的
// 信号一帧就没了,而且不可逆。厂商的做法是拿自己算的帧极值判,硬件检测器只是否决票
// (SPEC_baseline §3.2)。
void TestOwnSignalOverridesTheFirmwareFlag() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_noFingerAlphaShift = 0;
    tracker.m_noFingerMaxStep = 2000;
    PrimeBaseline(tracker);

    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;
    FillRawWithHighCell(frame);
    tracker.Process(frame, false);
    Require(PeakValue(frame) > 0, "a strong own signal must survive the firmware saying no finger");

    // 把门槛关掉就回到旧行为,这条守住开关本身真的在起作用。
    Solvers::Touch::BaselineTracker trusting;
    trusting.m_noFingerAlphaShift = 0;
    trusting.m_noFingerMaxStep = 2000;
    trusting.m_noFingerMaxSignal = 0;
    PrimeBaseline(trusting);
    Solvers::HeatmapFrame frame2;
    frame2.masterWasRead = true;
    frame2.masterSuffixValid = true;
    FillRawWithHighCell(frame2);
    trusting.Process(frame2, false);
    Require(PeakValue(frame2) == 0, "with the override off the old absorbing behaviour returns");

    // 真被吸收掉的那一份不会自己回来:切到有手指模式,基线已经爬到那个高度了。
    FillRawWithHighCell(frame2);
    trusting.Process(frame2, true);
    Require(PeakValue(frame2) == 0, "an absorbed peak does not come back on its own");
}

// ──── Finger mode: Freeze ────

void TestFingerFreezePositivePeak() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_peakThreshold = 305;
    PrimeBaseline(tracker);

    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;
    FillRawWithHighCell(frame);
    tracker.Process(frame, true);
    Require(PeakValue(frame) >= tracker.m_peakThreshold,
            "cell above peak threshold should freeze and output signal");
}

void TestFingerFreezeMultiFrameStaysFrozen() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_peakThreshold = 305;
    PrimeBaseline(tracker);

    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;
    for (int i = 0; i < 4; ++i) {
        FillRawWithHighCell(frame);
        tracker.Process(frame, true);
    }
    Require(PeakValue(frame) >= tracker.m_peakThreshold,
            "frozen peak should persist across multiple frames");
}

void TestFingerBackgroundAbsorbsNoise() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_peakThreshold = 305;
    PrimeBaseline(tracker);

    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;
    FillRaw(frame, kRawBaseline); // uniform baseline = no peaks
    tracker.Process(frame, true);
    Require(BackgroundValue(frame) == 0,
            "uniform background cells should output zero");
}

void TestFingerFreezeUsesCommonModeCorrectedDiff() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_peakThreshold = 305;
    PrimeBaseline(tracker);

    // Global negative shift, but one cell is higher relative to the panel
    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;
    FillRaw(frame, kRawBaseline - 500);
    frame.heatmapMatrix[kPeakRow][kPeakCol] = static_cast<int16_t>(kRawBaseline - 150);
    tracker.Process(frame, true);

    Require(PeakValue(frame) >= tracker.m_peakThreshold,
            "common-mode-corrected positive touch should freeze even when raw delta is negative");
}

// ──── Release Hold ────

void TestReleaseHoldNegativeRebound() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_peakThreshold = 305;
    tracker.m_releaseHoldFrames = 5;
    PrimeBaseline(tracker);

    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;

    // First, create a freeze
    FillRawWithHighCell(frame);
    tracker.Process(frame, true);
    Require(PeakValue(frame) >= tracker.m_peakThreshold, "should freeze peak");

    // Now drop the cell below baseline — release hold should be active
    FillRaw(frame, kRawBaseline);
    frame.heatmapMatrix[kPeakRow][kPeakCol] = static_cast<int16_t>(kRawBaseline - 100);
    tracker.Process(frame, true);
    Require(PeakValue(frame) < -tracker.m_negativeDeadband,
            "release hold should pass negative rebound through");
}

void TestReleaseHoldExpiresAfterFrames() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_peakThreshold = 305;
    tracker.m_releaseHoldFrames = 3;
    PrimeBaseline(tracker);

    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;

    // Create freeze
    FillRawWithHighCell(frame);
    tracker.Process(frame, true);

    // Run release hold down — refill each iteration (Process zeroes output)
    for (int i = 0; i < 10; ++i) {
        FillRaw(frame, kRawBaseline);
        tracker.Process(frame, true);
    }
    // By now release hold should be expired and the cell should output zero
    Require(PeakValue(frame) == 0, "after release hold expires, cell should output zero");
}

// ──── Recovery Mode ────

void TestRecoveryOnFalseToTrueTransition() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_recoveryAlphaShift = 0;     // full update in one step
    tracker.m_recoveryMaxStep = 2000;
    tracker.m_peakThreshold = 305;
    PrimeBaseline(tracker);

    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;

    // NoFinger then Finger transition triggers recovery
    FillRaw(frame, kRawBaseline + 600); // large offset
    tracker.Process(frame, false);
    FillRaw(frame, kRawBaseline + 600); // refill: Process zeroes output in place
    tracker.Process(frame, true);
    // Recovery should have absorbed the 600-offset background
    Require(BackgroundValue(frame) == 0,
            "recovery mode should converge background fast on false→true transition");
}

void TestRecoveryExitsOnFreezeDetection() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_recoveryAlphaShift = 0;
    tracker.m_recoveryMaxStep = 2000;
    tracker.m_peakThreshold = 305;
    PrimeBaseline(tracker);

    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;

    // NoFinger → Finger with a peak
    // Each Process call receives a new raw frame (simulates real pipeline).
    FillRawWithHighCell(frame);
    tracker.Process(frame, false);
    FillRawWithHighCell(frame);   // refill: Process() zeroes output in place
    tracker.Process(frame, true);
    Require(PeakValue(frame) >= tracker.m_peakThreshold,
            "recovery should produce peak output on first freeze detection");
}

void TestRecoveryContinuousWhenNoFreeze() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_recoveryAlphaShift = 0;
    tracker.m_recoveryMaxStep = 2000;
    tracker.m_recoveryMaxFrames = 60;
    tracker.m_peakThreshold = 305;
    PrimeBaseline(tracker);

    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;

    // Finger present but no peaks — recovery should continue
    FillRaw(frame, kRawBaseline + 600);
    tracker.Process(frame, false); // establish prevHadFinger=false

    for (int i = 0; i < 3; ++i) {
        FillRaw(frame, kRawBaseline + 600);
        tracker.Process(frame, true);
        Require(BackgroundValue(frame) == 0,
                "recovery should continue while no freeze cells exist");
    }
}

// ──── Background Drift Tracking ────

void TestBackgroundPositiveDriftTracking() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_positiveAlphaShift = 0;
    tracker.m_positiveMaxStep = 200;
    tracker.m_positiveDeadband = 14;
    tracker.m_peakThreshold = 305;
    PrimeBaseline(tracker);

    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;

    // First frame with finger — enters recovery; skip it by priming finger state
    FillRawWithHighCell(frame);
    tracker.Process(frame, true); // has finger + peak → freeze
    Require(PeakValue(frame) >= tracker.m_peakThreshold, "should freeze");

    // Now background cells with positive drift but no peak
    FillRaw(frame, kRawBaseline + 100); // above positiveDeadband
    tracker.Process(frame, true);
    Require(BackgroundValue(frame) == 0,
            "positive drifted background should output zero while tracking baseline");
}

// ──── Common-Mode Rejection ────

void TestCommonModeRejectsGlobalShift() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_peakThreshold = 305;
    PrimeBaseline(tracker);

    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;

    // All cells +500 — should be absorbed by common-mode
    FillRaw(frame, kRawBaseline + 500);
    tracker.Process(frame, true);
    Require(BackgroundValue(frame) == 0,
            "uniform global shift should be removed by common-mode");
}

// ──── Baseline Inheritance ────

void TestBaselineInheritedAcrossHasFingerToggle() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_noFingerAlphaShift = 0;
    tracker.m_noFingerMaxStep = 2000;
    PrimeBaseline(tracker);

    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;

    // Converge baseline during no-finger — refill each iteration
    for (int i = 0; i < 3; ++i) {
        FillRaw(frame, kRawBaseline + 500);
        tracker.Process(frame, false);
    }

    // Toggle to finger — baseline should be inherited
    FillRaw(frame, kRawBaseline + 500);
    tracker.Process(frame, true);
    Require(BackgroundValue(frame) == 0,
            "inherited baseline should give zero output on uniform panel");
}

// ──── Edge cases ────

void TestStartupWithFingerAlreadyPresent() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_peakThreshold = 305;
    tracker.m_recoveryAlphaShift = 0;
    tracker.m_recoveryMaxStep = 2000;
    PrimeBaseline(tracker);

    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;

    // First finger frame with a peak — recovery+freeze should both work
    FillRawWithHighCell(frame);
    tracker.Process(frame, true);
    Require(PeakValue(frame) >= tracker.m_peakThreshold,
            "startup with finger present should detect peak");
}

void TestClampBaselineRange() {
    Solvers::Touch::BaselineTracker tracker;
    tracker.m_baseline = 65535;
    tracker.Reset();

    Solvers::HeatmapFrame frame;
    frame.masterWasRead = true;
    frame.masterSuffixValid = true;
    FillRaw(frame, 65535);
    tracker.Process(frame, false);
    Require(PeakValue(frame) == 0, "baseline at max value should still work");
}

} // namespace

// ──── 贴边轨迹附近的维持门槛 ────

// 守的是「手指停在角上不会被基线吃掉」。信号强度取在进入门槛与维持门槛之间:
// 没有轨迹时该格不冻结、被背景更新吸收成 0;上一帧有贴边轨迹时该格冻结、信号留住。
// 这两半必须一起断言——只断言前者会让「门槛恒定放宽」也通过,只断言后者会让
// 「门槛恒定收紧」也通过。
void TestSustainThresholdNeedsAnEdgeTrack() {
    constexpr int kEdgeRow = 39;
    constexpr int kEdgeCol = 59;
    constexpr uint16_t kWeak = kRawBaseline + 200;   // 介于 100 与 305 之间

    auto run = [&](bool withAnchor) {
        Solvers::Touch::BaselineTracker tracker;
        PrimeBaseline(tracker);

        Solvers::Touch::TrackAnchor anchor{static_cast<float>(kEdgeCol) + 0.5f,
                                           static_cast<float>(kEdgeRow) + 0.5f};
        int16_t last = 0;
        for (int i = 0; i < 12; ++i) {
            Solvers::HeatmapFrame frame;
            frame.masterWasRead = true;
            frame.masterSuffixValid = true;
            FillRaw(frame, kRawBaseline);
            frame.heatmapMatrix[kEdgeRow][kEdgeCol] = static_cast<int16_t>(kWeak);
            if (withAnchor) {
                frame.touch.runtime.prevTrackAnchors = {&anchor, 1};
            }
            tracker.Process(frame, true);
            last = frame.touch.conditioned[kEdgeRow][kEdgeCol];
        }
        return last;
    };

    Require(run(true) > 100,
            "a weak contact under an edge-hugging track must survive the baseline");
    Require(run(false) == 0,
            "the same weak signal with no track must still be absorbed");
}

// 屏幕中间的轨迹不该享受维持门槛:掌落在中间,放宽它只会让掌上报更多。
void TestSustainThresholdIgnoresInteriorTracks() {
    constexpr uint16_t kWeak = kRawBaseline + 200;

    Solvers::Touch::BaselineTracker tracker;
    PrimeBaseline(tracker);

    Solvers::Touch::TrackAnchor anchor{static_cast<float>(kPeakCol) + 0.5f,
                                       static_cast<float>(kPeakRow) + 0.5f};
    int16_t last = 0;
    for (int i = 0; i < 12; ++i) {
        Solvers::HeatmapFrame frame;
        frame.masterWasRead = true;
        frame.masterSuffixValid = true;
        FillRaw(frame, kRawBaseline);
        frame.heatmapMatrix[kPeakRow][kPeakCol] = static_cast<int16_t>(kWeak);
        frame.touch.runtime.prevTrackAnchors = {&anchor, 1};
        tracker.Process(frame, true);
        last = PeakValue(frame);
    }
    Require(last == 0, "an interior track must not lower the freeze threshold");
}

int main() {
    try {
        TestSustainThresholdNeedsAnEdgeTrack();
        TestSustainThresholdIgnoresInteriorTracks();
        TestInitFromDefaultBaseline();
        TestDisabledPassesThrough();
        TestInvalidMasterZeroOutput();
        TestResetDropsPreviousDynamicBaseline();
        TestNoFingerUpdatesAllCells();
        TestNoFingerDeadbandSkipsUpdate();
        TestNoFingerNoiseTrackingStillOutputsZero();
        TestOwnSignalOverridesTheFirmwareFlag();
        TestFingerFreezePositivePeak();
        TestFingerFreezeMultiFrameStaysFrozen();
        TestFingerBackgroundAbsorbsNoise();
        TestFingerFreezeUsesCommonModeCorrectedDiff();
        TestReleaseHoldNegativeRebound();
        TestReleaseHoldExpiresAfterFrames();
        TestRecoveryOnFalseToTrueTransition();
        TestRecoveryExitsOnFreezeDetection();
        TestRecoveryContinuousWhenNoFreeze();
        TestBackgroundPositiveDriftTracking();
        TestCommonModeRejectsGlobalShift();
        TestBaselineInheritedAcrossHasFingerToggle();
        TestStartupWithFingerAlreadyPresent();
        TestClampBaselineRange();
        std::cout << "[TEST] Touch baseline tracker tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
