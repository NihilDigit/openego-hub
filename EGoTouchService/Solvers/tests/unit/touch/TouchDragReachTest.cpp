#include "TouchSolver/TouchGestureStateMachine.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

using Solvers::HeatmapFrame;
using Solvers::TouchContact;
using Solvers::Touch::TouchGestureStateMachine;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

TouchContact MakeContact(float x, float y, int state) {
    TouchContact c;
    c.id = 1;
    c.x = x;
    c.y = y;
    c.state = state;
    c.areaCells = 6;
    c.signalSum = 1200;
    c.sizeMm = 2.0f;
    c.isReported = true;
    c.lifeFlags = Solvers::TouchLifeMapped;
    return c;
}

// 一次拖动：从 startX 出发，每帧向左走 step 格，直到指尖落到 targetX。
// 返回最后一帧的输出坐标。
float DragLeftAndReport(TouchGestureStateMachine& gesture,
                        float startX, float targetX, float step, int maxFrames) {
    HeatmapFrame down;
    down.touch.output.contacts.push_back(MakeContact(startX, 20.0f, Solvers::TouchStateDown));
    gesture.Process(down);

    float x = startX;
    float lastOut = down.touch.output.contacts[0].x;
    for (int i = 0; i < maxFrames; ++i) {
        x = std::max(targetX, x - step);
        HeatmapFrame move;
        move.touch.output.contacts.push_back(MakeContact(x, 20.0f, Solvers::TouchStateMove));
        gesture.Process(move);
        lastOut = move.touch.output.contacts[0].x;
    }
    return lastOut;
}

// 指尖压到面板边界并停住，输出必须也到达边界。拖动偏移如果一直保留，输出会永远差
// 一个 m_dragThreshold，指针够不到起手方向那一侧的屏幕边缘——这正是「选区拖不到边」。
void TestDragReachesThePanelEdge() {
    TouchGestureStateMachine gesture;
    const float out = DragLeftAndReport(gesture, 20.0f, 0.0f, 1.0f, 60);
    Require(std::fabs(out) < 0.05f,
            "a drag that pins the finger to the edge must report the edge");
}

// 越界那一帧输出恰好落后指尖一个阈值。偏移存在的唯一理由就是这个：输出在阈值之前钉在
// anchor，越界时若直接给指尖真实位置，跳变正好是一个阈值。消解不能把这一帧的回退也消掉。
void TestDragStartHoldsBackOneThreshold() {
    TouchGestureStateMachine gesture;

    HeatmapFrame down;
    down.touch.output.contacts.push_back(MakeContact(20.0f, 20.0f, Solvers::TouchStateDown));
    gesture.Process(down);

    const float fingerX = 20.0f - gesture.m_dragThreshold * 3.0f;
    HeatmapFrame cross;
    cross.touch.output.contacts.push_back(MakeContact(fingerX, 20.0f, Solvers::TouchStateMove));
    gesture.Process(cross);
    const float crossX = cross.touch.output.contacts[0].x;

    Require(std::fabs((crossX - fingerX) - gesture.m_dragThreshold) < 0.01f,
            "the crossing frame should trail the fingertip by exactly one threshold");
}

// 消解是有限步的：拖够帧数之后输出等于指尖，不再残留偏移。
void TestOffsetIsFullyConsumed() {
    TouchGestureStateMachine gesture;

    HeatmapFrame down;
    down.touch.output.contacts.push_back(MakeContact(20.0f, 20.0f, Solvers::TouchStateDown));
    gesture.Process(down);

    float x = 20.0f;
    for (int i = 0; i < gesture.m_dragOffsetDecayFrames + 4; ++i) {
        x -= 0.5f;
        HeatmapFrame move;
        move.touch.output.contacts.push_back(MakeContact(x, 20.0f, Solvers::TouchStateMove));
        gesture.Process(move);
        if (i == gesture.m_dragOffsetDecayFrames + 3) {
            Require(std::fabs(move.touch.output.contacts[0].x - x) < 0.01f,
                    "after the decay window the output should equal the fingertip");
        }
    }
}

} // namespace

int main() {
    try {
        TestDragReachesThePanelEdge();
        TestDragStartHoldsBackOneThreshold();
        TestOffsetIsFullyConsumed();
        std::cout << "[TEST] Touch drag reach tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
