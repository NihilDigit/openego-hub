#include "ChargePolicy.h"

#include "Logger.h"

#include <windows.h>

#include <string>

namespace Service::ChargePolicy {

namespace {

constexpr const wchar_t* kIntentKey = L"SOFTWARE\\OpenEGoHub\\Charge";
constexpr const wchar_t* kIntentManual = L"Manual";
constexpr const wchar_t* kIntentLimit = L"Limit";

// 厂商的档位记录。名字与含义取自 SmartChargePlugin.dll 的 InitSmartChargeMode（RVA 0x2eef0）：
// PowerSafeManagerStatus 缺失或为 0 时插件无条件写智能充电，为 1 且有 CustomChargeCapacity
// 时写手动上限。两个值都是十进制串（REG_SZ），不是 DWORD——按 DWORD 写它读不出来。
//
// 只要 CustomChargeCapacity 这个值在，插件就走手动那一支，不再看 SmartChargeMode 与
// PowerSafeManagerMode 两个旧档序号，所以这里不必去动那两个值。内容解析用 _wtoi，非法串
// 当 0，随后被那一支的预夹改成 100——写进去的必须是干净的十进制数字。
constexpr const wchar_t* kVendorKey = L"SOFTWARE\\PCManager\\MBAPowerManager";
constexpr const wchar_t* kVendorStatus = L"PowerSafeManagerStatus";
constexpr const wchar_t* kVendorCapacity = L"CustomChargeCapacity";

// 改厂商的记录之前先留一份原值，与服务启动类型、用户态自启项两处的做法一致。
constexpr const wchar_t* kVendorBackupKey = L"SOFTWARE\\OpenEGoHub\\VendorChargeBackup";
constexpr const wchar_t* kBackupCaptured = L"Captured";

// 本服务是原生 ARM64，PC Manager 是 x64，两者在 ARM64 Windows 上看的是同一份 64 位注册表
// 视图，所以这里不需要 KEY_WOW64_* 标志。WOW6432Node 下那一份属于 x86 程序，与它无关。

[[nodiscard]] bool ReadDword(HKEY root, const wchar_t* subKey, const wchar_t* name,
                             DWORD& out) noexcept {
    DWORD type = 0;
    DWORD size = sizeof(out);
    return RegGetValueW(root, subKey, name, RRF_RT_REG_DWORD, &type, &out, &size) ==
           ERROR_SUCCESS;
}

[[nodiscard]] bool ReadString(HKEY root, const wchar_t* subKey, const wchar_t* name,
                              std::wstring& out) noexcept {
    wchar_t buffer[64]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, subKey, name, RRF_RT_REG_SZ, nullptr, buffer, &size) !=
        ERROR_SUCCESS) {
        return false;
    }
    out.assign(buffer);
    return true;
}

[[nodiscard]] bool WriteString(HKEY key, const wchar_t* name, const std::wstring& value) noexcept {
    const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(key, name, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(value.c_str()), bytes) == ERROR_SUCCESS;
}

// 原值只在第一次镜像之前记一次。不加这道判断的话，第二次镜像备份下来的就是我们自己写的
// 值，原值从此找不回来。
void CaptureVendorBackupOnce() noexcept {
    DWORD captured = 0;
    if (ReadDword(HKEY_LOCAL_MACHINE, kVendorBackupKey, kBackupCaptured, captured) &&
        captured != 0) {
        return;
    }

    HKEY backup = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kVendorBackupKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &backup,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }

    // 值不存在也要记下这个事实：插件对「缺失」和「为 0」的处理并不相同，恢复时得分得出来。
    std::wstring value;
    if (ReadString(HKEY_LOCAL_MACHINE, kVendorKey, kVendorStatus, value)) {
        (void)WriteString(backup, kVendorStatus, value);
    }
    if (ReadString(HKEY_LOCAL_MACHINE, kVendorKey, kVendorCapacity, value)) {
        (void)WriteString(backup, kVendorCapacity, value);
    }

    const DWORD one = 1;
    (void)RegSetValueExW(backup, kBackupCaptured, 0, REG_DWORD,
                         reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegCloseKey(backup);
}

} // namespace

Intent Load() noexcept {
    Intent intent{};
    DWORD manual = 0;
    if (!ReadDword(HKEY_LOCAL_MACHINE, kIntentKey, kIntentManual, manual)) return intent;

    intent.valid = true;
    intent.manual = manual != 0;
    if (!intent.manual) return intent;

    DWORD limit = 0;
    if (!ReadDword(HKEY_LOCAL_MACHINE, kIntentKey, kIntentLimit, limit) || limit > 100) {
        // 模式在而上限不在，说明这份记录被截断了。当作没有记录，而不是按一个猜出来的
        // 上限去改用户的机器。
        return Intent{};
    }
    intent.limit = static_cast<uint8_t>(limit);
    return intent;
}

void Store(const Intent& intent) noexcept {
    if (!intent.valid) return;

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kIntentKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        LOG_WARN("Service", __func__, "Charge", "Cannot record the charge intent (err={}).",
                 GetLastError());
        return;
    }

    const DWORD manual = intent.manual ? 1 : 0;
    const DWORD limit = intent.limit;
    (void)RegSetValueExW(key, kIntentManual, 0, REG_DWORD,
                         reinterpret_cast<const BYTE*>(&manual), sizeof(manual));
    (void)RegSetValueExW(key, kIntentLimit, 0, REG_DWORD,
                         reinterpret_cast<const BYTE*>(&limit), sizeof(limit));
    RegCloseKey(key);
}

void MirrorToVendor(const Intent& intent) noexcept {
    if (!intent.valid) return;

    HKEY vendor = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kVendorKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                      &vendor) != ERROR_SUCCESS) {
        return;  // PC Manager 没装，或这台机器上没有这份记录
    }

    CaptureVendorBackupOnce();

    // 智能充电对应 Status=0：插件读到 0 就写 CHMD=4 加固定的 65/70，与我们的 --smart 同值。
    // 手动则要 Status=1 加 CustomChargeCapacity，插件据此写 CHMD=1 与 (cap-5, cap)。
    bool ok = WriteString(vendor, kVendorStatus, intent.manual ? L"1" : L"0");
    if (intent.manual) {
        ok = WriteString(vendor, kVendorCapacity, std::to_wstring(intent.limit)) && ok;
    }
    RegCloseKey(vendor);

    if (!ok) {
        LOG_WARN("Service", __func__, "Charge",
                 "Cannot mirror the charge setting into PC Manager's record (err={}).",
                 GetLastError());
        return;
    }
    LOG_INFO("Service", __func__, "Charge",
             "PC Manager's charge record set to {} ({}%).",
             intent.manual ? "manual" : "smart", static_cast<unsigned>(intent.limit));
}

} // namespace Service::ChargePolicy
