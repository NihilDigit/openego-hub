#include "AccessoryBitmapCrop.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void SetPixel(std::vector<uint8_t>& buf, uint32_t width, uint32_t x, uint32_t y,
              uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    const size_t idx = (static_cast<size_t>(y) * width + x) * 4;
    buf[idx + 0] = b;
    buf[idx + 1] = g;
    buf[idx + 2] = r;
    buf[idx + 3] = a;
}

void TestEmptyAndNullInputs() {
    Require(!AccessoryBitmapCrop::ComputeAlphaCropBounds(nullptr, 8, 8).IsValid(),
            "null pixel pointer yields invalid crop rect");

    const uint8_t dummy[4] = {};
    Require(!AccessoryBitmapCrop::ComputeAlphaCropBounds(dummy, 0, 8).IsValid(),
            "zero width yields invalid crop rect");
    Require(!AccessoryBitmapCrop::ComputeAlphaCropBounds(dummy, 8, 0).IsValid(),
            "zero height yields invalid crop rect");
}

void TestFullyTransparentFallsBackToFullImage() {
    std::vector<uint8_t> pixels(6 * 4 * 4, 0);
    // alpha <= 2 属于透明留白，全图透明时应保留完整画布尺寸，避免生成 0x0 位图。
    SetPixel(pixels, 6, 2, 1, 255, 255, 255, 2);
    const auto rect = AccessoryBitmapCrop::ComputeAlphaCropBounds(pixels.data(), 6, 4);
    Require(rect.x == 0 && rect.y == 0 && rect.width == 6 && rect.height == 4,
            "fully transparent canvas keeps full dimensions");
}

void TestCropsTightBoundingBox() {
    std::vector<uint8_t> pixels(8 * 6 * 4, 0);
    SetPixel(pixels, 8, 1, 1, 10, 20, 30, 2);    // threshold noise, ignored
    SetPixel(pixels, 8, 2, 1, 10, 20, 30, 3);    // top-left non-transparent
    SetPixel(pixels, 8, 5, 4, 40, 50, 60, 200);  // bottom-right non-transparent

    const auto rect = AccessoryBitmapCrop::ComputeAlphaCropBounds(pixels.data(), 8, 6);
    Require(rect.x == 2 && rect.y == 1 && rect.width == 4 && rect.height == 4,
            "crop rect bounds match pixels with alpha > 2");

    const auto cropped = AccessoryBitmapCrop::ExtractPremultipliedSubrect(
        pixels.data(), 8, 6, rect);
    Require(cropped.size() == 4 * 4 * 4, "cropped buffer has tight stride");

    // top-left of cropped maps to (2, 1)
    Require(cropped[0] == 10 && cropped[1] == 20 && cropped[2] == 30 && cropped[3] == 3,
            "cropped top-left pixel preserved");

    // bottom-right of cropped (3, 3) maps to (5, 4)
    const size_t br = (3 * 4 + 3) * 4;
    Require(cropped[br + 0] == 40 && cropped[br + 1] == 50 &&
                cropped[br + 2] == 60 && cropped[br + 3] == 200,
            "cropped bottom-right pixel preserved");
}

void TestOutOfBoundsExtractReturnsEmpty() {
    std::vector<uint8_t> pixels(4 * 4 * 4, 255);
    Require(AccessoryBitmapCrop::ExtractPremultipliedSubrect(
                pixels.data(), 4, 4, AccessoryBitmapCrop::CropRect{2, 2, 3, 2})
                .empty(),
            "extract overflowing width returns empty");
    Require(AccessoryBitmapCrop::ExtractPremultipliedSubrect(
                pixels.data(), 4, 4, AccessoryBitmapCrop::CropRect{0, 0, 0, 2})
                .empty(),
            "zero-width rect returns empty");
}

}  // namespace

int main() {
    try {
        TestEmptyAndNullInputs();
        TestFullyTransparentFallsBackToFullImage();
        TestCropsTightBoundingBox();
        TestOutOfBoundsExtractReturnsEmpty();
        std::cout << "[TEST] Accessory bitmap crop tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
