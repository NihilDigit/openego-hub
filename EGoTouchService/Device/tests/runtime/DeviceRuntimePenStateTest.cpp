#include "penevt/PenEventBridge.h"
#include "runtime/DeviceRuntime.h"
#include "TestRequire.h"

#include <array>
#include <cstdint>
#include <iostream>

struct DeviceRuntimePenStateTestAccess {
    static Device::BtPenSample BtSample(DeviceRuntime& runtime) {
        Device::BtPenSample sample{};
        runtime.m_btPenLatch.Snapshot(sample);
        return sample;
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
                state.protocolHint == StylusProtocolHint::Hpp3,
            "PenModule should update the protocol hint");

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

    auto connected = MakePayloadEvent(PenUsbEventCode::PenConnStatus, 1);
    connected.semantic.hasConnection = true;
    connected.semantic.connected = true;
    runtime.IngestPenEvent(connected);

    state = runtime.GetPenStateSnapshot();
    Require(state.hasConnection && state.connected,
            "PenConnStatus should be the event that establishes connection");
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

// 配对状态是诊断量，不是笔身份。它变化时不能连带清掉蓝牙压力锁存——上一帧的压力还要
// 喂给这一帧的后端，清了就是一次落笔中途掉压。
void TestPairStatusDoesNotClearBtSample() {
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

    // 笔尖压力经蓝牙送达，锁存在这里；配对状态事件随后到达，不应该动它。
    const std::array<uint16_t, 4> pressure{100, 200, 300, 400};
    const std::array<uint16_t, 4> rawPressure{1000, 2000, 3000, 4000};
    runtime.IngestBtMcuPressurePacket(pressure, rawPressure, 0x12, 0x34);

    const auto penBefore = runtime.GetPenStateSnapshot();
    const auto btBefore = DeviceRuntimePenStateTestAccess::BtSample(runtime);
    Require(btBefore.hasSample && btBefore.hasFreq,
            "test precondition should latch the BT pressure packet");

    auto pairStatus = MakePayloadEvent(PenUsbEventCode::DevPairStatus, 9);
    pairStatus.semantic.hasPairStatus = true;
    pairStatus.semantic.pairStatus = 9;
    runtime.IngestPenEvent(pairStatus);

    const auto penAfter = runtime.GetPenStateSnapshot();
    const auto btAfter = DeviceRuntimePenStateTestAccess::BtSample(runtime);
    Require(penAfter.penRevision == penBefore.penRevision + 1,
            "pair status should remain visible through diagnostic revision");
    // pipelineRevision 仍是「笔身份变了」的判据：它前进才会让运行时清空蓝牙锁存。
    Require(penAfter.pipelineRevision == penBefore.pipelineRevision,
            "pair status should not advance the pen session generation");
    Require(btAfter.hasSample &&
                btAfter.seq == btBefore.seq &&
                btAfter.pressure == btBefore.pressure &&
                btAfter.rawPressure == btBefore.rawPressure &&
                btAfter.freq1 == btBefore.freq1 &&
                btAfter.freq2 == btBefore.freq2,
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

// 抑制状态位仍然要能翻转并被读回：托盘的 ScopedOneNoteInputSuppression 阻塞等待它经
// PenStatusChannel 变化。实际的输入闸门已经不在这一侧，这里只覆盖状态位本身。
void TestOneNoteInputSuppressionStateIsObservable() {
    auto runtime = MakeRuntime();
    Require(!runtime.IsInputSuppressed(),
            "input is not suppressed by default");

    runtime.SetInputSuppressed(true);
    Require(runtime.IsInputSuppressed(), "suppression state turns on");

    runtime.SetInputSuppressed(false);
    Require(!runtime.IsInputSuppressed(), "suppression state turns off");
}

} // namespace

int main() {
    try {
        TestIdentityEventsDoNotInferConnection();
        TestDuplicateDisconnectClearsIdentityObservably();
        TestPairStatusIsIndependentFromConnection();
        TestPairStatusDoesNotClearBtSample();
        TestPairStatusFrameProducesSemanticState();
        TestToggleEraserPublishesStateBeforeNotifyingUserSession();
        TestOneNoteInputSuppressionStateIsObservable();
        std::cout << "[TEST] DeviceRuntime pen state tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << '\n';
        return 1;
    }
}
