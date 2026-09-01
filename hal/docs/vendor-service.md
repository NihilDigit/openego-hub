# 原厂 HuaweiThpService 逆向记录

本文记录 `C:\Program Files\Huawei\HuaweiThpService\HuaweiThpService.exe` 的完整行为。
结论来自对本机已安装文件的 IL 静态分析；仓库仅保留行为规格与分析结论，不保存完整反汇编产物。

## 原厂服务是托管程序

该 exe 不是原生程序，而是 .NET Framework 程序集：PE 只有 `.text` 与 `.rsrc` 两个节，
没有导入表，CLI 头存在（runtime 2.5，ILONLY）。同目录的 `InstallUtil.exe` 是 .NET
服务安装工具，与此对应。

编译目标是 x64（machine 0x8664），不是 AnyCPU。因此在 ARM64 机器上，CLR 以 x64 进程
承载它，整个服务连同其加载的全部 DLL 都跑在 x64 模拟层内。

这决定了移植路径：托管代码无法编译为 ARM64EC，因此 ARM64EC 版本只能以原生 C++ 重写
这层薄壳，而不是修改原程序集。

## 依赖架构

安装目录内被实际加载的模块全部是 x64：`THP_Service.dll`、`TSACore.dll`、`TSAPrmt.dll`、
`SpiModule.dll`、`himax_thp_drv.dll`、`ApDaemon.dll`。目录里另有一批 x86 DLL
（`msvcrt.dll`、`kernel32.dll`、`api-ms-win-crt-*` 等），x64 进程无法加载它们，属打包冗余。

ARM64EC 而非纯 ARM64 是这里唯一可行的选择：纯 ARM64 进程不能加载 x64 DLL，而上述整条
算法链暂时保持 x64 不变。ARM64EC 允许同一进程内混合两种代码，后续可逐个把 DLL 替换为
原生实现，不必一次性重写全部。

## THP_Service.dll 接口

导出表恰好七项，服务全部用到，没有其他接口：

| 序号 | 名称 | 签名 |
| --- | --- | --- |
| 1 | `GetMESSAGE` | `void GetMESSAGE(char* buf, int* bufLen)` |
| 2 | `RegisterEventLogStatus` | `void (CallBackFunc)` |
| 3 | `RegisterGetPenEleValue` | `void (CallBackFunc)` |
| 4 | `RegisterPrintEventLog` | `void (CallBackFunc)` |
| 5 | `RegisterSetPenEleValue` | `void (CallBackFunc)` |
| 6 | `ThpFuncStart` | `int ThpFuncStart(void)` |
| 7 | `ThpFuncStop` | `int ThpFuncStop(void)` |

回调类型为 `int (*CallBackFunc)(int arg)`。

全部函数的调用约定是 cdecl：P/Invoke 声明写作 `pinvokeimpl("THP_Service.dll" cdecl)`，
委托上的 `UnmanagedFunctionPointerAttribute` 实参为 2，即 `CallingConvention.Cdecl`。
x64 下 cdecl 与 stdcall 是同一套 ABI，此处无实际差别，但重写时仍应显式标注，避免日后
移植到别的架构时出错。

`GetMESSAGE` 的 P/Invoke 带 `bestfit:off charmaperror:on`，参数按 `lpstr` 封送，即窄字符。
缓冲区长度常量 `MESSAGE_LENGTH` 为 0x40（64 字节），调用方按值传入长度并按引用接收。

## 服务生命周期

程序入口 `Program.Main` 只做一件事：`ServiceBase.Run(new HuaweiThpService())`。

`ServiceName` 在 `InitializeComponent` 中设为 `HUAWEIThpService`，注意与 SCM 中注册的
服务名 `HuaweiThpService` 大小写不一致。Windows 服务名不区分大小写，故不影响运行。

构造函数依次完成：

1. 建立 `EventLog("System")` 实例（成员 `log`，此后再未使用）。
2. 若事件源 `THPEvent` 不存在则以日志名 `THPLog` 创建，并将静态 `eventLog` 的
   Source 设为 `THPEvent`、Log 设为 `THPLog`。异常路径上的诊断信息写到这里。
3. `CanHandlePowerEvent = true`，即向 SCM 声明接受 `SERVICE_ACCEPT_POWEREVENT`。
4. `Process.GetCurrentProcess().PriorityClass = 0x100`，即 `REALTIME_PRIORITY_CLASS`。

`OnStart`：先 `FunctionRegister()`，再 `ThpFuncStart()`，返回值丢弃。

`OnStop`：`ThpFuncStop()`，返回值丢弃。

`OnCustomCommand(command)`：`command == 15`（`SERVICE_CONTROL_PRESHUTDOWN`）时直接返回，
其余转交基类。服务并未声明 `CanShutdown`，因此这条分支在当前配置下不会被触发。

`FunctionRegister` 注册四个回调，注册顺序为 PrintEventLog、EventLogStatus、
SetPenEleValue、GetPenEleValue。前两个是静态方法，后两个绑定到 `XmlOperator` 单例的
实例方法。四个委托都存入静态字段，这是必要的：否则 GC 会回收委托，DLL 持有的函数指针
将悬空。

## 四个回调的实际行为

`PrintEventLog(int)`：以 64 字节缓冲调用 `GetMESSAGE`，随后不使用取回的内容，返回 0。
消息被取走但未落盘，原厂在此处没有实现日志输出。

`EventLogStatus(int)`：返回静态字段 `eventStatus`。该字段从未被赋值，恒为 0。

`SetPenEleValue(int)` 与 `GetPenEleValue(int)`：转到 `XmlOperator`，见下节。

## XmlOperator

单例，双检锁，全部读写在同一个 `padlock` 上串行。配置路径为硬编码绝对路径
`C:\Program Files\Huawei\HuaweiThpService\HuaweiTHP.config.xml`。构造时若文件不存在则
创建，模板为：

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Config>
  <VHFFunction>1</VHFFunction>
  <LogFunction>0</LogFunction>
</Config>
```

`GetPenEleValue(a)` 按参数选键：`a == 0` 读 `Config/VHFFunction`，`a == 1` 读
`Config/LogFunction`，其余参数不匹配任何分支。取回的文本经 `Int32.Parse` 返回。
文件缺失、XML 非法、节点缺失、内容为空串、解析抛异常，一律返回 0；XML 非法时还会
删除该文件，留待下次调用重建。

`SetPenEleValue(penFunc)` 不读参数选键，只写 `Config/VHFFunction`，写入的正是参数本身的
十进制文本。没有写 `LogFunction` 的路径。写入前把文件属性重置为 `FileAttributes.Normal`
（0x80），以绕开只读标记。成功返回 0，任何失败分支返回 -1。

两个方法的异常处理都是 `catch (object)`，向 `THPEvent` 写一条固定文本后按上述约定返回。

## 与社区 ARM64EC 移植版的差异

指 `github.com/LanZhan-Harmony/HuaweiThpService`。该版本的整体骨架与原厂一致，接口签名、
七个导出、64 字节缓冲、实时优先级都对得上。已核出的偏差：

- 未设置 `CanHandlePowerEvent`。其 `SetServiceStatus` 只报告
  `SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN`，原厂还接受 `SERVICE_ACCEPT_POWEREVENT`。
  睡眠与唤醒时 SCM 不会通知该服务。
- 未创建 `THPEvent` / `THPLog` 事件源，异常诊断无处可去。
- `PrintEventLog` 之外的日志路径整体缺失，与原厂现状相同，不构成回归。

这些差异是否足以解释该版本不可用，尚未验证。

## 模块分层

静态导入关系如下，`TSACore.dll` 不在任何一方的导入表中，由 `ThpFuncStart` 之后动态加载：

```
HuaweiThpService.exe  (.NET 薄壳)
  └─ THP_Service.dll          服务逻辑、状态机、电源事件
       ├─ ApDaemon.dll        唯一接触 HID 的模块
       │    导入 HID.DLL 仅一项 HidD_GetHidGuid，配合 SetupDi* 枚举设备
       │    导出 FunInitList(_API_FUNC_S*), ThpMain, ThpNotify, ThpStart, ThpStop
       └─ SpiModule.dll       SPI 硬件访问
            THP_Service 只用其中三项：GetSpbDeviceNum, SpiReadAcpi, SpbModuleInit
```

`THP_Service.dll` 另外导入 `WS2_32.dll`（11 项，按序号导入）与
`USER32.dll` 的 `RegisterPowerSettingNotification` / `UnregisterPowerSettingNotification`。
后者对应 .NET 侧的 `CanHandlePowerEvent`，说明电源通知在 DLL 内部另有一套自己的注册，
与服务声明的 `SERVICE_ACCEPT_POWEREVENT` 是两条独立的路径。

没有任何一个模块导入 `vhfum.dll`，但这不表示原厂不用 VHF。设备实际运行的日志里有
`EnumDeviceVHF` 与 `FindMatchingDevice [HID]Find HUAWEI MCU HID DEVICE FOR PRESSURE`，
系统中也确实存在这三个设备：

```
THP SPI Device Driver  (ThpDevice)   SPI 通道，另有一个同名实例
THP VHF Device         (ThpDevice)   ApDaemon 的写入目标
Virtual HID Device     (HIDClass)    VHF 产出的标准 HID 设备
```

也就是说虚拟 HID 设备由内核侧的 Himax 驱动经 VHF 创建，`ApDaemon.dll` 只用 `SetupDi*`
枚举出它的设备接口再写报告，不需要链接 `vhfum.dll`。仅凭导入表判断会得出相反的结论，
这一条是在真机日志里纠正的。

## _API_FUNC_S 回调表

`FunInitList` 的实现只有两件事：把入参指针存进全局变量 `ApDaemon+0x164058`，然后写一条日志。

```asm
mov  qword ptr [0x180164058], rcx
test rcx, rcx
je   skip
lea  rcx, [0x18011BA38]     ; 日志文本
call 0x180002680
skip:
lea  rcx, [0x18011BA68]
jmp  0x180002680
```

该全局变量在 ApDaemon 内共有 11 处引用。其中 5 处是间接调用，全部取同一个槽位：

```asm
mov  rax, qword ptr [0x180164058]
mov  rax, qword ptr [rax+98h]
lea  rcx, [rsp+38h]          ; 参数是栈上的一个结构
call rax
```

余下 6 处只是把表指针存入内部对象的成员（偏移 0x6F8、0x5D0、0x2020、0xF0 等）备用。

结论：表的方向是 THP_Service 向 ApDaemon 提供回调，而非相反；结构至少 0x160 字节。

**「ApDaemon 只使用偏移 0x98 一个槽位」是错的，此处更正。** 那个说法只数了对
`[0x180164058]` 的直接解引用，漏掉了先把表指针存进对象成员、再从成员间接调用的那一批。跟踪
成员 0x6F8 / 0x5D0 / 0x2020 之后，实际取用的槽位至少有十个：

| 槽位 | 取用处 | 已知含义 |
|---|---|---|
| `+0x00` | `0x5AE60`、`0x5AE80` ×2 | 写手指报告，32 字节 |
| `+0x08` | `0x59BBF` | 写笔报告，13 字节 |
| `+0x78` | `0x5F418` | |
| `+0x98` | `0x288D`、`0x41D8`、`0x427A`、`0x42AD` ×2、`0x7C58E` | 事件与日志上报 |
| `+0xA0` | `0x59C19`、`0x5A35D` | |
| `+0xA8` | `0x5B688` | |
| `+0xB0` | `0xBED69` | |
| `+0xB8` | `0xF2C94` | |
| `+0xC0` | `0x5F45C` | |
| `+0xC8` | `0x5F434` | |
| `+0x148` | `0xDD14D` | |
| `+0x158` | `0xDD136` | |

前四项在 `THP_Service.dll` 里由 `0x144A0` 一次性填好：`+0x00` 写手指报告、`+0x08` 写笔报告、
`+0x10` 打开 VHF 注入器、`+0x18` 关闭。**触摸与笔的报告正是走这张表回到 THP_Service 写出去
的**，详见 `hal/docs/thp-eraser.md`。

五个调用点中有三个位于 `ThpStart`（0x1800041CC）与 `ThpStop`（0x180004273、0x1800042A6）
内部，与 .NET 侧 `PrintEventLog` 取回消息后即丢弃的设计相符，因此该槽位承担的是事件与
日志上报，不是触摸数据通路。

## 触摸数据的去向

真机日志给出了这条链的形状：

```
SpiModule  →  ApDaemon(ThpMain)  →  TSACore/afehal  →  THP VHF Device  →  Virtual HID Device
```

`ApDaemon.dll` 里的 `EnumDeviceVHF` 负责找到 `THP VHF Device` 的设备接口，触摸与笔的报告
写入该接口，再由 VHF 呈现为标准 HID。这意味着后续要在输出上做改造（例如把双击映射成
HID 的 Invert/Eraser 位），落点在 ApDaemon 与 VHF 设备之间，而不必触碰算法链。

写入用的具体函数与报告布局都已测绘：笔报告 `0x14620`（13 字节）、手指报告 `0x144E0`
（32 字节），布局与 `HidInjectorThp.sys` 的描述符逐位对上。橡皮位本来就在笔报告里，
厂商自己也会置它——上面设想的那种改造因此不必做。真正卡住的是它为什么不生效，
见 `thp-eraser.md`。

## ARM64EC 下的实测结果

`scripts/smoke.ps1` 在本机让出原厂服务后运行 ARM64EC 构建，整条链路正常初始化：

```
ThpMain enter
afename = W273AS2700
OpenLib afehalW273AS2700.dll -> himax_thp_drv.dll
OpenLib TSACore.dll
InitTsaPrmt Project id = W273AS1310 -> tsa init Success
UpdateFrameDim col = 60, row = 40
PropVersion 3.9.46.0943
[HID]Find HUAWEI MCU HID DEVICE FOR PRESSURE
BluetoothPen::TryToConnect connected
TsaFrame::ObInput touch:0, down:0
ChangeToIdlemode, idle in 300
```

进程优先级为 24（`REALTIME_PRIORITY_CLASS`），与原厂一致。

注意 AFE 名与 TSA 工程号并不相同：AFE HAL 是 `W273AS2700`，而 TSACore 初始化用的工程号是
`W273AS1310`。两者由原厂链路自行决定，不是配置项。
