#include "ColorPipeline.h"
#include "CubeFile.h"
#include "FactoryLut.h"
#include "GaokunDisplay.h"
#include "Lut.h"
#include "NaturalColor.h"
#include "Qdcm.h"
#include "Targets.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace Gaokun::Display;

namespace {

void PrintUsage() {
    wprintf(L"gaokun-display -- colour calibration for the MateBook E Go\n\n"
            L"  --preset <Native|sRGB|DisplayP3>  load the factory calibration\n"
            L"  --temperature <off|warm|neutral|cool|2000..10000>\n"
            L"                                    screen colour temperature in kelvin\n"
            L"  --eye-comfort <on|off>            cut the blue channel to 0.72\n"
            L"  --natural-color <on|off>          follow the ambient light colour temperature\n"
            L"  --natural-color-daemon            keep following it; Ctrl+C to stop\n"
            L"  --status                          report the three PCC settings\n"
            L"  --reset                           clear everything and go back to native\n"
            L"  --info                            report displays and firmware table status\n"
            L"  --3dlut <file.cube>               load a custom 3D LUT\n"
            L"  --igc <file.cube>                 load a custom input shaper (before the "
            L"3D LUT)\n\n"
            L"Gamut goes to the 3D LUT, the other three share the PCC matrix and are composed\n"
            L"before every write, so they all stack. IRIDAS and Resolve .cube files are both\n"
            L"accepted; sizes other than the hardware's (17 for the 3D LUT, 257 for the\n"
            L"shaper) are resampled.\n");
}

// --3dlut 与 --igc 共用的读取与重采样。硬件的尺寸是固定的，.cube 的不是。
[[nodiscard]] bool LoadCube(const wchar_t *path, bool wantThreeD, RgbTable &table) {
    CubeFile cube;
    std::string error;
    if (!ReadCubeFile(path, cube, error)) {
        wprintf(L"%ls: %hs\n", path, error.c_str());
        return false;
    }

    if (wantThreeD) {
        if (!cube.Is3d()) {
            wprintf(L"%ls holds a 1D table; --3dlut needs a 3D one\n", path);
            return false;
        }
        table = cube.lut3d.Resample(kLutSize).ToQdcm();
        return true;
    }

    if (!cube.Is1d()) {
        wprintf(L"%ls holds a 3D table; --igc needs a 1D one\n", path);
        return false;
    }
    table = cube.lut1d.Resize(kIgcEntries).ToQdcm();
    return true;
}

int ApplyCube(const wchar_t *path, bool threeD) {
    RgbTable table;
    if (!LoadCube(path, threeD, table)) return 1;

    Qdcm qdcm;
    if (!qdcm.Load()) {
        wprintf(L"qdcmlib unavailable (err=%lu)\n", GetLastError());
        return 1;
    }
    std::vector<uint32_t> ids;
    if (!EnumTargets(qdcm, ids)) return 1;

    int failures = 0;
    for (uint32_t id : ids) {
        const bool ok = threeD ? qdcm.SetLut3d(id, &table) : qdcm.SetIgc(id, &table);
        if (!ok) ++failures;
    }
    if (failures) {
        wprintf(L"applied with %d failed call(s)\n", failures);
        return 1;
    }
    wprintf(L"applied to %zu display(s)\n", ids.size());
    return 0;
}

int RunInfo() {
    const auto table = ReadDlutTable();
    if (table.empty()) {
        wprintf(L"DLUT firmware table: absent\n");
    } else {
        wprintf(L"DLUT firmware table: %zu bytes\n", table.size());
        wprintf(L"  DisplayP3 : %ls\n", PresetPresent(table, Preset::DisplayP3) ? L"present" : L"empty");
        wprintf(L"  sRGB      : %ls\n", PresetPresent(table, Preset::Srgb) ? L"present" : L"empty");
    }

    Qdcm qdcm;
    if (!qdcm.Load()) {
        wprintf(L"qdcmlib: unavailable (err=%lu)\n", GetLastError());
        return 1;
    }

    std::vector<uint32_t> ids;
    if (!qdcm.EnumDisplays(ids)) {
        wprintf(L"qdcmlib: display enumeration failed\n");
        return 1;
    }
    wprintf(L"displays: %zu\n", ids.size());
    for (uint32_t id : ids) {
        int32_t caps = 0;
        const int returned = qdcm.QueryCapsRaw(id, caps);
        // 0x200 = 支持 3D LUT，0x2 = 支持 PCC 矩阵。两个位的含义取自 goodies。
        wprintf(L"  id=%u caps=0x%08x (ret=0x%08x) 3dlut=%ls pcc=%ls\n", id, caps, returned,
                (caps & 0x200) ? L"yes" : L"no", (caps & 0x2) ? L"yes" : L"no");
    }
    return 0;
}

// 色域只改面板输出，不动操作系统的 ICC 关联——Photoshop 一类做色彩管理的应用因此仍按旧
// 色域解释画面。原厂那条 mscms 的关联路径在本机走不通（收下写入却不落地，管理员身份下
// 同样如此），实测记录在 docs/display-manage.md 6.5。
int ApplyGamut(Gamut gamut) {
    // Native 没有对应的 preset 槽位，它就是「不加 3D LUT」。
    RgbTable lut;
    const bool wantLut = gamut != Gamut::Native;
    if (wantLut) {
        const Preset preset = (gamut == Gamut::Srgb) ? Preset::Srgb : Preset::DisplayP3;
        const auto table = ReadDlutTable();
        if (table.empty()) {
            wprintf(L"the DLUT firmware table is not present on this machine\n");
            return 1;
        }
        if (!PresetPresent(table, preset)) {
            wprintf(L"that preset slot is empty on this machine\n");
            return 1;
        }
        if (!DecodePreset(table, preset, lut)) {
            wprintf(L"failed to decode the preset\n");
            return 1;
        }
    }

    Qdcm qdcm;
    if (!qdcm.Load()) {
        wprintf(L"qdcmlib unavailable (err=%lu)\n", GetLastError());
        return 1;
    }

    std::vector<uint32_t> ids;
    if (!EnumTargets(qdcm, ids)) return 1;

    // 工厂校准只有 3D LUT 一级，输入整形保持关闭，与原厂的 --preset 行为一致。
    int failures = 0;
    for (uint32_t id : ids) {
        if (!qdcm.SetIgc(id, nullptr)) ++failures;
        if (!qdcm.SetLut3d(id, wantLut ? &lut : nullptr)) ++failures;
    }
    if (failures) {
        wprintf(L"applied with %d failed call(s)\n", failures);
        return 1;
    }
    wprintf(L"applied to %zu display(s)\n", ids.size());
    return 0;
}

// 三项 PCC 能力的统一入口。改任意一项都走这里：先落盘，再把三项一起合成下发。分开下发
// 会互相覆盖——硬件上只有一份 PCC 矩阵。
int CommitColorState(const ColorState &state) {
    (void)StoreColorState(state);

    Qdcm qdcm;
    if (!qdcm.Load()) {
        wprintf(L"qdcmlib unavailable (err=%lu)\n", GetLastError());
        return 1;
    }
    std::vector<uint32_t> ids;
    if (!EnumTargets(qdcm, ids)) return 1;

    RgbGain gain;
    if (!ApplyColorState(qdcm, ids, state, &gain)) {
        wprintf(L"SetPcc failed\n");
        return 1;
    }
    wprintf(L"gain: r=%.4f g=%.4f b=%.4f\n", gain.r, gain.g, gain.b);
    return 0;
}

int RunTemperature(const wchar_t *arg) {
    ColorState state = LoadColorState();
    if (_wcsicmp(arg, L"off") == 0 || _wcsicmp(arg, L"neutral") == 0) {
        state.temperatureK = 0;
    } else if (_wcsicmp(arg, L"warm") == 0) {
        state.temperatureK = kTemperatureWarmK;
    } else if (_wcsicmp(arg, L"cool") == 0) {
        state.temperatureK = kTemperatureCoolK;
    } else {
        const int kelvin = _wtoi(arg);
        if (kelvin < kTemperatureMinK || kelvin > kTemperatureMaxK) {
            wprintf(L"colour temperature must be between %d and %d kelvin, or one of "
                    L"off/warm/neutral/cool\n",
                    kTemperatureMinK, kTemperatureMaxK);
            return 1;
        }
        state.temperatureK = kelvin;
    }
    return CommitColorState(state);
}

[[nodiscard]] bool ParseOnOff(const wchar_t *arg, bool &value) {
    if (_wcsicmp(arg, L"on") == 0) {
        value = true;
        return true;
    }
    if (_wcsicmp(arg, L"off") == 0) {
        value = false;
        return true;
    }
    return false;
}

int RunEyeComfort(const wchar_t *arg) {
    ColorState state = LoadColorState();
    if (!ParseOnOff(arg, state.eyeComfort)) {
        wprintf(L"--eye-comfort takes on or off\n");
        return 1;
    }
    return CommitColorState(state);
}

int RunNaturalColor(const wchar_t *arg) {
    ColorState state = LoadColorState();
    if (!ParseOnOff(arg, state.naturalColor)) {
        wprintf(L"--natural-color takes on or off\n");
        return 1;
    }
    if (state.naturalColor) {
        double cct = 0.0;
        const wchar_t *error = L"";
        if (!ReadAmbientCct(cct, error)) {
            wprintf(L"%ls\n", error);
            return 1;
        }
        state.sensorCct = cct;
        wprintf(L"ambient: %.0fK\n", cct);
    }
    // 这里只按当前环境光下发一次，不做渐变。持续跟随要另外拉起 --natural-color-daemon。
    return CommitColorState(state);
}

int RunNaturalColorDaemonCommand() {
    ColorState state = LoadColorState();
    if (!state.naturalColor) {
        wprintf(L"natural colour is off; run --natural-color on first\n");
        return 1;
    }
    Qdcm qdcm;
    if (!qdcm.Load()) {
        wprintf(L"qdcmlib unavailable (err=%lu)\n", GetLastError());
        return 1;
    }
    std::vector<uint32_t> ids;
    if (!EnumTargets(qdcm, ids)) return 1;
    return RunNaturalColorDaemon(qdcm, ids);
}

// 诊断：把环境光传感器的整份报告摊开。传感器声称支持色温、读出来却是空值时，光看
// --natural-color 的报错分不清是哪一层出的问题。
int RunSensorProbe() {
    ProbeSensor([](const wchar_t *line) { wprintf(L"%ls\n", line); });
    return 0;
}

int RunStatus() {
    const ColorState state = LoadColorState();
    const RgbGain gain = Compose(state);
    const std::wstring temperature =
        state.temperatureK ? std::to_wstring(state.temperatureK) + L"K" : L"off";
    wprintf(L"temperature : %ls\n", temperature.c_str());
    wprintf(L"eye comfort : %ls\n", state.eyeComfort ? L"on" : L"off");
    wprintf(L"natural col.: %ls", state.naturalColor ? L"on" : L"off");
    if (state.naturalColor) wprintf(L" (last ambient %.0fK)", state.sensorCct);
    wprintf(L"\n");
    wprintf(L"composed gain: r=%.4f g=%.4f b=%.4f\n", gain.r, gain.g, gain.b);
    return 0;
}

int RunReset() {
    Qdcm qdcm;
    if (!qdcm.Load()) {
        wprintf(L"qdcmlib unavailable (err=%lu)\n", GetLastError());
        return 1;
    }
    std::vector<uint32_t> ids;
    if (!EnumTargets(qdcm, ids)) return 1;

    // 顺序无所谓，三条通道彼此独立。PCC 没有开关位，单位阵就是它的关闭态。
    const ColorState cleared;
    float identity[9];
    ToPccMatrix(Compose(cleared), identity);
    for (uint32_t id : ids) {
        (void)qdcm.SetIgc(id, nullptr);
        (void)qdcm.SetLut3d(id, nullptr);
        (void)qdcm.SetPcc(id, identity);
    }
    (void)StoreColorState(cleared);
    wprintf(L"reset %zu display(s)\n", ids.size());
    return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    if (_wcsicmp(argv[1], L"--info") == 0) return RunInfo();
    if (_wcsicmp(argv[1], L"--status") == 0) return RunStatus();
    if (_wcsicmp(argv[1], L"--reset") == 0) return RunReset();
    if (_wcsicmp(argv[1], L"--natural-color-daemon") == 0) return RunNaturalColorDaemonCommand();
    if (_wcsicmp(argv[1], L"--sensor-probe") == 0) return RunSensorProbe();

    if (_wcsicmp(argv[1], L"--3dlut") == 0 && argc >= 3) return ApplyCube(argv[2], true);
    if (_wcsicmp(argv[1], L"--igc") == 0 && argc >= 3) return ApplyCube(argv[2], false);
    if (_wcsicmp(argv[1], L"--temperature") == 0 && argc >= 3) return RunTemperature(argv[2]);
    if (_wcsicmp(argv[1], L"--eye-comfort") == 0 && argc >= 3) return RunEyeComfort(argv[2]);
    if (_wcsicmp(argv[1], L"--natural-color") == 0 && argc >= 3) return RunNaturalColor(argv[2]);

    if (_wcsicmp(argv[1], L"--preset") == 0 && argc >= 3) {
        if (_wcsicmp(argv[2], L"Native") == 0) return ApplyGamut(Gamut::Native);
        if (_wcsicmp(argv[2], L"sRGB") == 0) return ApplyGamut(Gamut::Srgb);
        if (_wcsicmp(argv[2], L"DisplayP3") == 0) return ApplyGamut(Gamut::DisplayP3);
        wprintf(L"unknown preset: %ls\n", argv[2]);
        return 1;
    }

    PrintUsage();
    return 1;
}
