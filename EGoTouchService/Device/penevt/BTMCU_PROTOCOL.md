# BTMCU USB 协议与当前实现符合度

基于以下来源整理：
- `THP_Service.dll` 的 Ghidra MCP 逆向结果
- `full-dump.apmx64` / `penevent.apmx64` 的 API Monitor 记录
- `EGoTouchService/Device/penevt/PenEventBridge.cpp`
- `EGoTouchService/Device/btmcu/PenUsbTypes.h`
- `EGoTouchService/Device/btmcu/PenUsbPacketBuilder.h`
- `EGoTouchService/Device/btmcu/PenUsbInitSession.h`
- `EGoTouchService/Device/runtime/DeviceRuntime.cpp`

> 置信度标记
> - **Confirmed**: 已被反汇编/反编译、抓包或当前代码中的 exact capture 明确证实
> - **Likely**: 高概率成立，但还缺少抓包或更深层调用链确认
> - **Open**: 仍待确认

## 1. 总体结论

原厂 `THP_Service.dll` 不直接使用 Windows Bluetooth API 与手写笔通信，而是通过 MCU/BTMCU 暴露的 USB interface 传输 BT 笔事件与控制帧：

```text
THP_Service.dll / ApDaemon.dll
  -> MCU USB interface
    -> BTMCU
      -> Bluetooth stylus
```

当前项目中对应实现为：

```text
PenEventBridge          col00 事件/控制通道
btmcu/PenUsb*.h         协议 builder、parser、ACK 表、初始化状态机
DeviceRuntime           原厂状态位与上层行为映射
PenPressureReader       col01 压力通道，不属于本文事件协议主体
```

整体判断：当前 `penevt` + `btmcu` + `DeviceRuntime` 已经覆盖原厂关键 wire protocol：设备发现、`0x7101/0x7701` 初始查询、`0x8001` ACK、`0x7D01` 初始化参数、RX event 解析、核心状态位更新与按钮/橡皮擦事件转发。仍有少量边缘事件和未使用命令需要补齐或保持为待确认项。

---

## 2. 报文骨架

### 2.1 Host → MCU 发送帧（Confirmed）

所有已确认的主机发包最终都走原厂 `BtPen_SendPacket @ 0x18000f500`，其行为是：

```text
malloc(0x40)
memset(buf, 0, 0x40)
copy header[0..7]
force buf[6] = 0x11
copy payload to buf[8..]
WriteFile(handle, buf, payload_len + 8, ...)
```

注意：`WriteFile` 的实际长度由调用参数 `payload_len + 8` 决定，header 内的 `byte[1]` 不是 payload length。`0x7D01` 是最关键证据：它带 32 字节 payload，但 header 仍是 `07 01 02 00 01 7D 11 20`。

已确认发送帧布局：

已确认发送帧布局（`byte[1]/byte[4]/byte[7]` 三项经 KeyboardService.dll 与 PenService.dll
交叉比对后修正，见 docs/KBDMCU_PROTOCOL.md 3.2）：

| 偏移 | 含义 | 备注 |
|---|---|---|
| `byte[0]` | `0x07` | 目标地址（子系统所在 MCU 链路）；pen=`0x07`、键盘=`0x05`、detach=`0x09` |
| `byte[1]` | 恒 `0x00` | PenService.dll 与 KeyboardService.dll 全部发送点恒为 0，含带 payload 的帧。THP_Service.dll 抓包里带 payload 时为 `0x01`，是该 DLL 独有用法或被 MCU 忽略，非协议必需 |
| `byte[2]` | `0x02` | 源地址 = Host |
| `byte[3]` | `0x00` | 当前观测固定为 0 |
| `byte[4]` | 子系统 ID | 不是命令低字节。`(byte[0], byte[4])` 严格成对：pen=`0x01`、键盘=`0x02`、detach=`0x00`。文中 `0x7101` 这类写法实为 `(byte[5]<<8)\|byte[4]`，即命令码拼子系统 |
| `byte[5]` | 命令码 | 如 `0x71 / 0x77 / 0x7D / 0x7E / 0x80` |
| `byte[6]` | `0x11` | `BtPen_SendPacket` 强制覆盖 |
| `byte[7]` | payload 长度 | 恒等于调用方传入的 payload 字节数：无 payload=`0x00`、1 字节=`0x01`、`0x7D01` 的 32 字节=`0x20` |
| `byte[8..]` | payload | 长度由调用方传给 `BtPen_SendPacket` |

简写：

```text
07 F 02 00 CMD_LO CMD_HI 11 TAG [payload...]
```

### 2.2 MCU → Host 接收帧（Confirmed for dispatch fields）

原厂 `AsynchReadThreadProc @ 0x18000e1d0` 持续执行：

```text
ReadFile(handle, buffer, 0x40, ...)
```

然后把 64 字节包推进环形队列。`USB_AsynchProcThreadProc @ 0x18000cf40` 消费队列并分发事件。

API Monitor 中已确认的 RX 前缀：

```text
02 00 07 00 01 EVT 11 01 VALUE ...
```

已确认字段：

| 偏移 | 含义 | 备注 |
|---|---|---|
| `byte[0]` | `0x02` | MCU → Host marker，抓包确认 |
| `byte[1]` | `0x00` | 抓包中固定为 0 |
| `byte[2]` | `0x07` | 原厂分发逻辑会校验 |
| `byte[3]` | `0x00` | 抓包中固定为 0 |
| `byte[4]` | `0x01` | 原厂分发逻辑会校验 |
| `byte[5]` | event/status ID | switch dispatch key |
| `byte[6]` | `0x11` | 抓包确认 |
| `byte[7]` | payload length | `PenService.dll` 处理函数按该长度读取 `byte[8..]` |
| `byte[8]` | first payload/status value | 大多数状态事件只使用该字节 |
| `byte[9..]` | 后续 payload / padding | 有效长度由 `byte[7]` 决定 |

当前 `TryParsePenUsbEventFrame()` 与原厂已确认校验保持一致：要求 `packet.size() >= 8`、`packet[2] == 0x07`、`packet[4] == 0x01`，并返回 `eventCode = packet[5]`、按 `packet[7]` 截断后的 `payload = packet[8..8+len)`。

---

## 3. Host → MCU 命令表

| 命令 | 方向 | 帧头 / 示例 | Payload | 原厂触发点 | 当前实现 | 结论 |
|---|---|---|---|---|---|---|
| `0x7101` | Host → MCU | `07 00 02 00 01 71 11 00` | 无 | `BtPen_CheckPenStatus @ 0x18000e830` | `BuildPenUsbCommand(QueryPenStatus)` | **Confirmed** |
| `0x7701` | Host → MCU | `07 00 02 00 01 77 11 00` | 无 | `BtPen_CheckMcuStatus @ 0x18000e8c0` | `BuildPenUsbCommand(QueryPenInfo)` | **Confirmed** |
| `0x8001` | Host → MCU | `07 01 02 00 01 80 11 20 <ack>` | 1 字节 ACK code | `BtPen_SendEventAck @ 0x180011d20` | `BuildPenUsbEventAck()` | **Confirmed** |
| `0x7E01` | Host → MCU | `07 01 02 00 01 7E 11 01 <value>` | 1 字节 match info | `BtPen_GetReportInfo(type=4)` | enum 已有，当前未发送 | **Confirmed / not implemented path** |
| `0x7D01` | Host → MCU | `07 01 02 00 01 7D 11 20 <32 bytes>` | 32 字节 init params | `BtPen_HandleInitParamEvent @ 0x18000f660` | `BuildFactoryInitProtocolParamsCommand()` | **Confirmed** |

### 3.1 `0x8001` ACK 包（Confirmed）

```text
07 01 02 00 01 80 11 20 <ack_code>
```

API Monitor 已确认样本：

```text
07 01 02 00 01 80 11 20 01
07 01 02 00 01 80 11 20 06
07 01 02 00 01 80 11 20 0A
```

### 3.2 `0x7E01` Match-Info 响应（Confirmed）

原厂 `BtPen_GetReportInfo(type=4)` 会发送：

```text
07 01 02 00 01 7E 11 01 <value>
```

已抓到：

```text
07 01 02 00 01 7E 11 01 02
07 01 02 00 01 7E 11 01 03
07 01 02 00 01 7E 11 01 00
```

当前 `PenUsbCommandId::PairInfoSet` 已定义，但 `BuildPenUsbPayloadCommand()` 固定使用 `byte[7] = 0x20`。因此如果未来要实际发送 `0x7E01`，需要单独 builder 生成 `byte[7] = 0x01`，不能直接复用当前 generic payload builder。

### 3.3 `0x7D01` Init-Param 响应（Confirmed）

原厂 `BtPen_HandleInitParamEvent` 会把 ASCII/CSV token 转换成 32 字节二进制，再发：

```text
07 01 02 00 01 7D 11 20 <32-byte payload>
```

当前项目固定发送 exact factory capture：

```text
Header:
07 01 02 00 01 7D 11 20

Payload:
33 33 33 33 E7 02 12 04 58 02 1A 41 0F 01 01 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

该 payload 对应反推 token：

```text
3333,3333,2e7,412,258,411a,10f,1,
```

这是目前最可靠的 `0x7D01` 基线。

---

## 3.4 电量：只应答，不广播

MCU 从不主动上报电量。`BATTERY_STATUS` (`0x08`) 只在收到查询命令 `0x0801` 之后回一条，此外任何时候都不出现。

命令码来自原厂 `PenService.dll`。它导出一组 `CommandSendGetPenXxx`，每个函数体都在栈上拼同一种 8 字节包：

```
07 00 02 00 | 01 <code> 11 00
```

| 导出函数 | `<code>` | 命令 ID |
|---|---:|---|
| `CommandSendGetPenModule` | `0x00` | `0x0001`（与既有实现吻合，可作为格式自校验） |
| `CommandSendGetPenBattery` | `0x08` | `0x0801` |
| `CommandSendGetPenChargingStatus` | `0x09` | `0x0901` |
| `CommandSendGetPenConnectStatus` | `0x12` | `0x1201` |

**请求 `byte[5]` 与应答事件码同值。** 三条命令实测全部命中：`0x0801 → 0x08 payload=100`、`0x0901 → 0x09`、`0x1201 → 0x12 payload=2`。每条都只在发出命令后的那一段窗口里出现一次，空载基线段为零。

注意这里的 `0x0801` 是 `(byte[5]=0x08) << 8 | (byte[4]=0x01)` 的合写，同值的是命令的 `byte[5]` 与应答事件码，不是「请求码低字节」——`byte[4]` 恒为子系统 ID `0x01`（pen），与命令无关。键盘子系统的同类命令（`byte[4]=0x02`）遵循同一规律：`GetKeyboardBattery` 的 `byte[5]=0x08` 对应应答事件 `0x08`。详见 docs/KBDMCU_PROTOCOL.md。

实现：`PenEventBridge::MaybePollBattery()`，60 秒一次，连接建立后立即取第一次。轮询同时挂在 `OnIdleTick()` 和 `OnPacketReceived()` 上 —— `OnIdleTick()` 只在读超时时触发，而 MCU 会成串广播状态，只挂前者会在链路繁忙时被饿死。

### 一条曾经写进本节的错误结论

此前本节断言"这支笔不提供电量百分比"，依据是一次实测：分三段监听，空载、发 `QueryPenStatus (0x7101)` 之后、发 `QueryPenInfo (0x7701)` 之后，电量事件计数为 0。

实测本身没错，错在推断。用的两条命令与电量无关，"没触发"只说明这两条不是取电量的命令，不能推出电量不可得。正确做法是先去原厂二进制里找命令表，而不是拿手头已有的命令穷举。

### MCU 的周期性广播不可当作状态源

抓包里反复出现同一串固定序列，与我们发什么命令无关：

```
0x72=1 ×6 → 0x7B=88 ×6 → 0x28=1 → 0x09=1 ×3 → 0x72=3 ×6 → 0x7B=88 ×6 → 0x09=0
```

其中 `0x09` 在 1/0 之间跳、`0x72` 在 1（书写）/3（橡皮擦）之间跳，都不对应任何真实的物理状态变化。

「用 `0x0901` 做周期充电轮询」这个处方已实测并撤销：应答与广播共用事件码 `0x09`，收包侧无法区分，而广播以约 1 Hz 成串到达，「查询后窗口内的第一条 `0x09`」采到的大概率就是下一条广播——按 60 s 周期查询等于每分钟随机采样一次，面板在充电/未充电之间漂移。同时吸附弹窗依赖广播带来的状态变化触发，丢弃广播后弹窗一并消失。广播直喂驱动 UI 在实际使用中未复现「不停闪烁」（乱跳成串但 UI 侧感知稳定），维持直喂。

当前实现只在 USB 通道每次建立时发送一次 `0x0901`，用于解决「服务启动时笔已经吸附，因没有新的物理边沿而迟迟收不到首条充电状态」的问题。它不设置查询后的采样窗口，也不把应答当成特殊来源；所有 `0x09` 仍走同一个事件处理路径，之后不再主动查询。因此这条一次性启动查询只催促 MCU 尽快给出初始快照，不会重新引入上述周期随机采样机制。

## 4. MCU → Host 事件表

| 事件码 (`packet[5]`) | 名称/日志 | 原厂 Host 处理 | ACK code | 当前实现 | 结论 |
|---|---|---|---:|---|---|
| `0x03` | `USBD_SW_VERSION` | 记录日志 | 无 | 未命名，默认回调/日志 | **Confirmed / acceptable** |
| `0x08` | `BATTERY_STATUS` | 记录日志 | 无 | 已解析为 semantic；由 `0x0801` 查询触发，见 3.4 | **Confirmed** |
| `0x09` | `CHARGING_STATUS` | 记录日志 | 无 | 已解析为 semantic，驱动托盘面板 | **Confirmed / 有效** |
| `0x10` | `DEV_CONNECT` | 记录日志 | 无 | 已解析为 semantic | **Confirmed** |
| `0x12` | `DEV_CONNECT_STATUS` | 更新 `g_penStatusCode=1`，调用状态处理 | 无 | 未显式处理 | **Confirmed / gap** |
| `0x21` | `PEN_DOCK_STATUS` | 记录日志 | 无 | 未命名，默认回调/日志 | **Confirmed / acceptable** |
| `0x23` | `PEN_UPDATE_STATUS` | 记录日志 | 无 | 未命名，默认回调/日志 | **Confirmed / acceptable** |
| `0x27` | `PEN_KEY_FUNC_GET` | 记录日志 | 无 | 未命名，默认回调/日志 | **Confirmed / acceptable** |
| `0x2C` | `PEN_BATTERY_AFTER_CONN` | 记录日志 | 无 | 未命名，默认回调/日志 | **Confirmed / acceptable** |
| `0x2E` | `PEN_PAIR_DETECT_ACK` | 记录日志 | 无 | 未命名，默认回调/日志 | **Confirmed / acceptable** |
| `0x2F` | `PEN_CURRENT_FUNC` | 更新当前功能；value `1` 映射 erase/status 3 | `0x0B` | ACK + `func==1` 触发 `HandlePenButtonStatusCode(3)` | **Confirmed** |
| `0x70` | `PEN_AC_STATUS` | 更新 status bit0，通知状态 | `0x00` | ACK + `ApplyFactoryStatusFlagUpdate()` | **Confirmed** |
| `0x71` | `PEN_CONN_STATUS` | 更新连接状态，调用状态处理 | `0x01` | ACK + status bit + Init/DisconnectStylus | **Confirmed** |
| `0x72` | `PEN_CUR_STATUS` | 更新当前模式 bits `0x04/0x08` | `0x02` | ACK + status bits + semantic current mode | **Confirmed** |
| `0x73` | `PEN_TYPE_INFO` | 更新笔类型/ID bits `0x30` | `0x0D` | ACK + status bits + `SetStylusId` | **Confirmed** |
| `0x74` | `PEN_ROATE_ANGLE` | 更新角度 bits `0x40/0x80` | `0x03` | ACK + status bits | **Confirmed** |
| `0x75` | `PEN_TOUCH_MODE` | 更新第二状态字节 bit0 | `0x04` | ACK + status bit `0x0100` | **Confirmed** |
| `0x76` | `PEN_GLOBAL_PREVENT_MODE` | 更新第二状态字节 bit1 | `0x05` | ACK + status bit `0x0200` | **Confirmed** |
| `0x77` | `PEN_SCREEN_STATUS` / MCU status | 初始化中推动第二次 `0x7701`，平时 ACK | `0x06` | ACK + init session 推动第二次 `0x7701` | **Confirmed** |
| `0x78` | `PEN_HOLSTER` | 更新 holster bit | `0x07` | ACK + status bit `0x0800` | **Confirmed** |
| `0x79` | `PEN_FREQ_JUMP` | 记录/通知 | `0x08` | ACK + 记录 payload | **Confirmed** |
| `0x7B` | `PEN_REP_PARAM` | 原 switch 内只 log + ACK | `0x0A` | ACK + init session 发送 `0x7D01` | **Wire-compatible** |
| `0x7C` | `PEN_GLOBAL_ANNOTATION` | `HandlePenStatusCode(4)` | `0x0C` | ACK + `HandlePenButtonStatusCode(4)` | **Confirmed** |
| `0x7F` | `ERASER_TOGGLE` | 设置 `g_penEraserToggleState`，通知状态 | `0x09` | ACK + eraser state；OEMCustom 写 VHF | **Confirmed** |
| `0x28` | `PenTopBatteryWindow` | 请求宿主弹出笔电量提示窗 | 无 | 未命名，默认回调/日志 | **Confirmed** |
| `0x2A` | `PenDeviationReminder` | 笔吸附偏移提醒 | 无 | 映射为紧凑通知；本机日志尚未捕获该事件 | **Version-dependent** |

`0x2A` 的名称和回调槽由 `PenService.dll` 静态确认，但当前安装版的托管 PenApp 没有注册该
回调，`THP_Service.dll` 的事件 switch 也把它落入默认忽略分支。仓库保留解析和通知映射，供
会发出该事件的硬件/软件版本使用；不得用「已吸附但未充电」等状态组合猜测，否则满电会误报。

---

## 5. ACK 对照表（Confirmed）

当前 `GetFactoryBtMcuAckCode()` 与原厂映射一致：

| 事件码 | ACK code |
|---|---:|
| `0x70` | `0x00` |
| `0x71` | `0x01` |
| `0x72` | `0x02` |
| `0x74` | `0x03` |
| `0x75` | `0x04` |
| `0x76` | `0x05` |
| `0x77` | `0x06` |
| `0x78` | `0x07` |
| `0x79` | `0x08` |
| `0x7F` | `0x09` |
| `0x7B` | `0x0A` |
| `0x2F` | `0x0B` |
| `0x7C` | `0x0C` |
| `0x73` | `0x0D` |

`0x6F -> 0x0B` 已从当前 ACK 表移除；目前没有足够抓包证据支持该映射。

---

## 6. 初始化时序

### 6.1 原厂时序（Confirmed）

`Usb_Start` 创建读线程和处理线程后，会触发：

```text
BtPen_CheckPenStatus -> 0x7101
BtPen_CheckMcuStatus -> 0x7701
```

API Monitor 中确认的关键序列：

```text
TX 07 00 02 00 01 71 11 00              0x7101 CheckPenStatus
TX 07 00 02 00 01 77 11 00              0x7701 CheckMcuStatus #1
RX 02 00 07 00 01 77 11 01 01 ...       0x77 PEN_SCREEN_STATUS
TX 07 01 02 00 01 80 11 20 06           ACK 0x06
TX 07 00 02 00 01 77 11 00              0x7701 CheckMcuStatus #2
RX 02 00 07 00 01 7B 11 01 58 ...       0x7B PEN_REP_PARAM
TX 07 01 02 00 01 80 11 20 0A           ACK 0x0A
TX 07 01 02 00 01 7D 11 20 <32 bytes>   0x7D01 InitParam
```

### 6.2 当前实现时序（Confirmed）

`PenEventBridge::OnConnected()` 调用 `RunHandshake()`，`PenUsbInitSession` 状态机执行：

```text
OnConnected
  -> SendInitialQueries
     -> 0x7101
     -> 0x7701 #1

OnEvent(0x77)
  -> ACK 0x06
  -> 0x7701 #2

OnEvent(0x7B)
  -> ACK 0x0A
  -> 0x7D01 exact factory payload
```

这与抓包中的 wire-level 顺序一致。

需要注意：原厂 `case 0x7B` 本身只负责 log + ACK，`0x7D01` 来自另一条 service callback 路径：

```text
ServiceInterface[+0xA8]
  -> BtPen_GetReportInfo(type=2)
    -> BtPen_HandleInitParamEvent(...)
      -> BtPen_SendPacket(cmd=0x7D01, payload_len=0x20)
```

当前实现把这条跨组件路径折叠进 `PenUsbInitSession`。这不是原厂代码结构的逐行复刻，但 wire protocol 与时序目标一致。

---

## 7. 当前实现符合度分析

### 7.1 已符合原厂逻辑的部分

| 项目 | 当前实现 | 结论 |
|---|---|---|
| USB interface GUID | `PenEventBridge::FindDevicePath()` 使用 `{dd0ebedb-f1d6-4cfa-acca-71e66d3178ca}` | 符合 |
| 设备打开 | `CreateFileW(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING)` | 符合；当前额外使用 overlapped I/O |
| 读包长度 | `PenUsbTransportWin32::ReadPacket()` 使用 64 字节 buffer | 符合 |
| 写包长度 | `WritePacket()` 写入 vector 实际长度 | 符合 `payload_len + 8` 模型 |
| `0x7101` | `BuildPenUsbCommand(QueryPenStatus)` | exact |
| `0x7701` | `BuildPenUsbCommand(QueryPenInfo)` | exact |
| `0x8001` ACK | `BuildPenUsbEventAck()` + ACK 表 | exact |
| `0x7D01` init params | `BuildFactoryInitProtocolParamsCommand()` | exact factory capture |
| RX dispatch key | `TryParsePenUsbEventFrame()` 使用 `packet[5]` | 符合 |
| RX first payload | `payload[0]` 对应原厂 `packet[8]` | 符合 |
| ACK 时机 | `OnPacketReceived()` 先 ACK 再推进 init action | 符合抓包时序 |
| `0x77` 后第二次 `0x7701` | `PenUsbInitSession` | 符合 |
| `0x7B` 后 `0x7D01` | `PenUsbInitSession` | wire-compatible |
| 核心状态位 | `ApplyFactoryStatusFlagUpdate()` | 符合已逆向 bit mask |
| 连接/断开 | `PenConnStatus -> InitStylus/DisconnectStylus` | 项目语义等价 |
| 笔类型 | `PenTypeInfo -> SetStylusId` | 符合 |
| 当前功能 `0x2F` | `func==1 -> HandlePenButtonStatusCode(3)` | 符合 |
| 全局注解 `0x7C` | `HandlePenButtonStatusCode(4)` | 符合 |
| 橡皮擦 toggle `0x7F` | 保存 eraser state；OEMCustom 写 VHF | 符合项目路由 |

### 7.2 与原厂不完全一致但可接受的部分

| 项目 | 差异 | 判断 |
|---|---|---|
| I/O 模式 | 原厂同步 `ReadFile/WriteFile`；当前使用 overlapped + wait | wire-level 等价，可接受 |
| 队列模型 | 原厂读线程 + `0x41` stride ring buffer；当前通道线程直接回调 | 架构不同，但不影响协议 |
| `0x7B -> 0x7D01` 来源 | 原厂由 service callback 触发；当前由 init session 触发 | wire-compatible，可接受 |
| RX 校验 | 当前只校验 `packet[2]` 与 `packet[4]` | 与已确认原厂校验一致；若要更保守可额外校验 `0x02/0x11/0x01` |
| payload 暴露 | 当前按 `packet[7]` 截断 `packet[8..]` | 与 `PenService.dll` 字符串/模块处理函数一致 |
| 状态通知 | 原厂 `ThpNotifyBurst`；当前 callback + `SetEvent` | 项目语义等价 |

### 7.3 需要修正或补齐的 gap

| 项目 | 当前状态 | 建议 |
|---|---|---|
| `0x12 DEV_CONNECT_STATUS` | 已知原厂会更新状态，但当前未显式 enum/处理 | 如果抓包中出现，应补 `PenUsbEventCode` 和 DeviceRuntime 行为 |
| `0x7E01 PairInfoSet` | enum 已有，但 generic payload builder 会生成 `byte[7]=0x20`，原厂需要 `0x01` | 如果未来要发送，添加专用 `BuildPenUsbPairInfoSet()` |
| log-only 事件命名 | 多个已知事件当前会走 default log | 可补 enum/name helper，便于诊断；不影响 ACK |
| 帧头校验强度 | 我们要求 `packet[2]==0x07 && packet[4]==0x01`，原厂 `PenService.dll` 只校验 `packet[4]` | 接入键盘后放宽到只按 `packet[4]` 分流，否则可能丢弃原厂会接受的帧 |
| `SendScanMode()` | 当前未被调用，payload 编码路径未用抓包独立验证 | 不建议作为“已符合原厂”依赖，除非补抓包或单测证明输入 token 语义 |

---

## 8. Type-3 token 编码规则

`BtPen_HandleInitParamEvent` 的 token 编码规则已由 Ghidra 确认，当前 `EncodePenUsbType3Token()` 与之匹配：

| token 长度 | 输出字节 |
|---|---|
| `1` | `[hex(d0)]` |
| `2` | `[hex(d1), hex(d0)]` |
| `3` | `[hex(d1d2), hex(d0)]` |
| `4` | `[hex(d2d3), hex(d0d1)]` |

例：

```text
token 3333 -> 33 33
token 2e7  -> E7 02
token 412  -> 12 04
token 258  -> 58 02
token 411a -> 1A 41
```

---

## 9. 实现结论

当前 `EGoTouchService/Device/penevt` 本身只是事件通道薄封装；协议正确性主要由 `EGoTouchService/Device/btmcu` 与 `DeviceRuntime` 一起完成。

结论：

```text
核心 wire protocol：符合原厂
初始化握手：符合抓包时序
ACK 表：符合原厂
0x7D01 exact payload：符合当前已知 factory capture
核心状态位更新：符合已逆向逻辑
上层行为映射：符合项目目标，但不是 PenKit 管道逐字复刻
```

当前最值得优先补齐的是：

1. 为 `0x12 DEV_CONNECT_STATUS` 增加显式 enum 与状态处理。
2. 为 `0x7E01` 增加专用 builder，避免未来误用 `byte[7]=0x20` 的 generic payload builder。
3. 给 log-only 事件补全名称，提升调试可读性。
4. 放宽帧头校验到只按 `packet[4]` 分流，与原厂读线程一致。详见 `docs/ACCESSORY_CENTER.md`。
