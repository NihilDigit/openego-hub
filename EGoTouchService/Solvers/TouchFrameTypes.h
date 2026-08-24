#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "SolverBuildConfig.h"
#include "TouchSolver/TouchSharedTypes.h"

namespace Solvers {

namespace Touch {
class StylusTouchSuppressor;
}

enum TouchContactState : int {
    TouchStateDown = 0,
    TouchStateMove = 1,
    TouchStateUp = 2,
};

enum TouchLifeFlagBits : uint32_t {
    TouchLifeMapped = 1u << 0,
    TouchLifeNew = 1u << 1,
    TouchLifeLiftOff = 1u << 2,
    TouchLifeEdge = 1u << 3,
    TouchLifeDebounced = 1u << 4,
    TouchLifeAlwaysMatch = 1u << 5,
    TouchLifeSilentGap = 1u << 6,
};

// 第 5 级（笔画）的判定与阶段。判定只做一次，之后粘着；阶段是笔画交给第 6 级的
// 全部信息——第 6 级据此发事件，自己不再判掌。见设计文档第五节。
enum class StrokeVerdict : uint8_t {
    Pending = 0,
    Valid = 1,
    Palm = 2,
};

enum class StrokePhase : uint8_t {
    Holding = 0,    // 还没判，不要发任何事件，但笔画还活着
    Active = 1,     // 判为有效，按正常生命周期发 按下 / 移动 / 抬起
    Cancelled = 2,  // 判为掌。发过按下才需要 cancel，没发过就什么都不发
};

enum TouchReportEventCode : int {
    TouchReportIdle = 1,
    TouchReportDown = 2,
    TouchReportMove = 4,
    TouchReportUp = 0x20,
};

// 触摸点结构体 (用于 Stage 2 连通域计算)
struct TouchContact {
    int id = 0;
    float x = 0.0f;
    float y = 0.0f;
    int state = 0; // 0=Down, 1=Update, 2=Up
    int areaCells = 0;  // 连通域大小或强度
    int signalSum = 0; // 区域信号总和(对齐 TS 的 SigSum 语义)

    // Extended fields for TS/TE/TouchReport-aligned processing.
    // sizeMm 是从 signalSum 拟合的，受按压力度干扰，见 EstimateContactSizeMm 的说明。
    // areaMm2 是测量值：接触占据的传感器格数乘以格间距的平方，只随接触面积变化。掌与指的
    // 面积相差一个量级，靠 sizeMm 分不开的场合用这个。
    float sizeMm = 0.0f;
    float areaMm2 = 0.0f;
    bool isEdge = false;
    bool isReported = true;
    // EdgeRejector 的判定结果。它必须跑在 tracker 之前（那是唯一能按下标对齐 edgeInfos 的
    // 位置），而 tracker 会无条件重写 isReported，所以判定只能存在这里等 tracker 取用。
    bool edgeRejected = false;
    int prevIndex = -1;
    int debugFlags = 0;
    uint32_t edgeFlags = 0;
    uint8_t centroidEdgeFlags = 0;
    uint32_t ecFlags = 0;
    float edgeDistXCells = 0.0f;
    float edgeDistYCells = 0.0f;
    float matchXCells = 0.0f;
    float matchYCells = 0.0f;
    uint8_t ecWidthX = 0;
    uint8_t ecWidthY = 0;

    // TS/TE/TouchReport-aligned state mirrors
    uint32_t lifeFlags = 0;
    uint32_t reportFlags = 0;
    int reportEvent = 0;

    // Upstream peak identity, used only as a weak tracking hint.
    uint8_t sourcePeakId = 0;
    uint8_t sourcePeakAge = 0;

    // 生成这个接触点的峰的原始信号值。厂商的按下确认门槛比的正是峰值(而不是区内
    // 信号总和),两者在弱接触点上区分度差很多:掌语料上按总和拟合的最优门槛会误伤
    // 19% 的真接触点,按每格均值只误伤 17%,而峰值应当更好。
    int16_t peakSignal = 0;

    // 握持比例:最外一圈信号和 ÷ 次外一圈,Q4 定点(即真实比值的 16 倍),两轴各一个。
    //
    // 这是「接触点被边界切掉了多少」的可观测量,不是边缘补偿的内部变量。补偿是它的
    // 第一个消费者(LUT 索引),边缘拒绝与掌判也该读它。放在第 3 级算一次、挂在接触点
    // 上,而不是让每个消费者各自从 ZoneEdgeInfo 重算——重算的那份会随着谁先跑而漂移。
    uint8_t gripRatioXQ4 = 0;
    uint8_t gripRatioYQ4 = 0;

    // 第 5 级的归属与阶段。0 表示还没归到任何笔画（跟踪级没给 id 的接触点）。
    int     strokeId = 0;
    uint8_t strokePhase = static_cast<uint8_t>(StrokePhase::Active);
    // 按下该落在哪里。默认就是这条笔画第一帧的上报位置；移入修正生效时是反向外推
    // 出来的入界点。第 6 级发按下时取这个值，自己不再决定起手点。
    float   strokeDownX = 0.0f;
    float   strokeDownY = 0.0f;
};

template <typename T, size_t Capacity>
class FixedVector {
public:
    using value_type = T;
    using iterator = typename std::array<T, Capacity>::iterator;
    using const_iterator = typename std::array<T, Capacity>::const_iterator;

    [[nodiscard]] constexpr size_t size() const noexcept { return m_size; }
    [[nodiscard]] constexpr size_t capacity() const noexcept { return Capacity; }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_size == 0; }

    constexpr void clear() noexcept { m_size = 0; }
    constexpr void reserve(size_t) const noexcept {}

    constexpr void resize(size_t count) noexcept {
        const size_t newSize = std::min(count, Capacity);
        for (size_t i = m_size; i < newSize; ++i) {
            m_data[i] = T{};
        }
        m_size = newSize;
    }

    constexpr bool try_push_back(const T& value) noexcept {
        if (m_size >= Capacity) return false;
        m_data[m_size++] = value;
        return true;
    }

    constexpr bool try_push_back(T&& value) noexcept {
        if (m_size >= Capacity) return false;
        m_data[m_size++] = std::move(value);
        return true;
    }

    constexpr void push_back(const T& value) noexcept { (void)try_push_back(value); }
    constexpr void push_back(T&& value) noexcept { (void)try_push_back(std::move(value)); }

    constexpr void assign(const T* first, const T* last) noexcept {
        clear();
        for (const T* it = first; it != last && m_size < Capacity; ++it) {
            m_data[m_size++] = *it;
        }
    }

    template <typename InputIt>
    constexpr void assign(InputIt first, InputIt last) noexcept {
        clear();
        for (auto it = first; it != last && m_size < Capacity; ++it) {
            m_data[m_size++] = *it;
        }
    }

    constexpr iterator erase(const_iterator first, const_iterator last) noexcept {
        const size_t beginIndex = static_cast<size_t>(first - m_data.cbegin());
        const size_t endIndex = static_cast<size_t>(last - m_data.cbegin());
        if (beginIndex > m_size || endIndex <= beginIndex) {
            return begin() + static_cast<std::ptrdiff_t>(std::min(beginIndex, m_size));
        }
        const size_t clampedEnd = std::min(endIndex, m_size);
        const size_t removed = clampedEnd - beginIndex;
        std::move(m_data.begin() + static_cast<std::ptrdiff_t>(clampedEnd),
                  m_data.begin() + static_cast<std::ptrdiff_t>(m_size),
                  m_data.begin() + static_cast<std::ptrdiff_t>(beginIndex));
        m_size -= removed;
        return begin() + static_cast<std::ptrdiff_t>(beginIndex);
    }

    constexpr T& operator[](size_t index) noexcept { return m_data[index]; }
    constexpr const T& operator[](size_t index) const noexcept { return m_data[index]; }

    constexpr iterator begin() noexcept { return m_data.begin(); }
    constexpr iterator end() noexcept { return m_data.begin() + static_cast<std::ptrdiff_t>(m_size); }
    constexpr const_iterator begin() const noexcept { return m_data.begin(); }
    constexpr const_iterator end() const noexcept { return m_data.begin() + static_cast<std::ptrdiff_t>(m_size); }
    constexpr const_iterator cbegin() const noexcept { return begin(); }
    constexpr const_iterator cend() const noexcept { return end(); }

    constexpr std::span<T> span() noexcept { return {m_data.data(), m_size}; }
    constexpr std::span<const T> span() const noexcept { return {m_data.data(), m_size}; }

private:
    std::array<T, Capacity> m_data{};
    size_t m_size = 0;
};

inline constexpr size_t kMaxTouchContacts = 20;
inline constexpr size_t kMaxTouchZoneBoxes = 20;
inline constexpr size_t kMaxPalmDebugBoxes = 20;

struct TouchPacket {
    bool valid = false;
    uint8_t reportId = 0x01;
    uint8_t length = 0x20;
    std::array<uint8_t, 32> bytes{};
};

struct TouchPeak {
    int r = 0;
    int c = 0;
    int16_t z = 0;
    uint8_t id = 0;
};

// Represents a connected component in the heatmap greater than a global threshold
struct MacroZone {
    std::span<const int> pixels{}; // 1D indices (r * cols + c), owned by MacroZoneDetector arena
    int areaCells = 0;
    int signalSum = 0;
    int minR = 39;
    int maxR = 0;
    int minC = 59;
    int maxC = 0;
};

#if EGOTOUCH_DIAG
struct TouchDebugRect {
    int minR = 39;
    int maxR = 0;
    int minC = 59;
    int maxC = 0;
};

struct TouchZoneDebugBox {
    uint8_t zoneId = 0;
    uint8_t zoneIndex = 0;
    uint16_t reserved = 0;
    TouchDebugRect bbox{};
    int areaCells = 0;
    int signalSum = 0;
};

struct TouchPalmDebugBox {
    int id = 0;
    TouchDebugRect bbox{};
    TouchDebugRect expandedBbox{};
    int age = 0;
    int missed = 0;
    int lastMatchedZoneIndex = -1;
    int anchorPeakCount = 0;
    int signalSum = 0;
    bool matchedPalmThisFrame = false;
};
#endif

struct TouchOutputState {
    FixedVector<TouchContact, kMaxTouchContacts> contacts;
    std::array<TouchPacket, 2> touchPackets{};
};

#if EGOTOUCH_DIAG
struct TouchDebugFrame {
    std::vector<TouchPeak> peaks;
    std::array<uint8_t, 2400> touchZones{};
    std::array<uint8_t, 2400> peakZones{};
    FixedVector<TouchZoneDebugBox, kMaxTouchZoneBoxes> zoneBoxes;
    FixedVector<TouchPalmDebugBox, kMaxPalmDebugBoxes> palmBoxes;
};
#endif

// ── 运行时中间态（Pipeline 各阶段的共享数据总线）──────────────────────
// 全部使用 span/pointer 零拷贝引用模块内部存储，frame 与 pipeline 同生命周期。
struct TouchRuntimeState {
    // Phase 3: MacroZoneDetector → PeakDetector / TouchClassifier / PalmBoxSuppressor
    const std::vector<MacroZone>*              macroZones = nullptr;

    // Phase 3: PeakDetector → TouchClassifier / PalmBoxSuppressor / ContactExtractor
    std::span<const Touch::Peak>               peaks;
    int16_t                                    peakThreshold = 0;

    // Phase 4: PalmBoxSuppressor → ContactExtractor
    std::span<const Touch::PeakEvaluation>     peakEvaluations;

    // Phase 4: TouchClassifier → PalmBoxSuppressor
    std::span<const Touch::MacroZoneFeature>   zoneFeatures;

    // Phase 5: ContactExtractor → EdgeCompensator / EdgeRejector
    std::span<const ZoneEdgeInfo>              edgeInfos;
    const EdgeBounds*                          edgeBounds = nullptr;

    // 跨帧：Phase 6 的 TouchTracker 写入 → 下一帧 Phase 3 的 PeakDetector 读取。
    // 检测级在跟踪级之前跑，所以这里拿到的必然是上一帧的轨迹位置，迟滞正需要这个。
    std::span<const Touch::TrackAnchor>        prevTrackAnchors;

    // 跨帧标志：Phase 6 写入 → 下一帧 Phase 2 读取
    bool                                       hasLiveTouchState = false;

    // Stylus 抑制器配置指针
    const Touch::StylusTouchSuppressor*        stylusSuppress = nullptr;
};

struct TouchFrameData {
    // 调理级的产物。原始热图留在 HeatmapFrame::heatmapMatrix 里不动(契约 4)。
    //
    // 此前基线与 CMF 是**就地改写**原始热图的,于是「帧里那张图是原始的还是调理过的」
    // 取决于读它的人跑在管线的哪一步——重放工具因此要为旧录制特判,诊断也无从对照
    // 调理前后。分开之后这个问题不存在:原始图只有第 1 级读,调理图只有第 2 级写。
    int16_t conditioned[40][60]{};

    TouchOutputState output{};
    TouchRuntimeState runtime{};
#if EGOTOUCH_DIAG
    TouchDebugFrame debug{};
#endif

    inline void ResetRuntime() { runtime = {}; }
};

} // namespace Solvers
