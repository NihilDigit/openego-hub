// 电池信息。两条通路，代价差三个数量级：
//
//   电源状态 API   GetSystemPowerStatus / CallNtPowerInformation，实测 0.02-0.07 ms。
//   电池设备 IOCTL 首次约 35 ms，此后驱动有缓存，约 0.3 ms。
//
// 全程不碰厂商 DLL。docs/hardware-hal.md 记的「电池型号与制造商不可行」是针对 root\wmi 的
// BatteryStaticData 与 HardwareHal 的 GetBatteryName（后者查的正是那张表）；电池设备的
// IOCTL_BATTERY_QUERY_INFORMATION 能给出型号、制造商、序列号与化学体系，且不需要提权。
// 循环次数与设计容量同样从这里来，于是低频项一条 WMI 都不用走。

#include "GaokunPower.h"

#include <windows.h>

#include <batclass.h>
#include <devguid.h>
#include <powrprof.h>
#include <setupapi.h>

#include <cstring>

#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "setupapi.lib")

namespace Gaokun::Power {

namespace {

// 电池设备的一次会话。IOCTL 要先取 BatteryTag，之后每条查询都带着它——电池被换掉时 tag
// 失效，驱动据此拒绝陈旧的查询。
class BatteryDevice {
public:
    ~BatteryDevice() noexcept {
        if (m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle);
    }

    BatteryDevice(const BatteryDevice &) = delete;
    BatteryDevice &operator=(const BatteryDevice &) = delete;
    BatteryDevice() noexcept = default;

    // 只取第一块电池。GK-W7X 是单电池机型，多电池的合并策略要等有实机再定，凭空写一套
    // 求和逻辑无法验证。
    [[nodiscard]] Result Open() noexcept {
        HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_BATTERY, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE) return Result::Failed;

        Result result = Result::NoBattery;
        SP_DEVICE_INTERFACE_DATA face{};
        face.cbSize = sizeof(face);
        if (SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVCLASS_BATTERY, 0, &face)) {
            result = OpenInterface(set, face);
        }
        SetupDiDestroyDeviceInfoList(set);
        return result;
    }

    [[nodiscard]] bool Query(BATTERY_QUERY_INFORMATION_LEVEL level, void *out,
                             DWORD outSize) const noexcept {
        BATTERY_QUERY_INFORMATION query{};
        query.BatteryTag = m_tag;
        query.InformationLevel = level;
        DWORD written = 0;
        return DeviceIoControl(m_handle, IOCTL_BATTERY_QUERY_INFORMATION, &query, sizeof(query),
                               out, outSize, &written, nullptr) != FALSE;
    }

    [[nodiscard]] bool QueryStatus(BATTERY_STATUS &out) const noexcept {
        BATTERY_WAIT_STATUS wait{};
        wait.BatteryTag = m_tag;
        DWORD written = 0;
        return DeviceIoControl(m_handle, IOCTL_BATTERY_QUERY_STATUS, &wait, sizeof(wait),
                               &out, sizeof(out), &written, nullptr) != FALSE;
    }

private:
    [[nodiscard]] Result OpenInterface(HDEVINFO set, SP_DEVICE_INTERFACE_DATA &face) noexcept {
        // 先问长度再分配：接口详情的大小随设备路径变化。cbSize 填的是结构体头部的大小，
        // 不是缓冲区的大小，这是这套 API 的老陷阱。
        DWORD needed = 0;
        (void)SetupDiGetDeviceInterfaceDetailW(set, &face, nullptr, 0, &needed, nullptr);
        if (needed == 0) return Result::Failed;

        auto *detail = static_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(
            HeapAlloc(GetProcessHeap(), 0, needed));
        if (!detail) return Result::Failed;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        Result result = Result::Failed;
        if (SetupDiGetDeviceInterfaceDetailW(set, &face, detail, needed, &needed, nullptr)) {
            m_handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (m_handle == INVALID_HANDLE_VALUE) {
                result = GetLastError() == ERROR_ACCESS_DENIED ? Result::AccessDenied
                                                               : Result::Failed;
            } else {
                result = AcquireTag();
            }
        }
        HeapFree(GetProcessHeap(), 0, detail);
        return result;
    }

    [[nodiscard]] Result AcquireTag() noexcept {
        ULONG timeout = 0;  // 不等待。电池不在位时立刻失败，而不是把调用方挂住。
        DWORD written = 0;
        if (!DeviceIoControl(m_handle, IOCTL_BATTERY_QUERY_TAG, &timeout, sizeof(timeout),
                             &m_tag, sizeof(m_tag), &written, nullptr)) {
            return GetLastError() == ERROR_FILE_NOT_FOUND ? Result::NoBattery : Result::Failed;
        }
        return Result::Ok;
    }

    HANDLE m_handle = INVALID_HANDLE_VALUE;
    ULONG m_tag = 0;
};

// UTF-16 转 UTF-8，写不下时退到一个完整字符的边界。中途截断会切开多字节序列，渲染侧解码
// 直接失败——与 Pen/Keyboard 的产品名同一处考量。
void CopyText(const wchar_t *text, char *out, int capacity) noexcept {
    out[0] = '\0';
    if (!text || !text[0]) return;

    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return;
    if (size <= capacity) {
        (void)WideCharToMultiByte(CP_UTF8, 0, text, -1, out, capacity, nullptr, nullptr);
        return;
    }

    (void)WideCharToMultiByte(CP_UTF8, 0, text, -1, out, capacity, nullptr, nullptr);
    int end = capacity - 1;
    while (end > 0 && (static_cast<unsigned char>(out[end - 1]) & 0xC0) == 0x80) --end;
    if (end > 0 && (static_cast<unsigned char>(out[end - 1]) & 0x80) != 0) --end;
    out[end] = '\0';
}

void ReadText(const BatteryDevice &device, BATTERY_QUERY_INFORMATION_LEVEL level, char *out,
              int capacity) noexcept {
    wchar_t buffer[128]{};
    if (device.Query(level, buffer, sizeof(buffer) - sizeof(wchar_t))) {
        CopyText(buffer, out, capacity);
    }
}

// 放电：问固件。这条 IOCTL 是唯一在「插着电但在放电」时仍给得出数的来源，充电阈值生效
// 期间机器一直处于这个状态。代价实测 0.01-0.02 ms，连开设备 0.2 ms；进程内第一次约 30 ms,
// 驱动此后有缓存。
void ReadTimeToEmpty(LiveState &out) noexcept {
    BatteryDevice device;
    if (device.Open() != Result::Ok) return;

    ULONG seconds = 0;
    if (!device.Query(BatteryEstimatedTime, &seconds, sizeof(seconds))) return;
    // 固件也会在放电中途报未知，实测见过连续采样里插进来一次。这时留 Unknown，UI 显示
    // 「—」，不要拿别的算式顶上去——两者差将近一倍，跳变会比空着更难解释。
    if (seconds == BATTERY_UNKNOWN_TIME) return;

    out.remainingSeconds = seconds;
    out.remainingKind = TimeKind::ToEmpty;
}

// 充电：固件在充电时一律返回未知（实测三次采样全是 BATTERY_UNKNOWN_TIME），只能自己算。
void ComputeTimeToFull(LiveState &out) noexcept {
    const uint32_t seconds = SecondsToPercent(out, 100);
    if (seconds == kSecondsUnknown) return;

    out.remainingSeconds = seconds;
    out.remainingKind = TimeKind::ToFull;
}

} // namespace

const wchar_t *ToString(Result result) noexcept {
    switch (result) {
    case Result::Ok:           return L"ok";
    case Result::AccessDenied: return L"access denied";
    case Result::NoBattery:    return L"no battery";
    case Result::Unsupported:  return L"unsupported";
    case Result::Failed:       return L"failed";
    }
    return L"failed";
}

uint32_t SecondsToPercent(const LiveState &state, int targetPercent) noexcept {
    if (!state.charging || state.powerMilliWatt <= 0) return kSecondsUnknown;
    if (targetPercent <= 0 || targetPercent > 100) return kSecondsUnknown;
    if (state.fullChargedCapacityMWh == 0) return kSecondsUnknown;

    const uint64_t target =
        static_cast<uint64_t>(state.fullChargedCapacityMWh) * targetPercent / 100;
    if (target <= state.remainingCapacityMWh) return kSecondsUnknown;

    const uint64_t missing = target - state.remainingCapacityMWh;
    return static_cast<uint32_t>(missing * 3600 / state.powerMilliWatt);
}

double HealthPercent(const BatteryInfo &info) noexcept {
    if (info.designCapacityMWh == 0) return 0.0;
    return 100.0 * info.fullChargedCapacityMWh / info.designCapacityMWh;
}

Result ReadLiveState(LiveState &out) noexcept {
    out = LiveState{};

    SYSTEM_BATTERY_STATE state{};
    const NTSTATUS status = CallNtPowerInformation(SystemBatteryState, nullptr, 0, &state,
                                                  sizeof(state));
    if (status != 0) return Result::Failed;
    if (!state.BatteryPresent) return Result::NoBattery;

    out.batteryPresent = true;
    out.acOnline = state.AcOnLine != FALSE;
    out.remainingCapacityMWh = state.RemainingCapacity;
    out.fullChargedCapacityMWh = state.MaxCapacity;

    // Rate 的符号带着方向：放电为负，充电为正。厂商的两个导出各自要求一侧的状态才返回值，
    // 这里不再分叉。
    //
    // 结构体里 Rate 声明成 ULONG，但装的是有符号数，必须先转回来再比较——直接写
    // state.Rate > 0 对放电中的负值同样成立，于是放电被判成充电，而赋值给 int32 的功率
    // 又是对的，症状是「显示充电，功率却是负数」。
    const int32_t rate = static_cast<int32_t>(state.Rate);
    out.powerMilliWatt = rate;

    // 系统的两个标志实测会同时为真——刚切到充电的那一刻 Charging 已置位而 Discharging
    // 还没落，Rate 已经是正的。功率非零时以符号为准，为零时才没有别的依据。
    if (rate != 0) {
        out.charging = rate > 0;
        out.discharging = rate < 0;
    } else {
        out.charging = state.Charging != FALSE;
        out.discharging = state.Discharging != FALSE;
    }

    if (out.discharging) {
        ReadTimeToEmpty(out);
    } else if (out.charging) {
        ComputeTimeToFull(out);
    }

    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power) && power.BatteryLifePercent <= 100) {
        out.percent = power.BatteryLifePercent;
    } else if (state.MaxCapacity > 0) {
        // 系统报不出百分比时按容量折算，而不是留 kPercentUnknown：容量本身是可信的。
        out.percent = static_cast<uint8_t>(
            (static_cast<uint64_t>(state.RemainingCapacity) * 100 + state.MaxCapacity / 2) /
            state.MaxCapacity);
    }
    return Result::Ok;
}

Result ReadBatteryInfo(BatteryInfo &out) noexcept {
    out = BatteryInfo{};

    BatteryDevice device;
    const Result opened = device.Open();
    if (opened != Result::Ok) return opened;

    BATTERY_INFORMATION information{};
    if (!device.Query(BatteryInformation, &information, sizeof(information))) {
        return Result::Unsupported;
    }
    out.designCapacityMWh = information.DesignedCapacity;
    out.fullChargedCapacityMWh = information.FullChargedCapacity;
    out.cycleCount = information.CycleCount;
    out.capacityRelative = (information.Capabilities & BATTERY_CAPACITY_RELATIVE) != 0;
    // Chemistry 是四个字符，不带结尾零。
    memcpy(out.chemistry, information.Chemistry, sizeof(information.Chemistry));
    out.chemistry[sizeof(information.Chemistry)] = '\0';

    ReadText(device, BatteryDeviceName, out.deviceName, kTextCapacity);
    ReadText(device, BatteryManufactureName, out.manufacturer, kTextCapacity);
    ReadText(device, BatterySerialNumber, out.serialNumber, kTextCapacity);

    // 电压只有这一条通路。root\wmi 的 BatteryStatus 也给电压，但那要连 WMI，代价比这条
    // 已经打开的 IOCTL 通道更高。
    BATTERY_STATUS status{};
    if (device.QueryStatus(status)) {
        out.voltageMilliVolt = status.Voltage;
    }
    return Result::Ok;
}

} // namespace Gaokun::Power
