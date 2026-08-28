#include "NaturalColor.h"

#include "ColorPipeline.h"
#include "LightSensor.h"

#include <windows.h>

#include <cmath>
#include <cstdio>

namespace Gaokun::Display {

namespace {

volatile LONG g_stop = 0;

BOOL WINAPI ConsoleHandler(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        InterlockedExchange(&g_stop, 1);
        return TRUE;
    default:
        return FALSE;
    }
}

// COM 初始化的作用域。传感器对象只在本线程使用，套间选 MTA 还是 STA 对它没有影响，
// 这里取 MTA，免得后续在同一进程里挂一个消息循环才能收事件。
class ComScope {
public:
    ComScope() noexcept : m_ok(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope() noexcept {
        if (m_ok) CoUninitialize();
    }
    [[nodiscard]] bool Ok() const noexcept { return m_ok; }

private:
    bool m_ok;
};

[[nodiscard]] bool NearlyEqual(const RgbGain &a, const RgbGain &b) noexcept {
    constexpr double kEpsilon = 1e-4;
    return std::fabs(a.r - b.r) < kEpsilon && std::fabs(a.g - b.g) < kEpsilon &&
           std::fabs(a.b - b.b) < kEpsilon;
}

} // namespace

void ProbeSensor(void (*sink)(const wchar_t *)) noexcept {
    ComScope com;
    if (!com.Ok()) {
        sink(L"CoInitializeEx failed");
        return;
    }
    DumpAllLightSensors(sink);

    // 两种情形各读一遍。首次实测读到 10143K 时这里还没有 ISensorEvents，订阅是后来按
    // 「传感器要有人订阅才采样」的推断加的——而加完之后再没读到过值，所以要对照的是
    // 订阅本身是否反而挡住了轮询。
    for (const bool subscribe : {false, true}) {
        sink(subscribe ? L"=== with SetEventSink ===" : L"=== without SetEventSink ===");
        LightSensor sensor;
        if (!sensor.Open(subscribe)) {
            sink(sensor.LastError());
            continue;
        }
        sensor.DumpReport(sink);
    }
}

bool ReadAmbientCct(double &kelvin, const wchar_t *&error) noexcept {
    ComScope com;
    if (!com.Ok()) {
        error = L"CoInitializeEx failed";
        return false;
    }
    LightSensor sensor;
    if (!sensor.Open()) {
        error = sensor.LastError();
        return false;
    }
    if (!sensor.ReadCct(kelvin)) {
        // 带上传感器状态与 HRESULT：光看「没有数据」分不清是它还没准备好、被别的进程占着，
        // 还是这台机器上根本没有真实的光感元件。
        static wchar_t detail[160];
        std::swprintf(detail, std::size(detail),
                      L"%ls (sensor state %d, field hr 0x%08lX, vt %u)", sensor.LastError(),
                      sensor.State(), static_cast<unsigned long>(sensor.LastHresult()),
                      static_cast<unsigned>(sensor.LastVarType()));
        error = detail;
        return false;
    }
    return true;
}

int RunNaturalColorDaemon(Qdcm &qdcm, const std::vector<uint32_t> &targets) noexcept {
    ComScope com;
    if (!com.Ok()) {
        wprintf(L"CoInitializeEx failed\n");
        return 1;
    }

    LightSensor sensor;
    if (!sensor.Open()) {
        wprintf(L"%ls\n", sensor.LastError());
        return 1;
    }

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    wprintf(L"natural colour daemon running; Ctrl+C to stop\n");

    // 起点取上一次持久化的色温，避免守护进程重启时画面从中性猛跳回去。
    ColorState state = LoadColorState();
    RgbGain current = NaturalColorGain(state);
    RgbGain rampTarget = current;
    RgbGain rampStep{0.0, 0.0, 0.0};
    int framesLeft = 0;
    double storedCct = state.sensorCct;
    // 有意与首帧的合成结果不同，好让第一帧一定下发一次，把面板对齐到我们记的状态。
    RgbGain applied{-1.0, -1.0, -1.0};

    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        // 每帧重读状态：守护进程运行期间用户仍可能改色温或护眼，那两项由一次性命令写入
        // 注册表，只有重读才能把它们合进本帧的 PCC。
        state = LoadColorState();
        if (!state.naturalColor) {
            wprintf(L"natural colour was turned off; exiting\n");
            break;
        }

        double cct = 0.0;
        double lux = 0.0;
        const bool haveLux = sensor.ReadLux(lux);
        if (sensor.ReadCct(cct) && cct > 0.0 && (!haveLux || lux >= kNcLuminanceThreshold)) {
            const RgbGain target = GainForCct(cct);
            if (!NearlyEqual(target, rampTarget)) {
                rampTarget = target;
                rampStep = RgbGain{(target.r - current.r) / kNcFrameCount,
                                   (target.g - current.g) / kNcFrameCount,
                                   (target.b - current.b) / kNcFrameCount};
                framesLeft = kNcFrameCount;
                storedCct = cct;
            }
        }

        if (framesLeft > 0) {
            --framesLeft;
            if (framesLeft == 0) {
                current = rampTarget;
            } else {
                current.r += rampStep.r;
                current.g += rampStep.g;
                current.b += rampStep.b;
            }
            // 渐变收尾时才落盘。逐帧写注册表没有意义，而中途退出时保留的是上一个稳定值，
            // 下次启动从那里接着走。
            if (framesLeft == 0 && storedCct != state.sensorCct) {
                ColorState saved = state;
                saved.sensorCct = storedCct;
                (void)StoreColorState(saved);
            }
        }

        // 自然色彩这一项用 current（渐变中的中间值），另外两项照常从状态里合成，因此
        // 用户在守护进程运行期间改色温或护眼也能在下一帧生效。
        ColorState frame = state;
        frame.naturalColor = false;
        const RgbGain composed = Multiply(Compose(frame), current);

        // 只在结果真的变了才下发。稳态下这里一帧也不写，环境光不动时没有理由每 100 ms
        // 敲一次驱动。
        if (!NearlyEqual(composed, applied)) {
            if (!ApplyGain(qdcm, targets, composed)) {
                wprintf(L"SetPcc failed; stopping so the panel is not left half-applied\n");
                break;
            }
            applied = composed;
        }

        Sleep(kNcFrameMs);
    }

    SetConsoleCtrlHandler(ConsoleHandler, FALSE);
    return 0;
}

} // namespace Gaokun::Display
