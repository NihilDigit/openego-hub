// TouchSizeCalculator 此前用的换算在 signalSum 达到 368 时就饱和到 15mm，而真实接触的
// signalSum 普遍在几百到几万，于是 sizeMm 实际是个常量：轻搭的指尖和整个掌根读数相同。
// 那段代码没有任何测试覆盖，所以这里断言的是「跨真实取值范围保持可分辨」这个性质，而不是
// 复述换算公式——公式换一个写法仍应通过，饱和则必须失败。

#include "TouchFrameTypes.h"
#include "TouchSolver/ContactExtractor.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<Solvers::TouchContact> MakeContacts(std::initializer_list<std::pair<int, int>> areaAndSignal) {
    std::vector<Solvers::TouchContact> contacts;
    for (const auto& [areaCells, signalSum] : areaAndSignal) {
        Solvers::TouchContact c;
        c.areaCells = areaCells;
        c.signalSum = signalSum;
        contacts.push_back(c);
    }
    return contacts;
}

// 真实接触覆盖的信号范围，从最弱的一触到掌根。
void TestSizeStaysDistinguishableAcrossRealSignalRange() {
    Solvers::Touch::ContactExtractor::TouchSizeCalculator calc;
    auto contacts = MakeContacts({{2, 60}, {6, 500}, {12, 1200}, {30, 20000}, {60, 80000}});
    calc.Process(contacts);

    for (size_t i = 1; i < contacts.size(); ++i) {
        Require(contacts[i].sizeMm > contacts[i - 1].sizeMm,
                "sizeMm must keep increasing across the real signal range");
    }
    // 掌与最弱的一触必须差出一个明显的倍数，否则任何 sizeMm 阈值都无从设起。
    Require(contacts.back().sizeMm > contacts.front().sizeMm * 3.0f,
            "palm-scale signal should read several times a faint touch");
}

// areaMm2 是测量值：只随格数变化，与信号强度无关。用力按同一根手指不应让它变大。
void TestAreaMm2TracksAreaNotPressure() {
    Solvers::Touch::ContactExtractor::TouchSizeCalculator calc;
    auto contacts = MakeContacts({{6, 500}, {6, 40000}, {24, 500}});
    calc.Process(contacts);

    Require(contacts[0].areaMm2 == contacts[1].areaMm2,
            "areaMm2 must not change with signal strength");
    Require(contacts[2].areaMm2 > contacts[0].areaMm2 * 3.5f,
            "areaMm2 must scale with the contact's cell count");

    // 网格不是方形:列 4.5 mm、行 4.125 mm。按单值算会把面积高估 9%。
    const float cellMm2 = calc.m_pixelPitchMm * calc.m_rowPitchMm;
    Require(contacts[0].areaMm2 == 6.0f * cellMm2, "areaMm2 must be cells times the two pitches");
    Require(calc.m_pixelPitchMm != calc.m_rowPitchMm, "the grid is not square; keep the two apart");
}

// m_pixelPitchMm 此前绑了配置却没有任何读取方，改动它对输出毫无影响。
void TestPixelPitchAffectsAreaMm2() {
    Solvers::Touch::ContactExtractor::TouchSizeCalculator calc;
    auto atDefault = MakeContacts({{10, 1200}});
    calc.Process(atDefault);

    calc.m_pixelPitchMm *= 2.0f;
    auto atDoublePitch = MakeContacts({{10, 1200}});
    calc.Process(atDoublePitch);

    Require(atDoublePitch[0].areaMm2 == atDefault[0].areaMm2 * 2.0f,
            "doubling the column pitch must double areaMm2");

    // 两个方向各自生效,不是共用一个数。
    calc.m_rowPitchMm *= 2.0f;
    auto atBothDoubled = MakeContacts({{10, 1200}});
    calc.Process(atBothDoubled);
    Require(atBothDoubled[0].areaMm2 == atDefault[0].areaMm2 * 4.0f,
            "doubling both pitches must quadruple areaMm2");
}

} // namespace

int main() {
    try {
        TestSizeStaysDistinguishableAcrossRealSignalRange();
        TestAreaMm2TracksAreaNotPressure();
        TestPixelPitchAffectsAreaMm2();
        std::cout << "[TEST] Touch contact size tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
