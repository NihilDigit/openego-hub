# 触控栈重构参考：Chromium 对照与 EGoTouchRev 现状

## 0. 这份文档的用途与边界

这份文档面向接手触控栈重构的实现者，目的是省去两件事：通读两套代码库，以及重新做一遍事实核对。

文档只陈述事实与对应关系，**不规定重构方向**。哪些该改、改成什么样、按什么顺序做，由实现者自行判断。文中出现的 Chromium 设计是作为一个已在量产设备上运行多年的参照点列出的，不构成「应当照此实现」的结论；多处明确标注了不可直接移植的原因。

所有结论都带 `文件:行号`。标注「未确认」的条目表示本次核对没有得到确定答案，实现时需要自行验证，不要当作已知事实使用。

Chromium 侧行号对应 2026-08-23 从 `chromium/chromium` 默认分支取得的快照，与下节所列本机副本一致。上游代码会变动，引用前请以本机副本为准。

---

## 1. 代码位置

### 1.1 Chromium 源码副本

已下载到本机，无需重新获取：

```
C:\Users\rosetta\AppData\Local\Temp\claude\C--Codes-EGoTouchRev\34c7f03e-5312-4ca9-9a69-fc877eff8d38\scratchpad\
├── chromium_palm\       掌抑制全套（含 touch_event_converter_evdev.cc 副本 tec.cc）
├── chromium_evdev\src\  evdev 触摸事件通路
├── chromium_gesture\    手势识别层（21 个文件）
├── chromium_prediction\src\  预测与重采样
└── chromium_stylus\src\ 笔相关
```

补充获取的方式（网页抓取会被 Anubis 拦截，需走 GitHub API）：

```bash
gh api "repos/chromium/chromium/contents/<路径>" --jq '.content' | base64 -d > <文件>
gh api "repos/chromium/chromium/contents/<目录>" --jq '.[].name'      # 列目录
```

上游路径：

| 主题 | 路径 |
|---|---|
| evdev 触摸通路 | `ui/events/ozone/evdev/touch_event_converter_evdev.{h,cc}` |
| 掌抑制 | `ui/events/ozone/evdev/touch_filter/` |
| 掌抑制模型权重 | `ui/events/ozone/evdev/touch_filter/palm_model/`（`*_inference.cc` 为 1.05 MB 生成代码，无阅读价值） |
| 手势识别 | `ui/events/gesture_detection/` |
| 速度估计 | `ui/events/velocity_tracker/`（已从 gesture_detection 拆出） |
| 预测与重采样 | `ui/base/prediction/`、`third_party/blink/renderer/platform/widget/input/` |
| 笔（数位板） | `ui/events/ozone/evdev/pen_tablet_event_converter_evdev.{h,cc}` |
| 笔（按键设备） | `ui/events/ozone/evdev/stylus_button_event_converter_evdev.{h,cc}` |

注意：任务开始时假定的 `touch_noise/` 目录在当前上游已改名为 `touch_filter/`，`TouchNoiseFinder` 已改名为 `FalseTouchFinder`。旧资料按旧名检索会落空。

### 1.2 EGoTouchRev 关键文件

触摸管线，`EGoTouchService/Solvers/TouchSolver/`：

| 文件 | 行数 | 职责 |
|---|---:|---|
| `TouchPipeline.cpp` / `.h` | 655 / 120 | 级序编排、配置绑定与注入 |
| `MasterFrameParser.hpp` | 58 | 帧解析 |
| `BaselineTracker.hpp` | 365 | 自适应基线 |
| `CMFProcessor.hpp` | 158 | 共模滤波 |
| `MacroZoneDetector.hpp` | 143 | BFS 连通域 |
| `PeakDetector.hpp` | 432 | 峰值检测 |
| `TouchClassifier.hpp` | 257 | 掌/指分类（zone 级与 peak 级） |
| `PalmBoxSuppressor.hpp` | 434 | 掌框跟踪与抑制 |
| `ZoneExpander.hpp` | 711 | 区域展开与接触点生成 |
| `ContactExtractor.hpp` | 173 | 接触点提取与尺寸估计 |
| `EdgeCompensation.hpp` | 437 | 边缘补偿 `EdgeCompensator` 与边缘拒绝 `EdgeRejector` |
| `StylusTouchSuppressor.hpp` | 220 | 笔/触仲裁 |
| `TouchTracker.hpp` | 1150 | 轨迹跟踪 |
| `CoordinateFilter.hpp` | 99 | 1-Euro 坐标滤波 |
| `TouchGestureStateMachine.hpp` | 279 | 五相位手势状态机 |
| `GhostSuppressor.hpp` | 89 | 鬼点抑制 |
| `TouchSharedTypes.h` | 134 | 跨级共享数据类型 |
| `TouchDiagnosticCache.hpp` | 137 | 诊断缓存 |

共享类型：`EGoTouchService/Solvers/TouchFrameTypes.h`（`TouchContact`、`FixedVector`、`HeatmapFrame`）。

笔求解器，`EGoTouchService/Solvers/StylusSolver/`：`AsaTypes.hpp`、`StylusPipeline.{h,cpp}`、`StylusRuntimeCommit.hpp`，以及 `shared/`（10 个文件，含 `LinearFilterProcess.hpp`、`CoorIIRProcess.hpp`、`AftCoorProcess.hpp`、`StylusTouchArbiter.hpp`）、`hpp2/`（12 个）、`hpp3/`（8 个）。

配置：`Common/include/config/ConfigKeyId.h`（数值 id，Touch 段 `0x0100-0x01FF`，Stylus 段 `0x0200-0x02FF`）、`Common/source/config/ConfigKeyMap.cpp`（id ↔ yaml 路径）。

测试：`EGoTouchService/Solvers/tests/`，见第 8 节。

---

## 2. 管线级序对照

### 2.1 Chromium

一帧 = 两个 `SYN_REPORT` 之间的全部 evdev 事件。入口是 fd 可读回调，出口是 `DispatchTouchEvent`。

| 级 | 位置 | 职责 |
|---|---|---|
| 读取 | `touch_event_converter_evdev.cc:448-475` | 一次 `read()` 最多 `kNumTouchEvdevSlots*6+1` 个 `input_event` |
| 单点补 MT 协议 | `:525-543` | 无多点能力的设备把 `ABS_X/Y` 改写为 `ABS_MT_*`，`BTN_TOUCH` 改写为 `ABS_MT_TRACKING_ID`，下游只有 MT 一条路径 |
| 分发 | `:500-523` | `EV_SYN`/`EV_ABS`/`EV_KEY` 分派 |
| 写入 slot 状态 | `:574-630`、`:545-572` | 只写 `events_[current_slot_]` 字段并置 `altered`，不产生输出 |
| 延迟过滤 | `:753-754` | `FalseTouchFinder::HandleTouches` → `slots_should_delay_` |
| 掌过滤 | `:767-770` | `palm_detection_filter_->Filter(events_, timestamp, &hold, &suppress)` |
| 合并判定 | `:771-784` | `IsPalm()` → cancelled；hold → held；suppress → cancelled |
| 出队/上报 | `:790-842` | held 入队 / cancel 时丢弃队列 / 否则补发队列再发当前帧 |
| 生成事件类型 | `:998-1018`、`:647-675`、`:677-689` | 由 `(was_alive, is_alive)` 推导事件类型并派发 |

时间戳取自内核事件自身（`event_converter_evdev.cc:280-287`），不用接收时刻。

**这一层不做任何坐标滤波、去抖或预测。** 坐标只在出口做一次平移 `event.x - x_min_tuxels_`（`:687`）。预测在更上层的 `ui/base/prediction/`，与输入设备类型无关。

### 2.2 EGoTouchRev

`TouchPipeline.cpp:43-115`，五个阶段：

```
ProcessFrameParser        :43-50    m_frameParser
ProcessSignalConditioning :52-66    m_baseline → m_cmf
GenerateContacts          :68-86    m_macroZoneDet → m_peakDet
                                    → m_touchClassifier → m_palmBoxSuppressor
                                    → m_contactExtractor
PostProcessContacts       :88-93    m_edgeComp → m_edgeReject → m_stylusSuppress
ProcessTrackingAndGesture :101-113  m_tracker → m_coordFilter → m_gesture
```

信号调理级带短路：无手指且无存活触摸状态时走 `ResetIdleOutputs` 直接返回（`:59-65`）。

### 2.3 职责边界的差异

两边的分界线位置不同，这决定了很多机制无法一一对应：

- Chromium 从固件拿到的是**已经成形的接触点**（slot、tracking id、major/minor 椭圆、压力均由控制器给出）。它的全部工作从接触点开始。
- EGoTouchRev 从**原始热力图**开始，接触点由 `MacroZoneDetector` → `PeakDetector` → `ZoneExpander` → `ContactExtractor` 自行生成。Chromium 管线的输入相当于 EGoTouchRev 的 `PostProcessContacts` 入口。

因此：
- Chromium 的 `touch_filter/` 全部工作在接触点级；EGoTouchRev 的掌抑制主要工作在 peak / macro zone 级（`TouchClassifier`、`PalmBoxSuppressor`），在接触点生成**之前**。
- Chromium 拿不到热力图（`HeatmapPalmDetector` 是 2025 年新增的旁路，经 hidraw 单独读取，见 4.3）；EGoTouchRev 天然有热力图。
- Chromium 的 tracking id 由内核给出；EGoTouchRev 需要自己做轨迹匹配（`TouchTracker`）。

---

## 3. 数据结构对照

### 3.1 `InProgressTouchEvdev` ↔ `TouchContact`

`touch_evdev_types.h:21-79` ↔ `TouchFrameTypes.h:43-79`。

Chromium 侧一个实例对应一个 slot，**永不销毁，只被覆写**，并携带一组 `was_*` 影子字段记录上一帧状态：

```
touching / was_touching
cancelled / was_cancelled
delayed / was_delayed
held / was_held
altered            本帧是否被写过
```

`touch_is_alive = touching && !delayed && !cancelled`
`touch_was_alive = was_touching && !was_delayed && !was_cancelled`

事件类型由这两个布尔的 2×2 组合唯一决定（`touch_event_converter_evdev.cc:647-675`）：

| was_alive | is_alive | 事件 |
|---|---|---|
| 否 | 否 | `kUnknown`（不派发） |
| 否 | 是 | `Pressed` |
| 是 | 否 | `cancelled` ? `Cancelled` : `Released` |
| 是 | 是 | `Moved` |

EGoTouchRev 侧 `TouchContact` 每帧由 `ZoneExpander::ComputeCentroidsAndContacts` 重建（`ZoneExpander.hpp:613` 处 `contacts.clear()`），跨帧状态存在 `TouchTracker::TrackState` 与 `GestureSlot` 里，不在 `TouchContact` 上。

注意 `FixedVector::clear()` 只重置长度，底层数组的旧对象保留（`TouchFrameTypes.h:92`）。任何「每帧应当重新计算」的 `TouchContact` 字段都必须无条件写入，漏写会读到上一帧的值。`EdgeRejector::Process` 因此对 `edgeRejected` 做无条件清零（`EdgeCompensation.hpp` 内 `for (auto& tc : contacts) tc.edgeRejected = false;`）。

EGoTouchRev 表达「是否上报」用四个字段，语义有重叠，实现者需要留意：`isReported`、`reportEvent`、`lifeFlags`、`debugFlags`。

### 3.2 `PalmFilterStroke`：EGoTouchRev 无对应结构

`neural_stylus_palm_detection_filter_util.h:57-121`。按 tracking id 聚合一条笔画的采样序列，是 Chromium 掌抑制的核心数据结构：

```cpp
std::deque<PalmFilterSample> samples_;   // 按 max_sample_count 裁剪
uint64_t samples_seen_;                  // 累计采样数，与 samples_.size() 不同
base::TimeTicks first_sample_time_;
gfx::PointF unscaled_centroid_;          // Kahan 求和累加，抵消浮点误差
```

单个采样 `PalmFilterSample`（`:33-50`）：`major_radius`、`minor_radius`、`pressure`、`edge`、`tracking_id`、`point`、`time`。

EGoTouchRev 的掌判定是逐帧独立的，没有跨帧聚合的笔画结构。`PalmBoxSuppressor` 的 `TrackedPalmBox` 有 `age`/`missed`（`PalmBoxSuppressor.hpp:23-24`），但那是掌框的跟踪状态，不是采样序列。

### 3.3 设备物理量描述

`PalmFilterDeviceInfo`（`neural_stylus_palm_detection_filter_util.h:21-27`）：`max_x`、`max_y`、`x_res`、`y_res`、`major_radius_res`、`minor_radius_res`、`minor_radius_supported`。

Chromium 用这些把固件上报值换算到毫米，模型特征全部工作在毫米域。半径换算带一次坐标系对齐：`ABS_MT_TOUCH_MAJOR/MINOR` 的 resolution 可能与位置轴不同，`GetFingerSizeScale` 预算比例（`touch_event_converter_evdev.cc:104-114`）；`ABS_MT_ORIENTATION` 为 0 时 major/minor 与 x/y 轴对调（`:1068-1077`）。orientation 只支持 0/1 两值的简化上报，其余视为不支持（`SupportsOrientation`，`:1062-1066`，附 TODO b/185318572）。EGoTouchRev 对应的是 `ContactExtractor::TouchSizeCalculator::m_pixelPitchMm`（默认 4.5，配置键 `touch.zone_contact.touch_size_pixel_pitch_mm`）。

传感器网格 60×40（`EdgeCompensation.hpp:15-16` 的 `kGridColMax = 59`、`kGridRowMax = 39`）。

**网格间距有实测依据，且不是方形的。** `StylusSolver/shared/AftCoorProcess.hpp:33-46` 的注释记录了标定来源：`m_sensorDim1 = 4320`、`m_sensorDim2 = 2640` 取自固件 project block 的 `0xA2C`（TX 轴跨距）与 `0xA2A`（RX 轴跨距），单位 1/16 mm，即 **270 × 165 mm**，与本机 12.35 吋 16:10 面板（约 266 × 166 mm）吻合。注释还说明这个轴向映射不是猜的：跨 202 个 `Asa*` project 核对 span/count，此方向对 131 个给出自洽的间距，反向映射一个都不匹配。

据此：

```
列间距（TX） = 4320 / 16 / 60 = 4.5   mm
行间距（RX） = 2640 / 16 / 40 = 4.125 mm
```

`ContactExtractor::TouchSizeCalculator::m_pixelPitchMm = 4.5` 对列方向正确，对行方向偏大 9%。`areaMm2 = area * m_pixelPitchMm²` 用的是 `4.5² = 20.25`，而单格实际面积是 `4.5 × 4.125 = 18.5625`，**整体高估约 9%**。以 mm² 为单位标定阈值时需要把这一项计入，或改成两轴分别持有间距。

同一段注释还记录了这两个值曾被错填为 `0x0A00 / 0x0640`（2560/1600，即屏幕分辨率而非物理跨距），导致 `ScaleLockThreshold` 算出的阈值大约 1.7 倍。

---

## 4. 逐主题对照

各节按主题组织而非按管线顺序。两节标注了「Chromium 无对应」——4.5 轨迹跟踪与 4.12 前段——那是 EGoTouchRev 因为直接消费热力图而多出来的部分，Chromium 的输入从成形的接触点开始。想按管线顺序读的话，次序是 4.12 → 4.4 → 4.2 → 4.3 → 4.5 → 4.9 → 4.8。

### 4.1 hold / delay / cancel

Chromium 有三种「不上报」，语义各不相同，EGoTouchRev 三者都没有对应实现。

**delay**（`FalseTouchFinder`）：触摸点暂不出生，**不缓存任何东西**。`delayed` 参与 `is_alive` 计算，所以延迟中的触摸对上层等价于「尚未按下」。解除后从**当前状态**发 `Pressed`，之前的帧不补发。

**hold**（`PalmDetectionFilter` 的 `slots_to_hold`）：事件连同时间戳压入 `held_events_[slot]` 队列（`touch_event_converter_evdev.cc:797-808）。两种结局：

- 后续被 cancel：**整个队列丢弃**，并用队首事件的 `was_*` 复原状态（`:809-823`），上层从头到尾没见过这条笔画，也不需要收 `Cancelled`。
- 正常放行：**按原时间戳整队回放**（`:825-835`），每条置 `was_held = true`。

队列无长度上限；延迟上界由模型的 `max_sample_count` 决定（alpha 6 采样、beta 12 采样，@120Hz 约 50 ms / 100 ms）。

**cancel**：事件已经发出去之后的撤回。`cancelled` 置位后，`GetEventTypeForTouch` 在 `was_alive && !is_alive && cancelled` 分支返回 `kTouchCancelled` 发一条取消事件（`:666-671`）；随后 `was_cancelled` 置真，**同一条笔画后续所有帧都在 `:656-659` 被判 `kUnknown` 静默**，直到 `UpdateTrackingId` 遇到新的 tracking id 才清除（`:1037-1040`）。也就是说取消一次之后这条笔画就永久沉默，不会再产生任何事件，也不需要下游自行去重。

`DetectRepeatedTouch`（`:875-915`）另记录被取消触摸的位置：2 秒内、7 mm 半径内（`kRepeatedTouchThresholdInSquareMillimeter = 7.0*7.0`）出现在刚被取消处的新触摸会被计数。当前只用于 UMA 指标，不参与判定。

Chromium 默认行为是**任一点被判掌就取消整帧全部触点**（`MaybeCancelAllTouches`，`:691-707`）。注释写明这是对上层无法处理单点取消的迁就，带 TODO；`kEnableSingleCancelTouch` 默认关（`:190-192`）。这一条是对 Chromium 上层能力的补偿，不是算法设计。

EGoTouchRev 现状：`EdgeRejector` 与掌抑制都只能在接触点生成或建轨迹之前**丢弃**，一旦上报无法撤回。`ZoneExpander::AllowContactPeak`（`ZoneExpander.hpp:229-232`，调用点 `:50`、`:424`、`:539`）在 peak 级拦截，`TouchTracker` 新建轨迹分支的 `if (o.edgeRejected) continue;` 在轨迹级拦截。

### 4.2 掌抑制

#### Chromium 侧

四个 filter，工厂在 `palm_detection_filter_factory.cc`：

| filter | 文件 | 机制 | 主线默认 |
|---|---|---|---|
| `OpenPalmDetectionFilter` | `open_palm_detection_filter.cc` | 全放行 | 未启用其他时的兜底 |
| `HeuristicStylusPalmDetectionFilter` | 同名 `.cc`，89 行 | 纯时间关系，不看形态 | 关 |
| `NeuralStylusPalmDetectionFilter` | 同名 `.cc`，633 行 | 笔画级特征 + 神经网络 | 关 |
| `HeatmapPalmDetectionFilter` | 同名 `.cc`，197 行 | 经 hidraw 读电容热图 | 关（`kHeatmapPalmDetection = false`） |

**主线默认实际生效的只有固件判据**：`tool_type == MT_TOOL_PALM`，或 `major == major_max_`（长轴顶到量程上限）（`touch_event_converter_evdev.cc:709-732`）。启用神经网络 filter 时这两条会被一起关掉（`:217-224`）。

「长轴饱和即掌」这条把量程饱和本身当作证据，是一个可注意的判据形式。

神经网络 filter 的结构：

- 按 tracking id 维护 `PalmFilterStroke`，凑够 `max_sample_count` 跑**一次**推理，一条笔画只判一次（`:309-318` `ShouldDecideStroke`）。
- 三个决策点：早期采样（alpha 在第 2 个采样）跑推理命中置 **hold**（`:270-279`）；短笔画走启发式命中置 **hold**（`:264-269`）；到 `max_sample_count` 或抬起时强制判 → **suppress**（`:282-299`）。
- 判定结果对笔画粘着：`if (!is_palm_.test(slot) && ShouldDecideStroke(stroke))`。
- 逐采样特征 5 维（`kFeaturesPerSample = 5`，`:432-444`）：`major_radius`、`minor_radius`（不支持时取 major）、与上一采样的欧氏位移、到最近边缘的距离（mm）、存在位恒 1。笔画级追加：填充比例 `size / max_sample_count`、首尾直线距离、起始序号（`:451-463`）。启用会话计数时还带上会话内 tracking id 总数与当前活跃 tracking id 数。
- 启用重采样时走 `AppendResampledFeatures`（`:471-516`），按 `config.resample_period` 等间隔取点。`GetSampleAt` 在相邻采样间**只插值 x、y 和 edge，`major`/`minor`/`pressure` 直接取靠后那个采样的原值**（`neural_stylus_palm_detection_filter_util.cc:82-104`）——尺寸类量不做插值。
- 邻居特征（`:378-424`）：取最大的 N 个邻居，各自的完整特征加上「存在位 + 距离」前缀。
- `EraseOldStrokes` 按 `max_dead_neighbor_time = 100 ms` 清理，**已抬起的笔画在 100 ms 内仍参与邻居判定**。

短笔画启发式（`:320-362`），不进模型：

```
MaxMajorRadius() >= heuristic_palm_touch_limit         // 20.0 mm
BiggestSize()    >= heuristic_palm_area_limit          // 400.0 mm²
max_neighbor_distance_in_mm 内最大邻居的面积超限
```

模型配置数值（`palm_model/onedevice_train_palm_detection_filter_model.cc:69-101`）：

| 参数 | alpha (kohaku) | beta (redrix) |
|---|---:|---:|
| `heuristic_palm_touch_limit` | 20.0 mm | 20.0 mm |
| `heuristic_palm_area_limit` | 400.0 mm² | 400.0 mm² |
| `max_dead_neighbor_time` | 100 ms | 100 ms |
| `max_blank_time` | 100 ms | 100 ms |
| `nearest_neighbor_count` | 0 | 0 |
| `biggest_near_neighbor_count` | 4 | 4 |
| `max_neighbor_distance_in_mm` | 100 | 200 |
| `min_sample_count` | 3 | 5 |
| `max_sample_count` | 6 | 12 |
| `neighbor_min_sample_count` | 1 | 5 |
| `output_threshold` | 0.90271 | 4.465 |
| 特征向量长度 | 173 | 325 |
| `nn_delay_start_if_palm` | true | — |
| `early_stage_sample_counts` | {2} | — |

**模型权重不可移植。** 它消费固件上报的 major/minor 半径与压力，并按设备的 `major_radius_res`、`x_res` 归一化（`PalmFilterDeviceInfo`）。EGoTouchRev 的输入是热力图与自行提取的接触点，量纲与统计分布都不同。可参照的是特征设计与决策时序，不是权重。

`HeatmapPalmDetectionFilter` 本身不判掌，只记录每条笔画见过多少采样，达到 `max_sample_count` 后 `ShouldRunModel` 返回 false（`:164-167`）；真正判定在 `HeatmapPalmDetector::IsPalm`，实现位于 ChromeOS 的 ML service（platform2），**不在 chromium/chromium 仓库内**。仅支持 kRex（9 采样）与 kGeralt（12 采样）两款机器（`palm_detection_filter_factory.cc:333-341`）。

#### EGoTouchRev 侧

链路完整且接线通，逐帧工作：

```
TouchClassifier::AnalyzeZones   → MacroZoneFeature::palmClass   (TouchClassifier.hpp:175-181)
TouchClassifier::EvaluatePeaks  → PeakEvaluation::palmClass     (:223-231)
                                → PeakEvaluation::allowContact  (:234-235)
PalmBoxSuppressor               → 触及掌框的 peak allowContact = false
                                  (PalmBoxSuppressor.hpp:403-407)
ZoneExpander::AllowContactPeak  → 拦掉 allowContact == false 的 peak
                                  (ZoneExpander.hpp:229-232，调用点 :50/:424/:539)
```

`TouchPalmBoxSuppressTest.cpp:102-107` 已端到端覆盖这段接线：跑完 suppressor 再跑 `ZoneExpander`，断言 `contacts.empty()`。

zone 级评分（`TouchClassifier.hpp:154-183`）与 peak 级评分（`:208-232`）都是加权求和后按阈值分档，档位为 `PalmLikely` / `PalmCandidate` / `FingerLikely` / `Ambiguous`。

面积阈值（单位是传感器格数，按 4.5 mm 间距换算）：

| 成员 | 格数 | ≈ mm² | 是否可配 |
|---|---:|---:|---|
| `m_candidateAreaThreshold` | 35 | 709 | 否 |
| `m_likelyAreaThreshold` | 55 | 1114 | 否 |
| `m_areaThreshold` | 50 | 1012 | 是（`touch.classifier.area_threshold`） |

作为对照，Chromium 短笔画启发式认定「铁定是掌」的面积是 400 mm²。两者的面积定义不同——Chromium 取固件上报椭圆，EGoTouchRev 取 BFS 阈值以上的格数，后者随阈值变化，对软接触偏小——**这个数值对比只能作为量级参考，不能直接作为标定依据**。

`PalmBoxSuppressor` 的掌框跟踪有 `age`/`missed`，但 `m_maxHoldFrames` 默认 0（`PalmBoxSuppressor.hpp:44`），即不设超时保持。没有与 Chromium `max_dead_neighbor_time` 对应的「已消失的掌仍参与判定」机制。

### 4.3 边缘处理

Chromium `EdgeTouchFilter`（`touch_filter/edge_touch_filter.cc`）：

- 新触点落在距边框 ≤ `kMaxBorderDistance = 1`（设备坐标单位）时置 **delay**（`:200-206`）。
- **坐标一旦与起点不同即永久解除**（`:235-236`）。

这条判据直接编码「真实手指会移动，边框漏电产生的静止假点不会」。默认不启用，需命令行 `--edge-touch-filtering`，否则 `FalseTouchFinder::Create` 返回 `nullptr`（`false_touch_finder.cc:106-115`）。

EGoTouchRev `EdgeRejector`（`EdgeCompensation.hpp:383-437`）：

- 判据是「接触点贴在边界上，且 `EdgeCompensator` 没能把坐标从边界拉开」——即 `ecFlags` 的 `0x100`/`0x200` 未置位且距边界 ≤ `m_edgeMargin`（默认 2 格）。
- 结果写 `TouchContact::edgeRejected`，由 `TouchTracker` 在建轨迹前丢弃。
- 解除路径是间接的：`EdgeCompensator` 能修正该点时 `edgeRejected` 变为 false。

两者判据不同：Chromium 看**位移**，EGoTouchRev 看**补偿算法是否生效**。后者依赖 `EdgeCompensator` 工作正常。

`EdgeCompensator`（`EdgeCompensation.hpp:239-381`）是从固件逆向来的 CTD_EC 实现，含 256 项对数 LUT `g_ctd256Ln`（`:148-181`）与四个方向的分段配置 `g_defaultECProfiles`（`:184-189`）。分段阈值为 64/128/255，索引量是 `touchSizeByte = min(sizeMm, 255)`；`sizeMm` 实际取值远小于 64，**分段选择恒为 segment 0**，其余三段是死配置。

### 4.4 尺寸与物理量

`ContactExtractor::TouchSizeCalculator`（`ContactExtractor.hpp:113-131`）产出两个量：

- `TouchContact::sizeMm`：**拟合值**，`max(fallback, cbrt(signalSum) * signalScale)`，`signalSum` 为 0 时退化为 `max(fallback, sqrt(area) * areaScale)`。公式在 `TouchSharedTypes.h` 的 `EstimateContactSizeMm`。`signalSum` 同时受接触面积与按压力度影响，所以这个量分不开「用力的指尖」与「轻搭的掌根」。
- `TouchContact::areaMm2`：**测量值**，格数 × `m_pixelPitchMm²`，只随接触面积变化。

默认参数：`m_fallbackSizeMm = 1.0`、`m_sizeAreaScale = 0.22`、`m_sizeSignalScale = 0.35`、`m_pixelPitchMm = 4.5`。

`sizeMm` 的消费方与该值的可信度有关的注意点：

- `EdgeCompensator` 用它选 LUT 分段（`EdgeCompensation.hpp:360`），如上所述恒为 segment 0。
- `StylusTouchSuppressor::EstimateSizeMm`（`StylusTouchSuppressor.hpp:64-69`）优先取 `touch.sizeMm`。其弱尺寸判据 `sizeMm < m_stylusAftWeakSizeThresholdMm`（1.2）等价于 `signalSum < 40`，被同一个 `||` 里的 `signalSum < m_stylusAftWeakSignalThreshold`（240）完全覆盖，独立生效不了。
- `TouchTracker` 在两条分支上都用 `EstimateSizeMm(o.area, o.signalSum)` 重算并覆盖 `o.sizeMm`（`TouchTracker.hpp` 的匹配分支与新建分支），轨迹上取的是生命期内的最大值。
- `TouchTracker.hpp:675-676` 与 `:712-714` 的注释明确说明该值是拟合而非测量，并因此把 `sizeMm` 分支排除在 pen mode 抑制判据之外。

### 4.5 轨迹跟踪（EGoTouchRev 独有，Chromium 无对应）

Chromium 的 slot 与 tracking id 由内核给出，没有匹配问题。EGoTouchRev 需要自行把本帧接触点与上一帧轨迹配对，这部分是 `TouchTracker::Process`（`TouchTracker.hpp:821-1148`），实现者需要完整理解。

**七个阶段**

| 阶段 | 行号 | 内容 |
|---|---|---|
| 0 全局短路 | `:822-850` | 笔压制全局开关；`!m_enabled` 的直通模式逐帧顺序发号、一律 `TouchStateDown`、`isReported = !c.edgeRejected` |
| 1 输入裁剪 | `:852-878` | 截断到 `min(m_maxTouchCount, kMaxTracks=20)`；网格 `kRows=40, kCols=60, kEdgeMargin=2.0f` 硬编码；把已有轨迹拆成 `activePrev` 与 `silentPrev` 两个子集 |
| 2 active 匹配 | `:880-927` | 匈牙利/贪心求解 → 逐条复核门限（全局最优可能单条超限，`:882-890`）→ 单点 bootstrap 放宽（`:892-897`）→ always-match 兜底（`:899-926`） |
| 3 gap relink | `:929-962` | 仅对 `silentPrev`；要求双向互为最优（`:956`）且两个方向都通过歧义检查 |
| 4 已匹配输出 | `:970-1039` | 构造输出接触点、笔压制判定、`debugFlags` 三选一 |
| 5 新建轨迹 | `:1041-1092` | `AllocateId` → `edgeRejected` 拦截（`:1056`）→ down 拒收（`:1057-1062`） |
| 6 未匹配轨迹 | `:1094-1120` | 进 SilentGap 或产出 up |
| 7 收尾 | `:1123-1147` | `GhostSuppressor::ProcessTracked` → 写回 → 回写笔压制帧数 |

**匹配代价函数** `ComputeAssignmentCost`（`:286-330`）：基础是到预测点的距离平方（`:293`），超门限返回不可行（`:295`）；叠加反向运动惩罚（`:304`，上限 25）、速度突变惩罚（`:305-309`，上限 25）、`signalSum` 相对差 ×6、`area` 相对差 ×4、`sizeMm` 差平方 ×0.5（上限 9）、`sourcePeakId` 不一致惩罚（上限 10）。

**门限** `ComputeTrackGateSq`（`:251-284`）以 `m_maxTrackDistance`（默认 4.985 格）为基准，叠乘法因子：边缘 ×`m_edgeTrackBoost = 1.5`、小尺寸接触 ×`m_accThresholdBoost = 4.0` 或 ×2；SilentGap 相位另加速度相关量。

`m_alwaysMatchDistance`（默认 2.0）只用于 `:899-926` 的兜底，绕过代价函数，且排除边缘接触。

**`TrackPhase` 只有两个值**（`:87`）：`Active = 0`、`SilentGap = 1`。

SilentGap 语义是「轨迹在传感器上暂时消失，但按预测位置继续存在」。每帧产出一个隐藏接触 `BuildSilentGapContact`（`:467-501`）：`state = TouchStateMove`、`isReported = false`、`lifeFlags |= TouchLifeSilentGap`，坐标是 `x + vx * (m_predictionScale * gapFrames)` 并 clamp 到网格。退出条件是被匹配（回 Active）或 `gapFrames` 超过 `m_gapRelinkWindowFrames`（默认 4）。

**up 事件有两条通路，只有一条真正生效：**

- 通路 A，tracker 层 `BuildLiftOffContact`（`:503-528`）：`reportEvent = TouchReportUp`、`lifeFlags = TouchLifeLiftOff`，坐标取轨迹最后位置。
- 通路 B，手势层（`TouchGestureStateMachine.hpp:167-182`）：另造一个 `TouchContact`，坐标用 `slot.lastOutputX/Y`。

通路 A 的产物会被手势层在 `:138` 无条件压掉（`if (c.state == TouchStateUp) { c.isReported = false; continue; }`）。tracker 的 up 只起「让槽位在 `:113-116` 看不到接触」的作用，**真正发给系统的 up 一律来自通路 B**。

通路 B 在接触数组满时 `try_push_back` 失败，整个 `Process` 返回 false（`:178-180`）。

**「事件已发出、事后撤回」的通路不存在。** 一旦发出 `TouchReportDown` 或 up，没有任何机制能收回。现有的三处都是发出**之前**的预防性拦截：`edgeRejected`（`TouchTracker.hpp:1056`）、down 拒收（`:1057-1062`）、`stableFrames` 未达标时不发 down（`TouchGestureStateMachine.hpp:148-152`）。

### 4.6 事件语义字段

`TouchFrameTypes.h` 中四个字段表达重叠的信息，实现者需注意：

| 字段 | 定义 | 真实读取方 |
|---|---|---|
| `isReported` | `:59` | 有，控制流判据 |
| `reportEvent` | `:78` | 有，事件类型 |
| `lifeFlags` | `:76` | 只有 `TouchLifeSilentGap` 一位被消费（`TouchGestureStateMachine.hpp:108`、`:139`），另传给 `GhostSuppressor`（`TouchTracker.hpp:1129`）。其余位纯诊断 |
| `debugFlags` | `:64` | **仓库内无读取点** |

`lifeFlags` 与 `debugFlags` 信息上高度冗余：`TouchLifeNew ↔ 0x02`、`TouchLifeSilentGap ↔ 0x20`、`TouchLifeLiftOff ↔ 0x04`、gap relink ↔ `0x21`。

`isReported` 与 `reportEvent` 双重表达：`isReported == false` 时 `reportEvent` 一律被同时置 `TouchReportIdle`（`TouchGestureStateMachine.hpp:128`、`:142`、`:147`、`:152`），两者始终同步。

对照 Chromium：那边用 `touching`/`delayed`/`cancelled`/`held` 四个正交的布尔加一组 `was_*` 影子字段，事件类型由 `(was_alive, is_alive)` 的 2×2 表唯一推导（`touch_event_converter_evdev.cc:647-675`），不存在两个字段表达同一件事的情况。

### 4.7 速度估计

Chromium `VelocityTracker`（`ui/events/velocity_tracker/velocity_tracker.{h,cc}`）：

- 九种策略，默认 `LSQ2`（二阶最小二乘），Aura 平台用 `LSQ2_RESTRICTED`（`gesture_configuration_aura.cc:62`）。
- 头文件逐条写了质量评价：`LSQ1` 为 POOR（欠拟合、低估速度），`LSQ2` 为 VERY GOOD，`LSQ3` 为 UNUSABLE（过拟合，抬手瞬间发散）。并注明「改默认策略要非常小心，往往会显著恶化体验」（`velocity_tracker.h:47-55`）。
- 采样窗口 `kHorizonMS = 100 ms`，历史缓冲 `kHistorySize = 20` 环形。
- 阶数被样本数压低 `degree = min(degree_, m-1)`（`:631-633`）。
- **停止检测**：两次 MOVE 间隔 ≥ `kAssumePointerMoveStoppedTimeMs = 40 ms` 清空全部历史（`:293-299`）；UP 与上一事件间隔 ≥ `kAssumePointerUpStoppedTimeMs = 80 ms` 同样清空（`:174-177`）。注释说明 UP 阈值更大是因为某些设备会延迟合成 UP 以降低抬手噪声。
- **`LSQ2_RESTRICTED`**：拟合出的速度与「窗口内首末点实际位移向量」做点积，为负（方向相反）直接返回 false、速度置 0（`:640-651`）。这是抬手抖动导致方向反转的兜底。
- 单次 `AddMovement` 把事件里全部 historical 采样一并喂入；若最后一个采样是重采样产生的则跳过，只用真实点（`:302-378`）。

EGoTouchRev `TouchTracker` 的速度是单帧一阶差分（`TouchTracker.hpp:995-996`）：

```cpp
t.vx = (o.x - t.x) / frameSpan;   // frameSpan = wasSilentGap ? max(1, gapFrames+1) : 1  (:977)
```

无平滑、无滤波、无多点拟合。新建轨迹的 `vx/vy` 为 0，第一帧不产生速度。

**单位是「格/帧」而非「格/秒」**——分母是帧数不是时间。所有下游用法都隐含固定帧率假设。

三处用途：

1. `GetMatchReference`（`:227-234`）：预测匹配参考点，`framesAhead = m_predictionScale`（默认 1.0），SilentGap 时乘 `gapFrames + 1`。
2. `ComputeTrackGateSq`（`:260`）：速度参与门限放宽，`movingFast = speed > m_maxTrackDistance * 0.5`；SilentGap 额外窗口 `speed * 0.75 * gapFrames`。
3. `ComputeAssignmentCost`（`:301-309`）：反向运动惩罚与速度突变惩罚。

**方向校验存在但只是软惩罚**（`:302-304`）：点积为负时 `cost += min(25, ...)`，且有前置条件 `age > 1 && moveSq > 0.01 && prevSpeedSq > 0.04`，不构成硬拒绝。对比 Chromium 的 `LSQ2_RESTRICTED` 是方向相反直接把速度置 0。

**没有过期机制。** 轨迹在 SilentGap 里最多挂 `m_gapRelinkWindowFrames`（默认 4，@120Hz 约 33 ms）帧，`vx/vy` 一直是进入 gap 前那一帧的值，没有按 `gapFrames` 或时间的衰减/失效逻辑。Chromium 对应的是 40 ms 无移动即清空全部速度历史。

### 4.8 手势判定

Chromium `GestureDetector`（`gesture_detector.cc`）没有显式状态枚举，状态是一组布尔标志加四个 one-shot 定时器。

阈值默认值（`gesture_detector.h:42-93`，注释注明取自 Android `ViewConfiguration` 未缩放默认值，距离单位 dip，速度 dip/秒）：

| 字段 | 默认 | Aura 覆盖 |
|---|---:|---:|
| `showpress_timeout` | 180 ms | 150 ms |
| `shortpress_timeout` | 400 ms | 400 ms |
| `longpress_timeout` | 500 ms | 500 ms |
| `double_tap_timeout` | 300 ms | 400 ms |
| `double_tap_min_time` | 40 ms | — |
| `touch_slop` | 8 dip | 15（ChromeOS 6） |
| `stylus_slop` | 12 dip | 20（ChromeOS 10） |
| `double_tap_slop` | 100 dip | 20 |
| `minimum_fling_velocity` | 50 dip/s | 30 |
| `maximum_fling_velocity` | 8000 dip/s | 17000 |
| `two_finger_tap_timeout` | 700 ms | 800 |

定时器的实际延时是累加的：`SHORT_PRESS = shortpress_timeout + showpress_timeout`，`LONG_PRESS = longpress_timeout + showpress_timeout`（`gesture_detector.cc:58-62`）。代入默认值：ShowPress 180 ms → ShortPress 580 ms → LongPress 680 ms。启动定时器时还会扣掉事件处理延迟 `Now() - ev.GetEventTime()`（`:316-318`，受 `kCompensateGestureDetectorTimeouts` 控制）。

几个与误触相关的细节：

- slop 判定**逐指进行，各自相对自己的 down 点**（`IsWithinSlopForTap`，`:633-673`），第二指的基准是 `secondary_pointer_down_event_`。
- 面板不上报 touch major 时 slop 半径 ×2（平方系数 `kSlopEpsilonForZeroTouchMajor = 4.f`，`:32-34`、`:657-660`）。
- 比较前 slop 先加 `kSlopEpsilon = 0.05f` 再平方（`:484-491`），用于分数 dip 坐标下的像素级精确。
- 位移超过 slop 时 `timeout_handler_->Stop()` 停掉全部定时器（`:346`），tap 与长按从此不可能。`all_pointers_within_slop_regions_` 一旦转 false 不可逆，只在下一个 DOWN 重置。
- **首次滚动扣掉 slop**：`ComputeFirstScrollDelta` 逐指把位移向量按 `(d - touch_slop)/d` 缩短再平均（`gesture_provider.cc:768-814`），避免越过阈值瞬间跳变。三指及以上不扣。
- 滚动死区 `kScrollEpsilon = 0.1f`。
- **方向锁定**（`SnapScrollController`）：位移超 slop 后，`dx/dy > kMinSnapRatio = 1.25` 且 `dy < 2*slop` → 横向锁定，反之纵向；两轴都超 `2*slop` 则永久不锁。锁定时另一轴分量置 0，同时保留未锁定的原值供上层使用。解除条件是垂直于锁定轴的累计位移超过 channel distance；channel distance = `clamp(屏幕对角线 × (1.5×slop/480), 1.5×slop, 4.5×slop)`。

不确定期的处理方式是**先发出去、事后用取消事件修正**，不是扣住不发：DOWN 立刻发 `kGestureTapDown`；发现是拖动时，`TouchDispositionGestureFilter` 在发 `ScrollBegin` 前先补一个 `kGestureTapCancel`（`touch_disposition_gesture_filter.cc:483-486`）。

`TouchDispositionGestureFilter` 解决的是另一个问题：手势在触摸事件送去渲染进程之前就已生成，而渲染进程可能 `preventDefault()`。它按 ack 结果事后丢弃手势，并用四个 `needs_*_event_` 标志补发合成的结束事件，保证手势流成对。这一层是浏览器架构特有的，**与驱动栈无关**。

EGoTouchRev `TouchGestureStateMachine`（`TouchGestureStateMachine.hpp:13-15`）是显式五相位状态机，转移全部在 `UpdateSlot`（`:189-276`），槽位由 `contact->id - 1` 索引（`:110-111`），最多 20 个。

**槽 ↔ 接触的前置过滤**（`:105-116`）：`id <= 0` 或 `!isReported` 的接触被跳过，**除非带 `TouchLifeSilentGap`**（`:108-109`）——SilentGap 接触虽不上报，但仍喂养槽位，防止 gap 期间被当作抬起。`state == TouchStateUp` 的接触被清成 `nullptr`（`:113-116`），即「up 等价于无接触」。

**全部转移：**

| 源 | 条件 | 目标 | 行号 |
|---|---|---|---|
| Idle | 有接触 | PressCandidate | `:193` |
| PressCandidate | 接触消失 | ReleasePending，`missingFrames = 1` | `:205-206` |
| PressCandidate | 位移 > `m_dragThreshold` | Dragging（经 `EnterDragging`），清 `quickTapEligible` | `:219-221` |
| PressCandidate | `ageFrames >= m_longPressFrames` 且位移在 `m_longPressMoveTolerance` 内 | LongPressHold | `:224-228` |
| Dragging | 接触消失 | ReleasePending，`missingFrames = m_releasePendingFrames + 1` | `:233-235` |
| LongPressHold | 接触消失 | ReleasePending，`missingFrames = m_releasePendingFrames + 1` | `:246-248` |
| LongPressHold | 位移 > `m_dragThreshold` | Dragging | `:255-257` |
| ReleasePending | 接触重现 | 回 `slot.prevPhase` | `:263` |
| ReleasePending | `missingFrames > m_releasePendingFrames` | Idle（发 up）+ `slot.Reset()` + `upEmitted = true` | `:167-182` |

Dragging 是终态，不回 LongPressHold。抬起后同一 id 至少空一帧才能重新按下（`upEmitted` 机制，`:126-130`）。

**`missingFrames` 初值不对称**：PressCandidate 消失时置 1（`:206`），Dragging / LongPressHold 消失时置 `m_releasePendingFrames + 1`（`:235`、`:248`）。按默认 `m_releasePendingFrames = 0`，前者需再等一帧循环才发 up，后两者在同一帧的 Phase 3 立即发 up。这个差异在代码里没有注释说明。

**阈值以帧数为单位：**

| 参数 | 声明 | 默认 | 配置键 |
|---|---|---:|---|
| `m_pressCandidateFrames` | `:42` | 1 | `touch.gesture.press_candidate_frames` |
| `m_longPressFrames` | `:46` | 46 | `touch.gesture.long_press_frames` |
| `m_releasePendingFrames` | `:48` | 0 | `touch.gesture.release_pending_frames` |
| `m_dragThreshold` | `:45` | 0.8 格 | `touch.gesture.drag_threshold` |
| `m_pressCandidateMinSignal` | `:43` | 0 | **未绑定** |
| `m_pressCandidateMinSizeMm` | `:44` | 0.0 | **未绑定** |
| `m_longPressMoveTolerance` | `:47` | 0.8 格 | **未绑定** |

`frame.timestamp` 在整个 `EGoTouchService/Solvers/` 的 `.hpp` 里**只出现一次**——`CoordinateFilter.hpp:27`。`TouchTracker.hpp` 与 `TouchGestureStateMachine.hpp` 全文不引用时间戳，不做任何帧→毫秒换算。所有阈值都是纯帧计数，隐含固定帧率假定；`m_longPressFrames = 46` 在 120 Hz 约 383 ms、60 Hz 约 767 ms，代码中没有任何地方表达或校验这一点。

tracker 侧同样按帧计的阈值：`m_gapRelinkWindowFrames = 4`（`TouchTracker.hpp:24`）、`m_touchDownDebounceFrames = 1`（`:25`）、`m_stylusAftRecentFrames = 24`（`:50`）、`m_stylusAftDebounceFrames = 3`（`:56`）、`m_stylusAftSuppressFrames = 40`（`:59`）、`m_stylusAftPalmSuppressFrames = 100`（`:60`）、`kPenModeGapToleranceFrames = 5`（`:84`）。

`PressCandidate` 与 `LongPressHold` 阶段输出坐标钉在 anchor 上；进入 `Dragging` 时 `EnterDragging` 沿起手方向回退一个 `m_dragThreshold`，偏移量此后固定（效果对应 Chromium 扣掉首个 scroll delta 中 slop 部分）。

未实现的手势：双击、fling、缩放、双指点击、方向锁定。没有多指焦点概念——每个 slot 独立走状态机。

### 4.9 坐标滤波与预测

Chromium 的预测器（`ui/base/prediction/`，工厂 `predictor_factory.cc:38-56`）：

| 名称 | 模型 |
|---|---|
| `linear_resampling` | 两点线性插值/外推，Android `InputTransport.cpp` 的移植；滚动路径默认 |
| `kalman` | 匀加速模型，每轴一个 3 维 Kalman；触摸/鼠标路径默认 |
| `lsq` | 三点二阶最小二乘 |
| `linear` / `linear_2` | 一阶/二阶线性 |
| `empty` | 不预测 |

Kalman 细节（`kalman_predictor.cc`、`kalman_filter.cc`）：

- 状态向量 `X = [位置, 速度, 加速度]ᵀ`，位置单位 px，时间单位统一毫秒。
- 预测式基点用**最后一个真实观测点**而非滤波器的位置估计：
  `pos = last_point.pos + v * (1.0 * dt) + a * (0.5 * dt²)`
  系数 `kVelocityInfluence = 1.0`、`kAccelerationInfluence = 0.5`（`kalman_predictor.cc:17-19`）。
- 过程噪声 `Q = 0.01 * g gᵀ`，`g = [0.5·dt², dt, 1]ᵀ`。
- 协方差更新用 Joseph 形式，数值上比 `(I-KH)P` 稳定（`kalman_filter.cc:79-89`）。
- 稳定判据：两轴各迭代 ≥ `kStableIterNum = 4` 次才 `HasPrediction()`。

外推的四道硬闸（`input_predictor.h:76-86`）：

```
kMaxTimeDelta      = 20 ms    两点间隔超过它认为轨迹断了，Reset（四个预测器都有）
kMaxResampleTime   = 20 ms    重采样相对最后事件的最大外推量
kMaxPredictionTime = 25 ms    生成 PredictedEvents 的最大外推量
kResampleMaxPrediction = 8 ms 且不超过最近两事件间隔的一半（LinearResampling 自限）
```

`kResampleLatency = -5 ms`（`linear_resampling.cc:46`），即对齐到 vsync 前 5 ms；注释说明这样能让重采样更多落在插值而非外推区间，牺牲几毫秒延迟换精度。

**方向反转抑制**（`scroll_predictor.cc:487-502`）：新 delta 与参考 delta 符号相反则置 0。注释写明动机是上一帧过预测后，下一帧真实事件会试图往回补，造成滚动跳回。

`LeastSquaresPredictor` 的两条退化分支：三个采样几乎不变时直接返回最后一点、速度与加速度置 0（`least_squares_predictor.cc:22-25`，显式处理「手指停住」）；`XᵀX` 奇异时返回 false，上层不做预测。

1€ 后置滤波（`one_euro_filter.h:33-39`）：`mincutoff = 4.7 Hz`、`beta = 0.01`、`dcutoff = 1.0 Hz`，注释注明是 2023 年 3—5 月按 `Event.Jank.PredictorJankyFramePercentage` 指标实验调出的。

预测质量有专门的评估器 `prediction_metrics_handler.{h,cc}`，指标包括 `OverPrediction`/`UnderPrediction`/`WrongDirection`/`PredictionScore`/`PredictionJitter`/`VisualJitter`。

EGoTouchRev 侧：

- `CoordinateFilter`（`CoordinateFilter.hpp`）是 1-Euro，但 **cutoff 用速度平方项**而非教科书的线性项（`:60`），三个参数按平方响应标定：`m_minCutoff = 1.0`、`m_beta = 150.0`、`m_dCutoff = 100.0`。文件头注释明确警告不要直接套用线性 1-Euro 的常见取值。量纲是传感器格/秒，与 Chromium 的 px/ms 不同，**参数数值不可直接对比**。
- 没有显式的 dt 上限或 Reset。`alpha = 1/(1 + tau/te)` 在 `te` 很大时趋于 1，滤波器自动变透明，行为上等价于自复位。
- 状态是定长数组 `m_states[kMaxTouchIds + 1]`（`:90`，`kMaxTouchIds = 20`），**直接用 `contact.id` 当下标**（`:33`），没有 id→槽的映射表，没有代际号。`m_activeMask` 每帧清零，处理过的 id 打 1；帧末把所有 `activeMask == 0` 的槽整体重置（`:72-76`）。重新初始化的条件是 `!state.initialized || contact.state == TouchStateDown`（`:35`），命中时直接取原始坐标、速度清零、当帧不滤波。

  id 复用相关的三点行为：

  - **up 接触让槽多存活一帧**。tracker 的 up 接触仍带原 id（`TouchTracker.hpp:509`），而 `CoordinateFilter` 在 tracker 之后运行，所以 up 那一帧 `activeMask[id]` 被置 1，槽不回收；且因 `state == TouchStateUp` 不满足 `:35` 的重置条件，up 坐标会被旧状态滤波改写。这个改写无实际后果——手势层 `:138` 会把 up 接触压掉，真正的 up 由 `:172` 用 `slot.lastOutputX/Y` 另造。
  - **SilentGap 期间槽被持续喂养**：SilentGap 接触带原 id 与预测坐标，会被正常滤波并回写，即 gap 期间滤波器吃的是外推值而非测量值。
  - 没有 id 之外的身份校验（无代际号、`sourcePeakId` 不参与、无位置突变检测）。`AllocateId`（`TouchTracker.hpp:530-550`）的去重检查 `nextTracks` 与 `m_tracks` 两处，且新建循环跑在删除循环之前，同帧内 up 与新建撞 id 的路径被挡住。**是否存在其他能让 up 与新 down 在同帧共用同一 id 的路径未确认。**

- 触摸路径**没有预测**。
- 笔路径 `LinearFilterProcess.hpp:136-145` 每帧计算二次外推 `3*x0 - 3*x1 + x2` 写入 `runtime.post.predictedCoor`，**全仓库没有任何读取方**（`grep predictedCoor` 只有 `AsaTypes.hpp:169` 的声明与两处写入）。注释称其为「diagnostics and side-channel trend data」，但诊断侧也不读。没有外推上限、没有方向反转抑制、没有停止检测。

### 4.10 笔与触摸的仲裁

Chromium 主线**实际生效的只有一条**：笔一进感应范围就整台触摸屏禁用。

```cpp
// touch_event_converter_evdev.cc:999-1002
if (enable_palm_suppression_callback_) {
  enable_palm_suppression_callback_.Run(event->tool_code > 0);
}
```

判据 `tool_code > 0` **包含悬停**。回调落到 `InputDeviceFactoryEvdev::EnablePalmSuppression`（`input_device_factory_evdev.cc:829-840`），`IsDeviceEnabled` 对内置、有触摸屏、无笔的 converter 返回 false（`:523-526`）；被禁用的 converter 走 `OnDisabled()` → `ReleaseTouches()`，把所有 slot 标 cancelled 并派发。**没有时间窗，没有超时**，只对 `INPUT_DEVICE_INTERNAL` 生效。

带时间窗的那一层 `HeuristicStylusPalmDetectionFilter` 默认关闭，且 `heuristic_palm_stroke_count` 默认 0 使 hold 分支实际不生效（`stroke_length_` 自增后至少为 1）。参数默认值在 `ui/events/ozone/features.cc:31-40`：cancel 窗 0.4 秒、hold 窗 1.0 秒、hold 笔画数 0。判据用**笔画起始时刻**而非当前时刻，所以一笔一旦通过就整笔通过。头文件明说这是给「笔与手指互斥关系差的特定板子」用的（`.h:20-36`）。

**这条路 EGoTouchRev 没得选**：Chromium 能整体禁用是因为笔和手指是两个独立的 evdev 设备节点（`has_pen_ = info.HasKeyEvent(BTN_TOOL_PEN)`，一个 converter 实例要么是笔要么是手指）。EGoTouchRev 的笔与手指来自同一颗控制器的同一份热力图，无法按设备粒度禁用，只能做接触点级的仲裁。

EGoTouchRev 的 `StylusTouchSuppressor`（`StylusTouchSuppressor.hpp`）与 `TouchTracker` 内的 `m_stylusAft*` / `m_penModeSuppress*` 字段构成接触点级的空间 + 时间仲裁，复杂度高于 Chromium 主线实际启用的部分。两处存在同名参数（如 `m_stylusAftWeakSizeThresholdMm` 在 `StylusTouchSuppressor.hpp:26` 与 `TouchTracker.hpp:58` 各有一份，`TouchTracker.hpp:729` 处优先取 `ss` 指向的那份），分工需要实现者自行核对。

两个可注意的 Chromium 细节：

- **工具类型在 Pressed 那一帧锁定，整笔不变**（`touch_event_converter_evdev.cc:1010-1013`）。`ProcessKey` 里 pen 与 rubber 共用一个 `tool_code` 槽位，先到先得：已有非零值时新的按下被忽略，抬起时只有 code 匹配才清零（`:551-562`），防止交叠上报踩坏状态。
- **抬笔那一帧的坐标直接丢弃**（`pen_tablet_event_converter_evdev.cc:295-299`），注释写明是绕过固件在离开瞬间吐出的无效数据。

Chromium 侧笔的物理量处理有两处不一致，属于其自身瑕疵，不建议照搬：压力归一化只截上界不截下界，`value < pressure_min_` 会得到负值（`touch_event_converter_evdev.cc:1053-1060`）；倾角的 range 计算两条通路不同（触摸屏 `max - min`，数位板 `max - min + 1`），前者能取到 +90 后者取不到。

悬停高度 `ABS_DISTANCE` 被映射为 `ABS_MT_DISTANCE`（`:84-86`）但 `ProcessAbs` 没有对应分支，落到 default 丢弃，上层拿不到。

### 4.11 配置与可调性

Chromium 的手势参数集中在 `GestureConfiguration`（`gesture_configuration.h:204-279`），按平台派生（`_aura` / `_android` / `_default`），运行时可改。

EGoTouchRev 的配置有三份独立字面量，实现者需注意其一致性要求：

1. 成员初始值（各 `.hpp` 的类定义）
2. `binder.bind(...)` 的默认值（`TouchPipeline.cpp::registerBindings`）
3. `store.getOr(...)` 的回退值（`TouchPipeline.cpp::applyConfig`）

`SolversUnit_PipelineDefaultsConsistency`（`tests/unit/config/PipelineDefaultsConsistencyTest.cpp`）同时检查两条不变量：binder 默认必须等于成员初值；空 store 走一遍 `applyConfig` 必须是空操作（即回退值等于成员初值）。第二条是后加的——曾发生 `touch.palm_box.expand_rows` 成员与 binder 都是 9、回退值写成 1 的情况，三项中两项一致，旧检查看不出来。

新增配置键需要同时改四处：成员、binder、applyConfig、`ConfigKeyId.h` + `ConfigKeyMap.cpp`。没有 `ConfigKeyId` 的键会得到 `MaxKeyId`，功能上可用但无法经 TLV 通道（IPC）设置。

`TouchClassifier` 中未绑定配置的成员（改动需重新编译）：`m_densityThresholdLow`、`m_areaMinForDensity`、`m_elongatedEnabled`、`m_elongatedMinArea`、`m_elongatedAspectRatio`、`m_candidateAreaThreshold`、`m_candidateSignalThreshold`、`m_likelyAreaThreshold`、`m_fillRatioThreshold`、`m_flatSharpnessThreshold`、`m_strongPeakProminence`、`m_ambiguousMargin`、`m_palmLikelyAllowContact`。`PalmBoxSuppressor` 的 `m_mergeGapRows`、`m_mergeGapCols` 同样未绑定。以上成员经核对**均有真实读取方**，不是死代码。

### 4.12 前段：信号调理与特征提取（Chromium 无对应）

Chromium 的输入从接触点开始，这一整段没有对应物。以下是 EGoTouchRev 独有的部分。

**帧解析**（`MasterFrameParser.hpp:36-41`）：7 字节头之后的 4800 字节按小端解成 `int16_t heatmapMatrix[40][60]`。帧布局常量在 `Common/include/FrameLayout.h:18-37`，注释说明经 Ghidra 逆向 `himax_thp_drv.dll` 验证（`:9-10`）。

**`BaselineTracker`**（`BaselineTracker.hpp`）：Q8.24 定点基线，跨盖合/息屏继承，只有首帧或显式 `Reset()` 才回到 `0x7FEE`。

- 无手指（`:165-189`）：全格更新，输出清零。死区 `m_noiseDeadband = 90` 内步长 1，超出用 shift 3 / 步长 512 激进收敛。
- 有手指（`:195-269`）：先算全局共模 `commonDiff = EstimateDiffMedian(cells)`，即全部 2400 格 `raw - baseline` 的**中位数**（`:199`、`:332-342`）；`localDiff > m_peakThreshold = 305` 的格**冻结**，基线只跟随 `commonDiff`（`:230-245`）；其余走三档 IIR（`:275-303`）。

防止停留手指被吸进基线有三层：冻结本身无时限（`:230-245`）；release hold 60 帧内负值透传不更新基线（`:234-235`、`:250-261`）；共模用中位数而非均值，少量手指格拉不动全局估计（`:340`）。

需要注意的一条：recovery 的第二个触发条件是「有手指但上一帧一个冻结格都没有」（`:213-218`），此时旁路三档分类，用 shift 2 / 步长 256 最快速度追踪全部背景格，上限 `m_recoveryMaxFrames = 30` 帧。**若手指信号恰好压在 305 以下**（轻触，或大接触面被 CMF 削平），这条路径会在 30 帧内把手指吸进基线。冻结阈值是唯一护栏。

**`CMFProcessor`**（`CMFProcessor.hpp`）：带排除的行/列均值扣除，不是频域方法。只统计 `-2000 < val < 2000` 的格子做共模估计（`m_exclusionThreshold = 2000`，`:73`、`:81`），把手指这类强信号排除在外；`rowOffset = rowSum / validCount` 整数除（`:88`），clamp 到 ±2000。ARM64 有 NEON 实现（`:44-77`）。`m_mode` 支持四种模式但**无配置绑定**，运行期恒为 `RowWise`。

**`MacroZoneDetector`**（`MacroZoneDetector.hpp:32-115`）：8 邻域连通域，阈值取 `runtime.peakThreshold`（即 `PeakDetector::m_threshold = 280`）。本身没有最小面积判据，过滤在下游 `PeakDetector::m_macroZoneMinArea = 3`。按 `signalSum` 降序只留前 20 个。访问标记用 epoch 计数避免每帧清零（`:133-140`）。

**`PeakDetector`**（`PeakDetector.hpp:49-80`）顺序：`DetectInRange` → `ClosePeakSaddle` → Z8 → Z1 → macro zone 最小面积 → EdgePeak → 排序 → 截断 → ID 跟踪。

- 局部极大用非对称比较（`:174-189`）：扫描序之后的邻居用 `nv > v` 判否，之前的用 `nv >= v`，使平顶只留一个峰。
- 边缘阈值判据是 `c == 1 || c == 58 || r == 39`（`:165-171`），**不是 0 和 59**，且行方向只判下边缘。此不对称在代码中无注释说明。
- `m_z8Filter`（`:341-345`）：`(p.z >> 5) > p.neighborSignalSum`，剔除孤立尖刺（单格噪声/坏点）。
- `m_z1Filter`（`:350-352`）：`p.z < m_threshold` 剔除，实为兜底。
- `m_closePeakSaddleFilter`（`:264-295`）：同一手指被拆成两峰的合并。要求 `弱峰 - 鞍点 ≥ max(80, 弱峰 * 0.08)`，否则抑制弱峰。
- `DetectPressureDrift`（`:318-335`）：中等信号（375~750）上按行梯度判平摊掌压并丢弃。
- **排序是信号升序**（`:378-381`），随后 `m_peakCount = min(m_peakCount, m_maxPeaks = 20)`（`:76`）截断保留数组前 N 个，即最弱的那些。因 `DetectInRange` 内已有上限并替换最弱者（`:218-224`），这行在实际路径上是空操作——但重构时若把排序方向改成降序而保留这行，语义会变。
- 持久 id（`TrackPeakIDs`，`:388-429`）：贪心最近邻，曼哈顿距离 ≤ 3 视为同一峰。`m_nextPeakId` 是 `uint8_t`（`:103`），**会回绕且不跳过 0**。匹配按信号升序贪心，非全局最优。

**`ZoneExpander`**（`ZoneExpander.hpp:239-316`）：以峰为种子的 8 邻域 BFS。阈值 `CalcZoneThold`（`:200-215`）= `min(sigThold, peakSig) * 0x40 >> 7`，约 50%。掌中指特例：`FingerLikely` 的峰落在 `PalmCandidate`/`PalmLikely` 的 zone 里时，阈值抬到 `max(50%基准, peakSig * 0.70)`，并把半径限制在 `m_fingerInPalmMaxRadius = 3`（`:217-227`）。

`sig >= zoneThold` 的格进核心区并入队；`0 < sig < zoneThold` 的格标记但不入队，只累计到 `edgeArea`/`edgeSignalSum`（`:303-310`），形成一圈边缘环，不参与质心。

`m_dilateErode`（`:323-406`）是一次形态学闭运算，补 zone 内部噪声空洞。膨胀按 8 邻域多数投票，`bestCnt >= 3` 才填，投票源固定读原始快照所以不级联（`:328`）；腐蚀只回收膨胀新增的格，且「任一 8 邻域为空就撤销」，比标准腐蚀激进。

多指分割 `PartitionMultiFingerZone`（`:524-593`）是信号降序的分水岭：各峰为种子入大顶堆，每次弹出最强格把未归属的 8 邻居收编。堆序在信号相等时用 `owner`、`idx` 做确定性 tie-break（`:483-487`）。分块数上限 16。

**接触点数量超限有两处取舍，判据都是 `signalSum` 大者优先：**

1. 写入时（`InsertContactCandidate`，`:685-708`）：容器满后线性找最小 `signalSum` 项，新点更强才替换。
2. 最终 top-N（`:73-102`）：`contacts.size() > m_maxTouches`（默认 10）时按 `signalSum` 降序保留前 N 个，**并重新赋 `tc.id = i`**（`:90`）。

第 2 步覆盖掉了第 1 步继承自 `Peak::id` 的持久 id（`:639`）。**只有在接触点数未超 `m_maxTouches` 时，`contact.id` 才是峰的持久 id；超限时全部变成数组下标。** 下游 `TouchTracker` 若依赖 id 的跨帧稳定性，这条路径上是断的。`sourcePeakId`（`:640`）不受影响，保留原始峰 id。

**`GhostSuppressor`**（`GhostSuppressor.hpp`）针对互容矩阵的 RX 方向鬼影：同行上与真实接触点共线但信号明显更弱的假点。判据是行距 ≤ `m_rxGhostLineDelta`（实际值 0，即四舍五入后完全同行）且 `weak.signalSum < strong.signalSum * 0.5`。接触点数 ≥ 4 时直接放弃（`:47`）。

调用点在 `TouchTracker.hpp:1123-1130`，属跟踪阶段而非前段。对象每帧在栈上新建，无跨帧状态；类内默认值是死值，实际值每帧从 `TouchTracker` 的同名成员覆盖。

### 4.13 笔求解器（EGoTouchRev 侧，路径以 `EGoTouchService/Solvers/` 为根）

入口 `StylusSolver/StylusPipeline.cpp:170`：

```
ResetPerFrameState + 取 BT 压力快照      :171-172
会话终止短路                              :192-196
StylusFrameParser::Process               :200
HPP2 补救（terminal 且无 HPP3 证据时）    :205-209
分派 m_hpp2 / m_hpp3                      :217-221
共享尾链                                  :231-236
  EdgeCoorProcess → EdgeCoorPostProcess
  → CommonPost → CaptureFinal
  → StylusTouchArbiter → Commit
```

`CommonStylusPostPipeline.h:26-30` 内部固定顺序：`LinearFilter → CoorRevise → CoorSpeed → CoorIIR → AftCoor`。

**HPP2 路径在产品链路上不可达。** `hpp2LineValid` / `hpp2LineData` / `auxStatusFlags` / `framePressure` / `buttonBits`（`StylusFrameTypes.h:31-37`）在整个仓库里**没有任何生产代码写入**——只有 `tests/unit/stylus/StylusHpp2PipelineTest.cpp`、`Device/tests/runtime/DeviceRuntimePenStateTest.cpp` 与诊断上位机的只读显示提到它们。解析器默认选 HPP3（`StylusFrameParser.hpp:35`），HPP2 line 输入是三条取数路径里优先级最低的 last resort（`:180-187`，注释在 `:178-179`）。实际运行的永远是 HPP3。HPP2 子管线（`hpp2/`，12 个文件）与其 20 余个未绑定配置参数（`Hpp2Pipeline.h:37-66`）随之全部不可达。

**压力不来自热图，来自蓝牙笔 MCU。** `SetBtMcuPressure` / `SetBtMcuPressurePacket`（`StylusPipeline.cpp:302`、`:312`）写入 `m_btSample`，每帧加锁取一次快照（`:329`），按 seq 去重（`PressureSolver.hpp:139-146`）。链路（`hpp3/PressureSolver.hpp:44-101`）：

1. `lookaheadHoverGate`：原始压力为 0 直接判悬停，绕过整条链（`:62`）。
2. 曲线映射 `MapPressure`（`:166-185`）：`0x0FFF` 直通；`x <= 11` 压成 0/1；否则两段多项式（`x > 127` 用 4 次的 `m_polySeg2`，否则 `m_polySeg1`），再乘 `m_gainPercent/100` 并 clamp 到 `0..0x0FFF`。**系数出处无注释，未确认是本机标定还是通用曲线。**
3. IIR：`m_iirWeightQ8 = 64`，即等权各半，只在前后两帧都非零时生效。
4. 信号迟滞 `SuppressBtPressBySignal`（`:193-215`）：`signalX < 2200` 且两轴都不在边缘则 latch 清零，`signalX > 3200` 才解锁。
5. `tipDownCandidate = inRange && outputPressure >= m_tipDownPressureThreshold`（默认 1）。

后处理 `Hpp3PostPressureProcess.hpp` 另有 BT 频率跳变消抖（`:125-145`）、默认关闭的 `fakePressureDecrease`（`:12`、`:147-157`）、边缘信号过低迟滞（`:159-193`，enter 阈值在两轴同时边缘时取 `×2/3`，exit 阈值 3000）。

**坐标滤波五级：**

- `EdgeCoorProcess`（`shared/EdgeCoorProcess.hpp:61-131`）：高速出边检测。前后帧坐标差 > `0x200` 且本帧压力为 0、上帧非 0、且在边缘轴时，把上一帧压力搬进本帧（`:127-130`），多留一帧 pen-down。这是全链路唯一的尾帧补偿。
- `EdgeCoorPostProcess`（`shared/EdgeCoorPostProcess.hpp:56-77`）：距边界 `< 0x40` 归零，`[0x40, 0x400)` 线性拉伸到 `[0, 0x400)`。注释（`:52-54`）说明常量是硬编码固件值，不在 flash 表里。
- `LinearFilterProcess`：三部分。avg3 与二次外推都只写诊断字段不进主路（注释 `:171-172`）；linear correction 是 7 态状态机（`:192-255`，各态实现 `:399-484`），核心动作 `DragPoint2Line`（`:649-680`）把点沿法线拉向 PCA 拟合的直线，位移量 clamp 到 `maxDrag`；`m_dragLimit = 32`，进入阈 `m_enterMaxDistSq = 900`，退出阈 `m_exitCos1000 = 700` / `m_exitDistSq = 3600`。
- `CoorSpeedProcess`（`shared/CoorSpeedProcess.hpp:24-105`）：速度输入是**未滤波的** `tx1.coordinate.reportGlobalCoor`，注释 `:34-36` 说明这是为了不让 IIR 系数被前级平滑污染。`speedValue = sqrt((dx²+dy²)*100)/10`。
- `CoorIIRProcess`（`shared/CoorIIRProcess.hpp:125-181`）：Q8 定点，`filt = (coef*new + (maxCoef-coef)*old) / maxCoef`，`m_maxCoef = 32`。系数按速度插值：悬停 `(low=2, high=16, thold=20)`，书写 `(6, 18, 10)`，边缘激活时覆盖为悬停系数右移一位。`speed >= m_speedMax = 205` 取 high，之间线性插值。刚进 range 或状态计数不足 2 帧时旁路（`:92`）。

  `:14-27` 有一段注释记录参数曾被改坏的经过（写字模式 `(6,18)→(18,26)`、`speedMax 140→60`、悬停对被交换成 `(10,6)`）及后果：滤波常驻最弱档、笔迹发抖。并指名 flash 地址 `asa[0xA5C]–asa[0xA60]`。

- lock flash 不在 `LinearFilterProcess` 里，在 `shared/AftCoorProcess.hpp:18-31`：落笔瞬间锁定起始点（`:104-111`），移动量超过 `ScaleLockThreshold` 才释放锁并清偏移（`:119-140`）。释放时必须清 offset，注释 `:114-118` 说明这个 bug 在阈值填错时是潜伏的，改回原厂值后才显现为落笔点漂移。

**悬停没有独立布尔量**，对外是 `inRange = true, tipDown = false, pressure = 0` 的组合（`StylusRuntimeCommit.hpp:22-28`）；VHF 报文里体现为 `penState` 只置 `0x20` In-Range 不置 `0x01` Tip Switch（`Device/vhf/VhfReporterStylusPacketHelper.h:80-90`）。

**笔离开范围没有超时计数器。** 四条触发路径都汇到 `FinalizeTerminalFrame`（`StylusPipeline.cpp:284-300`）。终止帧照样 Commit 产出全零 output，并真的发一帧 out-of-range 报文（`emitWhenInvalid` 恒为 true，`StylusPipeline.h:59`）。`m_touchArbiter` 被刻意排除在 `ResetStatefulStages` 之外并仍在终止帧调用——注释 `:296-297` 说明终止帧正是笔离开那一帧，清零会把 linger 尾巴削没。

**笔/触共享状态**载体是 `frame.stylus`：

- `interop`（`StylusFrameTypes.h:54-66`）是唯一双向字段集。`StylusRuntimeCommit.hpp:38-48` 填充，`StylusTouchSuppressor` 读后回写 `recheckPassed`、`recheckThreshold`、`touchNullLike`、`touchSuppressActive/Frames`，`TouchTracker` 再读取。
- `output` 单向，触摸侧只读。坐标换算 `kStylusTouchCoordScale = 1/1024`，把笔的 `0x400/格` 换成触摸侧格单位。
- 另有指针通道 `frame.touch.runtime.stylusSuppress`，让 tracker 读 suppressor 的配置实例。

`StylusTouchArbiter`（`shared/StylusTouchArbiter.hpp:51-88`）四态 Idle/Hovering/Writing/Lingering，inRange 时重装 `m_lingerFrames`（默认 40，@120 Hz 约 330 ms），离开后逐帧递减。`:18-31` 的注释描述了完整抑制链路，并明确要求链路上任何一环都不得无条件清零。

**`StylusTouchSuppressor` 与 `TouchTracker` 存在逐行重复的实现：**

| 内容 | 位置 A | 位置 B |
|---|---|---|
| `StylusNoiseEvidence` 结构 | `StylusTouchSuppressor.hpp:37-49` | `TouchTracker.hpp:89-101` |
| `BuildStylusNoiseEvidence` 函数体 | `:70-110` | `TouchTracker.hpp:552-592` |
| `IsStrongTouchCandidate` | `:112-115` | `TouchTracker.hpp:594-599` |
| `m_stylusSuppress*` / `m_stylusAft*` 配置字段 | `:17-30` | `TouchTracker.hpp:43-62` |

缓解方式是 `TouchTracker` 优先读指针 `frame.touch.runtime.stylusSuppress` 里的值，取不到才用自己的成员（`:595-596`、`:602-603`、`:633`、`:688-689`、`:725-730`）。但 `m_stylusAftRadius`（`:691`）、`m_stylusAftRecentFrames`（`:653`）、`m_stylusAftPalm*`（`:678`、`:715-717`）、`m_penModeSuppressEnabled`（`:607`）只存在于 `TouchTracker`，没有对应的 suppressor 字段。

**两个 AFT 半径不同且互不引用**：suppressor 用 `m_stylusSuppressLocalDistance = 2.5`（`:19`），tracker 用 `m_stylusAftRadius = 2.8`（`TouchTracker.hpp:51`）。

`touchSuppressFrames` 里混了两种量纲——arbiter 的 linger 倒计时与 suppressor 的 debounce——注释 `:208-214` 明确警告只能当下界读，不能做算术。

职责划分：suppressor 做本帧接触点删除（近笔尖、弱信号的直接 erase）；tracker 做跨帧轨迹级抑制，两条路径 `ShouldStylusAftSuppress`（`:681-736`）与 `ShouldPenModeSuppress`（`:668-679`）。`:694-719` 有一大段注释说明「掌判在豁免之前」的顺序，并记录该分支在本机数据集上实际不可达（实测 area 31 / signalSum 18671，密度约为上限的两倍）。

**VHF 上报换算**（`Device/vhf/VhfReporterStylusPacketHelper.h`）：轴交换 + Y 反向，`dim2 → HID X ∈ [0,16000]`（`:37-43`），`dim1` 反向 `→ HID Y ∈ [0,25600]`（`:45-51`）；压力 clamp 4095（`:100`）。tilt clamp 是 ±9000（`:24`、`:102-107`），而 `tiltX/Y` 的单位是**度**（±90），**这个 clamp 实际永不生效**。

`:113-115` 的注释说明 Windows 只接受有限的 pen switch 组合，橡皮擦悬停必须先经过一帧 out-of-range——这是对主机 OS 而非面板固件的补偿。

---

## 5. 已确认的现存问题

### 5.1 本轮已修复（供理解代码演化，不需要重做）

| commit | 内容 |
|---|---|
| `687c0e9` | `EdgeRejector` 的两个互相抵消的缺陷：`tc.state == 0` 守卫在该位置恒真（`state` 无上游赋值），且其保护的 `isReported = false` 被 `TouchTracker` 无条件覆写。判定与后果拆开后功能才首次生效。 |
| `f0b1bd1` | `TouchSizeCalculator` 原用的换算在 `signalSum ≥ 368` 时饱和到 15 mm，实际取值范围内恒为常量；且该值随后被 `TouchTracker` 用另一公式覆盖，`StylusTouchSuppressor` 还有第三份同样的公式。统一为单一定义并新增测量值 `areaMm2`。 |
| `362e31f` | `touch.palm_box.expand_rows/cols` 的 `applyConfig` 回退值与成员初值不符（1 对 9、1 对 10），配置缺键时掌框外扩面积缩到约九分之一。 |
| `6476803` | 进入 `Dragging` 时输出坐标从 anchor 直接跳到指尖，跳变幅度为一个 `m_dragThreshold`（约 3.6 mm，屏幕上三十余像素）。 |

### 5.2 未修复

- **`runtime.post.predictedCoor` 是死代码**：`LinearFilterProcess.hpp:136-145` 每帧计算二次外推并写入，全仓库无读取方。
- **`EdgeCompensator` 的 LUT 分段恒为 segment 0**：索引量 `sizeMm` 的实际取值远小于第一段阈值 64，`g_defaultECProfiles` 的后三段从未被选中。
- **`StylusTouchSuppressor` 的弱尺寸判据不独立生效**：`sizeMm < 1.2` 等价于 `signalSum < 40`，被同一析取式里的 `signalSum < 240` 完全覆盖。
- **手势时间阈值以帧计**：`m_longPressFrames = 46` 等值在报点率变化时含义随之改变，`frame.timestamp` 可用但未被使用。
- **`PalmBoxSuppressor::Process(HeatmapFrame&)` 的陈旧发布路径**：`rt.macroZones` 为空时跳过 `CopyEvaluations` 却仍发布 `GetEvaluations()`，会把上一帧的评估结果发布出去。实际管线中 `MacroZoneDetector::Process` 无条件设置该指针（`MacroZoneDetector.hpp:29`）且顺序在前，故该分支在完整管线中不可达；仅在测试或裁剪管线中可能触发。
- **`ZoneExpander::m_edgeBounds` 被读但从未被写**（`ZoneExpander.hpp:35`）。读取方是 `ContactExtractor.hpp:155`（挂到 `runtime.edgeBounds`）与 `EdgeCompensation.hpp:260-261`、`:394-395`，全仓库没有任何赋值点，也无配置绑定，永远是默认的 `{0, 60, 0, 40}`（`TouchSharedTypes.h:17-22`）。**边缘补偿与边缘拒绝实际在按标称网格边界工作，而非实测边界。**
- **超过 `m_maxTouches` 时接触点 id 从持久峰 id 退化为数组下标**（`ZoneExpander.hpp:90`），见 4.12。
- **`BaselineTracker` 的 recovery 第二条件可能吸收停留手指**（`BaselineTracker.hpp:213-218`），见 4.12。
- **`PeakDetector::m_pressureDriftDebounceLimit`（`:35`）是彻底的死配置**：全仓库仅声明处一处出现，无 `registerBindings` 条目、无 `applyConfig` 赋值、无读取方。
- **多处只写不读的成员**：`BaselineTracker::m_freezeCellMask`（`:144`，注释称其为诊断标志但无诊断路径读取）、`ZoneExpander::ZoneUnit::edgeArea`/`edgeSignalSum`（`:121-122`）、`ZoneUnit::flags`（`:123`）、`ZoneUnit::peakSig`/`peakCol`/`peakRow`（`:125-126`，`peakSig` 另有独立路径传给 `CalcZoneThold`，此份冗余）、`Peak::macroZoneSignalSum`（`TouchSharedTypes.h:58`）、`TrackState::upEventEmitted`（`TouchTracker.hpp:109`，只在 `:1008` 写 false）、`GestureSlot::quickTapEligible`（`TouchGestureStateMachine.hpp:27`，在 `:200`、`:220`、`:227` 维护）。
- ~~**`TouchContact::debugFlags` 仓库内无读取点**~~ **勘误**：工作台接触点表格读它（`DiagnosticsWorkbench.Visualization.cpp:543`），且它携带 `lifeFlags` 没有的三条信息——AFT 抑制 `0x101`、gap relink `0x21`、silent gap 用的是预测坐标还是上帧坐标（`0x28` 对 `0x20`）。删它要先给这三条在新模型里安排位置，见 roadmap 步 3。
- **无调用方的公开接口**：`MacroZoneDetector::GetMutableMacroZones()`（`:118`）、`MacroZoneDetector::GetMacroZones()`（`:117`，结果实际经 `runtime.macroZones` 传递）、`ContactExtractor::GetEdgeBounds()`（`:169`）。
- **有读取但无配置绑定**（改动需重新编译）：`PeakDetector::m_sigTholdLimit`（`:21`）、`m_z8Radius`（`:28`）、`m_pressureDriftFilter`（`:24`）、`m_edgePeakFilter`（`:25`）、`m_closePeakMinSaddleDrop`（`:32`）、`m_closePeakMinSaddleRatio`（`:33`）、`CMFProcessor::m_mode`（`:19`）、`TouchGestureStateMachine::m_pressCandidateMinSignal`（`:43`）、`m_pressCandidateMinSizeMm`（`:44`）、`m_longPressMoveTolerance`（`:47`），以及 4.11 列出的 `TouchClassifier` 与 `PalmBoxSuppressor` 各项。
- **每帧栈上分配的大数组**：`ZoneExpander::Process` 的 `std::array<int, kGridSize> order`（`:78`，2400 项，未初始化但只用已赋值的前 `contactCount` 项）与 `keptContacts`/`keptEdgeInfos`（`:85-86`，各 256 项）。
- **`m_pixelPitchMm` 按方形网格处理，实际网格非方形**，`areaMm2` 高估约 9%，见 3.3。
- **VHF 的 tilt clamp 永不生效**：`Device/vhf/VhfReporterStylusPacketHelper.h:24`、`:102-107` 按 ±9000 clamp，而 `tiltX/Y` 的单位是度（±90）。
- **笔侧完全无读无写的成员**：`FlowRuntime::resetPost` / `resetNoise`（`AsaTypes.hpp:80-81`）、`SignalRuntime::overlapLike`（`:110`）、`DecisionRuntime::immediateRelease` / `keepInRange` / `enableCoordFilter` / `enableCoorReviser`（`:155-160`）、`SolvePoint::reportX` / `reportY` / `peakTx1` / `peakTx2` / `tx1X..tx2Y`（`:43-44`、`:48-49`、`:57-60`，恒为 0 但被 DVR 记录，`Common/DVRCore/include/DvrFormat.h:578-596`）、`StylusOutputState::packet`（`StylusFrameTypes.h:51`）、~~`StylusInputSnapshot::tx1BlockValid` / `tx2BlockValid` / `slaveWordOffset`~~（**勘误**：这三个由 `StylusFrameParser` 写、经 DVR 与共享内存传给工作台显示，不是死字段）、`Hpp3::Settings::enabled` 与 `Hpp3::Pipeline::m_enabled`（`hpp3/Hpp3Runtime.hpp:129`、`Hpp3Pipeline.h:24`，构造进 ctx 后无任何阶段读 `ctx.settings`）、`Hpp3::State`（空结构占位）、`GridFeature::dim1SelectedPeakOnEdge` / `dim2SelectedPeakOnEdge`（`Hpp3Runtime.hpp:109-110`）。
- **笔侧有写无读（诊断除外）**：`PressureRuntime::predictedAgeFrames`（恒写 0 无自增，语义本应是「输出压力是第几帧的外推值」，实际压力没有跨帧外推）、`DecisionRuntime::enableEdgeCorrect`、`PostRuntime::postIirCoor`（IIR 实为原地改 `finalCoor`）、`PostRuntime::predictedCoor`、`speedAvgDx/Dy`、`noiseRejectReason`、`freqBypassed`、`linearFilterActive` / `linearFilterDeltaDim1/Dim2`、`Hpp2::State::m_bypassCounter` / `m_prevBypassed`、`Hpp2::Runtime::buttonReleaseFrames`。
- **HPP2 整条路径不可达**，见 4.13。
- **笔按键未接进 VHF 报文**：`StylusRuntimeCommit.hpp:26` 有 TODO；且 `buttonPressed` 只有 HPP2 会置位，HPP3 恒 false。
- **仓库根目录有未跟踪的 `PointerPenProbe.obj`**：构建残留，与本次工作无关。
- **`.claude/worktrees/` 下有两份残留工作副本**（`agent-a3131aa58e0512f46`、`agent-a8c869e2a87f068e4`），会污染全仓库 grep，检索时需排除。
- **`docs/` 下有若干 `*.bak` 文件**：本机 hook 在改动未跟踪文件前自动备份产生，不要提交或清理。

---

## 6. 不可直接移植的部分

| 项 | 原因 |
|---|---|
| 神经网络掌抑制的**权重** | 训练输入是固件上报的 major/minor 半径与压力，按设备 resolution 归一化；EGoTouchRev 的量纲与统计分布不同。特征设计与决策时序可参照，权重不可用。 |
| `HeatmapPalmDetector` 的模型 | 不在 chromium/chromium 仓库内，位于 ChromeOS platform2 的 ML service；仅支持两款机型；主线默认关闭。 |
| 「笔进范围禁用整台触摸屏」 | 依赖笔与手指是两个独立 evdev 设备节点。EGoTouchRev 两者来自同一控制器的同一份热力图。 |
| `TouchDispositionGestureFilter` | 解决的是「手势已生成但渲染进程可能 `preventDefault()`」，属浏览器架构问题，驱动栈无对应。 |
| `MaybeCancelAllTouches` | Chromium 注释自陈是对上层无法处理单点取消的迁就，带 TODO，非算法设计。 |
| 各类硬件白名单 | `kStylusButtonDevices`（仅 Dell Active Pen PN579X）、`kKeyboardBlocklist`（XP-Pen 三款）、按 board 名硬编码的半径多项式（hatch/reef）、heatmap 设备 VID/PID 白名单——均为特定机型补偿。 |
| 1-Euro 参数数值 | 两边量纲不同（传感器格/秒 对 px/ms），且 EGoTouchRev 用速度平方项、Chromium 用线性项。 |
| 手势阈值数值 | Chromium 用 dip，EGoTouchRev 用传感器格，且屏幕物理尺寸与报点率不同。 |

---

## 7. 测试现状与可用的验证手段

`EGoTouchService/Solvers/tests/`，注册在 `tests/CMakeLists.txt`。当前 12 项，全部通过。

| 测试 | 覆盖 |
|---|---|
| `SolversUnit_TouchBaselineTracker` | 基线跟踪 |
| `SolversUnit_TouchTrackerGapRelink` | 轨迹匹配、silent gap 重连、边缘拒绝的接线、拖动起手不跳 |
| `SolversUnit_TouchStylusSuppress` | 笔/触抑制 |
| `SolversUnit_TouchEdgeCompensation` | 边缘补偿与边缘拒绝判定 |
| `SolversUnit_TouchCloseSplit` | 邻近峰分割 |
| `SolversUnit_TouchContactSize` | 尺寸估计的单调性、`areaMm2` 的测量语义 |
| `SolversUnit_TouchPalmBoxSuppress` | 掌框抑制，含到 `ZoneExpander` 的端到端接线 |
| `SolversUnit_TouchPenModeSuppress` | pen mode 抑制 |
| `SolversUnit_StylusHpp2Pipeline` | hpp2 管线 |
| `SolversUnit_StylusPipelineConfigRoundTrip` | 笔配置往返 |
| `SolversUnit_PipelineDefaultsConsistency` | 配置三份字面量的一致性 |
| `SolversUnit_StylusTouchArbiter` | 笔/触仲裁 |

另有 perf 类测试带 `slow` 标签，用 `-E "slow|perf"` 排除。

**构建方式**（本机 MSVC 环境变量不会自动进入 shell）：

```powershell
$bat = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
$out = cmd /c "`"$bat`" arm64 >nul 2>&1 && set"
foreach ($line in $out) { if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] -ErrorAction SilentlyContinue } }
cmake --build build\arm64-Debug
ctest --test-dir build\arm64-Debug -L solvers -E "slow|perf"
```

Release 配置经工具链调用会挂起，需在用户终端手动构建。

**验证测试是否真的抓得住问题**：本轮四个修复都做过反向验证——临时改坏实现，确认新测试变红。`EdgeRejector` 的原有测试正是因为只断言了单方向（「已修正的接触点不被拒」，在空实现下也恒真）而长期为绿，未能发现功能从未生效。建议对新增测试沿用同样的做法。

`docs/` 下另有 `touch_pipeline_architecture.md`（含级序图与文件清单）与 `stylus_touch_arbitration_design.md`，可作为背景，但其中部分描述早于本轮修改（例如 `:317` 处 `sizeMm = f(signalSum)` 的表述）。

---

## 8. 未确认项

以下条目本次核对未得出确定结论，实现时需自行验证：

- ~~本机触摸控制器的报点率是否恒定。~~ **已有结论**：恒定 122 Hz，帧间隔 8.2 ms，p5–p95 仅 8.0–8.5 ms，空闲间隙可达十余秒。见 roadmap 前置项 2。
- `hpp3/PressureSolver.hpp:34-38` 两段压力多项式的系数出处无注释，未确认是本机标定还是通用曲线。`m_btPressureMapOrderMode` 的 `OnCell`/`InCell` 顺序表（`:125-126`）同样没有注释解释其对应的硬件模式。
- `CoordinateFilter` 中是否存在能让 up 接触与新 down 接触在同一帧共用同一 id 的路径（见 4.9）。已确认 `AllocateId` 的常规路径被挡住，其余路径未穷举。
- `SharedPalmDetectionFilterState::latest_stylus_touch_time` 的注释称其包含 hover，但 `UpdateSharedPalmState` 的写入条件是 `events_[0].touching`，不含悬停（`touch_event_converter_evdev.cc:849-856`）。注释是否过时未确认。
- `HeuristicStylusPalmDetectionFilter` 在 ChromeOS 具体机型上是否由 finch 或 board config 打开。仓库内只能看到主线 feature 默认关。
- Aura 平台的手势阈值（字段名带 `_in_pixels_`）在高 DPI 屏上是否再做缩放。
- `EGoTouchApp` 诊断上位机在改名后未做过实际运行验证，只确认可编译。
