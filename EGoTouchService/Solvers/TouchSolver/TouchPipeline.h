#pragma once
// ══════════════════════════════════════════════════════════════════════
// TouchPipeline — Single-CPP linear orchestrator for the Touch solver.
// Architecture: mirrors StylusPipeline (v2.4).
//   - All algorithm modules are header-only (.hpp) and held as members.
//   - Process() executes all phases linearly, no virtual dispatch.
//   - Config schema/save/load unified in this class.
// ══════════════════════════════════════════════════════════════════════

#include "SolverTypes.h"

// ── Phase 1: Frame Parsing ──
#include "MasterFrameParser.hpp"

// ── Phase 2: Signal Conditioning ──
#include "BaselineTracker.hpp"
#include "CMFProcessor.hpp"

// ── Phase 3: Feature Extraction ──
#include "MacroZoneDetector.hpp"
#include "PeakDetector.hpp"
#include "TouchClassifier.hpp"
#include "PalmBoxSuppressor.hpp"

// ── Phase 4: Contact Extraction & Post-Processing ──
#include "ContactExtractor.hpp"
#include "EdgeCompensation.hpp"
#include "StylusTouchSuppressor.hpp"
#include "TouchDiagnosticCache.hpp"

// ── Phase 5: Tracking & Coordinate Filtering ──
#include "TouchTracker.hpp"
#include "CoordinateFilter.hpp"

// ── Phase 6: Stroke, Gesture & Output ──
#include "StrokeAggregator.hpp"
#include "TouchGestureStateMachine.hpp"

#include <atomic>
#include <string>
#include <vector>
#include <array>

namespace Config {
class ConfigBinder;
class ConfigStore;
}

namespace Solvers {

class TouchPipeline {
public:
    TouchPipeline() {}

    /// Main entry: processes one frame through all 6 phases.
    bool Process(HeatmapFrame& frame);
    bool ProcessMasterParserOnly(HeatmapFrame& frame);

    /// Runs the tracker/filter/gesture output stage. Exposed for focused unit tests.
    bool ProcessTrackingAndGesture(HeatmapFrame& frame);
    bool ProcessGestureOutput(HeatmapFrame& frame);

    /// Pipeline name for config file section header.
    std::string GetName() const { return "TouchPipeline"; }

    // ── Configuration interface ──
    void registerBindings(Config::ConfigBinder& binder);
    void applyConfig(const Config::ConfigStore& store);

    // ── Thread-safe accessors for UI/IPC (guarded by mutex) ──
    std::vector<Touch::Peak> GetPeaks() const;
    std::array<uint8_t, 2400> GetTouchZones() const;
    std::array<uint8_t, 2400> GetZoneEdge() const;

    int GetCachedPeakCount() const { return m_cachedPeakCount.load(std::memory_order_relaxed); }
    int GetCachedZoneCount() const { return m_cachedZoneCount.load(std::memory_order_relaxed); }
    int GetCachedContactCount() const { return m_cachedContactCount.load(std::memory_order_relaxed); }

    // ── Public module access (for direct parameter tuning from UI thread) ──
    // NOTE: Write access must be done when pipeline is idle or via config reload.
    Touch::MasterFrameParser         m_frameParser;
    Touch::BaselineTracker            m_baseline;
    Touch::CMFProcessor              m_cmf;
    Touch::MacroZoneDetector         m_macroZoneDet;
    Touch::PeakDetector              m_peakDet;
    Touch::TouchClassifier           m_touchClassifier;
    Touch::PalmBoxSuppressor         m_palmBoxSuppressor;
    Touch::ContactExtractor          m_contactExtractor;
    Touch::EdgeCompensator           m_edgeComp;
    Touch::EdgeRejector              m_edgeReject;
    Touch::StylusTouchSuppressor     m_stylusSuppress;
    Touch::TouchTracker              m_tracker;
    Touch::CoordinateFilter          m_coordFilter;
    Touch::StrokeAggregator          m_strokes;
    Touch::TouchGestureStateMachine  m_gesture;

private:
    void ReserveContactCapacity(HeatmapFrame& frame) const;
    bool ProcessFrameParser(HeatmapFrame& frame);
    bool ProcessSignalConditioning(HeatmapFrame& frame);
    void GenerateContacts(HeatmapFrame& frame);
    void PostProcessContacts(HeatmapFrame& frame);
    void UpdateContactCaches(HeatmapFrame& frame);
    void ResetIdleOutputs(HeatmapFrame& frame);

#if EGOTOUCH_DIAG
    void UpdateDiagnosticCaches(HeatmapFrame& frame);
#endif

#if EGOTOUCH_DIAG
    Touch::DiagnosticCache m_diagCache;
#endif

    bool m_prevHasLiveTouchState = false;

    // UI-thread-safe cache
    std::atomic<int> m_cachedPeakCount{0};
    std::atomic<int> m_cachedZoneCount{0};
    std::atomic<int> m_cachedContactCount{0};
};

} // namespace Solvers
