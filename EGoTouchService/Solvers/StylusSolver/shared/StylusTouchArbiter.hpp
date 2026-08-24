#pragma once

#include "StylusSolver/AsaTypes.hpp"
#include "SolverTypes.h"

#include <algorithm>
#include <cstdint>

namespace Solvers::Stylus {

// StylusTouchArbiter — produces the pen-side half of the pen/touch arbitration contract.
//
// DecisionRuntime::touchSuppressCarry / touchSuppressFrames have existed since the
// original port and are forwarded to the touch pipeline by StylusRuntimeCommit, but
// nothing ever wrote them: they were permanently false/0 and the only reader left was
// the diagnostics UI. This class fills that gap.
//
// The verdict is screen-wide and travels the whole way (see
// docs/stylus_touch_arbitration_design.md). DeviceRuntime runs the stylus pipeline first,
// so this class is the producer; StylusRuntimeCommit copies decision.touchSuppress* into
// interop; StylusTouchSuppressor::Process reads the incoming interop values before
// writing its own local-radius verdict and folds them back in with max(), so a pen-side
// assertion cannot be erased by a touch-side stage that saw nothing; TouchTracker is the
// consumer, latching interop.touchSuppressActive into its pen-mode gate. Nothing in that
// chain may clear the fields unconditionally — the linger tail exists precisely for the
// frames where every local test comes back negative.
//
// The "carry" in the field name is the point: suppression has to outlive the pen. When
// the tip lifts, the palm is usually still resting on the panel while the stylus signal
// is already gone, so any tip-distance test fails exactly when it is needed most. The
// Lingering state keeps the flag asserted for a configurable tail.
class StylusTouchArbiter {
public:
    enum class Mode : uint8_t {
        Idle = 0,    // no pen — touch unrestricted
        Hovering,    // pen in range, not touching
        Writing,     // pen in contact
        Lingering,   // pen just left; hold suppression to catch palm rebound
    };

    bool m_enabled = true;
    // ~330 ms at 120 Hz. Long enough to cover lift-off palm rebound, short enough not to
    // swallow a deliberate finger tap right after the pen is set down.
    int32_t m_lingerFrames = 40;

    inline void Reset() {
        m_mode = Mode::Idle;
        m_lingerLeft = 0;
    }

    inline void Process(HeatmapFrame& frame) {
        auto& runtime = frame.stylus.runtime.Active();
        auto& decision = runtime.decision;

        if (!m_enabled) {
            Reset();
            decision.touchSuppressCarry = false;
            decision.touchSuppressFrames = 0;
            return;
        }

        // Derived from the runtime rather than from stylus.output, because the arbiter
        // runs before StylusRuntimeCommit populates it. inRange is the same expression
        // Commit uses; writing is not — Commit derives tipDown from
        // decision.tipDownCandidate, this uses outputPressure > 0. The two can disagree
        // (a tipDownCandidate frame whose pressure has not yet been mapped above zero),
        // and today that is harmless: Writing and Hovering take the same branch here,
        // both assert the flag and both recharge the linger counter. Mode is exposed only
        // for tests and diagnostics. Anything that starts treating Writing differently
        // has to switch to tipDownCandidate first, or the two paths will disagree about
        // when a stroke begins.
        const bool inRange = decision.inRangeCandidate && runtime.post.finalValid;
        const bool writing = inRange && runtime.pressure.outputPressure > 0;

        if (inRange) {
            m_mode = writing ? Mode::Writing : Mode::Hovering;
            m_lingerLeft = std::max<int32_t>(m_lingerFrames, 0);
        } else if (m_lingerLeft > 0) {
            m_mode = Mode::Lingering;
            --m_lingerLeft;
        } else {
            m_mode = Mode::Idle;
        }

        decision.touchSuppressCarry = (m_mode != Mode::Idle);
        decision.touchSuppressFrames =
            static_cast<uint8_t>(std::clamp<int32_t>(m_lingerLeft, 0, 255));
    }

    Mode GetMode() const { return m_mode; }
    int32_t GetLingerRemaining() const { return m_lingerLeft; }

private:
    Mode m_mode = Mode::Idle;
    int32_t m_lingerLeft = 0;
};

} // namespace Solvers::Stylus
