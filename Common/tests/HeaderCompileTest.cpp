#include "config/ConfigCatalog.h"
#include "config/ConfigKeyId.h"
#include "config/ConfigKeyMap.h"
#include "config/ConfigSchemaSnapshot.h"
#include "FrameLayout.h"
#include "GuiLogSink.h"
#include "Logger.h"
#include "PenButtonConfig.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestCommonHeadersExposeExpectedTypes() {
    Require(Frame::kTotalFrameSize == 5402, "FrameLayout.h should expose frame constants");
#if defined(NDEBUG)
    Require(Common::GuiLogSink::kMaxLines == Common::GuiLogSink::kReleaseMaxLines, "GuiLogSink.h should expose release kMaxLines");
#else
    Require(Common::GuiLogSink::kMaxLines == Common::GuiLogSink::kDebugMaxLines, "GuiLogSink.h should expose debug kMaxLines");
#endif
    Require(ToString(PenButtonMode::OemCustom) != nullptr, "PenButtonConfig.h should expose ToString(PenButtonMode)");
    Require(Common::Logger::Get() == nullptr, "Logger.h should expose Logger::Get without requiring initialization");
    static_assert(std::is_class_v<Config::ConfigCatalog>);
    static_assert(std::is_class_v<Config::ConfigCatalogBuilder>);
    Require(static_cast<uint16_t>(Config::ConfigKeyId::MaxKeyId) == 0x0300, "ConfigKeyId.h should expose MaxKeyId");
}

} // namespace

int main() {
    try {
        TestCommonHeadersExposeExpectedTypes();
        std::cout << "[TEST] CommonHeaderCompileTest passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] CommonHeaderCompileTest failed: " << ex.what() << '\n';
        return 1;
    }
}
