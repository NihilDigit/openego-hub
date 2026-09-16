#include "pch.h"

#include "LogExport.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace LogExport {

namespace {

// 日志目录跟着 Common/source/Logger.cpp 走：那边读 logging.ini 也是经已知文件夹解析
// ProgramData，而不是展开 %ProgramData%——服务在 SYSTEM 下跑，那个环境变量未必存在。
fs::path DataRoot() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &raw))) return {};
    fs::path root{raw};
    CoTaskMemFree(raw);
    return root / L"OpenEGoHub";
}

fs::path LogsDir() {
    const fs::path root = DataRoot();
    return root.empty() ? fs::path{} : root / L"logs";
}

// 厂商日志目录。这是排查触控问题唯一有用的材料：我们这侧的日志只能说明宿主起没起来、
// 配置读到几，而面板 Project ID、总线是否通、固件刷写、SpbModuleInit 成败全在这里。
// 路径不随厂商安装位置变化，THP_Service.dll 与 himax_thp_drv.dll 各自硬编码了它的绝对
// 路径（前者写 HuaweiTHP，后者写 HuaweiThp，大小写不同但是同一个目录）。
fs::path VendorLogsDir() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &raw))) return {};
    fs::path root{raw};
    CoTaskMemFree(raw);
    return root / L"Huawei" / L"HuaweiTHP";
}

// 厂商日志按天分文件，目录里往往堆着几十天共上百 MB。导出是给人发回来的，不能整包带走，
// 所以两类各留最近几份；Service_LogFile 单份可达十几 MB，再截尾。
constexpr size_t kVendorHimaxKeep = 6;
constexpr size_t kVendorServiceKeep = 2;
constexpr unsigned long long kVendorServiceTailBytes = 4ull * 1024 * 1024;

std::wstring Timestamp(const wchar_t* format) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t text[64]{};
    std::swprintf(text, std::size(text), format, static_cast<unsigned>(now.wYear),
                  static_cast<unsigned>(now.wMonth), static_cast<unsigned>(now.wDay),
                  static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute),
                  static_cast<unsigned>(now.wSecond));
    return text;
}

// 复制一个正在被写的日志文件。
//
// 日志库以 _SH_DENYWR 打开这些文件，允许读者，所以复制本身安全，最坏情况是尾部截在半行。
// 但读者这一侧请求的共享模式必须含 FILE_SHARE_WRITE，否则与那个还开着的写句柄互斥。
// fs::copy_file 用什么共享模式没有文档保证，实测的行为不构成契约，所以自己开句柄。
bool CopyWhileOpen(const fs::path& source, const fs::path& destination) {
    const HANDLE in = CreateFileW(source.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) return false;

    const HANDLE out = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        CloseHandle(in);
        return false;
    }

    std::vector<char> buffer(64 * 1024);
    bool ok = true;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(in, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0) break;
        DWORD written = 0;
        if (!WriteFile(out, buffer.data(), read, &written, nullptr) || written != read) {
            ok = false;
            break;
        }
    }

    CloseHandle(out);
    CloseHandle(in);
    if (!ok) {
        std::error_code ec;
        fs::remove(destination, ec);
    }
    return ok;
}

// 只复制文件末尾的 maxBytes，用于厂商那几个大日志。排查要看的总是最后那一段。
// 截断点前移到下一个换行之后，否则第一行是半行，看的人会把它当成日志本身的异常。
// 共享模式的要求与 CopyWhileOpen 相同：厂商进程正开着这些文件写。
bool CopyTailWhileOpen(const fs::path& source, const fs::path& destination,
                       unsigned long long maxBytes) {
    const HANDLE in = CreateFileW(source.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(in, &size)) {
        CloseHandle(in);
        return false;
    }

    bool truncated = false;
    if (static_cast<unsigned long long>(size.QuadPart) > maxBytes) {
        LARGE_INTEGER offset{};
        offset.QuadPart = size.QuadPart - static_cast<long long>(maxBytes);
        if (!SetFilePointerEx(in, offset, nullptr, FILE_BEGIN)) {
            CloseHandle(in);
            return false;
        }
        truncated = true;
    }

    const HANDLE out = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        CloseHandle(in);
        return false;
    }

    std::vector<char> buffer(64 * 1024);
    bool ok = true;
    bool aligned = !truncated;  // 整份复制时不需要对齐到行首
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(in, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0) break;

        const char* data = buffer.data();
        DWORD length = read;
        if (!aligned) {
            const char* newline = static_cast<const char*>(memchr(data, '\n', length));
            if (!newline) continue;  // 这一块里没有换行，整块丢弃，继续找
            length -= static_cast<DWORD>(newline + 1 - data);
            data = newline + 1;
            aligned = true;
        }

        DWORD written = 0;
        if (!WriteFile(out, data, length, &written, nullptr) || written != length) {
            ok = false;
            break;
        }
    }

    CloseHandle(out);
    CloseHandle(in);
    if (!ok) {
        std::error_code ec;
        fs::remove(destination, ec);
    }
    return ok;
}

fs::path TarPath() {
    wchar_t system32[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system32, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return fs::path{system32} / L"tar.exe";
}

std::wstring Quote(const std::wstring& value) {
    return L'"' + value + L'"';
}

// 用系统自带的 bsdtar 打包，不引第三方 zip 库。-a 让它按 .zip 后缀选格式。
bool RunTar(const fs::path& archive, const fs::path& workingDir,
            const std::vector<std::wstring>& entries, std::wstring& error) {
    const fs::path tar = TarPath();
    if (tar.empty() || !fs::exists(tar)) {
        error = L"系统中没有找到 tar.exe，无法打包。";
        return false;
    }

    std::wstring command = Quote(tar.wstring()) + L" -a -c -f " + Quote(archive.wstring()) +
                           L" -C " + Quote(workingDir.wstring());
    for (const auto& entry : entries) {
        command += L' ';
        command += Quote(entry);
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(tar.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, workingDir.c_str(), &startup, &process)) {
        error = L"无法启动打包程序。";
        return false;
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (exitCode != 0) {
        error = L"打包程序以错误码 " + std::to_wstring(exitCode) + L" 退出。";
        return false;
    }
    return true;
}

// 从 SCM 读厂商服务的 ImagePath 与当前状态。
//
// 装机形态是排查触控问题的第一个岔路口：厂商那套装在哪、此刻在不在跑，决定了后面每一条
// 日志该怎么读。服务名与 VendorPath.cpp 保持一致，那边也是按这个名字定位原厂目录的。
void DescribeVendorService(std::wstring& imagePath, std::wstring& state) {
    imagePath = L"(服务未注册)";
    state = L"(未知)";

    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return;
    SC_HANDLE service = OpenServiceW(manager, L"HuaweiThpService",
                                     SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
    if (!service) {
        CloseServiceHandle(manager);
        return;
    }

    DWORD needed = 0;
    (void)QueryServiceConfigW(service, nullptr, 0, &needed);
    if (needed > 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<BYTE> buffer(needed);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (QueryServiceConfigW(service, config, needed, &needed) && config->lpBinaryPathName) {
            imagePath = config->lpBinaryPathName;
        }
    }

    SERVICE_STATUS_PROCESS status{};
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) {
        switch (status.dwCurrentState) {
            case SERVICE_STOPPED: state = L"已停止"; break;
            case SERVICE_RUNNING: state = L"正在运行"; break;
            case SERVICE_START_PENDING: state = L"正在启动"; break;
            case SERVICE_STOP_PENDING: state = L"正在停止"; break;
            default: state = L"状态 " + std::to_wstring(status.dwCurrentState); break;
        }
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
}

void WriteInfoFile(const fs::path& target, const fs::path& logsDir, size_t copied,
                   const std::vector<std::wstring>& skipped, bool iniPresent,
                   bool vendorRequested, size_t vendorCopied) {
    std::ofstream out(target, std::ios::binary);
    if (!out) return;
    out << "\xEF\xBB\xBF";  // BOM，否则记事本按 ANSI 读，中文全是乱码

    const auto utf8 = [](const std::wstring& text) {
        if (text.empty()) return std::string{};
        const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                             static_cast<int>(text.size()), nullptr, 0,
                                             nullptr, nullptr);
        std::string result(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                            result.data(), size, nullptr, nullptr);
        return result;
    };

    out << "OpenEGo Hub 日志导出\r\n";
    out << "导出时间: " << utf8(Timestamp(L"%04u-%02u-%02u %02u:%02u:%02u")) << "\r\n";
    out << "来源目录: " << utf8(logsDir.wstring()) << "\r\n";
    out << "文件数量: " << copied << "\r\n";
    out << "logging.ini: " << (iniPresent ? "已包含" : "不存在") << "\r\n";
    if (!skipped.empty()) {
        out << "读取失败并跳过:\r\n";
        for (const auto& name : skipped) {
            out << "  " << utf8(name) << "\r\n";
        }
    }

    out << "\r\n厂商日志: ";
    if (!vendorRequested) {
        out << "未附加\r\n";
    } else if (vendorCopied == 0) {
        out << "已勾选，但一份也没读到（" << utf8(VendorLogsDir().wstring()) << "）\r\n";
    } else {
        out << "已附加 " << vendorCopied << " 份，见 vendor 目录\r\n";
    }

    // 装机形态决定后面每一条日志该怎么读：厂商那套装在哪、此刻在不在跑。触控不工作时
    // 这两项往往比日志本身先给出答案。
    std::wstring imagePath;
    std::wstring state;
    DescribeVendorService(imagePath, state);
    out << "\r\n厂商服务 HuaweiThpService\r\n";
    out << "  ImagePath: " << utf8(imagePath) << "\r\n";
    out << "  当前状态: " << utf8(state) << "\r\n";
}

// 把厂商日志收进快照。两类分开取：himax 的每份几十 KB 可以多留几天，THP 的单份可达
// 十几 MB，只留最近两份并截尾。返回实际收进去的文件数。
size_t CollectVendorLogs(const fs::path& destination, std::vector<std::wstring>& skipped) {
    const fs::path source = VendorLogsDir();
    if (source.empty()) return 0;

    std::error_code ec;
    fs::directory_iterator it{source, ec};
    if (ec) return 0;

    using Entry = std::pair<fs::file_time_type, fs::path>;
    std::vector<Entry> himax;
    std::vector<Entry> service;
    for (const auto& entry : it) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc)) continue;
        const auto time = fs::last_write_time(entry.path(), entryEc);
        if (entryEc) continue;

        const std::wstring name = entry.path().filename().wstring();
        if (name.starts_with(L"hx_hal_log_")) {
            himax.emplace_back(time, entry.path());
        } else if (name.starts_with(L"Service_LogFile-")) {
            service.emplace_back(time, entry.path());
        }
    }

    const auto newestFirst = [](const Entry& a, const Entry& b) { return a.first > b.first; };
    std::sort(himax.begin(), himax.end(), newestFirst);
    std::sort(service.begin(), service.end(), newestFirst);
    if (himax.size() > kVendorHimaxKeep) himax.resize(kVendorHimaxKeep);
    if (service.size() > kVendorServiceKeep) service.resize(kVendorServiceKeep);

    fs::create_directories(destination, ec);
    if (ec) return 0;

    size_t copied = 0;
    const auto take = [&](const std::vector<Entry>& list, unsigned long long limit) {
        for (const auto& [time, path] : list) {
            const auto name = path.filename();
            if (CopyTailWhileOpen(path, destination / name, limit)) {
                ++copied;
            } else {
                skipped.push_back(name.wstring());
            }
        }
    };
    // himax 的日志整份带走，截尾反而会丢掉开头的 Project ID 与面板判定。
    take(himax, (std::numeric_limits<unsigned long long>::max)());
    take(service, kVendorServiceTailBytes);
    return copied;
}

// 快照目录的清理。打包完就没用了，留在 %TEMP% 里等于把日志复制了一份出去。
struct ScopedDirectory {
    fs::path path;
    ~ScopedDirectory() {
        if (path.empty()) return;
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

} // namespace

std::wstring SuggestedFileName() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t text[64]{};
    std::swprintf(text, std::size(text), L"OpenEGoHub-logs-%04u%02u%02u-%02u%02u.zip",
                  static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth),
                  static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour),
                  static_cast<unsigned>(now.wMinute));
    return text;
}

bool HasLogs() {
    const fs::path logs = LogsDir();
    if (logs.empty()) return false;
    std::error_code ec;
    fs::directory_iterator it{logs, ec};
    if (ec) return false;
    for (const auto& entry : it) {
        if (entry.is_regular_file(ec)) return true;
    }
    return false;
}

Result WriteArchive(const std::wstring& destinationZip, bool includeVendorLogs) {
    Result result;
    const fs::path logs = LogsDir();
    if (logs.empty()) {
        result.detail = L"无法定位 ProgramData 目录。";
        return result;
    }

    std::error_code ec;
    fs::directory_iterator it{logs, ec};
    if (ec) {
        result.status = Status::NoLogs;
        return result;
    }

    const fs::path tempRoot = fs::temp_directory_path(ec);
    if (ec || tempRoot.empty()) {
        result.detail = L"无法定位临时目录。";
        return result;
    }

    // 快照带上进程号：两个设置窗同时导出时不会互相覆盖。
    ScopedDirectory snapshot{tempRoot / (L"OpenEGoHub-log-export-" +
                                         std::to_wstring(GetCurrentProcessId()) + L"-" +
                                         Timestamp(L"%04u%02u%02u%02u%02u%02u"))};
    fs::create_directories(snapshot.path / L"logs", ec);
    if (ec) {
        result.detail = L"无法在临时目录中建立工作目录。";
        return result;
    }

    size_t copied = 0;
    std::vector<std::wstring> skipped;
    for (const auto& entry : it) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc)) continue;
        const auto name = entry.path().filename();
        if (CopyWhileOpen(entry.path(), snapshot.path / L"logs" / name)) {
            ++copied;
        } else {
            skipped.push_back(name.wstring());
        }
    }

    if (copied == 0 && skipped.empty()) {
        result.status = Status::NoLogs;
        return result;
    }
    if (copied == 0) {
        result.detail = L"日志文件全部读取失败。";
        return result;
    }

    std::vector<std::wstring> entries{L"logs"};

    // logging.ini 决定了这些日志记到什么级别，缺了它看日志的人无从判断「没有这条记录」
    // 是没发生还是没记。
    const bool iniPresent = fs::exists(DataRoot() / L"logging.ini", ec) &&
                            CopyWhileOpen(DataRoot() / L"logging.ini",
                                          snapshot.path / L"logging.ini");
    if (iniPresent) entries.push_back(L"logging.ini");

    // 厂商日志读不到不算导出失败：那只是少了一份材料，我们自己的日志仍然值得提交。
    size_t vendorCopied = 0;
    if (includeVendorLogs) {
        vendorCopied = CollectVendorLogs(snapshot.path / L"vendor", skipped);
        if (vendorCopied > 0) entries.push_back(L"vendor");
    }

    WriteInfoFile(snapshot.path / L"export-info.txt", logs, copied, skipped, iniPresent,
                  includeVendorLogs, vendorCopied);
    if (fs::exists(snapshot.path / L"export-info.txt", ec)) {
        entries.push_back(L"export-info.txt");
    }

    fs::remove(destinationZip, ec);
    if (!RunTar(destinationZip, snapshot.path, entries, result.detail)) {
        return result;
    }
    if (!fs::exists(destinationZip, ec) || fs::file_size(destinationZip, ec) == 0) {
        result.detail = L"打包程序没有写出文件。";
        return result;
    }

    result.status = Status::Success;
    return result;
}

} // namespace LogExport
