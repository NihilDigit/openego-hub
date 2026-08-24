#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "PenModuleModelId.h"

namespace Himax::Pen {

using NativeEventHandle = void*;

constexpr std::size_t kPenUsbPacketCapacity = 64;
constexpr std::size_t kPenUsbHeaderSize = 8;
constexpr std::size_t kPenUsbPayloadCapacity = kPenUsbPacketCapacity - kPenUsbHeaderSize;

struct PenUsbPacketBuffer {
    std::array<uint8_t, kPenUsbPacketCapacity> bytes{};
    std::size_t size = 0;

    [[nodiscard]] std::span<uint8_t> writable() noexcept { return bytes; }
    [[nodiscard]] std::span<const uint8_t> view() const noexcept { return std::span<const uint8_t>(bytes.data(), size); }
    [[nodiscard]] bool empty() const noexcept { return size == 0; }
    void clear() noexcept { size = 0; bytes.fill(0); }
};

struct PenUsbPayloadBuffer {
    std::array<uint8_t, kPenUsbPayloadCapacity> bytes{};
    std::size_t size = 0;

    [[nodiscard]] std::span<const uint8_t> view() const noexcept { return std::span<const uint8_t>(bytes.data(), size); }
    [[nodiscard]] bool empty() const noexcept { return size == 0; }
    [[nodiscard]] uint8_t operator[](std::size_t index) const noexcept { return bytes[index]; }
    bool assign(std::span<const uint8_t> payload) noexcept {
        if (payload.size() > bytes.size()) return false;
        std::copy(payload.begin(), payload.end(), bytes.begin());
        size = payload.size();
        if (size < bytes.size()) {
            std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(size), bytes.end(), 0);
        }
        return true;
    }
};

// 命令低字节与对应应答的事件码同值：QueryPenBattery(0x08) 的应答是 BATTERY_STATUS(0x08)。
// 这几条取自原厂 PenService.dll 的 CommandSendGetPenXxx 反汇编，每个函数在栈上拼
// 07 00 02 00 | 01 <code> 11 00，与这里的构造完全一致。
enum class PenUsbCommandId : uint16_t {
    QueryPenModule = 0x0001,
    QueryPenSerialNo = 0x0101,
    QueryHardwareVersion = 0x0201,
    QueryFirmwareVersion = 0x0301,
    QueryPenBattery = 0x0801,       // PenService: CommandSendGetPenBattery
    QueryChargingStatus = 0x0901,   // PenService: CommandSendGetPenChargingStatus
    QueryConnectStatus = 0x1201,    // PenService: CommandSendGetPenConnectStatus
    QueryPenStatus = 0x7101,
    QueryPenInfo = 0x7701,
    InitParamSet = 0x7D01,
    PairInfoSet = 0x7E01,
    EventAck = 0x8001,
};

struct ParsedPenUsbEventFrame {
    uint8_t eventCode = 0;
    std::span<const uint8_t> payload{};
};

// ── 键盘 / detach 子系统 ──────────────────────────────────────────────────
// 键盘与 pen 共用同一个 USB 端点，帧结构相同，靠 byte[0] 目标地址与 byte[4] 子系统 ID
// 区分。逆向结论见 docs/KBDMCU_PROTOCOL.md。本文件覆盖 detach support 以及设备页使用的
// 键盘连接、电量、充电、分离和固件状态。
// byte[4] 子系统 ID。笔与键盘共用同一个端点，这张表是双方的分流依据，不属于任何一方，
// 所以放在外层而不是 Kbd 里。(byte[0], byte[4]) 严格成对：pen=(0x07,0x01)、
// 键盘状态=(0x05,0x02)、detach 开关=(0x09,0x00)。
inline constexpr uint8_t kSubsystemPen           = 0x01;
inline constexpr uint8_t kSubsystemKeyboard      = 0x02;
inline constexpr uint8_t kSubsystemDetachSupport = 0x00;

namespace Kbd {

inline constexpr uint8_t kDetachDestination   = 0x09;  // byte[0]
inline constexpr uint8_t kCmdDetachSupportGet = 0x35;  // byte[5]，查询
inline constexpr uint8_t kCmdDetachSupportSet = 0x34;  // byte[5]，设置

inline constexpr uint8_t kKeyboardDestination = 0x05;  // byte[0]，键盘状态类命令
inline constexpr uint8_t kCmdFirmwareVersion  = 0x03;  // 字符串应答，型号靠它的平台前缀判定
inline constexpr uint8_t kCmdBattery          = 0x08;  // packet[8] 是百分比
inline constexpr uint8_t kCmdCharging         = 0x09;  // packet[8]：0=未充电，非 0=正在充电
inline constexpr uint8_t kCmdConnectStatus    = 0x12;  // packet[8] ∈ {1,2,3} 均视为已连接
inline constexpr uint8_t kCmdDetachStatus     = 0x31;  // 实机：非 0=已吸附，0=已分离

// 固件串的平台前缀是唯一可靠的型号判据：MCU 的模组查询对键盘返回 0，而宿主那条「按主机机型
// 查表」的路判的是主机不是键盘。两个已证实的前缀见下；其余一律当未知型号，绝不猜测——
// RX04 Glide 是另一种外形、另一张产品图，猜错会显示错误的设备。详见 docs/KEYBOARD_IDENTITY.md。
inline constexpr const char* kModelNameGaokun = "HUAWEI 智能磁吸键盘";
inline constexpr const char* kModelNameDiracR = "HUAWEI 智能磁吸键盘";

// RX0H 与 RX0I 的产品图字节完全相同，原厂对这一系列就用同一张，所以两者共用一个显示名不是
// 妥协。真正需要分开的是 Glide 那类另有外形的型号，而它落进「未知」分支。
inline std::string_view KeyboardModelNameFromFirmware(std::string_view firmware) noexcept {
    if (firmware.find("GAOKUN") != std::string_view::npos) return kModelNameGaokun;
    if (firmware.find("DIRACR") != std::string_view::npos) return kModelNameDiracR;
    return {};
}

// detach support 查询应答：MCU 可能走 ProcPipeMsg 内联路径回显 byte[4]==0x00，也可能走
// 分发表 byte[4]==0x02，两者都携带 byte[5]==0x35，值在 byte[8]（0=禁用，非 0=启用）。
// Set(0x34) 的应答会被原厂读线程的过滤规则丢弃，所以不解析它，Set 之后靠重新 Get 确认。
inline std::optional<bool> TryParseDetachSupportReply(
        std::span<const uint8_t> packet) noexcept {
    if (packet.size() < kPenUsbHeaderSize + 1) {
        return std::nullopt;
    }
    if (packet[4] != kSubsystemDetachSupport && packet[4] != kSubsystemKeyboard) {
        return std::nullopt;
    }
    if (packet[5] != kCmdDetachSupportGet) {
        return std::nullopt;
    }
    return packet[kPenUsbHeaderSize] != 0;
}

inline std::optional<bool> TryParseChargingStatus(
        std::span<const uint8_t> packet) noexcept {
    if (packet.size() < kPenUsbHeaderSize + 1 ||
        packet[4] != kSubsystemKeyboard ||
        packet[5] != kCmdCharging ||
        packet[7] < 1) {
        return std::nullopt;
    }
    return packet[kPenUsbHeaderSize] != 0;
}

} // namespace Kbd

constexpr std::size_t GetPenUsbEventMinimumPayloadLength(uint8_t eventCode) noexcept {
    switch (eventCode) {
    case 0x00: // PenModule
    case 0x01: // PenSerialNumber
    case 0x02: // PenHardwareVersion
    case 0x03: // UsbdSwVersion
    case 0x08: // BatteryStatus
    case 0x09: // ChargingStatus
    case 0x10: // DevConnect
    case 0x12: // DevPairStatus
    case 0x21: // PenDockStatus
    case 0x23: // PenUpdateStatus
    case 0x27: // PenKeyFuncGet
    case 0x2C: // PenBatteryAfterConn
    case 0x2E: // PenPairDetectAck
    case 0x2F: // PenCurrentFunc
    case 0x70: // PenAcStatus
    case 0x71: // PenConnStatus
    case 0x72: // PenCurStatus
    case 0x73: // PenTypeInfo
    case 0x74: // PenRotateAngle
    case 0x75: // PenTouchMode
    case 0x76: // PenGlobalPreventMode
    case 0x77: // PenScreenStatus
    case 0x78: // PenHolster
    case 0x79: // PenFreqJump
    case 0x7B: // PenRepParam
    case 0x7C: // PenGlobalAnnotation
    case 0x7F: // EraserToggle
        return 1;
    default:
        return 0;
    }
}

inline std::optional<ParsedPenUsbEventFrame> TryParsePenUsbEventFrame(
        std::span<const uint8_t> packet) noexcept {
    if (packet.size() < kPenUsbHeaderSize) {
        return std::nullopt;
    }
    // 只按子系统 ID 分流，与原厂读线程一致：PenService.dll 只校验 packet[4]，不看
    // packet[0]/[2]/[6]。这里曾经额外要求 packet[2]==0x07，比原厂更严——键盘与笔共用同一个
    // 端点之后，严格的方向是反的：多校验一个字节只会丢弃原厂会接受的帧，换不来任何安全性，
    // 因为非笔子系统在调用到这里之前就已经被 PenEventBridge 分流走了。
    if (packet[4] != kSubsystemPen) {
        return std::nullopt;
    }

    const std::size_t payloadLength = packet[7];
    if (payloadLength > kPenUsbPayloadCapacity ||
        packet.size() < kPenUsbHeaderSize + payloadLength ||
        payloadLength < GetPenUsbEventMinimumPayloadLength(packet[5])) {
        return std::nullopt;
    }

    return ParsedPenUsbEventFrame{
        packet[5],
        packet.subspan(kPenUsbHeaderSize, payloadLength),
    };
}

constexpr int GetFactoryBtMcuAckCode(uint8_t eventCode) noexcept {
    switch (eventCode) {
    case 0x2F: return 0x0B;
    case 0x70: return 0x00;
    case 0x71: return 0x01;
    case 0x72: return 0x02;
    case 0x73: return 0x0D;
    case 0x74: return 0x03;
    case 0x75: return 0x04;
    case 0x76: return 0x05;
    case 0x77: return 0x06;
    case 0x78: return 0x07;
    case 0x79: return 0x08;
    case 0x7B: return 0x0A;
    case 0x7C: return 0x0C;
    case 0x7F: return 0x09;
    default: return -1;
    }
}

enum class PenUsbEventCode : uint8_t {
    PenModule = 0x00,               // PenService PenModule / ModelId 上报
    PenSerialNumber = 0x01,         // PenService serial number ASCII 上报
    PenHardwareVersion = 0x02,      // PenService 硬件版本 ASCII 上报
    UsbdSwVersion = 0x03,
    BatteryStatus = 0x08,
    ChargingStatus = 0x09,
    DevConnect = 0x10,
    DevPairStatus = 0x12,
    PenDockStatus = 0x21,
    PenUpdateStatus = 0x23,
    PenKeyFuncGet = 0x27,
    PenTopBatteryWindow = 0x28,     // 原厂请求宿主显示连接/电量浮层
    PenCloseConnectWindow = 0x29,   // 原厂请求关闭连接浮层
    PenDeviationReminder = 0x2A,    // 笔未正确吸附的瞬时提醒
    PenBatteryAfterConn = 0x2C,
    PenPairDetectAck = 0x2E,
    PenCurrentFunc = 0x2F,
    PenUnknown6F = 0x6F,            // 未确认事件，不自动 ACK
    PenAcStatus = 0x70,
    PenConnStatus = 0x71,
    PenCurStatus = 0x72,            // 笔工作模式（书写/悬停/橡皮擦）
    PenTypeInfo = 0x73,             // 笔类型信息 → set_stylus_id
    PenRotateAngle = 0x74,          // 屏幕旋转角度
    PenTouchMode = 0x75,
    PenGlobalPreventMode = 0x76,
    PenScreenStatus = 0x77,         // 屏幕状态（非 PEN_READY！）
    PenHolster = 0x78,
    PenFreqJump = 0x79,
    PenRepParam = 0x7B,             // 初始化参数 (CSV) → 0x7D01 回显
    PenGlobalAnnotation = 0x7C,
    EraserToggle = 0x7F,
    Unknown = 0xFF,
};

enum class PenCurrentMode : uint8_t {
    Unknown = 0,
    Writing = 1,
    Hovering = 2,
    Eraser = 3,
};

enum class PenPressureRangeMode : uint8_t {
    Raw12Bit4096 = 0,
    Raw14Bit16382 = 1,
};

struct PenPressureStats {
    uint16_t press[4] = {0, 0, 0, 0};
    uint16_t rawPress[4] = {0, 0, 0, 0};
    uint8_t freq1 = 0;
    uint8_t freq2 = 0;
    uint8_t reportType = 0;
    PenPressureRangeMode pressureMode = PenPressureRangeMode::Raw12Bit4096;
    uint16_t pressureMax = 4095;
};

constexpr PenCurrentMode PenCurrentModeFromRaw(uint8_t raw) noexcept {
    switch (raw) {
    case 1: return PenCurrentMode::Writing;
    case 2: return PenCurrentMode::Hovering;
    case 3: return PenCurrentMode::Eraser;
    default: return PenCurrentMode::Unknown;
    }
}

constexpr const char* ToString(PenCurrentMode mode) noexcept {
    switch (mode) {
    case PenCurrentMode::Writing: return "writing";
    case PenCurrentMode::Hovering: return "hovering";
    case PenCurrentMode::Eraser: return "eraser";
    default: return "unknown";
    }
}

constexpr PenUsbEventCode PenUsbEventCodeFromRaw(uint8_t code) noexcept {
    switch (code) {
    case 0x00: return PenUsbEventCode::PenModule;
    case 0x01: return PenUsbEventCode::PenSerialNumber;
    case 0x02: return PenUsbEventCode::PenHardwareVersion;
    case 0x03: return PenUsbEventCode::UsbdSwVersion;
    case 0x08: return PenUsbEventCode::BatteryStatus;
    case 0x09: return PenUsbEventCode::ChargingStatus;
    case 0x10: return PenUsbEventCode::DevConnect;
    case 0x12: return PenUsbEventCode::DevPairStatus;
    case 0x21: return PenUsbEventCode::PenDockStatus;
    case 0x23: return PenUsbEventCode::PenUpdateStatus;
    case 0x27: return PenUsbEventCode::PenKeyFuncGet;
    case 0x28: return PenUsbEventCode::PenTopBatteryWindow;
    case 0x29: return PenUsbEventCode::PenCloseConnectWindow;
    case 0x2A: return PenUsbEventCode::PenDeviationReminder;
    case 0x2C: return PenUsbEventCode::PenBatteryAfterConn;
    case 0x2E: return PenUsbEventCode::PenPairDetectAck;
    case 0x2F: return PenUsbEventCode::PenCurrentFunc;
    case 0x6F: return PenUsbEventCode::PenUnknown6F;
    case 0x70: return PenUsbEventCode::PenAcStatus;
    case 0x71: return PenUsbEventCode::PenConnStatus;
    case 0x72: return PenUsbEventCode::PenCurStatus;
    case 0x73: return PenUsbEventCode::PenTypeInfo;
    case 0x74: return PenUsbEventCode::PenRotateAngle;
    case 0x75: return PenUsbEventCode::PenTouchMode;
    case 0x76: return PenUsbEventCode::PenGlobalPreventMode;
    case 0x77: return PenUsbEventCode::PenScreenStatus;
    case 0x78: return PenUsbEventCode::PenHolster;
    case 0x79: return PenUsbEventCode::PenFreqJump;
    case 0x7B: return PenUsbEventCode::PenRepParam;
    case 0x7C: return PenUsbEventCode::PenGlobalAnnotation;
    case 0x7F: return PenUsbEventCode::EraserToggle;
    default: return PenUsbEventCode::Unknown;
    }
}

constexpr const char* ToString(PenUsbEventCode code) noexcept {
    switch (code) {
    case PenUsbEventCode::PenModule: return "PEN_MODULE";
    case PenUsbEventCode::PenSerialNumber: return "PEN_SERIAL_NUMBER";
    case PenUsbEventCode::PenHardwareVersion: return "PEN_HARDWARE_VERSION";
    case PenUsbEventCode::UsbdSwVersion: return "USBD_SW_VERSION";
    case PenUsbEventCode::BatteryStatus: return "BATTERY_STATUS";
    case PenUsbEventCode::ChargingStatus: return "CHARGING_STATUS";
    case PenUsbEventCode::DevConnect: return "DEV_CONNECT";
    case PenUsbEventCode::DevPairStatus: return "DEV_PAIR_STATUS";
    case PenUsbEventCode::PenDockStatus: return "PEN_DOCK_STATUS";
    case PenUsbEventCode::PenUpdateStatus: return "PEN_UPDATE_STATUS";
    case PenUsbEventCode::PenKeyFuncGet: return "PEN_KEY_FUNC_GET";
    case PenUsbEventCode::PenTopBatteryWindow: return "PEN_TOP_BATTERY_WINDOW";
    case PenUsbEventCode::PenCloseConnectWindow: return "PEN_CLOSE_CONNECT_WINDOW";
    case PenUsbEventCode::PenDeviationReminder: return "PEN_DEVIATION_REMINDER";
    case PenUsbEventCode::PenBatteryAfterConn: return "PEN_BATTERY_AFTER_CONN";
    case PenUsbEventCode::PenPairDetectAck: return "PEN_PAIR_DETECT_ACK";
    case PenUsbEventCode::PenCurrentFunc: return "PEN_CURRENT_FUNC";
    case PenUsbEventCode::PenUnknown6F: return "UNKNOWN_0x6F";
    case PenUsbEventCode::PenAcStatus: return "PEN_AC_STATUS";
    case PenUsbEventCode::PenConnStatus: return "PEN_CONN_STATUS";
    case PenUsbEventCode::PenCurStatus: return "PEN_CUR_STATUS";
    case PenUsbEventCode::PenTypeInfo: return "PEN_TYPE_INFO";
    case PenUsbEventCode::PenRotateAngle: return "PEN_ROATE_ANGLE";
    case PenUsbEventCode::PenTouchMode: return "PEN_TOUCH_MODE";
    case PenUsbEventCode::PenGlobalPreventMode: return "PEN_GLOBAL_PREVENT_MODE";
    case PenUsbEventCode::PenScreenStatus: return "PEN_SCREEN_STATUS";
    case PenUsbEventCode::PenHolster: return "PEN_HOLSTER";
    case PenUsbEventCode::PenFreqJump: return "PEN_FREQ_JUMP";
    case PenUsbEventCode::PenRepParam: return "PEN_REP_PARAM";
    case PenUsbEventCode::PenGlobalAnnotation: return "PEN_GLOBAL_ANNOTATION";
    case PenUsbEventCode::EraserToggle: return "ERASER_TOGGLE";
    default: return "UNKNOWN_EVENT";
    }
}

constexpr const char* PenUsbEventNameFromRaw(uint8_t code) noexcept {
    return ToString(PenUsbEventCodeFromRaw(code));
}

constexpr bool FactoryStatusFlagsAffected(PenUsbEventCode code) noexcept {
    switch (code) {
    case PenUsbEventCode::PenAcStatus:
    case PenUsbEventCode::PenConnStatus:
    case PenUsbEventCode::PenCurStatus:
    case PenUsbEventCode::PenTypeInfo:
    case PenUsbEventCode::PenRotateAngle:
    case PenUsbEventCode::PenTouchMode:
    case PenUsbEventCode::PenGlobalPreventMode:
    case PenUsbEventCode::PenHolster:
        return true;
    default:
        return false;
    }
}

constexpr uint16_t SetFactoryFlagField(uint16_t flags,
                                       uint16_t mask,
                                       uint16_t value) noexcept {
    return static_cast<uint16_t>((flags & ~mask) | (value & mask));
}

constexpr uint16_t ApplyFactoryStatusFlagUpdate(uint16_t flags,
                                                PenUsbEventCode code,
                                                uint8_t payload) noexcept {
    switch (code) {
    case PenUsbEventCode::PenAcStatus:
        return SetFactoryFlagField(flags, 0x0001u, payload & 0x01u);
    case PenUsbEventCode::PenConnStatus:
        return SetFactoryFlagField(flags, 0x0002u, static_cast<uint16_t>((payload & 0x01u) << 1));
    case PenUsbEventCode::PenCurStatus:
        if (payload == 1) return SetFactoryFlagField(flags, 0x000Cu, 0);
        if (payload == 2) return SetFactoryFlagField(flags, 0x000Cu, 0x0004u);
        if (payload == 3) return SetFactoryFlagField(flags, 0x000Cu, 0x0008u);
        return flags;
    case PenUsbEventCode::PenTypeInfo:
        return SetFactoryFlagField(flags, 0x0030u, static_cast<uint16_t>((payload & 0x03u) << 4));
    case PenUsbEventCode::PenRotateAngle:
        if (payload == 2) return SetFactoryFlagField(flags, 0x00C0u, 0);
        if (payload == 4) return SetFactoryFlagField(flags, 0x00C0u, 0x0080u);
        return SetFactoryFlagField(flags, 0x00C0u, static_cast<uint16_t>((payload << 6) & 0x00C0u));
    case PenUsbEventCode::PenTouchMode:
        return SetFactoryFlagField(flags, 0x0100u, static_cast<uint16_t>((payload & 0x01u) << 8));
    case PenUsbEventCode::PenGlobalPreventMode:
        return SetFactoryFlagField(flags, 0x0200u, static_cast<uint16_t>((payload & 0x01u) << 9));
    case PenUsbEventCode::PenHolster:
        return SetFactoryFlagField(flags, 0x0800u, static_cast<uint16_t>((payload & 0x01u) << 11));
    default:
        return flags;
    }
}

struct PenSemanticState {
    bool hasConnection = false;
    bool connected = false;

    bool hasPairStatus = false;
    uint8_t pairStatus = 0;

    bool hasStylusId = false;
    uint8_t stylusId = 0;

    bool hasPenModuleModelId = false;
    uint32_t penModuleModelId = 0;
    PenModuleModel penModuleModel = PenModuleModel::Unknown;
    bool hasPenModuleProtocolHint = false;
    PenModuleProtocolHint penModuleProtocolHint = PenModuleProtocolHint::Auto;

    bool hasSerialNumber = false;
    std::string serialNumber;

    bool hasHardwareVersion = false;
    std::string hardwareVersion;

    bool hasFirmwareVersion = false;
    std::string firmwareVersion;

    bool hasCurrentMode = false;
    PenCurrentMode currentMode = PenCurrentMode::Unknown;
    uint8_t currentModeRaw = 0;

    bool hasEraserToggle = false;
    uint8_t eraserToggle = 0;

    bool hasCurrentFunc = false;
    uint8_t currentFunc = 0;

    // BatteryStatus / ChargingStatus / DevConnect were reaching the default branch and
    // being logged only, so the pen's charge state was observable in the log but nowhere
    // in the runtime state.
    bool hasBatteryLevel = false;
    uint8_t batteryLevel = 0;      // percent as reported by the MCU

    bool hasChargingState = false;
    bool charging = false;

    // Distinct from PenConnStatus (0x71), which reports the stylus link. DevConnect
    // (0x10) reports the MCU's own device-level attach state.
    bool hasDeviceConnected = false;
    bool deviceConnected = false;
};

struct PenEvent {
    PenUsbEventCode code = PenUsbEventCode::Unknown;
    PenUsbPayloadBuffer payload{};
    PenSemanticState semantic{};
    std::chrono::steady_clock::time_point receivedAt{};
};

using PenEventCallback = std::function<void(const PenEvent&)>;

} // namespace Himax::Pen
