#include "runtime/DeviceRuntime.h"
#include "Logger.h"
#include "SolverTypes.h"
#include "SystemEpochClock.h"
#include "config/ConfigBinder.h"
#include "config/SchemaValidator.h"


#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {
constexpr std::chrono::milliseconds kEventDebounce{400};
constexpr std::chrono::milliseconds kDisplayOffSuspendDelay{2000};

uint8_t PayloadByteOrZero(const Himax::Pen::PenEvent &ev) noexcept {
  return ev.payload.empty() ? 0 : ev.payload[0];
}

struct PenButtonRoutePlan {
  bool vhf = false;
  bool win32 = false;
};

const char* ToString(PenButtonAction::Type type) noexcept {
  switch (type) {
  case PenButtonAction::Type::Barrel:      return "Barrel";
  case PenButtonAction::Type::Eraser:      return "Eraser";
  case PenButtonAction::Type::DoubleClick: return "DoubleClick";
  default:                                 return "Unknown";
  }
}

PenButtonRoutePlan DefaultPenButtonRouteForMode(PenButtonMode mode) noexcept {
  switch (mode) {
  case PenButtonMode::OemCustom:
    return {.vhf = true, .win32 = false};
  case PenButtonMode::NativeBarrel:
  case PenButtonMode::NativeEraser:
    return {.vhf = false, .win32 = true};
  case PenButtonMode::WindowsInk:
  case PenButtonMode::ToggleEraser:
    // 这两个模式只有双击语义，而双击已不经过这张路由表（见 DispatchPenButtonAction）。
    // 表里剩下的 barrel/eraser 动作在这两个模式下没有接收方，所以两条后端都不开。
    // 此处原先写的是 WindowsInk「必须走 Win32」，名不副实：显式的 route 配置本来就能
    // 把它改成 VhfOnly，而那时双击照样得走回调。
    return {.vhf = false, .win32 = false};
  default:
    return {.vhf = true, .win32 = false};
  }
}

PenButtonRoutePlan ResolvePenButtonRoute(PenButtonMode mode,
                                         PenButtonRoute route,
                                         bool routeExplicit) noexcept {
  if (!routeExplicit && route == PenButtonRoute::VhfOnly) {
    return DefaultPenButtonRouteForMode(mode);
  }

  switch (route) {
  case PenButtonRoute::VhfOnly:
    return {.vhf = true, .win32 = false};
  case PenButtonRoute::Win32Only:
    return {.vhf = false, .win32 = true};
  case PenButtonRoute::VhfAndWin32:
    return {.vhf = true, .win32 = true};
  default:
    return DefaultPenButtonRouteForMode(mode);
  }
}

Solvers::StylusProtocolHint ResolveProtocolHintFromStylusId(uint8_t) noexcept {
  // PenTypeInfo is not a reliable HPP2/HPP3 discriminator.  Protocol selection
  // is driven by PenModule ModelId when available; otherwise the stylus pipeline
  // stays in Auto and resolves from packet shape.
  return Solvers::StylusProtocolHint::Auto;
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
      state.protocolHint != Solvers::StylusProtocolHint::Auto ||
      state.protocolHintFromPenModule || state.hasPenModuleModelId ||
      state.penModuleModelId != 0 ||
      state.penModuleModel != Himax::Pen::PenModuleModel::Unknown ||
      state.hasSerialNumber || !state.serialNumber.empty() ||
      state.hasHardwareVersion || !state.hardwareVersion.empty() ||
      state.hasFirmwareVersion || !state.firmwareVersion.empty();

  state.hasStylusId = false;
  state.stylusId = 0;
  state.protocolHint = Solvers::StylusProtocolHint::Auto;
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

Solvers::StylusProtocolHint ResolveProtocolHintFromPenModule(
    Himax::Pen::PenModuleProtocolHint hint) noexcept {
  switch (hint) {
  case Himax::Pen::PenModuleProtocolHint::Hpp2:
    return Solvers::StylusProtocolHint::Hpp2;
  case Himax::Pen::PenModuleProtocolHint::Hpp3:
    return Solvers::StylusProtocolHint::Hpp3;
  default:
    return Solvers::StylusProtocolHint::Auto;
  }
}

const char *ToString(Solvers::StylusProtocolHint hint) noexcept {
  switch (hint) {
  case Solvers::StylusProtocolHint::Auto:
    return "Auto";
  case Solvers::StylusProtocolHint::Hpp2:
    return "Hpp2";
  case Solvers::StylusProtocolHint::Hpp3:
    return "Hpp3";
  default:
    return "Unknown";
  }
}
} // namespace

// --------------- ToString helpers ---------------

const char *ToString(workerState s) noexcept {
  switch (s) {
  case workerState::suspend:
    return "suspend";
  case workerState::quit:
    return "quit";
  case workerState::ready:
    return "ready";
  case workerState::streaming:
    return "streaming";
  case workerState::recover:
    return "recover";
  default:
    return "unknown";
  }
}

const char *ToString(CommandSource s) noexcept {
  switch (s) {
  case CommandSource::External:
    return "External";
  case CommandSource::SystemPolicy:
    return "SystemPolicy";
  default:
    return "Unknown";
  }
}

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

PenAfeCommandPlan BuildPenAfeCommandPlan(const RuntimePenState& state) noexcept {
  PenAfeCommandPlan plan{};
  if (!state.hasConnection || !state.connected) {
    return plan;
  }

  plan.commands[plan.count++] = command{AFE_Command::InitStylus, 0};
  if (state.hasStylusId) {
    plan.commands[plan.count++] =
        command{AFE_Command::SetStylusId, state.stylusId};
  }
  return plan;
}

std::optional<command> BuildPenConnectionAfeCommand(
    bool connectionChanged, bool connected) noexcept {
  if (!connectionChanged) {
    return std::nullopt;
  }
  return command{
      connected ? AFE_Command::InitStylus : AFE_Command::DisconnectStylus,
      0};
}

// --------------- Lifecycle ---------------

DeviceRuntime::DeviceRuntime(const std::wstring &master,
                             const std::wstring &slave,
                             const std::wstring &interrupt)
    : m_chip(master, slave, interrupt) {
  m_vhfReporter.SetStylusPacketSensorRows(
      m_stylusPipeline.GetPacketSensorRows());
  m_vhfReporter.SetStylusPacketSensorCols(
      m_stylusPipeline.GetPacketSensorCols());
  m_vhfReporter.SetStylusPacketEmitWhenInvalid(
      m_stylusPipeline.GetEmitPacketWhenInvalid());
}

DeviceRuntime::~DeviceRuntime() { Stop(); }

bool DeviceRuntime::Start() {
  return StartStateMachine() == StartRequestResult::Started;
}

DeviceRuntime::StartRequestResult DeviceRuntime::StartStateMachine() {
  std::unique_lock<std::mutex> lifecycleLock(m_lifecycleMu);
  const auto caller = std::this_thread::get_id();
  if (m_workerThreadId == caller ||
      (m_thread.joinable() && m_thread.get_id() == caller)) {
    LOG_WARN("Runtime", __func__, "Thread",
             "Worker-thread start request rejected to avoid lifecycle self-deadlock.");
    return StartRequestResult::Failed;
  }

  m_lifecycleCv.wait(lifecycleLock, [this]() {
    return !m_stopInProgress && !m_selfStopDetached;
  });

  if (m_running.load(std::memory_order_acquire)) {
    bool lifecycleAccepting = false;
    {
      std::lock_guard<std::mutex> lk(m_mu);
      lifecycleAccepting = m_acceptExternalAfeCommands;
    }
    const bool workerIsQuitting =
        !lifecycleAccepting ||
        m_state.load(std::memory_order_acquire) == workerState::quit ||
        m_stopReason.load(std::memory_order_acquire) == StopReason::Shutdown;
    if (!workerIsQuitting) {
      return StartRequestResult::AlreadyRunning;
    }
    if (!m_thread.joinable() ||
        m_thread.get_id() == std::this_thread::get_id()) {
      return StartRequestResult::Failed;
    }

    m_lifecycleCv.wait(lifecycleLock, [this]() {
      return !m_running.load(std::memory_order_acquire) &&
             m_workerThreadId == std::thread::id{};
    });
  }

  if (m_thread.joinable()) {
    if (m_thread.get_id() == std::this_thread::get_id()) {
      LOG_WARN("Runtime", __func__, "Thread",
               "Start() called from worker thread while previous worker is joinable.");
      return StartRequestResult::Failed;
    }
    m_thread.join();
  }

  // Initialize every new-generation state before publishing running/accepting.
  // IngestPolicyEvent uses m_lifecycleMu, so events linearized after publication
  // cannot be overwritten by startup resets.
  m_stopReason.store(StopReason::None, std::memory_order_release);
  SetState(workerState::ready);
  m_needSuspendDeinit.store(false, std::memory_order_release);
  m_systemSuspendObserved.store(false, std::memory_order_release);
  m_consecutiveFrameErrors = 0;

  {
    std::lock_guard<std::mutex> penIngressLock(m_penIngressMu);
    m_acceptPenAfeCommands = true;
    if (m_autoMode.load(std::memory_order_acquire)) {
      m_penReplay.BeginInitCycle();
    } else {
      if (m_penReplay.generation == 0) {
        m_penReplay.BeginInitCycle();
      }
      m_penReplay.CompleteInitCycle();
    }

    std::lock_guard<std::mutex> lk(m_mu);
    CancelQueuedCommandsLocked("cancelled: superseded by runtime start");
    m_displayOffSuspendPending = false;
    m_lastEventByType.fill(std::chrono::steady_clock::time_point{});
    m_recoverCount = 0;
    m_lastNote = "Runtime started";
    m_acceptExternalAfeCommands = true;
    m_stopped.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
  }

  m_thread = std::thread(&DeviceRuntime::WorkerMain, this);
  LOG_INFO("Runtime", __func__, "ready", "Worker thread launched.");

  return StartRequestResult::Started;
}

void DeviceRuntime::Stop() {
  {
    std::unique_lock<std::mutex> lifecycleLock(m_lifecycleMu);
    if (m_selfStopDetached && m_workerThreadId == std::this_thread::get_id()) {
      return;
    }

    if (m_stopInProgress) {
      if ((m_thread.joinable() &&
           m_thread.get_id() == std::this_thread::get_id()) ||
          m_workerThreadId == std::this_thread::get_id()) {
        return;
      }
      m_lifecycleCv.wait(lifecycleLock,
                         [this]() { return !m_stopInProgress; });
      if (!m_running.load(std::memory_order_acquire) &&
          !m_thread.joinable()) {
        return;
      }
    }

    {
      // Close both accepting gates before cancelling queued work. Pen state
      // snapshots remain writable while stopped.
      std::lock_guard<std::mutex> penIngressLock(m_penIngressMu);
      m_acceptPenAfeCommands = false;
      m_penReplay.CompleteInitCycle();

      std::lock_guard<std::mutex> lk(m_mu);
      m_acceptExternalAfeCommands = false;
      m_stopped.store(true, std::memory_order_release);
      CancelQueuedCommandsLocked("cancelled: runtime stop");
    }
    m_stopReason.store(StopReason::Shutdown, std::memory_order_release);

    if (!m_running.load(std::memory_order_acquire) && !m_thread.joinable() &&
        !m_selfStopDetached) {
      SetState(workerState::quit);
      std::lock_guard<std::mutex> lk(m_mu);
      m_lastNote = "Runtime stopped";
      return;
    }

    m_stopInProgress = true;
  }

  m_chip.CancelPendingFrameRead();

  std::unique_lock<std::mutex> lifecycleLock(m_lifecycleMu);
  if (m_thread.joinable()) {
    if (m_thread.get_id() == std::this_thread::get_id()) {
      m_selfStopDetached = true;
      m_thread.detach();
      m_stopInProgress = false;
      LOG_INFO("Runtime", __func__, "Thread",
               "Stop() called from worker thread; worker detached and will exit asynchronously.");
      m_lifecycleCv.notify_all();
      return;
    }

    // WorkerMain clears m_workerThreadId under m_lifecycleMu as its final
    // owner-state access. Do not hold this mutex while joining, or the worker
    // cannot publish completion and Stop() can deadlock.
    lifecycleLock.unlock();
    m_thread.join();
    lifecycleLock.lock();
  } else if (m_selfStopDetached) {
    // A detached self-stop worker may set m_running=false before it finishes
    // lifecycle cleanup. Wait for the final cleanup marker, not just !running,
    // so Stop()/~DeviceRuntime cannot return while the detached worker can
    // still access owner fields.
    m_lifecycleCv.wait(lifecycleLock, [this]() {
      return !m_selfStopDetached &&
             m_workerThreadId == std::thread::id{};
    });
  }
  m_running.store(false, std::memory_order_release);
  SetState(workerState::quit);
  {
    std::lock_guard<std::mutex> lk(m_mu);
    m_lastNote = "Runtime stopped";
  }
  m_stopInProgress = false;
  lifecycleLock.unlock();
  m_lifecycleCv.notify_all();
}

DeviceRuntime::StartRequestResult DeviceRuntime::RequestStart() {
  return StartStateMachine();
}

DeviceRuntime::StopRequestResult DeviceRuntime::RequestStop() {
  const bool wasRunning = IsRunning();
  Stop();
  return wasRunning ? StopRequestResult::Stopped
                    : StopRequestResult::AlreadyStopped;
}

void DeviceRuntime::ApplyServicePolicy(bool autoMode, bool stylusVhfEnabled,
                                       PenButtonMode penButtonMode,
                                       PenButtonRoute penButtonRoute,
                                       bool penButtonRouteExplicit) {
  SetAutoMode(autoMode);
  SetStylusVhfEnabled(stylusVhfEnabled);
  SetPenButtonMode(penButtonMode);
  SetPenButtonRoute(penButtonRoute, penButtonRouteExplicit);
  LOG_INFO(
      "Runtime", __func__, "Policy",
      "Applied: autoMode={} stylusVhfEnabled={} penBtnMode={} penBtnRoute={} penBtnRouteExplicit={}",
      autoMode, stylusVhfEnabled, static_cast<int>(penButtonMode),
      static_cast<int>(penButtonRoute), penButtonRouteExplicit);
}

Config::ValidationResult DeviceRuntime::ValidateConfigStore(
    const Config::ConfigStore &store) const {
  std::lock_guard<std::mutex> lk(m_pipelineMu);

  // Build schema from fresh pipeline instances so validation remains read-only
  // and cannot accidentally strip setters from the runtime apply path.
  Solvers::TouchPipeline touchPipeline;
  Solvers::StylusPipeline stylusPipeline;
  Config::ConfigBinder binder;
  touchPipeline.registerBindings(binder);
  stylusPipeline.registerBindings(binder);
  return Config::SchemaValidator::validate(store, binder);
}

void DeviceRuntime::ApplyConfigStore(const Config::ConfigStore& store) {
  std::lock_guard<std::mutex> lk(m_pipelineMu);
  m_touchPipeline.applyConfig(store);
  m_stylusPipeline.applyConfig(store);
  m_vhfReporter.SetStylusPacketSensorRows(m_stylusPipeline.GetPacketSensorRows());
  m_vhfReporter.SetStylusPacketSensorCols(m_stylusPipeline.GetPacketSensorCols());
  m_vhfReporter.SetStylusPacketEmitWhenInvalid(m_stylusPipeline.GetEmitPacketWhenInvalid());
  LOG_INFO("Runtime", __func__, "Config", "Applied ConfigStore to touch/stylus pipelines.");
}

bool DeviceRuntime::IsShutdownRequested() const {
  return m_stopReason.load() == StopReason::Shutdown;
}

#ifdef _DEBUG
void DeviceRuntime::SetFramePushCallback(DeviceRuntime::FramePushCallback cb) {
  std::lock_guard<std::mutex> lk(m_framePushCbMu);
  m_framePushCb = std::move(cb);
}
#endif

void DeviceRuntime::SetVhfEnabled(bool enabled) {
  m_vhfReporter.SetEnabled(enabled);
}

bool DeviceRuntime::IsVhfEnabled() const {
  return m_vhfReporter.IsEnabled();
}

bool DeviceRuntime::IsVhfDeviceOpen() const {
  return m_vhfReporter.IsDeviceOpen();
}

void DeviceRuntime::SetInputSuppressed(bool suppressed) {
  m_vhfReporter.SetInputSuppressed(suppressed);
}

bool DeviceRuntime::IsInputSuppressed() const {
  return m_vhfReporter.IsInputSuppressed();
}

void DeviceRuntime::SetVhfTransposeEnabled(bool enabled) {
  m_vhfReporter.SetTransposeEnabled(enabled);
}

bool DeviceRuntime::IsVhfTransposeEnabled() const {
  return m_vhfReporter.IsTransposeEnabled();
}

void DeviceRuntime::SetMasterParserOnlyMode(bool enabled) {
  const bool wasEnabled =
      m_masterParserOnly.exchange(enabled, std::memory_order_acq_rel);
  if (!wasEnabled && enabled) {
    std::lock_guard<std::mutex> lk(m_pipelineMu);
    m_vhfReporter.FlushTouchAllUp();
  }
}

void DeviceRuntime::IngestBtMcuPressure(uint16_t p) {
  std::lock_guard<std::mutex> lk(m_pipelineMu);
  m_stylusPipeline.SetBtMcuPressure(p);
}

void DeviceRuntime::IngestBtMcuPressurePacket(
    const std::array<uint16_t, 4> &pressure,
    const std::array<uint16_t, 4> &rawPressure, uint8_t freq1, uint8_t freq2) {
  std::lock_guard<std::mutex> lk(m_pipelineMu);
  m_stylusPipeline.SetBtMcuPressurePacket(pressure, rawPressure, freq1, freq2);
}

void DeviceRuntime::ApplyPenStateToStylusPipeline() {
  Solvers::StylusPenSession session{};
  {
    std::lock_guard<std::mutex> lk(m_penStateMu);
    session.hasConnectionState = m_penState.hasConnection;
    session.connected = m_penState.connected;
    session.hasStylusId = m_penState.hasStylusId;
    session.stylusId = m_penState.stylusId;
    session.protocolHint = m_penState.protocolHint;
    session.revision = m_penState.pipelineRevision;
  }

  std::lock_guard<std::mutex> lk(m_pipelineMu);
  m_stylusPipeline.ApplyPenSession(session);
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
    ApplyPenStateToStylusPipeline();
  }

  if (result.stylusIdChanged) {
    command cmd{};
    cmd.type = AFE_Command::SetStylusId;
    cmd.param = result.nextStylusId;
    SubmitPenAfeCommandLocked(cmd, "PenIdentityChange->SetStylusId");
  }
}

void DeviceRuntime::SubmitPenAfeCommandLocked(command cmd, const char* reason) {
  if (!m_acceptPenAfeCommands || m_penReplay.pending) {
    return;
  }
  EnqueueCommand(cmd, CommandSource::SystemPolicy, reason,
                 m_penReplay.generation);
}

void DeviceRuntime::BeginPenReplayInitCycle() {
  std::lock_guard<std::mutex> penIngressLock(m_penIngressMu);
  if (m_acceptPenAfeCommands && !m_penReplay.pending) {
    m_penReplay.BeginInitCycle();
  }
}

void DeviceRuntime::ReplayPenStateAfterChipInit() {
  std::lock_guard<std::mutex> penIngressLock(m_penIngressMu);
  if (!m_acceptPenAfeCommands || !m_penReplay.pending) {
    return;
  }

  RuntimePenState snapshot{};
  {
    std::lock_guard<std::mutex> penStateLock(m_penStateMu);
    snapshot = m_penState;
  }

  const auto plan = BuildPenAfeCommandPlan(snapshot);
  for (std::size_t i = 0; i < plan.count; ++i) {
    const char* reason = plan.commands[i].type == AFE_Command::InitStylus
        ? "ChipInitReplay->InitStylus"
        : "ChipInitReplay->SetStylusId";
    ExecuteCommand(MakeQueuedCommand(
        plan.commands[i], CommandSource::SystemPolicy, reason));
  }
  m_penReplay.CompleteInitCycle();
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
    // 橡皮擦不参与 stylus pipeline 的会话身份，只影响 VHF 报文里的一个位。
    res.applyToPipeline = false;
    res.stateChanged = !state.hasEraserToggle || state.eraserToggle != next;
    state.hasEraserToggle = true;
    state.eraserToggle = next;
  });
}

void DeviceRuntime::DispatchPenButtonAction(const PenButtonAction& action, const char* source) {
  const auto mode = GetPenButtonMode();
  const auto route = GetPenButtonRoute();
  const auto plan = ResolvePenButtonRoute(
      mode, route, m_penButtonRouteExplicit.load(std::memory_order_acquire));

  bool vhfQueued = false;
  bool win32Attempted = false;
  bool win32Ok = false;

  // 双击不看路由计划：它不是「注入到哪个后端」的问题。WindowsInk 要把手势交给用户会话
  // 里的伴随进程，ToggleEraser 只翻转服务自己的橡皮擦位，两条路都不经过按键注入后端，
  // 被 plan.vhf/plan.win32 挡住只会让显式 route 配置连带关掉双击。
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
      m_vhfReporter.SetEraserState(next ? 1 : 0);
      vhfQueued = true;
      // 标准应用直接消费上面的 VHF 状态。桌面 OneNote 不消费虚拟笔的 eraser flags，托盘
      // 需要在用户会话里同步它自己的绘图工具；先发布状态、再发边沿，托盘才能读到 next。
      win32Attempted = true;
      win32Ok = signalUserSession();
      break;
    }
    default:
      break;
    }

    LOG_INFO("Runtime", __func__, "MCU",
             "{}: action={} mode={} route={} vhf={} win32={} win32_ok={}",
             source, ToString(action.type), ToString(mode), ToString(route),
             vhfQueued ? 1 : 0, win32Attempted ? 1 : 0, win32Ok ? 1 : 0);
    return;
  }

  // 1. VHF route
  if (plan.vhf) {
    if (action.type == PenButtonAction::Type::Barrel) {
      if (mode == PenButtonMode::OemCustom || mode == PenButtonMode::NativeBarrel) {
        m_vhfReporter.SetBarrelButtonState(action.pressed);
        vhfQueued = true;
      }
    } else if (action.type == PenButtonAction::Type::Eraser) {
      if (mode == PenButtonMode::OemCustom || mode == PenButtonMode::NativeEraser) {
        // 状态已由 ingest 侧的 UpdateEraserState 记下，这里只负责往 VHF 写。
        m_vhfReporter.SetEraserState(action.pressed ? 1 : 0);
        vhfQueued = true;
      }
    }
  }

  // 2. Win32 route
  if (plan.win32) {
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
           "{}: action={} pressed={} mode={} route={} vhf={} win32={} win32_ok={}",
           source,
           ToString(action.type),
           action.pressed, ToString(mode), ToString(route),
           vhfQueued ? 1 : 0, win32Attempted ? 1 : 0, win32Ok ? 1 : 0);
}

// --------------- 命令注入 ---------------

bool DeviceRuntime::SubmitExternalAfeCommand(AFE_Command type, uint8_t param) {
  command cmd{};
  cmd.type = type;
  cmd.param = param;
  auto qc = MakeQueuedCommand(cmd, CommandSource::External, "IPC AFE");

  std::lock_guard<std::mutex> lk(m_mu);
  // Recheck while holding the queue lock used by Stop() to clear pending work.
  // This closes the check-then-enqueue race at the stopped boundary.
  if (!m_acceptExternalAfeCommands ||
      !m_running.load(std::memory_order_acquire) ||
      m_stopped.load(std::memory_order_acquire)) {
    return false;
  }
  if (!m_cmdQueue.push(std::move(qc))) {
    LOG_ERROR("Runtime", __func__, "Queue", "Command queue overflow! Command dropped.");
    return false;
  }
  return true;
}

DeviceRuntime::QueuedCommand DeviceRuntime::MakeQueuedCommand(
    command cmd, CommandSource src, const char* reason,
    uint64_t penGeneration) {
  QueuedCommand qc{};
  qc.id = m_nextCmdId.fetch_add(1, std::memory_order_relaxed);
  qc.penGeneration = penGeneration;
  qc.cmd = cmd;
  qc.source = src;
  qc.enqueued_at = std::chrono::steady_clock::now();
  if (reason) {
    std::strncpy(qc.reason.data(), reason, qc.reason.size() - 1);
  }
  return qc;
}

uint64_t DeviceRuntime::EnqueueCommand(command cmd, CommandSource src,
                                       const char* reason,
                                       uint64_t penGeneration) {
  auto qc = MakeQueuedCommand(cmd, src, reason, penGeneration);
  const uint64_t commandId = qc.id;
  {
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_cmdQueue.push(std::move(qc))) {
      LOG_ERROR("Runtime", __func__, "Queue", "Command queue overflow! Command dropped.");
    }
  }
  return commandId;
}

uint64_t DeviceRuntime::SubmitCommand(command cmd, CommandSource src,
                                      const char *reason) {
  return EnqueueCommand(cmd, src, reason);
}

void DeviceRuntime::IngestPolicyEvent(const RuntimePolicyEvent &ev) {
  // Establish a generation boundary with Start/Stop. Events that acquire this
  // lock after running is published cannot be overwritten by startup resets.
  std::unique_lock<std::mutex> lifecycleLock(m_lifecycleMu);
  using EventType = RuntimePolicyEvent::Type;

  if (ev.type != EventType::Shutdown) {
    bool acceptingCommands = false;
    {
      std::lock_guard<std::mutex> lk(m_mu);
      acceptingCommands = m_acceptExternalAfeCommands;
    }
    const bool terminating =
        m_stopInProgress ||
        m_stopReason.load(std::memory_order_acquire) == StopReason::Shutdown;
    if (terminating) {
      LOG_INFO("Runtime", __func__, "Policy",
               "Ignoring non-terminal event ({}) while termination is in progress.",
               ToString(ev.type));
      return;
    }
    if (!acceptingCommands) {
      // 还没准备好,不是要关。**这一条会丢**:Windows 只在服务注册电源通知的那一刻
      // 送一次当前的显示/盖子状态,此后不再变化,于是运行时永远进不了 streaming——
      // 表现是重启服务后触摸整个不工作、工作台热图空白、DVR 录不出帧。
      //
      // 在这里补投过一版,被 SystemPowerPolicy 测试挡回来了:Start 发布 running 之后
      // 到达的 Shutdown 会被这个补投盖掉。补投的正确位置在 ServiceHost——它知道
      // 当前的显示/盖子状态,且不在 Start 的竞态里。未做。
      LOG_WARN("Runtime", __func__, "Policy",
               "Runtime not ready yet; dropping {} event. If this was the only "
               "display/lid notification, streaming will never start.",
               ToString(ev.type));
      return;
    }
  }

  const auto now = std::chrono::steady_clock::now();
  const bool isWakeEvent = ev.type == EventType::DisplayOn ||
                           ev.type == EventType::LidOn ||
                           ev.type == EventType::ResumeAutomatic;
  {
    std::lock_guard<std::mutex> lk(m_mu);
    const size_t key = static_cast<size_t>(ev.type);
    if (key < m_lastEventByType.size()) {
      auto lastTime = m_lastEventByType[key];
      if (lastTime != std::chrono::steady_clock::time_point{} &&
          now - lastTime < kEventDebounce &&
          !(isWakeEvent && m_displayOffSuspendPending)) {
        return;
      }
      m_lastEventByType[key] = now;
    }
  }

  switch (ev.type) {
  case EventType::DisplayOff: {
    std::lock_guard<std::mutex> lk(m_mu);
    m_displayOffSuspendPending = true;
    m_displayOffSuspendDeadline = now + kDisplayOffSuspendDelay;
  }
    LOG_INFO("Runtime", __func__, "Policy",
             "DisplayOff pending; suspend delayed by {} ms.",
             kDisplayOffSuspendDelay.count());
    m_chip.CancelPendingFrameRead();
    break;
  case EventType::LidOff: {
    std::lock_guard<std::mutex> lk(m_mu);
    m_displayOffSuspendPending = false;
  }
    LOG_INFO("Runtime", __func__, "Policy",
             "Sleep event ({}), requesting suspend.", ToString(ev.type));
    m_chip.CancelPendingFrameRead();
    {
      StopReason expected = StopReason::None;
      m_stopReason.compare_exchange_strong(
          expected, StopReason::ScreenOff, std::memory_order_acq_rel);
    }
    break;
  case EventType::Suspend: {
    {
      std::lock_guard<std::mutex> lk(m_mu);
      m_displayOffSuspendPending = false;
    }
    m_systemSuspendObserved.store(true, std::memory_order_release);
    LOG_INFO("Runtime", __func__, "Policy",
             "System suspend event, requesting immediate suspend.");
    m_chip.CancelPendingFrameRead();
    {
      StopReason expected = StopReason::None;
      m_stopReason.compare_exchange_strong(
          expected, StopReason::ScreenOff, std::memory_order_acq_rel);
    }
    break;
  }
  case EventType::DisplayOn:
  case EventType::LidOn:
  case EventType::ResumeAutomatic: {
    bool cancelledDisplayOff = false;
    {
      std::lock_guard<std::mutex> lk(m_mu);
      cancelledDisplayOff = m_displayOffSuspendPending;
      m_displayOffSuspendPending = false;
    }

    const bool resumedAfterSystemSuspend =
        m_systemSuspendObserved.exchange(false, std::memory_order_acq_rel);
    StopReason expected = StopReason::ScreenOff;
    const bool clearedScreenOff =
        m_stopReason.compare_exchange_strong(expected, StopReason::None);
    m_needSuspendDeinit.store(false, std::memory_order_release);

    LOG_INFO("Runtime", __func__, "Policy",
             "Wake event ({}), attempting resume.", ToString(ev.type));
    if (cancelledDisplayOff) {
      LOG_INFO("Runtime", __func__, "Policy",
               "Wake event cancelled pending DisplayOff suspend.");
    }
    if (clearedScreenOff) {
      LOG_INFO("Runtime", __func__, "Policy",
               "Wake event cleared pending ScreenOff stop reason.");
    }

    const workerState state = m_state.load(std::memory_order_acquire);
    // Baseline is now inherited across state transitions; no explicit
    // reacquire needed.
    if (state == workerState::suspend ||
        (resumedAfterSystemSuspend && IsRunning())) {
      m_chip.CancelPendingFrameRead();
      SetState(workerState::ready);
      LOG_INFO("Runtime", __func__, "Policy", "{}",
               resumedAfterSystemSuspend
                   ? "Resumed after system suspend -> ready for reinitialization."
                   : "Resumed from suspend -> ready (zero-cost wakeup).");
    } else if (IsRunning()) {
      LOG_INFO("Runtime", __func__, "Policy",
               "Runtime already active; wake event does not restart worker.");
    } else {
      LOG_INFO("Runtime", __func__, "Policy",
               "Wake event ignored because runtime is stopped.");
    }
  } break;
  case EventType::Shutdown: {
    LOG_INFO("Runtime", __func__, "Policy",
             "Shutdown event, requesting termination.");
    {
      std::lock_guard<std::mutex> penIngressLock(m_penIngressMu);
      m_acceptPenAfeCommands = false;
      m_penReplay.CompleteInitCycle();

      std::lock_guard<std::mutex> lk(m_mu);
      m_acceptExternalAfeCommands = false;
      CancelQueuedCommandsLocked("cancelled: shutdown requested");
    }
    m_stopReason.store(StopReason::Shutdown, std::memory_order_release);
    m_chip.CancelPendingFrameRead();
    break;
  }
  default:
    break;
  }
}

// --------------- Pipe 查询 ---------------

RuntimeSnapshot DeviceRuntime::GetSnapshot() const {
  std::lock_guard<std::mutex> lk(m_mu);
  RuntimeSnapshot s;
  s.state = m_state.load();
  s.stylus_connected = m_chip.IsStylusConnected();
  s.recover_count = m_recoverCount;
  s.queue_depth = m_cmdQueue.size();
  s.last_command_id = m_lastCmdId;
  s.last_note = m_lastNote;
  return s;
}

RuntimePenState DeviceRuntime::GetPenStateSnapshot() const {
  std::lock_guard<std::mutex> lk(m_penStateMu);
  return m_penState;
}

PenAfeReplayState DeviceRuntime::GetPenAfeReplayStateSnapshot() const {
  std::lock_guard<std::mutex> penIngressLock(m_penIngressMu);
  return m_penReplay;
}

std::vector<HistoryEntry> DeviceRuntime::GetHistory(std::size_t n) const {
  std::lock_guard<std::mutex> lk(m_mu);
  std::vector<HistoryEntry> result;
  if (m_historyCount == 0) {
    return result;
  }
  const size_t count = std::min(n, m_historyCount);
  result.reserve(count);

  size_t startIdx = (m_historyWriteIdx + kMaxHistoryItems - count) % kMaxHistoryItems;
  for (size_t i = 0; i < count; ++i) {
    result.push_back(m_history[(startIdx + i) % kMaxHistoryItems]);
  }
  return result;
}

void DeviceRuntime::ClearHistory() {
  std::lock_guard<std::mutex> lk(m_mu);
  m_historyWriteIdx = 0;
  m_historyCount = 0;
  m_history.fill(HistoryEntry{});
}

void DeviceRuntime::SetWorkerHookForTesting(std::function<void()> hook) {
  std::lock_guard<std::mutex> lock(m_workerHookMu);
  m_workerHookForTesting = std::move(hook);
}

bool DeviceRuntime::IsAcceptingExternalAfeCommands() const {
  std::lock_guard<std::mutex> lk(m_mu);
  return m_acceptExternalAfeCommands;
}

// --------------- 审计日志 ---------------

void DeviceRuntime::CancelQueuedCommandsLocked(const char* detail) {
  QueuedCommand qc{};
  while (m_cmdQueue.pop(qc)) {
    m_lastCmdId = qc.id;
    RecordHistory(qc, false, detail ? detail : "cancelled");
  }
}

void DeviceRuntime::RecordHistory(const QueuedCommand &qc, bool ok,
                                  const std::string &det) {
  HistoryEntry e;
  e.timestamp = std::chrono::system_clock::now();
  e.command_id = qc.id;
  e.command_name = qc.reason;
  e.source = qc.source;
  e.success = ok;
  std::memset(e.detail.data(), 0, e.detail.size());
  std::strncpy(e.detail.data(), det.c_str(), e.detail.size() - 1);

  m_history[m_historyWriteIdx] = std::move(e);
  m_historyWriteIdx = (m_historyWriteIdx + 1) % kMaxHistoryItems;
  if (m_historyCount < kMaxHistoryItems) {
    m_historyCount++;
  }
}

// --------------- 命令执行 ---------------

bool DeviceRuntime::ShouldExecuteCommand(const QueuedCommand& qc) {
  if (qc.penGeneration == 0) {
    return true;
  }

  std::lock_guard<std::mutex> penIngressLock(m_penIngressMu);
  return m_acceptPenAfeCommands && m_penReplay.IsCurrent(qc.penGeneration);
}

bool DeviceRuntime::ExecuteCommand(const QueuedCommand& qc) {
  const bool ok = static_cast<bool>(m_chip.SendAfeCommand(qc.cmd));
  {
    std::lock_guard<std::mutex> lk(m_mu);
    m_lastCmdId = qc.id;
    RecordHistory(qc, ok, ok ? "OK" : "afe_sendCommand failed");
  }
  if (!ok) {
    LOG_WARN("Runtime", __func__, "CmdExec",
             "Command '{}' (type={}) failed — skipping (non-fatal).",
             qc.reason.data(), static_cast<int>(qc.cmd.type));
  }
  return ok;
}

bool DeviceRuntime::DrainCommands() {
  while (true) {
    QueuedCommand qc{};
    {
      std::lock_guard<std::mutex> lk(m_mu);
      if (!m_cmdQueue.pop(qc)) {
        return true;
      }
    }
    if (!ShouldExecuteCommand(qc)) {
      LOG_INFO("Runtime", __func__, "Queue",
               "Dropped stale pen command '{}' (generation={}).",
               qc.reason.data(), qc.penGeneration);
      continue;
    }
    ExecuteCommand(qc);
  }
}

// ----------- Worker 核心循环 -----------

ThreadResult DeviceRuntime::WorkerMain() {
  {
    std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMu);
    m_workerThreadId = std::this_thread::get_id();
  }

  std::function<void()> workerHook;
  {
    std::lock_guard<std::mutex> hookLock(m_workerHookMu);
    workerHook = std::move(m_workerHookForTesting);
  }
  if (workerHook) {
    workerHook();
  }

  while (true) {
    bool displayOffSuspendDue = false;
    {
      std::lock_guard<std::mutex> lk(m_mu);
      if (m_displayOffSuspendPending &&
          std::chrono::steady_clock::now() >= m_displayOffSuspendDeadline &&
          m_stopReason.load(std::memory_order_acquire) !=
              StopReason::Shutdown) {
        m_displayOffSuspendPending = false;
        SetState(workerState::suspend);
        m_needSuspendDeinit.store(true, std::memory_order_release);
        CancelQueuedCommandsLocked("cancelled: display-off suspend");
        displayOffSuspendDue = true;
      }
    }
    if (displayOffSuspendDue) {
      LOG_INFO("Runtime", __func__, "Policy",
               "DisplayOff remained active for {} ms; entering suspend.",
               kDisplayOffSuspendDelay.count());
      continue;
    }

    // ── 检查停止请求，根据 StopReason 分流到 suspend 或 quit ──
    auto reason =
        m_stopReason.exchange(StopReason::None, std::memory_order_acq_rel);
    if (reason != StopReason::None) {
      if (reason == StopReason::ScreenOff) {
        LOG_INFO("Runtime", __func__, "StopReq",
                 "StopReason::ScreenOff consumed -> suspend");
        SetState(workerState::suspend);
        m_needSuspendDeinit.store(true, std::memory_order_release);
      } else {
        LOG_INFO("Runtime", __func__, "StopReq",
                 "StopReason::Shutdown consumed -> quit");
        {
          std::lock_guard<std::mutex> penIngressLock(m_penIngressMu);
          m_acceptPenAfeCommands = false;
          m_penReplay.CompleteInitCycle();
        }
        m_stopped.store(true, std::memory_order_release);
        SetState(workerState::quit);
      }
      std::lock_guard<std::mutex> lk(m_mu);
      CancelQueuedCommandsLocked(
          reason == StopReason::Shutdown
              ? "cancelled: shutdown consumed"
              : "cancelled: screen-off suspend");
    }

    DrainCommands();

    auto curState = m_state.load(std::memory_order_acquire);
    switch (curState) {
    case workerState::ready:
      OnReady();
      break;
    case workerState::streaming:
      OnStreaming();
      break;
    case workerState::recover:
      OnRecover();
      break;
    case workerState::suspend:
      OnSuspend();
      break;
    case workerState::quit:
      if (OnQuit()) {
        LOG_INFO("Runtime", __func__, "quit",
                 "Worker exited, m_running=false.");
        {
          std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMu);
          m_workerThreadId = std::thread::id{};
          m_selfStopDetached = false;
          m_running.store(false, std::memory_order_release); // allow restart via Start()
          m_lifecycleCv.notify_all();
        }
        return ThreadResult();
      }
      break;
    }
  }
  return ThreadResult();
}

// ----------- 状态处理 -----------

void DeviceRuntime::OnReady() {
  if (m_autoMode.load()) {
    BeginPenReplayInitCycle();
    if (auto r = m_chip.Init(); !r) {
      SetState(workerState::recover);
      return;
    }
    ReplayPenStateAfterChipInit();
    SetState(workerState::streaming);
  } else {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

void DeviceRuntime::OnStreaming() {
  bool displayOffSuspendPending = false;
  {
    std::lock_guard<std::mutex> lk(m_mu);
    displayOffSuspendPending = m_displayOffSuspendPending;
  }
  if (displayOffSuspendPending) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return;
  }

  auto res = m_chip.GetFrame();
  if (res == std::unexpected(ChipError::Timeout))
    return;
  if (!res) {
    // 连续 N 帧非Timeout失败才进入 recover；
    // 某些 AFE 命令可能导致一两帧 bus 暂时失响应，不算致命。
    m_consecutiveFrameErrors++;
    if (m_consecutiveFrameErrors >= kMaxConsecutiveFrameErrors) {
      LOG_ERROR("Runtime", __func__, "Streaming",
                "{} consecutive GetFrame failures -> recover.",
                m_consecutiveFrameErrors);
      m_consecutiveFrameErrors = 0;
      SetState(workerState::recover);
    } else {
      LOG_WARN("Runtime", __func__, "Streaming",
               "GetFrame failed ({}/{}), retrying...", m_consecutiveFrameErrors,
               kMaxConsecutiveFrameErrors);
    }
    return;
  }
  m_consecutiveFrameErrors = 0; // 成功读帧重置计数

  const auto &rawData = m_chip.GetFrameBuffer();

  // 0. Build frame (zero-copy from Chip)
  Solvers::HeatmapFrame touchFrame;
  touchFrame.rawPtr = rawData.data();
  touchFrame.rawLen = Frame::kTotalFrameSize;
  // 读到帧的时刻。求解层没有别的时间来源:frame.timestamp 曾经取自 master 后缀
  // word 9,而那个字是频点码不是戳,赋值链已删。笔画层拿这个量判两次接触是不是
  // 同一条笔画,没有它就只能数自己被调用了几次——而空闲帧根本不进管线
  // (ProcessSignalConditioning 提前返回),手指抬起 600 ms 会被数成一帧。
  touchFrame.receiveSystemEpochUs = Common::CaptureSystemEpochUs();
#if EGOTOUCH_DIAG
  // Debug/App 模式: 拷贝完整帧数据供 IPC 帧推送
  touchFrame.rawData.assign(rawData.begin(),
                            rawData.begin() + Frame::kTotalFrameSize);
#endif
  touchFrame.masterWasRead = m_chip.GetLastMasterWasRead();

  const bool stylusVhfEnabled =
      m_stylusVhfEnabled.load(std::memory_order_relaxed);
  bool dispatchTouch = false;
  {
    std::lock_guard<std::mutex> lk(m_pipelineMu);

    // 3. Stylus pipeline — reads rawPtr, writes frame.stylus
    m_stylusPipeline.Process(touchFrame);
  }

  // 笔报文在触摸管线之前发出:笔是延迟敏感路径,不为触摸处理买单。触摸管线
  // 只读 frame.stylus 的 interop/output,DispatchStylus 不改这些字段,顺序
  // 提前不影响仲裁。代价是 VHF 写入阻塞时会顺延本帧触摸,可接受。
  m_vhfReporter.DispatchStylus(touchFrame, stylusVhfEnabled);

  {
    std::lock_guard<std::mutex> lk(m_pipelineMu);

    // 4. Touch pipeline — reads frame, writes contacts/packets.
    // Skipped-master frames carry fresh stylus/slave data only; do not feed
    // them into touch modules, otherwise BaselineTracker treats the missing
    // master as invalid input and clears cross-frame finger visualization state.
    if (touchFrame.masterWasRead) {
      if (m_masterParserOnly.load(std::memory_order_relaxed)) {
        m_touchPipeline.ProcessMasterParserOnly(touchFrame);
      } else {
        dispatchTouch = m_touchPipeline.Process(touchFrame);
      }

      // Serialized re-check: if parser-only was enabled between pipeline
      // processing and this check, suppress touch dispatch. The lock
      // guarantees that SetMasterParserOnlyMode's FlushTouchAllUp()
      // either hasn't happened yet (touch→all-up = correct HID order)
      // or already happened (dispatch suppressed = correct).
      if (dispatchTouch && m_masterParserOnly.load(std::memory_order_relaxed)) {
        dispatchTouch = false;
      }
    }
  }

  // VHF dispatch can block on device I/O; keep it outside m_pipelineMu.
  // touchFrame owns processed stylus/contact vectors, and rawPtr references the
  // current chip frame buffer until the next GetFrame() on this worker thread.
  if (dispatchTouch) {
    m_vhfReporter.DispatchTouch(touchFrame);
  }

  // 5. 帧推送(工作台可视化)。回调只在 IPC 进入调试模式时被装上,没装就是没人看,
  // 所以「装没装」本身就是开关,不需要再按构建类型编译掉。
  //
  // 原先这里裹着 #ifdef _DEBUG。它取决于 CRT 选的是哪一档(/MTd 隐式定义 _DEBUG),
  // 而不是取决于「要不要可视化」——工作台连上了、日志也打了「已进入调试模式」,
  // 帧却一帧不来,查到最后就是这个宏。可视化通不通不该由 CRT 决定。
  //
  // 持锁调用,好让停用侧等到在途回调结束。
  std::lock_guard<std::mutex> lk(m_framePushCbMu);
  if (m_framePushCb) {
    m_framePushCb(touchFrame);
  }
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
  // Keep state mutation and any derived AFE submission on the same side of a
  // Stop/Start boundary. Stop still permits state updates after releasing this lock.
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
      res.applyToPipeline = false;   // battery has no bearing on the solver
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
      Solvers::StylusProtocolHint nextProtocolHint = hasModuleHint
          ? ResolveProtocolHintFromPenModule(modelInfo.protocolHint)
          : Solvers::StylusProtocolHint::Auto;
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
        Solvers::StylusProtocolHint nextProtocolHint = hasModuleHint
            ? ResolveProtocolHintFromPenModule(modelInfo.protocolHint)
            : Solvers::StylusProtocolHint::Auto;
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
    bool connected = false;
    bool connectionChanged = false;
    UpdatePenState([&](RuntimePenState& state, PenStateUpdateResult& res) {
      const bool hadConnection = state.hasConnection;
      const bool oldConnected = state.connected;

      state.hasConnection = ev.semantic.hasConnection;
      state.connected =
          ev.semantic.hasConnection ? ev.semantic.connected : false;
      connected = state.hasConnection && state.connected;

      connectionChanged =
          hadConnection != state.hasConnection || oldConnected != state.connected;
      res.stateChanged = connectionChanged;
      if (connectionChanged) {
        ResetPenTransientState(state);
      }

      if (!connected) {
        const bool pipelineIdentityChanged =
            state.hasStylusId ||
            state.protocolHint != Solvers::StylusProtocolHint::Auto;
        res.stateChanged = ClearPenIdentityState(state) || res.stateChanged;
        res.applyToPipeline = connectionChanged || pipelineIdentityChanged;
      } else if (!state.protocolHintFromPenModule && state.hasStylusId) {
        state.protocolHint = ResolveProtocolHintFromStylusId(state.stylusId);
      }
    });

    if (const auto cmd = BuildPenConnectionAfeCommand(
            connectionChanged, connected)) {
      SubmitPenAfeCommandLocked(
          *cmd,
          connected ? "PenConnStatus->Init" : "PenConnStatus->Disconnect");
    }
    break;
  }

  case EC::PenFreqJump: {
    break;
  }

  case EC::PenTypeInfo: {
    const bool hasStylusId = ev.semantic.hasStylusId;
    const uint8_t commandPenType = hasStylusId ? ev.semantic.stylusId : payload0;
    const uint8_t stateStylusId = hasStylusId ? commandPenType : 0;
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

    command cmd{};
    cmd.type = AFE_Command::SetStylusId;
    cmd.param = commandPenType;
    SubmitPenAfeCommandLocked(cmd, "PenTypeInfo->SetStylusId");
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

    // 0x25 GetPenKeySupport 在本机返回掩码 1：这支笔只支持一个按键功能，而实测它只在
    // 双击时发 0x2F（单击和长按什么都不发），payload 恒为 1。所以这里不需要按 payload
    // 分派手势种类。
    if (func == 1) {
      DispatchPenButtonAction({PenButtonAction::Type::DoubleClick, false, func},
                              "PenCurrentFunc");
    }
    break;
  }

  case EC::PenAcStatus:
  case EC::PenRotateAngle:
  case EC::PenTouchMode:
  case EC::PenGlobalPreventMode:
  case EC::PenHolster:
    break;

  case EC::PenGlobalAnnotation:
    // 只发按下、不发松开，看着像 0x2F 那个「按下且永不释放」的老毛病，实际不是：
    // VhfReporter::BuildStylusPacket 用 exchange(false) 取 barrel 位，每帧消费一次，
    // 所以一次 SetBarrelButtonState(true) 天然就是一个报告周期的脉冲，无需配对的复位；
    // Win32 那条路走的 InjectBarrelPulse 本身也是脉冲。
    // 约束：消费发生在 packet 构建时，与这一帧有没有笔无关。笔不在范围内时这次脉冲
    // 被消费掉却不会写进任何报文，等于丢失。要修得先让 barrel 位跨帧保留到写出为止，
    // 那是 VhfReporter 的改动，不在这里。
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

bool DeviceRuntime::OnQuit() {
  if (m_autoMode.load()) {
    if (auto res = m_chip.Deinit(false); !res) {
      LOG_WARN("Runtime", __func__, "quit", "Chip deinit failed during quit.");
    }
  }
  return true; // signal WorkerMain to return
}

void DeviceRuntime::OnSuspend() {
  // 首次进入 suspend 时执行 HoldReset（拉低 reset，关闭中断通道）
  if (m_needSuspendDeinit.exchange(false, std::memory_order_acq_rel)) {
    m_chip.HoldReset();
    LOG_INFO("Runtime", __func__, "suspend",
             "Entered suspend, chip reset held low. Waiting for wake event.");
  }
  // 低功耗等待，每 100ms 检查一次状态变更
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void DeviceRuntime::OnRecover() {
  m_recoverCount++;

  // 最大重试 30 次（500ms 间隔 ≈ 15 秒恢复窗口）
  if (m_recoverCount > 30) {
    LOG_ERROR("Runtime", __func__, "Recover",
              "Exceeded 30 recovery attempts, entering suspend.");
    m_recoverCount = 0;
    m_needSuspendDeinit.store(true, std::memory_order_release);
    SetState(workerState::suspend);
    return;
  }

  // 等待 500ms 再重试，给硬件从休眠/灭屏恢复的时间
  // 期间每 50ms 检查一次 stop 请求以保持响应性
  for (int i = 0; i < 10; ++i) {
    if (m_stopReason.load(std::memory_order_relaxed) != StopReason::None)
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  LOG_INFO("Runtime", __func__, "Recover", "Recovery attempt {}/30...",
           m_recoverCount);

  if (auto res = m_chip.check_bus(); !res)
    return;
  BeginPenReplayInitCycle();
  if (auto res = m_chip.Init(); !res)
    return;
  ReplayPenStateAfterChipInit();

  LOG_INFO("Runtime", __func__, "Recover",
           "Recovery succeeded after {} attempts.", m_recoverCount);
  SetState(workerState::streaming);
  m_recoverCount = 0;
}

void DeviceRuntime::SetState(workerState newState) {
  workerState old = m_state.exchange(newState, std::memory_order_acq_rel);
  if (old != newState) {
    LOG_INFO("Runtime", __func__, "StateTransition", "State changed: {} -> {}",
             ToString(old), ToString(newState));
  }
}
