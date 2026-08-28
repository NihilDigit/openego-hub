#pragma once

#include <cstdint>

// 电池信息与充电阈值。这一组全部走 Win32 与 WMI，不加载任何华为 DLL，因此实现是原生
// ARM64，调用方不必改成 ARM64EC——与 GaokunPen.h、GaokunKeyboard.h 的读者侧同一套约定，
// 但这里没有宿主进程，直接在调用方的线程里同步读取。
//
// 接口按代价分成两组，这是本头文件的结构要点：
//
//   LiveState    GetSystemPowerStatus 与 CallNtPowerInformation，实测 0.02-0.07 ms；放电时
//                多一次电池设备的 IOCTL 取剩余时间，连开设备实测 0.2 ms。可以 1 Hz 甚至
//                更快地刷。
//   BatteryInfo  电池设备的 IOCTL。首次约 35 ms——驱动要发起一次真实的 ACPI 事务；此后
//                驱动有缓存，约 0.3 ms。打开面板时读一次即可。
//
// 首次那 35 ms 不能落在 UI 线程上，这是分成两组的理由。满充容量两边都有：LiveState 里的
// 那份是随手带回的，要设计容量、健康度、循环次数才需要 BatteryInfo。
namespace Gaokun::Power {

// 读不到时上层需要区分「没权限」与「硬件不支持」，两者的 UI 处置不同：前者提示以服务身份
// 运行，后者隐藏卡片。一律返回 0 会让两者都显示成「0%」。
enum class Result : uint32_t {
    Ok = 0,
    AccessDenied,  ///< 需要管理员权限。读写充电阈值走 ROOT\WMI 的 OemWMIMethod，非提升进程
                   ///< 枚举不到实例；生产路径上 OpenEGoHubService 以 LocalSystem 运行。
    NoBattery,     ///< 系统报告没有电池
    Unsupported,   ///< 固件或驱动没有实现这项数据
    Failed,        ///< 通道本身出错
};

[[nodiscard]] const wchar_t *ToString(Result result) noexcept;

// ---- 高频项 ----

inline constexpr uint8_t kPercentUnknown = 0xFF;
inline constexpr uint32_t kSecondsUnknown = 0xFFFFFFFFu;

// remainingSeconds 量的是什么。两个方向的数来自不同的地方，混成一个「剩余时间」会让上层
// 把「还能用 2 小时」和「充满还要 2 小时」显示成同一句话。
enum class TimeKind : uint8_t {
    Unknown = 0,  ///< 取不到。与「取到 0」区分开：0 秒是真的快没电了
    ToEmpty,      ///< 还能用多久。电池固件自己的估计
    ToFull,       ///< 充满还要多久。由容量差除以当前功率算出，见 remainingSeconds
};

struct LiveState {
    uint8_t percent = kPercentUnknown;  ///< 0-100
    bool batteryPresent = false;
    bool acOnline = false;
    // 方向以 powerMilliWatt 的符号为准。系统上报的这两个标志实测会同时置位（切换到充电的
    // 那一刻，Charging 已置位而 Discharging 还没落），照抄会让 UI 同时显示充电与放电，
    // 所以功率非零时这两个字段由符号定，为零时才退回系统的标志。
    bool charging = false;
    // 接着电源也可能在放电：充电阈值生效时系统由电池供电到起充点。因此不能用 acOnline
    // 反推放电状态。
    bool discharging = false;
    uint32_t remainingCapacityMWh = 0;
    uint32_t fullChargedCapacityMWh = 0;
    // 正为充电，负为放电，0 为静止或读不到。厂商 HardwareHal 把充放电拆成两个只在对应
    // 状态下才成功的导出，调用方拿到失败无从区分「没在充」与「读不到」，这里合成一个带
    // 符号的功率。
    int32_t powerMilliWatt = 0;

    // 剩余时间。kind 为 Unknown 时恒为 kSecondsUnknown。
    //
    // 放电：取电池设备 IOCTL 的 BatteryEstimatedTime，这是固件自己的估计。不用
    // SYSTEM_BATTERY_STATE.EstimatedTime 或 GetSystemPowerStatus.BatteryLifeTime——接着
    // 电源时这两个恒为未知，而充电阈值生效时机器正是「插着电但在放电」，那一段最需要这个数。
    // 也不用「剩余容量除以当前功率」代替：同一时刻固件报 10289 s，这个算式给 18034 s，
    // 差了将近一倍，固件显然算进了低电量截止与放电曲线。两者不能互相顶替，固件报不出时
    // 就是 Unknown。
    //
    // 充电：固件在充电时一律返回未知（实测），所以这一侧是本层用 (满充 - 剩余) / 功率
    // 算出来的，不计充电末段的涓流减速。
    //
    // 它算的是充到满充容量的时间，没有考虑停充阈值。限到 80% 时机器会在 80% 停下，这个
    // 数永远不会兑现，等到的是充电停住、时间还剩一截。要会兑现的数字就调 SecondsToPercent
    // 并把阈值传进去；阈值读不回来（非提权）时才退回这里的 ToFull。
    uint32_t remainingSeconds = kSecondsUnknown;
    TimeKind remainingKind = TimeKind::Unknown;
};

// 充到指定电量还需要多久，秒。停充阈值生效时「充满」并不会发生，上层把阈值传进来才能得到
// 会兑现的数字。非充电状态、功率为零、目标不高于当前电量时返回 kSecondsUnknown。
//
// 目标电量按容量折算，不按 LiveState::percent——后者是系统上报的整数百分比，与容量之比
// 相差一两个点，用它会让快到目标时的读数在几分钟与「已到达」之间跳。
//
// 实测 84% 充到 95%、每 15 秒一采的结果（与 ToFull 同一个算式，结论共用）：
//
// 接近目标时偏悲观，不是偏乐观。91% 之后预测比实际多 10-100 秒，电量跳到 95% 的那一刻还
// 显示着 86 秒。根因是硬件按自己的判据停充：实际停在 34861 mWh，而 95% × 满充是 35042 mWh，
// 差 181 mWh（0.49 个百分点），按当时 7.5-8.4 W 折算正好约 80 秒。这半个百分点不做修正,
// 偏悲观是安全方向——多显示一分钟，好过让用户等一个已经过去的时刻——且它是硬件的判据与
// 容量折算之间的差，会随电池老化变化，用实测系数硬校准只对这一块电池的这个时刻成立。
//
// 涓流减速确实存在但更小：最后一个多百分点里净充电功率从约 8450 mW 降到 7470 mW，约一成。
// 更早那段从 12.7 W 掉到 8.4 W 不是涓流，是系统负载变了——同一电量下这两档来回跳过。
//
// 误差的大头正是这个负载：中段预测会差出 ±300 秒，方向取决于此后负载往哪边变，比上面两项
// 都大一个量级。上层要显示得稳，应当对功率或对结果做平滑，不要直接印瞬时值。
[[nodiscard]] uint32_t SecondsToPercent(const LiveState &state, int targetPercent) noexcept;

[[nodiscard]] Result ReadLiveState(LiveState &out) noexcept;

// ---- 低频项 ----

inline constexpr int kTextCapacity = 64;

// 字符串是 UTF-8。取自电池设备的 IOCTL，不是 root\wmi 的 BatteryStaticData——后者在本机
// 取实例即报「常规故障」，也不是 HardwareHal 的 GetBatteryName（它查的正是那张表）。
struct BatteryInfo {
    uint32_t designCapacityMWh = 0;
    uint32_t fullChargedCapacityMWh = 0;
    uint32_t cycleCount = 0;             ///< 0 表示驱动没有提供
    uint32_t voltageMilliVolt = 0;
    // 电池按相对单位而非 mWh 上报容量时置位。此时上面三个容量字段之间的比值仍然有效，
    // 但不能标 mWh。GK-W7X 上为 false。
    bool capacityRelative = false;
    char deviceName[kTextCapacity]{};
    char manufacturer[kTextCapacity]{};
    char serialNumber[kTextCapacity]{};
    char chemistry[8]{};                 ///< 四字符代号，如 LION
};

[[nodiscard]] Result ReadBatteryInfo(BatteryInfo &out) noexcept;

// 健康度。满充与设计容量之比，两者单位相同，capacityRelative 时同样成立。
[[nodiscard]] double HealthPercent(const BatteryInfo &info) noexcept;

// ---- 充电阈值 ----

inline constexpr int kMinChargeLimit = 50;
inline constexpr int kMaxChargeLimit = 100;
// 起充固定比停充低 5，这是原厂的取值方式：两者相等会让电池在阈值附近反复小幅充放。
inline constexpr int kChargeStartOffset = 5;

// 用户设定的手动阈值。此时阈值硬性生效。
inline constexpr uint8_t kChargeModeManual = 1;
// 华为 PC 管家的智能充电。阈值由系统按使用习惯动态调整、并不硬性生效——实测阈值写着 70
// 而电池充到了 100%。UI 必须把两种模式分开显示，所以读取接口连同模式一起返回，而不是
// 只给一个百分比。
inline constexpr uint8_t kChargeModeSmart = 4;

struct ChargeThreshold {
    uint8_t mode = 0;          ///< SBCM.CHMD
    uint8_t delay = 0;         ///< SBCM.DELY，单位未知；写入路径固定用 0x18
    uint8_t startPercent = 0;  ///< SBCM.STCP
    uint8_t stopPercent = 0;   ///< SBCM.SOCP

    [[nodiscard]] bool IsManual() const noexcept { return mode == kChargeModeManual; }
};

// 需要管理员权限。不用 HardwareHal 的 Battery::GetChargeThreshold：它把 size=2 传给一个
// 要求 size>=4 的分支，任何情况下都返回 false 且一个字节都不写。
[[nodiscard]] Result ReadChargeThreshold(ChargeThreshold &out) noexcept;

// 需要管理员权限。stopPercent 取 [kMinChargeLimit, kMaxChargeLimit]，起充点由实现按
// kChargeStartOffset 推出。写入后 mode 变为 kChargeModeManual。
[[nodiscard]] Result SetChargeLimit(int stopPercent) noexcept;

// 需要管理员权限。把充电交还给厂商的智能充电模式，写入后 mode 变为 kChargeModeSmart，
// 阈值随后由系统按使用习惯自行调整，界面上的上限数字不再代表一个会兑现的设定。
[[nodiscard]] Result SetSmartCharge() noexcept;

} // namespace Gaokun::Power
