# 键盘 MCU USB 协议（KeyboardService.dll 逆向）

目标：脱离 `KeyboardService.dll` 自行收发「键盘分离无线连接」开关。

分析对象：

```text
C:\Program Files\Huawei\PCManager\components\accessories_center\accessories_app\
  AccessoryApp\Lib\Plugins\Depend\KeyboardService.dll
```

PE 头：`Machine = 0x8664`（amd64），`ImageBase = 0x180000000`，`.text` RVA `0x1000`，大小 `0x55AC`。
全文 RVA 均相对该 ImageBase，可直接用 `KeyboardService.dll + RVA` 定位。

参照物：同目录下的 `PenService.dll`（`ImageBase` 相同），以及本仓库
`EGoTouchService/Device/penevt/BTMCU_PROTOCOL.md`（基于 `THP_Service.dll`）。

> 置信度标记
> - **Confirmed**：从反汇编逐条读出，或字节级比对确认
> - **Likely**：由两份二进制的一致模式推断，未经实机抓包
> - **Open**：需要实机抓包才能定论

---

## 1. 总体结论

`KeyboardService.dll` 与 `PenService.dll` 是同一套代码模板的两份实例：相同的设备打开逻辑、
相同的 8 字节帧头构造函数、相同的读线程 + 环形队列 + 分发表结构。两者打开的是**同一个
USB 设备接口**，帧头里的地址字段区分子系统。

```text
KeyboardService.dll / PenService.dll
  -> CM_Get_Device_Interface_ListW({dd0ebedb-f1d6-4cfa-acca-71e66d3178ca})
    -> CreateFileW(第一个接口路径)
      -> ReadFile/WriteFile 定长 0x40 字节
        -> MCU
          -> 键盘 / 手写笔
```

「键盘分离无线连接」开关完全落在这条通道上，无握手、无加密、无状态机前置条件：
`kbd-detach.c` 只启动两个线程就能收发，`.data` 里两个循环标志位初值即为 1。
**结论：可以脱离 `KeyboardService.dll` 自行实现。**

---

## 2. 设备端点

**与 pen 是同一个设备、同一个端点（Confirmed）。**

两份 DLL 的 `.rdata` 里各有且仅有一份 16 字节 GUID，二进制完全相同：

| 文件 | 文件偏移 | GUID 字节 |
|---|---|---|
| `KeyboardService.dll` | `0x61A0` | `DB BE 0E DD D6 F1 FA 4C AC CA 71 E6 6D 31 78 CA` |
| `PenService.dll` | `0xCFF8` | 同上 |

即 `{dd0ebedb-f1d6-4cfa-acca-71e66d3178ca}`，与 `PenEventBridge::FindDevicePath()` 使用的
interface GUID 一致。

打开逻辑在 `FindDevicePath @ RVA 0x3F00` 与 `OpenDevice @ RVA 0x4130`：

```text
CM_Get_Device_Interface_List_SizeW(&len, GUID, NULL, 0)      ; RVA 0x3F45
CM_Get_Device_Interface_ListW(GUID, NULL, buf, len, 0)       ; RVA 0x4059
取列表中的第一个字符串，截断到 0x100 个 wchar                  ; RVA 0x4079
CreateFileW(path, 0xC0000000, 3, NULL, OPEN_EXISTING, 0x80, NULL)  ; RVA 0x41E0
```

`dwDesiredAccess = GENERIC_READ|GENERIC_WRITE`，`dwShareMode = FILE_SHARE_READ|FILE_SHARE_WRITE`，
同步 I/O，无 `FILE_FLAG_OVERLAPPED`。

**不做任何 VID/PID/MI/usage 过滤**（Confirmed）：它直接取接口列表的第 0 项。这意味着两份 DLL
和本项目的 `PenEventBridge` 会打开同一个 device path，各自 `ReadFile` 会互相抢包——见第 7 节。

设备句柄全局量在 RVA `0xA130`，初值 `0xFFFFFFFFFFFFFFFF`。`kbd-detach.c` 用
`GetInterruptPipeMsg` 序言特征匹配定位的正是它（`mov qword ptr [rip+imm32], rax @ GetInterruptPipeMsg+0x24`，
目标 RVA `0xA130`）。

---

## 3. 报文骨架

### 3.1 Host → MCU（Confirmed）

所有导出的 `CommandSend*` 都把 8 字节帧头拼在栈上（两条 `mov dword ptr [rsp+N], imm32`），
然后调用同一个发送函数 `PacketConstructAndSend @ RVA 0x4930`：

```text
PacketConstructAndSend(const uint8_t header[8], const void* payload, int payloadLen)

  handle = OpenDevice()                       ; RVA 0x495F
  buf = malloc(0x40); memset(buf, 0, 0x40)    ; RVA 0x497D / 0x49A0
  buf[0..7] = header[0..7]                    ; RVA 0x498F..0x49E0，逐字节原样复制
  if (payload && payloadLen) {
      assert(payloadLen <= 0x38);             ; RVA 0x4A09
      memcpy(buf + 8, payload, payloadLen);
  }
  WriteFile(handle, buf, payloadLen + 8, &written, NULL)   ; RVA 0x4A32
```

与 `THP_Service.dll` 的 `BtPen_SendPacket` 有一处关键差异：**这里不强制覆写 `buf[6] = 0x11`**，
8 个字节全部由调用方给定。写入长度同样是 `payloadLen + 8`，与帧头内的字段无关。

字段含义：

| 偏移 | 键盘取值 | pen 取值 | 含义 |
|---|---|---|---|
| `byte[0]` | `0x05` / `0x09` | `0x07` | 目标地址（子系统所在的 MCU 链路） |
| `byte[1]` | `0x00` | `0x00` | 恒 0 |
| `byte[2]` | `0x02` | `0x02` | 源地址 = Host |
| `byte[3]` | `0x00` | `0x00` | 恒 0 |
| `byte[4]` | `0x02` / `0x00` | `0x01` | 子系统 ID（分发用） |
| `byte[5]` | 命令码 | 命令码 | 分发用 |
| `byte[6]` | `0x11` | `0x11` | 协议魔数 |
| `byte[7]` | payload 字节数 | payload 字节数 | 见下 |

简写：

```text
DST 00 02 00 GRP CMD 11 LEN [payload...]
```

### 3.2 对 BTMCU_PROTOCOL.md 的三点修正

**`byte[4]` 不是「命令低字节」，是子系统 ID（Confirmed）。** `BTMCU_PROTOCOL.md` 记的
「请求命令是 `07 00 02 00 | 01 <code> 11 00`，`byte[4]` 已观测均为 `0x01`」——只观测了 pen 一个子系统。
扫描两份 DLL 的全部帧头构造点后：

- `PenService.dll` 24 处发送点，`byte[0]` 全为 `0x07`，`byte[4]` 全为 `0x01`
- `KeyboardService.dll` 15 处发送点，`byte[0]` 为 `0x05`（13 处）或 `0x09`（2 处），
  `byte[4]` 对应为 `0x02` 或 `0x00`

`(byte[0], byte[4])` 严格成对出现：`(0x07, 0x01)` = 手写笔，`(0x05, 0x02)` = 键盘，
`(0x09, 0x00)` = detach-support 所在的第三个子系统。所以文档里「`0x7101`」这种写法实际是
`(byte[5] << 8) | byte[4]`，即「命令码 + 子系统」，这也正好解释了为什么「请求码低字节与应答事件码同值」
——低字节根本不是命令的一部分。

**`byte[7]` 是 payload 长度（Confirmed for these two DLLs）。** 在这两份 DLL 里，`byte[7]`
恒等于传给发送函数的 `payloadLen`：无 payload 写 `0x00`，1 字节 payload 写 `0x01`，
`PenService.dll` 的 `0x80`/`0x81` 命令带 4 字节 payload 就写 `0x04`。这与 RX 侧
`byte[7] = payload length` 是同一个字段。`BTMCU_PROTOCOL.md` 里记的「payload tag / subtype，
已观测 `0x00 / 0x01 / 0x20`」中，`0x20` 对应 `0x7D01` 的 32 字节 payload，也是长度。
唯一对不上的是抓包里的 `0x8001` ACK（`11 20` 但只带 1 字节 ACK code），而 `PenService.dll`
自己的 `0x80` 命令头是 `07 00 02 00 01 80 11 04`。**`THP_Service.dll` 的 `0x8001` 与
`PenService.dll` 的 `0x80` 不是同一条命令，不要互相套用。**（Open）

**`byte[1]` 恒为 0（Confirmed for these two DLLs）。** 两份 DLL 全部 39 个发送点的 `byte[1]`
都是 `0x00`，包括带 payload 的。而本项目 `BuildPenUsbPayloadCommandBuffer()` 在有 payload 时写
`byte[1] = 0x01`，依据是 `THP_Service.dll` 的抓包。两者不一致，说明 `byte[1]` 要么被 MCU 忽略，
要么是 `THP_Service.dll` 独有的用法。**不要据此改动现有 pen 代码**——pen 那边有实机抓包做依据，
这里只是提示该字段并非协议必需。（Open）

### 3.3 MCU → Host（Confirmed）

`GetInterruptPipeMsg @ RVA 0x4320` 是读线程：

```text
loop while (g_loopFlag @ 0xA140) {
    memset(buf, 0, 0x40);
    ReadFile(g_deviceHandle @ 0xA130, buf, 0x40, &read, NULL)   ; RVA 0x4431
    if (失败) { CloseHandle; Sleep(1000); 重新 OpenDevice; continue; }   ; RVA 0x4574
    ...过滤，见下...
}
```

过滤规则（RVA `0x471D`、`0x47C8`）——**只有两类帧会进队列，其余全部丢弃**：

```text
if (buf[4] == 0x02 && buf[5] 命中分发表)  -> 入队
if (buf[4] == 0x00 && buf[5] == 0x35)     -> 入队
```

注意过滤只看 `byte[4]` 和 `byte[5]`，**不校验 `byte[0]`、`byte[2]`、`byte[6]`**。pen 的事件
（`byte[4] == 0x01`）在这里被静默丢弃，反之亦然。

RX 帧字段与 TX 对称，`byte[0]`/`byte[2]` 互换（`byte[0] = 0x02` = Host）：

```text
02 00 DST 00 GRP EVT 11 LEN <payload...>
```

`byte[8]` 是所有已确认事件的取值字节。

队列结构（Confirmed）：环形缓冲区在 RVA `0xAA68`，槽位 `0x40` 字节，写索引 `0xEA68`、
读索引 `0xEA6C` 均为 8 位自回绕（`inc eax; movzx ecx, al`），即 256 槽。信号量 `0xAA30`、
临界区 `0xAA40`。`ProcPipeMsg @ RVA 0x2B50` 是消费线程，`WaitForSingleObject(信号量, INFINITE)`
后取一槽分发。

---

## 4. 「键盘分离无线连接」开关

### 4.1 请求帧（Confirmed，直接从反汇编读出）

**`CommandSendKbdDetachSupportGet()` @ RVA 0x14A0**

```asm
0014a0  4883ec28           sub  rsp, 0x28
0014a4  4533c0             xor  r8d, r8d              ; payloadLen = 0
0014a7  c744243009000200   mov  dword [rsp+0x30], 0x00020009
0014af  33d2               xor  edx, edx              ; payload = NULL
0014b1  c744243400351100   mov  dword [rsp+0x34], 0x00113500
0014b9  488d4c2430         lea  rcx, [rsp+0x30]
0014be  e86d340000         call PacketConstructAndSend
```

小端展开两个立即数：

```text
09 00 02 00 00 35 11 00
```

上线 8 字节。

**`CommandSendKbdDetachSupportSet(uint8_t enable)` @ RVA 0x1440**

```asm
001440  4883ec28           sub  rsp, 0x28
001444  884c2430           mov  byte [rsp+0x30], cl   ; payload[0] = enable
001448  488d542430         lea  rdx, [rsp+0x30]       ; payload
00144d  488d4c2438         lea  rcx, [rsp+0x38]       ; header
001452  c744243809000200   mov  dword [rsp+0x38], 0x00020009
00145a  41b801000000       mov  r8d, 1                ; payloadLen = 1
001460  c744243c00341101   mov  dword [rsp+0x3c], 0x01113400
001468  e8c3340000         call PacketConstructAndSend
```

```text
09 00 02 00 00 34 11 01 <enable>
```

上线 9 字节。`enable` 由调用方原样传入，DLL 不做范围检查；`kbd-detach.c` 传 1/0。

字段解读：

| 偏移 | Get | Set | 含义 |
|---|---|---|---|
| `byte[0]` | `0x09` | `0x09` | 目标地址；与键盘状态类命令的 `0x05` 不同 |
| `byte[1]` | `0x00` | `0x00` | 恒 0 |
| `byte[2]` | `0x02` | `0x02` | 源 = Host |
| `byte[3]` | `0x00` | `0x00` | 恒 0 |
| `byte[4]` | `0x00` | `0x00` | 子系统 ID；键盘状态类是 `0x02` |
| `byte[5]` | `0x35` | `0x34` | 命令码：`0x35` = 查询，`0x34` = 设置 |
| `byte[6]` | `0x11` | `0x11` | 魔数 |
| `byte[7]` | `0x00` | `0x01` | payload 长度 |
| `byte[8]` | — | `enable` | `1` = 启用分离无线连接，`0` = 禁用 |

**关于 `(0x09, 0x00)` 这一对（Likely）**：detach-support 是这两份 DLL 里唯一不落在
`(0x05, 0x02)` 或 `(0x07, 0x01)` 上的命令。合理推断是这个开关由平板主控 MCU 而非键盘自身持有
（键盘分离后仍要能改这个设置，所以不能放在键盘侧）。未经抓包证实，实现时照抄字节即可，
不要试图把它归并到 `0x05/0x02`。

### 4.2 应答帧（Confirmed）

`0x35` 的应答有两条接收路径，都最终调用 `RegisterCallBackKbdDetachSupportGet` 注册的回调：

**路径 A —— `byte[4] == 0x00`，`ProcPipeMsg` 内联特判（RVA 0x2C71）**

```asm
002c71  807c246535   cmp byte [rsp+0x65], 0x35   ; packet[5] == 0x35
002c76  0f85ae000000 jne skip
002c7c  0fb65c2468   movzx ebx, byte [rsp+0x68]  ; ebx = packet[8]
...     日志 "DetachSupportStatus: "
002d22  8bcb         mov ecx, ebx
002d24  ff9068010000 call qword [rax+0x168]      ; 回调槽 +0x168
```

进入这段的前置条件是 `packet[4] == 0`（RVA `0x2C69` 的 `test al, al; jne skip`）。

**路径 B —— `byte[4] == 0x02`，走分发表（handler @ RVA 0x2980）**

分发表命中 `0x35` 后调 `0x2980`，同样取 `packet[8]` 并 tail-call `[obj+0x168]`。

回调槽归属由 `RegisterCallBackKbdDetachSupportGet @ RVA 0x1480` 确认：

```asm
00148e  48899868010000  mov qword [rax+0x168], rbx
```

所以应答帧为：

```text
02 00 09 00 00 35 11 01 <value>      路径 A（Likely：byte[0]/byte[2] 由 TX 对称推断，
                                      接收侧并不校验这两字节）
02 00 05 00 02 35 11 01 <value>      路径 B
```

`value` 语义：`0` = 已禁用，非 0 = 已启用。`kbd-detach.c` 直接 `puts(arg ? "enabled" : "disabled")`，
DLL 侧不做任何映射，原样透传 `packet[8]`。

**`0x34`（Set）的应答（Confirmed 一半）**：分发表里有 `0x34 -> RVA 0x28A0`，
tail-call 回调槽 `+0x160`（= `RegisterCallBackKbdDetachSupportSet`），取值同样是 `packet[8]`。
但**只有 `byte[4] == 0x02` 的 `0x34` 应答会被收下**——读线程的过滤规则里，`byte[4] == 0x00`
只放行 `0x35` 一个码。而请求是用 `byte[4] == 0x00` 发出去的。

如果 MCU 按请求回显子系统 ID（`0x35` 的路径 A 特判说明它确实会回显 `0x00`），那么
Set 的应答会被读线程丢掉，`RegisterCallBackKbdDetachSupportSet` 永远不触发。这与
`kbd-detach.c` 的写法吻合：它 set 完直接 `return 0`，从不等应答。

**实现建议：Set 之后重新发一次 Get 来确认生效，不要等 `0x34` 应答。**（Likely）

### 4.3 是否会周期广播

`0x31 DetachStatus`（`byte[4] == 0x02`）是独立的一条，handler `RVA 0x27C0` → 回调槽 `+0x110`
（`RegisterCallBackUpdateDetachStatus`），日志前缀 `"DetachStatus: "`。它与
`0x34/0x35` 的 `"DetachSupportStatus: "` 是两件事：前者是「键盘当前是否已分离」，
后者是「是否允许分离后无线连接」。

读线程对分发表里的任何码都无条件放行，不区分是否是自己请求的应答，所以 MCU 可以主动推送。
但 **`0x35` 是否会被主动广播、以及广播值是否可信，二进制里读不出来**（Open）。参照
`BTMCU_PROTOCOL.md` 里 charging 的坑，建议：只信任自己发 Get 之后 100ms 窗口内收到的
`0x35`，把窗口外的当作提示、重新 Get 一次核对。

---

## 5. 完整命令 / 事件表

### 5.1 Host → MCU（Confirmed，全部来自帧头构造点扫描 + 逐条反汇编）

| 导出符号 | 函数 RVA | 帧头字节 | payload |
|---|---|---|---|
| `CommandSendGetKeyboardModule` | `0x11D0` | `05 00 02 00 02 00 11 00` | 无 |
| `CommandSendGetKeyboardSerialNo` | `0x13D0` | `05 00 02 00 02 01 11 00` | 无 |
| `CommandSendGetKeyboardHardwareVersion` | `0x1690` | `05 00 02 00 02 02 11 00` | 无 |
| `CommandSendGetKeyboardFirmwareVersion` | `0x18A0` | `05 00 02 00 02 03 11 00` | 无 |
| `CommandSendGetKeyboardBattery` | `0x1AB0` | `05 00 02 00 02 08 11 00` | 无 |
| `CommandSendGetKeyboardConnectStatus` | `0x1B00` | `05 00 02 00 02 12 11 00` | 无 |
| `CommandSendGetKeyboardChargingStatus` | `0x1B70` | `05 00 02 00 02 09 11 01` | 无（见下） |
| `CommandSendNewConnectRespond` | `0x1C10` | `05 00 02 00 02 15 11 01` | 1 字节 |
| `CommandSendGetKeyboardDetachStatus` | `0x2A60` | `05 00 02 00 02 31 11 00` | 无 |
| `CommandSendKbdDetachSupportSet` | `0x1440` | `09 00 02 00 00 34 11 01` | 1 字节 enable |
| `CommandSendKbdDetachSupportGet` | `0x14A0` | `09 00 02 00 00 35 11 00` | 无 |
| 内部（`0x12` 收到后由新线程发） | `0x24D0` | `05 00 02 00 02 65 11 00` | 无 |
| 内部（`0x39` 收到后由新线程发） | `0x2A90` | `05 00 02 00 02 40 11 00` | 无 |

**`0x09` charging 的 `byte[7]` 是原厂 bug（Confirmed）**：`CommandSendGetKeyboardChargingStatus`
写了 `byte[7] = 0x01`，但 `edx`（payload 指针）和 `r8d`（长度）都是 0，所以实际
`WriteFile` 只发 8 字节，帧头声称有 1 字节 payload 而 payload 不存在。复刻时照原样发 8 字节。

当前实现已按这个怪例在连接初始化时查询一次，并消费键盘子系统周期上报的 `0x09`：真实值经
`PenEventBridge::KbdState` 和 `PenStatusChannel` 送到设置页。UI 只有在该字段已知且为真时显示
“正在充电”，不会用“已吸附”推断充电。

**没有 ACK 机制（Confirmed by absence）**：`KeyboardService.dll` 里除上表外没有任何发送点。
pen 那套「收到事件回 `0x8001` ACK」在键盘子系统上不存在。收到事件不需要回任何东西。

### 5.2 MCU → Host 分发表

分发表在 RVA `0xA040`，13 项，步长 `0x10`（`+0x00` 是 4 字节 key，`+0x08` 是函数指针），
表尾 `0xA110`。`byte[4] == 0x02` 时按 `byte[5]` 线性查表。

| `byte[5]` | handler RVA | 取值 | 回调槽 | 对应 `RegisterCallBack*` |
|---|---|---|---|---|
| `0x00` | `0x1D20` | 字符串 | `+0x128` | `UpdateKeyboardModule` |
| `0x01` | `0x1EF0` | 字符串 | `+0x130` | `UpdateKeyboardSerialNo` |
| `0x02` | `0x20D0` | 字符串 | `+0x138` | `UpdateKeyboardHardwareVersion` |
| `0x03` | `0x22B0` | 字符串 | `+0x140` | `UpdateKeyboardFirmwareVersion` |
| `0x08` | `0x2490` | `packet[8]` | `+0x108` | `UpdateBatteryVolume` |
| `0x09` | `0x2570` | `packet[8]` | `+0x120` | `UpdateKeyboardChargingStatus` |
| `0x12` | `0x2500` | `packet[8] ∈ {1,2,3}` → 1 | `+0x118` | `UpdateKeyboardConnectStatus` |
| `0x15` | `0x25B0` | 结构体 | `+0x148` | `NewKeyboardConnectRequest` |
| `0x16` | `0x2780` | `packet[8] == 1`；`>1` 直接丢弃 | `+0x150` | `NewKeyboardConnectResult` |
| `0x31` | `0x27C0` | `packet[8]` | `+0x110` | `UpdateDetachStatus` |
| `0x34` | `0x28A0` | `packet[8]` | `+0x160` | `KbdDetachSupportSet` |
| `0x35` | `0x2980` | `packet[8]` | `+0x168` | `KbdDetachSupportGet` |
| `0x39` | `0x2AC0` | 恒传 0 | `+0x170` | `KbdConnectResult` |

外加 `byte[4] == 0x00` 且 `byte[5] == 0x35` 的内联路径 → `+0x168`。

两条事件会触发 DLL 自动回发一条查询（都是新起一个线程发）：

```text
收到 0x12（连接状态）  -> 回调 +0x118 -> 发 05 00 02 00 02 65 11 00
收到 0x39（连接结果）  -> 回调 +0x170(0) -> 发 05 00 02 00 02 40 11 00
```

`0x65` 与 `0x40` 的语义未知（Open）。若只做 detach 开关，这两条可以不实现。

未在分发表中的码（包括 `0x40` / `0x65` 自己的应答）会被读线程丢弃——原厂 DLL 发了查询却收不到应答，
这看起来同样是 bug，但不影响 detach 功能。

---

## 6. 脱离 DLL 自行实现

### 6.1 最小实现

```text
1. CM_Get_Device_Interface_List_SizeW / CM_Get_Device_Interface_ListW
   GUID = {dd0ebedb-f1d6-4cfa-acca-71e66d3178ca}，取第 0 项
2. CreateFileW(path, GENERIC_READ|GENERIC_WRITE,
                FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL)
3. 读线程：ReadFile(h, buf, 0x40) 循环；失败则 Sleep(1000) 重开句柄
4. 查询：WriteFile(h, "09 00 02 00 00 35 11 00", 8)
   等 buf[5] == 0x35 && (buf[4] == 0x00 || buf[4] == 0x02) 的帧，取 buf[8]
5. 设置：WriteFile(h, "09 00 02 00 00 34 11 01 <enable>", 9)
   然后重发一次步骤 4 确认
```

无握手、无初始化参数、无 ACK、无加密。这一点与 pen 侧的
`0x7101 → 0x7701 → 0x77 → 0x7701 → 0x7B → 0x7D01` 握手序列形成对比：键盘子系统不需要任何前置交互。

超时参数照 `kbd-detach.c`：发 Get 后等 100ms，最多重试 3 次。

### 6.2 对接到 `PenEventBridge` 式结构的字段映射

现有 `btmcu` 的类型与本协议的对应关系：

| 现有构件 | 键盘侧对应 | 需要的改动 |
|---|---|---|
| `kPenUsbPacketCapacity = 64` | 同 | 无 |
| `kPenUsbHeaderSize = 8` | 同 | 无 |
| `PenUsbPacketBuffer` | 同 | 无 |
| `BuildPenUsbCommandBuffer()` | 硬编码 `bytes[0]=0x07`、`bytes[4]=id&0xFF` | 需要把 `(DST, GRP)` 提为参数，或另写 builder |
| `PenUsbCommandId`（`(code<<8)\|grp`） | 键盘为 `0x3500`/`0x3400`/`0x3102` 等 | 编码方式可沿用，但 `grp` 不再恒为 `0x01` |
| `TryParsePenUsbEventFrame()` | 校验 `packet[2]==0x07 && packet[4]==0x01` | 键盘帧过不了这两条校验，需要放开 |
| `GetFactoryBtMcuAckCode()` | 键盘无 ACK | 键盘事件不得走 ACK 路径 |
| `PenUsbInitSession` | 键盘无握手 | 键盘通道不需要 init session |
| `PenUsbTransportWin32` | 同一 device path | **可直接复用同一个句柄，见 6.3** |

建议的最小改造：把帧头构造和解析里的 `0x07` / `0x01` 提成 `(destination, subsystem)` 常量对，
`Pen = {0x07, 0x01}`、`Keyboard = {0x05, 0x02}`、`KbdDetachSupport = {0x09, 0x00}`；
RX 解析改成按 `packet[4]` 路由到不同的 handler 表，`packet[2]` 不再硬校验 `0x07`
（键盘侧原厂本来就不校验这个字节）。

### 6.3 必须注意的冲突

**同一个 device path 上的多读者问题（Confirmed 事实，后果为 Likely）**：`PenService.dll`、
`KeyboardService.dll`、本项目的 `PenEventBridge` 打开的是同一个接口、同一个句柄路径，
每一方都在 `ReadFile` 0x40 字节。USB 中断管道的一个包只会交付给一个读者，谁抢到算谁的。

两份原厂 DLL 靠「按 `byte[4]` 丢弃不属于自己的帧」共存，代价是丢包——键盘 DLL 吃掉的 pen 事件
不会回到 pen DLL 手里。

所以：**不要在 `PenEventBridge` 之外再开一个独立句柄跑键盘读线程**。正确做法是在
`PenEventBridge` 已有的读循环里按 `packet[4]` 分流，`0x01` 走 pen、`0x02`/`0x00` 走键盘。
这样既不丢包，也不需要第二个线程。

### 6.4 还没摸清的部分

| 项 | 状态 | 需要什么 |
|---|---|---|
| `byte[0]` 的确切语义（为何 detach-support 用 `0x09`） | Open | 抓包，或找到处理 `0x09` 的固件/其他上位机组件 |
| `0x35` 是否会被 MCU 主动周期广播 | Open | 实机长时间抓包 |
| `0x34`（Set）是否真的有应答、回显哪个 `byte[4]` | Open | 实机抓包 Set 前后的 RX |
| `enable` 是否只接受 0/1 | Open | DLL 不校验；实机试 `0x02` 观察 |
| `byte[1]` 的作用 | Open | 两份 DLL 恒为 0，`THP_Service.dll` 有非 0 |
| `0x40` / `0x65` 语义 | Open | 抓包；与 detach 功能无关 |
| RX 帧的 `byte[0]`/`byte[2]`/`byte[6]` 实际取值 | Likely（由 TX 对称推断） | 抓包；接收侧不校验，不影响实现 |

以上 Open 项**都不阻塞 detach 开关的自实现**：请求字节是从反汇编逐条读出的确定值，
应答的分发键（`byte[4]`、`byte[5]`）和取值位置（`byte[8]`）也是确定的。

---

## 7. 实机验证入口

`KeyboardService.dll` 自带文件日志（`.rdata` RVA `0x7750`）：

```text
C://ProgramData//Huawei//HuaweiKeyboardAPP//Service_LogFile-YYYYMMDD.txt
```

日志前缀可用来对照本文的事件命名：`"DetachStatus: "`（`0x31`）、
`"DetachSupportStatus: "`（`0x34` / `0x35` / 内联路径）。日志里打的就是 `packet[8]` 的十进制值。

跑一遍 `kbd-detach.exe`（enable / disable / 查询各一次），比对该日志与自实现读到的字节，
是成本最低的一次端到端验证。

---

## 8. 分析方法

复现路径，脚本在 worktree 的 `scratch/`（不提交）：

```text
pefile 读导出表 / IAT / 节表
capstone x86-64 反汇编指定 RVA 区间，解析 rip-relative 目标
正则扫 .text 里的 c7 44 24 <disp8> <imm32> 对，按 disp/disp+4 配对还原 8 字节帧头
二进制比对两份 DLL 的 interface GUID
```

关键 RVA 速查：

```text
0x14A0  CommandSendKbdDetachSupportGet
0x1440  CommandSendKbdDetachSupportSet
0x1480  RegisterCallBackKbdDetachSupportGet   -> 回调槽 +0x168
0x1420  RegisterCallBackKbdDetachSupportSet   -> 回调槽 +0x160
0x4930  PacketConstructAndSend(header8, payload, len)
0x4130  OpenDevice
0x3F00  FindDevicePath
0x4320  GetInterruptPipeMsg（读线程）
0x2B50  ProcPipeMsg（分发线程）
0x2980  handler 0x35
0x28A0  handler 0x34
0x27C0  handler 0x31
0xA040  分发表（13 项 × 0x10）
0xA110  ProcPipeMsg 循环标志（初值 1）
0xA130  g_deviceHandle（初值 INVALID_HANDLE_VALUE）
0xA140  GetInterruptPipeMsg 循环标志（初值 1）
0xAA30  队列信号量  0xAA40 临界区  0xAA68 环形缓冲区
0xEA68  写索引      0xEA6C 读索引
```
