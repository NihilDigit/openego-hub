#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace Himax::Pen {

enum class PenModuleModel : uint8_t {
    Unknown = 0,
    Cd52,
    Cd54,
    Cd54R,
    Cd54S,
};

enum class PenModuleProtocolHint : uint8_t {
    Auto = 0,
    Hpp2,
    Hpp3,
};

struct PenModuleModelInfo {
    uint32_t modelId = 0;
    PenModuleModel model = PenModuleModel::Unknown;
    PenModuleProtocolHint protocolHint = PenModuleProtocolHint::Auto;
    const char* name = "Unknown";
};

constexpr uint32_t kPenModuleModelIdCd52 = 0x00011Au;
constexpr uint32_t kPenModuleModelIdCd54 = 0x00011Bu;
constexpr uint32_t kPenModuleModelIdCd54R = 0x01011Bu;
constexpr uint32_t kPenModuleModelIdCd54S = 0x443002u;

inline std::optional<uint32_t> TryParsePenModuleModelId(
        std::span<const uint8_t> payload,
        uint8_t payloadLength) noexcept {
    if (payloadLength == 0 || payloadLength > 4 || payload.size() < payloadLength) {
        return std::nullopt;
    }

    uint32_t modelId = 0;
    for (uint8_t i = 0; i < payloadLength; ++i) {
        modelId |= static_cast<uint32_t>(payload[i]) << (8u * i);
    }
    return modelId;
}

constexpr PenModuleModelInfo ResolvePenModuleModel(uint32_t modelId) noexcept {
    switch (modelId) {
    case kPenModuleModelIdCd52:
        return PenModuleModelInfo{modelId, PenModuleModel::Cd52,
                                  PenModuleProtocolHint::Hpp2, "CD52"};
    case kPenModuleModelIdCd54:
        return PenModuleModelInfo{modelId, PenModuleModel::Cd54,
                                  PenModuleProtocolHint::Hpp3, "CD54"};
    case kPenModuleModelIdCd54R:
        return PenModuleModelInfo{modelId, PenModuleModel::Cd54R,
                                  PenModuleProtocolHint::Hpp3, "CD54R"};
    case kPenModuleModelIdCd54S:
        return PenModuleModelInfo{modelId, PenModuleModel::Cd54S,
                                  PenModuleProtocolHint::Hpp3, "CD54S"};
    default:
        return PenModuleModelInfo{modelId, PenModuleModel::Unknown,
                                  PenModuleProtocolHint::Hpp3, "Unknown"};
    }
}

inline std::optional<PenModuleModelInfo> TryResolvePenModuleModelFromText(
        std::string_view text) noexcept {
    if (text.find("CD54S") != std::string_view::npos) {
        return ResolvePenModuleModel(kPenModuleModelIdCd54S);
    }
    if (text.find("CD54R") != std::string_view::npos) {
        return ResolvePenModuleModel(kPenModuleModelIdCd54R);
    }
    if (text.find("CD54") != std::string_view::npos) {
        return ResolvePenModuleModel(kPenModuleModelIdCd54);
    }
    if (text.find("CD52") != std::string_view::npos) {
        return ResolvePenModuleModel(kPenModuleModelIdCd52);
    }
    return std::nullopt;
}

constexpr std::optional<uint8_t> TryResolveStylusIdFromPenModule(
        PenModuleModel model) noexcept {
    switch (model) {
    case PenModuleModel::Cd52:
        return 1;
    case PenModuleModel::Cd54:
    case PenModuleModel::Cd54R:
    case PenModuleModel::Cd54S:
        return 2;
    default:
        return std::nullopt;
    }
}

constexpr const char* ToString(PenModuleModel model) noexcept {
    switch (model) {
    case PenModuleModel::Cd52: return "CD52";
    case PenModuleModel::Cd54: return "CD54";
    case PenModuleModel::Cd54R: return "CD54R";
    case PenModuleModel::Cd54S: return "CD54S";
    default: return "Unknown";
    }
}

// 面向用户的产品名。代号（ToString）留给日志和诊断，两者不能混用：代号必须稳定可 grep，
// 产品名则要随华为的叫法走。
//
// 对照取自华为官方《手写笔与平板/笔记本适配清单》：
//   CD52            → M-Pencil（第一代）
//   CD54 / CD54-L   → M-Pencil（第二代）
//   CD54-S/-S-L     → M-Pencil（第三代）
//
// CD54R 不在那份清单里，它不是零售型号——只出现在华为选件中心的 CD54RPenApp.dll 插件和这里
// 的 0x01011B。它按第二代显示，依据是实机：MateBook E Go 随附的这支报 0x01011B，而机主确认
// 手上是第二代。0x01011B 与 CD54 的 0x00011B 只差一个字节，同批 DLL 里还有
// CD54_VENDOR_SUNWODA / CD54_VENDOR_LONGQI / CD54_L_TYPE，与「二代的某个代工或长短款变体」
// 一致。
//
// 不要拿选件中心的产品图反推代数：65819_00.png（CD54R）与 4468738_00.png（CD54S）逐字节相
// 同，美术资源是跨型号复用的。这条线索曾指向第三代，是错的——图的颜色（白色）与实物相符，
// 但两个型号共用一张图这件事本身说明不了它们同代。
constexpr const char* ToDisplayName(PenModuleModel model) noexcept {
    switch (model) {
    case PenModuleModel::Cd52:  return "HUAWEI M-Pencil（第一代）";
    case PenModuleModel::Cd54:
    case PenModuleModel::Cd54R: return "HUAWEI M-Pencil（第二代）";
    case PenModuleModel::Cd54S: return "HUAWEI M-Pencil（第三代）";
    default: return "";
    }
}

constexpr const char* ToString(PenModuleProtocolHint hint) noexcept {
    switch (hint) {
    case PenModuleProtocolHint::Hpp2: return "Hpp2";
    case PenModuleProtocolHint::Hpp3: return "Hpp3";
    default: return "Auto";
    }
}

} // namespace Himax::Pen
