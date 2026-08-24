// Covers stage 2/3 of pen/touch arbitration: TouchTracker consuming the arbiter's
// screen-wide verdict, rather than the tip-radius AFT alone.
//
// Behaviours that matter and are easy to regress:
//   * Suppression must survive the linger tail. When the pen lifts, the stylus point
//     goes invalid, so every tip-distance test fails exactly when the palm is still
//     resting on the panel. Only the arbiter's carried flag covers that window.
//   * A gesture already in progress when the pen arrives must not be hijacked. That is
//     what the track-age test protects; without it, hovering a pen would kill an
//     in-flight scroll. The flip side is that the episode clock must not restart on a
//     dropped frame, or every live track inherits that exemption for good.
//   * Neither path may key on sizeMm, which is fitted from signalSum and therefore says
//     "palm" about a fingertip.
//
// The last group also covers the local tip-radius path (ShouldStylusAftSuppress), which
// needs a latched tip position rather than the arbiter flag.

#include "TouchSolver/TouchTracker.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using Solvers::HeatmapFrame;
using Solvers::TouchContact;
using Solvers::Touch::TouchTracker;

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

TouchContact MakeContact(float x, float y, int areaCells, int signalSum, float sizeMm) {
    TouchContact c{};
    c.x = x;
    c.y = y;
    c.areaCells = areaCells;
    c.signalSum = signalSum;
    c.sizeMm = sizeMm;
    return c;
}

// A stylus frame strong enough for ResolveStylusAftContext to latch a tip position.
// The bar is: point valid and in range, interop.recheckPassed (true by default), and a
// peak at least half the recheck threshold with either pressure or a dominant TX2 line.
struct StylusSpec {
    bool inRange = false;
    float x = 0.0f;
    float y = 0.0f;
    uint16_t pressure = 0;
    uint16_t signalX = 0;
    uint16_t signalY = 0;
    uint16_t recheckThreshold = 0;
};

StylusSpec WritingAt(float x, float y) {
    StylusSpec s;
    s.inRange = true;
    s.x = x;
    s.y = y;
    s.pressure = 180;
    s.recheckThreshold = 400;
    s.signalX = 300;
    s.signalY = 900; // >= recheckThreshold, so tx2Strong holds
    return s;
}

// A palm-sized contact: matches the signal density measured on real recordings
// (areaCells 31 / signalSum 18671).
TouchContact PalmContact(float x = 30.0f, float y = 20.0f) {
    return MakeContact(x, y, 31, 18671, 3.0f);
}

// A fingertip: below every palm threshold.
TouchContact FingerContact(float x = 10.0f, float y = 10.0f) {
    return MakeContact(x, y, 8, 3000, 1.1f);
}

// Drive one frame and report whether the tracked contact was published.
// penActive mirrors what StylusTouchArbiter emitted and StylusTouchSuppressor carried
// through interop; stylus is the tip signal the local AFT path needs, absent by default
// so that the pen-mode tests exercise the screen-wide route alone.
bool StepAndCheckReported(TouchTracker& tracker,
                          bool penActive,
                          const TouchContact& c,
                          const StylusSpec& stylus = StylusSpec{}) {
    HeatmapFrame frame{};
    frame.stylus.interop.touchSuppressActive = penActive;
    frame.stylus.interop.recheckThreshold = stylus.recheckThreshold;
    frame.stylus.interop.signalX = stylus.signalX;
    frame.stylus.interop.signalY = stylus.signalY;
    frame.stylus.interop.maxRawPeak = std::max(stylus.signalX, stylus.signalY);
    frame.stylus.output.valid = stylus.inRange;
    frame.stylus.output.inRange = stylus.inRange;
    frame.stylus.output.tipDown = stylus.pressure > 0;
    frame.stylus.output.pressure = stylus.pressure;
    frame.stylus.output.point.valid = stylus.inRange;
    frame.stylus.output.point.x = stylus.x * 1024.0f;
    frame.stylus.output.point.y = stylus.y * 1024.0f;
    frame.touch.output.contacts.assign(&c, &c + 1);
    tracker.Process(frame);
    for (const auto& out : frame.touch.output.contacts) {
        if (out.id > 0) return out.isReported;
    }
    return true;
}

void TestPalmSuppressedWhilePenActive() {
    TouchTracker tracker;
    const auto palm = PalmContact();

    // Track is born while the pen is already active -> candidate for rejection.
    bool reported = true;
    for (int i = 0; i < 4; ++i) {
        reported = StepAndCheckReported(tracker, true, palm);
    }
    Require(!reported, "palm arriving during pen mode should not be reported");
}

// The linger tail is the window the local AFT path structurally cannot cover: the tip has
// left, so there is no position to measure a distance against, while the palm is still
// down. Model it properly — write with a real tip signal first, then take the tip away and
// keep only the arbiter's flag.
void TestSuppressionSurvivesLingerAfterPenLeaves() {
    TouchTracker tracker;
    const auto palm = PalmContact(30.0f, 20.0f);
    // Tip well outside m_stylusAftRadius of the palm, the realistic geometry: the writing
    // hand rests centimetres away from where the nib is, so only the screen-wide verdict
    // can reach it even while the pen is fully in range.
    const auto writing = WritingAt(34.0f, 24.0f);

    for (int i = 0; i < 4; ++i) StepAndCheckReported(tracker, true, palm, writing);

    // Tip gone, flag still asserted: this is what StylusTouchArbiter emits while
    // Lingering.
    for (int i = 0; i < 10; ++i) {
        const bool reported = StepAndCheckReported(tracker, true, palm);
        Require(!reported, "palm must stay suppressed through the linger tail");
    }
}

// Once the arbiter drops the flag the episode is genuinely over, and the residual hold on
// the track does NOT keep suppressing on its own — the countdown only gates while pen mode
// or the local AFT context is live (see the penGate branch in TouchTracker::Process). The
// tail belongs to the pen side; if it ever needs to be longer, m_lingerFrames is the knob,
// not the touch-side counter.
void TestSuppressionEndsWhenPenModeFlagDrops() {
    TouchTracker tracker;
    const auto palm = PalmContact();

    for (int i = 0; i < 4; ++i) StepAndCheckReported(tracker, true, palm);
    Require(StepAndCheckReported(tracker, false, palm),
            "with pen mode dropped the palm must be published again");
}

// A single frame without the flag is a dropout, not the end of the episode. Any frame that
// carries no stylus payload zeroes interop, and if that restarted the episode clock every
// surviving track would become older than the episode and inherit the "in-progress
// gesture" exemption permanently — a palm being rejected would start reporting one frame
// later and never stop.
//
// m_stylusAftPalmSuppressFrames is cut to 2 so the residual hold cannot mask the age test:
// with the shipped 100 the track would stay suppressed by the countdown alone for the
// whole test and the regression would pass unnoticed.
void TestSingleFrameGapDoesNotRestartPenModeEpisode() {
    TouchTracker tracker;
    tracker.m_stylusAftPalmSuppressFrames = 2;
    const auto palm = PalmContact();

    for (int i = 0; i < 3; ++i) {
        Require(!StepAndCheckReported(tracker, true, palm),
                "palm born during pen mode must be suppressed");
    }

    StepAndCheckReported(tracker, false, palm); // the dropout

    for (int i = 0; i < 6; ++i) {
        Require(!StepAndCheckReported(tracker, true, palm),
                "a track older than the dropout must not become exempt after it");
    }
}

// The tolerance is a window, not an amnesty: a gap long enough to be a real departure ends
// the episode, and the contact that outlived it is then a gesture in progress by the same
// rule that protects a scroll.
void TestLongPenModeGapEndsTheEpisode() {
    TouchTracker tracker;
    tracker.m_stylusAftPalmSuppressFrames = 2;
    const auto palm = PalmContact();

    for (int i = 0; i < 3; ++i) StepAndCheckReported(tracker, true, palm);
    for (int i = 0; i < 9; ++i) StepAndCheckReported(tracker, false, palm);

    Require(StepAndCheckReported(tracker, true, palm),
            "after a full departure the surviving track predates the new episode");
}

void TestPreexistingGestureIsNotHijacked() {
    TouchTracker tracker;
    const auto palm = PalmContact();

    // A large contact is already being tracked with no pen anywhere.
    bool reported = false;
    for (int i = 0; i < 12; ++i) {
        reported = StepAndCheckReported(tracker, false, palm);
    }
    Require(reported, "an established contact should report normally with no pen present");

    // Pen shows up. The track predates the pen-mode episode, so it must survive.
    for (int i = 0; i < 3; ++i) {
        reported = StepAndCheckReported(tracker, true, palm);
        Require(reported, "a gesture already in progress must not be hijacked by pen mode");
    }
}

void TestFingerNotSuppressedByPenMode() {
    TouchTracker tracker;
    const auto finger = FingerContact();

    bool reported = false;
    for (int i = 0; i < 4; ++i) {
        reported = StepAndCheckReported(tracker, true, finger);
    }
    Require(reported, "a fingertip is below the palm thresholds and must still report");
}

void TestDisabledPenModeIsInert() {
    TouchTracker tracker;
    tracker.m_penModeSuppressEnabled = false;
    const auto palm = PalmContact();

    bool reported = false;
    for (int i = 0; i < 4; ++i) {
        reported = StepAndCheckReported(tracker, true, palm);
    }
    Require(reported, "with pen-mode suppression disabled nothing should be rejected");
}

// The screen-wide path must not key on sizeMm. sizeMm is fitted as
// cbrt(signalSum) * 0.35, so a plain fingertip (signalSum 3000 -> ~5.0 mm) clears the
// 2.5 mm palm threshold; keying on it would suppress every touch on screen for ~830 ms
// each time a pen came into range. The tip-radius path has the same constraint, covered
// by TestAgedFingerNearTipKeepsReporting.
void TestFingerIsNotTreatedAsPalmBySizeMm() {
    const auto finger = FingerContact();
    const float fittedSizeMm = std::cbrt(static_cast<float>(finger.signalSum)) * 0.35f;
    Require(fittedSizeMm > 2.5f,
            "precondition: the fitted sizeMm of a fingertip does exceed the palm threshold");
    Require(finger.areaCells < 20,
            "precondition: the same fingertip is well under the palm areaCells threshold");

    TouchTracker tracker;
    bool reported = false;
    for (int i = 0; i < 6; ++i) {
        reported = StepAndCheckReported(tracker, true, finger);
    }
    Require(reported, "a fingertip must not be rejected as a palm on the sizeMm branch");
}

// ── Local tip-radius AFT path (ShouldStylusAftSuppress) ─────────────────────────
//
// Reaching it needs a latched tip position, which the pen-mode tests above deliberately
// withhold. Both tests age the contact before the pen appears: the AFT path suppresses any
// contact younger than m_stylusAftDebounceFrames outright, so a fresh track proves nothing
// about the palm/exemption ordering.

// Palm is decided BEFORE the strong-touch exemption. The palm this asserts on is exactly
// the one measured on the recorded fixture (areaCells 31 / signalSum 18671), and it clears the
// exemption comfortably — with the branches in the other order it would be waved through,
// which was the state that made m_stylusAftPalm* dead configuration.
void TestRestingPalmNearTipIsRejectedDespiteStrongTouchExemption() {
    TouchTracker tracker;
    const auto palm = PalmContact(30.0f, 20.0f);
    Require(palm.signalSum >= tracker.m_stylusSuppressTouchSignalKeep &&
                palm.areaCells >= tracker.m_stylusSuppressTouchAreaKeep,
            "precondition: this palm does qualify for the strong-touch exemption");
    Require(palm.areaCells >= tracker.m_stylusAftPalmAreaThreshold,
            "precondition: and it is over the palm areaCells threshold");

    bool reported = false;
    for (int i = 0; i < 8; ++i) reported = StepAndCheckReported(tracker, false, palm);
    Require(reported, "precondition: the palm reports normally before the pen shows up");

    // Pen starts writing just beside it, inside m_stylusAftRadius. Only the local AFT path
    // is armed here — the arbiter flag stays clear, so nothing but ShouldStylusAftSuppress
    // can reject this contact.
    reported = StepAndCheckReported(tracker, false, palm, WritingAt(31.5f, 21.0f));
    Require(!reported, "a resting palm within the tip radius must be rejected as palm");
}

// The same path must not key on sizeMm. TouchTracker overwrites contact.sizeMm with the
// value fitted from signalSum before the AFT test runs, so a plain fingertip arrives
// carrying ~5 mm and would be called a palm by any sizeMm branch — and then pinned for
// m_stylusAftPalmSuppressFrames, since palm is now evaluated first.
void TestAgedFingerNearTipKeepsReporting() {
    TouchTracker tracker;
    const auto finger = FingerContact(30.0f, 20.0f);
    const float fittedSizeMm = std::cbrt(static_cast<float>(finger.signalSum)) * 0.35f;
    Require(fittedSizeMm > tracker.m_stylusAftPalmSizeThresholdMm,
            "precondition: the fitted sizeMm of a fingertip exceeds the palm threshold");
    Require(finger.areaCells < tracker.m_stylusAftPalmAreaThreshold,
            "precondition: its measured areaCells is well under the palm threshold");

    for (int i = 0; i < 8; ++i) StepAndCheckReported(tracker, false, finger);

    const bool reported =
        StepAndCheckReported(tracker, false, finger, WritingAt(31.5f, 21.0f));
    Require(reported, "an established fingertip near the tip must keep reporting");
}

} // namespace

int main() {
    try {
        TestPalmSuppressedWhilePenActive();
        TestSuppressionSurvivesLingerAfterPenLeaves();
        TestSuppressionEndsWhenPenModeFlagDrops();
        TestSingleFrameGapDoesNotRestartPenModeEpisode();
        TestLongPenModeGapEndsTheEpisode();
        TestPreexistingGestureIsNotHijacked();
        TestFingerNotSuppressedByPenMode();
        TestDisabledPenModeIsInert();
        TestFingerIsNotTreatedAsPalmBySizeMm();
        TestRestingPalmNearTipIsRejectedDespiteStrongTouchExemption();
        TestAgedFingerNearTipKeepsReporting();
        std::cout << "[TEST] Pen-mode touch suppression tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
