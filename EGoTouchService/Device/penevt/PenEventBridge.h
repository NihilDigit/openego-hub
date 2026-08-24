#pragma once

#include "btmcu/BtHidChannel.h"
#include "btmcu/PenUsbInitSession.h"
#include "btmcu/PenUsbTypes.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>

namespace Himax::Pen {

/// PenEventBridge — BT MCU 事件通道 (col00)
///
/// 负责：
///   - USB HID col00 设备发现（SetupDi GUID 枚举）
///   - 初始握手 (0x7101 + 0x7701 + 0x7701)
///   - 事件帧读取 + 自动 ACK (0x8001)
///   - 0x7B InitParam → 0x7D01 回显
///   - 上层事件回调分发
class PenEventBridge : public BtHidChannel {
public:
    PenEventBridge() = default;
    ~PenEventBridge() override;

    /// 向 BT MCU 发送 SetScanMode 命令，通知笔切换扫描频率。
    /// 对应原厂 THP_Service::BtPen_SendPacket + ApDaemon::SetScanMode 的联合逻辑。
    /// @param freq1  新的 TX1 频率码
    /// @param freq2  新的 TX2 频率码
    /// @param mode   0=正常扫描, 3=检测模式（默认 0）
    /// @return true if packet was sent successfully
    bool SendScanMode(uint8_t freq1, uint8_t freq2, uint8_t mode = 0);

    /// 发送原厂固定的 0x7D01 初始化参数，不使用动态扫描模式参数。
    bool SendFactoryInitProtocolParams();

    /// 设置 MCU 事件回调（线程安全）。回调从事件读取线程发起，不得长时间阻塞。
    void SetEventCallback(PenEventCallback cb);
    /// 设置状态事件句柄（用于通知 App 侧刷新状态）
    void SetNotifyEvent(NativeEventHandle h) {
        m_notifyEvent.store(h, std::memory_order_release);
    }

    /// 手动触发握手（0x7101 + 0x7701 + 0x7701），通常无需手动调用。
    void RunHandshake();

    bool SendQueryPenModule();
    bool SendQuerySerialNumber();
    bool SendQueryHardwareVersion();
    bool SendQueryFirmwareVersion();
    bool SendQueryPenStatus();
    bool SendFirstMcuStatusQuery();
    bool SendSecondMcuStatusQuery();
    bool SendPairInfoSet(uint8_t value);

    /// 主动查询电量。MCU 不主动播报电量，只在收到 0x0801 后回一条 BATTERY_STATUS。
    bool SendQueryPenBattery();

    /// 主动查询一次充电状态。仅用于通道刚建立时补齐初始快照；后续仍由 0x09 广播驱动，
    /// 不得放进周期轮询，否则会重新引入查询应答与广播无法区分导致的状态漂移。
    bool SendQueryChargingStatus();

    // ── 键盘「分离后无线连接」开关 ────────────────────────────────────────
    // 与 pen 走同一个 USB 端点，按子系统 ID 分流，详见 docs/KBDMCU_PROTOCOL.md。

    using KbdDetachSupportCallback = std::function<void(bool enabled)>;

    /// 键盘状态快照。与 detach support 分开：那是一个设置，这些是状态。
    /// present 为真同时意味着「这是华为一体化键盘」——第三方键盘不注册这个接口，
    /// 根本走不到 MCU 键盘子系统上来。详见 docs/KEYBOARD_IDENTITY.md。
    struct KbdState {
        bool hasPresent = false;
        bool present = false;
        bool hasDetached = false;
        bool detached = false;     // true = 已与主机分离
        bool hasBattery = false;
        uint8_t battery = 0;       // 百分比
        bool hasCharging = false;
        bool charging = false;
        std::string firmware;      // 空表示尚未答复
        std::string modelName;     // 由固件串前缀推出，无法判定时为空
    };

    using KbdStateCallback = std::function<void(const KbdState& state)>;

    /// 只从已经建立的基线识别连接边沿。初次查询把当前状态填进来时不得弹窗；连接状态和
    /// 吸附状态都可能表达同一次物理动作，调用方还需做短时去重。
    [[nodiscard]] static constexpr bool IsKbdConnectionEdge(
            const KbdState& before, const KbdState& after) noexcept {
        const bool becamePresent =
            before.hasPresent && !before.present && after.hasPresent && after.present;
        const bool becameAttached =
            before.hasDetached && before.detached &&
            after.hasDetached && !after.detached;
        return becamePresent || becameAttached;
    }

    /// 设置 detach support 状态回调（线程安全）。回调从读线程发起，不得长时间阻塞。
    void SetKbdDetachSupportCallback(KbdDetachSupportCallback cb);

    /// 查询当前是否允许键盘分离后无线连接。应答从读线程异步到达，经回调与缓存更新。
    bool SendKbdDetachSupportGet();

    /// 设置该开关。Set 的应答会被 MCU 读线程过滤掉，所以内部随即重发一次 Get 确认实际值。
    bool SendKbdDetachSupportSet(bool enable);

    /// 已知的最近一次 detach support 状态；未收到过应答时为 nullopt。
    [[nodiscard]] std::optional<bool> KbdDetachSupport() const;

    /// 设置键盘状态回调（线程安全）。回调从读线程发起，不得长时间阻塞。
    void SetKbdStateCallback(KbdStateCallback cb);

    /// 查询键盘连接、分离、电量、充电与固件状态。应答异步到达，经回调与缓存更新。
    bool SendKbdStatusQueries();

    /// 最近一次已知的键盘状态。
    [[nodiscard]] KbdState KbdStateSnapshot() const;

protected:
    std::optional<std::wstring> FindDevicePath() override;
    void OnConnected() override;
    void OnPacketReceived(std::span<const uint8_t> packet) override;

    void OnIdleTick() override;
    const char* ChannelName() const override { return "PenEventBridge"; }

private:
    static int GetAckCode(uint8_t eventCode);

    bool SendRawPacket(std::span<const uint8_t> pkt);
    // 非 pen 子系统的帧（byte[4] != 0x01）的入口。当前只消费 detach support 应答，其余
    // 键盘帧记 debug 后丢弃。
    void HandleKeyboardFrame(std::span<const uint8_t> packet);
    void ApplyKbdPresent(bool present);
    void TickKbdAbsentDebounce();
    void NotifyKbdState();
    void MaybePollBattery();
    void MaybeRetryPenModuleQuery();
    void SendAck(uint8_t eventCode, uint8_t ackCode);
    void ExecuteInitAction(PenUsbInitAction action);
    void AdvanceSessionFromEvent(uint8_t eventCode);

    mutable std::mutex m_cbMutex;
    mutable std::mutex m_sessionMutex;
    mutable std::mutex m_txMutex;
    std::shared_ptr<const PenEventCallback> m_eventCallback;
    std::shared_ptr<const KbdDetachSupportCallback> m_kbdDetachCallback;
    std::shared_ptr<const KbdStateCallback> m_kbdStateCallback;

    // detach support 与键盘状态缓存。读线程写，任意线程经取值接口读，故加锁。
    mutable std::mutex m_kbdMutex;
    bool m_kbdDetachSupport = false;
    bool m_kbdDetachSupportKnown = false;
    KbdState m_kbdState;

    // 连接状态的去抖。实测息屏与唤醒会让 0x12 报出 1~2 秒的假断开，直接透传会让设备页
    // 每次亮屏都闪一次「键盘已移除」。做成不对称的：连上立即生效，断开要连续静默满这段
    // 时间才承认——误报断开的代价远大于晚一秒承认断开。
    std::chrono::steady_clock::time_point m_kbdAbsentSince{};
    bool m_kbdAbsentPending = false;
    std::atomic<NativeEventHandle> m_notifyEvent{nullptr};
    PenUsbInitSession m_initSession;

    // 只在读线程上访问（OnConnected / OnPacketReceived / OnIdleTick 都在 WorkerFunc 里），
    // 因此不需要原子。
    std::chrono::steady_clock::time_point m_nextBatteryPollAt{};

    // MCU 对 0x0001 QueryPenModule 的应答时有时无——实测 8 次连接里有 4 次不回。不回时上层
    // 只能从固件版本串反推型号，得到的是同代的近似值（CD54R 会退化成 CD54），型号 ID 因此在
    // 两次启动之间摇摆。补发几次把它拉稳。
    std::chrono::steady_clock::time_point m_penModuleRetryAt{};
    int m_penModuleAttempts = 0;
    bool m_penModuleAnswered = false;
};

} // namespace Himax::Pen
