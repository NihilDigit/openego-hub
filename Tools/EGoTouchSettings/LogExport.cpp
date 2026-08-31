#include "pch.h"

#include "LogExport.h"

#include <filesystem>
#include <fstream>
#include <system_error>
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

void WriteInfoFile(const fs::path& target, const fs::path& logsDir, size_t copied,
                   const std::vector<std::wstring>& skipped, bool iniPresent) {
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

Result WriteArchive(const std::wstring& destinationZip) {
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

    WriteInfoFile(snapshot.path / L"export-info.txt", logs, copied, skipped, iniPresent);
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
