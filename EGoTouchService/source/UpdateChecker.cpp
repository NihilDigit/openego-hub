#include "UpdateChecker.h"

#include "AppVersion.h"
#include "Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <bcrypt.h>
#include <shlobj.h>
#include <userenv.h>
#include <winhttp.h>
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <functional>
#include <string_view>
#include <vector>

namespace Service::Update {

namespace {

// GitHub 对未认证请求的限额是每小时 60 次，而这里一天最多几次，不需要令牌，也不该带：
// 服务跑在 LocalSystem 下，机器上没有哪个凭据是属于「这台机器」的。
constexpr wchar_t kApiBaseUrlDefault[] = L"https://api.github.com";
constexpr wchar_t kApiPath[] = L"/repos/NihilDigit/openego-hub/releases/latest";

// 直连 GitHub 不通时的镜像。国内直连 api.github.com 经常超时，这是唯一的回退路径。
// 换镜像只改这一个常量。
//
// 信任边界：走镜像时 MSI 与 SHA256SUMS 来自同一个第三方代理，哈希校验因此挡不住一个恶意
// 镜像——它可以同时替换两者，算出来的哈希当然对得上。这与 MSI 至今没有签名是同一层问题
// （issue #4 的「待定」里记着），但它把信任面从「GitHub 加发布账户」扩大到「再加上镜像
// 运营方」。等 MSI 签名落地之后，签名校验才是真正能挡住这条的东西，那时这段注释也要跟着
// 重写。
constexpr wchar_t kMirrorPrefix[] = L"https://ghfast.top/";

// 镜像那条路上的两个地址。ghfast 不代理 API（实测 403，14 字节），所以版本号只能从
// github.com 的 releases/latest 那一跳的 Location 里取，资产地址则按发布流程的命名约定
// 拼出来——那条路上没有 JSON 可读。命名见 .github/workflows/release.yml。
constexpr wchar_t kReleasesLatestUrl[] =
    L"https://github.com/NihilDigit/openego-hub/releases/latest";
constexpr wchar_t kReleaseDownloadBase[] =
    L"https://github.com/NihilDigit/openego-hub/releases/download/";

// GitHub 无条件要求 User-Agent，缺了这一个头就是 403，不是 400，排查时很容易看岔。
#define OPENEGO_WIDE_INNER(x) L##x
#define OPENEGO_WIDE(x) OPENEGO_WIDE_INNER(x)
constexpr wchar_t kUserAgent[] = L"OpenEGoHub/" OPENEGO_WIDE(OPENEGO_VERSION_STRING);

constexpr wchar_t kApiHeaders[] =
    L"Accept: application/vnd.github+json\r\n"
    L"X-GitHub-Api-Version: 2022-11-28\r\n";

// JSON 响应几十 KB，校验和文件几十字节；上限只是不让一个畸形响应把内存吃光。
constexpr uint64_t kMaxJsonBytes = 4u * 1024u * 1024u;
constexpr uint64_t kMaxChecksumBytes = 1024u * 1024u;
// MSI 目前约 10 MB。留出量级余地，同时挡住「下载地址指到了别的东西」这种情况。
constexpr uint64_t kMaxMsiBytes = 512ull * 1024ull * 1024ull;

constexpr wchar_t kUpdateKey[] = L"SOFTWARE\\OpenEGoHub\\Update";
constexpr wchar_t kSkippedValue[] = L"SkippedVersion";
constexpr wchar_t kApiBaseValue[] = L"ApiBaseUrl";
constexpr wchar_t kAutoCheckValue[] = L"AutoCheck";
constexpr wchar_t kRelaunchTrayValue[] = L"RelaunchTray";

// ── WinHTTP ──────────────────────────────────────────────────────────────────

class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET handle) : m_handle(handle) {}
    ~InternetHandle() { if (m_handle) WinHttpCloseHandle(m_handle); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    InternetHandle& operator=(HINTERNET handle) {
        if (m_handle) WinHttpCloseHandle(m_handle);
        m_handle = handle;
        return *this;
    }
    [[nodiscard]] HINTERNET get() const noexcept { return m_handle; }
    explicit operator bool() const noexcept { return m_handle != nullptr; }

private:
    HINTERNET m_handle = nullptr;
};

bool EqualsAsciiNoCase(std::wstring_view lhs, std::wstring_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        wchar_t a = lhs[i];
        wchar_t b = rhs[i];
        if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b - L'A' + L'a');
        if (a != b) return false;
    }
    return true;
}

// 一次 GET，响应体交给 sink 分段消费。sink 返回 false（落盘失败）时整个请求按失败处理。
//
// 分段而不是先收进内存再处理：MSI 有十几兆，而这条路径上唯一需要完整内存副本的只有两个
// 几十 KB 的文本响应。
//
// redirectLocation 非空时要的不是响应体而是重定向目标：关掉自动跟随，只取回 Location。
// 镜像那条路的版本号就藏在这个头里——ghfast 不代理 API（实测 403），能用的只有 github.com
// 的 releases/latest 那一跳。
bool HttpGet(const std::wstring& url,
             const wchar_t* extraHeaders,
             uint64_t maxBytes,
             const std::function<bool(const char*, DWORD)>& sink,
             std::wstring* redirectLocation,
             uint32_t& error) {
    error = 0;

    std::array<wchar_t, 256> hostBuffer{};
    std::array<wchar_t, 2048> pathBuffer{};
    std::array<wchar_t, 2048> extraBuffer{};

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.lpszHostName = hostBuffer.data();
    components.dwHostNameLength = static_cast<DWORD>(hostBuffer.size());
    components.lpszUrlPath = pathBuffer.data();
    components.dwUrlPathLength = static_cast<DWORD>(pathBuffer.size());
    components.lpszExtraInfo = extraBuffer.data();
    components.dwExtraInfoLength = static_cast<DWORD>(extraBuffer.size());

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) {
        error = GetLastError();
        return false;
    }
    // 明文 HTTP 只对本机放行。下载地址取自一份刚从网上取来的 JSON，若允许任意 http，
    // 一份被改写的 JSON 就能把安装包的下载整条降级成明文——那时安装包的完整性只剩下
    // 同样经明文取回的那份校验和，等于没有。127.0.0.1 与 localhost 之外一律要求 https。
    //
    // ::1 不在白名单里：离线 mock 按 IPv4 起就够了，多放一种写法就多一条要一起想清楚的
    // 规则。要用 IPv6 回环时在这里显式加，而不是改成「凡是回环地址」那种模糊判据。
    const std::wstring_view host(components.lpszHostName, components.dwHostNameLength);
    const bool loopback = EqualsAsciiNoCase(host, L"127.0.0.1") ||
                          EqualsAsciiNoCase(host, L"localhost");
    const bool schemeOk = components.nScheme == INTERNET_SCHEME_HTTPS ||
                          (components.nScheme == INTERNET_SCHEME_HTTP && loopback);
    if (!schemeOk) {
        LOG_WARN("Service", __func__, "Update",
                 "Refusing a plaintext HTTP request to a non-loopback host.");
        error = kErrorMalformedResponse;
        return false;
    }

    // 服务跑在 LocalSystem 下，没有用户的 IE 代理设置可继承；自动代理是这个上下文里唯一
    // 能用的一种，没有代理时它等价于直连。
    InternetHandle session(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = GetLastError();
        return false;
    }
    // 解析 / 连接 / 发送 / 接收。接收一段留 60 秒：下载走的是同一条路径，而慢链路上一段
    // 数据等上半分钟是正常的。
    WinHttpSetTimeouts(session.get(), 15000, 15000, 30000, 60000);

    InternetHandle connect(WinHttpConnect(session.get(), components.lpszHostName,
                                          components.nPort, 0));
    if (!connect) {
        error = GetLastError();
        return false;
    }

    std::wstring target(components.lpszUrlPath, components.dwUrlPathLength);
    target.append(components.lpszExtraInfo, components.dwExtraInfoLength);

    InternetHandle request(WinHttpOpenRequest(
        connect.get(), L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        error = GetLastError();
        return false;
    }

    if (redirectLocation) {
        DWORD disable = WINHTTP_DISABLE_REDIRECTS;
        if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disable,
                              sizeof(disable))) {
            error = GetLastError();
            return false;
        }
    }

    if (extraHeaders && !WinHttpAddRequestHeaders(request.get(), extraHeaders,
                                                  static_cast<DWORD>(-1),
                                                  WINHTTP_ADDREQ_FLAG_ADD)) {
        error = GetLastError();
        return false;
    }

    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        error = GetLastError();
        return false;
    }

    // 重定向由 WinHTTP 自己跟（默认策略只允许 https→https，正是需要的）：发布资产的
    // browser_download_url 会跳到另一台主机上。
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(),
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX)) {
        error = GetLastError();
        return false;
    }
    if (redirectLocation) {
        if (status != 301 && status != 302 && status != 303 && status != 307 &&
            status != 308) {
            error = kErrorHttpStatusBase + status;
            return false;
        }
        DWORD locationSize = 0;
        WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_LOCATION,
                            WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &locationSize,
                            WINHTTP_NO_HEADER_INDEX);
        if (locationSize == 0 || locationSize > 8192) {
            error = kErrorMalformedResponse;
            return false;
        }
        std::wstring location(locationSize / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_LOCATION,
                                 WINHTTP_HEADER_NAME_BY_INDEX, location.data(),
                                 &locationSize, WINHTTP_NO_HEADER_INDEX)) {
            error = GetLastError();
            return false;
        }
        location.resize(locationSize / sizeof(wchar_t));
        *redirectLocation = location;
        return true;
    }

    if (status != 200) {
        error = kErrorHttpStatusBase + status;
        return false;
    }

    std::array<char, 16384> buffer{};
    uint64_t total = 0;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            error = GetLastError();
            return false;
        }
        if (available == 0) break;

        while (available > 0) {
            const DWORD want = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            DWORD read = 0;
            if (!WinHttpReadData(request.get(), buffer.data(), want, &read)) {
                error = GetLastError();
                return false;
            }
            if (read == 0) break;

            total += read;
            if (total > maxBytes) {
                error = kErrorMalformedResponse;
                return false;
            }
            if (!sink(buffer.data(), read)) {
                error = GetLastError();
                return false;
            }
            available -= read;
        }
    }
    return true;
}

bool HttpGetText(const std::wstring& url, const wchar_t* extraHeaders, uint64_t maxBytes,
                 std::string& body, uint32_t& error) {
    body.clear();
    return HttpGet(url, extraHeaders, maxBytes,
                   [&body](const char* data, DWORD size) {
                       body.append(data, size);
                       return true;
                   },
                   nullptr, error);
}

bool HttpGetRedirect(const std::wstring& url, std::wstring& location, uint32_t& error) {
    location.clear();
    return HttpGet(url, nullptr, 0, nullptr, &location, error);
}

bool HttpGetFile(const std::wstring& url, const std::wstring& path, uint64_t maxBytes,
                 uint32_t& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }

    const bool ok = HttpGet(url, nullptr, maxBytes,
                            [file](const char* data, DWORD size) {
                                DWORD written = 0;
                                return WriteFile(file, data, size, &written, nullptr) != FALSE &&
                                       written == size;
                            },
                            nullptr, error);
    CloseHandle(file);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

// ── JSON ─────────────────────────────────────────────────────────────────────

// 仓库里没有 JSON 解析器，这一个功能也不值得引一个。需要的只是三个字符串字段，而引库的
// 代价是把一个第三方头文件连同它的编译期和许可证拖进构建；自己写一个完整解析器则比按
// 字段名取值大一个数量级。解析失败的后果只是「这次没查到更新」，一小时后重来。
//
// 取值方式因此刻意保守，只认 "key" : "value" 这一种形态：
//   * 值里出现反斜杠就整条作废。合法的 tag 与下载地址里不会有转义，而一个只对了一半的
//     反转义器会把畸形输入悄悄变成一个看起来合理的字符串。
//   * 超长作废，控制字符作废。
// 任何一条不满足都返回 nullopt，调用方按检查失败处理，而不是按「没有更新」。
constexpr size_t kMaxFieldLength = 512;

std::optional<std::string> ExtractStringField(std::string_view json,
                                              std::string_view key,
                                              size_t& pos) {
    std::string needle;
    needle.reserve(key.size() + 2);
    needle.push_back('"');
    needle.append(key);
    needle.push_back('"');

    const size_t found = json.find(needle, pos);
    if (found == std::string_view::npos) return std::nullopt;

    size_t cursor = found + needle.size();
    while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t')) ++cursor;
    if (cursor >= json.size() || json[cursor] != ':') return std::nullopt;
    ++cursor;
    while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t')) ++cursor;
    if (cursor >= json.size() || json[cursor] != '"') return std::nullopt;
    ++cursor;

    const size_t start = cursor;
    while (cursor < json.size() && json[cursor] != '"') {
        const unsigned char ch = static_cast<unsigned char>(json[cursor]);
        if (ch == '\\' || ch < 0x20) return std::nullopt;
        if (cursor - start >= kMaxFieldLength) return std::nullopt;
        ++cursor;
    }
    if (cursor >= json.size()) return std::nullopt;

    pos = cursor + 1;
    return std::string(json.substr(start, cursor - start));
}

std::wstring Widen(std::string_view ascii) {
    std::wstring out;
    out.reserve(ascii.size());
    for (const char ch : ascii) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    }
    return out;
}

// 日志只吃窄串。这条路径上的宽串全是 URL 和文件名，非 ASCII 的字符在那里本来就不合法，
// 落成 '?' 正好让它在日志里显眼。
std::string NarrowAscii(std::wstring_view wide) {
    std::string out;
    out.reserve(wide.size());
    for (const wchar_t ch : wide) {
        out.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    }
    return out;
}

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// 地址末段的文件名。校验和文件按文件名索引，所以这一段必须和资产名逐字相同。
std::string FileNameFromUrl(std::string_view url) {
    const size_t slash = url.find_last_of('/');
    if (slash == std::string_view::npos || slash + 1 >= url.size()) return {};
    return std::string(url.substr(slash + 1));
}

// 文件名要落到磁盘上，而它来自网络。只放行发布流程真正会产生的那一类名字。
bool IsSafeFileName(std::string_view name) {
    if (name.empty() || name.size() > 128) return false;
    for (const char ch : name) {
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        if (!ok) return false;
    }
    return name.front() != '.';
}

// ── 版本 ─────────────────────────────────────────────────────────────────────

std::optional<uint8_t> ParseByte(std::string_view text) {
    if (text.empty() || text.size() > 3) return std::nullopt;
    unsigned value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return std::nullopt;
        value = value * 10 + static_cast<unsigned>(ch - '0');
    }
    if (value > 255) return std::nullopt;
    return static_cast<uint8_t>(value);
}

// ── 哈希 ─────────────────────────────────────────────────────────────────────

std::string ToHex(const std::vector<uint8_t>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const uint8_t byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

// BCrypt 而不是自带一份 SHA256 实现：算法就在系统里，自研一份还要有人去证明它对。
bool ComputeFileSha256(const std::wstring& path, std::string& hex, uint32_t& error) {
    hex.clear();

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                                  nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        error = static_cast<uint32_t>(status);
        return false;
    }

    DWORD hashLength = 0;
    DWORD copied = 0;
    status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                               reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength),
                               &copied, 0);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    }
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = static_cast<uint32_t>(status);
        return false;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = file != INVALID_HANDLE_VALUE;
    if (!ok) error = GetLastError();

    if (ok) {
        std::array<uint8_t, 65536> buffer{};
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                          nullptr)) {
                error = GetLastError();
                ok = false;
                break;
            }
            if (read == 0) break;
            status = BCryptHashData(hash, buffer.data(), read, 0);
            if (!BCRYPT_SUCCESS(status)) {
                error = static_cast<uint32_t>(status);
                ok = false;
                break;
            }
        }
        CloseHandle(file);
    }

    if (ok) {
        std::vector<uint8_t> digest(hashLength);
        status = BCryptFinishHash(hash, digest.data(), hashLength, 0);
        if (BCRYPT_SUCCESS(status)) {
            hex = ToHex(digest);
        } else {
            error = static_cast<uint32_t>(status);
            ok = false;
        }
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

// 校验和文件是 "<64 位十六进制>  <文件名>" 一行一条（release.yml 的 Get-FileHash 输出）。
// 按文件名找行，而不是认第一行：以后多一个资产，第一行就不是 MSI 那一条了。
std::optional<std::string> FindChecksum(std::string_view text, std::string_view fileName) {
    size_t pos = 0;
    // pwsh 的 Set-Content -Encoding UTF8 不写 BOM，但换一版 PowerShell 就未必，先跳过。
    if (text.starts_with("\xEF\xBB\xBF")) pos = 3;

    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;

        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.remove_suffix(1);
        }
        const size_t space = line.find(' ');
        if (space == std::string_view::npos) continue;

        std::string_view hash = line.substr(0, space);
        std::string_view name = line.substr(space);
        while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
        if (name != fileName) continue;
        if (hash.size() != 64) return std::nullopt;

        std::string lowered;
        lowered.reserve(hash.size());
        for (const char ch : hash) {
            if (!std::isxdigit(static_cast<unsigned char>(ch))) return std::nullopt;
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        return lowered;
    }
    return std::nullopt;
}

// ── 路径 ─────────────────────────────────────────────────────────────────────

// 走 SHGetKnownFolderPath 而不是读 %ProgramData%，理由与 PenSettingsStore 那处相同：
// 服务在 SYSTEM 下跑，环境块由 SCM 继承，拿不准里面有什么。
std::wstring UpdateDirectory() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &raw))) {
        if (raw) CoTaskMemFree(raw);
        return {};
    }
    std::wstring path(raw);
    CoTaskMemFree(raw);
    path += L"\\OpenEGoHub\\update";

    // 中间那一级多半已经存在（PenSettingsStore 建过），SHCreateDirectoryExW 对已存在的
    // 目录返回 ERROR_ALREADY_EXISTS，按成功处理。
    const int created = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS &&
        created != ERROR_FILE_EXISTS) {
        LOG_WARN("Service", __func__, "Update",
                 "Cannot create the update download directory (err={}).", created);
        return {};
    }
    return path;
}

// 接口基址的覆盖。测试钩子：离线 mock 用它把 releases/latest 指到本机，真机验证不必等
// 一次真实发布。
//
// 不新增信任边界：这个值在 HKLM 下，只有管理员写得进去，而能写 HKLM 的人本来就能换掉服务
// 的可执行文件；发布的 MSI 至今没有签名（issue #4 里记为待定），签名落地之前，安装包的
// 真实性本来也只靠那一次 SHA256 与取回校验和的那条链路。签名做起来之后这个钩子要重新评估
// ——那时它就成了绕过签名校验的一条路。
//
// 覆盖生效时由调用方记一行 WARN：有人在真机上忘了删这个键时，日志要能一眼看出来。这里
// 不记，是因为「有没有覆盖」还要被下载路径查一次，用来决定不走镜像——那次查询不该再刷一
// 条日志。未设或形态不合法时返回空串。
std::wstring ApiBaseUrlOverride() {
    wchar_t buffer[512]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kUpdateKey, kApiBaseValue, RRF_RT_REG_SZ, nullptr,
                     buffer, &size) != ERROR_SUCCESS) {
        return {};
    }

    std::wstring base(buffer);
    while (!base.empty() && base.back() == L'/') base.pop_back();
    // 形态过一道：拼不出合法 URL 的值一律当作没设，而不是让 WinHttpCrackUrl 去报一个
    // 看不出来源的错误。协议是否放行由 HttpGet 那道回环判据决定，这里不重复。
    if (!base.starts_with(L"http://") && !base.starts_with(L"https://")) {
        LOG_WARN("Service", __func__, "Update",
                 "Ignoring the ApiBaseUrl override: not an http(s) URL.");
        return {};
    }
    return base;
}

// 已经带镜像前缀的地址不再叠一层。
std::wstring WithMirror(const std::wstring& url) {
    if (url.starts_with(kMirrorPrefix)) return url;
    return std::wstring(kMirrorPrefix) + url;
}

// 从 releases/latest 的 Location 里取 tag。
//
// 实测 ghfast 回来的是相对形式 /https://github.com/.../releases/tag/v0.4.0，直连 GitHub
// 给的是绝对形式；两种都认。判据取「最后一个 /tag/ 之后的那一段」，对两种形式都成立，
// 也不必去猜前缀被代理改写成了什么样子。
std::optional<std::string> TagFromReleaseLocation(const std::wstring& location) {
    const std::string narrow = NarrowAscii(location);
    constexpr std::string_view kMarker = "/tag/";
    const size_t marker = narrow.rfind(kMarker);
    if (marker == std::string::npos) return std::nullopt;

    std::string tag = narrow.substr(marker + kMarker.size());
    // 查询串和末尾斜杠都不是 tag 的一部分。
    const size_t query = tag.find_first_of("?#");
    if (query != std::string::npos) tag.resize(query);
    while (!tag.empty() && tag.back() == '/') tag.pop_back();

    if (tag.empty() || tag.size() > 32) return std::nullopt;
    return tag;
}

// 镜像那条路：从 Location 取 tag，资产地址按发布流程的命名约定拼。
std::optional<ReleaseInfo> FetchLatestViaMirror(uint32_t& error) {
    std::wstring location;
    if (!HttpGetRedirect(WithMirror(kReleasesLatestUrl), location, error)) {
        return std::nullopt;
    }

    const auto tag = TagFromReleaseLocation(location);
    if (!tag) {
        LOG_WARN("Service", __func__, "Update",
                 "The mirror's redirect carries no release tag.");
        error = kErrorMalformedResponse;
        return std::nullopt;
    }
    const auto version = ParseTag(*tag);
    if (!version) {
        LOG_WARN("Service", __func__, "Update",
                 "Ignoring release tag '{}' from the mirror: not a vMAJOR.MINOR.PATCH tag "
                 "this build can represent.", *tag);
        error = kErrorMalformedResponse;
        return std::nullopt;
    }

    const std::wstring wideTag = Widen(*tag);
    const std::wstring directory = std::wstring(kReleaseDownloadBase) + wideTag + L"/";

    ReleaseInfo info{};
    info.version = *version;
    info.msiFileName = L"OpenEGoHubSetup_arm64_" + wideTag + L".msi";
    info.msiUrl = WithMirror(directory + info.msiFileName);
    info.checksumUrl =
        WithMirror(directory + L"OpenEGoHub_arm64_" + wideTag + L"_SHA256SUMS.txt");
    return info;
}

} // namespace

std::string ToString(const Version& version) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u", static_cast<unsigned>(version.major),
                  static_cast<unsigned>(version.minor), static_cast<unsigned>(version.patch));
    return buffer;
}

Version CurrentVersion() noexcept {
    return Version{static_cast<uint8_t>(OPENEGO_VERSION_MAJOR),
                   static_cast<uint8_t>(OPENEGO_VERSION_MINOR),
                   static_cast<uint8_t>(OPENEGO_VERSION_PATCH)};
}

std::optional<Version> ParseTag(std::string_view tag) {
    if (tag.empty() || tag.front() != 'v') return std::nullopt;
    tag.remove_prefix(1);

    const size_t first = tag.find('.');
    if (first == std::string_view::npos) return std::nullopt;
    const size_t second = tag.find('.', first + 1);
    if (second == std::string_view::npos) return std::nullopt;
    // 第三段之后不允许再有任何东西：v1.2.3-rc1 不是这个发布流程会产生的 tag，把它当成
    // 1.2.3 就等于把一个预发布推给了用户。
    if (tag.find('.', second + 1) != std::string_view::npos) return std::nullopt;

    const auto major = ParseByte(tag.substr(0, first));
    const auto minor = ParseByte(tag.substr(first + 1, second - first - 1));
    const auto patch = ParseByte(tag.substr(second + 1));
    if (!major || !minor || !patch) return std::nullopt;

    return Version{*major, *minor, *patch};
}

std::optional<ReleaseInfo> FetchLatestRelease(uint32_t& error) {
    error = 0;

    // 覆盖生效时这条路径整个钉在 mock 上：不回退镜像，也不碰外网。覆盖着还去连 ghfast
    // 就把测试搞脏了——测出来的是镜像的行为，而不是被测的那份 mock。
    const std::wstring override = ApiBaseUrlOverride();
    const bool overridden = !override.empty();
    if (overridden) {
        LOG_WARN("Service", __func__, "Update",
                 "Update API base URL overridden by registry to '{}'; this is a test hook, "
                 "mirror fallback is disabled.", NarrowAscii(override));
    }

    const std::wstring url = (overridden ? override : kApiBaseUrlDefault) + std::wstring(kApiPath);

    std::string body;
    if (!HttpGetText(url, kApiHeaders, kMaxJsonBytes, body, error)) {
        if (overridden) return std::nullopt;

        // 直连不通（DNS、连接、超时或非 2xx）才回退。JSON 解析失败不回退：那说明连上了，
        // 换一条路拿到的是同一份看不懂的东西。
        const uint32_t directError = error;
        LOG_WARN("Service", __func__, "Update",
                 "Direct GitHub API check failed (err={}); falling back to the mirror {}.",
                 directError, NarrowAscii(kMirrorPrefix));
        return FetchLatestViaMirror(error);
    }

    size_t cursor = 0;
    const auto tag = ExtractStringField(body, "tag_name", cursor);
    if (!tag) {
        error = kErrorMalformedResponse;
        return std::nullopt;
    }
    const auto version = ParseTag(*tag);
    if (!version) {
        LOG_WARN("Service", __func__, "Update",
                 "Ignoring release tag '{}': not a vMAJOR.MINOR.PATCH tag this build can "
                 "represent.", *tag);
        error = kErrorMalformedResponse;
        return std::nullopt;
    }

    ReleaseInfo info{};
    info.version = *version;

    // 资产名带版本号，且两个资产的名字都由 release.yml 生成，所以按后缀认而不是按全名
    // 拼——拼出来的名字和工作流一旦分叉，表现是永远查不到资产，而不是报错。
    std::string msiUrl;
    std::string checksumUrl;
    cursor = 0;
    while (auto asset = ExtractStringField(body, "browser_download_url", cursor)) {
        if (msiUrl.empty() && EndsWith(*asset, ".msi")) {
            msiUrl = *asset;
        } else if (checksumUrl.empty() && EndsWith(*asset, "_SHA256SUMS.txt")) {
            checksumUrl = *asset;
        }
    }
    if (msiUrl.empty() || checksumUrl.empty()) {
        LOG_WARN("Service", __func__, "Update",
                 "Release {} carries no MSI/checksum asset pair.", *tag);
        error = kErrorMalformedResponse;
        return std::nullopt;
    }

    const std::string fileName = FileNameFromUrl(msiUrl);
    if (!IsSafeFileName(fileName)) {
        error = kErrorBadAssetName;
        return std::nullopt;
    }

    info.msiUrl = Widen(msiUrl);
    info.checksumUrl = Widen(checksumUrl);
    info.msiFileName = Widen(fileName);
    return info;
}

std::optional<Version> LoadSkippedVersion() {
    wchar_t buffer[32]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kUpdateKey, kSkippedValue, RRF_RT_REG_SZ, nullptr,
                     buffer, &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    std::string narrow;
    for (const wchar_t ch : std::wstring_view(buffer)) {
        if (ch >= 128) return std::nullopt;
        narrow.push_back(static_cast<char>(ch));
    }
    // ParseTag 要 'v' 前缀，注册表里记的是纯版本号，这里补上而不是另写一个解析器。
    return ParseTag("v" + narrow);
}

void StoreSkippedVersion(const Version& version) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUpdateKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        LOG_WARN("Service", __func__, "Update", "Cannot record the skipped version (err={}).",
                 GetLastError());
        return;
    }

    const std::wstring text = Widen(ToString(version));
    const auto bytes = static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t));
    (void)RegSetValueExW(key, kSkippedValue, 0, REG_SZ,
                         reinterpret_cast<const BYTE*>(text.c_str()), bytes);
    RegCloseKey(key);
}

std::optional<bool> LoadAutoCheck() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kUpdateKey, kAutoCheckValue, RRF_RT_REG_DWORD, nullptr,
                     &value, &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return value != 0;
}

void StoreAutoCheck(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUpdateKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        LOG_WARN("Service", __func__, "Update",
                 "Cannot record the automatic update check setting (err={}).", GetLastError());
        return;
    }

    const DWORD value = enabled ? 1 : 0;
    (void)RegSetValueExW(key, kAutoCheckValue, 0, REG_DWORD,
                         reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

void MarkTrayRelaunchPending() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUpdateKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        LOG_WARN("Service", __func__, "Update",
                 "Cannot mark the tray for relaunch after the update (err={}).",
                 GetLastError());
        return;
    }

    const DWORD one = 1;
    (void)RegSetValueExW(key, kRelaunchTrayValue, 0, REG_DWORD,
                         reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegCloseKey(key);
}

void ClearTrayRelaunchPending() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUpdateKey, 0, KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS) {
        return;
    }
    (void)RegDeleteValueW(key, kRelaunchTrayValue);
    RegCloseKey(key);
}

bool TakeTrayRelaunchPending() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kUpdateKey, kRelaunchTrayValue, RRF_RT_REG_DWORD,
                     nullptr, &value, &size) != ERROR_SUCCESS) {
        return false;
    }
    ClearTrayRelaunchPending();
    return value != 0;
}

bool LaunchTrayInActiveSession(const std::wstring& exePath, uint32_t& error) {
    error = 0;

    const DWORD session = WTSGetActiveConsoleSessionId();
    if (session == 0xFFFFFFFF) {
        return false;  // 没有连着的控制台会话，等于没人登录
    }

    HANDLE token = nullptr;
    if (!WTSQueryUserToken(session, &token)) {
        // 登录界面、注销之后、以及纯远程会话都会走到这里。不是故障，调用方记一行 info。
        return false;
    }

    // 用户环境块。不给的话托盘拿到的是 SYSTEM 的环境，%APPDATA% 指向 SYSTEM 的配置文件，
    // 它写下的任何用户级状态都会落到错误的地方。
    void* environment = nullptr;
    const bool haveEnvironment = CreateEnvironmentBlock(&environment, token, FALSE) != FALSE;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    // 交互桌面。服务的默认桌面是 Service-0x0-3e7$，在那里创建的窗口用户看不见。
    wchar_t desktop[] = L"winsta0\\default";
    startup.lpDesktop = desktop;

    PROCESS_INFORMATION process{};
    std::wstring commandLine = L"\"" + exePath + L"\"";

    // WTSQueryUserToken 给的是该会话交互用户的令牌；管理员账户在 UAC 下拿到的是被过滤过的
    // 那一份，也就是中完整性——正是托盘需要的。这里不做任何提升。
    const BOOL ok = CreateProcessAsUserW(
        token, exePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
        CREATE_UNICODE_ENVIRONMENT, environment, nullptr, &startup, &process);
    const DWORD lastError = GetLastError();

    if (haveEnvironment) DestroyEnvironmentBlock(environment);
    CloseHandle(token);

    if (!ok) {
        error = lastError;
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool DownloadAndVerify(const ReleaseInfo& release, std::wstring& msiPath, uint32_t& error) {
    msiPath.clear();
    error = 0;

    const std::wstring directory = UpdateDirectory();
    if (directory.empty()) {
        error = ERROR_PATH_NOT_FOUND;
        return false;
    }

    // 测试钩子生效时不回退镜像，与检查那侧同一个理由。
    const bool allowMirror = ApiBaseUrlOverride().empty();

    const std::wstring target = directory + L"\\" + release.msiFileName;
    if (!HttpGetFile(release.msiUrl, target, kMaxMsiBytes, error)) {
        const uint32_t directError = error;
        // 已经是镜像地址时 WithMirror 原样返回，再试一次只是重复同一条失败的请求。
        const std::wstring mirrored = WithMirror(release.msiUrl);
        if (!allowMirror || mirrored == release.msiUrl) return false;

        LOG_WARN("Service", __func__, "Update",
                 "Direct installer download failed (err={}); falling back to the mirror {}.",
                 directError, NarrowAscii(kMirrorPrefix));
        if (!HttpGetFile(mirrored, target, kMaxMsiBytes, error)) return false;
    }

    std::string checksums;
    if (!HttpGetText(release.checksumUrl, nullptr, kMaxChecksumBytes, checksums, error)) {
        const uint32_t directError = error;
        const std::wstring mirrored = WithMirror(release.checksumUrl);
        if (!allowMirror || mirrored == release.checksumUrl) {
            DeleteFileW(target.c_str());
            return false;
        }

        LOG_WARN("Service", __func__, "Update",
                 "Direct checksum download failed (err={}); falling back to the mirror {}.",
                 directError, NarrowAscii(kMirrorPrefix));
        if (!HttpGetText(mirrored, nullptr, kMaxChecksumBytes, checksums, error)) {
            DeleteFileW(target.c_str());
            return false;
        }
    }

    std::string narrowName;
    for (const wchar_t ch : release.msiFileName) narrowName.push_back(static_cast<char>(ch));

    const auto expected = FindChecksum(checksums, narrowName);
    if (!expected) {
        LOG_ERROR("Service", __func__, "Update",
                  "The checksum file carries no entry for the downloaded installer.");
        DeleteFileW(target.c_str());
        error = kErrorChecksumMissing;
        return false;
    }

    std::string actual;
    if (!ComputeFileSha256(target, actual, error)) {
        DeleteFileW(target.c_str());
        return false;
    }
    if (actual != *expected) {
        LOG_ERROR("Service", __func__, "Update",
                  "SHA256 mismatch on the downloaded installer; discarding it.");
        DeleteFileW(target.c_str());
        error = kErrorChecksumMismatch;
        return false;
    }

    msiPath = target;
    return true;
}

bool LaunchInstaller(const std::wstring& msiPath, uint32_t& error) {
    error = 0;

    wchar_t systemDirectory[MAX_PATH]{};
    if (GetSystemDirectoryW(systemDirectory, MAX_PATH) == 0) {
        error = GetLastError();
        return false;
    }
    const std::wstring exe = std::wstring(systemDirectory) + L"\\msiexec.exe";
    std::wstring commandLine = L"\"" + exe + L"\" /i \"" + msiPath + L"\" /qn /norestart";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    // 待验证的假设：MSI 会停掉本服务，而 msiexec 是本服务拉起的子进程。DETACHED_PROCESS
    // 断开控制台继承，CREATE_BREAKAWAY_FROM_JOB 让它脱离服务所在的作业对象，两者合起来
    // 应当让它在服务被停掉之后继续跑完。这一条还没有在真机上验证过；若不成立，安装会在
    // 服务停止的那一刻半途而废，届时要改成先落一个计划任务或独立的引导进程再退出。
    DWORD flags = DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB | CREATE_NEW_PROCESS_GROUP;
    BOOL ok = CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, flags,
                             nullptr, nullptr, &startup, &process);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
        // 作业对象不允许脱离时 CREATE_BREAKAWAY_FROM_JOB 直接让创建失败。没有作业约束的
        // 服务进程本来就不需要这个标志，退而求其次总比装不上好。
        flags &= ~static_cast<DWORD>(CREATE_BREAKAWAY_FROM_JOB);
        commandLine = L"\"" + exe + L"\" /i \"" + msiPath + L"\" /qn /norestart";
        ok = CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, flags,
                            nullptr, nullptr, &startup, &process);
    }
    if (!ok) {
        error = GetLastError();
        return false;
    }

    // 句柄立刻关掉：等它结束没有意义，msiexec 要做的第一件事就是停掉本服务。
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

} // namespace Service::Update
