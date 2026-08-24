// SessionWriter 的正确性基准是既有的 WriteBinaryFile:同一批帧与配置快照,
// 流式写出的 .dvrbin 必须与全内存写出的逐字节一致。字节等价成立后,读回
// 校验由 roundtrip 测试的既有覆盖背书,这里只抽查关键字段。

#include "DvrBinaryIO.h"
#include "DvrCoreTestSupport.h"

#include <cstdio>
#include <filesystem>
#include <vector>

using DvrCoreTest::MakeFrameSlot;
using DvrCoreTest::MakeRuntimeConfigSnapshot;
using DvrCoreTest::ReadBytes;
using DvrCoreTest::Require;
using DvrCoreTest::TempPath;

namespace {

std::vector<Dvr::DvrFrameSlot> MakeFrames(size_t count) {
    std::vector<Dvr::DvrFrameSlot> frames;
    frames.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        frames.push_back(MakeFrameSlot(1000 + i, 500000 + i * 8333, i + 1));
    }
    return frames;
}

void VerifyByteEquivalence(bool withConfig) {
    const auto frames = MakeFrames(5);
    const auto config = MakeRuntimeConfigSnapshot();
    const Dvr::RuntimeConfigSnapshot* configPtr = withConfig ? &config : nullptr;

    const auto batchPath = TempPath(withConfig ? "session_batch_cfg" : "session_batch");
    const auto streamPath = TempPath(withConfig ? "session_stream_cfg" : "session_stream");

    Require(Dvr::WriteBinaryFile(batchPath, frames, nullptr, nullptr, configPtr),
            "batch write succeeds");

    Dvr::SessionWriter writer;
    Require(writer.Begin(streamPath), "session Begin succeeds");
    Require(writer.IsActive(), "session is active after Begin");
    for (const auto& frame : frames) {
        Require(writer.Append(frame), "session Append succeeds");
    }
    Require(writer.FrameCount() == frames.size(), "session tracks frame count");
    Require(writer.Finalize(configPtr), "session Finalize succeeds");
    Require(!writer.IsActive(), "session is inactive after Finalize");

    std::filesystem::path partial = streamPath;
    partial += ".partial";
    Require(!std::filesystem::exists(partial), "partial file is removed after Finalize");

    Require(ReadBytes(batchPath) == ReadBytes(streamPath),
            "streamed file is byte-identical to batch-written file");

    std::vector<Solvers::HeatmapFrame> readBack;
    Dvr::RuntimeConfigSnapshot readConfig;
    int version = 0;
    std::string error;
    Require(Dvr::ReadBinaryFile(streamPath, readBack, version, nullptr, &error,
                                nullptr, nullptr, &readConfig),
            "streamed file reads back");
    Require(readBack.size() == frames.size(), "frame count reads back");
    Require(readBack[2].timestamp == 1002, "timestamp reads back");
    Require(readBack[4].receiveSystemEpochUs == 500000 + 4 * 8333, "epoch reads back");
    Require(readConfig.Empty() != withConfig, "config presence matches what was written");

    std::filesystem::remove(batchPath);
    std::filesystem::remove(streamPath);
}

void VerifyEmptySessionLeavesNoFile() {
    const auto path = TempPath("session_empty");
    Dvr::SessionWriter writer;
    Require(writer.Begin(path), "empty session Begin succeeds");
    Require(!writer.Finalize(), "empty session Finalize reports failure");
    std::filesystem::path partial = path;
    partial += ".partial";
    Require(!std::filesystem::exists(partial), "empty session removes partial file");
    Require(!std::filesystem::exists(path), "empty session writes no final file");
}

void VerifyAbortLeavesNoFile() {
    const auto path = TempPath("session_abort");
    Dvr::SessionWriter writer;
    Require(writer.Begin(path), "abort-case Begin succeeds");
    Require(writer.Append(MakeFrameSlot()), "abort-case Append succeeds");
    writer.Abort();
    Require(!writer.IsActive(), "session is inactive after Abort");
    std::filesystem::path partial = path;
    partial += ".partial";
    Require(!std::filesystem::exists(partial), "Abort removes partial file");
    Require(!std::filesystem::exists(path), "Abort writes no final file");
}

} // namespace

int main() {
    try {
        VerifyByteEquivalence(false);
        VerifyByteEquivalence(true);
        VerifyEmptySessionLeavesNoFile();
        VerifyAbortLeavesNoFile();
    } catch (const std::exception& e) {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
