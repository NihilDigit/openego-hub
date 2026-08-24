#include "StylusSolver/StylusPipeline.h"
#include "AftCoorProcess.hpp"
#include "CoorIIRProcess.hpp"
#include "config/ConfigBinder.h"
#include "config/ConfigSchemaSnapshot.h"
#include "config/ConfigStore.h"
#include "config/ConfigValue.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

const Config::ConfigSchemaEntry* FindSchemaEntry(const Config::ConfigSchemaSnapshot& schema,
                                                 std::string_view yamlPath) {
    for (const auto& entry : schema.entries) {
        if (entry.yamlPath == yamlPath) {
            return &entry;
        }
    }
    return nullptr;
}

void RequireInt32Range(const Config::ConfigSchemaEntry& entry,
                       int32_t expectedDefault,
                       double expectedMin,
                       double expectedMax) {
    Require(entry.uiType == Config::ConfigUiType::Int32, "schema entry should be Int32");
    Require(entry.defaultValue == Config::ConfigValue(expectedDefault), "schema entry default should match");
    Require(entry.currentValue == Config::ConfigValue(expectedDefault), "schema entry current value should match default");
    Require(entry.range.has_value(), "schema entry should expose a range");
    Require(entry.range->min == expectedMin, "schema entry range min should match");
    Require(entry.range->max == expectedMax, "schema entry range max should match");
}

void RequireBoolDefault(const Config::ConfigSchemaEntry& entry, bool expectedDefault) {
    Require(entry.uiType == Config::ConfigUiType::Bool, "schema entry should be Bool");
    Require(entry.defaultValue == Config::ConfigValue(expectedDefault), "schema entry default should match");
    Require(entry.currentValue == Config::ConfigValue(expectedDefault), "schema entry current value should match default");
    Require(!entry.range.has_value(), "bool schema entry should not expose a range");
}

void RequireIirEntry(const Config::ConfigSchemaSnapshot& schema,
                     std::string_view yamlPath,
                     int32_t expectedDefault,
                     double expectedMin,
                     double expectedMax) {
    const auto* entry = FindSchemaEntry(schema, yamlPath);
    Require(entry != nullptr, "schema should contain expected IIR entry");
    Require(entry->moduleTag == "Stylus / Coordinate", "IIR entry should be grouped in Stylus / Coordinate");
    Require(entry->boundToRuntime, "IIR entry should be bound to runtime");
    Require(!entry->description.empty(), "IIR entry should expose a description");
    RequireInt32Range(*entry, expectedDefault, expectedMin, expectedMax);
}

Config::ConfigSchemaSnapshot BuildStylusSchema() {
    Solvers::StylusPipeline pipeline;
    Config::ConfigBinder binder;
    pipeline.registerBindings(binder);
    Config::ConfigStore defaults;
    binder.writeDefaults(defaults);
    binder.apply(defaults);
    return binder.snapshot();
}

void TestStylusIirBindingsExposeCompleteUiSchema() {
    const auto schema = BuildStylusSchema();

    const auto* enabled = FindSchemaEntry(schema, "stylus.sp.iir_filter_enabled");
    Require(enabled != nullptr, "schema should contain IIR enable entry");
    Require(enabled->moduleTag == "Stylus / Coordinate", "IIR enable entry should be grouped in Stylus / Coordinate");
    Require(enabled->boundToRuntime, "IIR enable entry should be bound to runtime");
    RequireBoolDefault(*enabled, true);

    RequireIirEntry(schema, "stylus.sp.iir_coef_low_hover", 2, 0.0, 255.0);
    RequireIirEntry(schema, "stylus.sp.iir_coef_high_hover", 16, 0.0, 255.0);
    RequireIirEntry(schema, "stylus.sp.iir_speed_thold_hover", 20, 0.0, 255.0);
    RequireIirEntry(schema, "stylus.sp.iir_coef_low_writing", 6, 0.0, 255.0);
    RequireIirEntry(schema, "stylus.sp.iir_coef_high_writing", 18, 0.0, 255.0);
    RequireIirEntry(schema, "stylus.sp.iir_speed_thold_writing", 10, 0.0, 255.0);
    RequireIirEntry(schema, "stylus.sp.iir_speed_max", 205, 0.0, 1000.0);
    RequireIirEntry(schema, "stylus.sp.iir_max_coef", 32, 1.0, 255.0);
}

// The IIR keys are registered with bindSchema(), i.e. schema-only with no member
// binding, so SolversUnit_PipelineDefaultsConsistency cannot reach them: the binder has
// no getter to compare against, and applyConfig() converts them by hand, so nothing links
// the in-class initializers to the schema. Assert them directly, or a repeat
// of 8726a2b (which silently weakened writing-mode smoothing and caused jagged strokes)
// would again pass every test.
void TestCoorIirMemberInitializersMatchSchema() {
    const Solvers::Stylus::CoorIIRProcess iir;

    Require(iir.m_enabled, "IIR should be enabled by default");
    Require(iir.m_coefLowHover == 2, "m_coefLowHover must be 2 (asa[0xA5E])");
    Require(iir.m_coefHighHover == 16, "m_coefHighHover must be 16 (asa[0xA5F])");
    Require(iir.m_speedTholdHover == 20, "m_speedTholdHover must be 20 (0x14)");
    Require(iir.m_coefLowWriting == 6, "m_coefLowWriting must be 6 (asa[0xA5C])");
    Require(iir.m_coefHighWriting == 18, "m_coefHighWriting must be 18 (asa[0xA5D])");
    Require(iir.m_speedTholdWriting == 10, "m_speedTholdWriting must be 10 (0x0A)");
    Require(iir.m_speedMax == 205, "m_speedMax must be 205 (0xCD)");
    Require(iir.m_maxCoef == 32, "m_maxCoef must be 32 (asa[0xA60])");

    // A lower coefficient means more smoothing, so the low/high pair must not be
    // inverted: moving faster has to track more, never smooth more.
    Require(iir.m_coefLowHover < iir.m_coefHighHover,
            "hover coefficients inverted: faster motion must track more, not smooth more");
    Require(iir.m_coefLowWriting < iir.m_coefHighWriting,
            "writing coefficients inverted: faster motion must track more, not smooth more");
    Require(iir.m_speedTholdWriting < iir.m_speedMax,
            "speed threshold must sit below speed max or the interpolation band collapses");
}

// Pins the pen-down lock thresholds to the values carried by this device's project in
// the original TSAPrmt.dll. They previously held 1/2, which is byte-for-byte the Ags3
// project (a 32x48 panel) while the rest of the port targets Gaokun (60x40) — the two
// projects had been mixed. Verified by reading flash[0xA58..0xA5B] out of every
// g_tsaPrmtFlashAsaGaokunHimax* export.
void TestAftLockThresholdsMatchDeviceProject() {
    const Solvers::Stylus::AftCoorProcess aft;

    Require(aft.m_lockFlashEdgeX == 8, "m_lockFlashEdgeX must be 8 (flash[0xA58], Gaokun)");
    Require(aft.m_lockFlashEdgeY == 16, "m_lockFlashEdgeY must be 16 (flash[0xA59], Gaokun)");
    Require(aft.m_lockFlashInBandX == 0, "m_lockFlashInBandX must be 0 (flash[0xA5A])");
    Require(aft.m_lockFlashInBandY == 0, "m_lockFlashInBandY must be 0 (flash[0xA5B])");

    // Sensor geometry is the fingerprint that identifies the project: Gaokun is 60x40,
    // Ags3 is 32x48. If these ever change, the flash values above no longer apply.
    Require(aft.m_sensorTxCount == 60, "bTxCount must be 60 for this device");
    Require(aft.m_sensorRxCount == 40, "bRxCount must be 40 for this device");

    // Physical span per axis, NOT display resolution. ScaleLockThreshold divides by
    // these, so 2560/1600 (the panel's pixel resolution) inflated every lock threshold.
    Require(aft.m_sensorDim1 == 4320, "TX span must be 4320 (asaPrmt @0xA2C)");
    Require(aft.m_sensorDim2 == 2640, "RX span must be 2640 (asaPrmt @0xA2A)");

    // The span/count pitch must stay physically plausible: a real sensor grid is close
    // to square. Screen-resolution values fail this by a wide margin.
    const double pitchTx = static_cast<double>(aft.m_sensorDim1) / aft.m_sensorTxCount;
    const double pitchRx = static_cast<double>(aft.m_sensorDim2) / aft.m_sensorRxCount;
    const double dev = (pitchTx > pitchRx ? pitchTx - pitchRx : pitchRx - pitchTx) /
                       (pitchTx > pitchRx ? pitchTx : pitchRx);
    Require(dev < 0.15, "TX/RX pitch diverge too far — sensor span values look wrong");
}

} // namespace

int main() {
    try {
        TestStylusIirBindingsExposeCompleteUiSchema();
        TestCoorIirMemberInitializersMatchSchema();
        TestAftLockThresholdsMatchDeviceProject();
        std::cout << "[TEST] Stylus pipeline config schema tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
