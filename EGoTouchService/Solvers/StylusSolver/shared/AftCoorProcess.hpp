#pragma once

#include "StylusSolver/AsaTypes.hpp"
#include "SolverTypes.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace Solvers::Stylus {

class AftCoorProcess {
public:
    bool m_enabled = true;

    // ── Lock thresholds (flash params equivalent) ──
    // In-band thresholds (coordinate within sensor interior)
    uint8_t m_lockFlashInBandX = 0;  // flash[0xA5A]
    uint8_t m_lockFlashInBandY = 0;  // flash[0xA5B]

    // Edge thresholds (coordinate near sensor boundary)
    //
    // Verified against the original TSAPrmt.dll parameter block: every
    // g_tsaPrmtFlashAsaGaokunHimax* project carries 8 / 16 here. The 1 / 2 that used to
    // sit in this file is byte-for-byte the Ags3 project's value — a different panel
    // (32x48 sensor vs this device's 60x40), so the port had mixed two projects: the IIR
    // coefficients below came from Gaokun while these lock thresholds came from Ags3.
    // Too small a threshold releases the pen-down lock almost immediately, letting the
    // touchdown coordinate jitter through instead of being pinned to the start point.
    uint8_t m_lockFlashEdgeX = 8;   // flash[0xA58]
    uint8_t m_lockFlashEdgeY = 16;  // flash[0xA59]

    // Sensor geometry (asaPrmt, offsets within the TSAPrmt project block)
    //
    // ScaleLockThreshold() divides by these, so they must be the sensor's PHYSICAL span
    // along each axis in the same unit the flash thresholds use — not screen pixels.
    // They previously held 0x0A00 / 0x0640 = 2560 / 1600, which is this panel's display
    // resolution, giving thresholds ~1.7x too large.
    //
    // The correct values come straight out of the project block: 0xA2C is the TX-axis
    // span and 0xA2A the RX-axis one. That mapping is not a guess — checking
    // span/count across all 202 Asa* projects with plausible geometry, this direction
    // yields a self-consistent pitch for 131 of them while the reverse mapping matches
    // none. Ags3 lands on exactly 72 for both axes; Gaokun is 72 (TX) and 66 (RX), a
    // slightly non-square grid. At 1/16 mm that is 270 x 165 mm, matching this device's
    // 12.35" 16:10 panel (~266 x 166 mm).
    int m_sensorTxCount = 60;   // bTxCount   @0xA28
    int m_sensorRxCount = 40;   // bRxCount   @0xA29
    int m_sensorDim1 = 4320;    // TX span    @0xA2C
    int m_sensorDim2 = 2640;    // RX span    @0xA2A

    // Bypass gate: skip lock when this ASA-style flag is active
    bool m_bypassLock = false;

    inline void Reset() {
        m_startX = 0;
        m_startY = 0;
        m_lockOffsetX = 0;
        m_lockOffsetY = 0;
        m_flagLockX = false;
        m_flagLockY = false;
        m_prevPressure = 0;
    }

    inline void Process(HeatmapFrame& frame) {
        auto& runtime = frame.stylus.runtime.Active();
        auto& coor = runtime.post.finalCoor;
        const uint16_t curPressure = runtime.pressure.outputPressure;

        if (!m_enabled || m_bypassLock) {
            // Pass-through: just clamp to sensor bounds
            if (coor.valid) {
                ClampToSensor(coor);
            }
            m_prevPressure = curPressure;
            return;
        }

        if (!coor.valid) {
            Reset();
            m_prevPressure = curPressure;
            return;
        }

        // ── Select thresholds based on coordinate position ──
        // TSACore: if coor < 0x401 or coor >= (count-1)*0x400 → edge threshold
        const int32_t minBound = 0x401;
        const int32_t maxBoundX = (m_sensorTxCount - 1) * Asa::kCoorUnit;
        const int32_t maxBoundY = (m_sensorRxCount - 1) * Asa::kCoorUnit;

        uint32_t thresholdX;
        uint32_t thresholdY;

        if (coor.dim1 < minBound || coor.dim2 < minBound ||
            coor.dim1 >= maxBoundX || coor.dim2 >= maxBoundY) {
            thresholdX = ScaleLockThreshold(m_lockFlashEdgeX, m_sensorTxCount, m_sensorDim1);
            thresholdY = ScaleLockThreshold(m_lockFlashEdgeY, m_sensorRxCount, m_sensorDim2);
        } else {
            thresholdX = ScaleLockThreshold(m_lockFlashInBandX, m_sensorTxCount, m_sensorDim1);
            thresholdY = ScaleLockThreshold(m_lockFlashInBandY, m_sensorRxCount, m_sensorDim2);
        }

        // ── Pen-down transition: lock to start position ──
        if (curPressure != 0 && m_prevPressure == 0) {
            m_startX = coor.dim1;
            m_startY = coor.dim2;
            m_flagLockX = true;
            m_flagLockY = true;
            m_lockOffsetX = 0;
            m_lockOffsetY = 0;
        }

        // ── X-axis lock ──
        // Releasing the lock must also drop the offset. The subtraction below runs
        // unconditionally, so an offset left behind at release would shift the rest of
        // the stroke by roughly the threshold until the next pen-down reset it. That was
        // latent while the thresholds were wrong (0.10 mm on X), and became visible as a
        // drifting touchdown point once they were corrected to the stock values (0.50 mm).
        if (m_flagLockX) {
            const int32_t diff = coor.dim1 - m_startX;
            const uint32_t absDiff = static_cast<uint32_t>(std::abs(diff));
            if (absDiff > thresholdX) {
                m_flagLockX = false;   // movement exceeded threshold → release lock
                m_lockOffsetX = 0;     // and stop displacing the coordinate
            } else {
                m_lockOffsetX = diff;  // track offset while locked
            }
        }

        // ── Y-axis lock ──
        if (m_flagLockY) {
            const int32_t diff = coor.dim2 - m_startY;
            const uint32_t absDiff = static_cast<uint32_t>(std::abs(diff));
            if (absDiff > thresholdY) {
                m_flagLockY = false;
                m_lockOffsetY = 0;
            } else {
                m_lockOffsetY = diff;
            }
        }

        // ── Apply offset and clamp ──
        int32_t finalX = coor.dim1 - m_lockOffsetX;
        int32_t finalY = coor.dim2 - m_lockOffsetY;

        finalX = std::clamp(finalX, 0, m_sensorTxCount * Asa::kCoorUnit);
        finalY = std::clamp(finalY, 0, m_sensorRxCount * Asa::kCoorUnit);

        coor.dim1 = finalX;
        coor.dim2 = finalY;

#if EGOTOUCH_DIAG
        runtime.post.lockActiveX = m_flagLockX;
        runtime.post.lockActiveY = m_flagLockY;
        runtime.post.lockOffsetX = m_lockOffsetX;
        runtime.post.lockOffsetY = m_lockOffsetY;
        runtime.post.lockThresholdX = static_cast<int32_t>(thresholdX);
        runtime.post.lockThresholdY = static_cast<int32_t>(thresholdY);
#endif

        m_prevPressure = curPressure;
    }

private:
    int32_t m_startX = 0;
    int32_t m_startY = 0;
    int32_t m_lockOffsetX = 0;
    int32_t m_lockOffsetY = 0;
    bool m_flagLockX = false;
    bool m_flagLockY = false;
    uint16_t m_prevPressure = 0;

    static inline uint32_t ScaleLockThreshold(uint8_t flashValue, int sensorCount, int physicalSize) {
        if (physicalSize <= 0) return 0;
        return (static_cast<uint32_t>(flashValue) * static_cast<uint32_t>(sensorCount) * Asa::kCoorUnit) /
               static_cast<uint32_t>(physicalSize);
    }

    inline void ClampToSensor(Asa::CoorResult& coor) const {
        coor.dim1 = std::clamp(coor.dim1, 0, m_sensorTxCount * Asa::kCoorUnit);
        coor.dim2 = std::clamp(coor.dim2, 0, m_sensorRxCount * Asa::kCoorUnit);
    }
};

} // namespace Solvers::Stylus
