#include "ThpConfig.h"

#include <windows.h>

#include <charconv>
#include <optional>
#include <string_view>
#include <vector>

namespace Thp {

namespace {

constexpr const wchar_t *kConfigPath =
    L"C:\\Program Files\\Huawei\\HuaweiThpService\\HuaweiTHP.config.xml";

constexpr const char *kTemplate =
    "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<Config>\r\n"
    "  <VHFFunction>1</VHFFunction>\r\n"
    "  <LogFunction>0</LogFunction>\r\n"
    "</Config>";

// 这里没有用 MSXML 复刻 System.Xml。回调由 THP_Service.dll 的线程发起，那些线程未必
// 初始化过 COM，也无从得知它们的套间模型；为一次读两个整数在回调里反复 CoInitialize
// 是把一个文本问题换成一个线程亲和性问题。文件格式固定且由本进程生成，直接按文本处理。
[[nodiscard]] std::optional<std::string> ReadAll(const wchar_t *path) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > (1 << 20)) {
        CloseHandle(file);
        return std::nullopt;
    }

    std::string text(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok = text.empty() ||
                    ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok) return std::nullopt;
    text.resize(read);
    return text;
}

[[nodiscard]] bool WriteAll(const wchar_t *path, std::string_view text) noexcept {
    // 原厂在保存前把文件属性重置为 Normal，用来绕开只读标记。同样照做，否则用户
    // 或别的工具设过只读时写入会静默失败，而调用方只能看到一个 -1。
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()),
                              &written, nullptr) &&
                    written == text.size();
    CloseHandle(file);
    return ok;
}

// 取 <name>...</name> 之间的原文。找不到标签返回 nullopt，标签存在但内容为空返回空串——
// 这两者在原厂的分支里都归到「返回 0」，但保留区分能让调用方读起来不必猜。
[[nodiscard]] std::optional<std::string_view> ElementText(std::string_view xml,
                                                          std::string_view name) noexcept {
    const std::string open = "<" + std::string(name) + ">";
    const std::string close = "</" + std::string(name) + ">";

    const size_t begin = xml.find(open);
    if (begin == std::string_view::npos) return std::nullopt;
    const size_t from = begin + open.size();
    const size_t end = xml.find(close, from);
    if (end == std::string_view::npos) return std::nullopt;

    return xml.substr(from, end - from);
}

[[nodiscard]] std::string_view Trim(std::string_view s) noexcept {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

// 原厂用 XmlDocument.Load 是否抛异常来判定，非法就删文件等下次重建。这里退到一个更窄的
// 判据：根元素 Config 必须成对存在。够不够严只影响「损坏的文件何时被重建」，而重建本身
// 是幂等的，宽一点不会把正确的文件误删。
[[nodiscard]] bool LooksLikeConfig(std::string_view xml) noexcept {
    return xml.find("<Config>") != std::string_view::npos &&
           xml.find("</Config>") != std::string_view::npos;
}

[[nodiscard]] std::optional<int> ParseInt(std::string_view text) noexcept {
    const std::string_view t = Trim(text);
    if (t.empty()) return std::nullopt;
    int value = 0;
    const auto *last = t.data() + t.size();
    const auto result = std::from_chars(t.data(), last, value);
    if (result.ec != std::errc{} || result.ptr != last) return std::nullopt;
    return value;
}

} // namespace

const wchar_t *ConfigStore::FilePath() noexcept { return kConfigPath; }

ConfigStore &ConfigStore::Instance() noexcept {
    // 原厂是双检锁单例；这里用函数内静态量，初始化的线程安全由语言保证。
    static ConfigStore instance;
    return instance;
}

ConfigStore::ConfigStore() noexcept { CreateFileIfMissing(); }

void ConfigStore::CreateFileIfMissing() noexcept {
    if (GetFileAttributesW(kConfigPath) != INVALID_FILE_ATTRIBUTES) return;
    (void)WriteAll(kConfigPath, kTemplate);
}

int ConfigStore::GetPenEleValue(int index) noexcept {
    // 原厂只认 0 和 1，其余参数不进入任何分支，落到初值 0。
    const char *element = index == 0 ? "VHFFunction" : (index == 1 ? "LogFunction" : nullptr);
    if (!element) return 0;

    std::lock_guard<std::mutex> guard(m_lock);
    CreateFileIfMissing();

    const auto xml = ReadAll(kConfigPath);
    if (!xml) return 0;

    if (!LooksLikeConfig(*xml)) {
        DeleteFileW(kConfigPath);
        return 0;
    }

    const auto text = ElementText(*xml, element);
    if (!text) return 0;

    const auto value = ParseInt(*text);
    return value ? *value : 0;
}

int ConfigStore::SetPenEleValue(int value) noexcept {
    std::lock_guard<std::mutex> guard(m_lock);
    CreateFileIfMissing();

    const auto xml = ReadAll(kConfigPath);
    if (!xml || !LooksLikeConfig(*xml)) return -1;

    const auto current = ElementText(*xml, "VHFFunction");
    if (!current) return -1;

    // 只替换 VHFFunction 的文本，其余字节原样保留。原厂走的是 XmlDocument 改节点再 Save，
    // 效果同样是保住 LogFunction 与文件其余内容。
    const size_t offset = static_cast<size_t>(current->data() - xml->data());
    std::string updated = *xml;
    updated.replace(offset, current->size(), std::to_string(value));

    return WriteAll(kConfigPath, updated) ? 0 : -1;
}

} // namespace Thp
