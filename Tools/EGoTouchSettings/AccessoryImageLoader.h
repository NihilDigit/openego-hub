#pragma once

#include <cstdint>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Foundation.h>

namespace winrt::EGoTouchSettings::implementation::AccessoryImages {

// 图片始终从本机已安装的华为组件读取；encoded 只存在于进程内，不会写入安装目录或仓库。
struct Asset {
    std::vector<uint8_t> encoded;

    explicit operator bool() const noexcept { return !encoded.empty(); }
};

// 设备页的卡片图是首选。只有对应型号没有卡片图时才尝试 PenApp 内的连接图，最后退到
// 已安装插件提供的通用笔图。弹窗和设备页必须调用同一个 resolver，避免型号口径分叉。
Asset ResolvePen(uint32_t modelId);
Asset ResolveKeyboard();

// 本机整机图（平板与键盘）。厂商没有按机型或产品码命名的整机照，PC Manager 的资源目录里
// 只有配件的产品图和设备类别的抽象图标；唯一一张画着这台机器的图嵌在本机键盘插件里，
// 走的是与笔图相同的托管资源读取路径。
Asset ResolveMachine();

// 解码后按 alpha 非透明像素的外接矩形裁掉画布空白，并留出少量抗锯齿安全边距。
// 这是 Windows Imaging + WinUI 的通用加载路径，不依赖 Direct2D，也不修改源文件。
winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Foundation::Size>
DecodeCroppedAsync(
    std::vector<uint8_t> encoded,
    winrt::Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource const& destination);

} // namespace winrt::EGoTouchSettings::implementation::AccessoryImages
