#include "PenSettingsStore.h"

#include "Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace Service {

namespace {

constexpr wchar_t kDirectoryName[] = L"OpenEGoHub";
constexpr wchar_t kFileName[]      = L"pen_settings.ini";
constexpr wchar_t kSection[]       = L"PenButton";
constexpr wchar_t kKey[]           = L"pen_button_mode";

// 走 SHGetKnownFolderPath 而不是读 %ProgramData%：服务在 SYSTEM 下跑，环境块由 SCM 继承，
// 拿不准里面有什么；已知文件夹 ID 不依赖环境。
std::wstring ProgramDataDirectory() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &raw))) {
        if (raw) CoTaskMemFree(raw);
        return {};
    }
    std::wstring path(raw);
    CoTaskMemFree(raw);
    return path;
}

std::wstring DirectoryPath() {
    const auto base = ProgramDataDirectory();
    if (base.empty()) {
        return {};
    }
    return base + L"\\" + kDirectoryName;
}

std::string NarrowToken(const std::wstring& wide) {
    std::string out;
    out.reserve(wide.size());
    for (const wchar_t ch : wide) {
        // token 全是 ASCII 小写字母和下划线，超出这个集合的一律不是合法 token，
        // 直接原样塞进去让后面的查表失败即可。
        out.push_back(ch < 128 ? static_cast<char>(std::tolower(static_cast<unsigned char>(ch)))
                               : '?');
    }
    return out;
}

std::wstring WidenToken(const char* token) {
    std::wstring out;
    for (const char* p = token; p && *p; ++p) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    }
    return out;
}

} // namespace

std::wstring PenSettingsStore::FilePath() {
    const auto dir = DirectoryPath();
    if (dir.empty()) {
        return {};
    }
    return dir + L"\\" + kFileName;
}

std::optional<PenButtonMode> PenSettingsStore::LoadPenButtonMode() {
    const auto path = FilePath();
    if (path.empty()) {
        LOG_WARN("Service", __func__, "Config",
                 "Cannot resolve ProgramData; pen button mode falls back to the default.");
        return std::nullopt;
    }

    std::array<wchar_t, 64> buffer{};
    const DWORD length = GetPrivateProfileStringW(
        kSection, kKey, L"", buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    if (length == 0) {
        // 首次启动没有这个文件，属于常态，不值一条 WARN。
        return std::nullopt;
    }

    const auto token = NarrowToken(std::wstring(buffer.data(), length));
    const auto parsed = PenButtonModeFromToken(token);
    if (!parsed) {
        LOG_WARN("Service", __func__, "Config",
                 "Persisted pen_button_mode '{}' is not a known token; falling back to the default.",
                 token);
        return std::nullopt;
    }
    return parsed;
}

bool PenSettingsStore::SavePenButtonMode(PenButtonMode mode) {
    const auto dir = DirectoryPath();
    if (dir.empty()) {
        return false;
    }
    if (!CreateDirectoryW(dir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        LOG_WARN("Service", __func__, "Config",
                 "Cannot create the settings directory (err={}).", GetLastError());
        return false;
    }

    const auto path = dir + L"\\" + kFileName;
    const auto value = WidenToken(ToPenButtonModeToken(mode));
    if (!WritePrivateProfileStringW(kSection, kKey, value.c_str(), path.c_str())) {
        LOG_WARN("Service", __func__, "Config",
                 "Failed to persist pen_button_mode (err={}).", GetLastError());
        return false;
    }
    return true;
}

} // namespace Service
