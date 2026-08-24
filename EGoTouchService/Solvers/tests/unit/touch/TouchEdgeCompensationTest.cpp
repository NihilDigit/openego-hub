#include "TouchSolver/EdgeCompensation.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void RequireNear(float actual, float expected, float epsilon, const char* message) {
    if (std::fabs(actual - expected) > epsilon) {
        throw std::runtime_error(message);
    }
}

Solvers::ZoneEdgeInfo MakeEdgeInfo(uint8_t minCol, uint8_t maxCol,
                                   uint8_t minRow, uint8_t maxRow) {
    Solvers::ZoneEdgeInfo info;
    info.minCol = minCol;
    info.maxCol = maxCol;
    info.minRow = minRow;
    info.maxRow = maxRow;
    Solvers::TZ_GetEdgeTouchedFlag(info);
    return info;
}

// 守的是「这些数字确实是从本机面板 W273AS2700 的参数表里读出来的」，不是「默认值没被
// 人改过」。四张表在 flash+0x840 / +0x851 / +0x862 / +0x873，Dim1 两张上界 224、
// Dim2 两张 96；过渡常量在 CTD_ECGetFinalOffset 里硬编码为 1.0 格与 0.25 格。
// 取表方式见 docs/tsacore_ground_truth.md。
void TestDefaultProfilesMatchVendorTable() {
    Solvers::Touch::EdgeCompensator compensator;
    RequireNear(compensator.m_ecStrength, 1.0f, 0.0001f, "EC strength should be full");
    RequireNear(compensator.m_ecFullCompRange, 1.0f, 0.0001f,
                "EC full range should be the vendor's one cell");
    RequireNear(compensator.m_ecBlendRange, 0.25f, 0.0001f,
                "EC blend range should be the vendor's quarter cell");
    for (int edge = 0; edge < 4; ++edge) {
        Require(compensator.m_profiles[edge].numSegments == 1,
                "this panel's profile has a single segment");
        Require(compensator.m_profiles[edge].segments[0].touchSizeThreshold == 7,
                "segment size threshold should be the vendor's");
        Require(compensator.m_profiles[edge].segments[0].lutIdxLow == 16,
                "segment low LUT index should be the vendor's");
    }
    // 上界按轴分，不按远近端分——这是移植期把四份写成同一个占位符时丢掉的区别。
    Require(compensator.m_profiles[0].segments[0].lutIdxHigh == 224, "Dim1 near high LUT");
    Require(compensator.m_profiles[1].segments[0].lutIdxHigh == 224, "Dim1 far high LUT");
    Require(compensator.m_profiles[2].segments[0].lutIdxHigh == 96, "Dim2 near high LUT");
    Require(compensator.m_profiles[3].segments[0].lutIdxHigh == 96, "Dim2 far high LUT");
}

// grip 比例是边缘补偿的输入，它决定接触点被放到最外一格里的哪个位置。这里守的是
// 「比例越大，补偿把点推得越靠边」这条单调性——它一旦反了，表现就是够不到边或者
// 越过边界，而两者都不会让别的测试变红。
void TestGripRatioDrivesCompensation() {
    using Solvers::Touch::TZ_CentroidGripRatio;
    Require(TZ_CentroidGripRatio(0, 100) == 0, "no outer signal means no grip");
    Require(TZ_CentroidGripRatio(100, 0) == 0xFF, "outer-only means the ratio saturates");
    Require(TZ_CentroidGripRatio(100, 100) == 16, "equal sums give sixteen");
    Require(TZ_CentroidGripRatio(100, 200) == 8, "half as much outer gives eight");
    Require(TZ_CentroidGripRatio(10000, 100) == 0xFF, "the ratio clamps to a byte");

    const Solvers::ECProfile& prof = Solvers::Touch::g_defaultECProfiles[0];
    float prevDistance = 1e9f;
    for (uint8_t ratio : {uint8_t{20}, uint8_t{60}, uint8_t{120}, uint8_t{220}}) {
        const int off = Solvers::Touch::ECGetOffset(ratio, 8, prof);
        const int compOff = 256 - off;
        const int distance = Solvers::Touch::ECGetFinalOffset(0, compOff, 256, 64);
        Require(static_cast<float>(distance) < prevDistance,
                "a stronger grip ratio must place the contact closer to the boundary");
        prevDistance = static_cast<float>(distance);
    }
    Require(prevDistance < 64.0f,
            "at a high grip ratio the contact should end up well inside a quarter cell");
}

void TestDim1NearCorrectionMetadata() {
    Solvers::Touch::EdgeCompensator compensator;
    std::vector<Solvers::TouchContact> contacts(1);
    contacts[0].x = 0.5f;
    contacts[0].y = 20.0f;
    contacts[0].state = Solvers::TouchStateDown;
    // 管线里这两个字段由 ZoneExpander 在接触点生成时写好，这里照做。
    contacts[0].matchXCells = contacts[0].x;
    contacts[0].matchYCells = contacts[0].y;

    std::vector<Solvers::ZoneEdgeInfo> edgeInfos(1, MakeEdgeInfo(0, 2, 18, 22));
    edgeInfos[0].colEdgeWidth = 3;

    compensator.Process(contacts, edgeInfos, Solvers::EdgeBounds{});

    Require(contacts[0].isEdge, "near-edge contact should remain marked as edge");
    Require((contacts[0].edgeFlags & 0x20) != 0, "edge flags should include boundary touch");
    Require((contacts[0].centroidEdgeFlags & 0x01) != 0, "centroid flags should include Dim1 near edge");
    Require((contacts[0].ecFlags & 0x100) != 0, "Dim1 correction flag should be set");
    Require(contacts[0].ecWidthX == 3, "edge width should come from threshold-scanned column edge width");
    // 补偿必须只改上报坐标。跟踪级按 matchXCells 做帧间配对，补偿量随 grip 比例
    // 逐帧抖动，一旦被写进这里，匹配器就会看到并不存在的位移，笔画在边缘断开。
    RequireNear(contacts[0].matchXCells, 0.5f, 0.0001f,
                "compensation must leave the matching coordinate untouched");
    Require(contacts[0].x != contacts[0].matchXCells,
            "this fixture is meant to be compensated; if the two agree the test proves nothing");
    Require(contacts[0].edgeDistXCells > 0.0f, "corrected X edge distance should be populated");
    Require(std::fabs(contacts[0].x - 0.5f) > 0.0001f, "X coordinate should be corrected");
}

void TestDim2FarCorrectionMetadata() {
    Solvers::Touch::EdgeCompensator compensator;
    std::vector<Solvers::TouchContact> contacts(1);
    contacts[0].x = 30.0f;
    contacts[0].y = 39.5f;
    contacts[0].state = Solvers::TouchStateDown;
    contacts[0].matchXCells = contacts[0].x;
    contacts[0].matchYCells = contacts[0].y;

    std::vector<Solvers::ZoneEdgeInfo> edgeInfos(1, MakeEdgeInfo(28, 32, 37, 39));
    edgeInfos[0].rowEdgeWidth = 4;

    compensator.Process(contacts, edgeInfos, Solvers::EdgeBounds{});

    Require((contacts[0].centroidEdgeFlags & 0x08) != 0, "centroid flags should include Dim2 far edge");
    Require((contacts[0].ecFlags & 0x200) != 0, "Dim2 correction flag should be set");
    Require(contacts[0].ecWidthY == 4, "Y edge width should come from threshold-scanned row edge width");
    RequireNear(contacts[0].matchYCells, 39.5f, 0.0001f,
                "compensation must leave the matching coordinate untouched");
    Require(contacts[0].y != contacts[0].matchYCells,
            "this fixture is meant to be compensated; if the two agree the test proves nothing");
    Require(contacts[0].edgeDistYCells > 0.0f, "corrected Y edge distance should be populated");
    Require(std::fabs(contacts[0].y - 39.5f) > 0.0001f, "Y coordinate should be corrected");
}

void TestOnlyOutermostCellTriggersCorrection() {
    Solvers::Touch::EdgeCompensator compensator;
    compensator.m_ecFullCompRange = 4.0f;
    std::vector<Solvers::TouchContact> contacts(2);
    contacts[0].x = 0.5f;
    contacts[0].y = 20.0f;
    contacts[1].x = 1.5f;
    contacts[1].y = 20.0f;

    std::vector<Solvers::ZoneEdgeInfo> edgeInfos(2, MakeEdgeInfo(0, 2, 18, 22));
    compensator.Process(contacts, edgeInfos, Solvers::EdgeBounds{});

    Require((contacts[0].ecFlags & 0x100) != 0, "outermost cell should receive Dim1 correction");
    Require((contacts[1].centroidEdgeFlags & 0x01) == 0, "second cell should not receive Dim1 edge direction");
    Require((contacts[1].ecFlags & 0x100) == 0, "second cell should not receive Dim1 correction");
    RequireNear(contacts[1].x, 1.5f, 0.0001f, "second cell X should not change");
}

// 远端与近端必须都能触发。远端判据曾要求质心越过最后一格的中心，而质心是 0..N-1 号格
// 的加权平均，到不了那里，右边和下边的补偿于是一次也没生效过。原实现按峰的格索引判，
// near 是 index==0、far 是 index==N-1，严格镜像。
void TestFarEdgeIsCompensatedLikeNearEdge() {
    std::vector<Solvers::ZoneEdgeInfo> nearInfos(1, MakeEdgeInfo(0, 2, 18, 22));
    Solvers::Touch::EdgeCompensator nearComp;
    std::vector<Solvers::TouchContact> nearContacts(1);
    nearContacts[0].x = 0.5f;
    nearContacts[0].y = 20.0f;
    nearComp.Process(nearContacts, nearInfos, Solvers::EdgeBounds{});

    std::vector<Solvers::ZoneEdgeInfo> farInfos(1, MakeEdgeInfo(57, 59, 18, 22));
    Solvers::Touch::EdgeCompensator farComp;
    std::vector<Solvers::TouchContact> farContacts(1);
    farContacts[0].x = 58.5f;
    farContacts[0].y = 20.0f;
    farComp.Process(farContacts, farInfos, Solvers::EdgeBounds{});

    // 断言停在标志这一层。补偿量还取决于 EdgeBounds 与两个过渡常量，那两处各自
    // 另有问题（bounds 从不写入、常量与 TSACore 真值不符），不该混进这条测试。
    Require((nearContacts[0].centroidEdgeFlags & 0x01) != 0,
            "near edge should raise its centroid flag");
    Require((farContacts[0].centroidEdgeFlags & 0x02) != 0,
            "far edge should raise its centroid flag too");
}

// 补偿只对贴边的接触点生效，屏幕里侧的坐标不该被动到，否则手感会变成
// 「靠近边缘就往边上飘」。
void TestWidenedBlendDoesNotReachInward() {
    Solvers::Touch::EdgeCompensator compensator;
    std::vector<Solvers::TouchContact> contacts(1);
    contacts[0].x = 3.0f;
    contacts[0].y = 20.0f;

    std::vector<Solvers::ZoneEdgeInfo> edgeInfos(1, MakeEdgeInfo(2, 4, 18, 22));
    compensator.Process(contacts, edgeInfos, Solvers::EdgeBounds{});

    RequireNear(contacts[0].x, 3.0f, 0.0001f,
                "a contact three cells in should not be moved by edge compensation");
}

void TestStrengthScalesCorrectionAmplitude() {
    std::vector<Solvers::ZoneEdgeInfo> edgeInfos(1, MakeEdgeInfo(0, 2, 18, 22));

    Solvers::Touch::EdgeCompensator fullCompensator;
    fullCompensator.m_ecStrength = 1.0f;
    std::vector<Solvers::TouchContact> fullContacts(1);
    fullContacts[0].x = 0.5f;
    fullContacts[0].y = 20.0f;
    fullCompensator.Process(fullContacts, edgeInfos, Solvers::EdgeBounds{});

    Solvers::Touch::EdgeCompensator reducedCompensator;
    reducedCompensator.m_ecStrength = 0.35f;
    std::vector<Solvers::TouchContact> reducedContacts(1);
    reducedContacts[0].x = 0.5f;
    reducedContacts[0].y = 20.0f;
    reducedCompensator.Process(reducedContacts, edgeInfos, Solvers::EdgeBounds{});

    const float fullDelta = std::fabs(fullContacts[0].x - 0.5f);
    const float reducedDelta = std::fabs(reducedContacts[0].x - 0.5f);
    Require(reducedDelta > 0.0f, "reduced EC strength should still apply correction");
    Require(reducedDelta < fullDelta, "reduced EC strength should reduce correction amplitude");
}

void TestEdgeWidthScansThreshold() {
    Solvers::ZoneEdgeInfo info;
    int16_t heatmap[40][60] = {};
    for (int row = 8; row <= 12; ++row) {
        heatmap[row][0] = 320;
    }
    Solvers::TZ_UpdateEdgeInfo(info, 320, 0, 10, 7);
    Solvers::TZ_UpdateEdgeInfo(info, 320, 0, 11, 7);
    Solvers::TZ_GetEdgeWidth(info, heatmap, 300);

    Require(info.colEdgeWidth == 5, "column edge width should scan contiguous cells above threshold");

    Solvers::ZoneEdgeInfo farInfo;
    int16_t farHeatmap[40][60] = {};
    for (int row = 0; row <= 2; ++row) {
        farHeatmap[row][59] = 320;
    }
    Solvers::TZ_UpdateEdgeInfo(farInfo, 320, 59, 0, 7);
    Solvers::TZ_GetEdgeWidth(farInfo, farHeatmap, 300);
    Require(farInfo.colEdgeWidth == 3, "far column edge width should initialize max-side scan state");
}

void TestNonEdgeContactIsUnchanged() {
    Solvers::Touch::EdgeCompensator compensator;
    std::vector<Solvers::TouchContact> contacts(1);
    contacts[0].x = 20.5f;
    contacts[0].y = 11.5f;

    std::vector<Solvers::ZoneEdgeInfo> edgeInfos(1, MakeEdgeInfo(20, 22, 10, 12));
    compensator.Process(contacts, edgeInfos, Solvers::EdgeBounds{});

    Require(!contacts[0].isEdge, "non-edge contact should not be marked as edge");
    Require(contacts[0].ecFlags == 0, "non-edge contact should not receive EC flags");
    RequireNear(contacts[0].x, 20.5f, 0.0001f, "non-edge X should not change");
    RequireNear(contacts[0].y, 11.5f, 0.0001f, "non-edge Y should not change");
}

// 两个方向都要断言。只断言「修好的点不被拒」的话，EdgeRejector 变成空实现也照样通过——
// 它此前正是空实现（判定条件恒真、写入被 tracker 覆盖），而这条测试一直是绿的。
void TestEdgeRejectorFlagsOnlyUncorrectedContacts() {
    std::vector<Solvers::ZoneEdgeInfo> edgeInfos(1, MakeEdgeInfo(0, 2, 18, 22));
    edgeInfos[0].colEdgeWidth = 3;

    auto makePinnedContact = []() {
        std::vector<Solvers::TouchContact> contacts(1);
        contacts[0].x = 0.5f;
        contacts[0].y = 20.0f;
        return contacts;
    };

    Solvers::Touch::EdgeRejector rejector;

    auto corrected = makePinnedContact();
    Solvers::Touch::EdgeCompensator compensator;
    compensator.Process(corrected, edgeInfos, Solvers::EdgeBounds{});
    rejector.Process(corrected, edgeInfos, Solvers::EdgeBounds{});
    Require(!corrected[0].edgeRejected, "corrected edge contact should not be rejected");

    // 关掉 EC 复现「坐标钉在边界上、补偿没能把它拉开」的状态，这正是拒绝要针对的情况。
    auto uncorrected = makePinnedContact();
    Solvers::Touch::EdgeCompensator disabled;
    disabled.m_enabled = false;
    disabled.Process(uncorrected, edgeInfos, Solvers::EdgeBounds{});
    rejector.Process(uncorrected, edgeInfos, Solvers::EdgeBounds{});
    Require(uncorrected[0].edgeRejected, "uncorrected pinned edge contact should be rejected");
}

// 上一帧的拒绝结论不能顺着 FixedVector 复用的槽位漏到下一帧。
void TestEdgeRejectorClearsStaleFlag() {
    std::vector<Solvers::ZoneEdgeInfo> edgeInfos(1, MakeEdgeInfo(20, 22, 18, 22));
    std::vector<Solvers::TouchContact> contacts(1);
    contacts[0].x = 20.5f;
    contacts[0].y = 20.0f;
    contacts[0].edgeRejected = true;

    Solvers::Touch::EdgeRejector rejector;
    rejector.Process(contacts, edgeInfos, Solvers::EdgeBounds{});
    Require(!contacts[0].edgeRejected, "non-edge contact should clear a stale rejection");

    contacts[0].edgeRejected = true;
    rejector.m_enabled = false;
    rejector.Process(contacts, edgeInfos, Solvers::EdgeBounds{});
    Require(!contacts[0].edgeRejected, "disabled rejector should clear a stale rejection");
}

} // namespace

int main() {
    try {
        TestDefaultProfilesMatchVendorTable();
        TestGripRatioDrivesCompensation();
        TestDim1NearCorrectionMetadata();
        TestDim2FarCorrectionMetadata();
        TestOnlyOutermostCellTriggersCorrection();
        TestFarEdgeIsCompensatedLikeNearEdge();
        TestWidenedBlendDoesNotReachInward();
        TestStrengthScalesCorrectionAmplitude();
        TestEdgeWidthScansThreshold();
        TestNonEdgeContactIsUnchanged();
        TestEdgeRejectorFlagsOnlyUncorrectedContacts();
        TestEdgeRejectorClearsStaleFlag();
        std::cout << "[TEST] Touch edge compensation tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
