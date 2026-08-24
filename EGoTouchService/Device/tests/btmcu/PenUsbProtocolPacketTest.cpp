#include "btmcu/PenUsbPacketBuilder.h"
#include "btmcu/PenUsbTypes.h"
#include "penevt/PenEventBridge.h"
#include "TestRequire.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using DeviceTests::Require;

void TestValidFactoryEventFrameParses() {
    const std::array<uint8_t, 12> packet{
        0x00, 0x00, 0x07, 0x00,
        0x01, 0x71, 0x00, 0x04,
        0x01, 0xAA, 0xBB, 0xCC,
    };

    auto parsed = Himax::Pen::TryParsePenUsbEventFrame(packet);
    Require(parsed.has_value(), "valid factory event frame should parse");
    Require(parsed->eventCode == 0x71, "event code should come from packet[5]");
    Require(parsed->payload.size() == 4, "payload should start at packet[8]");
    Require(parsed->payload[0] == 0x01, "payload[0] should preserve packet[8]");
}

void TestMinimumFactoryEventFrameParses() {
    const std::array<uint8_t, 9> packet{
        0x00, 0x00, 0x07, 0x00,
        0x01, 0x6F, 0x00, 0x01,
        0xEE,
    };

    auto parsed = Himax::Pen::TryParsePenUsbEventFrame(packet);
    Require(parsed.has_value(), "minimum 9-byte factory event frame should parse");
    Require(parsed->eventCode == 0x6F, "minimum frame event code should be preserved");
    Require(parsed->payload.size() == 1, "minimum frame should expose one payload byte");
    Require(parsed->payload[0] == 0xEE, "minimum frame payload should start at packet[8]");
}

void TestHardwareVersionEventFrameParses() {
    const std::array<uint8_t, 14> packet{
        0x00, 0x00, 0x07, 0x00,
        0x01, 0x02, 0x11, 0x05,
        'H', 'W', '1', '.', '2', 0x00,
    };

    auto parsed = Himax::Pen::TryParsePenUsbEventFrame(packet);
    Require(parsed.has_value(), "hardware version frame should parse");
    Require(parsed->eventCode == 0x02, "hardware version event code should be 0x02");
    Require(parsed->payload.size() == 5, "hardware version payload length should come from packet[7]");
    Require(parsed->payload[0] == 'H', "hardware version payload[0] should preserve packet[8]");
}

void TestInvalidFactoryEventFramesAreRejected() {
    const std::array<uint8_t, 7> shortPacket{
        0x00, 0x00, 0x07, 0x00,
        0x01, 0x71, 0x00,
    };
    Require(!Himax::Pen::TryParsePenUsbEventFrame(shortPacket).has_value(),
            "short packet should be rejected");

    // 原厂 PenService.dll 的读线程只校验 packet[4]，不看 packet[2]。这条钉住这个刻意的放宽：
    // 收紧回去会丢弃原厂会接受的帧，而键盘共用同一端点之后这种帧是真实存在的。
    // 与 TestValidFactoryEventFrameParses 的帧逐字节相同，只把 packet[2] 从 0x07 换成 0x02。
    std::array<uint8_t, 12> otherByteTwo{
        0x00, 0x00, 0x02, 0x00,
        0x01, 0x71, 0x00, 0x04,
        0x01, 0xAA, 0xBB, 0xCC,
    };
    Require(Himax::Pen::TryParsePenUsbEventFrame(otherByteTwo).has_value(),
            "packet[2] must not gate parsing; only the subsystem ID at packet[4] does");

    std::array<uint8_t, 9> wrongCommandLow{
        0x00, 0x00, 0x07, 0x00,
        0x00, 0x71, 0x00, 0x00,
        0x01,
    };
    Require(!Himax::Pen::TryParsePenUsbEventFrame(wrongCommandLow).has_value(),
            "packet with wrong packet[4] command low byte should be rejected");

    std::array<uint8_t, 9> truncatedPayload{
        0x00, 0x00, 0x07, 0x00,
        0x01, 0x71, 0x00, 0x02,
        0x01,
    };
    Require(!Himax::Pen::TryParsePenUsbEventFrame(truncatedPayload).has_value(),
            "packet whose payload length exceeds bytesRead should be rejected");

    std::array<uint8_t, 9> validBytesAtWrongOffsets{
        0x07, 0x01, 0x00, 0x00,
        0x00, 0x71, 0x07, 0x01,
        0x01,
    };
    Require(!Himax::Pen::TryParsePenUsbEventFrame(validBytesAtWrongOffsets).has_value(),
            "parser must key on the subsystem ID at packet[4], not on bytes that merely look right elsewhere");
}

void TestMalformedKnownEventsAreRejectedBeforeDispatch() {
    constexpr std::array<uint8_t, 27> eventsRequiringPayload{
        0x00, 0x01, 0x02, 0x03, 0x08, 0x09, 0x10,
        0x12, 0x21, 0x23, 0x27, 0x2C, 0x2E, 0x2F,
        0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76,
        0x77, 0x78, 0x79, 0x7B, 0x7C, 0x7F,
    };

    for (const uint8_t eventCode : eventsRequiringPayload) {
        const std::array<uint8_t, 8> packet{
            0x00, 0x00, 0x07, 0x00,
            0x01, eventCode, 0x00, 0x00,
        };
        Require(!Himax::Pen::TryParsePenUsbEventFrame(packet).has_value(),
                "known event with empty payload must be rejected before ACK/session/callback processing");
    }

    const std::array<uint8_t, 8> unknownEventWithoutPayload{
        0x00, 0x00, 0x07, 0x00,
        0x01, 0x55, 0x00, 0x00,
    };
    Require(Himax::Pen::TryParsePenUsbEventFrame(unknownEventWithoutPayload).has_value(),
            "unknown events should not inherit a fabricated payload requirement");
}

void TestFactoryAckTable() {
    using Himax::Pen::GetFactoryBtMcuAckCode;

    Require(GetFactoryBtMcuAckCode(0x2F) == 0x0B, "0x2F should ACK 0x0B");
    Require(GetFactoryBtMcuAckCode(0x70) == 0x00, "0x70 should ACK 0");
    Require(GetFactoryBtMcuAckCode(0x71) == 0x01, "0x71 should ACK 1");
    Require(GetFactoryBtMcuAckCode(0x72) == 0x02, "0x72 should ACK 2");
    Require(GetFactoryBtMcuAckCode(0x73) == 0x0D, "0x73 should ACK 0x0D");
    Require(GetFactoryBtMcuAckCode(0x74) == 0x03, "0x74 should ACK 3");
    Require(GetFactoryBtMcuAckCode(0x75) == 0x04, "0x75 should ACK 4");
    Require(GetFactoryBtMcuAckCode(0x76) == 0x05, "0x76 should ACK 5");
    Require(GetFactoryBtMcuAckCode(0x77) == 0x06, "0x77 should ACK 6");
    Require(GetFactoryBtMcuAckCode(0x78) == 0x07, "0x78 should ACK 7");
    Require(GetFactoryBtMcuAckCode(0x79) == 0x08, "0x79 should ACK 8");
    Require(GetFactoryBtMcuAckCode(0x7B) == 0x0A, "0x7B should ACK 0x0A");
    Require(GetFactoryBtMcuAckCode(0x7C) == 0x0C, "0x7C should ACK 0x0C");
    Require(GetFactoryBtMcuAckCode(0x7F) == 0x09, "0x7F should ACK 9");
    Require(GetFactoryBtMcuAckCode(0x00) == -1, "PenModule 0x00 should not be ACKed without factory evidence");
    Require(GetFactoryBtMcuAckCode(0x28) == -1, "Open event 0x28 should not be ACKed without factory evidence");
    Require(GetFactoryBtMcuAckCode(0x6F) == -1, "0x6F should not be ACKed without factory evidence");
}

void TestEventCodeNames() {
    using Himax::Pen::PenUsbEventCode;
    using Himax::Pen::PenUsbEventCodeFromRaw;
    using Himax::Pen::PenUsbEventNameFromRaw;
    using Himax::Pen::ToString;

    Require(PenUsbEventCodeFromRaw(0x01) == PenUsbEventCode::PenSerialNumber,
            "0x01 should map to PenSerialNumber");
    Require(PenUsbEventCodeFromRaw(0x71) == PenUsbEventCode::PenConnStatus,
            "0x71 should map to PenConnStatus");
    Require(PenUsbEventCodeFromRaw(0x7B) == PenUsbEventCode::PenRepParam,
            "0x7B should map to PenRepParam");
    Require(PenUsbEventCodeFromRaw(0x28) == PenUsbEventCode::PenTopBatteryWindow,
            "0x28 should map to the top battery window event");
    Require(PenUsbEventCodeFromRaw(0x2A) == PenUsbEventCode::PenDeviationReminder,
            "0x2A should map to the pen deviation reminder event");
    Require(PenUsbEventCodeFromRaw(0x55) == PenUsbEventCode::Unknown,
            "unknown event code should map to Unknown");

    Require(std::string(ToString(PenUsbEventCode::PenConnStatus)) == "PEN_CONN_STATUS",
            "0x71 should format as PEN_CONN_STATUS");
    Require(std::string(ToString(PenUsbEventCode::PenRepParam)) == "PEN_REP_PARAM",
            "0x7B should format as PEN_REP_PARAM");
    Require(std::string(ToString(PenUsbEventCode::PenRotateAngle)) == "PEN_ROATE_ANGLE",
            "0x74 should preserve the protocol document spelling");
    Require(std::string(PenUsbEventNameFromRaw(0x28)) == "PEN_TOP_BATTERY_WINDOW",
            "0x28 should format as the top battery window event");
    Require(std::string(PenUsbEventNameFromRaw(0x2A)) == "PEN_DEVIATION_REMINDER",
            "0x2A should format as the pen deviation reminder event");
    Require(std::string(PenUsbEventNameFromRaw(0x55)) == "UNKNOWN_EVENT",
            "unknown event code should format as UNKNOWN_EVENT");
}

void TestCommandPacketBuilders() {
    using Himax::Pen::BuildPenUsbCommand;
    using Himax::Pen::BuildPenUsbEventAck;
    using Himax::Pen::BuildPenUsbPayloadCommand;
    using Himax::Pen::PenUsbCommandId;

    Require(BuildPenUsbCommand(PenUsbCommandId::QueryPenModule) ==
                std::vector<uint8_t>({0x07, 0x00, 0x02, 0x00, 0x01, 0x00, 0x11, 0x00}),
            "0x0001 command packet should query pen module");
    Require(BuildPenUsbCommand(PenUsbCommandId::QueryPenSerialNo) ==
                std::vector<uint8_t>({0x07, 0x00, 0x02, 0x00, 0x01, 0x01, 0x11, 0x00}),
            "0x0101 command packet should query pen serial number");
    Require(BuildPenUsbCommand(PenUsbCommandId::QueryHardwareVersion) ==
                std::vector<uint8_t>({0x07, 0x00, 0x02, 0x00, 0x01, 0x02, 0x11, 0x00}),
            "0x0201 command packet should query pen hardware version");
    Require(BuildPenUsbCommand(PenUsbCommandId::QueryFirmwareVersion) ==
                std::vector<uint8_t>({0x07, 0x00, 0x02, 0x00, 0x01, 0x03, 0x11, 0x00}),
            "0x0301 command packet should query pen firmware version");
    // 下面三条的字节序列直接来自原厂 PenService.dll 里 CommandSendGetPenBattery /
    // GetPenChargingStatus / GetPenConnectStatus 的栈上立即数，是这几个命令码唯一的
    // 校验来源 —— 抄错一个字节，笔就只是不回应，没有其他报错。
    Require(BuildPenUsbCommand(PenUsbCommandId::QueryPenBattery) ==
                std::vector<uint8_t>({0x07, 0x00, 0x02, 0x00, 0x01, 0x08, 0x11, 0x00}),
            "0x0801 command packet should match PenService.dll CommandSendGetPenBattery");
    Require(BuildPenUsbCommand(PenUsbCommandId::QueryChargingStatus) ==
                std::vector<uint8_t>({0x07, 0x00, 0x02, 0x00, 0x01, 0x09, 0x11, 0x00}),
            "0x0901 command packet should match PenService.dll CommandSendGetPenChargingStatus");
    Require(BuildPenUsbCommand(PenUsbCommandId::QueryConnectStatus) ==
                std::vector<uint8_t>({0x07, 0x00, 0x02, 0x00, 0x01, 0x12, 0x11, 0x00}),
            "0x1201 command packet should match PenService.dll CommandSendGetPenConnectStatus");
    Require(BuildPenUsbCommand(PenUsbCommandId::QueryPenStatus) ==
                std::vector<uint8_t>({0x07, 0x00, 0x02, 0x00, 0x01, 0x71, 0x11, 0x00}),
            "0x7101 command packet should match factory bytes");
    Require(BuildPenUsbCommand(PenUsbCommandId::QueryPenInfo) ==
                std::vector<uint8_t>({0x07, 0x00, 0x02, 0x00, 0x01, 0x77, 0x11, 0x00}),
            "0x7701 command packet should match factory bytes");
    Require(BuildPenUsbCommand(PenUsbCommandId::PairInfoSet) ==
                std::vector<uint8_t>({0x07, 0x00, 0x02, 0x00, 0x01, 0x7E, 0x11, 0x00}),
            "0x7E01 command packet should use little-endian command id");
    Require(BuildPenUsbEventAck(0x0A) ==
                std::vector<uint8_t>({0x07, 0x01, 0x02, 0x00, 0x01, 0x80, 0x11, 0x20, 0x0A}),
            "0x8001 ACK packet should match factory bytes");

    const std::array<uint8_t, 3> payload{0xAA, 0xBB, 0xCC};
    Require(BuildPenUsbPayloadCommand(PenUsbCommandId::InitParamSet, payload) ==
                std::vector<uint8_t>({0x07, 0x01, 0x02, 0x00, 0x01, 0x7D, 0x11, 0x20, 0xAA, 0xBB, 0xCC}),
            "payload command should preserve header and append payload bytes");
}

void TestUtf8PayloadDecoding() {
    const std::array<uint8_t, 8> version{'H', 'W', '1', '.', '2', 0x00, 'X', 'Y'};
    Require(Himax::Pen::DecodePenUsbUtf8Payload(version) == "HW1.2",
            "UTF-8 payload should stop at the first NUL byte");

    const std::array<uint8_t, 7> utf8{'H', 'W', '-', 0xE7, 0xAC, 0x94, 0x00};
    Require(Himax::Pen::DecodePenUsbUtf8Payload(utf8) == std::string("HW-\xE7\xAC\x94", 6),
            "UTF-8 payload should preserve multibyte characters");

    const std::array<uint8_t, 6> mixed{'A', 0x01, 'B', 0x7F, 'C', 0x00};
    Require(Himax::Pen::DecodePenUsbUtf8Payload(mixed).empty(),
            "payload with control characters should be rejected");

    const std::array<uint8_t, 4> binary{0x01, 0x02, 0x1F, 0x00};
    Require(Himax::Pen::DecodePenUsbUtf8Payload(binary).empty(),
            "binary payload should not be exposed as a version string");

    const std::array<uint8_t, 3> invalid{0xE7, 0xAC, 0x00};
    Require(Himax::Pen::DecodePenUsbUtf8Payload(invalid).empty(),
            "truncated UTF-8 payload should be rejected");

    const std::array<uint8_t, 3> empty{0x00, 'H', 'W'};
    Require(Himax::Pen::DecodePenUsbUtf8Payload(empty).empty(),
            "payload starting with NUL should decode as an empty string");
}

void TestType3Encoding() {
    std::array<uint8_t, 8> out{};
    Require(Himax::Pen::EncodePenUsbType3Token("", out) == 0, "empty token should not emit bytes");

    out = {};
    Require(Himax::Pen::EncodePenUsbType3Token("7", out) == 1, "1-digit token should encode to 1 byte");
    Require(out[0] == 0x07, "7 should encode as 07");

    out = {};
    Require(Himax::Pen::EncodePenUsbType3Token("12", out) == 2, "2-digit token should encode to 2 bytes");
    Require(out[0] == 0x02 && out[1] == 0x01, "12 should encode as 02 01");

    out = {};
    Require(Himax::Pen::EncodePenUsbType3Token("3333", out) == 2, "4-digit token should encode to 2 bytes");
    Require(out[0] == 0x33 && out[1] == 0x33, "3333 should encode as 33 33");

    out = {};
    Require(Himax::Pen::EncodePenUsbType3Token("2e7", out) == 2, "3-digit token should encode to 2 bytes");
    Require(out[0] == 0xE7 && out[1] == 0x02, "2e7 should encode as e7 02");

    const auto payload = Himax::Pen::BuildScanModePayload(51, 68, 1);
    Require(payload[0] == 0x01 && payload[1] == 0x05,
            "scan freq1 decimal token should preserve current type-3 behavior");
    Require(payload[2] == 0x08 && payload[3] == 0x06,
            "scan freq2 decimal token should preserve current type-3 behavior");
    Require(payload[4] == 0x03, "non-zero scan mode should encode as mode 3");

    const auto packet = Himax::Pen::BuildScanModeCommandBuffer(51, 68, 1);
    Require(packet.size == 40, "scan mode command should include the 8-byte header and 32-byte payload");
    Require(packet.bytes[4] == 0x01 && packet.bytes[5] == 0x7D,
            "scan mode command should target 0x7D01");
    Require(packet.bytes[8] == 0x01 && packet.bytes[9] == 0x05 &&
                packet.bytes[10] == 0x08 && packet.bytes[11] == 0x06 &&
                packet.bytes[12] == 0x03,
            "scan mode command payload prefix should be 01 05 08 06 03");

    const auto offPayload = Himax::Pen::BuildScanModePayload(51, 68, 0);
    Require(offPayload[4] == 0x00, "zero scan mode should encode as mode 0");
}

void TestFactoryInitParamsPacket() {
    const auto packet = Himax::Pen::BuildFactoryInitProtocolParamsCommand();
    const std::vector<uint8_t> expected{
        0x07, 0x01, 0x02, 0x00, 0x01, 0x7D, 0x11, 0x20,
        0x33, 0x33, 0x33, 0x33, 0xE7, 0x02, 0x12, 0x04,
        0x58, 0x02, 0x1A, 0x41, 0x0F, 0x01, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    Require(packet == expected, "factory init params packet should remain exact capture");
    Require(packet != Himax::Pen::BuildScanModeCommand(0, 0, 0),
            "factory init packet must remain distinct from an all-zero scan-mode command");
}

// ── 键盘 detach support ──────────────────────────────────────────────────
void TestKbdDetachSupportCommandBytes() {
    const auto get = Himax::Pen::BuildKbdDetachSupportGetBuffer();
    const std::array<uint8_t, 8> expectedGet{
        0x09, 0x00, 0x02, 0x00, 0x00, 0x35, 0x11, 0x00,
    };
    Require(get.size == expectedGet.size(),
            "detach support Get must be 8 bytes");
    Require(std::equal(expectedGet.begin(), expectedGet.end(), get.bytes.begin()),
            "detach support Get bytes must match KBDMCU_PROTOCOL 4.1");

    const auto setOn = Himax::Pen::BuildKbdDetachSupportSetBuffer(true);
    const std::array<uint8_t, 9> expectedSetOn{
        0x09, 0x00, 0x02, 0x00, 0x00, 0x34, 0x11, 0x01, 0x01,
    };
    Require(setOn.size == expectedSetOn.size(),
            "detach support Set must be 9 bytes");
    Require(std::equal(expectedSetOn.begin(), expectedSetOn.end(), setOn.bytes.begin()),
            "detach support Set(enable) bytes must match KBDMCU_PROTOCOL 4.1");

    const auto setOff = Himax::Pen::BuildKbdDetachSupportSetBuffer(false);
    Require(setOff.bytes[8] == 0x00, "detach support Set(disable) payload must be 0");
    // byte[7] 是长度不是定长 tag：Get 为 0，Set 为 1。这正是与 pen payload builder（恒 0x20）
    // 的关键区别，写死会让 MCU 按错误长度读 payload。
    Require(get.bytes[7] == 0x00 && setOn.bytes[7] == 0x01,
            "byte[7] must equal payload length, not a fixed tag");
}

void TestKbdDetachSupportReplyParsing() {
    // 内联路径：byte[4]==0x00。
    const std::array<uint8_t, 9> inlineEnabled{
        0x02, 0x00, 0x09, 0x00, 0x00, 0x35, 0x11, 0x01, 0x01,
    };
    auto a = Himax::Pen::Kbd::TryParseDetachSupportReply(inlineEnabled);
    Require(a.has_value() && *a == true,
            "inline-path 0x35 reply with value 1 should parse as enabled");

    // 分发表路径：byte[4]==0x02。
    const std::array<uint8_t, 9> dispatchDisabled{
        0x02, 0x00, 0x05, 0x00, 0x02, 0x35, 0x11, 0x01, 0x00,
    };
    auto b = Himax::Pen::Kbd::TryParseDetachSupportReply(dispatchDisabled);
    Require(b.has_value() && *b == false,
            "dispatch-path 0x35 reply with value 0 should parse as disabled");

    // pen 子系统（byte[4]==0x01）不是 detach 应答。
    const std::array<uint8_t, 9> penFrame{
        0x02, 0x00, 0x07, 0x00, 0x01, 0x35, 0x11, 0x01, 0x01,
    };
    Require(!Himax::Pen::Kbd::TryParseDetachSupportReply(penFrame).has_value(),
            "pen-subsystem frame must not be read as a detach reply");

    // 命令码不是 0x35（如 Set 的 0x34）不解析。
    const std::array<uint8_t, 9> setEcho{
        0x02, 0x00, 0x09, 0x00, 0x00, 0x34, 0x11, 0x01, 0x01,
    };
    Require(!Himax::Pen::Kbd::TryParseDetachSupportReply(setEcho).has_value(),
            "0x34 echo must not be parsed as a support status");

    // 缺 payload 字节的短帧拒绝。
    const std::array<uint8_t, 8> noValue{
        0x02, 0x00, 0x09, 0x00, 0x00, 0x35, 0x11, 0x00,
    };
    Require(!Himax::Pen::Kbd::TryParseDetachSupportReply(noValue).has_value(),
            "reply without a value byte should be rejected");
}

void TestKbdChargingProtocol() {
    const auto query = Himax::Pen::BuildKbdChargingStatusGetBuffer();
    const std::array<uint8_t, 8> expected{
        0x05, 0x00, 0x02, 0x00, 0x02, 0x09, 0x11, 0x01,
    };
    Require(query.size == expected.size(),
            "keyboard charging query must write exactly 8 bytes despite byte[7]=1");
    Require(std::equal(expected.begin(), expected.end(), query.bytes.begin()),
            "keyboard charging query must preserve the factory byte sequence");

    const std::array<uint8_t, 9> charging{
        0x02, 0x00, 0x05, 0x00, 0x02, 0x09, 0x11, 0x01, 0x01,
    };
    const auto on = Himax::Pen::Kbd::TryParseChargingStatus(charging);
    Require(on.has_value() && *on,
            "keyboard 0x09 payload 1 should parse as charging");

    auto notCharging = charging;
    notCharging[8] = 0;
    const auto off = Himax::Pen::Kbd::TryParseChargingStatus(notCharging);
    Require(off.has_value() && !*off,
            "keyboard 0x09 payload 0 should parse as not charging");

    auto wrongSubsystem = charging;
    wrongSubsystem[4] = Himax::Pen::kSubsystemPen;
    Require(!Himax::Pen::Kbd::TryParseChargingStatus(wrongSubsystem).has_value(),
            "pen 0x09 must not be read as keyboard charging");

    const std::array<uint8_t, 8> missingPayload{
        0x02, 0x00, 0x05, 0x00, 0x02, 0x09, 0x11, 0x00,
    };
    Require(!Himax::Pen::Kbd::TryParseChargingStatus(missingPayload).has_value(),
            "keyboard charging reply without payload must be rejected");
}

void TestKbdConnectionEdges() {
    using KbdState = Himax::Pen::PenEventBridge::KbdState;
    const auto edge = Himax::Pen::PenEventBridge::IsKbdConnectionEdge;

    KbdState unknown{};
    KbdState initialConnected{};
    initialConnected.hasPresent = true;
    initialConnected.present = true;
    Require(!edge(unknown, initialConnected),
            "initial keyboard status must establish a baseline without notification");

    KbdState absent = initialConnected;
    absent.present = false;
    Require(edge(absent, initialConnected),
            "keyboard absent-to-present transition should notify");
    Require(!edge(initialConnected, initialConnected),
            "duplicate connected status should not notify");

    KbdState detached = initialConnected;
    detached.hasDetached = true;
    detached.detached = true;
    KbdState attached = detached;
    attached.detached = false;
    Require(edge(detached, attached),
            "keyboard detached-to-attached transition should notify");
    Require(!edge(attached, detached),
            "keyboard detach transition should not notify");
}

} // namespace

int main() {
    try {
        TestValidFactoryEventFrameParses();
        TestMinimumFactoryEventFrameParses();
        TestHardwareVersionEventFrameParses();
        TestInvalidFactoryEventFramesAreRejected();
        TestMalformedKnownEventsAreRejectedBeforeDispatch();
        TestFactoryAckTable();
        TestEventCodeNames();
        TestCommandPacketBuilders();
        TestUtf8PayloadDecoding();
        TestType3Encoding();
        TestFactoryInitParamsPacket();
        TestKbdDetachSupportCommandBytes();
        TestKbdDetachSupportReplyParsing();
        TestKbdChargingProtocol();
        TestKbdConnectionEdges();
        std::cout << "[TEST] Device Pen USB protocol packet tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
