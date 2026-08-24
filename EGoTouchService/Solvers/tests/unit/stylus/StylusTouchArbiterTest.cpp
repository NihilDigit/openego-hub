// Covers the pen-side half of the pen/touch arbitration contract.
//
// DecisionRuntime::touchSuppressCarry / touchSuppressFrames were dead fields for the
// whole life of the port — declared, forwarded to the touch pipeline by
// StylusRuntimeCommit, and never written by anyone. StylusTouchArbiter fills that gap.
// The behaviour that matters and is easy to regress is the linger tail: suppression must
// outlive the pen, because at lift-off the palm is still on the panel while the stylus
// signal is already gone.

#include "StylusTouchArbiter.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using Solvers::Stylus::StylusTouchArbiter;

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// Drives one frame through the arbiter with the given pen state.
void Step(StylusTouchArbiter& arbiter,
          Solvers::HeatmapFrame& frame,
          bool inRange,
          uint16_t pressure) {
    auto& runtime = frame.stylus.runtime.Active();
    runtime.decision.inRangeCandidate = inRange;
    runtime.post.finalValid = inRange;
    runtime.pressure.outputPressure = pressure;
    arbiter.Process(frame);
}

const Asa::DecisionRuntime& Decision(Solvers::HeatmapFrame& frame) {
    return frame.stylus.runtime.Active().decision;
}

void TestIdleWhenNoPen() {
    StylusTouchArbiter arbiter;
    Solvers::HeatmapFrame frame{};

    Step(arbiter, frame, false, 0);

    Require(arbiter.GetMode() == StylusTouchArbiter::Mode::Idle, "no pen should stay Idle");
    Require(!Decision(frame).touchSuppressCarry, "Idle must not assert suppression");
    Require(Decision(frame).touchSuppressFrames == 0, "Idle must not hold linger frames");
}

void TestHoverAndWritingModes() {
    StylusTouchArbiter arbiter;
    Solvers::HeatmapFrame frame{};

    Step(arbiter, frame, true, 0);
    Require(arbiter.GetMode() == StylusTouchArbiter::Mode::Hovering, "in range without pressure is Hovering");
    Require(Decision(frame).touchSuppressCarry, "Hovering must assert suppression");

    Step(arbiter, frame, true, 400);
    Require(arbiter.GetMode() == StylusTouchArbiter::Mode::Writing, "pressure makes it Writing");
    Require(Decision(frame).touchSuppressCarry, "Writing must assert suppression");

    Step(arbiter, frame, true, 0);
    Require(arbiter.GetMode() == StylusTouchArbiter::Mode::Hovering, "losing pressure returns to Hovering");
}

void TestLingerOutlivesThePen() {
    StylusTouchArbiter arbiter;
    arbiter.m_lingerFrames = 5;
    Solvers::HeatmapFrame frame{};

    Step(arbiter, frame, true, 400);
    Require(Decision(frame).touchSuppressFrames == 5, "writing should arm the full linger window");

    // Pen leaves. This is the case tip-distance tests cannot handle: the stylus signal is
    // gone, but the palm has not lifted yet.
    for (int i = 4; i >= 0; --i) {
        Step(arbiter, frame, false, 0);
        if (i > 0) {
            Require(arbiter.GetMode() == StylusTouchArbiter::Mode::Lingering,
                    "suppression must persist while the linger window runs");
            Require(Decision(frame).touchSuppressCarry,
                    "linger frames must keep the suppression flag asserted");
        }
        Require(Decision(frame).touchSuppressFrames == static_cast<uint8_t>(i),
                "linger counter should tick down exactly one frame at a time");
    }

    Step(arbiter, frame, false, 0);
    Require(arbiter.GetMode() == StylusTouchArbiter::Mode::Idle, "arbiter must fall back to Idle");
    Require(!Decision(frame).touchSuppressCarry, "suppression must release once linger expires");
}

void TestPenReturnRearmsWindow() {
    StylusTouchArbiter arbiter;
    arbiter.m_lingerFrames = 4;
    Solvers::HeatmapFrame frame{};

    Step(arbiter, frame, true, 0);
    Step(arbiter, frame, false, 0);
    Require(Decision(frame).touchSuppressFrames == 3, "window should have started draining");

    // A pen that dips out of range for a frame and comes back must not inherit a
    // half-drained window, or repeated hover flicker would erode the protection.
    Step(arbiter, frame, true, 0);
    Require(arbiter.GetMode() == StylusTouchArbiter::Mode::Hovering, "pen returning resumes Hovering");
    Require(Decision(frame).touchSuppressFrames == 4, "returning pen must re-arm the full window");
}

void TestDisabledIsInert() {
    StylusTouchArbiter arbiter;
    arbiter.m_enabled = false;
    Solvers::HeatmapFrame frame{};

    Step(arbiter, frame, true, 400);

    Require(arbiter.GetMode() == StylusTouchArbiter::Mode::Idle, "disabled arbiter stays Idle");
    Require(!Decision(frame).touchSuppressCarry, "disabled arbiter must not assert suppression");
    Require(Decision(frame).touchSuppressFrames == 0, "disabled arbiter must not hold frames");
}

void TestZeroLingerReleasesImmediately() {
    StylusTouchArbiter arbiter;
    arbiter.m_lingerFrames = 0;
    Solvers::HeatmapFrame frame{};

    Step(arbiter, frame, true, 400);
    Require(Decision(frame).touchSuppressCarry, "writing still suppresses with a zero window");

    Step(arbiter, frame, false, 0);
    Require(arbiter.GetMode() == StylusTouchArbiter::Mode::Idle, "zero linger releases on the next frame");
    Require(!Decision(frame).touchSuppressCarry, "zero linger must not leave suppression asserted");
}

} // namespace

int main() {
    try {
        TestIdleWhenNoPen();
        TestHoverAndWritingModes();
        TestLingerOutlivesThePen();
        TestPenReturnRearmsWindow();
        TestDisabledIsInert();
        TestZeroLingerReleasesImmediately();
        std::cout << "[TEST] Stylus touch arbiter tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
