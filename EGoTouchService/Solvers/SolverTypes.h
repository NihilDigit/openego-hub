#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "SolverBuildConfig.h"
#include "FrameLayout.h"
#include "TouchFrameTypes.h"
#include "StylusFrameTypes.h"

namespace Solvers {

struct HeatmapFrame {
#if EGOTOUCH_DIAG
    std::vector<uint8_t> rawData;
#endif
    const uint8_t* rawPtr = nullptr;
    size_t rawLen = 0;

    Frame::MasterSuffixView masterSuffix{};
    Frame::SlaveSuffixView slaveSuffix{};
    bool masterSuffixValid = false;
    bool slaveSuffixValid = false;

    int16_t heatmapMatrix[40][60];

    TouchFrameData touch;
    StylusFrameData stylus;

    // 采集时刻。原先由 master 后缀 word 9 填充，而该字是频点码不是时间戳，
    // 赋值链已删除；在时间基一步接上 QPC 采集戳之前这里恒为 0，
    // 下游（1-Euro 的 dt、回放节拍）因此走各自的兜底分支。
    uint64_t timestamp;
    uint64_t receiveSystemEpochUs = 0;
    bool masterWasRead = true;

    HeatmapFrame() : timestamp(0) {}
};

} // namespace Solvers
