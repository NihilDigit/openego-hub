#include "pch.h"

#include "AccessoryBitmapCrop.h"
#include "AccessoryImageLoader.h"
#include "ManagedResource.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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

constexpr uint32_t kDiskCacheMagic = 0x45474F43u;  // "EGOC"
constexpr uint32_t kDiskCacheVersion = 1u;
constexpr uint32_t kMaxCachedDimension = 4096u;
constexpr size_t kMaxDiskCacheEntries = 16;
constexpr size_t kMaxMemoryCacheEntries = 16;

struct DecodedBitmapEntry {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> premulBgra;
    uint64_t useOrder = 0;

    bool IsValid() const noexcept {
        return width > 0 && height > 0 && width <= kMaxCachedDimension &&
               height <= kMaxCachedDimension &&
               premulBgra.size() == static_cast<size_t>(width) * height * 4;
    }
};

struct alignas(4) DiskCacheHeader {
    uint32_t magic = kDiskCacheMagic;
    uint32_t version = kDiskCacheVersion;
    uint32_t width = 0;
    uint32_t height = 0;
};

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

uint32_t Rotr32(uint32_t x, uint32_t n) noexcept {
    return (x >> n) | (x << (32u - n));
}

std::string Sha256Hex(const std::vector<uint8_t>& input) {
    static constexpr uint32_t kRoundConstants[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };
    uint32_t h[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    const uint64_t bitLen = static_cast<uint64_t>(input.size()) * 8ull;
    std::vector<uint8_t> padded(input);
    padded.push_back(0x80u);
    while ((padded.size() % 64u) != 56u) {
        padded.push_back(0u);
    }
    for (int i = 7; i >= 0; --i) {
        padded.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFFu));
    }

    for (size_t offset = 0; offset < padded.size(); offset += 64u) {
        uint32_t w[64]{};
        for (int i = 0; i < 16; ++i) {
            const size_t idx = offset + static_cast<size_t>(i) * 4u;
            w[i] = (static_cast<uint32_t>(padded[idx + 0]) << 24) |
                   (static_cast<uint32_t>(padded[idx + 1]) << 16) |
                   (static_cast<uint32_t>(padded[idx + 2]) << 8) |
                   static_cast<uint32_t>(padded[idx + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = Rotr32(w[i - 15], 7) ^ Rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr32(w[i - 2], 17) ^ Rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = Rotr32(e, 6) ^ Rotr32(e, 11) ^ Rotr32(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = hh + S1 + ch + kRoundConstants[i] + w[i];
            const uint32_t S0 = Rotr32(a, 2) ^ Rotr32(a, 13) ^ Rotr32(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = S0 + maj;

            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 8; ++i) {
        for (int shift = 7; shift >= 0; --shift) {
            out[static_cast<size_t>(i * 8 + (7 - shift))] =
                kHex[(h[i] >> (shift * 4)) & 0xFu];
        }
    }
    return out;
}

std::wstring DiskCacheDirectory() {
    wchar_t localAppData[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) {
        return {};
    }
    std::wstring base = std::wstring{localAppData} + L"\\OpenEGoHub";
    (void)CreateDirectoryW(base.c_str(), nullptr);
    base += L"\\Cache\\";
    (void)CreateDirectoryW(base.c_str(), nullptr);
    return base;
}

std::wstring WidenAscii(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

void PruneDiskCache(const std::wstring& dir) {
    if (dir.empty()) return;
    struct FileEntry {
        std::wstring path;
        ULONGLONG writeTime = 0;
    };
    std::vector<FileEntry> files;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"*.bin").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER ft{};
        ft.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        ft.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        files.push_back({dir + fd.cFileName, ft.QuadPart});
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    if (files.size() <= kMaxDiskCacheEntries) return;
    std::sort(files.begin(), files.end(), [](const FileEntry& lhs, const FileEntry& rhs) {
        return lhs.writeTime > rhs.writeTime;
    });
    for (size_t i = kMaxDiskCacheEntries; i < files.size(); ++i) {
        (void)DeleteFileW(files[i].path.c_str());
    }
}

bool TryLoadFromDiskCache(const std::string& sha256Hex, DecodedBitmapEntry& out) {
    const std::wstring dir = DiskCacheDirectory();
    if (dir.empty()) return false;
    const std::wstring path = dir + WidenAscii(sha256Hex) + L".bin";
    const auto bytes = ReadFileBytes(path);
    if (bytes.size() <= sizeof(DiskCacheHeader)) return false;

    DiskCacheHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kDiskCacheMagic || header.version != kDiskCacheVersion ||
        header.width == 0 || header.height == 0 ||
        header.width > kMaxCachedDimension || header.height > kMaxCachedDimension) {
        return false;
    }

    const size_t expectedPixels =
        static_cast<size_t>(header.width) * static_cast<size_t>(header.height) * 4u;
    if (bytes.size() != sizeof(DiskCacheHeader) + expectedPixels) return false;

    out.width = header.width;
    out.height = header.height;
    out.premulBgra.assign(bytes.begin() + sizeof(DiskCacheHeader), bytes.end());
    return out.IsValid();
}

void SaveToDiskCache(const std::string& sha256Hex, const DecodedBitmapEntry& entry) {
    if (!entry.IsValid()) return;
    const std::wstring dir = DiskCacheDirectory();
    if (dir.empty()) return;

    const std::wstring finalPath = dir + WidenAscii(sha256Hex) + L".bin";
    const std::wstring tempPath =
        finalPath + L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";

    std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
    if (!file) return;
    DiskCacheHeader header{};
    header.width = entry.width;
    header.height = entry.height;
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(entry.premulBgra.data()),
               static_cast<std::streamsize>(entry.premulBgra.size()));
    file.flush();
    const bool writeOk = static_cast<bool>(file);
    file.close();

    if (!writeOk ||
        !MoveFileExW(tempPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        (void)DeleteFileW(tempPath.c_str());
        return;
    }
    PruneDiskCache(dir);
}

std::mutex g_memoryCacheMutex;
std::unordered_map<std::string, DecodedBitmapEntry> g_memoryCache;
uint64_t g_memoryUseCounter = 0;

std::optional<DecodedBitmapEntry> TryGetFromMemoryCache(const std::string& sha256Hex) {
    std::lock_guard<std::mutex> lock(g_memoryCacheMutex);
    const auto it = g_memoryCache.find(sha256Hex);
    if (it == g_memoryCache.end()) return std::nullopt;
    it->second.useOrder = ++g_memoryUseCounter;
    return it->second;
}

void PutInMemoryCache(const std::string& sha256Hex, const DecodedBitmapEntry& entry) {
    if (!entry.IsValid()) return;
    std::lock_guard<std::mutex> lock(g_memoryCacheMutex);
    DecodedBitmapEntry stored = entry;
    stored.useOrder = ++g_memoryUseCounter;
    g_memoryCache[sha256Hex] = std::move(stored);
    if (g_memoryCache.size() <= kMaxMemoryCacheEntries) return;

    auto oldest = g_memoryCache.begin();
    for (auto it = g_memoryCache.begin(); it != g_memoryCache.end(); ++it) {
        if (it->second.useOrder < oldest->second.useOrder) oldest = it;
    }
    g_memoryCache.erase(oldest);
}

SoftwareBitmap CreateSoftwareBitmapFromBgra(
        uint32_t width, uint32_t height, const std::vector<uint8_t>& premulBgra) {
    if (width == 0 || height == 0 ||
        premulBgra.size() != static_cast<size_t>(width) * height * 4) {
        return nullptr;
    }
    DataWriter writer;
    writer.WriteBytes(array_view<const uint8_t>{premulBgra});
    const IBuffer buffer = writer.DetachBuffer();
    return SoftwareBitmap::CreateCopyFromBuffer(
        buffer, BitmapPixelFormat::Bgra8,
        static_cast<int32_t>(width), static_cast<int32_t>(height),
        BitmapAlphaMode::Premultiplied);
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
    const std::wstring pluginRoot = root +
        L"components\\accessories_center\\accessories_app\\AccessoryApp\\Lib\\Plugins\\";
    Asset resolved = EmbeddedAsset(
        pluginRoot + L"GaokunKeyboardApp.dll", L"resources/gaokun_raven_picture.png");
    if (!resolved) {
        resolved = EmbeddedAsset(
            pluginRoot + L"DiracRKeyboardApp.dll", L"resources/gaokun_raven_picture.png");
    }
    const std::wstring cards =
        root + L"components\\accessories_center\\res\\drawable\\cards\\";
    if (!resolved) resolved = FileAsset(cards + L"RX0H_00.png");
    if (!resolved) resolved = FileAsset(cards + L"RX0I_00.png");
    if (resolved) cache = resolved;
    return resolved;
}

Asset ResolveMachine() {
    static std::mutex cacheMutex;
    static std::optional<Asset> cache;
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (cache) return *cache;

    const std::wstring root = PCManagerRoot();
    if (root.empty()) return {};

    const std::wstring plugin = root +
        L"components\\accessories_center\\accessories_app\\AccessoryApp\\Lib\\Plugins\\"
        L"GaokunKeyboardApp.dll";

    // 分离态与吸附态各有一张 648x648，画的都是平板加键盘。取吸附态：那是这台机器平时的
    // 样子，两个部件贴合成一体，也更接近产品照。
    Asset resolved = EmbeddedAsset(plugin, L"resources/snapping.png");
    if (!resolved) resolved = EmbeddedAsset(plugin, L"resources/detach.png");
    if (resolved) cache = resolved;
    return resolved;
}

IAsyncOperation<Windows::Foundation::Size> DecodeCroppedAsync(
        std::vector<uint8_t> encoded,
        SoftwareBitmapSource const& destination) {
    if (encoded.empty() || !destination) co_return Windows::Foundation::Size{};

    winrt::apartment_context uiThread;
    const std::string key = Sha256Hex(encoded);

    if (const auto cached = TryGetFromMemoryCache(key)) {
        const SoftwareBitmap bitmap = CreateSoftwareBitmapFromBgra(
            cached->width, cached->height, cached->premulBgra);
        if (bitmap) {
            co_await destination.SetBitmapAsync(bitmap);
            co_return Windows::Foundation::Size{
                static_cast<float>(cached->width), static_cast<float>(cached->height)};
        }
    }

    co_await winrt::resume_background();

    DecodedBitmapEntry entry;
    if (!TryLoadFromDiskCache(key, entry)) {
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
        if (width > 0 && height > 0) {
            const PixelDataProvider pixels = co_await decoder.GetPixelDataAsync(
                BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied, BitmapTransform{},
                ExifOrientationMode::IgnoreExifOrientation, ColorManagementMode::DoNotColorManage);
            const com_array<uint8_t> bgra = pixels.DetachPixelData();
            if (bgra.size() >= static_cast<size_t>(width) * height * 4) {
                const auto tight = AccessoryBitmapCrop::ComputeAlphaCropBounds(
                    bgra.data(), width, height, 2);
                AccessoryBitmapCrop::CropRect crop{0, 0, width, height};
                if (tight.IsValid() &&
                    !(tight.x == 0 && tight.y == 0 &&
                      tight.width == width && tight.height == height)) {
                    const uint32_t padding = (std::max)(
                        2u, static_cast<uint32_t>(std::lround(
                                (std::max)(tight.width, tight.height) * 0.02)));
                    const uint32_t x = tight.x > padding ? tight.x - padding : 0;
                    const uint32_t y = tight.y > padding ? tight.y - padding : 0;
                    const uint32_t cropRight = (std::min)(width, tight.x + tight.width + padding);
                    const uint32_t cropBottom = (std::min)(height, tight.y + tight.height + padding);
                    crop = AccessoryBitmapCrop::CropRect{x, y, cropRight - x, cropBottom - y};
                }

                auto croppedPixels = AccessoryBitmapCrop::ExtractPremultipliedSubrect(
                    bgra.data(), width, height, crop);
                if (!croppedPixels.empty()) {
                    entry.width = crop.width;
                    entry.height = crop.height;
                    entry.premulBgra = std::move(croppedPixels);
                    SaveToDiskCache(key, entry);
                }
            }
        }
    }
    if (entry.IsValid()) {
        PutInMemoryCache(key, entry);
    }

    co_await uiThread;
    if (!entry.IsValid()) co_return Windows::Foundation::Size{};

    const SoftwareBitmap bitmap = CreateSoftwareBitmapFromBgra(
        entry.width, entry.height, entry.premulBgra);
    if (!bitmap) co_return Windows::Foundation::Size{};
    co_await destination.SetBitmapAsync(bitmap);
    co_return Windows::Foundation::Size{
        static_cast<float>(entry.width), static_cast<float>(entry.height)};
}

} // namespace winrt::EGoTouchSettings::implementation::AccessoryImages
