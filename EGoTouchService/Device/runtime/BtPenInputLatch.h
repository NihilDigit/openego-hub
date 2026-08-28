#pragma once

#include <array>
#include <cstdint>
#include <mutex>

namespace Device {

/// 蓝牙笔 MCU 送来的压力与频点。MCU 的上报节奏与触摸帧无关，锁存最新一份，取帧时快照。
///
/// 这个结构按字节整体交给 TSA 后端（TsaFrameInput::bluetoothPen 是一个 span），后端按
/// 自己的 ABI 解码，所以字段顺序和类型是对外契约，不能为了好看重排。它沿用求解器时代
/// Asa::BtInputSnapshot 的布局，下面的 static_assert 守住这一点。
struct BtPenSample {
    std::array<uint16_t, 4> pressure{};
    std::array<uint16_t, 4> rawPressure{};
    uint32_t seq = 0;
    uint8_t freq1 = 0;
    uint8_t freq2 = 0;
    bool hasSample = false;
    bool hasFreq = false;
};

static_assert(sizeof(BtPenSample) == 24,
              "BtPenSample is handed to the TSA backend as raw bytes; its layout is an ABI "
              "contract and must stay at the 24 bytes the vendor adapters decode.");
static_assert(alignof(BtPenSample) == 4, "BtPenSample must keep its 4-byte alignment.");

/// 写在 MCU 通知线程，读在取帧线程，所以整份快照进出都持锁。seq 只用于让消费方分辨
/// 「同一份样本又读了一次」与「来了新样本」，不参与解码。
class BtPenInputLatch {
public:
    void SetPressure(uint16_t pressure) {
        BtPenSample next{};
        // 单值上报只有末位压力有意义，其余留零：MCU 在这条路径上不发原始值与频点。
        next.pressure[3] = pressure;
        next.hasSample = true;
        Store(next);
    }

    void SetPressurePacket(const std::array<uint16_t, 4> &pressure,
                           const std::array<uint16_t, 4> &rawPressure, uint8_t freq1,
                           uint8_t freq2) {
        BtPenSample next{};
        next.pressure = pressure;
        next.rawPressure = rawPressure;
        next.freq1 = freq1;
        next.freq2 = freq2;
        next.hasSample = true;
        next.hasFreq = true;
        Store(next);
    }

    void Snapshot(BtPenSample &out) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        out = m_sample;
    }

    /// 笔的连接状态变化时清空。上一支笔的压力不能落到下一次落笔上。
    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sample = {};
    }

private:
    void Store(BtPenSample &next) {
        std::lock_guard<std::mutex> lock(m_mutex);
        next.seq = m_sample.seq + 1;
        m_sample = next;
    }

    mutable std::mutex m_mutex;
    BtPenSample m_sample{};
};

} // namespace Device
