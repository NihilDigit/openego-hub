#include "pch.h"

#include "DeviceInfo.h"

#include <algorithm>
#include <cwchar>

namespace DeviceInfo {

namespace {

// SMBIOS 表通过 GetSystemFirmwareTable 的 'RSMB' 取出，前面有一段自己的表头，
// 结构数据从 Length 之后开始。
#pragma pack(push, 1)
struct RawSmbiosData {
    uint8_t used20CallingMethod;
    uint8_t majorVersion;
    uint8_t minorVersion;
    uint8_t dmiRevision;
    uint32_t length;
    uint8_t data[1];
};

struct StructureHeader {
    uint8_t type;
    uint8_t length;   // 只是格式化区的长度，后面还有字符串区
    uint16_t handle;
};
#pragma pack(pop)

// 固件没填的字段不会留空，而是写一个占位词。本机的内存厂商和料号都是字面的 "Null"，
// 照原样显示出来比不显示这一行更糟，所以在取值这一层就统一滤掉。
bool IsPlaceholder(const std::wstring& value) {
    static constexpr const wchar_t* kPlaceholders[] = {
        L"Null", L"None", L"Unknown", L"Not Specified", L"Default string",
        L"To Be Filled By O.E.M.", L"System Serial Number", L"Not Applicable",
    };
    for (const wchar_t* placeholder : kPlaceholders) {
        if (_wcsicmp(value.c_str(), placeholder) == 0) return true;
    }
    return false;
}

// SMBIOS 里每个结构的格式化区之后跟一串以 NUL 分隔的字符串，整段以连续两个 NUL 收尾。
// 字段里存的是序号，从 1 开始；0 表示没有值。
std::wstring StringAt(const uint8_t* structure, size_t available, uint8_t index) {
    if (index == 0) return {};
    const auto* header = reinterpret_cast<const StructureHeader*>(structure);
    if (header->length >= available) return {};

    const char* cursor = reinterpret_cast<const char*>(structure) + header->length;
    const char* end = reinterpret_cast<const char*>(structure) + available;

    for (uint8_t i = 1; cursor < end; ++i) {
        const size_t length = strnlen(cursor, static_cast<size_t>(end - cursor));
        if (length == 0) break;   // 连续两个 NUL，字符串区结束
        if (i == index) {
            // SMBIOS 的字符串是 ASCII，逐字节放大即可，不必过 MultiByteToWideChar。
            std::wstring value(cursor, cursor + length);
            return IsPlaceholder(value) ? std::wstring{} : value;
        }
        cursor += length + 1;
    }
    return {};
}

// 整个结构（格式化区 + 字符串区）的长度，用来走到下一个结构。
size_t StructureSize(const uint8_t* structure, size_t available) {
    const auto* header = reinterpret_cast<const StructureHeader*>(structure);
    if (header->length < sizeof(StructureHeader) || header->length > available) return 0;

    size_t offset = header->length;
    // 字符串区至少是一个 NUL；两个连续 NUL 才算结束。
    while (offset + 1 < available) {
        if (structure[offset] == 0 && structure[offset + 1] == 0) return offset + 2;
        ++offset;
    }
    return 0;
}

const wchar_t* MemoryTypeName(uint8_t code) {
    // SMBIOS 3.4 的 Memory Type 枚举，只列本机可能出现的几种。
    switch (code) {
    case 0x18: return L"DDR3";
    case 0x1A: return L"DDR4";
    case 0x1B: return L"LPDDR";
    case 0x1C: return L"LPDDR2";
    case 0x1D: return L"LPDDR3";
    case 0x1E: return L"LPDDR4";
    case 0x20: return L"HBM";
    case 0x21: return L"HBM2";
    case 0x22: return L"DDR5";
    case 0x23: return L"LPDDR5";
    default:   return L"";
    }
}

void ParseSmbios(Info& info) {
    const UINT size = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
    if (size == 0) return;

    std::vector<uint8_t> buffer(size);
    if (GetSystemFirmwareTable('RSMB', 0, buffer.data(), size) != size) return;
    if (size < offsetof(RawSmbiosData, data)) return;

    const auto* raw = reinterpret_cast<const RawSmbiosData*>(buffer.data());
    const uint8_t* table = raw->data;
    const size_t tableSize =
        std::min<size_t>(raw->length, size - offsetof(RawSmbiosData, data));

    size_t offset = 0;
    while (offset + sizeof(StructureHeader) <= tableSize) {
        const uint8_t* structure = table + offset;
        const auto* header = reinterpret_cast<const StructureHeader*>(structure);
        const size_t remaining = tableSize - offset;
        const size_t total = StructureSize(structure, remaining);
        if (total == 0) break;

        switch (header->type) {
        case 0:   // BIOS Information
            if (header->length >= 0x09) {
                info.biosVendor = StringAt(structure, total, structure[0x04]);
                info.biosVersion = StringAt(structure, total, structure[0x05]);
                info.biosDate = StringAt(structure, total, structure[0x08]);
            }
            break;

        case 1:   // System Information
            if (header->length >= 0x08) {
                info.manufacturer = StringAt(structure, total, structure[0x04]);
                info.productName = StringAt(structure, total, structure[0x05]);
                info.version = StringAt(structure, total, structure[0x06]);
                info.serialNumber = StringAt(structure, total, structure[0x07]);
            }
            // SKU 与 Family 是 SMBIOS 2.4 才加的，短结构里没有这两个字段。
            if (header->length >= 0x1A) {
                info.skuNumber = StringAt(structure, total, structure[0x19]);
            }
            if (header->length >= 0x1B) {
                info.family = StringAt(structure, total, structure[0x1A]);
            }
            break;

        case 4:   // Processor Information
            if (header->length >= 0x11 && info.processor.empty()) {
                info.processor = StringAt(structure, total, structure[0x10]);
            }
            if (header->length >= 0x24 && info.processorCores == 0) {
                info.processorCores = structure[0x23];
            }
            break;

        case 17: { // Memory Device
            if (header->length < 0x15) break;

            MemoryModule module;
            uint16_t sizeField = 0;
            std::memcpy(&sizeField, structure + 0x0C, sizeof(sizeField));
            if (sizeField == 0) break;          // 空槽位

            if (sizeField == 0x7FFF) {
                // 单条超过 32GB-1MB 时真实容量在扩展字段里，单位是 MB。
                if (header->length >= 0x20) {
                    uint32_t extended = 0;
                    std::memcpy(&extended, structure + 0x1C, sizeof(extended));
                    module.sizeBytes = static_cast<uint64_t>(extended) * 1024 * 1024;
                }
            } else {
                // 最高位表示单位：置位是 KB，清零是 MB。
                const uint64_t value = sizeField & 0x7FFF;
                module.sizeBytes = (sizeField & 0x8000) ? value * 1024 : value * 1024 * 1024;
            }

            if (header->length >= 0x13) module.type = MemoryTypeName(structure[0x12]);
            if (header->length >= 0x17) {
                uint16_t speed = 0;
                std::memcpy(&speed, structure + 0x15, sizeof(speed));
                module.speedMhz = speed;
            }
            if (header->length >= 0x18) {
                module.manufacturer = StringAt(structure, total, structure[0x17]);
            }
            if (header->length >= 0x1B) {
                module.partNumber = StringAt(structure, total, structure[0x1A]);
            }

            info.memoryModules.push_back(std::move(module));
            break;
        }

        default:
            break;
        }

        // 类型 127 是结束标记。
        if (header->type == 127) break;
        offset += total;
    }
}

std::wstring ReadRegistryString(HKEY root, const wchar_t* subKey, const wchar_t* value) {
    wchar_t buffer[512]{};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    if (RegGetValueW(root, subKey, value, RRF_RT_REG_SZ, &type, buffer, &size) != ERROR_SUCCESS) {
        return {};
    }
    return buffer;
}

DWORD ReadRegistryDword(HKEY root, const wchar_t* subKey, const wchar_t* value) {
    DWORD result = 0;
    DWORD size = sizeof(result);
    if (RegGetValueW(root, subKey, value, RRF_RT_REG_DWORD, nullptr, &result, &size) !=
        ERROR_SUCCESS) {
        return 0;
    }
    return result;
}

void QueryOs(Info& info) {
    constexpr const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    info.osName = ReadRegistryString(HKEY_LOCAL_MACHINE, key, L"ProductName");

    // ProductName 在 Windows 11 上仍然写着 Windows 10，得靠内部版本号纠正——微软没有改这个值，
    // 系统设置界面显示的名字也是另算的。
    const DWORD build = ReadRegistryDword(HKEY_LOCAL_MACHINE, key, L"CurrentBuildNumber");
    const std::wstring buildText =
        ReadRegistryString(HKEY_LOCAL_MACHINE, key, L"CurrentBuildNumber");
    const DWORD buildNumber = build != 0 ? build : static_cast<DWORD>(_wtoi(buildText.c_str()));
    if (buildNumber >= 22000) {
        const size_t pos = info.osName.find(L"Windows 10");
        if (pos != std::wstring::npos) info.osName.replace(pos, 10, L"Windows 11");
    }

    const std::wstring display =
        ReadRegistryString(HKEY_LOCAL_MACHINE, key, L"DisplayVersion");
    const DWORD revision = ReadRegistryDword(HKEY_LOCAL_MACHINE, key, L"UBR");

    wchar_t version[128]{};
    if (!display.empty()) {
        std::swprintf(version, std::size(version), L"%s (%lu.%lu)", display.c_str(),
                      buildNumber, revision);
    } else {
        std::swprintf(version, std::size(version), L"%lu.%lu", buildNumber, revision);
    }
    info.osVersion = version;
}

// 固件的 Family 写的是「MateBook E」，少了这一代的 Go 后缀，本机 GK-W7X 实际是
// MateBook E Go。华为自己的文件里也找不到带 Go 的名字，取不到更准的来源，只能在这里补。
//
// 补的范围限定在 GK- 前缀，也就是 Gaokun 平台（handoff 的 Product.xml 里 GK-X5X、GK-W7X、
// GK-X5XS 连号排在一起），整个系列都是 MateBook E Go。其余机型一律照用固件写的值——这一条
// 是本项目的认定，不是固件数据，不该扩大到没有依据的机型上。
void FixupFamily(Info& info) {
    if (info.family.empty()) return;
    if (info.productName.rfind(L"GK-", 0) != 0) return;
    if (info.family.find(L"Go") != std::wstring::npos) return;   // 固件哪天补上了就不必再改
    info.family += L" Go";
}

void QueryDisplay(Info& info) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) return;
    info.displayWidth = mode.dmPelsWidth;
    info.displayHeight = mode.dmPelsHeight;
    info.displayRefreshHz = mode.dmDisplayFrequency;
}

} // namespace

Info Query() noexcept {
    Info info;

    ParseSmbios(info);
    FixupFamily(info);

    if (info.processor.empty()) {
        info.processor = ReadRegistryString(
            HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    }

    // 容量优先用 SMBIOS 逐条累加的标称值。GlobalMemoryStatusEx 报的是操作系统可用的物理
    // 内存，固件保留掉一部分之后本机是 15 GB——那是「系统能用多少」，而规格页要答的是
    // 「装了多少」。固件没有逐条列全时才退回它。
    for (const auto& module : info.memoryModules) {
        info.memoryTotalBytes += module.sizeBytes;
    }
    if (info.memoryTotalBytes == 0) {
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory)) info.memoryTotalBytes = memory.ullTotalPhys;
    }

    QueryOs(info);
    QueryDisplay(info);
    return info;
}

std::wstring FormatBytes(uint64_t bytes) {
    if (bytes == 0) return {};

    // 物理内存的总量算上固件保留之后不是整数，向上取到最近的 GB 才是用户认得的那个数字。
    constexpr uint64_t gb = 1024ull * 1024 * 1024;
    if (bytes >= gb) {
        const double value = static_cast<double>(bytes) / static_cast<double>(gb);
        wchar_t text[64]{};
        std::swprintf(text, std::size(text), L"%.0f GB", value);
        return text;
    }

    wchar_t text[64]{};
    std::swprintf(text, std::size(text), L"%.0f MB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

} // namespace DeviceInfo
