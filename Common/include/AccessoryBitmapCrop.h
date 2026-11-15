#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace AccessoryBitmapCrop {

struct CropRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return width > 0 && height > 0;
    }
};

// 从 BGRA8 像素缓冲区计算非透明内容的紧致外接矩形。
// 配件素材 PNG 往往带大面积透明留白，裁去 alpha <= alphaThreshold 的边缘后
// 才能在设置页和横幅卡片里保持一致的视觉占比。若整图全透明则回退为原图尺寸。
[[nodiscard]] inline CropRect ComputeAlphaCropBounds(
    const uint8_t* bgraPixels,
    uint32_t width,
    uint32_t height,
    uint8_t alphaThreshold = 2) noexcept {
    if (!bgraPixels || width == 0 || height == 0) {
        return {};
    }

    uint32_t minX = width;
    uint32_t minY = height;
    uint32_t maxX = 0;
    uint32_t maxY = 0;
    bool foundOpaque = false;

    for (uint32_t y = 0; y < height; ++y) {
        const size_t rowOffset = static_cast<size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t alpha = bgraPixels[rowOffset + static_cast<size_t>(x) * 4 + 3];
            if (alpha > alphaThreshold) {
                if (!foundOpaque) {
                    minX = maxX = x;
                    minY = maxY = y;
                    foundOpaque = true;
                } else {
                    minX = (std::min)(minX, x);
                    minY = (std::min)(minY, y);
                    maxX = (std::max)(maxX, x);
                    maxY = (std::max)(maxY, y);
                }
            }
        }
    }

    if (!foundOpaque) {
        return CropRect{0, 0, width, height};
    }

    return CropRect{
        minX,
        minY,
        maxX - minX + 1,
        maxY - minY + 1,
    };
}

// 从源 BGRA8 像素缓冲区拷贝 rect 指定区域，输出紧致行排列的新像素缓冲区。
[[nodiscard]] inline std::vector<uint8_t> ExtractPremultipliedSubrect(
    const uint8_t* srcBgraPixels,
    uint32_t srcWidth,
    uint32_t srcHeight,
    const CropRect& rect) {
    if (!srcBgraPixels || !rect.IsValid() || rect.x >= srcWidth || rect.y >= srcHeight ||
        rect.width > srcWidth - rect.x || rect.height > srcHeight - rect.y) {
        return {};
    }

    const size_t srcStride = static_cast<size_t>(srcWidth) * 4;
    const size_t dstStride = static_cast<size_t>(rect.width) * 4;
    std::vector<uint8_t> cropped(dstStride * rect.height);

    for (uint32_t row = 0; row < rect.height; ++row) {
        const uint8_t* srcRow = srcBgraPixels +
                                static_cast<size_t>(rect.y + row) * srcStride +
                                static_cast<size_t>(rect.x) * 4;
        uint8_t* dstRow = cropped.data() + static_cast<size_t>(row) * dstStride;
        std::memcpy(dstRow, srcRow, dstStride);
    }

    return cropped;
}

}  // namespace AccessoryBitmapCrop
