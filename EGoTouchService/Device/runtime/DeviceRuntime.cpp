#include "runtime/DeviceRuntime.h"
#include "Logger.h"
#include "config/ConfigBinder.h"
#include "config/SchemaValidator.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace {
constexpr std::chrono::milliseconds kEventDebounce{400};

uint8_t PayloadByteOrZero(const Himax::Pen::PenEvent &ev) noexcept {
  return ev.payload.empty() ? 0 : ev.payload[0];
}

const char* ToString(PenButtonAction::Type type) noexcept {
  switch (type) {
  case PenButtonAction::Type::Barrel:      return "Barrel";
  case PenButtonAction::Type::Eraser:      return "Eraser";
  case PenButtonAction::Type::DoubleClick: return "DoubleClick";
  default:                                 return "Unknown";
  }
}

// 按键注入只剩 Win32 一条路。VHF 注入随自研采集一起搬进了 hal 的宿主进程，本进程不再有
// VHF 设备，所以路由不再是「选哪个后端」而只是「注不注入」。PenButtonRoute 仍留在配置
// schema 里（键 id 只增不减），VhfOnly 于是等价于不注入。
[[nodiscard]] bool PenButtonInjectsWin32(PenButtonMode mode, PenButtonRoute route,
                                         bool routeExplicit) noexcept {
  if (!routeExplicit) {
    // 未显式配置时按模式定。WindowsInk 与 ToggleEraser 只有双击语义，而双击在
    // DispatchPenButtonAction 里先行分派、根本走不到这里，所以它们不需要注入。
    return mode == PenButtonMode::NativeBarrel || mode == PenButtonMode::NativeEraser;
  }
  return route == PenButtonRoute::Win32Only || route == PenButtonRoute::VhfAndWin32;
}

StylusProtocolHint ResolveProtocolHintFromStylusId(uint8_t) noexcept {
  // PenTypeInfo is not a reliable HPP2/HPP3 discriminator.  Protocol selection
  // is driven by PenModule ModelId when available; otherwise the hint stays in
  // Auto.
  return StylusProtocolHint::Auto;
}

void ResetPenTransientState(RuntimePenState &state) noexcept {
  state.hasCurrentMode = false;
  state.currentMode = Himax::Pen::PenCurrentMode::Unknown;
  state.currentModeRaw = 0;
  state.hasEraserToggle = false;
  state.eraserToggle = 0;
  state.hasCurrentFunc = false;
  state.currentFunc = 0;
}

bool ClearPenIdentityState(RuntimePenState &state) noexcept {
  const bool changed =
      state.hasStylusId || state.stylusId != 0 ||
      state.protocolHint != StylusProtocolHint::Auto ||
      state.protocolHintFromPenModule || state.hasPenModuleModelId ||
      state.penModuleModelId != 0 ||
      state.penModuleModel != Himax::Pen::PenModuleModel::Unknown ||
      state.hasSerialNumber || !state.serialNumber.empty() ||
      state.hasHardwareVersion || !state.hardwareVersion.empty() ||
      state.hasFirmwareVersion || !state.firmwareVersion.empty();

  state.hasStylusId = false;
  state.stylusId = 0;
  state.protocolHint = StylusProtocolHint::Auto;
  state.protocolHintFromPenModule = false;
  state.hasPenModuleModelId = false;
  state.penModuleModelId = 0;
  state.penModuleModel = Himax::Pen::PenModuleModel::Unknown;
  state.hasSerialNumber = false;
  state.serialNumber.clear();
  state.hasHardwareVersion = false;
  state.hardwareVersion.clear();
  state.hasFirmwareVersion = false;
  state.firmwareVersion.clear();
  return changed;
}

StylusProtocolHint ResolveProtocolHintFromPenModule(
    Himax::Pen::PenModuleProtocolHint hint) noexcept {
  switch (hint) {
  case Himax::Pen::PenModuleProtocolHint::Hpp2:
    return StylusProtocolHint::Hpp2;
  case Himax::Pen::PenModuleProtocolHint::Hpp3:
    return StylusProtocolHint::Hpp3;
  default:
    return StylusProtocolHint::Auto;
  }
}
} // namespace

// --------------- ToString helpers ---------------

const char *ToString(RuntimePolicyEvent::Type type) noexcept {
  switch (type) {
  case RuntimePolicyEvent::Type::DisplayOn:
    return "DisplayOn";
  case RuntimePolicyEvent::Type::DisplayOff:
    return "DisplayOff";
  case RuntimePolicyEvent::Type::LidOn:
    return "LidOn";
  case RuntimePolicyEvent::Type::LidOff:
    return "LidOff";
  case RuntimePolicyEvent::Type::Suspend:
    return "Suspend";
  case RuntimePolicyEvent::Type::Shutdown:
    return "Shutdown";
  case RuntimePolicyEvent::Type::ResumeAutomatic:
    return "ResumeAutomatic";
  default:
    return "Unknown";
  }
}

// --------------- Lifecycle ---------------

// 三个设备路径是自研采集时代的构造参数。采集已经整体搬到 GaokunThpHost，这里不再打开任何
// 设备；签名保留是因为调用点还在按它构造，改签名属于另一次改动。
DeviceRuntime::DeviceRuntime(const std::wstring &,
                             const std::wstring &,
                             const std::wstring &) {}

DeviceRuntime::~DeviceRuntime() { Stop(); }

void DeviceRuntime::Stop() {
  m_shutdownRequested.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(m_penIngressMu);
    m_penReplay.CompleteInitCycle();
  }
  m_btPenLatch.Clear();
}

void DeviceRuntime::ApplyServicePolicy(bool autoMode, bool stylusVhfEnabled,
                                       PenButtonMode penButtonMode,
                                       PenButtonRoute penButtonRoute,
                                       bool penButtonRouteExplicit) {
  SetAutoMode(autoMode);
  SetPenButtonMode(penButtonMode);
  SetPenButtonRoute(penButtonRoute, penButtonRouteExplicit);
  // stylusVhfEnabled 现在没有接收方：笔的实时报文由 GaokunThpHost 里的原厂 VHF 发出，本类
  // 关不掉它。仍然记进日志，配置里的取值和实际行为对不上时至少能看出是哪一边。
  LOG_INFO(
      "Runtime", __func__, "Policy",
      "Applied: autoMode={} stylusVhfEnabled={} (no effect) penBtnMode={} penBtnRoute={} penBtnRouteExplicit={}",
      autoMode, stylusVhfEnabled, static_cast<int>(penButtonMode),
      static_cast<int>(penButtonRoute), penButtonRouteExplicit);
}

Config::ValidationResult DeviceRuntime::ValidateConfigStore(
    const Config::ConfigStore &store) const {
  std::lock_guard<std::mutex> lk(m_policyMu);

  // 运行时这一层不再声明任何配置键：触摸与笔的参数曾由求解器的 registerBindings 贡献，
  // 算法搬进原厂算法链之后由它自己的参数表管，服务侧的键仍在 RegisterServiceConfigBindings。
  Config::ConfigBinder binder;
  return Config::SchemaValidator::validate(store, binder);
}

void DeviceRuntime::SetInputSuppressed(bool suppressed) {
  m_inputSuppressed.store(suppressed, std::memory_order_release);
}

bool DeviceRuntime::IsInputSuppressed() const {
  return m_inputSuppressed.load(std::memory_order_acquire);
}

void DeviceRuntime::IngestBtMcuPressure(uint16_t p) {
  m_btPenLatch.SetPressure(p);
}

void DeviceRuntime::IngestBtMcuPressurePacket(
    const std::array<uint16_t, 4> &pressure,
    const std::array<uint16_t, 4> &rawPressure, uint8_t freq1, uint8_t freq2) {
  m_btPenLatch.SetPressurePacket(pressure, rawPressure, freq1, freq2);
}

void DeviceRuntime::ApplyPenSessionChange() {
  // 笔的连接或身份变了就丢弃锁存的压力，否则上一支笔的最后一次压力会落到下一次落笔上。
  m_btPenLatch.Clear();
}

void DeviceRuntime::SetPenStateChangedCallback(PenStateChangedCallback cb) {
  std::lock_guard<std::mutex> lk(m_penStateChangedCbMu);
  m_penStateChangedCb = std::move(cb);
}

void DeviceRuntime::SetPenDoubleClickCallback(PenDoubleClickCallback cb) {
  std::lock_guard<std::mutex> lk(m_penDoubleClickCbMu);
  m_penDoubleClickCb = std::move(cb);
}

void DeviceRuntime::UpdatePenState(std::function<void(RuntimePenState&, PenStateUpdateResult&)> updateFn) {
  PenStateUpdateResult result{};
  bool applyToPipeline = false;
  bool notifyChanged = false;
  RuntimePenState snapshot{};
  {
    std::lock_guard<std::mutex> lk(m_penStateMu);
    updateFn(m_penState, result);

    const bool changed = result.stateChanged || result.stylusIdChanged;
    if (changed) {
      ++m_penState.penRevision;
      if (result.applyToPipeline) {
        ++m_penState.pipelineRevision;
        applyToPipeline = true;
      }
      notifyChanged = true;
      snapshot = m_penState;
    }
  }

  // Deliberately outside m_penStateMu: the callback publishes to shared memory and
  // signals an event, and holding the state lock across that would let a slow reader
  // stall pen event ingestion.
  if (notifyChanged) {
    PenStateChangedCallback cb;
    {
      std::lock_guard<std::mutex> lk(m_penStateChangedCbMu);
      cb = m_penStateChangedCb;
    }
    if (cb) cb(snapshot);
  }

  if (applyToPipeline) {
    ApplyPenSessionChange();
  }
}

// 橡皮擦开关的唯一状态源。硬件 0x7F EraserToggle 和 ToggleEraser 模式下的双击都改这一个
// 值——各记各的，下一次双击就会从对方看不见的状态上翻转，两个来源互相打架。
bool DeviceRuntime::IsEraserActive() const {
  std::lock_guard<std::mutex> lk(m_penStateMu);
  return m_penState.hasEraserToggle && m_penState.eraserToggle != 0;
}

void DeviceRuntime::UpdateEraserState(bool active) {
  const uint8_t next = active ? 1 : 0;
  UpdatePenState([next](RuntimePenState& state, PenStateUpdateResult& res) {
    // 橡皮擦不参与笔的会话身份，只影响发布出去的一个状态位。
    res.applyToPipeline = false;
    res.stateChanged = !state.hasEraserToggle || state.eraserToggle != next;
    state.hasEraserToggle = true;
    state.eraserToggle = next;
  });
}

void DeviceRuntime::DispatchPenButtonAction(const PenButtonAction& action, const char* source) {
  const auto mode = GetPenButtonMode();
  const auto route = GetPenButtonRoute();
  const bool injectsWin32 = PenButtonInjectsWin32(
      mode, route, m_penButtonRouteExplicit.load(std::memory_order_acquire));

  bool win32Attempted = false;
  bool win32Ok = false;

  // 双击不看路由计划：它不是「注入到哪个后端」的问题。WindowsInk 要把手势交给用户会话
  // 里的伴随进程，ToggleEraser 只翻转服务自己的橡皮擦位，两条路都不经过按键注入后端，
  // 被注入开关挡住只会让显式 route 配置连带关掉双击。
  if (action.type == PenButtonAction::Type::DoubleClick) {
    auto signalUserSession = [this] {
      PenDoubleClickCallback cb;
      {
        std::lock_guard<std::mutex> lk(m_penDoubleClickCbMu);
        cb = m_penDoubleClickCb;
      }
      return cb ? cb() : false;
    };

    switch (mode) {
    case PenButtonMode::WindowsInk: {
      // 服务自己调 SendInput 会以 ERROR_ACCESS_DENIED 失败——会话 0 没有可交互的输入桌面。
      win32Attempted = true;
      // 回调返回的是「事件真的送达了托盘」，不是「装过回调」：托盘没跑或事件没建成时
      // 它返回 false，日志里的 win32_ok 才不会把无声失效记成成功。
      win32Ok = signalUserSession();
      break;
    }
    case PenButtonMode::ToggleEraser: {
      const bool next = !IsEraserActive();
      UpdateEraserState(next);
      // 先发布状态、再发边沿，托盘才能读到 next：桌面 OneNote 不消费虚拟笔的 eraser
      // flags，托盘需要在用户会话里同步它自己的绘图工具。
      win32Attempted = true;
      win32Ok = signalUserSession();
      break;
    }
    default:
      break;
    }

    LOG_INFO("Runtime", __func__, "MCU",
             "{}: action={} mode={} route={} win32={} win32_ok={}",
             source, ToString(action.type), ToString(mode), ToString(route),
             win32Attempted ? 1 : 0, win32Ok ? 1 : 0);
    return;
  }

  // Barrel/Eraser 的 VHF 后端已经不在本进程里，只剩 Win32 这一条路。
  if (injectsWin32) {
    win32Attempted = true;
    // 笔脉冲要落在一个屏幕坐标上；快捷键不需要，所以只在用得到时才取光标位置。
    auto cursorPoint = [] {
      POINT pt{};
      GetCursorPos(&pt);
      return pt;
    };

    switch (action.type) {
    case PenButtonAction::Type::DoubleClick:
      break;  // 已在上面按 mode 分派完毕并返回，走不到这里。
    case PenButtonAction::Type::Barrel:
      // 这里原先发的是 Win+F22，而专做此事的 InjectBarrelPulse 就在旁边且无人调用——
      // 接错了线。F22 在系统里没有接收方，所以这条路一直是哑的。
      if (action.pressed &&
          (mode == PenButtonMode::OemCustom || mode == PenButtonMode::NativeBarrel)) {
        win32Ok = m_synthPenButton.InjectBarrelPulse(cursorPoint());
      }
      break;
    case PenButtonAction::Type::Eraser:
      if (action.pressed && mode == PenButtonMode::NativeEraser) {
        win32Ok = m_synthPenButton.InjectEraserPulse(cursorPoint());
      }
      break;
    }
  }

  LOG_INFO("Runtime", __func__, "MCU",
           "{}: action={} pressed={} mode={} route={} win32={} win32_ok={}",
           source,
           ToString(action.type),
           action.pressed, ToString(mode), ToString(route),
           win32Attempted ? 1 : 0, win32Ok ? 1 : 0);
}

// --------------- 电源事件 ---------------

// 只剩观测：本类不再持有工作线程或设备句柄，灭屏与休眠没有需要它停下的东西。保留这条
// 入口是因为去抖和日志仍然是排查「唤醒之后笔状态不刷新」的第一现场。
void DeviceRuntime::IngestPolicyEvent(const RuntimePolicyEvent &ev) {
  using EventType = RuntimePolicyEvent::Type;

  if (ev.type == EventType::Shutdown) {
    m_shutdownRequested.store(true, std::memory_order_release);
  } else if (m_shutdownRequested.load(std::memory_order_acquire)) {
    LOG_INFO("Runtime", __func__, "Policy",
             "Ignoring non-terminal event ({}) after shutdown was requested.",
             ToString(ev.type));
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lk(m_mu);
    const size_t key = static_cast<size_t>(ev.type);
    if (key < m_lastEventByType.size()) {
      auto lastTime = m_lastEventByType[key];
      if (lastTime != std::chrono::steady_clock::time_point{} &&
          now - lastTime < kEventDebounce) {
        return;
      }
      m_lastEventByType[key] = now;
    }
  }

  LOG_INFO("Runtime", __func__, "Policy", "System event observed: {}.",
           ToString(ev.type));
}

// --------------- 快照 ---------------

RuntimePenState DeviceRuntime::GetPenStateSnapshot() const {
  std::lock_guard<std::mutex> lk(m_penStateMu);
  return m_penState;
}

PenAfeReplayState DeviceRuntime::GetPenAfeReplayStateSnapshot() const {
  std::lock_guard<std::mutex> penIngressLock(m_penIngressMu);
  return m_penReplay;
}

// --------------- MCU 事件路由 ---------------

void DeviceRuntime::HandlePenButtonStatusCode(uint8_t statusCode,
                                              uint8_t rawEventPayload,
                                              const char *source) {
  // statusCode 3 and 4 represent a barrel button action in this context
  PenButtonAction action{PenButtonAction::Type::Barrel, true, rawEventPayload};
  DispatchPenButtonAction(action, source);
}

void DeviceRuntime::IngestPenEvent(const Himax::Pen::PenEvent &ev) {
  // 把状态改写串行化在同一把锁下：MCU 事件可能来自多条读线程。
  std::lock_guard<std::mutex> penIngressLock(m_penIngressMu);
  using EC = Himax::Pen::PenUsbEventCode;
  const uint8_t payload0 = PayloadByteOrZero(ev);

  if (Himax::Pen::FactoryStatusFlagsAffected(ev.code)) {
    std::lock_guard<std::mutex> lk(m_penStateMu);
    m_penState.factoryStatusFlags = Himax::Pen::ApplyFactoryStatusFlagUpdate(
        m_penState.factoryStatusFlags, ev.code, payload0);
  }

  switch (ev.code) {

  case EC::DevPairStatus: {
    if (!ev.semantic.hasPairStatus) {
      LOG_WARN("Runtime", __func__, "MCU",
               "DevPairStatus ignored because no valid pair status semantic was present.");
      break;
    }

    UpdatePenState([&](RuntimePenState& state, PenStateUpdateResult& res) {
      res.applyToPipeline = false;
      res.stateChanged = !state.hasPairStatus ||
                         state.pairStatus != ev.semantic.pairStatus;
      state.hasPairStatus = true;
      state.pairStatus = ev.semantic.pairStatus;
    });
    break;
  }

  case EC::BatteryStatus:
  case EC::PenBatteryAfterConn: {
    if (!ev.semantic.hasBatteryLevel) break;
    UpdatePenState([&](RuntimePenState& state, PenStateUpdateResult& res) {
      res.applyToPipeline = false;   // battery has no bearing on pen identity
      res.stateChanged = !state.hasBatteryLevel ||
                         state.batteryLevel != ev.semantic.batteryLevel;
      state.hasBatteryLevel = true;
      state.batteryLevel = ev.semantic.batteryLevel;
    });
    break;
  }

  case EC::ChargingStatus: {
    if (!ev.semantic.hasChargingState) break;
    bool changed = false;
    UpdatePenState([&](RuntimePenState& state, PenStateUpdateResult& res) {
      res.applyToPipeline = false;
      changed = !state.hasChargingState || state.charging != ev.semantic.charging;
      res.stateChanged = changed;
      state.hasChargingState = true;
      state.charging = ev.semantic.charging;
    });
    if (changed) {
      LOG_INFO("Runtime", __func__, "MCU", "Pen charging state -> {}.",
               ev.semantic.charging ? "charging" : "not charging");
    }
    break;
  }

  case EC::DevConnect: {
    if (!ev.semantic.hasDeviceConnected) break;
    UpdatePenState([&](RuntimePenState& state, PenStateUpdateResult& res) {
      res.applyToPipeline = false;
      res.stateChanged = !state.hasDeviceConnected ||
                         state.deviceConnected != ev.semantic.deviceConnected;
      state.hasDeviceConnected = true;
      state.deviceConnected = ev.semantic.deviceConnected;
    });
    break;
  }

  case EC::PenModule: {
    if (!ev.semantic.hasPenModuleModelId) {
      LOG_WARN("Runtime", __func__, "MCU",
               "PenModule ignored because no valid ModelId semantic was present.");
      break;
    }

    UpdatePenState([&](RuntimePenState& state, PenStateUpdateResult& res) {
      const bool oldHasStylusId = state.hasStylusId;
      const uint8_t oldStylusId = state.stylusId;
      const auto oldProtocolHint = state.protocolHint;
      const Himax::Pen::PenModuleModelInfo modelInfo{
          ev.semantic.penModuleModelId,
          ev.semantic.penModuleModel,
          ev.semantic.hasPenModuleProtocolHint
              ? ev.semantic.penModuleProtocolHint
              : Himax::Pen::PenModuleProtocolHint::Auto,
          Himax::Pen::ToString(ev.semantic.penModuleModel)};

      const bool hasModuleHint =
          modelInfo.protocolHint != Himax::Pen::PenModuleProtocolHint::Auto;
      StylusProtocolHint nextProtocolHint = hasModuleHint
          ? ResolveProtocolHintFromPenModule(modelInfo.protocolHint)
          : StylusProtocolHint::Auto;
      if (!hasModuleHint && state.hasStylusId) {
        nextProtocolHint = ResolveProtocolHintFromStylusId(state.stylusId);
      }

      res.stateChanged = !state.hasPenModuleModelId ||
                       state.penModuleModelId != modelInfo.modelId ||
                       state.penModuleModel != modelInfo.model ||
                       state.protocolHintFromPenModule != hasModuleHint ||
                       state.protocolHint != nextProtocolHint;

      if (const auto derivedStylusId =
              Himax::Pen::TryResolveStylusIdFromPenModule(modelInfo.model);
          derivedStylusId && !state.hasStylusId) {
        state.hasStylusId = true;
        state.stylusId = *derivedStylusId;
        res.stylusIdChanged = true;
        res.nextStylusId = *derivedStylusId;
      }

      state.hasPenModuleModelId = true;
      state.penModuleModelId = modelInfo.modelId;
      state.penModuleModel = modelInfo.model;
      state.protocolHintFromPenModule = hasModuleHint;
      state.protocolHint = nextProtocolHint;
      res.applyToPipeline = oldHasStylusId != state.hasStylusId ||
                            oldStylusId != state.stylusId ||
                            oldProtocolHint != state.protocolHint;
    });
    break;
  }

  case EC::PenSerialNumber: {
    if (!ev.semantic.hasSerialNumber) {
      LOG_WARN("Runtime", __func__, "MCU",
               "PenSerialNumber ignored because no valid serial semantic was present.");
      break;
    }

    UpdatePenState([&](RuntimePenState& state, PenStateUpdateResult& res) {
      res.applyToPipeline = false;
      res.stateChanged = !state.hasSerialNumber ||
                         state.serialNumber != ev.semantic.serialNumber;
      state.hasSerialNumber = true;
      state.serialNumber = ev.semantic.serialNumber;
    });
    break;
  }

  case EC::PenHardwareVersion: {
    if (!ev.semantic.hasHardwareVersion) {
      LOG_WARN("Runtime", __func__, "MCU",
               "PenHardwareVersion ignored because no valid version semantic was present.");
      break;
    }

    UpdatePenState([&](RuntimePenState& state, PenStateUpdateResult& res) {
      res.applyToPipeline = false;
      res.stateChanged = !state.hasHardwareVersion ||
                         state.hardwareVersion != ev.semantic.hardwareVersion;
      state.hasHardwareVersion = true;
      state.hardwareVersion = ev.semantic.hardwareVersion;
    });
    break;
  }

  case EC::UsbdSwVersion: {
    if (!ev.semantic.hasFirmwareVersion) {
      LOG_WARN("Runtime", __func__, "MCU",
               "UsbdSwVersion ignored because no valid firmware semantic was present.");
      break;
    }

    UpdatePenState([&](RuntimePenState& state, PenStateUpdateResult& res) {
      const bool oldHasStylusId = state.hasStylusId;
      const uint8_t oldStylusId = state.stylusId;
      const auto oldProtocolHint = state.protocolHint;
      res.applyToPipeline = false;
      res.stateChanged = !state.hasFirmwareVersion ||
                         state.firmwareVersion != ev.semantic.firmwareVersion;
      state.hasFirmwareVersion = true;
      state.firmwareVersion = ev.semantic.firmwareVersion;

      const auto modelInfoFromFirmware =
          Himax::Pen::TryResolvePenModuleModelFromText(state.firmwareVersion);

      if (modelInfoFromFirmware && !state.hasPenModuleModelId) {
        const Himax::Pen::PenModuleModelInfo &modelInfo = *modelInfoFromFirmware;

        const bool hasModuleHint =
            modelInfo.protocolHint != Himax::Pen::PenModuleProtocolHint::Auto;
        StylusProtocolHint nextProtocolHint = hasModuleHint
            ? ResolveProtocolHintFromPenModule(modelInfo.protocolHint)
            : StylusProtocolHint::Auto;
        if (!hasModuleHint && state.hasStylusId) {
          nextProtocolHint = ResolveProtocolHintFromStylusId(state.stylusId);
        }

        res.stateChanged = res.stateChanged ||
                           !state.hasPenModuleModelId ||
                           state.penModuleModelId != modelInfo.modelId ||
                           state.penModuleModel != modelInfo.model ||
                           state.protocolHintFromPenModule != hasModuleHint ||
                           state.protocolHint != nextProtocolHint;

        if (const auto derivedStylusId =
                Himax::Pen::TryResolveStylusIdFromPenModule(modelInfo.model);
            derivedStylusId && !state.hasStylusId) {
          state.hasStylusId = true;
          state.stylusId = *derivedStylusId;
          res.stylusIdChanged = true;
          res.nextStylusId = *derivedStylusId;
        }

        state.hasPenModuleModelId = true;
        state.penModuleModelId = modelInfo.modelId;
        state.penModuleModel = modelInfo.model;
        state.protocolHintFromPenModule = hasModuleHint;
        state.protocolHint = nextProtocolHint;

        LOG_INFO("Runtime", __func__, "MCU",
                 "Derived PenModule ModelId from USBD_SW_VERSION: model={} id=0x{:06X} protocol={}",
                 modelInfoFromFirmware->name,
                 static_cast<unsigned int>(modelInfoFromFirmware->modelId),
                 Himax::Pen::ToString(modelInfoFromFirmware->protocolHint));
      }
      res.applyToPipeline = oldHasStylusId != state.hasStylusId ||
                            oldStylusId != state.stylusId ||
                            oldProtocolHint != state.protocolHint;
    });
    break;
  }

  case EC::PenConnStatus: {
    UpdatePenState([&](RuntimePenState& state, PenStateUpdateResult& res) {
      const bool hadConnection = state.hasConnection;
      const bool oldConnected = state.connected;

      state.hasConnection = ev.semantic.hasConnection;
      state.connected =
          ev.semantic.hasConnection ? ev.semantic.connected : false;
      const bool connected = state.hasConnection && state.connected;

      const bool connectionChanged =
          hadConnection != state.hasConnection || oldConnected != state.connected;
      res.stateChanged = connectionChanged;
      if (connectionChanged) {
        ResetPenTransientState(state);
      }

      if (!connected) {
        const bool pipelineIdentityChanged =
            state.hasStylusId ||
            state.protocolHint != StylusProtocolHint::Auto;
        res.stateChanged = ClearPenIdentityState(state) || res.stateChanged;
        res.applyToPipeline = connectionChanged || pipelineIdentityChanged;
      } else if (!state.protocolHintFromPenModule && state.hasStylusId) {
        state.protocolHint = ResolveProtocolHintFromStylusId(state.stylusId);
      }
    });
    break;
  }

  case EC::PenFreqJump: {
    break;
  }

  case EC::PenTypeInfo: {
    const bool hasStylusId = ev.semantic.hasStylusId;
    const uint8_t stateStylusId = hasStylusId ? ev.semantic.stylusId : 0;
    const auto fallbackProtocolHint = ResolveProtocolHintFromStylusId(stateStylusId);

    UpdatePenState([&](RuntimePenState& state, PenStateUpdateResult& res) {
      const bool protocolFromPenModule = state.protocolHintFromPenModule;
      res.stateChanged =
          state.hasStylusId != hasStylusId ||
          state.stylusId != stateStylusId ||
          (!protocolFromPenModule && state.protocolHint != fallbackProtocolHint);

      state.hasStylusId = hasStylusId;
      state.stylusId = stateStylusId;
      if (!protocolFromPenModule) {
        state.protocolHint = fallbackProtocolHint;
      }
    });
    break;
  }

  case EC::PenCurStatus: {
    UpdatePenState([&](RuntimePenState& state, PenStateUpdateResult& res) {
      state.hasCurrentMode = ev.semantic.hasCurrentMode;
      state.currentModeRaw =
          ev.semantic.hasCurrentMode ? ev.semantic.currentModeRaw : 0;
      state.currentMode = ev.semantic.hasCurrentMode
                               ? ev.semantic.currentMode
                               : Himax::Pen::PenCurrentMode::Unknown;
    });
    break;
  }

  case EC::PenCurrentFunc: {
    uint8_t func = 0;
    UpdatePenState([&](RuntimePenState& state, PenStateUpdateResult& res) {
      state.hasCurrentFunc = ev.semantic.hasCurrentFunc;
      state.currentFunc =
          ev.semantic.hasCurrentFunc ? ev.semantic.currentFunc : payload0;
      func = state.currentFunc;
    });

    // 0x25 GetPenKeySupport 在本机返回掩码 1：这支笔只支持一个按键功能，实测它也只在
    // 双击时发 0x2F，单击和长按什么都不发。所以收到这个事件就等于「侧键被双击了」，
    // 不必按 payload 判断手势种类。
    //
    // payload 是当前功能编号，不是手势种类——原厂 CallbackPenCurrentFunc 拿它区分的是
    // 「进入橡皮(1)」还是「退出橡皮(0)」，取值随侧键绑定而变（见 docs/pen_eraser_flow.md）。
    // 这里此前写死 func == 1 才分派，于是把绑定改成截屏一类之后双击就再也没有反应,
    // 而日志里连一条分派记录都不会留下。
    LOG_INFO("Runtime", __func__, "MCU", "PenCurrentFunc payload={}", func);
    DispatchPenButtonAction({PenButtonAction::Type::DoubleClick, false, func},
                            "PenCurrentFunc");
    break;
  }

  case EC::PenAcStatus:
  case EC::PenRotateAngle:
  case EC::PenTouchMode:
  case EC::PenGlobalPreventMode:
  case EC::PenHolster:
    break;

  case EC::PenGlobalAnnotation:
    // 只发按下、不发松开。Win32 那条路走的 InjectBarrelPulse 本身就是脉冲，不需要配对的
    // 复位。
    DispatchPenButtonAction({PenButtonAction::Type::Barrel, true, payload0}, "PenGlobalAnnotation");
    break;

  case EC::EraserToggle: {
    const uint8_t eraserState =
        ev.semantic.hasEraserToggle ? ev.semantic.eraserToggle : 0;
    // 经 UpdateEraserState 落地，与 ToggleEraser 模式的双击共用同一个开关。
    UpdateEraserState(eraserState != 0);
    DispatchPenButtonAction({PenButtonAction::Type::Eraser, eraserState != 0, eraserState}, "EraserToggle");
    break;
  }

  default:
    break;
  }
}
