# PenService.dll 事件码表 (HID col00 完整映射)

基于 Ghidra 对 `PenService.dll` 中 `GetInterruptPipeMsg` 及分发表 (`180014130`) 的完整逆向，共识别出 21 个事件码。这些事件主要由 MCU 发送，用于触发操作系统层面的 UI（如配对、电量悬浮窗）或业务逻辑。

下表、槽位归属和后面几节的发送侧内容，后来又用 `dumpbin /disasm` 逐条复核过一遍，槽位由两条
独立的线索交叉确认：处理函数里唯一那条 `[rax+NNNh]` 位移，和每个 `RegisterCallback*` 导出写入
的位移。复核订正了两处，都是名称错位而槽位无误，见文末。

本文以 `PenService.dll` 的 ImageBase `0x180000000` 为准，RVA 与 VA 混用时以 `0x18000xxxx`
形式给出完整 VA。

| 事件码(Hex) | 偏移 | 底层处理函数 | 回调注册函数 (`RegisterCallback...`) | 含义 / 触发功能 |
|------------|-----|------------|-----------------------------------|---------------|
| `0x00` | 0x120 | FUN_18000a220 | `UpdatePenModule` | 笔模块信息更新 |
| `0x01` | 0x128 | FUN_18000a280 | `UpdatePenSerialNo` | 获取/上报笔序列号 (含日志 "HandlePenSerialNumber") |
| `0x02` | 0x130 | FUN_18000a460 | `UpdatePenHardwareVersion` | 获取/上报硬件版本 (含日志 "HandlePenHardwareVersion") |
| `0x03` | 0x138 | FUN_18000a640 | `UpdatePenFirmwareVersion` | 获取/上报固件版本 (含日志 "HandlePenFirmwareVersion") |
| `0x08` | 0x108 | FUN_18000a820 | `UpdateBatteryVolume` | 笔电量百分比更新 |
| `0x09` | 0x118 | FUN_18000a900 | `UpdatePenChargingStatus` | 充电状态更新 (输出 "Charging Status: ") |
| `0x12` | 0x110 | FUN_18000a890 | `UpdatePenConnectStatus` | 笔连接状态（会额外开启防抖动相关检测线程） |
| **`0x15`** | **0x140** | **FUN_18000aae0** | **`NewPenConnectRequest`** | **发起新笔配对请求UI (收到后会回发 0x2E01 ACK)** |
| `0x16` | 0x148 | FUN_18000ab60 | `NewPenConnectResult` | 新笔配对结果通知 |
| `0x24` | 0x150 | FUN_18000aba0 | `TransferPenMode` | 笔模式切换 |
| `0x25` | 0x158 | FUN_18000ac20 | `UpdatePenKeySupport` | 查询笔按键功能支持 |
| `0x26` | 0x160 | FUN_18000ac60 | `UpdatePenKeyFuncSet` | 笔按键功能设置 |
| `0x27` | 0x168 | FUN_18000aca0 | `UpdatePenKeyFuncGet` | 笔按键功能读取 |
| **`0x28`** | **0x170** | **FUN_18000ace0** | **`PenTopBatteryWindow`** | **呼出顶部电量悬浮窗 (纯通知)** |
| **`0x29`** | **0x178** | **FUN_18000ad10** | **`PenCloseConnectWindow`** | **关闭配对/连接窗口提示 (纯通知)** |
| `0x2A` | 0x180 | FUN_18000ad40 | `PenDeviationReminder` | 笔吸附偏移提醒（当前安装版回调未注册） |
| `0x2C` | 0x188 | FUN_18000ad70 | `PenFirstBatAfterConn` | 连接后首次电量播报 |
| `0x2D` | 0x190 | FUN_18000abe0 | `PenOskPrevensionMode` | 软键盘防误触模式开关通知 |
| `0x2F` | 0x1B0 | FUN_18000ae20 | `PenCurrentFunc` | 笔当前激活功能反馈 |
| `0x67` | 0x198 | FUN_18000ada0 | `TpRotateAngle` | 触控板旋转角度 |
| `0x68` | 0x1A0 | FUN_18000ade0 | `SysLang` | 系统语言环境请求/反馈 |

### 逆向发现汇总
通过遍历比对 `PenService.dll` 内部映射表（共 21 项），我们发现不仅 `0x15`, `0x28`, `0x29` 在此列，包括常用的电量(`0x08`)、固件版本(`0x03`)、充电状态(`0x09`) 等管理型指令全部由该组件接管。

> **注意：** `THP_Service.dll` 控制的 `0x71`~`0x7F` 更侧重于数据/书写状态及频率匹配逻辑；而 `PenService.dll` 则更像是一个专职的“辅助/UI交互后台”，这些 `0x00`-`0x68` 事件码属于外围生态范畴。

---

## 1. 发送侧：报文怎么拼出来

所有 `CommandSend*` 导出都汇到同一个发包函数 `sub_1800082F0`，签名可以还原为：

```c
void Send(const uint8_t header[8], const void *payload, int payloadLen);
```

函数体（RVA `0x82F0`）：

```
1800082F0: ...
180008345: lea   ecx,[r13+40h]
180008349: call  qword ptr [18000E3D8h]   ; malloc(0x40)
180008360: mov   ecx,40h
180008365: rep stos byte ptr [rdi]        ; memset 0
180008367: movzx edx,byte ptr [r14]       ; header[0..7] 逐字节复制
18000836B: mov   byte ptr [rbx],dl
...
1800083A5: test  r15,r15                  ; payload == NULL ?
1800083A8: je    1800083F8
1800083AA: mov   r9,rbp                   ; len
1800083AD: lea   rcx,[rbx+8]              ; 目标 buf+8
1800083B1: mov   r8,r15                   ; 源 payload
1800083B4: lea   edx,[r13+38h]            ; 目标容量 0x38
1800083B8: call  180003680                ; memcpy_s
...
1800083FD: lea   r9,[rsp+80h]
180008405: lea   r8d,[rbp+8]              ; 写入长度 = payloadLen + 8
180008409: mov   rdx,rbx
18000840C: mov   rcx,rsi
18000840F: call  qword ptr [18000E100h]   ; WriteFile
```

两点与 `THP_Service.dll` 的 `BtPen_SendPacket` 不同，实现时不要照抄那一份：

- **帧头 8 字节原样透传**，`byte[6]` 不像 `BtPen_SendPacket` 那样被强制改写成 `0x11`。这里
  `0x11` 是每个导出自己在立即数里写好的。
- 缓冲区固定 `malloc(0x40)` 并清零，但 `WriteFile` 的长度是 `payloadLen + 8`，不是 `0x40`。

帧头由调用方用两条 `mov dword ptr [rsp+disp], imm32` 拼在栈上，小端展开即得 8 字节：

| 偏移 | 值 | 含义 |
|---|---|---|
| `byte[0]` | `0x07` | 目标地址，笔子系统所在的 MCU 链路 |
| `byte[1]` | `0x00` | 全部发送点恒为 0，含带 payload 的帧 |
| `byte[2]` | `0x02` | 源地址 = Host |
| `byte[3]` | `0x00` | 恒 0 |
| `byte[4]` | `0x01` | 子系统 ID，笔 |
| `byte[5]` | 命令码 | 见下表 |
| `byte[6]` | `0x11` | 恒 `0x11` |
| `byte[7]` | payload 长度 | 与传给 `Send` 的 `payloadLen` 相等 |

设备路径由 `sub_180007730` 通过 `CM_Get_Device_Interface_ListW` 按接口 GUID
`dd0ebedb-f1d6-4cfa-acca-71e66d3178ca` 枚举得到，与本项目 `PenEventBridge` 是同一个通道。

## 2. 命令表：帧头、payload 与参数处理

`CommandSendPenCurrentFunc` 是模板，其余同构：

```
18000A9B0: sub   rsp,28h
18000A9B4: mov   byte ptr [rsp+30h],cl        ; payload[0] = (uint8)arg
18000A9B8: lea   rdx,[rsp+30h]                ; payload
18000A9BD: lea   rcx,[rsp+38h]                ; header
18000A9C2: mov   dword ptr [rsp+38h],20007h   ; 07 00 02 00
18000A9CA: mov   r8d,1                        ; payloadLen
18000A9D0: mov   dword ptr [rsp+3Ch],1112F01h ; 01 2F 11 01
18000A9D8: call  1800082F0
```

| 导出 | RVA | 帧头 | payload | 参数处理 |
|---|---|---|---:|---|
| `CommandSendTransferPenMode` | `0x9500` | `07 00 02 00 01 24 11 01` | 1 | `cmp ecx,1; sete` — 布尔化 |
| `CommandSendGetPenKeySupport` | `0x9F50` | `07 00 02 00 01 25 11 00` | 0 | 无参数 |
| `CommandSendSetPenKeyFunc` | `0x9FC0` | `07 00 02 00 01 26 11 01` | 1 | `cl`，纯截断 |
| `CommandSendGetPenKeyFunc` | `0xA020` | `07 00 02 00 01 27 11 00` | 0 | 无参数 |
| `CommandSendOskPrevensionMode` | `0xA160` | `07 00 02 00 01 2D 11 01` | 1 | `cmp ecx,1; sete` — 布尔化 |
| `CommandSendTpRotateAngle` | `0xA1E0` | `07 00 02 00 01 67 11 01` | 1 | `cl`，纯截断 |
| `CommandSendPenCurrentFunc` | `0xA9B0` | `07 00 02 00 01 2F 11 01` | 1 | `cl`，纯截断 |
| `CommandSendTouchChr` | `0xA9F0` | `07 00 02 00 01 80 11 04` | 4 | 参数是指针，拷 4 字节 |
| `CommandSendGlobalPreventionChr` | `0xAA20` | `07 00 02 00 01 81 11 04` | 4 | 参数是指针，拷 4 字节 |
| `CommandSendDoubleFuncChr` | `0xAA50` | `07 00 02 00 01 82 11 00` | 0 | 无参数 |
| `CommandSendSysLang` | `0xAAA0` | `07 00 02 00 01 68 11 01` | 1 | `cl`，纯截断 |

**`TransferPenMode` 和 `OskPrevensionMode` 的参数会被布尔化。** 两者都是

```
180009504: cmp   ecx,1
180009522: sete  byte ptr [rsp+30h]
```

即传 `1` 发 `1`，传其他任何值（含 `2`、`-1`）一律发 `0`。其余带 1 字节 payload 的命令是
`mov byte ptr [rsp+30h],cl`，只取低 8 位，没有掩码、偏移或查表。

`CommandSendTouchChr` 和 `CommandSendGlobalPreventionChr` 的参数不是整数而是**指针**
（`mov rdx,rcx` 后直接交给 `memcpy_s` 拷 4 字节）。按整数调用会把整数值当地址解引用。

## 3. 接收侧：两个线程的分工

`GetInterruptPipeMsg`（RVA `0x7DA0`）和 `ProcPipeMsg`（RVA `0xAE60`）不是「读」和「处理」
那么笼统的分工，边界很具体：

**`GetInterruptPipeMsg` 只入队，一个回调都不调。** 它循环 `ReadFile` 0x40 字节，然后：

```
18000818D: cmp   byte ptr [rbp+4],1        ; packet[4] == 0x01，唯一的校验
180008191: jne   180008237                 ; 不等 -> 丢弃，回读循环
180008197: movzx ecx,byte ptr [rbp+5]      ; 事件码
18000819B: mov   rax,r15                   ; r15 = 0x180014130，分发表首
1800081A0: cmp   dword ptr [rax],ecx
1800081A2: je    1800081B2
1800081A4: add   rax,10h                   ; 步长 16
1800081A8: cmp   rax,r14                   ; r14 = 0x180014280，表尾
1800081AB: jl    1800081A0
1800081AD: jmp   180008237                 ; 表里没有 -> 丢弃
1800081B2: ...                             ; 命中：整包搬进 xmm6..xmm9
1800081EE: movups xmmword ptr [rax+r12],xmm6 ; r12 = 0x180018FD8，环形缓冲
180008210: mov   dword ptr [1801CFD8h],ecx   ; 写索引，movzx ecx,al 即 256 项回绕
180008231: call  qword ptr [18000E0B0h]      ; ReleaseSemaphore
```

**分发表在这里当白名单用**：`byte[5]` 不在 21 项里的包直接丢，连队列都进不去。

**`ProcPipeMsg` 出队并调用**，它把同样的两项校验又做了一遍：

```
18000AF61: cmp   byte ptr [rsp+24h],1      ; 复查 packet[4] == 0x01
18000AF66: jne   18000AF90
18000AF68: movzx ecx,byte ptr [rsp+25h]    ; 事件码
18000AF6D: xor   edx,edx                   ; edx = 表内序号
18000AF6F: mov   rax,rdi                   ; rdi = 0x180014130
18000AF72: cmp   dword ptr [rax],ecx
18000AF74: je    18000AF84
18000AF76: inc   rdx
18000AF79: add   rax,10h
18000AF7D: cmp   rax,rbx                   ; rbx = 0x180014280
18000AF80: jl    18000AF72
18000AF84: add   rdx,rdx
18000AF87: lea   rcx,[rsp+20h]             ; 参数 = 整包指针
18000AF8C: call  qword ptr [rdi+rdx*8+8]   ; 处理函数
```

**`GetInterruptPipeMsg` 不发任何包。** 它只做 `CM_Get_Device_Interface_ListW` →
`CreateFileW` → 循环 `ReadFile`，没有 `0x7101` / `0x7701` / `0x7D01` 那套 BT 笔初始化握手——
那套在 `THP_Service.dll` 里，本仓库 `PenUsbInitSession` 抄的是它。只加载 `PenService.dll` 并
不会让笔链路进入已初始化状态。

## 4. 分发表：原始顺序与槽位

`.data` RVA `0x14130`，16 字节一项，前 8 字节事件码、后 8 字节函数指针，共 21 项，表尾
`0x14280`。表内顺序不是升序（`0x16` 在 `0x15` 前、`0x27` 在 `0x26` 前、`0x2F` 在最末），按
原始字节顺序列出：

| 序号 | `byte[5]` | 处理函数 RVA | 槽位 | RegisterCallback 导出 |
|---:|---:|---|---|---|
| 0 | `0x00` | `0x0A220` | `+0x120` | `RegisterCallBackUpdatePenModule` |
| 1 | `0x01` | `0x0A280` | `+0x128` | `RegisterCallBackUpdatePenSerialNo` |
| 2 | `0x02` | `0x0A460` | `+0x130` | `RegisterCallBackUpdatePenHardwareVersion` |
| 3 | `0x03` | `0x0A640` | `+0x138` | `RegisterCallBackUpdatePenFirmwareVersion` |
| 4 | `0x08` | `0x0A820` | `+0x108` | `RegisterCallBackUpdateBatteryVolume` |
| 5 | `0x09` | `0x0A900` | `+0x118` | `RegisterCallBackUpdatePenChargingStatus` |
| 6 | `0x12` | `0x0A890` | `+0x110` | `RegisterCallBackUpdatePenConnectStatus` |
| 7 | `0x16` | `0x0AB60` | `+0x148` | `RegisterCallBackNewPenConnectResult` |
| 8 | `0x15` | `0x0AAE0` | `+0x140` | `RegisterCallBackNewPenConnectRequest` |
| 9 | `0x24` | `0x0ABA0` | `+0x150` | `RegisterCallBackTransferPenMode` |
| 10 | `0x25` | `0x0AC20` | `+0x158` | `RegisterCallBackUpdatePenKeySupport` |
| 11 | `0x27` | `0x0ACA0` | `+0x168` | `RegisterCallBackUpdatePenKeyFuncGet` |
| 12 | `0x26` | `0x0AC60` | `+0x160` | `RegisterCallBackUpdatePenKeyFuncSet` |
| 13 | `0x28` | `0x0ACE0` | `+0x170` | `RegisterCallbackPenTopBatteryWindow` |
| 14 | `0x29` | `0x0AD10` | `+0x178` | `RegisterCallbackPenCloseConnectWindow` |
| 15 | `0x2A` | `0x0AD40` | `+0x180` | `RegisterCallbackPenDeviationReminder` |
| 16 | `0x2C` | `0x0AD70` | `+0x188` | `RegisterCallbackPenFirstBatAfterConn` |
| 17 | `0x2D` | `0x0ABE0` | `+0x190` | `RegisterCallbackPenOskPrevensionMode` |
| 18 | `0x67` | `0x0ADA0` | `+0x198` | `RegisterCallbackTpRotateAngle` |
| 19 | `0x68` | `0x0ADE0` | `+0x1A0` | `RegisterCallbackSysLang` |
| 20 | `0x2F` | `0x0AE20` | `+0x1B0` | `RegisterCallbackPenCurrentFunc` |

有两个导出写的槽位不在这张表里，它们的事件不走中断管道分发：

- `RegisterCallbackPenUpdateRessult` 写 `+0x1A8`，同一个监听对象但无人分发。
- `RegisterCallbackPenUpdateProgress`（`0xA120`）写的是**另一个单例**的 `+0x28`
  （`call 180001320`，而非分发路径上的 `call 1800011E0`）。OTA 进度走 `MbbProtocal` 那条
  独立通道。

## 5. `0x2F` 到回调的路径上没有任何状态判断

这一条决定了排查方向，单独记。处理函数 `0x18000AE20` 全文：

```
18000AE20: push  rbx
18000AE22: sub   rsp,20h
18000AE26: movzx ebx,byte ptr [rcx+8]      ; payload[0]
18000AE2A: call  1800011E0                 ; 监听对象单例
18000AE2F: cmp   qword ptr [rax+1B0h],0    ; 回调槽为空 -> 直接返回
18000AE37: je    18000AE4D
18000AE39: call  1800011E0
18000AE3E: movzx ecx,bl
18000AE41: add   rsp,20h
18000AE45: pop   rbx
18000AE46: jmp   qword ptr [rax+1B0h]      ; 尾调用，唯一参数 = payload[0]
18000AE4D: add   rsp,20h
18000AE51: pop   rbx
18000AE52: ret
```

**唯一的 `cmp` 是「回调指针是否为 NULL」。** 没有全局标志、没有连接状态检查、没有与
`keyFunc` 的比较。`0x24`、`0x25`、`0x26`、`0x27`、`0x28`、`0x29`、`0x2A`、`0x2C`、`0x2D`、
`0x2F`、`0x67`、`0x68` 这十二个处理函数结构逐字相同，只有槽位偏移不同。

从入口到回调的完整路径，除了 `packet[4]==0x01` 和「事件码在分发表里」这两项，再无任何条件。
**所以收不到某个事件只有两种可能：回调没注册，或者 MCU 根本没发。** 我们独占中断端点、双击
笔 45 秒收不到 `0x2F`，回调是注册好的，那就只剩后者。MCU 的上报条件在固件里，`PenService.dll`
里没有任何线索，本轮定不了。

一条相关的旁证：`CD54RPenApp.dll`（modelID `65819`，就是本机这支笔）只有
`CommandSendPenCurrentFunc` / `RegisterCallbackPenCurrentFunc` 的 P/Invoke 声明——那是
`McuInteractTool` 源码模板共用的——却没有 `CallbackPenCurrentFunc` 方法、没有
`CallbackPenCurrentFunc, current pen func is ` 日志串、没有 `Window_Eraser`。全套只在
`CD54PenApp.dll` 里。**厂商自己在 CD54R 上就不指望收到 `0x2F`。**

## 6. 三条 Chr 命令是只发不收的下行命令

`0x80`（`CommandSendTouchChr`）、`0x81`（`CommandSendGlobalPreventionChr`）、
`0x82`（`CommandSendDoubleFuncChr`）都不在上面那张分发表里，发出后 `PenService.dll` 不处理
任何对应回包。

**`CommandSendDoubleFuncChr` 是光杆命令**，无参数、无 payload：

```
18000AA50: sub   rsp,28h
18000AA54: xor   r8d,r8d                     ; payloadLen = 0
18000AA57: mov   dword ptr [rsp+30h],20007h  ; 07 00 02 00
18000AA5F: xor   edx,edx                     ; payload = NULL
18000AA61: mov   dword ptr [rsp+34h],118201h ; 01 82 11 00
18000AA69: lea   rcx,[rsp+30h]
18000AA6E: call  1800082F0
```

名字里的「DoubleFunc」指双击功能，**但它不是双击事件源**：没有参数可传，也没有对应的接收
事件。找双击事件应当看 `0x2F`，不是它。

这三条也不在本机这支笔的链路上：只有 `CD54PenApp.dll` 引用它们，`CD54RPenApp.dll`、
`CD54SPenApp.dll`、`AlitaPenApp.dll`、`CD52PenApp.dll` 都没有。

## 7. 待查：`0x80` 与 `THP_Service` 的 ACK 命令撞码

`CommandSendTouchChr` 的 `byte[5]` 是 `0x80`，而 `THP_Service.dll` 的事件 ACK 命令
（`EGoTouchService/Device/penevt/BTMCU_PROTOCOL.md` 3.1 节的 `0x8001`）用的也是 `0x80`。
两者只在长度上不同：

```
CommandSendTouchChr    07 00 02 00 01 80 11 04 <4 字节>
BtPen_SendEventAck     07 01 02 00 01 80 11 20 <1 字节 ack>
```

MCU 是靠 `byte[7]` 区分这两种用法，还是两份 DLL 里有一份记错了，本轮定不了。往 `0x80` 上加
东西之前先把这条查清。

## 8. 实机结论：`CommandSendPenCurrentFunc(1)` 不产生橡皮

真机验证过，记下来省得再试：调 `CommandSendPenCurrentFunc(1)` 之后

- 笔停止书写，`WM_POINTER` 探针里 `pressure` 恒为 0；
- **HID 的 Eraser / Invert 位始终不置**，OneNote 里擦不掉任何东西。

也就是说这条命令确实改了 MCU 状态，但那个状态没有变成应用能识别的橡皮。

原因在 `PenService.dll` 的能力边界上，静态可证：

- **导入表全量**是 `dwmapi`、`USER32`（仅 `FindWindowW` / `FindWindowExW` / `GetWindowLongW`，
  服务于 `IsTabTipKeyboardVisble`）、`SETUPAPI`（仅 `CM_Get_Device_Interface_ListW` 与
  `_SizeW`）、`MSVCP140`、`KERNEL32`、`WS2_32`、`WINMM`、`VCRUNTIME140`。**没有 `HID.DLL`，
  没有 `SendInput`，没有任何 ink / pointer API。**
- 597 条字符串里 `thp`、`touch`、`tsa`、`himax`、`hid`、`eraser`、`ink` 命中数为 0。

**`PenService.dll` 是一条纯 MCU 侧信道，橡皮位不可能从这里进入书写信号。** 橡皮要让应用看见
就必须出现在 HID 报文里，而报文来自 `THP_Service.dll` → 厂商 VHF 那条链，在 `GaokunThpHost`
内部。厂商自己收到 `0x2F` 后做的也只是弹一个 `Window_Eraser` 提示窗（`CD54PenApp.dll` 的日志
串 `Enter Show EraserWindowShow.`），是 UI 提示，不是输入注入。

## 9. 本轮复核订正的两处

都是名称错位，槽位一直是对的：

- **`0x24`–`0x27` 四行整体挪了一格。** 原记 `0x24 = UpdatePenKeySupport`、
  `0x25 = UpdatePenKeyFuncSet`、`0x26 = UpdatePenKeyFuncGet`、
  `0x27 = TransferPenMode`（推测）。按槽位重新 join 后是 `0x24 = TransferPenMode`、
  `0x25 = UpdatePenKeySupport`、`0x26 = UpdatePenKeyFuncSet`、`0x27 = UpdatePenKeyFuncGet`。
  `docs/ACCESSORY_CENTER.md` 4.2 节的分发表一直是对的，两份文档此前互相矛盾。
  发送侧也印证：`CommandSendTransferPenMode` 的 `byte[5]` 就是 `0x24`，
  `CommandSendSetPenKeyFunc` 是 `0x26`，`CommandSendGetPenKeyFunc` 是 `0x27`。
- **`0x67` 不是 OTA 进度。** 原记 `PenUpdateProgress`（推测）。槽位 `+0x198` 属于
  `RegisterCallbackTpRotateAngle`，且发送侧有同码的 `CommandSendTpRotateAngle`。真正的
  `RegisterCallbackPenUpdateProgress` 写的是另一个单例的 `+0x28`，不经中断管道分发。
