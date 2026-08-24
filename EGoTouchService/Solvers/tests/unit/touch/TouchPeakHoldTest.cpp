#include "TouchSolver/PeakDetector.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Solvers::HeatmapFrame MakeZeroedHeatmapFrame() {
    Solvers::HeatmapFrame frame{};
    std::fill(&frame.touch.conditioned[0][0], &frame.touch.conditioned[0][0] + 40 * 60, int16_t{0});
    return frame;
}

Solvers::MacroZone MakeZoneFromFrame(const Solvers::HeatmapFrame& frame,
                                     std::vector<int>& pixels,
                                     int threshold) {
    Solvers::MacroZone zone;
    zone.minR = 39;
    zone.maxR = 0;
    zone.minC = 59;
    zone.maxC = 0;
    for (int r = 0; r < 40; ++r) {
        for (int c = 0; c < 60; ++c) {
            const int16_t sig = frame.touch.conditioned[r][c];
            if (sig < threshold) continue;
            pixels.push_back(r * 60 + c);
            zone.areaCells += 1;
            zone.signalSum += sig;
            zone.minR = std::min(zone.minR, r);
            zone.maxR = std::max(zone.maxR, r);
            zone.minC = std::min(zone.minC, c);
            zone.maxC = std::max(zone.maxC, c);
        }
    }
    zone.pixels = std::span<const int>(pixels.data(), pixels.size());
    return zone;
}

// 手指滑向下边缘：超阈的格子先是三个，再缩到两个。位置取自 164645 语料里
// 真实断触的那一处（面板左下角 (39,0) 一带）。
void BuildEdgeFrame(Solvers::HeatmapFrame& frame, int aboveThresholdCells) {
    frame.touch.conditioned[39][0] = 1800;
    frame.touch.conditioned[39][1] = aboveThresholdCells >= 2 ? 340 : 120;
    frame.touch.conditioned[38][0] = aboveThresholdCells >= 3 ? 320 : 100;
}

std::span<const Solvers::Touch::Peak> DetectOnce(Solvers::Touch::PeakDetector& detector,
                                                 const Solvers::HeatmapFrame& frame,
                                                 std::span<const Solvers::Touch::TrackAnchor> anchors) {
    std::vector<int> pixels;
    std::vector<Solvers::MacroZone> zones;
    zones.push_back(MakeZoneFromFrame(frame, pixels, detector.m_threshold));
    detector.Detect(frame, zones, anchors);
    return detector.GetPeaks();
}

// 缩到两格时，没有轨迹压着的峰按起始判据落选 —— 这是「起始严」的一半，
// 也是反向验证的锚点：若维持判据被误用在没有轨迹的地方，这条会红。
void TestShrunkEdgeZoneIsRejectedWithoutTrack() {
    Solvers::Touch::PeakDetector detector;
    auto frame = MakeZeroedHeatmapFrame();
    BuildEdgeFrame(frame, 2);

    const auto peaks = DetectOnce(detector, frame, {});
    Require(peaks.empty(),
            "a two-cell edge zone with no track on it should not produce a peak");
}

// 同一帧，上一帧的轨迹就压在这个峰上：改用维持判据，峰必须留下来。
// 这一格丢掉，轨迹就进 SilentGap 不上报，OS 看到触点消失后重连成新的 down。
void TestShrunkEdgeZoneSurvivesUnderTrack() {
    Solvers::Touch::PeakDetector detector;
    auto frame = MakeZeroedHeatmapFrame();
    BuildEdgeFrame(frame, 2);

    const Solvers::Touch::TrackAnchor anchor{0.4f, 38.9f};  // x=列, y=行
    const auto peaks = DetectOnce(detector, frame, std::span{&anchor, 1});
    Require(peaks.size() == 1, "a tracked edge peak should survive the shrunk zone");
    Require(peaks[0].r == 39 && peaks[0].c == 0, "the surviving peak should be the tracked cell");
}

// 维持判据只对轨迹附近放宽。远处同样缩水的区域不该沾光，否则边缘噪声会被
// 一路留成接触点。
void TestHoldAreaDoesNotReachDistantZones() {
    Solvers::Touch::PeakDetector detector;
    auto frame = MakeZeroedHeatmapFrame();
    BuildEdgeFrame(frame, 2);

    const Solvers::Touch::TrackAnchor faraway{40.0f, 10.0f};
    const auto peaks = DetectOnce(detector, frame, std::span{&faraway, 1});
    Require(peaks.empty(), "a track elsewhere on the panel should not hold this zone");
}

// 未缩水的三格区域两种情况都该出峰，确认上面两条测的是维持判据而不是别的门。
void TestFullSizeEdgeZoneAlwaysProducesPeak() {
    Solvers::Touch::PeakDetector detector;
    auto frame = MakeZeroedHeatmapFrame();
    BuildEdgeFrame(frame, 3);

    Require(!DetectOnce(detector, frame, {}).empty(),
            "a three-cell edge zone should pass the entry areaCells on its own");
}

} // namespace

int main() {
    try {
        TestShrunkEdgeZoneIsRejectedWithoutTrack();
        TestShrunkEdgeZoneSurvivesUnderTrack();
        TestHoldAreaDoesNotReachDistantZones();
        TestFullSizeEdgeZoneAlwaysProducesPeak();
        std::cout << "[TEST] Touch peak hold tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
