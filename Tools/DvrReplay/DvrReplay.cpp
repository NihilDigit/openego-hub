// DvrReplay — 离线重放 .dvrbin 录制:把录制帧的原始 SPI 数据重新喂进
// TouchPipeline / StylusPipeline,输出逐帧结果 CSV,供调参对比与重构回归。
//
// 用法:
//   DvrReplay --dataset <path.dvrbin> [--out <path.csv>]
//             [--set touch.gesture.drag_threshold=0.8]... [--config <overrides.txt>]
//   DvrReplay --diff <a.csv> <b.csv>
//
// 只在 EGOTOUCH_DIAG 构建(Debug)下可用:.dvrbin 读取端只在 DIAG 下把 rawData
// 接到 HeatmapFrame(DvrBinaryReader.cpp 的 rawPtr 接线),且 DIAG 改变
// HeatmapFrame 布局,工具必须与 Solvers 库同配置编译。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "DvrBinaryIO.h"
#include "FrameLayout.h"
#include "SolverTypes.h"
#include "TouchSolver/TouchPipeline.h"
#include "StylusPipeline.h"
#include "config/ConfigBinder.h"
#include "config/ConfigStore.h"

namespace {

int RunDiff(const std::filesystem::path& pathA, const std::filesystem::path& pathB) {
    std::ifstream fa(pathA), fb(pathB);
    if (!fa) { std::fprintf(stderr, "cannot open %s\n", pathA.string().c_str()); return 2; }
    if (!fb) { std::fprintf(stderr, "cannot open %s\n", pathB.string().c_str()); return 2; }

    std::string la, lb;
    size_t lineNo = 0, mismatches = 0;
    constexpr size_t kMaxShown = 10;
    for (;;) {
        const bool oka = static_cast<bool>(std::getline(fa, la));
        const bool okb = static_cast<bool>(std::getline(fb, lb));
        if (!oka && !okb) break;
        ++lineNo;
        if (oka && okb && la == lb) continue;
        ++mismatches;
        if (mismatches <= kMaxShown) {
            std::printf("line %zu:\n  A: %s\n  B: %s\n",
                        lineNo,
                        oka ? la.c_str() : "<eof>",
                        okb ? lb.c_str() : "<eof>");
        }
    }
    if (mismatches == 0) {
        std::printf("identical (%zu lines)\n", lineNo);
        return 0;
    }
    std::printf("%zu differing line(s), first %zu shown\n",
                mismatches, std::min(mismatches, kMaxShown));
    return 1;
}

} // namespace

#if EGOTOUCH_DIAG

namespace {

bool ApplyOverride(Config::ConfigStore& store, std::string_view spec) {
    const size_t eq = spec.find('=');
    if (eq == std::string_view::npos) {
        std::fprintf(stderr, "override missing '=': %.*s\n",
                     static_cast<int>(spec.size()), spec.data());
        return false;
    }
    const std::string path(spec.substr(0, eq));
    const std::string value(spec.substr(eq + 1));
    if (!store.has(path)) {
        std::fprintf(stderr, "unknown config key: %s\n", path.c_str());
        return false;
    }
    // 覆盖值的类型跟随 binder 默认值:applyConfig 用类型化 getOr 取值,
    // 类型不符会静默落回默认,所以这里必须按既有类型解析。
    const auto current = store.getOr<Config::ConfigValue>(path, Config::ConfigValue{});
    try {
        if (std::holds_alternative<bool>(current)) {
            if (value == "true" || value == "1") store.set(path, true);
            else if (value == "false" || value == "0") store.set(path, false);
            else throw std::invalid_argument(value);
        } else if (std::holds_alternative<int32_t>(current)) {
            store.set(path, static_cast<int32_t>(std::stol(value)));
        } else if (std::holds_alternative<float>(current)) {
            store.set(path, std::stof(value));
        } else {
            store.set(path, value);
        }
    } catch (const std::exception&) {
        std::fprintf(stderr, "cannot parse value for %s: %s\n", path.c_str(), value.c_str());
        return false;
    }
    return true;
}

bool LoadOverridesFile(Config::ConfigStore& store, const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open overrides file: %s\n", path.string().c_str());
        return false;
    }
    std::string line;
    size_t lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        const size_t begin = line.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos || line[begin] == '#') continue;
        const size_t end = line.find_last_not_of(" \t\r\n");
        if (!ApplyOverride(store, std::string_view(line).substr(begin, end - begin + 1))) {
            std::fprintf(stderr, "  at %s:%zu\n", path.string().c_str(), lineNo);
            return false;
        }
    }
    return true;
}

// 旧录制(黑匣子导出)可能不带 rawData,但 heatmapMatrix 与两段 suffix words 是
// 必录字段,而它们正是 MasterFrameParser 从 raw 中解出的全部内容(header 7 字节
// 无人消费,MasterFrameParser.hpp 从 rawPtr+kHeaderBytes 起读)。据此可以无损
// 合成一帧 raw;slave header 里被读取的 status/checksum16 用录制值回填,其余
// 3 字节置零(仅被 memcpy 进 rawSlaveHdr 存档,无解码方)。
std::vector<uint8_t> BuildSyntheticRaw(const Solvers::HeatmapFrame& src) {
    std::vector<uint8_t> raw;
    const bool withSlave = src.slaveSuffixValid;
    raw.assign(withSlave ? Frame::kTotalFrameSize : Frame::kMasterFrameSize, 0);

    uint8_t* matrix = raw.data() + Frame::kMatrixOffset;
    for (int i = 0; i < Frame::kMatrixCells; ++i) {
        const auto v = static_cast<uint16_t>(
            reinterpret_cast<const int16_t*>(src.heatmapMatrix)[i]);
        matrix[i * 2] = static_cast<uint8_t>(v & 0xFF);
        matrix[i * 2 + 1] = static_cast<uint8_t>(v >> 8);
    }
    uint8_t* master = raw.data() + Frame::kMasterSuffixOffset;
    for (int i = 0; i < Frame::kMasterSuffixWords; ++i) {
        master[i * 2] = static_cast<uint8_t>(src.masterSuffix.words[i] & 0xFF);
        master[i * 2 + 1] = static_cast<uint8_t>(src.masterSuffix.words[i] >> 8);
    }
    if (withSlave) {
        uint8_t* slaveHdr = raw.data() + Frame::kSlaveHeaderOffset;
        const uint16_t status = static_cast<uint16_t>(src.stylus.input.status);
        const uint16_t checksum = src.stylus.input.checksum16;
        slaveHdr[0] = static_cast<uint8_t>(status & 0xFF);
        slaveHdr[1] = static_cast<uint8_t>(status >> 8);
        slaveHdr[2] = static_cast<uint8_t>(checksum & 0xFF);
        slaveHdr[3] = static_cast<uint8_t>(checksum >> 8);
        uint8_t* slave = raw.data() + Frame::kSlaveSuffixOffset;
        for (int i = 0; i < Frame::kSlaveSuffixWords; ++i) {
            slave[i * 2] = static_cast<uint8_t>(src.slaveSuffix.words[i] & 0xFF);
            slave[i * 2 + 1] = static_cast<uint8_t>(src.slaveSuffix.words[i] >> 8);
        }
    }
    return raw;
}

// 字段追加只能加在末尾：scripts/ 下的分析脚本按位置取,插在中间会静默错位。
void WriteContactCell(std::FILE* out, const Solvers::TouchContact& c) {
    std::fprintf(out, "%d:%d:%d:%d:%.3f:%.3f:%d:%d:%.3f:%d:%d:%d:%.3f:%.3f:%d:%d",
                 c.id, c.state, c.reportEvent, c.isReported ? 1 : 0,
                 c.x, c.y, c.areaCells, c.signalSum, c.sizeMm, c.peakSignal,
                 c.strokeId, static_cast<int>(c.strokePhase),
                 c.matchXCells, c.matchYCells,
                 static_cast<int>(c.gripRatioXQ4), static_cast<int>(c.gripRatioYQ4));
}

// OS 看到的是「上报集合里有没有这个 id」，不是 reportEvent 的取值：一条笔画起手会
// 连着好几帧都报 Down（手势层停在 PressCandidate），按行数去数 Down 会把一次落指数成
// 五六次。这里改成数 id 进入上报集合的次数，并区分首次出现与断开后重连——重连才是
// 边缘掉触真正的症状。
struct ContactAppearanceStats {
    std::set<int> reported;      // 上一帧的上报集合
    std::set<int> everSeen;
    size_t fresh = 0;            // 首次出现，等于真实笔画数
    size_t reconnectMove = 0;    // 笔画中途断开后又接上
    size_t reconnectUp = 0;      // 静默若干帧之后才补发的抬起
    size_t liftOffs = 0;
};

void AccumulateAppearances(const Solvers::HeatmapFrame& f, ContactAppearanceStats& s) {
    std::set<int> cur;
    for (const auto& c : f.touch.output.contacts) {
        if (!c.isReported) continue;
        cur.insert(c.id);
        if (c.reportEvent == Solvers::TouchReportUp) ++s.liftOffs;
        if (s.reported.count(c.id) != 0) continue;
        if (s.everSeen.insert(c.id).second) {
            ++s.fresh;
        } else if (c.reportEvent == Solvers::TouchReportUp) {
            ++s.reconnectUp;
        } else {
            ++s.reconnectMove;
        }
    }
    s.reported = std::move(cur);
}

void WriteFrameRow(std::FILE* out, size_t index, const Solvers::HeatmapFrame& f,
                   size_t* framesWithReported) {
    const auto& so = f.stylus.output;
    // 倾角与原始坐标中间量:重放模式下由管线实时填充;dump-recorded 模式下
    // 录制里没有这组运行态,恒为零。rawCoor 是 CoorSpeed 同款的未滤波参考
    // (tx1 全局坐标,进入 LinearFilter/CoorIIR 之前),用于离线量化滤波链滞后。
    const auto& tilt = f.stylus.runtime.Active().tilt;
    const auto& rawCoor = f.stylus.runtime.Active().tx1.coordinate.reportGlobalCoor;
    // 固件自己的手指判据。它一旦翻假，BaselineTracker 会把整张热力图清零，
    // 下游连宏区都建不出来——边缘掉触的入口在这里，不在峰检测的阈值上。
    const bool fwHasFinger = f.masterSuffixValid && f.masterSuffix.hasFinger();
    // 调理后热力图的峰值。峰列表为空时它区分两种原因：信号被基线吃掉（hmMax 落到阈值以下），
    // 还是信号还在而检测级漏掉了。
    int hmMax = 0, hmMaxR = -1, hmMaxC = -1;
    for (int r = 0; r < Frame::kTxCount; ++r) {
        for (int c = 0; c < Frame::kRxCount; ++c) {
            const int v = f.touch.conditioned[r][c];
            if (v > hmMax) { hmMax = v; hmMaxR = r; hmMaxC = c; }
        }
    }
    // 峰值格所属宏区的面积。宏区是峰检测的候选来源，面积不足会整块落选，
    // 所以「hmMax 很高但 peaks 为空」时先看这一列。
    int zoneCount = 0, maxCellZoneArea = 0;
    if (f.touch.runtime.macroZones) {
        zoneCount = static_cast<int>(f.touch.runtime.macroZones->size());
        const int maxIdx = hmMaxR * Frame::kRxCount + hmMaxC;
        for (const auto& z : *f.touch.runtime.macroZones) {
            for (int px : z.pixels) {
                if (px == maxIdx) { maxCellZoneArea = z.areaCells; break; }
            }
            if (maxCellZoneArea != 0) break;
        }
    }
    std::fprintf(out, "%zu,%llu,%llu,%d,%d,%d,%.3f,%.3f,%u,%d,%d,%d,%d,%u,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%zu,",
                 index,
                 static_cast<unsigned long long>(f.timestamp),
                 static_cast<unsigned long long>(f.receiveSystemEpochUs),
                 so.valid ? 1 : 0, so.inRange ? 1 : 0, so.tipDown ? 1 : 0,
                 so.point.x, so.point.y, static_cast<unsigned>(so.pressure),
                 static_cast<int>(so.point.tiltX), static_cast<int>(so.point.tiltY),
                 static_cast<int>(tilt.diffDim1), static_cast<int>(tilt.diffDim2),
                 static_cast<unsigned>(tilt.lenLimit),
                 static_cast<unsigned>(tilt.signalRatio),
                 static_cast<int>(tilt.preTiltDim1), static_cast<int>(tilt.preTiltDim2),
#if EGOTOUCH_DIAG
                 static_cast<int>(tilt.rawDiffDim1), static_cast<int>(tilt.rawDiffDim2),
#else
                 0, 0,
#endif
                 rawCoor.valid ? 1 : 0,
                 static_cast<int>(rawCoor.dim1), static_cast<int>(rawCoor.dim2),
                 f.masterWasRead ? 1 : 0, f.masterSuffixValid ? 1 : 0, fwHasFinger ? 1 : 0,
                 hmMax, hmMaxR, hmMaxC, zoneCount, maxCellZoneArea,
                 f.touch.output.contacts.size());
    // 峰值格的 8 邻域，行优先。局部极大判据对上方与左侧要求严格大于、对下方与
    // 右侧允许相等，边缘格的邻域缺项，看原值最省事。
    {
        std::string nbr;
        for (int dr = -1; dr <= 1; ++dr) {
            for (int dc = -1; dc <= 1; ++dc) {
                if (dr == 0 && dc == 0) continue;
                const int nr = hmMaxR + dr, nc = hmMaxC + dc;
                if (!nbr.empty()) nbr += ':';
                nbr += (nr < 0 || nr >= Frame::kTxCount || nc < 0 || nc >= Frame::kRxCount)
                           ? "x"
                           : std::to_string(f.touch.conditioned[nr][nc]);
            }
        }
        std::fprintf(out, "%s,", nbr.c_str());
    }
#if EGOTOUCH_DIAG
    // 峰列表(r:c:z:id)在接触列表之前,便于定位「轨迹扑动帧里峰还在不在」一类问题
    {
        bool first = true;
        // 峰后面跟着它的评估结果:掌判在哪一级把它拦下来的,只看接触点是看不出来的。
        int pi = 0;
        for (const auto& p : f.touch.debug.peaks) {
            if (!first) std::fputc(';', out);
            first = false;
            const auto& evals = f.touch.runtime.peakEvaluations;
            const bool hasEval = pi < static_cast<int>(evals.size());
            std::fprintf(out, "%d:%d:%d:%u:%d:%d:%u:%.2f", p.r, p.c, static_cast<int>(p.z),
                         static_cast<unsigned>(p.id),
                         hasEval ? static_cast<int>(evals[static_cast<size_t>(pi)].palmClass) : -1,
                         hasEval ? (evals[static_cast<size_t>(pi)].allowContact ? 1 : 0) : -1,
                         hasEval ? evals[static_cast<size_t>(pi)].evalFlags : 0u,
                         hasEval ? evals[static_cast<size_t>(pi)].sharpness : 0.0f);
            ++pi;
        }
    }
#endif
    std::fputc(',', out);
    bool anyReported = false;
    bool first = true;
    for (const auto& c : f.touch.output.contacts) {
        if (!first) std::fputc(';', out);
        first = false;
        WriteContactCell(out, c);
        if (c.isReported) anyReported = true;
    }
    std::fputc('\n', out);
    if (anyReported && framesWithReported) ++*framesWithReported;
}

constexpr const char* kCsvHeader =
    "frame,timestamp,epochUs,stylusValid,stylusInRange,stylusTipDown,"
    "stylusX,stylusY,stylusPressure,stylusTiltX,stylusTiltY,"
    "tiltDiff1,tiltDiff2,tiltLenLimit,tiltSignalRatio,preTilt1,preTilt2,"
    "rawDiff1,rawDiff2,rawCoorValid,rawCoor1,rawCoor2,"
    "masterWasRead,masterSuffixValid,fwHasFinger,hmMax,hmMaxR,hmMaxC,"
    "zoneCount,maxCellZoneArea,contactCount,hmNbr,peaks,contacts\n";

int RunDumpRecorded(const std::filesystem::path& datasetPath,
                    const std::filesystem::path& outPath) {
    std::vector<Solvers::HeatmapFrame> frames;
    int version = 0;
    std::string error;
    if (!Dvr::ReadBinaryFile(Dvr::ResolveReplayBinaryPath(datasetPath),
                             frames, version, nullptr, &error)) {
        std::fprintf(stderr, "failed to read dataset: %s\n",
                     error.empty() ? "unknown error" : error.c_str());
        return 2;
    }
    std::FILE* out = _wfopen(outPath.wstring().c_str(), L"wb");
    if (!out) {
        std::fprintf(stderr, "cannot open output: %s\n", outPath.string().c_str());
        return 2;
    }
    std::fprintf(out, "%s", kCsvHeader);
    for (size_t i = 0; i < frames.size(); ++i) {
        WriteFrameRow(out, i, frames[i], nullptr);
    }
    std::fclose(out);
    std::printf("dumped %zu recorded frames -> %s\n", frames.size(), outPath.string().c_str());
    return 0;
}

// TSACore 对拍宿主是 x64 进程,让它再实现一遍 DVR 容器格式不划算。这里把矩阵摊平
// 成一个自描述的定长文件,两边只需就这一个 32 字节头达成一致。
// 取的是设备原始帧里的矩阵,不是 frames[i].heatmapMatrix——后者在录制链路上已被
// 管线就地改写过(基线扣除与 CMF),而 TSACore 要的是未经调理的 raw。
int RunDumpFrames(const std::filesystem::path& datasetPath,
                  const std::filesystem::path& outPath) {
    std::vector<Solvers::HeatmapFrame> frames;
    int version = 0;
    std::string error;
    if (!Dvr::ReadBinaryFile(Dvr::ResolveReplayBinaryPath(datasetPath),
                             frames, version, nullptr, &error)) {
        std::fprintf(stderr, "failed to read dataset: %s\n",
                     error.empty() ? "unknown error" : error.c_str());
        return 2;
    }
    size_t usable = 0;
    for (const auto& f : frames) {
        if (f.rawLen >= static_cast<size_t>(Frame::kMasterFrameSize)) ++usable;
    }
    if (usable == 0) {
        std::fprintf(stderr,
                     "this dataset carries no device raw frames; the recorded matrix is "
                     "post-conditioning and cannot drive TSACore\n");
        return 2;
    }

    std::FILE* out = _wfopen(outPath.wstring().c_str(), L"wb");
    if (!out) {
        std::fprintf(stderr, "cannot open output: %s\n", outPath.string().c_str());
        return 2;
    }
    struct Header {
        char     magic[8];      // "EGOFRM01"
        uint32_t frameCount;
        uint16_t rows;          // Tx
        uint16_t cols;          // Rx
        uint32_t cellBytes;     // 2
        uint32_t reserved[3];
    } hdr{};
    std::memcpy(hdr.magic, "EGOFRM01", 8);
    hdr.frameCount = static_cast<uint32_t>(usable);
    hdr.rows       = static_cast<uint16_t>(Frame::kTxCount);
    hdr.cols       = static_cast<uint16_t>(Frame::kRxCount);
    hdr.cellBytes  = 2;
    std::fwrite(&hdr, sizeof hdr, 1, out);

    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& f = frames[i];
        if (f.rawLen < static_cast<size_t>(Frame::kMasterFrameSize)) continue;
        const uint32_t index = static_cast<uint32_t>(i);
        std::fwrite(&index, sizeof index, 1, out);
        std::fwrite(f.rawData.data() + Frame::kMatrixOffset, 1, Frame::kMatrixBytes, out);
    }
    std::fclose(out);
    std::printf("dumped %zu/%zu raw frames (%dx%d) -> %s\n",
                usable, frames.size(), Frame::kTxCount, Frame::kRxCount,
                outPath.string().c_str());
    return 0;
}

int RunReplay(const std::filesystem::path& datasetPath,
              const std::filesystem::path& outPath,
              const std::vector<std::string>& overrideSpecs,
              const std::filesystem::path& overridesFile) {
    std::vector<Solvers::HeatmapFrame> frames;
    int version = 0;
    uint32_t flags = 0;
    std::string error;
    std::fprintf(stderr, "[stage] reading dataset\n");
    if (!Dvr::ReadBinaryFile(Dvr::ResolveReplayBinaryPath(datasetPath),
                             frames, version, &flags, &error)) {
        std::fprintf(stderr, "failed to read dataset: %s\n",
                     error.empty() ? "unknown error" : error.c_str());
        return 2;
    }
    std::printf("dataset: %s (format v%d, %zu frames)\n",
                datasetPath.string().c_str(), version, frames.size());
    if (frames.empty()) return 2;

    size_t framesWithRaw = 0;
    size_t framesWithBt = 0;
    uint16_t maxBtPressure = 0;
    for (const auto& f : frames) {
        if (f.rawLen >= static_cast<size_t>(Frame::kMasterFrameSize)) ++framesWithRaw;
        const auto& bt = f.stylus.input.btSample;
        if (bt.hasSample) ++framesWithBt;
        maxBtPressure = std::max(maxBtPressure, bt.pressure[3]);
    }
    std::printf("  frames with BT sample: %zu, max BT pressure: %u\n",
                framesWithBt, static_cast<unsigned>(maxBtPressure));
    const bool legacyDataset = framesWithRaw == 0;
    if (framesWithRaw < frames.size()) {
        std::printf("  %zu/%zu frame(s) lack raw data; synthesizing from decoded "
                    "heatmap + suffix words\n",
                    frames.size() - framesWithRaw, frames.size());
    }

    Solvers::TouchPipeline touchPipeline;
    Solvers::StylusPipeline stylusPipeline;

    Config::ConfigBinder binder;
    touchPipeline.registerBindings(binder);
    stylusPipeline.registerBindings(binder);
    Config::ConfigStore store;
    binder.writeDefaults(store);
    if (!overridesFile.empty() && !LoadOverridesFile(store, overridesFile)) return 2;
    for (const auto& spec : overrideSpecs) {
        if (!ApplyOverride(store, spec)) return 2;
    }
    touchPipeline.applyConfig(store);
    stylusPipeline.applyConfig(store);

    if (legacyDataset) {
        // 无 rawData 的录制里,heatmapMatrix 是管线跑完后拷出的,已经过基线扣除与
        // CMF(BaselineTracker.hpp:82 注明 in-place)。再调理一遍会把信号扣没,
        // 所以直接旁路这两级,把录制矩阵当作调理后的输入。
        touchPipeline.m_baseline.m_enabled = false;
        touchPipeline.m_cmf.m_enabled = false;
        std::printf("  legacy dataset: heatmap is post-conditioning; "
                    "baseline/CMF stages bypassed\n");
    }

    std::FILE* out = _wfopen(outPath.wstring().c_str(), L"wb");
    if (!out) {
        std::fprintf(stderr, "cannot open output: %s\n", outPath.string().c_str());
        return 2;
    }
    std::fprintf(out, "%s", kCsvHeader);

    // 录制的 btSeq 只在新蓝牙包到达的帧递增;只在 seq 变化的帧注入,
    // 让管线内部的按 seq 去重逻辑看到与在线一致的节奏。
    uint32_t lastBtSeq = 0;
    bool haveBtSeq = false;

    size_t shortFrames = 0;
    size_t framesWithReported = 0;
    ContactAppearanceStats appearances;
    size_t stylusTipFrames = 0;

    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& src = frames[i];

        Solvers::HeatmapFrame in;
        if (src.rawLen >= static_cast<size_t>(Frame::kMasterFrameSize)) {
            in.rawData = src.rawData;
        } else {
            in.rawData = BuildSyntheticRaw(src);
            ++shortFrames;
        }
        in.rawPtr = in.rawData.data();
        in.rawLen = in.rawData.size();
        in.timestamp = src.timestamp;
        in.receiveSystemEpochUs = src.receiveSystemEpochUs;
        in.masterWasRead = src.masterWasRead;

        const auto& bt = src.stylus.input.btSample;
        if (bt.hasSample && (!haveBtSeq || bt.seq != lastBtSeq)) {
            if (bt.hasFreq) {
                stylusPipeline.SetBtMcuPressurePacket(bt.pressure, bt.rawPressure,
                                                      bt.freq1, bt.freq2);
            } else {
                stylusPipeline.SetBtMcuPressure(bt.pressure[3]);
            }
            lastBtSeq = bt.seq;
            haveBtSeq = true;
        }

        // 与 DeviceRuntime 的在线顺序一致:先笔后触摸。
        if ((i % 64) == 0) std::fprintf(stderr, "\rframe %zu/%zu", i, frames.size());
        stylusPipeline.Process(in);
        touchPipeline.Process(in);

        if (in.stylus.output.tipDown) ++stylusTipFrames;
        AccumulateAppearances(in, appearances);
        WriteFrameRow(out, i, in, &framesWithReported);
    }
    std::fclose(out);

    std::printf("replayed %zu frames -> %s\n", frames.size(), outPath.string().c_str());
    if (shortFrames != 0) {
        std::printf("  %zu frame(s) replayed from synthesized raw\n", shortFrames);
    }
    std::printf("  frames with reported contacts: %zu\n", framesWithReported);
    std::printf("  contact appearances: %zu fresh, %zu mid-stroke reconnects, "
                "%zu delayed lift-offs\n",
                appearances.fresh, appearances.reconnectMove, appearances.reconnectUp);
    std::printf("  lift-off events: %zu\n", appearances.liftOffs);
    std::printf("  stylus tip-down frames: %zu\n", stylusTipFrames);
    return 0;
}

} // namespace

#endif // EGOTOUCH_DIAG

int main(int argc, char** argv) {
    // 诊断工具,输出量小:无缓冲让崩溃前的进度不丢。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::filesystem::path dataset, out, overridesFile, diffA, diffB;
    std::vector<std::string> overrideSpecs;
    bool diffMode = false;
    bool dumpRecorded = false;
    bool dumpFrames = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires an argument\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--dataset") dataset = next("--dataset");
        else if (arg == "--out") out = next("--out");
        else if (arg == "--dump-recorded") dumpRecorded = true;
        else if (arg == "--dump-frames") dumpFrames = true;
        else if (arg == "--set") overrideSpecs.emplace_back(next("--set"));
        else if (arg == "--config") overridesFile = next("--config");
        else if (arg == "--diff") {
            diffMode = true;
            diffA = next("--diff");
            diffB = next("--diff");
        } else {
            std::fprintf(stderr, "unknown argument: %.*s\n",
                         static_cast<int>(arg.size()), arg.data());
            return 2;
        }
    }

    if (diffMode) return RunDiff(diffA, diffB);

    if (dataset.empty()) {
        std::fprintf(stderr,
                     "usage: DvrReplay --dataset <path.dvrbin> [--out <path.csv>]\n"
                     "                 [--set key=value]... [--config <overrides.txt>]\n"
                     "       DvrReplay --dataset <path.dvrbin> --dump-recorded [--out <path.csv>]\n"
                     "       DvrReplay --dataset <path.dvrbin> --dump-frames [--out <path.bin>]\n"
                     "       DvrReplay --diff <a.csv> <b.csv>\n");
        return 2;
    }
    if (out.empty()) {
        out = dataset;
        out.replace_extension(dumpFrames     ? ".frames.bin"
                              : dumpRecorded ? ".recorded.csv"
                                             : ".replay.csv");
    }

#if EGOTOUCH_DIAG
    if (dumpFrames) return RunDumpFrames(dataset, out);
    if (dumpRecorded) return RunDumpRecorded(dataset, out);
    return RunReplay(dataset, out, overrideSpecs, overridesFile);
#else
    std::fprintf(stderr,
                 "DvrReplay was built without EGOTOUCH_DIAG; the dvrbin reader only wires "
                 "raw frame data in DIAG builds. Rebuild in Debug configuration.\n");
    return 2;
#endif
}
