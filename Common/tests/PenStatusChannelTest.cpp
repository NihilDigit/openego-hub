// Covers the read-only pen status broadcast the tray companion consumes.
//
// The channel exists because the service (session 0) cannot draw and the tray must not
// require elevation. Two properties matter enough to pin down: a reader must never
// observe a half-written snapshot, and "absent" must stay distinguishable from "present
// and false" — a pen that reports "not charging" has to read differently from a pen that
// never reported at all, or the panel would show a confident wrong state.

#include "PenStatusChannel.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 撕裂只在字段真的不同时才可观测。两份状态必须处处不同，所以每个字段都从 charging 派生
// 出两个取值，包括 modelId 和 modelName——原先两份都叫 "TestPen"、modelId 恒为 0，那 36
// 个字节里的撕裂无论发生多少次都读不出来。
//
// modelName 填满整个容量且逐字节不同：只写前几个字符的话，一次只覆盖了字符串尾部的撕裂
// 同样看不见。
PenStatus::State MakeState(uint8_t battery, bool charging, bool attached) {
    PenStatus::State s{};
    s.hasBatteryLevel = true;
    s.batteryLevel = battery;
    s.hasChargingState = true;
    s.charging = charging;
    s.hasDeviceAttached = true;
    s.deviceAttached = attached;
    s.hasStylusLink = true;
    s.stylusLinked = attached;
    s.hasPenButtonMode = true;
    s.penButtonMode = charging ? 3 : 1;
    s.hasEraserActive = true;
    s.eraserActive = charging;
    s.hasTouchProvider = true;
    s.touchProvider = charging ? PenStatus::TouchProviderState::EGoTouch
                               : PenStatus::TouchProviderState::Huawei;
    s.hasProviderError = true;
    s.providerError = charging ? 17 : 29;
    s.hasInputSuppressed = true;
    s.inputSuppressed = charging;
    s.hasKbdDetachSupport = true;
    s.kbdDetachSupport = charging;
    s.hasPenFirmware = true;
    s.hasPenHardware = true;
    s.hasPenSerial = true;
    s.hasKbdPresent = true;
    s.kbdPresent = attached;
    s.hasKbdDetached = true;
    s.kbdDetached = !attached;
    s.hasKbdBattery = true;
    s.kbdBatteryLevel = charging ? 73 : 41;
    s.hasKbdCharging = true;
    s.kbdCharging = charging;
    s.modelId = charging ? 0xA1B2C3D4u : 0x5E6F7081u;
    s.notificationSequence = charging ? 0x10203040u : 0x50607080u;
    s.notificationKind = charging ? PenStatus::NotificationKind::PenConnected
                                  : PenStatus::NotificationKind::PenDeviation;
    // 每个字符串字段填满到容量上限：这样一旦某个字段的容量改小、或写侧漏掉收尾，
    // 读回的内容就会与写入的不同，而不是恰好因为短字符串而看不出来。
    const auto fill = [charging](char* dst, size_t size, char even, char odd) {
        std::memset(dst, charging ? even : odd, size - 1);
        dst[size - 1] = '\0';
    };
    fill(s.modelName, sizeof(s.modelName), 'W', 'k');
    fill(s.penFirmware, sizeof(s.penFirmware), 'f', 'F');
    fill(s.penHardware, sizeof(s.penHardware), 'h', 'H');
    fill(s.penSerial, sizeof(s.penSerial), 's', 'S');
    fill(s.kbdModelName, sizeof(s.kbdModelName), 'm', 'M');
    fill(s.kbdFirmware, sizeof(s.kbdFirmware), 'b', 'B');
    return s;
}

// 除时间戳外逐字段比较。时间戳由 Publish 自己盖，调用方给的值不参与。
bool SameState(const PenStatus::State& a, const PenStatus::State& b) {
    return a.hasBatteryLevel == b.hasBatteryLevel &&
           a.batteryLevel == b.batteryLevel &&
           a.hasChargingState == b.hasChargingState &&
           a.charging == b.charging &&
           a.hasDeviceAttached == b.hasDeviceAttached &&
           a.deviceAttached == b.deviceAttached &&
           a.hasStylusLink == b.hasStylusLink &&
           a.stylusLinked == b.stylusLinked &&
           a.hasPenButtonMode == b.hasPenButtonMode &&
           a.penButtonMode == b.penButtonMode &&
           a.hasEraserActive == b.hasEraserActive &&
           a.eraserActive == b.eraserActive &&
           a.hasTouchProvider == b.hasTouchProvider &&
           a.touchProvider == b.touchProvider &&
           a.hasProviderError == b.hasProviderError &&
           a.providerError == b.providerError &&
           a.hasInputSuppressed == b.hasInputSuppressed &&
           a.inputSuppressed == b.inputSuppressed &&
           a.hasKbdDetachSupport == b.hasKbdDetachSupport &&
           a.kbdDetachSupport == b.kbdDetachSupport &&
           a.hasPenFirmware == b.hasPenFirmware &&
           a.hasPenHardware == b.hasPenHardware &&
           a.hasPenSerial == b.hasPenSerial &&
           a.hasKbdPresent == b.hasKbdPresent &&
           a.kbdPresent == b.kbdPresent &&
           a.hasKbdDetached == b.hasKbdDetached &&
           a.kbdDetached == b.kbdDetached &&
           a.hasKbdBattery == b.hasKbdBattery &&
           a.kbdBatteryLevel == b.kbdBatteryLevel &&
           a.hasKbdCharging == b.hasKbdCharging &&
           a.kbdCharging == b.kbdCharging &&
           a.modelId == b.modelId &&
           a.notificationSequence == b.notificationSequence &&
           a.notificationKind == b.notificationKind &&
           std::memcmp(a.modelName, b.modelName, sizeof(a.modelName)) == 0 &&
           std::memcmp(a.penFirmware, b.penFirmware, sizeof(a.penFirmware)) == 0 &&
           std::memcmp(a.penHardware, b.penHardware, sizeof(a.penHardware)) == 0 &&
           std::memcmp(a.penSerial, b.penSerial, sizeof(a.penSerial)) == 0 &&
           std::memcmp(a.kbdModelName, b.kbdModelName, sizeof(a.kbdModelName)) == 0 &&
           std::memcmp(a.kbdFirmware, b.kbdFirmware, sizeof(a.kbdFirmware)) == 0;
}

bool TryOpenWriter(PenStatus::Writer& w) {
    // 钉死 Local。Auto 会先试 Global，提权运行时那次会成功，于是测试的写者和运行中服务的
    // 写者共用一个 seqlock 互踩，还会把假状态灌进托盘真正在读的通道。
    return w.Open(PenStatus::Scope::Local);
}

void TestRoundTrip() {
    PenStatus::Writer writer;
    Require(TryOpenWriter(writer), "writer opens (Global, or Local when unprivileged)");

    PenStatus::Reader reader;
    Require(reader.Open(writer.OpenedScope()), "reader attaches to the published mapping");

    PenStatus::State published = MakeState(73, true, true);
    published.notificationKind = PenStatus::NotificationKind::KeyboardConnected;
    Require(writer.Publish(published), "publish succeeds");

    PenStatus::State got{};
    Require(reader.Read(got), "reader returns a consistent snapshot");
    Require(SameState(got, published), "every field round-trips");
    Require(got.updatedAtUnixMs != 0, "publish stamps a timestamp");
}

// 同一命名空间只能有一个写者。放行第二个会让两个 seqlock 写者互踩，而每一方都以为通道
// 是自己的；提权跑测试时那第二个写者就是测试自己，第一个是运行中的服务。
void TestSecondWriterIsRejected() {
    PenStatus::Writer first;
    Require(first.Open(PenStatus::Scope::Local), "first writer takes the namespace");

    PenStatus::Writer second;
    Require(!second.Open(PenStatus::Scope::Local), "second writer must not attach");

    // 前一个释放后名字重新可用，否则一次测试运行就会把后续用例全挡掉。
    first.Close();
    PenStatus::Writer third;
    Require(third.Open(PenStatus::Scope::Local), "namespace is reusable once released");
}

void TestAbsentIsNotFalse() {
    PenStatus::Writer writer;
    Require(TryOpenWriter(writer), "writer opens");
    PenStatus::Reader reader;
    Require(reader.Open(writer.OpenedScope()), "reader attaches");

    // A pen that reports "not charging" must not look like a pen that never reported.
    PenStatus::State reported{};
    reported.hasChargingState = true;
    reported.charging = false;
    Require(writer.Publish(reported), "publish a present-but-false state");

    PenStatus::State got{};
    Require(reader.Read(got), "read back");
    Require(got.hasChargingState, "presence bit survives");
    Require(!got.charging, "value stays false");
    Require(!got.hasBatteryLevel, "a field never published stays absent");
    Require(!got.hasPenButtonMode, "pen button mode stays absent until reported");
    Require(!got.hasEraserActive, "eraser state stays absent until reported");
    Require(!got.hasTouchProvider, "touch provider stays absent until reported");
    Require(!got.hasProviderError, "provider error stays absent until reported");
    Require(!got.hasInputSuppressed,
            "input suppression stays absent until reported");
    Require(!got.hasKbdDetachSupport,
            "keyboard detach support stays absent until reported");
}

// MCU 没答复过时两位都是 0，面板要把它显示成「未知」而不是「已关闭」——这两种状态在
// Payload 里都会把 kbdDetachSupport 这一位读成 0，能分开它们的只有 has 标志位。
void TestKbdDetachSupportUnknownVsDisabled() {
    PenStatus::Writer writer;
    Require(TryOpenWriter(writer), "writer opens");
    PenStatus::Reader reader;
    Require(reader.Open(writer.OpenedScope()), "reader attaches");

    PenStatus::State unknown{};
    Require(writer.Publish(unknown), "publish a state that never reported the flag");
    PenStatus::State got{};
    Require(reader.Read(got), "read back");
    Require(!got.hasKbdDetachSupport, "no report reads as unknown");
    Require(!got.kbdDetachSupport, "the value bit stays false alongside unknown");

    PenStatus::State disabled{};
    disabled.hasKbdDetachSupport = true;
    disabled.kbdDetachSupport = false;
    Require(writer.Publish(disabled), "publish an explicit disable");
    Require(reader.Read(got), "read back");
    Require(got.hasKbdDetachSupport, "an explicit disable is marked present");
    Require(!got.kbdDetachSupport, "and reads as off");

    PenStatus::State enabled{};
    enabled.hasKbdDetachSupport = true;
    enabled.kbdDetachSupport = true;
    Require(writer.Publish(enabled), "publish an explicit enable");
    Require(reader.Read(got), "read back");
    Require(got.hasKbdDetachSupport, "an explicit enable is marked present");
    Require(got.kbdDetachSupport, "and reads as on");
}

void TestEventChannelIsUsable() {
    PenStatus::Writer writer;
    Require(TryOpenWriter(writer), "writer opens");

    // Open() 在事件建不起来时仍然返回 true——共享内存可用，面板靠轮询照常工作。ACL 字符串
    // 里一个非法的权限记号曾让两个事件都没被创建，而且一直没人发现，直到有功能真的需要
    // 信号。这条断言就是补上那个缺口。
    Require(writer.HasEventChannel(), "Open() must actually create the named events");

    PenStatus::Reader reader;
    Require(reader.Open(writer.OpenedScope()), "reader attaches");
    Require(writer.SignalDoubleClick(), "signalling a gesture succeeds");
    // 自动重置事件在被等待者取走之前保持有信号，所以先发后等是可靠的。
    Require(reader.WaitForDoubleClick(2000), "reader observes the gesture");
}

void TestNoTornReadUnderConcurrentWrites() {
    PenStatus::Writer writer;
    Require(TryOpenWriter(writer), "writer opens");
    PenStatus::Reader reader;
    Require(reader.Open(writer.OpenedScope()), "reader attaches");

    // The writer alternates between two states that differ in every field. A torn read
    // would surface as a mixture the writer never published.
    //
    // Publish one of the two states first: the mapping starts zero-filled (and an earlier
    // case published an empty state into it), so without this the first reads legitimately
    // observe all-zero and would be misread as tearing.
    Require(writer.Publish(MakeState(5, false, false)), "seed a known state");

    std::atomic<bool> stop{false};
    std::thread producer([&] {
        bool flip = false;
        while (!stop.load(std::memory_order_relaxed)) {
            writer.Publish(flip ? MakeState(100, true, true) : MakeState(5, false, false));
            flip = !flip;
        }
    });

    const PenStatus::State full = MakeState(100, true, true);
    const PenStatus::State empty = MakeState(5, false, false);

    // 按时长跑，不按次数。撕裂是概率事件：要抓到它，读者的 memcpy 得正好跨在写者的
    // memcpy 中间。固定次数有两个问题——次数太少时可能在生产者线程启动之前就跑完（那样
    // mixed == 0 只说明通道压根没被并发写过），而即使跑满，几千次读也远不够采样。
    // 拿掉 Publish 里的奇数窗口做反向验证时，几千次读一次都抓不到，几十万次才稳定抓到。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);

    int sawFull = 0, sawEmpty = 0, mixed = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        PenStatus::State got{};
        if (!reader.Read(got)) continue;
        // 必须与其中一份逐字段完全相符。只看几个标志位的话，撕在 modelName 或 modelId 上
        // 的读会被判为合格。
        if (SameState(got, full))        ++sawFull;
        else if (SameState(got, empty))  ++sawEmpty;
        else                             ++mixed;
    }
    stop.store(true, std::memory_order_relaxed);
    producer.join();

    Require(sawFull > 0 && sawEmpty > 0, "the reader observed both published states");
    Require(mixed == 0, "no read mixed fields from two different publishes");
}

} // namespace

int main() {
    try {
        TestRoundTrip();
        TestSecondWriterIsRejected();
        TestAbsentIsNotFalse();
        TestKbdDetachSupportUnknownVsDisabled();
        TestEventChannelIsUsable();
        TestNoTornReadUnderConcurrentWrites();
        std::cout << "[TEST] Pen status channel tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
