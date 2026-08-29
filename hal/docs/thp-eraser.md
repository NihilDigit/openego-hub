# 原厂橡皮擦链路逆向记录

本文回答一个问题：向 MCU 发出 `CommandSendPenCurrentFunc(1)` 之后，橡皮为什么没有变成
HID 报告里的 `Invert(0x3C)` 与 `Eraser(0x45)` 位。

**结论是这条链路在厂商代码里是完整的，位序也正确，断点不在代码里。** 反汇编能证明：事件
落到一个全局、报告写入函数每帧读它、置位逻辑与驱动的报告描述符逐位吻合、没有旁路、没有筛
选、算法链不碰那两位。本机实测也证明那个全局确实随命令在 0 与 1 之间翻转。可是全局为 1
期间划笔，`penFlags` 仍是 `NONE`。

这个矛盾没有解开。本文记录已经查实的机制和**已经排除的每一条假设**，免得下一个人重走。

结论来源分三类，文中逐条标注：

- **原生反汇编**：`THP_Service.dll`、`ApDaemon.dll` 经 `dumpbin /disasm`。
- **驱动数据**：`C:\Windows\System32\drivers\HidInjectorThp.sys` 内嵌的 HID 报告描述符，
  手工解码。
- **本机实测**：对运行中的 `GaokunThpHost` 用 `ReadProcessMemory` 只读观测，配合
  `WM_POINTER` 探针。全程未写目标进程内存、未注入、未下断点。

`THP_Service.dll` 的 ImageBase 是 `0x180000000`。文中给 RVA，反汇编片段沿用 dumpbin 打印
的完整 VA。

## 一、链路全貌

```
MCU ── USB 中断端点 ──> THP_Service.dll 的 USB 读线程
                          事件 0x7F ERASER_TOGGLE
                          └─> 全局 [0xB6A8C] = payload

Himax SPI ─> ApDaemon ─> TSACore ─> ApDaemon 组装 13 字节笔报告（obj+0x674）
                                      └─> API 表 +0x08 = THP_Service!0x14620
                                            读 [0xB6A8C]
                                            == 1 ? 置 Invert|Eraser、清 Tip Switch
                                                 : 清 Invert|Eraser
                                            └─> WriteFile ─> HidInjectorThp ─> VHF
```

两条链在 `THP_Service.dll` 里交汇，交汇点只有 `[0xB6A8C]` 这一个 4 字节全局。

## 二、`ERASER_TOGGLE` 事件与全局 `0xB6A8C`

### 2.1 事件分发

`USB_AsynchProcThreadProc` 对每个 64 字节包先做两项校验，再查跳转表：

```
000000018000EB04: cmp   al,byte ptr [1800B6A2Ah]     ; packet[4] == 0x01
000000018000EB17: cmp   al,byte ptr [1800B6A28h]     ; packet[2] == 0x07
000000018000EB23: shr   rcx,28h                      ; packet[5] = 事件码
000000018000EB2A: add   eax,0FFFFFFFDh
000000018000EB2D: cmp   eax,7Ch                      ; 有效范围 0x03..0x7F
000000018000EB38: movzx eax,byte ptr [r14+rax+0FC54h]  ; 索引表
000000018000EB41: mov   ecx,dword ptr [r14+rax*4+0FBF0h] ; 目标表
000000018000EB4C: jmp   rcx
```

期望值来自 `Usb_Start` 里的 `mov dword ptr [1800B6A28h],12010207h`，小端展开即
`[0xB6A28]=0x07`、`[0xB6A2A]=0x01`。

索引表 `0xFC54` 与目标表 `0xFBF0` 解出 24 个非默认分支：

| 事件 | handler | 事件 | handler | 事件 | handler |
|---|---|---|---|---|---|
| `0x03` | `0x0EB4E` | `0x2C` | `0x0EC7D` | `0x76` | `0x0F554` |
| `0x08` | `0x0EC53` | `0x2E` | `0x0EC8F` | `0x77` | `0x0F62E` |
| `0x09` | `0x0EC65` | `0x2F` | `0x0F89E` | `0x78` | `0x0F6A0` |
| `0x10` | `0x0EB87` | `0x70` | `0x0ECC5` | `0x79` | `0x0F748` |
| `0x12` | `0x0EB96` | `0x71` | `0x0EE03` | `0x7B` | `0x0F85E` |
| `0x21` | `0x0EC71` | `0x72` | `0x0F0D0` | `0x7C` | `0x0F986` |
| `0x23` | `0x0ECA1` | `0x73` | `0x0FA6A` | `0x7F` | `0x0F7BA` |
| `0x27` | `0x0ECB3` | `0x74` | `0x0F247` | `0x75` | `0x0F41C` |

其余 101 个码落到默认分支 `0x0FB0B`，直接丢弃。

### 2.2 `0x7F` 的处理函数

带字节的完整反汇编。**从取 payload 到写全局，中间一条跳转指令都没有：**

```
000000018000F7BA: 0F B6 5C 24 78     movzx  ebx,byte ptr [rsp+78h]   ; packet[8] = payload
000000018000F7BF: 8B D3              mov    edx,ebx
000000018000F7C1: 48 8D 4C 24 50     lea    rcx,[rsp+50h]
000000018000F7C6: E8 15 8E FF FF     call   1800085E0                ; to_string(payload)
000000018000F7CB: 90                 nop
000000018000F7CC: 41 B9 12 00 00 00  mov    r9d,12h
000000018000F7D2: 4C 8D 05 CF 75 07  lea    r8,[180086DA8h]          ; "[USB]ERASER_TOGGLE"
000000018000F7D9: 33 D2              xor    edx,edx
000000018000F7DB: 48 8B C8           mov    rcx,rax
000000018000F7DE: E8 7D 86 FF FF     call   180007E60                ; 拼串
000000018000F7E3: 48 89 74 24 30     mov    qword ptr [rsp+30h],rsi
000000018000F7E8: 48 89 74 24 38     mov    qword ptr [rsp+38h],rsi
000000018000F7ED: 0F 10 00           movups xmm0,xmmword ptr [rax]
000000018000F7F0: 0F 11 44 24 20     movups xmmword ptr [rsp+20h],xmm0
000000018000F7F5: 0F 10 48 10        movups xmm1,xmmword ptr [rax+10h]
000000018000F7F9: 0F 11 4C 24 30     movups xmmword ptr [rsp+30h],xmm1
000000018000F7FE: 48 89 70 10        mov    qword ptr [rax+10h],rsi
000000018000F802: 48 C7 40 18 0F ..  mov    qword ptr [rax+18h],0Fh
000000018000F80A: C6 00 00           mov    byte ptr [rax],0
000000018000F80D: 48 8D 4C 24 20     lea    rcx,[rsp+20h]
000000018000F812: E8 59 53 FF FF     call   180004B70                ; 落盘的日志调用
000000018000F817: 90                 nop
000000018000F818: 48 8D 4C 24 50     lea    rcx,[rsp+50h]
000000018000F81D: E8 9E 1E FF FF     call   1800016C0                ; 析构临时串
000000018000F822: 89 1D 64 72 0A 00  mov    dword ptr [1800B6A8Ch],ebx  ; 写全局
000000018000F844: mov cl,9
000000018000F846: call 180013D50                                     ; SendEventAck(0x09)
```

中间 0x50 字节全是 `std::string` 的构造、拷贝与析构。`ebx` 是被调用者保存寄存器，三次
`call` 都必须保住它。日志里出现 `ERASER_TOGGLE1`，全局就一定被写成了 1——除非中间抛异常，
但那会一路传出去，不会只跳过这一条 `mov`。

### 2.3 全局的性质

`0x1800B6A8C` 落在 `.data` 的未初始化尾部（`.data` VA `0xB2000`，raw 只有 `0x3800`，而偏移
是 `0x4A8C`），进程启动时为 0。

**写入点只有 `0x0F822` 一处，读取点只有 `0x10460` 一处：**

```
0000000180010460: 8B 05 26 66 0A 00  mov  eax,dword ptr [1800B6A8Ch]
0000000180010466: C3                 ret
```

两条都是 RIP 相对寻址，位移在链接期固定：`0x18000F828 + 0xA7264` 与
`0x180010466 + 0xA6626` 都等于 `0x1800B6A8C`。ASLR 只平移整个映像，RIP 跟着一起平移，两者
永远指向同一个地址。

`ERASER_TOGGLE` 的 payload 原值直接进全局，没有任何变换，所以 payload 为 0 的那条事件走同
一条指令把它写回 0。**复位路径存在，但同样只能由 MCU 驱动。**

## 三、笔报告写入函数 `0x14620`

### 3.1 反汇编

```
0000000180014620: push  rbx
0000000180014622: sub   rsp,90h
0000000180014644: mov   rbx,rcx                     ; rcx = 13 字节报告缓冲
0000000180014647: test  rcx,rcx
000000018001464A: je    180014678
000000018001464C: call  180010460                   ; 读 [0xB6A8C]
0000000180014651: cmp   eax,1
0000000180014654: jne   180014663
0000000180014656: movzx eax,byte ptr [rbx+1]        ; 按钮字节
000000018001465A: and   al,0FEh                     ;   清 bit0
000000018001465C: or    al,0Ch                      ;   置 bit2 | bit3
000000018001465E: mov   byte ptr [rbx+1],al
0000000180014661: jmp   180014667
0000000180014663: and   byte ptr [rbx+1],0F3h       ; 否则强制清 bit2 | bit3
0000000180014667: mov   rcx,qword ptr [1800C9380h]  ; VHF 注入器句柄
000000018001466E: test  rcx,rcx
0000000180014671: jne   180014682
0000000180014673: call  180014420                   ; 句柄为空 -> 重开，本帧不写
0000000180014678: mov   eax,1
000000018001467D: jmp   180014729
0000000180014682: cmp   byte ptr [1800C938Ch],0
0000000180014689: jne   180014678                   ; 抑制标志置位 -> 本帧不写
00000001800146A1: lea   r8d,[rax+0Dh]               ; 长度 13
00000001800146A5: mov   rdx,rbx
00000001800146A8: call  qword ptr [1800840C0h]      ; WriteFile
                  ... 失败时日志 "[VHF]WritePenFile failed! error = "
```

`else` 分支是强制清零：算法链无论如何都写不出这两位，唯一来源就是那个全局。

`THP_Service.dll` 里对 `WriteFile` 导入桩 `[0x1800840C0]` 的全部调用点共 12 处，其中只有两
处以 VHF 句柄 `0x1800C9380` 为目标：`0x1455B` 写 `0x20` 字节的手指报告，`0x146A8` 写
`0x0D` 字节的笔报告。**向 VHF 写笔报告的地方只有这一个。**

### 3.2 报告描述符

`HidInjectorThp.sys`（服务名 `HidInjectorThp`，设备 `ROOT\THPDEVICE\0000`）内嵌的笔集合，
文件偏移 `0x3C0A`：

```
05 0D       Usage Page (Digitizer)
09 02       Usage (Pen)
A1 01       Collection (Application)
85 08         Report ID (8)
09 20         Usage (Stylus)
A1 00         Collection (Physical)
09 42           Tip Switch       bit  8
09 44           Barrel Switch    bit  9
09 3C           Invert           bit 10
09 45           Eraser           bit 11
15 00 25 01 75 01 95 04 81 02
95 01 81 03     常量填充          bit 12
09 32           In Range         bit 13
95 02 81 03     常量填充          bits 14..15
95 01 75 08
09 51           Contact Id       bits 16..23
05 01 09 30     X                bits 24..39
      09 31     Y                bits 40..55
05 0D 09 30     Tip Pressure     bits 56..71
      09 3D     X Tilt           bits 72..87
      09 3E     Y Tilt           bits 88..103
```

总长 104 bit = 13 字节，与 `lea r8d,[rax+0Dh]` 吻合。交叉校验：同一驱动里手指集合
（Report ID 1）总长 256 bit = 32 字节，正好是手指写入函数的 `0x20`。

由此得到 13 字节布局：

| 字节 | 内容 |
|---|---|
| `[0]` | Report ID = `0x08` |
| `[1]` | bit0 Tip Switch、bit1 Barrel Switch、bit2 Invert、bit3 Eraser、bit5 In Range |
| `[2]` | Contact Id |
| `[3..4]` | X |
| `[5..6]` | Y |
| `[7..8]` | Tip Pressure |
| `[9..10]` | X Tilt |
| `[11..12]` | Y Tilt |

于是三条位运算的含义是确定的：

- `or al,0Ch` —— 置 Invert 与 Eraser
- `and al,0FEh` —— 清 Tip Switch
- `and al,0F3h` —— 清 Invert 与 Eraser

这正是标准的按键式橡皮语义，一位不差。

## 四、ApDaemon 一侧

`ApDaemon.dll` 的导入表里没有 `CreateFileW`、`WriteFile`、`DeviceIoControl`，HID 相关只有
`HidD_GetHidGuid` 加四个 `SetupDi*`。**它不可能自己写 VHF**，只能通过 `FunInitList` 收到的
API 表回调 `THP_Service`。

API 表由 `THP_Service!0x144A0` 填充，基址存在 `[0xB2000]`：

| 槽位 | 目标 | 作用 |
|---|---|---|
| `+0x00` | `0x144E0` | 写手指报告，32 字节，日志 `[VHF]WriteFingerFile failed!` |
| `+0x08` | `0x14620` | 写笔报告，13 字节，日志 `[VHF]WritePenFile failed!` |
| `+0x10` | `0x14750` | `[VHF]Vhf_Start!`，打开注入器 |
| `+0x18` | `0x147A0` | `[VHF]Vhf_Stop!`，关闭 |

ApDaemon 把表指针存进对象成员 `+0x6F8`，笔报告的组装与调用是全 DLL 唯一的一处：

```
0000000180059BB8: mov  rax,qword ptr [rdi+6F8h]     ; API 表
0000000180059BBF: mov  rdx,qword ptr [rax+8]        ; +0x08 = WritePenReport
0000000180059BC3: lea  rcx,[rdi+674h]              ; 常驻的 13 字节缓冲
0000000180059BCA: call rdx
```

`rcx` 是对象基址加固定偏移，是成员地址不是指针字段，**不可能为空**。

报告缓冲是对象的常驻成员而非每帧新建的栈缓冲，初始化在 `0x4CBDD`
（`mov word ptr [rsi+674h],2008h`，即 Report ID = 8、按钮字节 = `0x20`）。ApDaemon 每帧只把
两组位异或合并进去：

```
0000000180059B20: movzx ecx,byte ptr [rsi+8]
0000000180059B24: xor   cl,byte ptr [rdi+675h]
0000000180059B2A: and   cl,1                       ; dst = (dst & ~1) | (src & 1)
0000000180059B2D: xor   cl,byte ptr [rdi+675h]
0000000180059B33: mov   byte ptr [rdi+675h],cl
0000000180059B39: movzx eax,byte ptr [rsi+4]
0000000180059B3D: shl   al,5
0000000180059B40: xor   al,cl
0000000180059B42: and   al,60h                     ; dst = (dst & ~0x60) | ((src<<5) & 0x60)
0000000180059B44: xor   al,cl
0000000180059B46: mov   byte ptr [rdi+675h],al
```

**bit2 与 bit3 它一次都不碰。** 而且缓冲常驻，这两位一旦被置上会一直留着，直到某一帧走
`0x14620` 的 `else` 分支被 `and 0F3h` 清掉。这一点是排除法的关键：只要橡皮分支曾经执行过
哪怕一帧，`penFlags` 就应当出现过一次 `PEN_FLAG_INVERTED`。

算法链里也没有橡皮的概念：`ApDaemon.dll`、`TSACore.dll`、`TSAPrmt.dll`、`himax_thp_drv.dll`
四个文件里 `raser`、`nvert`、`TOGGLE` 三个子串的命中数全部为 0。

## 五、一处被推翻的推断：`pressure=0` 与 Tip Switch 无关

最初看到 `and al,0FEh` 清 Tip Switch，又实测到「发完命令笔写不出字、`pressure` 恒 0」，很
自然会把两者连起来，认为橡皮分支执行了、只是 `or al,0Ch` 莫名失效。

**这个推断是错的，推翻它的是 `WM_POINTER` 里的 `DOWN` 事件本身。** 能产生 `DOWN` 的接触位
只有 Tip Switch 和 Eraser 两个。既然 `penFlags` 是 `NONE`，Eraser 位为 0，那么置着的只能是
Tip Switch——也就是说 `and al,0FEh` 根本没有执行。而 `and al,0FEh` 与 `or al,0Ch` 在同一个
基本块内，中间没有分支，不存在「清了 bit0 却没置 bit2/bit3」的可能。

报告里的压力字段由 ApDaemon 从帧结构直接搬运：

```
0000000180059B4C: movzx eax,word ptr [rsi+24h]
0000000180059B50: mov   word ptr [rdi+67Bh],ax
```

而压力的物理来源不是 SPI，是 MCU 的压力 HID 集合——ApDaemon 里 `[HID]Find HUAWEI MCU HID
DEVICE FOR PRESSURE` 找的就是它，对应本项目的 `PenPressureReader`（col01）。

后一轮实测中，全局为 1 期间划笔读到 `pressure=906`，是正常压力。**所以压力与橡皮状态之间
也不存在简单的联动。** 第一轮那次 `pressure=0` 的成因本文没有定论，能确定的只有一条：它不
是 Tip Switch 被清造成的。

## 六、已经排除的假设

每条都附判据。这一节的用途是告诉下一个人哪些路已经走死。

**日志不可信，`ERASER_TOGGLE1` 打出来时全局可能还没写。** 排除。第 2.2 节的带字节反汇编
显示日志调用与写全局之间零跳转，`ebx` 全程保持。

**读写不是同一个地址，ASLR 下指向不同页。** 排除。两处都是 RIP 相对寻址，位移链接期固定，
解析结果恒为「实际基址 + 0xB6A8C」，结构上无法拆散。实测也确认：发
`CommandSendPenCurrentFunc(1)` 后读到 `eraserToggle = 1`，发 `(0)` 后读到 `0`，跟着命令正
确翻转。

**存在第三处写入点把全局改回 0。** 排除。搜索范围放到整个 `0x1800B6A00`–`0x1800B6AFF`，
共 34 处引用，逐个追 `lea` 的去向：`0xB6A10` 是读线程的 `lpParameter`，该线程对它只有一处
读 `cmp [rsi+0Ch]`，全函数无写入；`0xB6A78` 是 `CreateThread` 的 `lpThreadId`；`0xB6A90` 与
`0xB6A98` 是投进队列的 `{ctx, fn}` 对，配对函数 `0x180005D10` 对该指针只做
`movsxd rbx,dword ptr [rbx]`，读而不写；`0xB6A94` 传给日志格式化；`0xB6AB0` 是临界区对象。
没有 `memset` 或 `rep stos` 覆盖这一段。仍存的理论盲区：从很远的基址算术合成指针，MSVC 访问
互不相干的全局不会这么生成，但这是判断不是证明。

**`0x14620` 收到空指针，整帧跳过。** 排除。全 ApDaemon 只在 `0x59BBF` 取用 `+0x08` 槽位，
紧接着 `lea rcx,[rdi+674h]`，是成员地址。

**宿主进程在事件与实测之间重启，全局被重置。** 排除。`GaokunThpHost` pid 13692 启动于
13:30:37，早于 `13:32:33` 那条 `ERASER_TOGGLE1`。这条曾经是最有力的候选，因为日志文件名是
`Service_LogFile-<账户名>-<日期>.txt`，**不含 PID 且追加写**，同名进程重启在日志里看不出断
口。判据是进程启动时间，不是日志。

**多个读者抢 MCU 中断端点，`0x7F` 被别人吃掉。** 不是本次的成因——全局确实到了 1。但这个
隐患真实存在：`THP_Service.dll` 用的 MCU 接口 GUID 是
`dd0ebedb-f1d6-4cfa-acca-71e66d3178ca`（`.rdata 0x86520`），与 `PenService.dll` 和本项目的
`PenEventBridge` 是同一个，一个中断包只交付一个读者。排查其他 MCU 事件时要先想到它。

**Windows 对按键式橡皮要求经过 out-of-range 状态迁移，中途翻转的 Invert 被丢弃。** 排除。
实测把笔完全拿开、等到 `LEAVE` 之后再 `ENTER`，`penFlags` 不变。

**加载的是另一个版本的 `THP_Service.dll`。** 排除。机器上只有两份，
`C:\Program Files\Huawei\HuaweiThpService\` 与 `C:\Tools\re\`（分析用拷贝），SHA-256 相同
（`d856f644…f8c1`），就是本文反汇编的那一份。

**存在第二个加载 `THP_Service.dll` 的进程，写和读发生在两个地址空间。** 排除。实测只有一个
`GaokunThpHost`，`HuaweiThpService` 处于 STOPPED。

**`vhfSuppress` 置位导致带 Invert 的那一帧被丢弃。** 排除，见下节。

## 七、`vhfSuppress`：`0xC938C`

语义是「抑制全部 VHF 写入」：非 0 时手指报告和笔报告都在 `WriteFile` 之前返回。setter 在
`0x14490`（`mov byte ptr [1800C938Ch],cl; ret`），全 DLL 只有两个调用者，都在
`0x71 PEN_CONN_STATUS` 的处理函数里：

```
000000018000EFCD: call  1800020C0                ; 回调单例 0x1800B56D0
000000018000EFD5: call  1800020B0                ; 取 +0x58 槽 = GetPenEleValue
000000018000EFDA: xor   ecx,ecx
000000018000EFDC: call  rax                      ; GetPenEleValue(0) 读 Config/VHFFunction
000000018000EFDE: test  eax,eax
000000018000EFE0: jne   18000F0B2                ; 非 0 -> 整段跳过
000000018000EFE6: call  1800149F0                ; IOCTL 0x4001C04 查 DeviceNUM
000000018000EFEB: cmp   eax,1
000000018000EFEE: jne   18000F0B2
000000018000EFF4: movzx ecx,al
000000018000EFF7: call  180014490                ; SetVhfSuppress(1)
000000018000EFFC: call  180014800                ; 两次 IOCTL_TEST_CMD 0x4001C00
000000018000F001: call  1800020C0
000000018000F009: call  1800021C0                ; 取 +0x50 槽 = SetPenEleValue
000000018000F00E: mov   ecx,1
000000018000F013: call  rax                      ; SetPenEleValue(1) -> VHFFunction=1
                  ... 日志 "hidDeviceStatus <n>" ...
000000018000F0AB: xor   ecx,ecx
000000018000F0AD: call  180014490                ; SetVhfSuppress(0)
000000018000F0B2: mov   cl,1
000000018000F0B4: call  180013D50                ; SendEventAck(0x01)
```

这是一段一次性自检：首次运行（`VHFFunction == 0`）时短暂抑制、给注入器发两条测试 IOCTL、
把 `VHFFunction` 写成 1，此后每次连接直接跳过。

它不可能造成本文的现象，三条理由：与橡皮无关，只在 `0x71` 事件上被动过；对笔和手指一视同
仁，真被置位触摸也会一起死；检查发生在改完 `byte[1]` **之后**，被抑制的那一帧改动仍留在常
驻缓冲里，下一帧照样带出去。

顺带一提，这个字节是「OneNote 输入抑制无处落地」那条已知缺口唯一可用的落点，代价是笔与触摸
一起被抑制。

## 八、观测手法

用 `ReadProcessMemory` 读运行中宿主的三个全局即可，不必下断点也不必注入：

| 偏移 | 名称 | 判读 |
|---|---|---|
| `+0xB6A8C` | `eraserToggle` | 写报告时必须等于 1 才会置 Invert/Eraser |
| `+0xC938C` | `vhfSuppress` | 非 0 时笔和手指报告都不写 |
| `+0xC9380` | `vhfHandle` | VHF 注入器句柄，0 表示没打开 |

做法：`Get-Process GaokunThpHost` 取到进程，从 `.Modules` 里找 `THP_Service.dll` 拿基址，
`OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION)` 后按上表偏移读。同时打印进程的
`StartTime`——排除「宿主重启」那条假设靠的就是它，日志里没有 PID，看不出重启。

需要管理员权限，目标是 LocalSystem 进程。

本机记录到的两组值：

```
ERASER_TOGGLE1 之后                    eraserToggle = 1   vhfHandle = 0x294
发 CommandSendPenCurrentFunc(0) 之后
ERASER_TOGGLE0                         eraserToggle = 0   vhfHandle = 0x294
```

## 九、结论与未解之处

已经确定的：

- 橡皮状态能进入 HID 报告，路径完整、位序正确，没有被写死。
- 写报告的地方只有 `0x14620` 一处，调用它的地方只有 `ApDaemon!0x59BBF` 一处，没有旁路。
- 那两位的唯一来源是 `[0xB6A8C]`，算法链既不写也不读它。
- `[0xB6A8C]` 的唯一来源是 MCU 的 `0x7F ERASER_TOGGLE` 事件，payload 原值直入。
- `CommandSendPenCurrentFunc(1)` 确实能让 MCU 发出该事件，全局确实被置成 1。

未解的：**全局为 1、句柄有效（`0x294`）、进程未重启、地址无误，而 `0x14620` 的橡皮分支就是
不执行**——`penFlags` 全程 `NONE`，且由于报告缓冲常驻，哪怕执行过一帧也会留下痕迹。第六节
列出的每一条解释都已经被判据排除，本文不为它编一个新的。

静态分析到此为止，二进制里没有更多信息。下一步唯一可行的手段是**在 `THP_Service.dll+0x14620`
下断点**，观察三件事：该函数是否真的被调用、`0x180010460` 的返回值、以及 `[rbx+1]` 改完之后
的实际值。

这与第八节的只读观测不是一个量级：断点要对一个正在驱动触控的 LocalSystem 进程动手，进程一
旦被挂起，触控和笔输入立即中断，恢复不当会让设备停在中间状态。投入之前先权衡收益。

## 附：地址表

`THP_Service.dll`，ImageBase `0x180000000`：

| 位置 | RVA | 含义 |
|---|---|---|
| `ERASER_TOGGLE` 处理函数 | `0x0F7BA` | 事件 `0x7F`，ACK `0x09` |
| `PEN_CURRENT_FUNC` 处理函数 | `0x0F89E` | 事件 `0x2F`，写 `0xB6A90`，与报告无关 |
| 橡皮状态全局 | `0xB6A8C` | 唯一写入 `0x0F822`，唯一读取 `0x10460` |
| `GetEraserToggle` | `0x10460` | 两条指令 |
| 笔报告写入 | `0x14620` | 13 字节，施加 Invert/Eraser |
| 手指报告写入 | `0x144E0` | 32 字节 |
| VHF 打开 / 关闭 | `0x14750` / `0x147A0` | |
| API 表注册 | `0x144A0` | 表基址 `[0xB2000]` |
| VHF 注入器句柄 | `0xC9380` | |
| VHF 抑制标志 / setter | `0xC938C` / `0x14490` | |
| 事件跳转表 | `0xFC54` 索引 / `0xFBF0` 目标 | 事件码 `0x03`–`0x7F`，24 个非默认分支 |
| 包过滤期望值 | `0xB6A28` = `0x12010207` | `packet[2]==0x07`、`packet[4]==0x01` |
| 回调单例 | `0xB56D0` | `+0x40` PrintEventLog、`+0x48` EventLogStatus、`+0x50` SetPenEleValue、`+0x58` GetPenEleValue |
| MCU 接口 GUID | `0x86520` | `dd0ebedb-f1d6-4cfa-acca-71e66d3178ca` |
| VHF 接口 GUID | `0x88208` | `{59819B74-F102-469A-9009-3CAF35FD4686}` |

`ApDaemon.dll`：

| 位置 | RVA | 含义 |
|---|---|---|
| 笔报告组装与写出 | `0x59BB8` | 全 DLL 唯一取用 `+0x08` 的地方 |
| 笔报告缓冲 | `obj+0x674` | 13 字节，`+0x675` 是按钮字节 |
| API 表指针成员 | `obj+0x6F8` | 全局副本在 `0x164058` |
| 报告缓冲初始化 | `0x4CBDD` | Report ID = 8、按钮字节 = `0x20` |
