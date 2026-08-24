#pragma once

#include "DvrFormat.h"
#include "DvrFrameSlot.h"
#include "DvrTypes.h"
#include "SolverTypes.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Dvr {

std::filesystem::path ResolveReplayBinaryPath(const std::filesystem::path& input);

/// 连续会话录制:逐帧追加定长记录到 <final>.partial,Finalize 时组装成
/// 标准 DVR2 .dvrbin。与环形缓冲 + 触发导出互补,用于录制分钟级语料而不
/// 占用等量内存(每帧约 17 KB,只有 index 项留在内存)。
class SessionWriter {
public:
    ~SessionWriter();
    bool Begin(const std::filesystem::path& finalPath);
    bool Append(const DvrFrameSlot& frame);
    bool Finalize(const RuntimeConfigSnapshot* runtimeConfig = nullptr);
    void Abort();

    bool IsActive() const { return m_active; }
    size_t FrameCount() const { return m_index.size(); }
    const std::filesystem::path& FinalPath() const { return m_finalPath; }

private:
    bool m_active = false;
    std::filesystem::path m_finalPath;
    std::filesystem::path m_partialPath;
    std::ofstream m_stream;
    std::vector<Format::Dvr2IndexEntry> m_index;
    uint32_t m_flags = 0;
};

bool WriteBinaryFile(const std::filesystem::path& filePath,
                     const std::vector<DvrFrameSlot>& frames,
                     const DynamicDebugSchema* dynamicSchema = nullptr,
                     const std::vector<DvrDynamicDebugFrameSlot>* dynamicFrames = nullptr,
                     const RuntimeConfigSnapshot* runtimeConfig = nullptr,
                     uint32_t* outFlags = nullptr);

bool ReadBinaryFile(const std::filesystem::path& filePath,
                    std::vector<Solvers::HeatmapFrame>& outFrames,
                    int& outVersion,
                    uint32_t* outFlags = nullptr,
                    std::string* outError = nullptr,
                    DynamicDebugSchema* outDynamicSchema = nullptr,
                    std::vector<DynamicDebugFrame>* outDynamicFrames = nullptr,
                    RuntimeConfigSnapshot* outRuntimeConfig = nullptr);

} // namespace Dvr

namespace App {

inline std::filesystem::path ResolveReplayBinaryPath(const std::filesystem::path& input) {
    return Dvr::ResolveReplayBinaryPath(input);
}

inline bool WriteDvrBinaryFile(const std::filesystem::path& filePath,
                               const std::vector<Dvr::DvrFrameSlot>& frames,
                               const DvrDynamicDebugSchema* dynamicSchema = nullptr,
                               const std::vector<Dvr::DvrDynamicDebugFrameSlot>* dynamicFrames = nullptr,
                               const DvrRuntimeConfigSnapshot* runtimeConfig = nullptr,
                               uint32_t* outFlags = nullptr) {
    return Dvr::WriteBinaryFile(filePath, frames, dynamicSchema, dynamicFrames, runtimeConfig, outFlags);
}

inline bool ReadDvrBinaryFile(const std::filesystem::path& filePath,
                              std::vector<Solvers::HeatmapFrame>& outFrames,
                              int& outVersion,
                              uint32_t* outFlags = nullptr,
                              std::string* outError = nullptr,
                              DvrDynamicDebugSchema* outDynamicSchema = nullptr,
                              std::vector<DvrDynamicDebugFrame>* outDynamicFrames = nullptr,
                              DvrRuntimeConfigSnapshot* outRuntimeConfig = nullptr) {
    return Dvr::ReadBinaryFile(filePath, outFrames, outVersion, outFlags, outError, outDynamicSchema, outDynamicFrames, outRuntimeConfig);
}

} // namespace App
