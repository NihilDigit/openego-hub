#include "vhf/VhfReporterStylusPacketHelper.h"
#include "TestRequire.h"

#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using DeviceTests::Require;
using Solvers::HeatmapFrame;

uint16_t ReadU16Le(const Solvers::StylusPacket& packet, size_t offset) {
    return static_cast<uint16_t>(
        static_cast<uint16_t>(packet.bytes[offset]) |
        (static_cast<uint16_t>(packet.bytes[offset + 1]) << 8));
}

int16_t ReadI16Le(const Solvers::StylusPacket& packet, size_t offset) {
    return static_cast<int16_t>(ReadU16Le(packet, offset));
}

HeatmapFrame MakeOutputDrivenStylusFrame() {
    HeatmapFrame frame{};

    frame.stylus.output.valid = true;
    frame.stylus.output.inRange = true;
    frame.stylus.output.tipDown = true;
    frame.stylus.output.pressure = 321;
    frame.stylus.output.point.valid = true;
    frame.stylus.output.point.x = 12.0f * 1024.0f;
    frame.stylus.output.point.y = 18.0f * 1024.0f;
    frame.stylus.output.point.tiltX = 7;
    frame.stylus.output.point.tiltY = -3;
    frame.stylus.output.point.pressure = frame.stylus.output.pressure;
    return frame;
}

HeatmapFrame MakeInvalidOutputFrameWithStaleLegacyValidData() {
    HeatmapFrame frame{};
    return frame;
}

VhfStylusPacket::Config MakeDefaultStylusPacketConfig() {
    VhfStylusPacket::Config config;
    config.sensorRows = 40;
    config.sensorCols = 60;
    config.emitWhenInvalid = true;
    return config;
}

void TestStylusPacketHelperBuildsValidPacketFromOutput() {
    auto frame = MakeOutputDrivenStylusFrame();
    const auto packet = VhfStylusPacket::Build(
        frame.stylus, MakeDefaultStylusPacketConfig());

    Require(packet.valid,
            "valid output should build a stylus packet");
    Require(packet.length == 13,
            "VHF-built stylus packet should use 13-byte report length");
    Require(packet.bytes[0] == 0x08,
            "VHF-built stylus packet should keep stylus report id");
    Require(packet.bytes[1] == 0x21,
            "VHF-built valid stylus packet should encode TipSwitch and InRange");
    Require(ReadU16Le(packet, 3) == 7200,
            "VHF-built stylus packet should map output Y into HID X");
    Require(ReadU16Le(packet, 5) == 20480,
            "VHF-built stylus packet should map output X into HID Y");
    Require(ReadU16Le(packet, 7) == 321,
            "VHF-built stylus packet should encode output pressure");
    // 描述符 logical ±9000 的单位是百分之一度;轴映射与坐标一致(HID X 对应
    // 面板 dim2、HID Y 为反向 dim1),符号推导见 VhfReporterStylusPacketHelper.h。
    Require(ReadI16Le(packet, 9) == 300,
            "VHF-built stylus packet should encode HID X tilt as -tiltY centidegrees");
    Require(ReadI16Le(packet, 11) == 700,
            "VHF-built stylus packet should encode HID Y tilt as +tiltX centidegrees");
    Require(VhfStylusPacket::ExtractPenState(packet) == 0x21,
            "helper pen state extraction should match raw packet");
}

void TestStylusPacketHelperBuildsInvalidZeroStatePacketFromInvalidOutputWhenEnabled() {
    auto frame = MakeInvalidOutputFrameWithStaleLegacyValidData();
    auto config = MakeDefaultStylusPacketConfig();
    config.emitWhenInvalid = true;
    const auto packet = VhfStylusPacket::Build(frame.stylus, config);

    Require(packet.valid,
            "invalid output should still build a packet when emitWhenInvalid is enabled");
    Require(packet.length == 13,
            "invalid output should build a 13-byte packet");
    Require(packet.bytes[1] == 0,
            "invalid output should clear stylus state bits");
    Require(ReadU16Le(packet, 3) == 0,
            "invalid output should clear HID X");
    Require(ReadU16Le(packet, 5) == 0,
            "invalid output should clear HID Y");
    Require(ReadU16Le(packet, 7) == 0,
            "invalid output should clear pressure");
    Require(VhfStylusPacket::ExtractPenState(packet) == 0,
            "invalid output should decode neutral pen state");
}

void TestStylusPacketHelperSuppressesInvalidPacketFromInvalidOutputWhenDisabled() {
    auto frame = MakeInvalidOutputFrameWithStaleLegacyValidData();
    auto config = MakeDefaultStylusPacketConfig();
    config.emitWhenInvalid = false;
    const auto packet = VhfStylusPacket::Build(frame.stylus, config);

    Require(!packet.valid,
            "invalid output should stay suppressed when emitWhenInvalid is disabled");
    Require(packet.length == 13,
            "suppressed invalid packet should preserve HID report length");
    Require(VhfStylusPacket::ExtractPenState(packet) == 0,
            "suppressed invalid packet should decode neutral pen state");
}

void TestBarrelButtonAndInRangeBits() {
    auto frame = MakeOutputDrivenStylusFrame();
    auto config = MakeDefaultStylusPacketConfig();
    config.barrelButton = true;

    auto packet = VhfStylusPacket::Build(frame.stylus, config);
    Require(packet.bytes[1] == 0x23,
            "barrel button should add barrel switch bit without clearing tip/in-range bits");

    frame.stylus.output.tipDown = false;
    frame.stylus.output.inRange = false;
    packet = VhfStylusPacket::Build(frame.stylus, config);
    Require(packet.valid, "point-valid output should still produce a report when out of range");
    Require(packet.bytes[1] == 0x02,
            "out-of-range point should clear InRange and TipSwitch while preserving barrel bit");
}

void TestPressureCoordinateAndTiltClamps() {
    auto frame = MakeOutputDrivenStylusFrame();
    frame.stylus.output.pressure = 9000;
    frame.stylus.output.point.x = -10.0f * 1024.0f;
    frame.stylus.output.point.y = 99.0f * 1024.0f;
    frame.stylus.output.point.tiltX = 12000;
    frame.stylus.output.point.tiltY = 12000;

    const auto packet = VhfStylusPacket::Build(frame.stylus, MakeDefaultStylusPacketConfig());
    Require(ReadU16Le(packet, 3) == 16000,
            "Y beyond active rows should clamp HID X to max");
    Require(ReadU16Le(packet, 5) == 25600,
            "negative X should clamp HID Y to max because X is inverted");
    Require(ReadU16Le(packet, 7) == 4095,
            "pressure should clamp to HID max 4095");
    Require(ReadI16Le(packet, 9) == -9000,
            "positive tiltY should clamp HID X tilt to -9000 after sign flip");
    Require(ReadI16Le(packet, 11) == 9000,
            "positive tiltX should clamp HID Y tilt to +9000");
}

Solvers::StylusPacket MakeRawStylusPacket(uint8_t penState, uint16_t pressure = 0) {
    Solvers::StylusPacket packet{};
    packet.valid = true;
    packet.reportId = 0x08;
    packet.length = 13;
    packet.bytes[0] = packet.reportId;
    packet.bytes[1] = penState;
    packet.bytes[3] = 0x34;
    packet.bytes[4] = 0x12;
    packet.bytes[5] = 0x78;
    packet.bytes[6] = 0x56;
    packet.bytes[7] = static_cast<uint8_t>(pressure & 0xFFu);
    packet.bytes[8] = static_cast<uint8_t>((pressure >> 8) & 0xFFu);
    return packet;
}

void TestEraserHoverAndContactUseValidSwitchCombinations() {
    const auto hover = VhfStylusPacket::ApplyEraserToolState(
        MakeRawStylusPacket(0x20), true, false);
    Require(hover.prependOutOfRange,
            "switching from pen hover to eraser hover must prepend out-of-range");
    Require(hover.transitionBytes[1] == 0,
            "tool transition report must clear all pen switches");
    Require(hover.bytes[1] == 0x24,
            "eraser hover must report InRange+Invert without Eraser");
    Require(hover.eraserApplied,
            "eraser mode should become applied after the transition");

    const auto contact = VhfStylusPacket::ApplyEraserToolState(
        MakeRawStylusPacket(0x21, 321), true, hover.eraserApplied);
    Require(!contact.prependOutOfRange,
            "hover-to-contact within eraser mode must not leave range");
    Require(contact.bytes[1] == 0x28,
            "button eraser contact must report InRange+Eraser without Tip or Invert");
    Require(contact.bytes[7] == 0x41 && contact.bytes[8] == 0x01,
            "eraser contact should preserve measured pressure");
}

void TestToolSwitchIsDeferredUntilContactEnds() {
    const auto contact = VhfStylusPacket::ApplyEraserToolState(
        MakeRawStylusPacket(0x21, 123), true, false);
    Require(!contact.prependOutOfRange,
            "a tool switch requested during contact must not inject a transition");
    Require(!contact.eraserApplied && contact.bytes[1] == 0x21,
            "a tool switch requested during contact must keep the current pen tool");

    const auto lifted = VhfStylusPacket::ApplyEraserToolState(
        MakeRawStylusPacket(0x20), true, contact.eraserApplied);
    Require(lifted.prependOutOfRange,
            "the deferred tool switch should apply on the first hover frame");
    Require(lifted.bytes[1] == 0x24 && lifted.eraserApplied,
            "lifting after a deferred switch should enter eraser hover");
}

void TestSwitchingBackToPenAlsoTraversesOutOfRange() {
    const auto pen = VhfStylusPacket::ApplyEraserToolState(
        MakeRawStylusPacket(0x20), false, true);
    Require(pen.prependOutOfRange,
            "switching from eraser hover to pen hover must prepend out-of-range");
    Require(pen.transitionBytes[1] == 0 && pen.bytes[1] == 0x20,
            "eraser-to-pen transition must be neutral before normal pen hover");
    Require(!pen.eraserApplied,
            "pen mode should become applied after the reverse transition");
}

void TestOutOfRangeAppliesRequestedToolWithoutSyntheticTransition() {
    const auto update = VhfStylusPacket::ApplyEraserToolState(
        MakeRawStylusPacket(0x00), true, false);
    Require(!update.prependOutOfRange,
            "an already out-of-range pen does not need another transition report");
    Require(update.bytes[1] == 0 && update.eraserApplied,
            "out-of-range state stays neutral while arming eraser for the next hover");
}

} // namespace

int main() {
    try {
        TestStylusPacketHelperBuildsValidPacketFromOutput();
        TestStylusPacketHelperBuildsInvalidZeroStatePacketFromInvalidOutputWhenEnabled();
        TestStylusPacketHelperSuppressesInvalidPacketFromInvalidOutputWhenDisabled();
        TestBarrelButtonAndInRangeBits();
        TestPressureCoordinateAndTiltClamps();
        TestEraserHoverAndContactUseValidSwitchCombinations();
        TestToolSwitchIsDeferredUntilContactEnds();
        TestSwitchingBackToPenAlsoTraversesOutOfRange();
        TestOutOfRangeAppliesRequestedToolWithoutSyntheticTransition();
        std::cout << "[TEST] Device VHF reporter stylus packet tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
