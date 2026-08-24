#include "penevt/PenEventBridge.h"
#include "runtime/DeviceRuntime.h"
#include "TestRequire.h"

#include <array>
#include <cstdint>
#include <iostream>

struct DeviceRuntimePenStateTestAccess {
    struct PipelineState {
        Solvers::StylusPenSession session{};
        Asa::BtInputSnapshot btSample{};
        bool lastFrameWasTerminal = true;
    };

    static bool Process(DeviceRuntime& runtime, Solvers::HeatmapFrame& frame) {
        std::lock_guard<std::mutex> lock(runtime.m_pipelineMu);
        return runtime.m_stylusPipeline.Process(frame);
    }

    static PipelineState Snapshot(DeviceRuntime& runtime) {
        std::lock_guard<std::mutex> pipelineLock(runtime.m_pipelineMu);
        std::lock_guard<std::mutex> btLock(runtime.m_stylusPipeline.m_btMutex);
        return {
            runtime.m_stylusPipeline.m_penSession,
            runtime.m_stylusPipeline.m_btSample,
            runtime.m_stylusPipeline.m_lastFrameWasTerminal,
        };
    }

    static void DispatchDoubleClick(DeviceRuntime& runtime) {
        runtime.DispatchPenButtonAction(
            {PenButtonAction::Type::DoubleClick, true, 0}, "TestDoubleClick");
    }
};

namespace {

using DeviceTests::Require;
using Himax::Pen::PenEvent;
using Himax::Pen::PenUsbEventCode;

DeviceRuntime MakeRuntime() {
    return DeviceRuntime(
        L"\\\\?\\EGoTouchMissingMaster",
        L"\\\\?\\EGoTouchMissingSlave",
        L"\\\\?\\EGoTouchMissingInterrupt");
}

PenEvent MakePayloadEvent(PenUsbEventCode code, uint8_t payload) {
    PenEvent event{};
    event.code = code;
    const std::array<uint8_t, 1> bytes{payload};
    (void)event.payload.assign(bytes);
    return event;
}

// Feeds the parser through its slave-suffix path: one 9x9 block per TX with a
// valid anchor and a single strong cell, which is the least the coordinate
// solver needs to produce a peak on both axes.
Solvers::HeatmapFrame MakeWritingFrame() {
    Solvers::HeatmapFrame frame{};
    frame.slaveSuffixValid = true;

    const auto fillBlock = [&](int base, int peakRow, int peakCol) {
        frame.slaveSuffix.words[base + 0] = 4;   // anchorRow
        frame.slaveSuffix.words[base + 1] = 4;   // anchorCol
        for (int r = 0; r < Frame::kGridDim; ++r) {
            for (int c = 0; c < Frame::kGridDim; ++c) {
                frame.slaveSuffix.words[base + 2 + r * Frame::kGridDim + c] = 200;
            }
        }
        frame.slaveSuffix.words[base + 2 + peakRow * Frame::kGridDim + peakCol] = 3000;
    };
    fillBlock(0, 4, 4);
    fillBlock(Frame::kBlockWords, 4, 4);
    return frame;
}

void TestIdentityEventsDoNotInferConnection() {
    auto runtime = MakeRuntime();

    auto module = MakePayloadEvent(PenUsbEventCode::PenModule, 0);
    module.semantic.hasPenModuleModelId = true;
    module.semantic.penModuleModelId = Himax::Pen::kPenModuleModelIdCd54S;
    module.semantic.penModuleModel = Himax::Pen::PenModuleModel::Cd54S;
    module.semantic.hasPenModuleProtocolHint = true;
    module.semantic.penModuleProtocolHint = Himax::Pen::PenModuleProtocolHint::Hpp3;
    runtime.IngestPenEvent(module);

    auto state = runtime.GetPenStateSnapshot();
    Require(!state.hasConnection && !state.connected,
            "PenModule must not infer a connection state");
    Require(state.hasPenModuleModelId &&
                state.penModuleModelId == Himax::Pen::kPenModuleModelIdCd54S,
            "PenModule should update model identity");
    Require(state.hasStylusId && state.stylusId == 2,
            "PenModule should retain model-derived stylus ID behavior");
    Require(state.protocolHintFromPenModule &&
                state.protocolHint == Solvers::StylusProtocolHint::Hpp3,
            "PenModule should update the protocol hint");
    Require(runtime.GetSnapshot().queue_depth == 0,
            "PenModule must not submit AFE work before the runtime command gate opens");

    auto serial = MakePayloadEvent(PenUsbEventCode::PenSerialNumber, 0);
    serial.semantic.hasSerialNumber = true;
    serial.semantic.serialNumber = "SERIAL-42";
    runtime.IngestPenEvent(serial);

    auto hardware = MakePayloadEvent(PenUsbEventCode::PenHardwareVersion, 0);
    hardware.semantic.hasHardwareVersion = true;
    hardware.semantic.hardwareVersion = "HW-1.2";
    runtime.IngestPenEvent(hardware);

    auto firmware = MakePayloadEvent(PenUsbEventCode::UsbdSwVersion, 0);
    firmware.semantic.hasFirmwareVersion = true;
    firmware.semantic.firmwareVersion = "CD54S 1.0.0.122";
    runtime.IngestPenEvent(firmware);

    auto penType = MakePayloadEvent(PenUsbEventCode::PenTypeInfo, 3);
    penType.semantic.hasStylusId = true;
    penType.semantic.stylusId = 3;
    runtime.IngestPenEvent(penType);

    state = runtime.GetPenStateSnapshot();
    Require(!state.hasConnection && !state.connected,
            "version and PenTypeInfo events must not infer connection");
    Require(state.hasSerialNumber && state.serialNumber == "SERIAL-42",
            "serial identity should update without connection inference");
    Require(state.hasHardwareVersion && state.hardwareVersion == "HW-1.2",
            "hardware version should update without connection inference");
    Require(state.hasFirmwareVersion && state.firmwareVersion == "CD54S 1.0.0.122",
            "firmware version should update without connection inference");
    Require(state.hasStylusId && state.stylusId == 3,
            "PenTypeInfo should update stylus ID");
    Require(state.penRevision == 5,
            "each distinct identity update should advance pen revision once");
    Require(runtime.GetSnapshot().queue_depth == 0,
            "identity updates must not bypass the closed runtime command gate");

    auto connected = MakePayloadEvent(PenUsbEventCode::PenConnStatus, 1);
    connected.semantic.hasConnection = true;
    connected.semantic.connected = true;
    runtime.IngestPenEvent(connected);

    state = runtime.GetPenStateSnapshot();
    Require(state.hasConnection && state.connected,
            "PenConnStatus should be the event that establishes connection");
    Require(runtime.GetSnapshot().queue_depth == 0,
            "connected PenConnStatus must not bypass the closed runtime command gate");
}

void TestDuplicateDisconnectClearsIdentityObservably() {
    auto runtime = MakeRuntime();

    auto disconnected = MakePayloadEvent(PenUsbEventCode::PenConnStatus, 0);
    disconnected.semantic.hasConnection = true;
    disconnected.semantic.connected = false;
    runtime.IngestPenEvent(disconnected);

    auto serial = MakePayloadEvent(PenUsbEventCode::PenSerialNumber, 0);
    serial.semantic.hasSerialNumber = true;
    serial.semantic.serialNumber = "STALE-SERIAL";
    runtime.IngestPenEvent(serial);

    auto pairStatus = MakePayloadEvent(PenUsbEventCode::DevPairStatus, 7);
    pairStatus.semantic.hasPairStatus = true;
    pairStatus.semantic.pairStatus = 7;
    runtime.IngestPenEvent(pairStatus);

    auto state = runtime.GetPenStateSnapshot();
    Require(state.penRevision == 3 && state.hasSerialNumber,
            "disconnected identity update should remain observable until status refresh");

    runtime.IngestPenEvent(disconnected);
    state = runtime.GetPenStateSnapshot();
    Require(state.hasConnection && !state.connected,
            "duplicate disconnected status should preserve explicit connection state");
    Require(!state.hasSerialNumber && state.serialNumber.empty(),
            "duplicate disconnected status should clear stale identity");
    Require(state.hasPairStatus && state.pairStatus == 7,
            "disconnect identity cleanup should preserve independent pair status");
    Require(state.penRevision == 4,
            "identity cleanup on duplicate disconnect should advance revision");
}

void TestPairStatusIsIndependentFromConnection() {
    auto runtime = MakeRuntime();

    auto pairStatus = MakePayloadEvent(PenUsbEventCode::DevPairStatus, 2);
    pairStatus.semantic.hasPairStatus = true;
    pairStatus.semantic.pairStatus = 2;
    runtime.IngestPenEvent(pairStatus);

    auto state = runtime.GetPenStateSnapshot();
    Require(state.hasPairStatus && state.pairStatus == 2,
            "DevPairStatus should update independent pair state");
    Require(!state.hasConnection && !state.connected,
            "DevPairStatus must not fabricate connection");
    Require(state.penRevision == 1,
            "first pair status should advance pen revision");
    Require(runtime.GetSnapshot().queue_depth == 0,
            "DevPairStatus must not enqueue an AFE connection command");

    runtime.IngestPenEvent(pairStatus);
    Require(runtime.GetPenStateSnapshot().penRevision == 1,
            "duplicate pair status should not advance revision");

    pairStatus.payload.bytes[0] = 3;
    pairStatus.semantic.pairStatus = 3;
    runtime.IngestPenEvent(pairStatus);
    state = runtime.GetPenStateSnapshot();
    Require(state.hasPairStatus && state.pairStatus == 3 && state.penRevision == 2,
            "changed pair status should update state and revision");
    Require(state.pipelineRevision == 0,
            "pair-only updates should not advance stylus pipeline generation");
}

void TestPairStatusDuringWritingPreservesPipelineAndBtSample() {
    auto runtime = MakeRuntime();

    auto connected = MakePayloadEvent(PenUsbEventCode::PenConnStatus, 1);
    connected.semantic.hasConnection = true;
    connected.semantic.connected = true;
    runtime.IngestPenEvent(connected);

    auto module = MakePayloadEvent(PenUsbEventCode::PenModule, 0);
    module.semantic.hasPenModuleModelId = true;
    module.semantic.penModuleModelId = Himax::Pen::kPenModuleModelIdCd54S;
    module.semantic.penModuleModel = Himax::Pen::PenModuleModel::Cd54S;
    module.semantic.hasPenModuleProtocolHint = true;
    module.semantic.penModuleProtocolHint = Himax::Pen::PenModuleProtocolHint::Hpp3;
    runtime.IngestPenEvent(module);

    // Tip pressure arrives over BT, so it has to be in place before the frame
    // that is supposed to read as writing.
    const std::array<uint16_t, 4> pressure{100, 200, 300, 400};
    const std::array<uint16_t, 4> rawPressure{1000, 2000, 3000, 4000};
    runtime.IngestBtMcuPressurePacket(pressure, rawPressure, 0x12, 0x34);

    auto writingFrame = MakeWritingFrame();
    Require(DeviceRuntimePenStateTestAccess::Process(runtime, writingFrame),
            "connected writing frame should process");
    Require(writingFrame.stylus.output.valid && writingFrame.stylus.output.tipDown,
            "test precondition should establish active writing state");

    const auto penBefore = runtime.GetPenStateSnapshot();
    const auto pipelineBefore = DeviceRuntimePenStateTestAccess::Snapshot(runtime);
    Require(!pipelineBefore.lastFrameWasTerminal && pipelineBefore.btSample.hasSample,
            "test precondition should retain writing state and BT sample");

    auto pairStatus = MakePayloadEvent(PenUsbEventCode::DevPairStatus, 9);
    pairStatus.semantic.hasPairStatus = true;
    pairStatus.semantic.pairStatus = 9;
    runtime.IngestPenEvent(pairStatus);

    const auto penAfter = runtime.GetPenStateSnapshot();
    const auto pipelineAfter = DeviceRuntimePenStateTestAccess::Snapshot(runtime);
    Require(penAfter.penRevision == penBefore.penRevision + 1,
            "pair status should remain visible through diagnostic revision");
    Require(penAfter.pipelineRevision == penBefore.pipelineRevision,
            "pair status should not advance stylus pipeline generation");
    Require(pipelineAfter.session.revision == pipelineBefore.session.revision,
            "pair status should not publish a new stylus session");
    Require(!pipelineAfter.lastFrameWasTerminal,
            "pair status should not reset active writing stages");
    Require(pipelineAfter.btSample.hasSample &&
                pipelineAfter.btSample.seq == pipelineBefore.btSample.seq &&
                pipelineAfter.btSample.pressure == pipelineBefore.btSample.pressure &&
                pipelineAfter.btSample.rawPressure == pipelineBefore.btSample.rawPressure,
            "pair status should not clear the current BT pressure sample");
}

class TestPenEventBridge final : public Himax::Pen::PenEventBridge {
public:
    using PenEventBridge::OnPacketReceived;
};

void TestPairStatusFrameProducesSemanticState() {
    TestPenEventBridge bridge;
    PenEvent received{};
    bool called = false;
    bridge.SetEventCallback([&](const PenEvent& event) {
        received = event;
        called = true;
    });

    const std::array<uint8_t, 9> packet{
        0x00, 0x00, 0x07, 0x00,
        0x01, 0x12, 0x00, 0x01,
        0xA5,
    };
    bridge.OnPacketReceived(packet);

    Require(called, "DevPairStatus frame should be dispatched");
    Require(received.code == PenUsbEventCode::DevPairStatus,
            "0x12 frame should retain DevPairStatus code");
    Require(received.semantic.hasPairStatus && received.semantic.pairStatus == 0xA5,
            "0x12 payload should populate pair status semantics");
    Require(!received.semantic.hasConnection,
            "0x12 semantic parsing must not fabricate connection");
}

void TestToggleEraserPublishesStateBeforeNotifyingUserSession() {
    auto runtime = MakeRuntime();
    runtime.SetPenButtonMode(PenButtonMode::ToggleEraser);

    int notifications = 0;
    bool stateSeenByFirstNotification = false;
    bool stateSeenBySecondNotification = true;
    runtime.SetPenDoubleClickCallback([&] {
        ++notifications;
        const auto state = runtime.GetPenStateSnapshot();
        if (notifications == 1) {
            stateSeenByFirstNotification =
                state.hasEraserToggle && state.eraserToggle == 1;
        } else if (notifications == 2) {
            stateSeenBySecondNotification =
                state.hasEraserToggle && state.eraserToggle != 0;
        }
        return true;
    });

    DeviceRuntimePenStateTestAccess::DispatchDoubleClick(runtime);
    DeviceRuntimePenStateTestAccess::DispatchDoubleClick(runtime);

    Require(notifications == 2,
            "ToggleEraser should notify the user-session companion on every edge");
    Require(stateSeenByFirstNotification,
            "the enable state must be published before the first notification");
    Require(!stateSeenBySecondNotification,
            "the disable state must be published before the second notification");
}

void TestOneNoteInputSuppressionIsIndependentFromVhfEnabled() {
    auto runtime = MakeRuntime();
    Require(runtime.IsVhfEnabled(), "test precondition: VHF is enabled");
    Require(!runtime.IsInputSuppressed(),
            "input is not suppressed by default");

    runtime.SetInputSuppressed(true);
    Require(runtime.IsInputSuppressed(), "suppression gate turns on");
    Require(runtime.IsVhfEnabled(),
            "short suppression must not rewrite the persistent VHF setting");

    runtime.SetInputSuppressed(false);
    Require(!runtime.IsInputSuppressed(), "suppression gate turns off");
    Require(runtime.IsVhfEnabled(), "VHF remains enabled after suppression");
}

} // namespace

int main() {
    try {
        TestIdentityEventsDoNotInferConnection();
        TestDuplicateDisconnectClearsIdentityObservably();
        TestPairStatusIsIndependentFromConnection();
        TestPairStatusDuringWritingPreservesPipelineAndBtSample();
        TestPairStatusFrameProducesSemanticState();
        TestToggleEraserPublishesStateBeforeNotifyingUserSession();
        TestOneNoteInputSuppressionIsIndependentFromVhfEnabled();
        std::cout << "[TEST] DeviceRuntime pen state tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << '\n';
        return 1;
    }
}
