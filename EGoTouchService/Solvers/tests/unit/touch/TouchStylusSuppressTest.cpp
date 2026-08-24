#include "TouchSolver/StylusTouchSuppressor.hpp"
#include "TouchSolver/TouchTracker.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using Solvers::HeatmapFrame;
using Solvers::TouchContact;
using Solvers::Touch::StylusTouchSuppressor;
using Solvers::Touch::TouchTracker;

struct ContactSpec {
    float x = 0.0f;
    float y = 0.0f;
    int areaCells = 0;
    int signalSum = 0;
    float sizeMm = 0.0f;
};

struct StylusSpec {
    bool valid = false;
    float x = 0.0f;
    float y = 0.0f;
    uint16_t pressure = 0;
    uint16_t signalX = 0;
    uint16_t signalY = 0;
    bool tipDown = false;
};

struct TrackerHarness {
    StylusTouchSuppressor stylusSuppressor;
    TouchTracker tracker;
    uint64_t timestamp = 0;

    TrackerHarness() {
        stylusSuppressor.m_stylusSuppressGlobalEnabled = true;
        stylusSuppressor.m_stylusSuppressLocalEnabled = true;
        stylusSuppressor.m_stylusAftEnabled = true;
        tracker.m_stylusSuppressGlobalEnabled = true;
        tracker.m_stylusSuppressLocalEnabled = true;
        tracker.m_stylusAftEnabled = true;
    }

    HeatmapFrame Run(std::initializer_list<ContactSpec> contacts, const StylusSpec& stylus) {
        HeatmapFrame frame;
        timestamp += 8;
        frame.timestamp = timestamp;
        for (const auto& spec : contacts) {
            TouchContact contact;
            contact.x = spec.x;
            contact.y = spec.y;
            contact.areaCells = spec.areaCells;
            contact.signalSum = spec.signalSum;
            contact.sizeMm = spec.sizeMm;
            frame.touch.output.contacts.push_back(contact);
        }

        frame.stylus.output.valid = stylus.valid;
        frame.stylus.output.inRange = stylus.valid;
        frame.stylus.output.tipDown = stylus.tipDown;
        frame.stylus.output.pressure = stylus.pressure;
        frame.stylus.output.point.valid = stylus.valid;
        frame.stylus.output.point.x = stylus.x * 1024.0f;
        frame.stylus.output.point.y = stylus.y * 1024.0f;
        frame.stylus.output.point.pressure = stylus.pressure;
        frame.stylus.interop.signalX = stylus.signalX;
        frame.stylus.interop.signalY = stylus.signalY;
        frame.stylus.interop.maxRawPeak = std::max(stylus.signalX, stylus.signalY);

        stylusSuppressor.Process(frame);
        tracker.Process(frame);
        return frame;
    }
};

StylusSpec MakeStylusSpec(bool valid,
                          float x,
                          float y,
                          uint16_t pressure,
                          uint16_t signalX,
                          uint16_t signalY,
                          bool tipDown) {
    StylusSpec stylus;
    stylus.valid = valid;
    stylus.x = x;
    stylus.y = y;
    stylus.pressure = pressure;
    stylus.signalX = signalX;
    stylus.signalY = signalY;
    stylus.tipDown = tipDown;
    return stylus;
}

std::vector<const TouchContact*> VisibleContacts(const HeatmapFrame& frame) {
    std::vector<const TouchContact*> out;
    for (const auto& contact : frame.touch.output.contacts) {
        if (contact.isReported) out.push_back(&contact);
    }
    return out;
}

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void TestWeakTouchNearStylusIsSuppressedUsingStylusCoordinates() {
    TrackerHarness harness;
    const StylusSpec stylus = MakeStylusSpec(true, 12.5f, 7.75f, 180, 500, 2200, true);
    const auto frame = harness.Run({
        ContactSpec{12.5f, 7.75f, 3, 160, 0.8f}
    }, stylus);

    Require(frame.touch.output.contacts.empty(), "weak overlap contact should be locally suppressed");
    Require(frame.stylus.interop.touchSuppressActive, "stylus suppress flag should be active");
    Require(frame.stylus.interop.recheckOverlap, "stylus overlap flag should be set");
    Require(frame.stylus.interop.touchSuppressFrames > 0, "suppress hold should be armed");
}

void TestZeroPressureInteropSignalStillSuppressesWeakOverlapTouch() {
    TrackerHarness harness;
    const StylusSpec stylus = MakeStylusSpec(true, 12.5f, 7.75f, 0, 500, 2200, false);
    const auto frame = harness.Run({
        ContactSpec{12.55f, 7.80f, 3, 160, 0.8f}
    }, stylus);

    Require(frame.touch.output.contacts.empty(), "strong interop signal should suppress weak overlap even at zero pressure");
    Require(frame.stylus.interop.touchSuppressActive, "touch suppress should stay active from interop evidence");
}

void TestStrongTouchNearStylusIsPreserved() {
    TrackerHarness harness;
    const StylusSpec stylus = MakeStylusSpec(true, 18.0f, 12.0f, 160, 700, 2100, true);
    const auto frame = harness.Run({
        ContactSpec{18.1f, 12.1f, 18, 7200, 3.2f}
    }, stylus);

    const auto visible = VisibleContacts(frame);
    Require(visible.size() == 1, "strong real touch near stylus should be preserved");
    Require(visible[0]->signalSum == 7200, "preserved touch should keep original signal");
}

void TestFarTouchIsNotMisSuppressed() {
    TrackerHarness harness;
    const StylusSpec stylus = MakeStylusSpec(true, 8.0f, 8.0f, 140, 600, 2000, true);
    const auto frame = harness.Run({
        ContactSpec{24.0f, 20.0f, 10, 1200, 1.9f}
    }, stylus);

    const auto visible = VisibleContacts(frame);
    Require(visible.size() == 1, "far touch should remain visible");
    Require(!frame.stylus.interop.recheckOverlap, "far touch should not look overlapped with stylus");
}

void TestAftKeepsSuppressingRecentWeakTouchAfterStylusLeaves() {
    TrackerHarness harness;

    const auto seed = harness.Run({}, MakeStylusSpec(true, 10.0f, 10.0f, 200, 640, 2400, true));
    Require(seed.touch.output.contacts.empty(), "seed stylus frame should not create contacts");

    const auto aftFrame = harness.Run({
        ContactSpec{10.2f, 10.1f, 4, 180, 0.9f}
    }, StylusSpec{});

    Require(aftFrame.touch.output.contacts.size() == 1, "AFT frame should still keep hidden track state");
    Require(!aftFrame.touch.output.contacts[0].isReported, "recent weak touch should be hidden by AFT");
    Require(aftFrame.touch.output.contacts[0].id > 0, "AFT-hidden touch should still receive a track id");

    const auto aftHold = harness.Run({
        ContactSpec{10.3f, 10.0f, 4, 180, 0.9f}
    }, StylusSpec{});
    Require(aftHold.touch.output.contacts.size() == 1, "AFT hold frame should keep the tracked touch");
    Require(!aftHold.touch.output.contacts[0].isReported, "AFT hold should continue suppressing weak touch");
}

void TestGlobalDisableBypassesLocalAndAftSuppression() {
    TrackerHarness harness;
    harness.stylusSuppressor.m_stylusSuppressGlobalEnabled = false;
    harness.tracker.m_stylusSuppressGlobalEnabled = false;

    const StylusSpec stylus = MakeStylusSpec(true, 12.5f, 7.75f, 180, 500, 2200, true);
    const auto frame = harness.Run({
        ContactSpec{12.5f, 7.75f, 3, 160, 0.8f}
    }, stylus);

    const auto visible = VisibleContacts(frame);
    Require(frame.touch.output.contacts.size() == 1, "global-off contact should not be erased by local suppression");
    Require(visible.size() == 1, "global-off contact should remain reported");
    Require(!frame.stylus.interop.touchSuppressActive, "global-off should clear touch suppress active flag");
    Require(frame.stylus.interop.touchSuppressFrames == 0, "global-off should clear suppress hold frames");
}

void TestGlobalDisableClearsExistingAftSuppressionHold() {
    TrackerHarness harness;

    harness.Run({}, MakeStylusSpec(true, 10.0f, 10.0f, 200, 640, 2400, true));
    const auto aftFrame = harness.Run({
        ContactSpec{10.2f, 10.1f, 4, 180, 0.9f}
    }, StylusSpec{});
    Require(aftFrame.touch.output.contacts.size() == 1, "AFT setup should create hidden tracked touch");
    Require(!aftFrame.touch.output.contacts[0].isReported, "AFT setup touch should be hidden");

    harness.stylusSuppressor.m_stylusSuppressGlobalEnabled = false;
    harness.tracker.m_stylusSuppressGlobalEnabled = false;
    const auto disabledFrame = harness.Run({
        ContactSpec{10.3f, 10.0f, 4, 180, 0.9f}
    }, StylusSpec{});

    const auto visible = VisibleContacts(disabledFrame);
    Require(disabledFrame.touch.output.contacts.size() == 1, "global-off AFT hold should keep the tracked touch");
    Require(visible.size() == 1, "global-off should report the touch despite previous AFT hold");
    Require(!disabledFrame.stylus.interop.touchSuppressActive, "global-off should clear active suppress state");
    Require(disabledFrame.stylus.interop.touchSuppressFrames == 0, "global-off should clear previous suppress frames");
}

// ── StylusTouchSuppressor as a relay ────────────────────────────────────────────
//
// The stage sits between StylusTouchArbiter (which writes the screen-wide verdict into
// interop) and TouchTracker (which consumes it), and it rewrites the same two fields from
// its own local-radius conclusion. Erasing what arrived is the failure mode that matters:
// the arbiter's verdict is the only thing covering the linger tail, where the tip signal
// is gone and every distance test here necessarily comes back negative.
HeatmapFrame RunSuppressorOnly(StylusTouchSuppressor& suppressor,
                               bool penModeActive,
                               uint8_t penModeFrames,
                               std::initializer_list<ContactSpec> contacts,
                               const StylusSpec& stylus) {
    HeatmapFrame frame;
    frame.stylus.interop.touchSuppressActive = penModeActive;
    frame.stylus.interop.touchSuppressFrames = penModeFrames;
    for (const auto& spec : contacts) {
        TouchContact contact;
        contact.x = spec.x;
        contact.y = spec.y;
        contact.areaCells = spec.areaCells;
        contact.signalSum = spec.signalSum;
        contact.sizeMm = spec.sizeMm;
        frame.touch.output.contacts.push_back(contact);
    }
    frame.stylus.output.valid = stylus.valid;
    frame.stylus.output.inRange = stylus.valid;
    frame.stylus.output.tipDown = stylus.tipDown;
    frame.stylus.output.pressure = stylus.pressure;
    frame.stylus.output.point.valid = stylus.valid;
    frame.stylus.output.point.x = stylus.x * 1024.0f;
    frame.stylus.output.point.y = stylus.y * 1024.0f;
    frame.stylus.interop.signalX = stylus.signalX;
    frame.stylus.interop.signalY = stylus.signalY;
    frame.stylus.interop.maxRawPeak = std::max(stylus.signalX, stylus.signalY);
    suppressor.Process(frame);
    return frame;
}

void TestSuppressorCarriesPenModeVerdictWithNoTipSignal() {
    StylusTouchSuppressor suppressor;

    // The linger tail: flag asserted, no stylus point at all. Every local test bails out,
    // and the incoming verdict has to come out the other side untouched.
    const auto frame = RunSuppressorOnly(suppressor, true, 33, {
        ContactSpec{24.0f, 20.0f, 10, 1200, 1.9f}
    }, StylusSpec{});

    Require(frame.stylus.interop.touchSuppressActive,
            "pen-mode verdict must survive a frame with no tip signal");
    Require(frame.stylus.interop.touchSuppressFrames == 33,
            "the arbiter's linger countdown must survive unchanged");
    Require(frame.touch.output.contacts.size() == 1,
            "carrying the verdict must not erase contacts by itself");
}

void TestSuppressorTakesMaxOfPenModeAndOwnHold() {
    const StylusSpec stylus = MakeStylusSpec(true, 12.5f, 7.75f, 180, 500, 2200, true);
    const ContactSpec weakOverlap{12.5f, 7.75f, 3, 160, 0.8f};

    StylusTouchSuppressor lowIncoming;
    const auto ownWins = RunSuppressorOnly(lowIncoming, true, 5, {weakOverlap}, stylus);
    Require(ownWins.touch.output.contacts.empty(), "weak overlap contact should be erased");
    Require(ownWins.stylus.interop.touchSuppressFrames > 5,
            "this stage's own hold must not be capped by a shorter incoming countdown");

    StylusTouchSuppressor highIncoming;
    const auto incomingWins = RunSuppressorOnly(highIncoming, true, 200, {weakOverlap}, stylus);
    Require(incomingWins.stylus.interop.touchSuppressFrames == 200,
            "a longer incoming countdown must not be shortened to this stage's hold");
}

void TestSuppressorInventsNothingWithoutEvidence() {
    StylusTouchSuppressor suppressor;
    const auto frame = RunSuppressorOnly(suppressor, false, 0, {
        ContactSpec{24.0f, 20.0f, 10, 1200, 1.9f}
    }, StylusSpec{});

    Require(!frame.stylus.interop.touchSuppressActive,
            "no pen and no evidence must leave the verdict clear");
    Require(frame.stylus.interop.touchSuppressFrames == 0,
            "no pen and no evidence must leave the countdown at zero");
}

} // namespace

int main() {
    try {
        TestWeakTouchNearStylusIsSuppressedUsingStylusCoordinates();
        TestZeroPressureInteropSignalStillSuppressesWeakOverlapTouch();
        TestStrongTouchNearStylusIsPreserved();
        TestFarTouchIsNotMisSuppressed();
        TestAftKeepsSuppressingRecentWeakTouchAfterStylusLeaves();
        TestGlobalDisableBypassesLocalAndAftSuppression();
        TestGlobalDisableClearsExistingAftSuppressionHold();
        TestSuppressorCarriesPenModeVerdictWithNoTipSignal();
        TestSuppressorTakesMaxOfPenModeAndOwnHold();
        TestSuppressorInventsNothingWithoutEvidence();
        std::cout << "[TEST] TouchTracker stylus suppress tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
