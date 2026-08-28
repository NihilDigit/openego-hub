#pragma once

#include <windows.h>

#include <cstdint>

#include "GaokunPower.h"

// ROOT\WMI 的 OemWMIMethod::OemWMIfun 通道。厂商 BIOS 的所有 ACPI 方法都压在这一个方法上，
// 由请求缓冲的头两字节（MFID/SFID）区分，充电阈值的读与写只差 SFID 一个字节。
//
// 单独成文件是因为读与写共用整条 COM 通路：连接、代理认证、取实例路径、构造入参。先前它
// 只服务写入，内联在 ChargeLimit.cpp 里。
namespace Gaokun::Power::Oem {

inline constexpr ULONG kBufferSize = 64;

// 发一次 OemWMIfun。request 的前 requestSize 字节写入 u8Input 的头部，其余补零；
// 返回的 u8Output 复制到 response 的前 responseSize 字节。
//
// dryRun 走完全部 COM 步骤但不发出 ExecMethod：改充电阈值会改变电池的实际行为，需要能在
// 不动设置的前提下确认这条通路是通的。
[[nodiscard]] Result Invoke(const uint8_t *request, size_t requestSize,
                            uint8_t *response, size_t responseSize,
                            bool dryRun, HRESULT &failure) noexcept;

// 诊断用。Invoke 的每一步都会调这个回调，置空则静默。
void SetTrace(void (*trace)(const wchar_t *what, HRESULT hr)) noexcept;

} // namespace Gaokun::Power::Oem
