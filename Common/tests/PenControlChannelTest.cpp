// Covers the tray→service control channel.
//
// 三个性质值得钉住：一份提交只被消费一次（否则托盘每选一次菜单，服务会在每个轮询周期重复
// 应用一次）；服务重启不重放上一次会话里的旧提交（那会覆盖掉这期间从别处改过的配置）；
// 并发提交下 Host 读不到撕裂的载荷。

#include "PenControlChannel.h"

#include <atomic>
#include <chrono>
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

// 钉死 Local。默认的 Auto 在提权运行时会建到或认领 Global，也就是运行中服务的那条通道，
// 测试的提交会真的送进服务。
PenControl::Scope OpenHostForTest(PenControl::Host& host) {
    Require(host.Open(PenControl::Scope::Local), "host opens in the session-local namespace");
    return PenControl::Scope::Local;
}

// 托盘不创建对象。服务不在时它必须打不开，而不是建出一个服务永远不会读的映射。
void TestClientFailsWithoutHost() {
    PenControl::Client client;
    Require(!client.Open(PenControl::Scope::Local), "client must not create the channel");
    Require(!client.IsOpen(), "a failed Open leaves the client closed");
}

void TestSubmitRoundTrip() {
    PenControl::Host host;
    const auto scope = OpenHostForTest(host);

    PenControl::Client client;
    Require(client.Open(scope), "client attaches to the host's channel");

    PenControl::Command command{};
    Require(!host.PollCommand(command), "a fresh channel has nothing to consume");

    Require(client.SubmitPenButtonMode(3), "submit succeeds");
    Require(host.PollCommand(command), "host sees the submission");
    Require(command.hasPenButtonMode, "the presence flag survives");
    Require(command.penButtonMode == 3, "the mode round-trips");
    Require(command.revision != 0, "the submission carries a revision");
    Require(command.submittedAtUnixMs != 0, "submit stamps a timestamp");

    // 同一份提交不能被消费两次：服务是在轮询循环里调用它的。
    Require(!host.PollCommand(command), "an unchanged revision is not re-delivered");

    Require(client.SubmitProviderLease(PenControl::ProviderLeaseCommand::AcquireOrRenew),
            "lease submit succeeds");
    Require(host.PollCommand(command), "host sees the provider lease");
    Require(command.hasProviderLease, "the lease presence flag survives");
    Require(command.providerLease == PenControl::ProviderLeaseCommand::AcquireOrRenew,
            "the lease command round-trips");
    Require(!command.hasPenButtonMode,
            "a lease-only command does not fabricate a pen mode");

    Require(client.SubmitInputSuppression(
                PenControl::InputSuppressionCommand::BeginOrRenew),
            "input suppression submit succeeds");
    Require(host.PollCommand(command), "host sees the input suppression command");
    Require(command.hasInputSuppression,
            "the input suppression presence flag survives");
    Require(command.inputSuppression ==
                PenControl::InputSuppressionCommand::BeginOrRenew,
            "the input suppression command round-trips");
    Require(!command.hasProviderLease,
            "an input-only command does not fabricate a provider lease");
}

// 键盘分离支持与 penButtonMode / providerLease / inputSuppression 共用同一份 Payload。
// 提交前者时不该顺手带上后三者的 has 标志——那会让服务在应用键盘设置的同时，把用户没碰过
// 的其它设置也当作「有新提交」改掉。
void TestKbdDetachSupportRoundTripDoesNotLeakOtherFields() {
    PenControl::Host host;
    const auto scope = OpenHostForTest(host);
    PenControl::Client client;
    Require(client.Open(scope), "client attaches");

    PenControl::Command command{};
    Require(client.SubmitKbdDetachSupport(PenControl::KbdDetachSupportCommand::Enable),
            "kbd detach support submit succeeds");
    Require(host.PollCommand(command), "host sees the submission");
    Require(command.hasKbdDetachSupport, "the presence flag survives");
    Require(command.kbdDetachSupport == PenControl::KbdDetachSupportCommand::Enable,
            "the command round-trips");
    Require(!command.hasPenButtonMode && !command.hasProviderLease &&
                !command.hasInputSuppression,
            "a kbd-only command does not fabricate the other shared-payload fields");

    Require(client.SubmitKbdDetachSupport(PenControl::KbdDetachSupportCommand::Disable),
            "disable submit succeeds");
    Require(host.PollCommand(command), "host sees the disable");
    Require(command.kbdDetachSupport == PenControl::KbdDetachSupportCommand::Disable,
            "disable round-trips");

    // None 表示「这条提交与键盘无关」，不应该产生任何东西可消费——Submit 本身要拒绝，而不是
    // 送出一份 has 标志为 false 的空提交，否则每次调用都会白白推进 revision 并唤醒 Host。
    const uint32_t revisionBefore = command.revision;
    Require(!client.SubmitKbdDetachSupport(PenControl::KbdDetachSupportCommand::None),
            "None is rejected rather than submitted");
    Require(!host.PollCommand(command), "no submission means nothing to poll");
    // PollCommand 在返回 false 时不碰 out 参数，所以 command 仍是上一次成功轮询时的值——
    // 借此确认 revision 真的没被推进过，而不是恰好又变回了同一个数。
    Require(command.revision == revisionBefore,
            "a rejected submit does not advance the revision Host last saw");
}

// 同一命名空间只能有一个活着的 Host。放行第二个，两个 Host 各自维护基线，同一份提交会被
// 应用两次；提权跑测试时那第二个 Host 就是测试自己，第一个是运行中的服务。
void TestSecondHostIsRejected() {
    PenControl::Host first;
    OpenHostForTest(first);

    PenControl::Host second;
    Require(!second.Open(PenControl::Scope::Local), "second host must not attach");
    Require(!second.IsOpen(), "a rejected host stays closed");

    // 闸门必须随第一个 Host 释放，否则一次测试运行会把后续用例全挡掉——重启后的服务同理。
    first.Close();
    PenControl::Host third;
    Require(third.Open(PenControl::Scope::Local), "the gate is reusable once released");
}

void TestSubmitEventWakesHost() {
    PenControl::Host host;
    const auto scope = OpenHostForTest(host);
    Require(host.HasEventChannel(), "Open() must actually create the named event");

    PenControl::Client client;
    Require(client.Open(scope), "client attaches");
    Require(client.SubmitPenButtonMode(2), "submit succeeds");

    // 自动重置事件在被等待者取走之前保持有信号，所以先发后等是可靠的。
    Require(host.WaitForSubmit(2000), "host wakes on the submit event");
}

// 服务重启后，上一次会话里早已应用过的提交不该再被应用一遍。Client 全程握着映射句柄，
// 对象在 Host 关闭后依然存活，载荷里的 revision 也就留在原处——正是真实的重启场景。
void TestNoReplayAfterHostRestart() {
    PenControl::Client client;
    PenControl::Command command{};

    {
        PenControl::Host host;
        const auto scope = OpenHostForTest(host);
        Require(client.Open(scope), "client attaches");
        Require(client.SubmitPenButtonMode(1), "submit succeeds");
        Require(host.PollCommand(command), "first host consumes the submission");
    }

    PenControl::Host restarted;
    Require(restarted.Open(PenControl::Scope::Local),
            "host reopens onto the surviving mapping");
    PenControl::Command replayed{};
    Require(!restarted.PollCommand(replayed), "an already-applied submission is not replayed");

    // 重启后的新提交照常送达，否则上面那条断言用「通道坏了」也能满足。
    Require(client.SubmitPenButtonMode(2), "submit after restart succeeds");
    Require(restarted.PollCommand(replayed), "a genuinely new submission still arrives");
    Require(replayed.penButtonMode == 2, "the new mode arrives");
}

// 并发提交下 Host 读到的每一份都自洽。
//
// 说明这条能证到什么程度：把 Submit 里的奇数窗口拿掉重跑，这个用例抓不到——序号仍然每次
// 提交加二，读者两侧的序号比对照样能否掉绝大多数撕裂，剩下的窗口对这份 24 字节的载荷来说
// 太窄，跑一秒半也撞不上一次。真正对撕裂敏感的是 PenStatusChannelTest 里的同名用例：那份
// 载荷 56 字节，同样的反向验证稳定失败。两条通道的 seqlock 是同一套写法。
//
// 所以这条守的是别的东西：revision 单调、一份提交只被消费一次、mode 与 revision 的对应
// 关系不错位。
void TestConcurrentSubmitsStayConsistent() {
    PenControl::Host host;
    const auto scope = OpenHostForTest(host);
    PenControl::Client client;
    Require(client.Open(scope), "client attaches");

    // 让 mode 由 revision 的奇偶决定。撕裂读会把一次提交的 revision 和另一次的 mode 拼在
    // 一起，那样两者的对应关系就破了；只断言「mode 是个合法值」的话，撕裂永远抓不到。
    // Client 是唯一写者，revision 每次恰好加一，所以这个关系是可预测的。
    Require(client.SubmitPenButtonMode(1), "seed a submission");
    PenControl::Command seed{};
    Require(host.PollCommand(seed), "host consumes the seed");

    const auto modeFor = [](uint32_t revision) -> uint8_t {
        return (revision % 2 == 0) ? 2 : 1;
    };

    std::atomic<bool> stop{false};
    std::thread producer([&] {
        uint32_t next = seed.revision;
        while (!stop.load(std::memory_order_relaxed)) {
            ++next;
            client.SubmitPenButtonMode(modeFor(next));
        }
    });

    // 按时长跑，不按次数：固定次数会在生产者线程真正启动之前就跑完——空轮一次几十纳秒，
    // 两万次不到一毫秒，而线程起来要更久，那时一次观测都没有，断言全部空过。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);

    int seen = 0, mismatched = 0, regressed = 0;
    uint32_t last = seed.revision;
    uint64_t lastStamp = seed.submittedAtUnixMs;
    while (std::chrono::steady_clock::now() < deadline) {
        PenControl::Command got{};
        if (!host.PollCommand(got)) continue;
        ++seen;
        if (!got.hasPenButtonMode || got.penButtonMode != modeFor(got.revision)) ++mismatched;
        // revision 与时间戳分处载荷的两端，都必须单调。撕裂把两次提交拼在一起时，被拼进来
        // 的那一半会比另一半旧。
        if (got.revision < last || got.submittedAtUnixMs < lastStamp) ++regressed;
        last = got.revision;
        lastStamp = got.submittedAtUnixMs;
    }
    stop.store(true, std::memory_order_relaxed);
    producer.join();

    Require(seen > 0, "at least some submissions were observed");
    Require(mismatched == 0, "no poll mixed fields from two different submissions");
    Require(regressed == 0, "revision and timestamp never go backwards");
}

} // namespace

int main() {
    try {
        TestClientFailsWithoutHost();
        TestSubmitRoundTrip();
        TestKbdDetachSupportRoundTripDoesNotLeakOtherFields();
        TestSecondHostIsRejected();
        TestSubmitEventWakesHost();
        TestNoReplayAfterHostRestart();
        TestConcurrentSubmitsStayConsistent();
        std::cout << "[TEST] Pen control channel tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
