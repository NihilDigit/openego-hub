#pragma once

#include <string>

namespace Thp {

// 从 SCM 里 HuaweiThpService 的 ImagePath 推导原厂安装目录。
//
// 不用「自身所在目录」是因为本程序不再安装到原厂目录里；也不硬编码路径，那样在原厂装到
// 别处时会失败，而失败现象是 LoadLibrary 找不到 THP_Service.dll，与真正的原因隔了几层。
// 服务即使处于停止或禁用状态，其 ImagePath 依然可读，所以让路之后仍能推导成功。
[[nodiscard]] bool DiscoverVendorDirectory(std::wstring &directory) noexcept;

// ImagePath 可能带引号，也可能跟着参数。仅取可执行文件路径所在的目录。
// 独立出来是为了能不依赖 SCM 单独测试。
[[nodiscard]] bool VendorDirectoryFromImagePath(const std::wstring &imagePath,
                                                std::wstring &directory) noexcept;

} // namespace Thp
