#pragma once

#include <windows.h>

#include "GaokunPower.h"

// 公开接口的带诊断版本。命令行要打印失败的 HRESULT 与「走完通路但不下发」的试运行，
// 上层 UI 两样都不需要，所以留在这里而不进 GaokunPower.h。
namespace Gaokun::Power::Detail {

[[nodiscard]] Result ReadChargeThreshold(ChargeThreshold &out, HRESULT &failure) noexcept;
[[nodiscard]] Result SetChargeLimit(int stopPercent, bool dryRun, HRESULT &failure) noexcept;
[[nodiscard]] Result SetSmartCharge(bool dryRun, HRESULT &failure) noexcept;

} // namespace Gaokun::Power::Detail
