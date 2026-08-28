// 充电阈值的读与写。两者都是 OemWMIMethod::OemWMIfun 上的一条命令，只差 SFID 一个字节。
//
// 写请求的字段含义取自 goodies 的 Set-ChargeLimit.ps1：
//   [0] 0x03  MFID
//   [1] 0x15  SFID = SBCM
//   [2] 0x01  SBCM.CHMD  充电模式
//   [3] 0x18  SBCM.DELY
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
constexpr uint8_t kWriteDelay = 0x18;

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
        kMfid, kSfidWrite, kChargeModeManual, kWriteDelay,
        static_cast<uint8_t>(stopPercent - kChargeStartOffset),
        static_cast<uint8_t>(stopPercent),
    };
    return Oem::Invoke(request, sizeof(request), nullptr, 0, dryRun, failure);
}

// 交还给厂商的智能充电。CHMD 写 4 就够，阈值字段照旧带上——实测写入后读回 mode 变为 4
// 而 start/stop 保持传入的值，随后由系统按使用习惯自行调整。
//
// 这里不试图恢复「用户接管之前」的那一组阈值：智能模式下那两个数是系统自己算的，
// 存一份旧值再写回去只会让它从一个过时的起点重新学。
Result SetSmartCharge(bool dryRun, HRESULT &failure) noexcept {
    ChargeThreshold current{};
    if (ReadChargeThreshold(current, failure) != Result::Ok) {
        // 读不到就用一组中性值。写入本身只依赖 CHMD，阈值随后会被系统覆盖。
        current.startPercent = kMaxChargeLimit - kChargeStartOffset;
        current.stopPercent = kMaxChargeLimit;
    }

    const uint8_t request[]{
        kMfid, kSfidWrite, kChargeModeSmart, kWriteDelay,
        current.startPercent, current.stopPercent,
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
