#include "VendorPath.h"

#include <windows.h>

#include <vector>

namespace Thp {

namespace {

constexpr const wchar_t *kVendorServiceName = L"HuaweiThpService";

} // namespace

bool VendorDirectoryFromImagePath(const std::wstring &imagePath,
                                  std::wstring &directory) noexcept {
    if (imagePath.empty()) return false;

    std::wstring executable;
    if (imagePath.front() == L'"') {
        const size_t closing = imagePath.find(L'"', 1);
        if (closing == std::wstring::npos) return false;
        executable = imagePath.substr(1, closing - 1);
    } else {
        // 未加引号时以第一个空格断开。原厂 ImagePath 是带引号的，这条分支只为容错。
        const size_t space = imagePath.find(L' ');
        executable = space == std::wstring::npos ? imagePath : imagePath.substr(0, space);
    }

    const size_t slash = executable.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;

    directory = executable.substr(0, slash);
    return !directory.empty();
}

bool DiscoverVendorDirectory(std::wstring &directory) noexcept {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return false;

    SC_HANDLE service = OpenServiceW(manager, kVendorServiceName, SERVICE_QUERY_CONFIG);
    if (!service) {
        CloseServiceHandle(manager);
        return false;
    }

    DWORD needed = 0;
    (void)QueryServiceConfigW(service, nullptr, 0, &needed);
    bool ok = false;
    if (needed > 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<BYTE> buffer(needed);
        auto *config = reinterpret_cast<QUERY_SERVICE_CONFIGW *>(buffer.data());
        if (QueryServiceConfigW(service, config, needed, &needed) && config->lpBinaryPathName) {
            ok = VendorDirectoryFromImagePath(config->lpBinaryPathName, directory);
        }
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return ok;
}

} // namespace Thp
