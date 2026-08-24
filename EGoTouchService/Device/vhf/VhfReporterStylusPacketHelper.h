#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "SolverTypes.h"

namespace VhfStylusPacket {

struct Config {
    int sensorRows = 40;
    int sensorCols = 60;
    bool emitWhenInvalid = true;
    bool barrelButton = false;
};

namespace detail {

constexpr float kHidMaxX = 16000.0f;
constexpr float kHidMaxY = 25600.0f;
constexpr int16_t kTiltMax = 9000;
constexpr float kCoorUnit = 1024.0f;

inline void WriteU16Le(std::array<uint8_t, 17>& bytes,
                       size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value & 0xFFu);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

inline uint16_t ClampToU16(int32_t value) {
    return static_cast<uint16_t>(std::clamp(value, 0, 0xFFFF));
}

inline uint16_t MapOutputYToHidX(float sensorY, int sensorRows) {
    const float activeRows =
        std::max(1.0f, static_cast<float>(std::max(sensorRows, 1)) * kCoorUnit);
    const float clampedY = std::clamp(sensorY, 0.0f, activeRows);
    const auto hidX = static_cast<int32_t>(std::lround((clampedY / activeRows) * kHidMaxX));
    return ClampToU16(hidX);
}

inline uint16_t MapOutputXToHidY(float sensorX, int sensorCols) {
    const float activeCols =
        std::max(1.0f, static_cast<float>(std::max(sensorCols, 1)) * kCoorUnit);
    const float clampedX = std::clamp(sensorX, 0.0f, activeCols);
    const auto hidY = static_cast<int32_t>(std::lround((1.0f - (clampedX / activeCols)) * kHidMaxY));
    return ClampToU16(hidY);
}

inline bool HasReportableOutput(const Solvers::StylusOutputState& output) {
    return output.valid || output.point.valid || output.inRange;
}

} // namespace detail

inline Solvers::StylusPacket Build(const Solvers::StylusFrameData& stylus,
                                   const Config& config) {
    Solvers::StylusPacket packet{};
    packet.reportId = 0x08;
    packet.length = 13;

    if (!detail::HasReportableOutput(stylus.output)) {
        if (!config.emitWhenInvalid) {
            return packet;
        }

        packet.valid = true;
        packet.bytes.fill(0);
        packet.bytes[0] = packet.reportId;
        return packet;
    }

    packet.valid = true;
    packet.bytes.fill(0);
    packet.bytes[0] = packet.reportId;

    uint8_t penState = 0;
    if (stylus.output.tipDown) {
        penState |= 0x01u;   // Tip Switch
    }
    if (config.barrelButton) {
        penState |= 0x02u;   // Barrel Switch
    }
    if (stylus.output.inRange) {
        penState |= 0x20u;   // In Range
    }
    packet.bytes[1] = penState;
    packet.bytes[2] = 0x00u;

    detail::WriteU16Le(packet.bytes, 3,
                       detail::MapOutputYToHidX(stylus.output.point.y,
                                                config.sensorRows));
    detail::WriteU16Le(packet.bytes, 5,
                       detail::MapOutputXToHidY(stylus.output.point.x,
                                                config.sensorCols));
    detail::WriteU16Le(packet.bytes, 7,
                       static_cast<uint16_t>(std::min<uint32_t>(stylus.output.pressure, 4095u)));

    // 描述符的 tilt logical 范围 ±9000,单位是百分之一度;point.tilt* 是度,不乘
    // 100 时上报值只有真实值的 1/100。轴映射跟随坐标:HID X 轴对应面板 dim2、
    // HID Y 是反向的 dim1。tiltDim 的符号约定是 diff = 笔尖线圈 − 笔杆线圈,
    // 笔顶倒向 +dim2 时 tiltDim2 为负,而 HID XTilt 以倒向 +X 为正,故取负;
    // dim1 的两次反号(坐标反向 + diff 方向)相互抵消,YTilt 直接取正。
    const int32_t tiltX = std::clamp(static_cast<int32_t>(stylus.output.point.tiltY) * -100,
                                     static_cast<int32_t>(-detail::kTiltMax),
                                     static_cast<int32_t>(detail::kTiltMax));
    const int32_t tiltY = std::clamp(static_cast<int32_t>(stylus.output.point.tiltX) * 100,
                                     static_cast<int32_t>(-detail::kTiltMax),
                                     static_cast<int32_t>(detail::kTiltMax));
    detail::WriteU16Le(packet.bytes, 9, static_cast<uint16_t>(static_cast<int16_t>(tiltX)));
    detail::WriteU16Le(packet.bytes, 11, static_cast<uint16_t>(static_cast<int16_t>(tiltY)));
    return packet;
}

// Windows 只接受有限的 pen switch 组合。尤其是橡皮擦悬停只能报告
// InRange + Invert，Eraser 只能在接触时出现；普通笔与橡皮擦在悬停中切换时还必须先经过
// 一帧 out-of-range。把这段规则放在可测试的 helper 里，避免 reporter 再次退化成按位 OR。
struct EraserToolUpdate {
    std::array<uint8_t, 17> bytes{};
    std::array<uint8_t, 17> transitionBytes{};
    bool prependOutOfRange = false;
    bool eraserApplied = false;
};

inline EraserToolUpdate ApplyEraserToolState(
        const Solvers::StylusPacket& packet,
        bool eraserRequested,
        bool eraserApplied) {
    constexpr uint8_t kTip = 0x01u;
    constexpr uint8_t kInvert = 0x04u;
    constexpr uint8_t kEraser = 0x08u;
    constexpr uint8_t kInRange = 0x20u;
    constexpr uint8_t kToolBits = kTip | 0x02u | kInvert | kEraser;

    EraserToolUpdate update{};
    update.bytes = packet.bytes;
    update.transitionBytes = packet.bytes;
    update.eraserApplied = eraserApplied;

    const uint8_t rawState = packet.bytes[1];
    const bool tipDown = (rawState & kTip) != 0;
    const bool inRange = (rawState & kInRange) != 0;

    if (eraserRequested != eraserApplied && !tipDown) {
        // 悬停中不能在 pen 与 eraser intent 之间直接跳。先发一帧保持当前位置的
        // out-of-range，再发送新工具状态。已经 out-of-range 时无需重复中性帧。
        update.prependOutOfRange = inRange;
        update.eraserApplied = eraserRequested;
        if (update.prependOutOfRange) {
            update.transitionBytes[1] = 0;
            update.transitionBytes[7] = 0;
            update.transitionBytes[8] = 0;
        }
    }

    if (!update.eraserApplied) {
        update.bytes[1] = static_cast<uint8_t>(rawState & ~(kInvert | kEraser));
        return update;
    }

    if (!inRange) {
        // out-of-range 报文不能携带 tip/barrel/invert/eraser；否则 Windows 可能拒绝状态。
        update.bytes[1] = 0;
        update.bytes[7] = 0;
        update.bytes[8] = 0;
        return update;
    }

    update.bytes[1] = static_cast<uint8_t>(rawState & ~kToolBits);
    if (tipDown) {
        // 双击切换模拟的是按钮式橡皮擦：接触时只报告 Eraser。部分旧 ink
        // 客户端（例如桌面版 OneNote）会严格区分按钮式与笔尾式状态机。
        update.bytes[1] = static_cast<uint8_t>(update.bytes[1] | kEraser);
    } else {
        // 悬停只表达擦除意图，绝不能提前置 Eraser。
        update.bytes[1] = static_cast<uint8_t>(update.bytes[1] | kInvert);
    }
    return update;
}

inline uint8_t ExtractPenState(const Solvers::StylusPacket& packet) {
    return packet.valid ? packet.bytes[1] : 0;
}

} // namespace VhfStylusPacket
