#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "SolverTypes.h"

#ifndef _WINDOWS_
#include <Windows.h>
#endif

/// VhfReporter — 负责将 Pipeline 输出的 TouchPacket /
/// stylus output contract 通过 VHF HID 注入器驱动写入系统。
/// 入口：Dispatch(HeatmapFrame&)  —— Worker 一行调用。
class VhfReporter {
public:
    VhfReporter();
    ~VhfReporter();
    VhfReporter(const VhfReporter&) = delete;
    VhfReporter& operator=(const VhfReporter&) = delete;

    /// 主入口 (legacy, 后向兼容)
    void Dispatch(Solvers::HeatmapFrame& frame);

    /// 独立手写笔写入；writeEnabled=false 时只回填 packet/diag，不写 VHF
    void DispatchStylus(Solvers::HeatmapFrame& frame, bool writeEnabled = true);

    /// 独立手指写入 (含 BuildTouchReports)
    void DispatchTouch(Solvers::HeatmapFrame& frame);

    void SetStylusPacketSensorRows(int rows);
    void SetStylusPacketSensorCols(int cols);
    void SetStylusPacketEmitWhenInvalid(bool v);

    // 开关
    void SetEnabled(bool v);
    bool IsEnabled() const { return m_enabled.load(std::memory_order_relaxed); }
    void FlushTouchAllUp();

    // OneNote 兼容操作期间的短时输出闸门。它与长期 VHF enabled 配置正交：冻结时先终止
    // 已上报的触摸和笔接触，再丢弃后续报告；解除后下一帧自然恢复。
    void SetInputSuppressed(bool suppressed);
    bool IsInputSuppressed() const {
        return m_inputSuppressed.load(std::memory_order_acquire);
    }

    void SetTransposeEnabled(bool v) {
        m_transpose.store(v, std::memory_order_relaxed);
    }
    bool IsTransposeEnabled() const {
        return m_transpose.load(std::memory_order_relaxed);
    }

    void SetEraserState(uint8_t v) {
        m_eraserRequested.store(v == 1u, std::memory_order_release);
    }

    void SetBarrelButtonState(bool pressed) {
        m_barrelButtonState.store(pressed, std::memory_order_relaxed);
    }

    bool IsDeviceOpen() const;
    void Close();

private:
    struct StylusDispatchPacket {
        Solvers::StylusPacket packet{};
        uint8_t penState = 0;
    };

    struct StylusToolPacket {
        std::array<uint8_t, 17> bytes{};
        std::array<uint8_t, 17> transitionBytes{};
        bool prependOutOfRange = false;
    };

    bool UpdateTouchState(bool hasTouch);
    StylusDispatchPacket BuildStylusPacket(
        const Solvers::StylusFrameData& stylus);
    StylusToolPacket ApplyStylusToolState(
        const Solvers::StylusPacket& packet);


    void WriteTouchPacketsLocked(
        const std::array<Solvers::TouchPacket, 2>& packets);
    void WriteTouchAllUpLocked();
    void WriteStylusNeutralLocked();
    void WriteStylusPacketLocked(const uint8_t* data, size_t len);

    bool CanDispatchWriteLocked() const;
    bool EnsureDeviceOpenLocked();
    void CloseDeviceLocked();
    void ScheduleReopenLocked();
    bool WritePacketLocked(const uint8_t* data, size_t len,
                           const char* tag);

    static constexpr auto kReopenBackoff = std::chrono::milliseconds(200);

    std::atomic<bool> m_enabled{true};
    std::atomic<bool> m_inputSuppressed{false};
    std::atomic<bool> m_transpose{false};
    std::atomic<bool> m_hadTouchLastFrame{false};
    std::atomic<bool> m_hadStylusActiveLastFrame{false};
    std::atomic<bool> m_eraserRequested{false};
    std::atomic<bool> m_barrelButtonState{false};

    // requested 由按键/配置线程写，applied 只在报文路径推进。单独加锁是为了兼容 legacy
    // Dispatch 与独立 DispatchStylus 被不同调用方使用的情况。
    std::mutex m_stylusToolMu;
    bool m_eraserApplied = false;

    int m_stylusSensorRows = 40;
    int m_stylusSensorCols = 60;

    mutable std::mutex m_mu;
    bool m_closed = false;
    bool m_emitStylusPacketWhenInvalid = true;
    HANDLE m_handle = INVALID_HANDLE_VALUE;
    std::chrono::steady_clock::time_point m_nextOpenAttempt{};

    static const GUID kVhfGuid;
};
