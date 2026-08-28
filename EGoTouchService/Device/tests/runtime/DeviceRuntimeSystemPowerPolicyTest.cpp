#include "runtime/DeviceRuntime.h"

#include <iostream>

namespace {

// 采集与注入交给 GaokunThpHost 之后，DeviceRuntime 不再有工作线程、芯片和 AFE 命令队列，
// 原先围绕状态机的用例连同被测代码一起去掉了。电源事件现在只剩去抖和日志，没有可断言的
// 可观察行为，也一并去掉。留下的是笔的 AFE 重放代次，它是纯状态逻辑。

bool RunPenReplayCoversResumeAndRecoverInitCyclesTest() {
    PenAfeReplayState replay{};

    replay.BeginInitCycle(); // initial start
    const uint64_t initialGeneration = replay.generation;
    if (!replay.pending || replay.IsCurrent(initialGeneration)) {
        std::cerr << "[TEST] Initial init cycle did not suppress in-flight pen commands.\n";
        return false;
    }
    replay.CompleteInitCycle();
    if (!replay.IsCurrent(initialGeneration)) {
        std::cerr << "[TEST] Initial replay generation did not become current.\n";
        return false;
    }

    replay.BeginInitCycle(); // suspend/resume reinitialization
    const uint64_t resumeGeneration = replay.generation;
    if (resumeGeneration == initialGeneration ||
        replay.IsCurrent(initialGeneration) || replay.IsCurrent(resumeGeneration)) {
        std::cerr << "[TEST] Resume init cycle did not invalidate pre-reset commands.\n";
        return false;
    }
    replay.CompleteInitCycle();
    if (!replay.IsCurrent(resumeGeneration)) {
        std::cerr << "[TEST] Resume replay generation did not become current.\n";
        return false;
    }

    replay.BeginInitCycle(); // recover reinitialization
    const uint64_t recoverGeneration = replay.generation;
    if (recoverGeneration == resumeGeneration || replay.IsCurrent(resumeGeneration)) {
        std::cerr << "[TEST] Recover init cycle did not invalidate pre-reset commands.\n";
        return false;
    }
    replay.CompleteInitCycle();
    if (!replay.IsCurrent(recoverGeneration)) {
        std::cerr << "[TEST] Recover replay generation did not become current.\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!RunPenReplayCoversResumeAndRecoverInitCyclesTest()) {
        return 1;
    }

    std::cout << "[TEST] DeviceRuntime system power policy tests passed.\n";
    return 0;
}
