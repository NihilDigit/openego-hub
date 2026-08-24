#pragma once
// ManagedResource — 从 .NET 程序集里按名字取出一个嵌入资源的原始字节。
//
// 为什么需要它：华为选件中心把高分辨率的笔照片放在插件 DLL 的 WPF 资源流（*.g.resources）
// 里，散装文件里只有低分辨率版本。LoadLibraryEx + FindResource 那套只认 Win32 资源，读不到
// 托管资源，所以要自己走 PE → CLI 头 → 资源区 → .resources 二进制格式。
//
// 只读、只解析，不加载也不执行目标程序集——它是别人的二进制，我们只当数据文件看。
//
// 这是对第三方内部打包格式的依赖，对方改版就会失效。所有调用点都必须有兜底路径，把取不到
// 当作正常情况处理。

#include <cstdint>
#include <string>
#include <vector>

namespace ManagedRes {

/// 从 assemblyPath 指向的 .NET 程序集里找出名为 resourceName 的嵌入资源。
/// resourceName 是 .resources 流内的项名，例如 "resources/pic_00011b00_connect@1.5x.png"。
/// 找到返回 true 并填充 out；任何一步对不上都返回 false，不抛异常。
bool ReadEmbeddedResource(const std::wstring& assemblyPath,
                          const std::wstring& resourceName,
                          std::vector<uint8_t>& out);

} // namespace ManagedRes
