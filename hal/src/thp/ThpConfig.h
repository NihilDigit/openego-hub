#pragma once

#include <mutex>
#include <string>

// 原厂 XmlOperator 的等价实现。语义逐条对照 docs/vendor-service.md 的「XmlOperator」一节，
// 包括那些看起来像缺陷的部分：SetPenEleValue 只写 VHFFunction、Get 的越界参数返回 0、
// XML 非法时删除文件而不是修复。这些行为 THP_Service.dll 可能依赖，不做改良。
namespace Thp {

class ConfigStore {
public:
    static ConfigStore &Instance() noexcept;

    // index 0 取 VHFFunction，1 取 LogFunction，其余返回 0。任何失败路径都返回 0。
    [[nodiscard]] int GetPenEleValue(int index) noexcept;

    // 只写 VHFFunction，写入的是参数本身的十进制文本。成功返回 0，失败返回 -1。
    [[nodiscard]] int SetPenEleValue(int value) noexcept;

    // 原厂把路径硬编码为安装目录下的绝对路径。保留这一点，配置文件才与原厂服务是同一份，
    // 用户此前改过的值不会因为换了宿主而失效。
    static const wchar_t *FilePath() noexcept;

private:
    ConfigStore() noexcept;

    // 文件不存在时按原厂模板重建。
    void CreateFileIfMissing() noexcept;

    std::mutex m_lock;
};

} // namespace Thp
