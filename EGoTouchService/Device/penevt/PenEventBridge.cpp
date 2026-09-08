#include "penevt/PenEventBridge.h"
#include "btmcu/PenUsbPacketBuilder.h"
#include "Logger.h"

#include <Windows.h>
#include <SetupAPI.h>
#include <hidsdi.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace Himax::Pen {

namespace {

const GUID kEventDeviceGuid =
    {0xdd0ebedb, 0xf1d6, 0x4cfa, {0xac, 0xca, 0x71, 0xe6, 0x6d, 0x31, 0x78, 0xca}};

} // namespace

PenEventBridge::~PenEventBridge() {
    Stop();
}

// ── 回调设置 ───────────────────────────────────────────────────────────────
void PenEventBridge::SetEventCallback(PenEventCallback cb) {
    auto callback = cb ? std::make_shared<const PenEventCallback>(std::move(cb)) : nullptr;
    std::lock_guard<std::mutex> lk(m_cbMutex);
    m_eventCallback = std::move(callback);
}

// ── 设备路径发现 ───────────────────────────────────────────────────────────
std::optional<std::wstring> PenEventBridge::FindDevicePath() {
    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &kEventDeviceGuid, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return std::nullopt;

    std::optional<std::wstring> result;
    for (DWORD i = 0; ; ++i) {
        SP_DEVICE_INTERFACE_DATA ifData{};
        ifData.cbSize = sizeof(ifData);
        if (!SetupDiEnumDeviceInterfaces(devInfo, nullptr,
                                         &kEventDeviceGuid, i, &ifData)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            continue;
        }
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &reqSize, nullptr);
        if (reqSize < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<uint8_t> buf(reqSize, 0);
        auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, det, reqSize,
                                              nullptr, nullptr)) {
            continue;
        }

        HANDLE probe = CreateFileW(det->DevicePath,
                                   GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
        if (probe == INVALID_HANDLE_VALUE) {
            LOG_WARN("PenEvent", __func__, "MCU",
                     "Skipping event device path that failed probe open: error={}",
                     GetLastError());
            continue;
        }

        CloseHandle(probe);
        result = det->DevicePath;
        break;
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return result;
}

// ── 协议辅助 ───────────────────────────────────────────────────────────────
bool PenEventBridge::SendRawPacket(std::span<const uint8_t> pkt) {
    std::lock_guard<std::mutex> txLock(m_txMutex);
    if (!IsTransportOpen()) {
        LOG_WARN("PenEvent", __func__, "MCU", "Transport not open; dropping packet send.");
        return false;
    }

    auto result = GetTransport()->WritePacket(pkt);
    if (!result) {
        LOG_WARN("PenEvent", __func__, "MCU",
                 "WritePacket failed while sending {} bytes (error={}).",
                 pkt.size(), static_cast<int>(result.error()));
        return false;
    }

    return true;
}

bool PenEventBridge::SendQueryPenModule() {
    const auto query = BuildPenUsbCommandBuffer(PenUsbCommandId::QueryPenModule);
    if (!SendRawPacket(query.view())) {
        LOG_WARN("PenEvent", __func__, "MCU", "Failed to send 0x0001 QueryPenModule.");
        return false;
    }

    LOG_INFO("PenEvent", __func__, "MCU", "Sent 0x0001 QueryPenModule.");
    return true;
}

bool PenEventBridge::SendQuerySerialNumber() {
    const auto query = BuildPenUsbCommandBuffer(PenUsbCommandId::QueryPenSerialNo);
    if (!SendRawPacket(query.view())) {
        LOG_WARN("PenEvent", __func__, "MCU", "Failed to send 0x0101 QueryPenSerialNo.");
        return false;
    }

    LOG_INFO("PenEvent", __func__, "MCU", "Sent 0x0101 QueryPenSerialNo.");
    return true;
}

bool PenEventBridge::SendQueryHardwareVersion() {
    const auto query = BuildPenUsbCommandBuffer(PenUsbCommandId::QueryHardwareVersion);
    if (!SendRawPacket(query.view())) {
        LOG_WARN("PenEvent", __func__, "MCU", "Failed to send 0x0201 QueryHardwareVersion.");
        return false;
    }

    LOG_INFO("PenEvent", __func__, "MCU", "Sent 0x0201 QueryHardwareVersion.");
    return true;
}

bool PenEventBridge::SendQueryFirmwareVersion() {
    const auto query = BuildPenUsbCommandBuffer(PenUsbCommandId::QueryFirmwareVersion);
    if (!SendRawPacket(query.view())) {
        LOG_WARN("PenEvent", __func__, "MCU", "Failed to send 0x0301 QueryFirmwareVersion.");
        return false;
    }

    LOG_INFO("PenEvent", __func__, "MCU", "Sent 0x0301 QueryFirmwareVersion.");
    return true;
}

bool PenEventBridge::SendQueryPenBattery() {
    const auto query = BuildPenUsbCommandBuffer(PenUsbCommandId::QueryPenBattery);
    if (!SendRawPacket(query.view())) {
        LOG_WARN("PenEvent", __func__, "MCU", "Failed to send 0x0801 QueryPenBattery.");
        return false;
    }

    LOG_INFO("PenEvent", __func__, "MCU", "Sent 0x0801 QueryPenBattery.");
    return true;
}

bool PenEventBridge::SendQueryChargingStatus() {
    const auto query = BuildPenUsbCommandBuffer(PenUsbCommandId::QueryChargingStatus);
    if (!SendRawPacket(query.view())) {
        LOG_WARN("PenEvent", __func__, "MCU", "Failed to send 0x0901 QueryChargingStatus.");
        return false;
    }

    LOG_INFO("PenEvent", __func__, "MCU", "Sent one-shot 0x0901 QueryChargingStatus.");
    return true;
}

// ── 键盘 detach support ──────────────────────────────────────────────────
void PenEventBridge::SetKbdDetachSupportCallback(KbdDetachSupportCallback cb) {
    auto callback = cb ? std::make_shared<const KbdDetachSupportCallback>(std::move(cb)) : nullptr;
    std::lock_guard<std::mutex> lk(m_cbMutex);
    m_kbdDetachCallback = std::move(callback);
}

bool PenEventBridge::SendKbdDetachSupportGet() {
    const auto query = BuildKbdDetachSupportGetBuffer();
    if (!SendRawPacket(query.view())) {
        LOG_WARN("PenEvent", __func__, "MCU", "Failed to send 0x35 KbdDetachSupportGet.");
        return false;
    }
    LOG_INFO("PenEvent", __func__, "MCU", "Sent 0x35 KbdDetachSupportGet.");
    return true;
}

bool PenEventBridge::SendKbdDetachSupportSet(bool enable) {
    const auto command = BuildKbdDetachSupportSetBuffer(enable);
    if (!SendRawPacket(command.view())) {
        LOG_WARN("PenEvent", __func__, "MCU", "Failed to send 0x34 KbdDetachSupportSet.");
        return false;
    }
    LOG_INFO("PenEvent", __func__, "MCU", "Sent 0x34 KbdDetachSupportSet enable={}.", enable);
    // Set 的应答（byte[4]==0x00 的 0x34）会被 MCU 读线程的过滤规则丢弃，等不到。重发一次
    // Get，用它的应答确认实际生效值。见 docs/KBDMCU_PROTOCOL.md 4.2。
    return SendKbdDetachSupportGet();
}

std::optional<bool> PenEventBridge::KbdDetachSupport() const {
    std::lock_guard<std::mutex> lk(m_kbdMutex);
    if (!m_kbdDetachSupportKnown) {
        return std::nullopt;
    }
    return m_kbdDetachSupport;
}

void PenEventBridge::HandleKeyboardFrame(std::span<const uint8_t> packet) {
    if (auto support = Kbd::TryParseDetachSupportReply(packet)) {
        const bool enabled = *support;
        {
            std::lock_guard<std::mutex> lk(m_kbdMutex);
            m_kbdDetachSupport = enabled;
            m_kbdDetachSupportKnown = true;
        }
        LOG_INFO("PenEvent", __func__, "MCU",
                 "Keyboard detach support = {}.", enabled);

        std::shared_ptr<const KbdDetachSupportCallback> cb;
        {
            std::lock_guard<std::mutex> lk(m_cbMutex);
            cb = m_kbdDetachCallback;
        }
        if (cb && *cb) {
            (*cb)(enabled);
        }
        return;
    }

    if (packet.size() >= kPenUsbHeaderSize && packet[4] == kSubsystemKeyboard) {
        const uint8_t code = packet[5];
        const std::size_t payloadLength = packet[7];
        const bool hasByte = packet.size() > kPenUsbHeaderSize;

        if (code == Kbd::kCmdConnectStatus && hasByte) {
            // 原厂把 packet[8] ∈ {1,2,3} 全部折成「已连接」——那三个值区分的是连接方式，
            // 不是连不连得上。
            ApplyKbdPresent(packet[kPenUsbHeaderSize] != 0);
            return;
        }
        if (code == Kbd::kCmdBattery && hasByte) {
            const uint8_t level = packet[kPenUsbHeaderSize];
            {
                std::lock_guard<std::mutex> lk(m_kbdMutex);
                m_kbdState.hasBattery = true;
                m_kbdState.battery = level;
            }
            LOG_INFO("PenEvent", __func__, "MCU", "Keyboard battery = {}%.", level);
            NotifyKbdState();
            return;
        }
        if (auto charging = Kbd::TryParseChargingStatus(packet)) {
            {
                std::lock_guard<std::mutex> lk(m_kbdMutex);
                m_kbdState.hasCharging = true;
                m_kbdState.charging = *charging;
            }
            LOG_INFO("PenEvent", __func__, "MCU",
                     "Keyboard charging = {}.", *charging);
            NotifyKbdState();
            return;
        }
        if (code == Kbd::kCmdDetachStatus && hasByte) {
            // 极性与 docs/KEYBOARD_IDENTITY.md 的静态分析相反：那份结论是从插件按 0x31 切换
            // Detach.png / Snapping.png 推出来的，两张图对应哪个值只能靠推断，推反了。
            // 实测键盘吸附在位时 packet[8] 为非零，所以非零是「已吸附」。
            const bool detached = packet[kPenUsbHeaderSize] == 0;
            {
                std::lock_guard<std::mutex> lk(m_kbdMutex);
                m_kbdState.hasDetached = true;
                m_kbdState.detached = detached;
            }
            LOG_INFO("PenEvent", __func__, "MCU", "Keyboard detached = {}.", detached);
            NotifyKbdState();
            return;
        }
        if (code == Kbd::kCmdFirmwareVersion && payloadLength > 0 &&
            packet.size() >= kPenUsbHeaderSize + payloadLength) {
            const auto* first = reinterpret_cast<const char*>(packet.data() + kPenUsbHeaderSize);
            std::string firmware(first, payloadLength);
            // MCU 用 NUL 填满定长字段，尾部零字节不属于版本串本身。
            firmware.erase(firmware.find_last_not_of('\0') + 1);
            const std::string_view model = Kbd::KeyboardModelNameFromFirmware(firmware);
            {
                std::lock_guard<std::mutex> lk(m_kbdMutex);
                m_kbdState.firmware = firmware;
                m_kbdState.modelName = std::string(model);
            }
            LOG_INFO("PenEvent", __func__, "MCU",
                     "Keyboard firmware = '{}', model = '{}'.",
                     firmware, model.empty() ? "unknown" : std::string(model));
            NotifyKbdState();
            return;
        }
    }

    // 其它键盘子系统帧降级到 debug——它们是合法的键盘
    // 广播，不是坏包，不该按 pen 的非法包路径刷 WARN。
    LOG_DEBUG("PenEvent", __func__, "MCU",
              "Ignoring keyboard-subsystem frame: subsystem=0x{:02X} code=0x{:02X}.",
              packet.size() > 4 ? packet[4] : 0, packet.size() > 5 ? packet[5] : 0);
}

namespace {
// 连接状态去抖窗口。实测息屏/唤醒造成的假断开在 1~2 秒内自行恢复，取略大于它。
constexpr auto kKbdAbsentConfirmDelay = std::chrono::seconds(3);
} // namespace

void PenEventBridge::ApplyKbdPresent(bool present) {
    if (present) {
        // 连上立即生效，并撤销正在计时的「疑似断开」。
        m_kbdAbsentPending = false;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_kbdMutex);
            changed = !m_kbdState.hasPresent || !m_kbdState.present;
            m_kbdState.hasPresent = true;
            m_kbdState.present = true;
        }
        if (changed) {
            LOG_INFO("PenEvent", __func__, "MCU", "Keyboard present.");
            NotifyKbdState();
        }
        return;
    }

    // 断开不立即承认：起一个计时，OnIdleTick 里等满窗口没被撤销才落地。
    if (!m_kbdAbsentPending) {
        m_kbdAbsentPending = true;
        m_kbdAbsentSince = std::chrono::steady_clock::now();
    }
}

void PenEventBridge::TickKbdAbsentDebounce() {
    if (!m_kbdAbsentPending) return;
    if (std::chrono::steady_clock::now() - m_kbdAbsentSince < kKbdAbsentConfirmDelay) return;

    m_kbdAbsentPending = false;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_kbdMutex);
        changed = !m_kbdState.hasPresent || m_kbdState.present;
        m_kbdState.hasPresent = true;
        m_kbdState.present = false;
    }
    if (changed) {
        LOG_INFO("PenEvent", __func__, "MCU", "Keyboard absent (debounced).");
        NotifyKbdState();
    }
}

void PenEventBridge::NotifyKbdState() {
    KbdState snapshot;
    {
        std::lock_guard<std::mutex> lk(m_kbdMutex);
        snapshot = m_kbdState;
    }
    std::shared_ptr<const KbdStateCallback> cb;
    {
        std::lock_guard<std::mutex> lk(m_cbMutex);
        cb = m_kbdStateCallback;
    }
    if (cb && *cb) {
        (*cb)(snapshot);
    }
}

void PenEventBridge::SetKbdStateCallback(KbdStateCallback cb) {
    auto callback = cb ? std::make_shared<const KbdStateCallback>(std::move(cb)) : nullptr;
    std::lock_guard<std::mutex> lk(m_cbMutex);
    m_kbdStateCallback = std::move(callback);
}

PenEventBridge::KbdState PenEventBridge::KbdStateSnapshot() const {
    std::lock_guard<std::mutex> lk(m_kbdMutex);
    return m_kbdState;
}

bool PenEventBridge::SendKbdStatusQueries() {
    bool ok = SendRawPacket(BuildKbdConnectStatusGetBuffer().view());
    ok = SendRawPacket(BuildKbdDetachStatusGetBuffer().view()) && ok;
    ok = SendRawPacket(BuildKbdFirmwareVersionGetBuffer().view()) && ok;
    ok = SendRawPacket(BuildKbdBatteryGetBuffer().view()) && ok;
    ok = SendRawPacket(BuildKbdChargingStatusGetBuffer().view()) && ok;
    return ok;
}

namespace {
// 重发间隔。读循环的超时是 1 秒，链路安静时补发实际落在下一次 idle tick 上，所以这个值只是
// 下限，不必调细——它是连接时的一次性收敛，不在延迟敏感路径上。
constexpr auto kPenModuleRetryDelay = std::chrono::milliseconds(500);
// 连原厂也不是每次都能拿到，试几次拿不到就认了：上层有从固件版本串反推的兜底，型号不会空。
// 这是总发送次数，不是重试次数：OnConnected 里的那一次算作 attempt 1，补发只有 3 次。
constexpr int kPenModuleMaxAttempts = 4;
} // namespace

void PenEventBridge::MaybeRetryPenModuleQuery() {
    if (m_penModuleAnswered || m_penModuleAttempts >= kPenModuleMaxAttempts) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < m_penModuleRetryAt) {
        return;
    }
    m_penModuleRetryAt = now + kPenModuleRetryDelay;
    ++m_penModuleAttempts;
    LOG_INFO("PenEvent", __func__, "MCU",
             "PenModule unanswered, resending 0x0001 (attempt {}/{}).",
             m_penModuleAttempts, kPenModuleMaxAttempts);
    (void)SendQueryPenModule();
}

void PenEventBridge::OnIdleTick() {
    MaybeRetryPenModuleQuery();
    TickKbdAbsentDebounce();
}

// ── BtHidChannel hooks ────────────────────────────────────────────────────
//
// 本类是 MCU 端点上的被动读者。握手（0x7101、两次 0x7701）、每个事件的 0x8001 ACK 和
// 0x7D01 初始化参数都由 THP_Service.dll 在它的宿主进程里完成——触控在我们手上时那是
// GaokunThpHost，交还华为时是 HuaweiThpService，两者装的是同一个 DLL。这里曾经把这一套
// 再做一遍，结果 MCU 在笔唤醒重协商的那一刻收到两份 ACK 外加我们的查询，之后 TSACore 就
// 间歇性地收不到蓝牙压力，笔尖在信号弱的区域被当成手指。原厂选件中心的 PenService.dll
// 同样只读不发，除了下面这些一次性查询和用户触发的命令。
void PenEventBridge::OnConnected() {
    m_penModuleAnswered = false;
    m_penModuleAttempts = 1;
    m_penModuleRetryAt = std::chrono::steady_clock::now() + kPenModuleRetryDelay;
    (void)SendQueryPenModule();
    (void)SendQuerySerialNumber();
    (void)SendQueryHardwareVersion();
    (void)SendQueryFirmwareVersion();
    // 服务可能在笔已经吸附时启动；MCU 不会为既有状态补发吸附边沿，因此主动取一次当前
    // 充电状态和电量建立基线。只在每次 USB 通道建立时发这两条，不加入任何周期轮询：
    // 之后的电量由 MCU 在笔重连时推送的 0x2C 和 0x28 顶部电量窗事件维持。
    (void)SendQueryChargingStatus();
    (void)SendQueryPenBattery();
    // 连接建立时取一次键盘 detach support 的初始状态，供上层显示当前开关位置。
    (void)SendKbdDetachSupportGet();
    // 键盘的连接、分离与固件版本同样只在这里主动问一次；之后靠 MCU 的事件推送维持，
    // 它是回调驱动的，没有轮询。
    (void)SendKbdStatusQueries();
    m_kbdAbsentPending = false;
}

// TODO: 这条端点上有两个读者。本类打开的 device path 与 hal 的 GaokunKeyboardHost（经厂商
// PenService.dll / KeyboardService.dll）是同一个，而一个中断包只交付给一个读者，谁抢到算谁
// 的，见 docs/KBDMCU_PROTOCOL.md 6.3。下面这段按子系统 ID 分流只解决了「本进程内不误判」，
// 解决不了跨进程丢帧：界面读的是 hal 那份快照，它漏掉的帧就是界面上迟迟不更新的状态。
//
// 两条路都可行，选哪条要先定：让 hal 宿主成为唯一读者、本类改从它的快照取键盘状态；或者
// 反过来，键盘状态回到本类、hal 只留笔。前者与「厂商 DLL 是唯一数据源」的现有方向一致，
// 但侧键握手（PenEventBridge 的 0x7101 + 0x7701 + 0x7B InitParam）必须留在本类，那是
// 厂商 DLL 不做而 MCU 又必需的一步。
void PenEventBridge::OnPacketReceived(std::span<const uint8_t> packet) {
    // 与键盘共用同一个 USB 端点，一个中断包只交付一个读者。先按子系统 ID（byte[4]）分流：
    // 0x01 是 pen 事件，走下面的原有解析；其余（0x00 detach、0x02 键盘）交给键盘处理。不分
    // 流的话键盘帧会被 pen 解析当作非法包每帧刷一条 WARN。详见 docs/KBDMCU_PROTOCOL.md 6.3。
    if (packet.size() >= kPenUsbHeaderSize && packet[4] != kSubsystemPen) {
        HandleKeyboardFrame(packet);
        return;
    }

    auto parsed = TryParsePenUsbEventFrame(packet);
    if (!parsed) {
        LOG_WARN("PenEvent", __func__, "MCU",
                 "Dropping invalid RX packet: size={}B.", packet.size());
        return;
    }

    const uint8_t eventCode = parsed->eventCode;

    LOG_INFO("PenEvent", __func__, "MCU", "Received event packet: code=0x{:02X} (Name={}) payloadLen={}",
             eventCode, PenUsbEventNameFromRaw(eventCode), parsed->payload.size());

    if (eventCode == static_cast<uint8_t>(PenUsbEventCode::PenModule) && !m_penModuleAnswered) {
        m_penModuleAnswered = true;
        if (m_penModuleAttempts > 1) {
            LOG_INFO("PenEvent", __func__, "MCU",
                     "PenModule answered on attempt {}.", m_penModuleAttempts);
        }
    }

    PenEvent ev;
    bool hasEvent = false;
    {
        ev.code = static_cast<PenUsbEventCode>(eventCode);
        (void)ev.payload.assign(parsed->payload);
        ev.receivedAt = std::chrono::steady_clock::now();

        if (!ev.payload.empty()) {
            switch (ev.code) {
            case PenUsbEventCode::PenModule: {
                const auto payloadLength = static_cast<uint8_t>(parsed->payload.size());
                auto modelId = TryParsePenModuleModelId(parsed->payload, payloadLength);
                if (!modelId) {
                    LOG_WARN("PenEvent", __func__, "MCU",
                             "PenModule ignored: invalid payloadLen={}",
                             parsed->payload.size());
                    break;
                }

                const auto modelInfo = ResolvePenModuleModel(*modelId);
                ev.semantic.hasPenModuleModelId = true;
                ev.semantic.penModuleModelId = *modelId;
                ev.semantic.penModuleModel = modelInfo.model;
                ev.semantic.hasPenModuleProtocolHint =
                    modelInfo.protocolHint != PenModuleProtocolHint::Auto;
                ev.semantic.penModuleProtocolHint = modelInfo.protocolHint;
                break;
            }
            case PenUsbEventCode::PenSerialNumber: {
                auto serialNumber = DecodePenUsbUtf8Payload(parsed->payload);
                ev.semantic.hasSerialNumber = !serialNumber.empty();
                ev.semantic.serialNumber = std::move(serialNumber);
                if (!ev.semantic.hasSerialNumber) {
                    LOG_WARN("PenEvent", __func__, "MCU",
                             "PenSerialNumber ignored: empty or invalid UTF-8 payloadLen={}",
                             parsed->payload.size());
                }
                break;
            }
            case PenUsbEventCode::PenHardwareVersion: {
                auto hardwareVersion = DecodePenUsbUtf8Payload(parsed->payload);
                ev.semantic.hasHardwareVersion = !hardwareVersion.empty();
                ev.semantic.hardwareVersion = std::move(hardwareVersion);
                if (!ev.semantic.hasHardwareVersion) {
                    LOG_WARN("PenEvent", __func__, "MCU",
                             "PenHardwareVersion ignored: empty or invalid UTF-8 payloadLen={}",
                             parsed->payload.size());
                }
                break;
            }
            case PenUsbEventCode::UsbdSwVersion: {
                auto firmwareVersion = DecodePenUsbUtf8Payload(parsed->payload);
                ev.semantic.hasFirmwareVersion = !firmwareVersion.empty();
                ev.semantic.firmwareVersion = std::move(firmwareVersion);
                if (!ev.semantic.hasFirmwareVersion) {
                    LOG_WARN("PenEvent", __func__, "MCU",
                             "UsbdSwVersion ignored: empty or invalid UTF-8 payloadLen={}",
                             parsed->payload.size());
                }
                break;
            }
            case PenUsbEventCode::DevPairStatus:
                ev.semantic.hasPairStatus = true;
                ev.semantic.pairStatus = ev.payload[0];
                break;
            case PenUsbEventCode::PenConnStatus:
                ev.semantic.hasConnection = true;
                ev.semantic.connected = (ev.payload[0] != 0);
                break;
            case PenUsbEventCode::PenTypeInfo:
                ev.semantic.hasStylusId = true;
                ev.semantic.stylusId = ev.payload[0];
                break;
            case PenUsbEventCode::PenCurStatus:
                ev.semantic.hasCurrentMode = true;
                ev.semantic.currentModeRaw = ev.payload[0];
                ev.semantic.currentMode = PenCurrentModeFromRaw(ev.payload[0]);
                break;
            case PenUsbEventCode::EraserToggle:
                ev.semantic.hasEraserToggle = true;
                ev.semantic.eraserToggle = ev.payload[0];
                break;
            case PenUsbEventCode::PenCurrentFunc:
                ev.semantic.hasCurrentFunc = true;
                ev.semantic.currentFunc = ev.payload[0];
                break;
            case PenUsbEventCode::BatteryStatus:
            case PenUsbEventCode::PenBatteryAfterConn:
                // Both carry a percentage; 0x2C is the unsolicited report the MCU sends
                // right after a pen reconnects. Values above 100 are treated as garbage
                // rather than clamped, so a decode error stays visible instead of
                // silently pinning the gauge at full.
                if (ev.payload[0] <= 100) {
                    ev.semantic.hasBatteryLevel = true;
                    ev.semantic.batteryLevel = ev.payload[0];
                } else {
                    LOG_WARN("PenEvent", __func__, "MCU",
                             "Battery level {} out of range, ignored.", ev.payload[0]);
                }
                break;
            case PenUsbEventCode::ChargingStatus:
                // 曾按 BTMCU_PROTOCOL.md 3.4 的说法改成只采信 0x0901 应答、丢弃周期广播,
                // 实机证明是倒退：广播以约 1 Hz 成串到达，查询后到达的第一条 0x09 大概率
                // 就是下一条广播，采样成了抛硬币；且吸附弹窗正靠广播带来的状态变化触发,
                // 丢弃广播后弹窗一并消失。广播直喂在旧版上被用户确认是正常的。
                ev.semantic.hasChargingState = true;
                ev.semantic.charging = (ev.payload[0] != 0);
                break;
            case PenUsbEventCode::DevConnect:
                ev.semantic.hasDeviceConnected = true;
                ev.semantic.deviceConnected = (ev.payload[0] != 0);
                break;
            default:
                break;
            }
        }

        hasEvent = true;
    }

    if (hasEvent) {
        std::shared_ptr<const PenEventCallback> callback;
        {
            std::lock_guard<std::mutex> lk(m_cbMutex);
            callback = m_eventCallback;
        }
        if (callback) {
            (*callback)(ev);
        }
    }

    if (const auto notifyEvent = m_notifyEvent.load(std::memory_order_acquire)) {
        SetEvent(static_cast<HANDLE>(notifyEvent));
    }

    // OnIdleTick 只在读超时时触发，而 MCU 会成串地广播状态；光靠 OnIdleTick，链路一忙
    // 补发就被饿死。放在这里保证补发按墙钟走，与流量无关。
    MaybeRetryPenModuleQuery();
}

} // namespace Himax::Pen
