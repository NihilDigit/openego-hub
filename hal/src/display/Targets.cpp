#include "Targets.h"

#include <cstdio>

namespace Gaokun::Display {

// 只挑真正支持 3D LUT 的显示器。不加这道过滤，对不支持的目标下发只会得到一个失败计数，
// 看不出是能力问题还是调用问题。
//
// 目标必须恰好是一个。DLUT 固件表里的校准是给这台机器的内屏测的，套到外接显示器上只是
// 把别人的面板特性硬灌进去，颜色只会更不准。而 qdcmlib 不报告哪一个是内置面板，也没有
// 能和 Windows 的显示器对应起来的标识，所以无法在多个目标里挑出内屏——此时宁可什么都不做
// 并说明原因，也不要赌一个。原厂工具同样在这里直接放弃。
bool EnumTargets(Qdcm &qdcm, std::vector<uint32_t> &targets) {
    std::vector<uint32_t> ids;
    if (!qdcm.EnumDisplays(ids)) {
        wprintf(L"display enumeration failed\n");
        return false;
    }
    targets.clear();
    for (uint32_t id : ids) {
        int32_t caps = 0;
        if (qdcm.QueryCaps(id, caps) && (caps & kCapLut3d) != 0) targets.push_back(id);
    }
    if (targets.empty()) {
        wprintf(L"no display reports 3D LUT support\n");
        return false;
    }
    if (targets.size() > 1) {
        wprintf(L"%zu displays report 3D LUT support; cannot tell which one is the built-in\n"
                L"panel, and the factory calibration only applies to that one. Disconnect the\n"
                L"external display and try again.\n",
                targets.size());
        return false;
    }
    return true;
}

} // namespace Gaokun::Display
