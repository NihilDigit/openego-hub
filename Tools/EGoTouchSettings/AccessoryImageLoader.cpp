#include "pch.h"

#include "AccessoryImageLoader.h"
#include "ManagedResource.h"

#include <cmath>
#include <fstream>
#include <mutex>
#include <optional>
#include <unordered_map>

using namespace winrt;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Storage::Streams;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Media::Imaging;

namespace winrt::EGoTouchSettings::implementation::AccessoryImages {

namespace {

std::wstring PCManagerRoot() {
    wchar_t root[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, 0, root))) return {};
    return std::wstring{root} + L"\\Huawei\\PCManager\\";
}

std::vector<uint8_t> ReadFileBytes(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const std::streamoff length = file.tellg();
    if (length <= 0) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), length)) return {};
    return bytes;
}

Asset FileAsset(const std::wstring& path) {
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    auto bytes = ReadFileBytes(path);
    if (bytes.empty()) return {};
    return Asset{std::move(bytes)};
}

std::wstring PenPluginPath(uint32_t modelId) {
    const std::wstring root = PCManagerRoot();
    if (root.empty()) return {};
    const wchar_t* dll = nullptr;
    switch (modelId) {
    case 0x00011Au: dll = L"CD52PenApp.dll"; break;
    case 0x00011Bu: dll = L"CD54PenApp.dll"; break;
    case 0x01011Bu: dll = L"CD54RPenApp.dll"; break;
    case 0x443002u: dll = L"CD54SPenApp.dll"; break;
    default: return {};
    }
    return root +
        L"components\\accessories_center\\accessories_app\\AccessoryApp\\Lib\\Plugins\\" +
        dll;
}

std::wstring PenConnectResourceName(uint32_t modelId) {
    wchar_t name[64]{};
    std::swprintf(name, std::size(name),
                  L"resources/pic_%08x_connect@1.5x.png", modelId << 8);
    return name;
}

Asset EmbeddedAsset(const std::wstring& plugin, const std::wstring& resource) {
    if (plugin.empty() || resource.empty()) return {};
    std::vector<uint8_t> bytes;
    if (!ManagedRes::ReadEmbeddedResource(plugin, resource, bytes) || bytes.empty()) return {};
    return Asset{std::move(bytes)};
}

Asset ResolveGenericPen() {
    const std::wstring root = PCManagerRoot();
    if (root.empty()) return {};
    const std::wstring pluginRoot = root +
        L"components\\accessories_center\\accessories_app\\AccessoryApp\\Lib\\Plugins\\";
    constexpr const wchar_t* plugins[] = {
        L"CD54PenApp.dll", L"CD54RPenApp.dll", L"CD52PenApp.dll", L"CD54SPenApp.dll",
    };
    for (const wchar_t* dll : plugins) {
        if (auto asset = EmbeddedAsset(
                pluginRoot + dll, L"resources/pic_vector_connect@1.5x.png")) {
            return asset;
        }
    }
    return {};
}

} // namespace

Asset ResolvePen(uint32_t modelId) {
    // Settings 进程启动时设备页先解析；弹窗随后拿同一份缓存，因此即使 PC Manager 正在更新
    // 资源，两处也不会一个取到卡片图、另一个取到插件回退图。
    static std::mutex cacheMutex;
    static std::unordered_map<uint32_t, Asset> cache;
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (const auto it = cache.find(modelId); it != cache.end()) return it->second;

    Asset resolved;
    const std::wstring root = PCManagerRoot();
    if (!root.empty() && modelId != 0) {
        const std::wstring card = root +
            L"components\\accessories_center\\res\\drawable\\cards\\" +
            std::to_wstring(modelId) + L"_00.png";
        resolved = FileAsset(card);
    }

    const std::wstring plugin = resolved ? std::wstring{} : PenPluginPath(modelId);
    if (!resolved && !plugin.empty()) {
        resolved = EmbeddedAsset(plugin, PenConnectResourceName(modelId));
        // 当前安装版 CD54R 插件没有自己的连接图，原厂也回退到 CD54 银色笔图。
        if (!resolved && modelId == 0x01011Bu) {
            resolved = EmbeddedAsset(plugin, PenConnectResourceName(0x00011Bu));
        }
        if (!resolved) {
            resolved = EmbeddedAsset(plugin, L"resources/pic_vector_connect@1.5x.png");
        }
    }
    if (!resolved) resolved = ResolveGenericPen();
    if (resolved) cache.emplace(modelId, resolved);
    return resolved;
}

Asset ResolveKeyboard() {
    static std::mutex cacheMutex;
    static std::optional<Asset> cache;
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (cache) return *cache;

    const std::wstring root = PCManagerRoot();
    if (root.empty()) return {};
    const std::wstring cards =
        root + L"components\\accessories_center\\res\\drawable\\cards\\";
    Asset resolved = FileAsset(cards + L"RX0H_00.png");
    if (!resolved) resolved = FileAsset(cards + L"RX0I_00.png");
    if (resolved) cache = resolved;
    return resolved;
}

IAsyncOperation<Windows::Foundation::Size> DecodeCroppedAsync(
        std::vector<uint8_t> encoded,
        SoftwareBitmapSource const& destination) {
    if (encoded.empty() || !destination) co_return Windows::Foundation::Size{};

    InMemoryRandomAccessStream stream;
    DataWriter writer(stream);
    writer.WriteBytes(encoded);
    co_await writer.StoreAsync();
    co_await writer.FlushAsync();
    writer.DetachStream();
    stream.Seek(0);

    const BitmapDecoder decoder = co_await BitmapDecoder::CreateAsync(stream);
    const uint32_t width = decoder.PixelWidth();
    const uint32_t height = decoder.PixelHeight();
    if (width == 0 || height == 0) co_return Windows::Foundation::Size{};

    const PixelDataProvider pixels = co_await decoder.GetPixelDataAsync(
        BitmapPixelFormat::Bgra8, BitmapAlphaMode::Straight, BitmapTransform{},
        ExifOrientationMode::IgnoreExifOrientation, ColorManagementMode::DoNotColorManage);
    const com_array<uint8_t> bgra = pixels.DetachPixelData();

    uint32_t left = width;
    uint32_t top = height;
    uint32_t right = 0;
    uint32_t bottom = 0;
    bool found = false;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t alpha = (static_cast<size_t>(y) * width + x) * 4 + 3;
            if (alpha >= static_cast<size_t>(bgra.size()) ||
                bgra[static_cast<uint32_t>(alpha)] <= 2) continue;
            found = true;
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x);
            bottom = std::max(bottom, y);
        }
    }

    BitmapBounds bounds{0, 0, width, height};
    if (found) {
        const uint32_t contentWidth = right - left + 1;
        const uint32_t contentHeight = bottom - top + 1;
        const uint32_t padding = std::max(
            2u, static_cast<uint32_t>(std::lround(std::max(contentWidth, contentHeight) * 0.02)));
        const uint32_t x = left > padding ? left - padding : 0;
        const uint32_t y = top > padding ? top - padding : 0;
        const uint32_t cropRight = std::min(width, right + padding + 1);
        const uint32_t cropBottom = std::min(height, bottom + padding + 1);
        bounds = BitmapBounds{x, y, cropRight - x, cropBottom - y};
    }

    BitmapTransform transform;
    transform.Bounds(bounds);
    const SoftwareBitmap bitmap = co_await decoder.GetSoftwareBitmapAsync(
        BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied, transform,
        ExifOrientationMode::IgnoreExifOrientation, ColorManagementMode::DoNotColorManage);
    co_await destination.SetBitmapAsync(bitmap);
    co_return Windows::Foundation::Size{
        static_cast<float>(bounds.Width), static_cast<float>(bounds.Height)};
}

} // namespace winrt::EGoTouchSettings::implementation::AccessoryImages
