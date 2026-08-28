#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 本机硬件信息。
//
// 数据取自 SMBIOS（GetSystemFirmwareTable 的 'RSMB'）与几个不需要特权的 Win32 调用，
// 不经过华为的任何 DLL。没有走 WMI：那要先初始化 COM 再过一遍 IWbemLocator 和查询语言，
// 而这里要的每一项 SMBIOS 都直接给得出，读固件表的用法项目里也已经有了（见色彩校准读
// ACPI 的 DLUT 表）。
namespace DeviceInfo {

struct MemoryModule {
    std::wstring manufacturer;
    std::wstring partNumber;
    std::wstring type;        // LPDDR5 / DDR4 ……
    uint64_t sizeBytes = 0;
    uint32_t speedMhz = 0;
};

struct Info {
    // SMBIOS 类型 1。本机是 productName=GK-W7X、version=M1010、sku=C233。
    // family 是营销名。固件给到的是「MateBook E」，少了这一代的 Go 后缀，Query 会对 Gaokun
    // 平台补上，见 FixupFamily。PC Manager 的配置里找不到 GK-W7X 到营销名的对应，华为自己的
    // 文件里也没有出现过带 Go 的写法。
    std::wstring manufacturer;
    std::wstring productName;
    std::wstring version;
    std::wstring serialNumber;
    std::wstring skuNumber;
    std::wstring family;

    // SMBIOS 类型 0
    std::wstring biosVendor;
    std::wstring biosVersion;
    std::wstring biosDate;

    // SMBIOS 类型 4，取不到时回退到注册表里的处理器名
    std::wstring processor;
    uint32_t processorCores = 0;

    // 逐条内存来自 SMBIOS 类型 17；总量另取自 GlobalMemoryStatusEx，因为焊死的内存
    // 未必逐条列全，而总量任何情况下都准。
    std::vector<MemoryModule> memoryModules;
    uint64_t memoryTotalBytes = 0;

    std::wstring osName;
    std::wstring osVersion;   // 24H2 (26220.1234)

    uint32_t displayWidth = 0;
    uint32_t displayHeight = 0;
    uint32_t displayRefreshHz = 0;
};

// 同步读取，耗时在毫秒量级。失败的项留空，调用方按空值决定是否显示该行。
[[nodiscard]] Info Query() noexcept;

// 16 GB、512 GB 这样的可读写法。
[[nodiscard]] std::wstring FormatBytes(uint64_t bytes);

} // namespace DeviceInfo
