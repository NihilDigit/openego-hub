#include "Logger.h"
#include "GuiLogSink.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void ShutdownLogger() {
    Common::Logger::Shutdown();
}

std::filesystem::path TestPath(const std::string& name) {
    return std::filesystem::current_path() / name;
}

bool ContainsLog(std::string_view text) {
    auto lines = Common::GuiLogSink::Instance()->GetLines();
    return std::any_of(lines.begin(), lines.end(), [text](const std::string& line) {
        return line.find(text) != std::string::npos;
    });
}

bool WaitForContains(std::string_view text) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (ContainsLog(text)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return ContainsLog(text);
}

void TestInvalidDirectoryDoesNotInitialize() {
    ShutdownLogger();
    const auto invalidDir = TestPath("CommonLoggerTest_invalid_dir_marker");
    std::filesystem::remove_all(invalidDir);

    {
        std::ofstream out(invalidDir);
        out << "This is a file, not a directory";
    }

    Common::GuiLogSink::Instance()->Clear();
    Common::Logger::Init("CommonLoggerTestInvalid", invalidDir);

    // Logging should be ignored safely because initialization failed
    LOG_WARN("Common", "LoggerTest", "InvalidDir", "invalid-dir marker");

    std::filesystem::remove(invalidDir);

    Require(!ContainsLog("invalid-dir marker"),
            "Logger should not initialize or log when directory is invalid");
}

void TestInitializeRepeatAndShutdown() {
    ShutdownLogger();
    const auto logDir = TestPath("CommonLoggerTest_repeat_logs");
    std::filesystem::remove_all(logDir);

    Common::GuiLogSink::Instance()->Clear();

    Common::Logger::Init("CommonLoggerTestRepeatA", logDir);
    Common::Logger::Init("CommonLoggerTestRepeatB", logDir); // Should be ignored safely

    LOG_WARN("Common", "LoggerTest", "RepeatInit", "repeat-init marker {}", 17);
    Require(WaitForContains("repeat-init marker 17"),
            "GuiLogSink should receive macro output after init");

    ShutdownLogger();
    std::filesystem::remove_all(logDir);
}

void TestExtraSinkReceivesFormattedMacroOutput() {
    ShutdownLogger();
    const auto logDir = TestPath("CommonLoggerTest_extra_sink_logs");
    std::filesystem::remove_all(logDir);

    Common::GuiLogSink::Instance()->Clear();
    Common::Logger::Init("CommonLoggerTestExtraSink", logDir);

    LOG_WARN("LayerA", "MethodB", "StateC", "value {}", 42);
    Require(WaitForContains("[LayerA] [MethodB] [StateC] value 42"),
            "LOG_WARN should emit formatted layer/method/state message to GuiLogSink");

    ShutdownLogger();
    std::filesystem::remove_all(logDir);
}

// 说明符曾被整段忽略：{:06X} 的 spec 是 ":06X"，解析时没跳过冒号，于是宽度和进制都丢了，
// 全部按十进制打印。而调用点普遍写成 "0x{:06X}"，前缀是手写的，结果就是一个看着像十六进制的
// 十进制数——日志因此会把人引向错误结论，比不打还糟。
void TestFormatSpecsAreHonoured() {
    Require(MiniFmt::format("{:06X}", 0x11Bu) == "00011B", "{:06X} should be zero-padded hex");
    Require(MiniFmt::format("{:02X}", 1u) == "01", "{:02X} should be zero-padded hex");
    Require(MiniFmt::format("{:x}", 0x2Fu) == "2f", "{:x} should be lowercase hex");
    Require(MiniFmt::format("{}", 283u) == "283", "plain {} stays decimal");
    // 十六进制与十进制在这个值上不同，才验得出进制真的生效了。
    Require(MiniFmt::format("{:X}", 283u) != MiniFmt::format("{}", 283u),
            "hex and decimal must differ for a value where they actually differ");
}

} // namespace

int main() {
    try {
        TestFormatSpecsAreHonoured();
        TestInvalidDirectoryDoesNotInitialize();
        TestInitializeRepeatAndShutdown();
        TestExtraSinkReceivesFormattedMacroOutput();
        ShutdownLogger();
        std::cout << "[TEST] CommonLoggerTest passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] CommonLoggerTest failed: " << ex.what() << '\n';
        ShutdownLogger();
        return 1;
    }
}
