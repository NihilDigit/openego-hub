// 充电阈值的读与写。两者都是 OemWMIMethod::OemWMIfun 上的一条命令，只差 SFID 一个字节。
//
// 写请求的字段含义取自 goodies 的 Set-ChargeLimit.ps1：
//   [0] 0x03  MFID
//   [1] 0x15  SFID = SBCM
//   [2] 0x01  SBCM.CHMD  充电模式
//   [3] 0x48  SBCM.DELY  EC 开始限充所要求的连续接电小时数
//   [4]       SBCM.STCP  开始充电的电量阈值
//   [5]       SBCM.SOCP  停止充电的电量阈值
//
// 读请求只有 MFID 与 SFID=0x16，无载荷；返回的 u8Output 前五字节与写请求的参数一一对应，
// 见下面的 kOffset*。这一段是实测确认的：写入 80 之后 out[3]/out[4] 变为 75/80，
// out[1] 由 4 变为 1。
//
// 不用 HardwareHal 的 Battery::GetChargeThreshold：它把 size=2 传给一个要求 size>=4 的
// 分支，任何情况下都返回 false 且一个字节都不写，见 docs/hardware-hal.md。

#include "power/ChargeLimit.h"

#include "power/OemWmi.h"

namespace Gaokun::Power {

namespace {

constexpr uint8_t kMfid = 0x03;
constexpr uint8_t kSfidWrite = 0x15;
constexpr uint8_t kSfidRead = 0x16;

// 读回的字节位序。out[0] 是命令状态，实测始终为 0，没有见过非零值，因此不据此判定失败。
constexpr size_t kOffsetMode = 1;
constexpr size_t kOffsetDelay = 2;
constexpr size_t kOffsetStart = 3;
constexpr size_t kOffsetStop = 4;
constexpr size_t kResponseSize = 8;

} // namespace

namespace Detail {

Result ReadChargeThreshold(ChargeThreshold &out, HRESULT &failure) noexcept {
    out = ChargeThreshold{};

    const uint8_t request[]{kMfid, kSfidRead};
    uint8_t response[kResponseSize]{};
    const Result result = Oem::Invoke(request, sizeof(request), response, sizeof(response),
                                      false, failure);
    if (result != Result::Ok) return result;

    out.mode = response[kOffsetMode];
    out.delay = response[kOffsetDelay];
    out.startPercent = response[kOffsetStart];
    out.stopPercent = response[kOffsetStop];

    // 阈值恒在 [0, 100]。越界说明位序对不上，报 Unsupported 而不是把噪声当读数交出去。
    if (out.startPercent > 100 || out.stopPercent > 100) return Result::Unsupported;
    return Result::Ok;
}

Result SetChargeLimit(int stopPercent, bool dryRun, HRESULT &failure) noexcept {
    if (stopPercent < kMinChargeLimit || stopPercent > kMaxChargeLimit) {
        failure = E_INVALIDARG;
        return Result::Failed;
    }

    const uint8_t request[]{
        kMfid, kSfidWrite, kChargeModeManual, kSmartChargeDelayHours,
        static_cast<uint8_t>(stopPercent - kChargeStartOffset),
        static_cast<uint8_t>(stopPercent),
    };
    return Oem::Invoke(request, sizeof(request), nullptr, 0, dryRun, failure);
}

// 切到智能充电。四个字节全部是常量，与原厂写下的一组一致。
//
// 阈值不沿用 EC 里的当前值。智能模式下这两个数不是「用户上一次设的上限」，而是限充生效
// 之后要维持的区间，原厂固定 65/70；沿用当前值会让同一个按钮按出不同结果——按之前手动
// 设过 90，智能充电就变成维持 90。
Result SetSmartCharge(bool dryRun, HRESULT &failure) noexcept {
    const uint8_t request[]{
        kMfid, kSfidWrite, kChargeModeSmart, kSmartChargeDelayHours,
        kSmartChargeStartPercent, kSmartChargeStopPercent,
    };
    return Oem::Invoke(request, sizeof(request), nullptr, 0, dryRun, failure);
}

} // namespace Detail

Result ReadChargeThreshold(ChargeThreshold &out) noexcept {
    HRESULT failure = S_OK;
    return Detail::ReadChargeThreshold(out, failure);
}

Result SetChargeLimit(int stopPercent) noexcept {
    HRESULT failure = S_OK;
    return Detail::SetChargeLimit(stopPercent, false, failure);
}

Result SetSmartCharge() noexcept {
    HRESULT failure = S_OK;
    return Detail::SetSmartCharge(false, failure);
}

} // namespace Gaokun::Power
