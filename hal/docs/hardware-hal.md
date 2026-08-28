# HardwareHal.dll 逆向记录

本文记录 `C:\Program Files\Huawei\BasicService\HardwareHal.dll`（553904 字节，448 个导出，
文件日期 2026-02-11）的能力边界与调用方式。

结论分三档，正文中逐项标注：

- **实测**：在本机 x64 测试程序中真实调用过，附实测值。
- **静态**：从反汇编或导入表读出，未实际调用。
- **未验证**：既未调用也无法从静态分析确定。

实测环境为 HUAWEI GK-W7X（Gaokun，Snapdragon 8cx Gen 3，Windows 11 ARM64），
测试进程为 x64 非提升进程。**提升权限下的行为未验证**，这一点对散热组是决定性的，
见「BiosWmi 通道需要管理员权限」一节。

## 调用模型：普通同步导出，不是消息循环

这是与 `THP_Service.dll`、`PenService.dll`、`KeyboardService.dll` 最重要的差别。
HardwareHal 没有阻塞式消息循环，没有回调注册，不需要独立线程，也没有「设备未打开就访问违例」
的行为。每个导出都是一次同步调用，当场返回。

依据：

- 导入表里没有任何线程或同步原语的重型依赖，只有 `WmiUtil.dll`、`HwFileUtil.dll`、
  `SETUPAPI`、`POWRPROF`、`wlanapi`、`ole32`/`OLEAUT32`（WMI 走 COM）。
- 存在 `HalSetCallback` 导出，但它只服务于一个模块（模块号 3，显示器热插拔），
  且是可选的；电池、散热、系统信息三组完全不经过它。
- 实测：进程内 `LoadLibraryEx` 之后直接调用任意 getter 即可返回，无需任何 `Init`。

失败时返回错误码，不抛异常，不访问违例。**唯一一处实测到的访问违例是我们自己的用法错误**，
见「踩过的坑」第 1 条。

### 初始化

无需显式初始化，但有两个前提：

1. **COM 必须先初始化**。大量导出经 WMI 取值，DLL 自身不调用 `CoInitializeEx`。
   宿主进程需要在调用前执行 `CoInitializeEx(nullptr, COINIT_MULTITHREADED)` 与
   `CoInitializeSecurity(...)`。未初始化时这些导出静默返回空字符串或 0，不报错。
   实测：补上 COM 初始化后，`SystemInfo`、`Baseboard`、`SmBiosHelper`、`Cpu` 全组从
   全空变为全部有值。
2. **对象缓冲区必须清零**。见下一节。

各类的 `Init()` 导出（`Battery::Init`、`Cpu::Init`、`Memory::Init`、`VideoCard::Init`）
是幂等的惰性初始化，且已被内联进每个 getter 的入口，因此可以不调用。

### C++ 修饰名的绑定方式

448 个导出中绝大多数是 C++ 修饰名。要点：

- **没有虚函数**。导出表里没有任何 `??_7`（vftable）符号，还原出的签名里也没有 `virtual`。
  因此不需要按虚表布局绑定，直接 `GetProcAddress` 拿函数地址调用即可。
- **x64 只有一种调用约定**。`undname` 把成员函数还原成 `__cdecl` 是显示惯例，实际是
  Microsoft x64 ABI：`this` 在 RCX，后续参数依次为 RDX、R8、R9，其余压栈。
  `__thiscall`/`__stdcall`/`__cdecl` 的区分在 x64 上不存在。
- **返回 `std::string` / `std::wstring` 时参数顺序是 `this` 在前**。
  MSVC x64 的成员函数隐藏返回指针排在 `this` 之后，即 `RCX = this`、`RDX = 返回缓冲区`、
  `R8 = 第一个真实参数`。这与 Itanium ABI 相反，写错会得到全空字符串而不是崩溃。
  依据（`Battery::GetBatteryName`，RVA 0x158F0）：

  ```
  mov  r8d, 2        ; 转发给 GetProperty 的枚举值
  mov  rbx, rdx      ; 保存 rdx
  call GetProperty
  mov  rax, rbx      ; 返回 rdx —— 按 ABI，返回值必须是隐藏返回指针
  ```

  绑定原型写成 `void* (__fastcall*)(void* self, void* ret, ...)`。

- **返回的 `std::wstring` 是 MSVC 布局**：`union { wchar_t buf[8]; wchar_t* ptr; }` +
  `size_t size` + `size_t capacity`，`capacity >= 8` 时取指针，否则取内联缓冲区。
  `std::string` 同理，阈值是 16。DLL 链接的是 `MSVCP140.dll`，宿主用同一套运行库时可以
  直接用 `std::wstring` 接收；否则按上述布局手工解析并放弃释放（会泄漏，用于短命进程无妨）。

### 对象生命周期

类都是纯数据的小结构，没有虚表。构造函数只做清零：

| 类 | 构造函数 | 实例大小 | 析构函数 |
| --- | --- | --- | --- |
| `HotInterface` | RVA 0x3AC0，`mov rax,rcx; ret`（空） | 0（无状态） | RVA 0x3AD0，空 |
| `Battery` | RVA 0x15710，`mov byte[rcx],0` | 1 字节 | 空 |
| `Cpu` | 同 0x15710 | 1 字节 | 空 |
| `Baseboard` / `BiosFun` / `SystemInfo` / `AudioCard` / `Biometric` | RVA 0x3AC0（空） | 无状态 | 空 |
| `Memory` | RVA 0x273C0，`mov qword[rcx],0` | 8 字节 | RVA 0x28490，会释放 |
| `Disk` | RVA 0x19BE0，清零 0x28 字节 | 0x28 字节 | RVA 0x19C00，会释放 |
| `SmBiosHelper` | 单例，`SmBiosHelper::GetInstance()` 返回 DLL 内静态对象 | — | 不要析构 |

实践上：为每个类分配一块**独立的、清零的**缓冲区（64 字节足够覆盖上表全部），
调用构造函数，用完对 `Memory` 与 `Disk` 调析构函数。`HotInterface` 的方法完全忽略 `this`
（反汇编中 RCX 在入口即被覆盖），传任意非空指针都可以。

**不要多个类共用同一块缓冲区**——这会导致访问违例，见「踩过的坑」第 1 条。

## 电池

`Battery` 类的每个 getter 都是对内部 C 导出 `BtQueryInformation` 的薄封装：

```c
// extern "C"，无修饰名，可直接绑定
int BtQueryInformation(int infoId, void* outBuf, unsigned int outSize, unsigned long* bytesWritten);
```

返回 1 表示成功，0 表示失败。`infoId` 与语义的对应关系（从 RVA 0x16C30 的跳转表还原）：

| id | 对应的 C++ 导出 | 数据来源 | 大小 | 实测值 |
| --- | --- | --- | --- | --- |
| 0 | `Battery::CheckBattery` | `GetSystemPowerStatus`，`BatteryFlag != 0x80 && != 0xFF` | 4 | 1 |
| 1 | `Battery::GetBatteryManufacturer` | WMI `SELECT ManufactureName FROM BatteryStaticData` | 字符串 | `""`（失败） |
| 2 | `Battery::GetBatteryName` | WMI `SELECT DeviceName FROM BatteryStaticData` | 字符串 | `""`（失败） |
| 3 | `Battery::GetBatteryRemainCapacity` | `CallNtPowerInformation(SystemBatteryState)` 的 `RemainingCapacity` | 4 | 36886 |
| 4 | `Battery::GetBatteryFullCapacity` | 同上的 `MaxCapacity` | 4 | 36886 |
| 5 | `Battery::GetBatteryDesignCapacity` | RVA 0x16440，取设计容量 | 4 | 45076 |
| 6 | `Battery::AcLine` | `GetSystemPowerStatus.ACLineStatus` | 4（只取低字节） | 1 |
| 7 | `Battery::GetBatteryVoltage` | RVA 0x16600，WMI `SELECT Voltage FROM BatteryStatus` | 4 | 8516 |
| 8 | `Battery::GetBattertLoss` | `(design - full) * 10000 / design`，再除以 100 得浮点 | 4 | 18.16 |
| 9 | `Battery::GetDisChargeRate` | `SystemBatteryState.Rate`，要求 `AcOnLine == 0` 且 `Rate < 0` | 4 | 失败（当时接着电源） |
| 10 | `Battery::GetChargeRate` | 同上，要求 `AcOnLine == 1` 且 `Rate > 0` | 4 | 失败（当时已充满） |

单位：容量为 mWh，电压为 mV，充放电功率为 mW。`GetBattertLoss` 返回百分比，
实测 18.16 与 `(45076 - 36886) / 45076 = 18.17%` 吻合。

另有 `Battery::GetBatteryPercent`（RVA 0x15C00），直接读 `GetSystemPowerStatus` 的
`BatteryLifePercent`，最多重试 3 次；实测 100。

`HalQueryInformation(6, 任意, id, buf, size, &written)` 等价于 `BtQueryInformation(id, ...)`，
但多一层转发，没有理由用它。

### 可行性判断（电池）

- **可以直接用**：容量、电压、健康度（`GetBattertLoss`）、交流供电状态、电量百分比。
  这些都不需要管理员权限，实测在普通权限下全部有值。
- **要先满足条件**：充放电功率（id 9/10）。逻辑本身正确，只是需要在放电/充电中才返回值。
  实现时应当同时读 `AcLine` 判断当前处于哪一侧，否则拿到 0 会误判为「无功耗」。
- **不可行**：`GetBatteryName` / `GetBatteryManufacturer`。它们查 `root\wmi` 的
  `BatteryStaticData`，本机该类存在但取实例报「常规故障」（用 PowerShell
  `Get-CimInstance -Namespace root/wmi -ClassName BatteryStaticData` 复现），
  与权限无关，是固件没有实现这张表。要电池型号只能另找来源。
- **不可行**：循环次数、健康度百分比之外的健康信息、电池温度。HardwareHal 没有对应导出。
  但这些**不必依赖 HardwareHal**，见下节。

### 循环次数与满充容量：绕开 HardwareHal

`root\wmi` 下的标准电池类在本机可用，且普通权限即可读，比 HardwareHal 更直接：

| 类 | 属性 | 实测值 |
| --- | --- | --- |
| `BatteryCycleCount` | `CycleCount` | 233 |
| `BatteryFullChargedCapacity` | `FullChargedCapacity` | 36886 |
| `BatteryStatus` | `RemainingCapacity` / `Voltage` / `ChargeRate` / `DischargeRate` / `Charging` / `Discharging` / `PowerOnline` | 36886 / 8516 / 0 / 0 / False / False / True |
| `BatteryRuntime` | `EstimatedRuntime` | 0xFFFFFFFF（接电源时无意义） |
| `BatteryTemperature` | — | 类存在但无实例 |
| `BatteryStaticData` | — | 取实例报错 |

实例名统一为 `ACPI\PNP0C0A\1_0`。**循环次数只能从这里拿**，HardwareHal 没有这个能力。

### 充电阈值：`Battery::GetChargeThreshold` 是坏的

这是本次调研最值得记的一条结论。

```
bool Battery::GetChargeThreshold(StBatteryChangeThresholdLimit& out);
bool Battery::SetChargeThreshold(const StBatteryChangeThresholdLimit& in);
```

`StBatteryChangeThresholdLimit` 是 2 字节，两个 `uint8`（由 `SetChargeThreshold` 读
`word ptr [rdx]` 再拆成 `cl` 与 `dl` 两个参数确定）。

`GetChargeThreshold` 的实现（RVA 0x15B80）是：

```
mov  ecx, 9            ; infoId = 9
lea  r9,  [rsp+30h]    ; &bytesWritten
lea  r8d, [rcx-7]      ; size = 2
call BtQueryInformation
cmp  eax, 1
sete al
```

id 9 是**放电功率**分支，而该分支入口第一件事就是 `cmp esi,4; jb fail`——要求缓冲区至少
4 字节。调用方传的是 2。因此这个函数**在任何情况下都返回 false，且一个字节都不写**。
看起来是厂商把 `GetDisChargeRate` 的 id 复制过来忘了改。

实测（非提升进程）：`ret=0`，2 字节输出缓冲区保持调用前填入的 `AB AB` 不变。
同一路径下 `BtQueryInformation(9, &v, 4, &w)` 也返回 0（因为当时接着电源），
但那是数据条件不满足，与本条无关。

**结论：HardwareHal 读不回充电阈值。** 现有「只写不可读」的缺陷不能靠这个导出补上。

### 充电阈值的真正读回路径：`WmiUtil.dll` 的 `GetSmartChargeMode`

HardwareHal 自己不碰硬件，`SetChargeThreshold` 转手调用的是同目录 `WmiUtil.dll` 的
`HwSDK::BiosWmi::SetBatteryChargeThreshlod(unsigned char, unsigned char)`（拼写是厂商原样）。
同一个 `WmiUtil.dll` 里有配对的读接口：

```
static int HwSDK::BiosWmi::GetSmartChargeMode(HwSDK::SmartChargeMode& mode,
                                              HwSDK::ChargeCapacity& capacity);
```

修饰名 `?GetSmartChargeMode@BiosWmi@HwSDK@@SAHAEAW4SmartChargeMode@2@AEAUChargeCapacity@2@@Z`，
是**静态函数**，没有 `this`，绑定为 `int (__fastcall*)(int* mode, void* capacity)`。

从 RVA 0x5600 的反汇编读出的语义（**静态**，未在提升权限下实测）：

- 输入缓冲区 0x20 字节清零后，`word[0] = 0x1603`，即字节序列 `03 16`，无额外载荷。
- 输出缓冲区 0x100 字节。
- 解析：`out[1]` 为模式，必须 `< 7`，写入 `mode`；`out[3]` 写入 `capacity` 的第一个 int，
  `out[4]` 写入第二个 int。因此 `ChargeCapacity` 是 `{ int a; int b; }`，8 字节，
  语义上应当是下限与上限阈值。

与写入侧对照（`SetBatteryChargeThreshlod`，RVA 0x3110）：`word[0] = 0x1003`，
即 `03 10`，`in[2] = 第一个参数`，`in[3] = 第二个参数`。读写命令字只差一个字节
（0x10 写 / 0x16 读），落在同一条 `OemWMIMethod::OemWMIfun` 通道上。

也就是说，**读回阈值不需要 HardwareHal，用现有的 `OemWMIfun` 写通道发 `03 16` 即可**，
输出的第 1、3、4 字节就是模式与两个阈值。这条路径比引入 HardwareHal 干净得多。

**这一条是静态结论，必须实测确认字节位序与阈值的上下限对应关系再落地。**

## 散热

`HotInterface` 的每个方法都是三五条指令的转发，`this` 被立刻丢弃：

| 导出 | 转发到 `WmiUtil.dll` | 参数 |
| --- | --- | --- |
| `GetFan0Speed()` / `GetFan1Speed()` | `BiosWmi::GetFanSpeed(int&, FanSpeedLocateNumber)` | 索引 0 / 1 |
| `GetSensorTemperature(uint64 idx)` | `BiosWmi::GetSensorTemperature(int&, QuerySensorTemperatureIndex)` | 透传 idx |
| `GetVoltageUSB0/1`、`GetVoltageBattery0/1`、`GetCurrentBattery0/1` | `BiosWmi::GetVoltageOrCurrent(int&, VoltageCurrentLocateNumber)` | 0、1、0x20、0x21 等 |

`GetSensorTemperature` 在调用前把输出预置为 `0x7FFFFFFF`，`GetFanSpeed` 预置为 0；
底层失败时这些哨兵值会原样返回。**因此返回 0 转速与 0x7FFFFFFF 温度都要当作「读取失败」，
不能当作真实读数。**

### 实测结果（非提升进程）

全部失败：

```
GetFan0Speed = 0            GetFan1Speed = 0
GetSensorTemperature(0..19) = 2147483647（全部）
GetVoltageUSB0/1 = 0        GetVoltageBattery0/1 = 0
GetCurrentBattery0/1 = 0
```

直接调用 `WmiUtil.dll` 的 `BiosWmi::*` 也一样，且返回码统一为 **-11**：

```
GetFanSpeed(0..3)              ret=-11
GetSensorTemperature(0..23)    ret=-11
GetVoltageOrCurrent(...)       ret=-11
GetSmartChargeMode             ret=-11
GetDeviceBiosSwitchStatus(...) ret=-11
GetTurboMode / GetWorkingMode / GetCameraStatus / ...  ret=-11
```

### BiosWmi 通道需要管理员权限

统一的 -11 来自同一处：`WmiUtil.dll` 中所有 `BiosWmi::*` 都走
`ROOT\WMI` 命名空间的 `OemWMIMethod` 类，实例过滤条件是
`.InstanceName='ACPI\PNP0C14\HWMI_0'`，方法是 `OemWMIfun`（三个参数
`u8Input` / `u32Resrved` / `u8Output`）。字符串都在 `WmiUtil.dll` 里，可直接复核。

本机确实存在该设备（`Get-PnpDevice` 显示 `ACPI\PNP0C14\HWMI`，状态 OK），
类元数据也读得到（`Get-CimClass` 能列出 `OemWMIfun` 及其参数）。但在非提升进程中：

```
Get-CimInstance -Namespace root/wmi -ClassName OemWMIMethod
→ 拒绝访问
```

这与整组 -11 完全吻合。**因此散热组几乎可以确定是权限问题而非硬件不支持**，
但「提升权限后能读到值」这一点**本次没有验证**——测试进程无法自我提权，
交互式 UAC 在当前会话中不可用。落地前必须以管理员身份重跑一次。

另有一点需要单独确认：MateBook E Go 是无风扇机型。即使提权成功，
`GetFan0Speed` / `GetFan1Speed` 也很可能返回 0 或失败，这属于机型特性而非缺陷。
温度传感器与之无关，应当能读到。

### 性能模式与风扇策略

HardwareHal **没有**性能模式相关导出。但 `WmiUtil.dll` 有完整的一组（**静态**，全部未实测）：

| 接口 | 命令字（`word[0]`） | 说明 |
| --- | --- | --- |
| `BiosWmi::GetPerformanceMode(PerformanceModeValue&)` | 0x1204 | 输出 `out[2]`，取值须 `< 3`，即三档 |
| `BiosWmi::SetPerformanceMode(PerformanceModeValue)` | 0x1304 | `in[2] = 值` |
| `BiosWmi::GetTurboMode(bool&)` / `SetTurboMode(bool)` | — | — |
| `BiosWmi::GetWorkingMode(uint8&)` / `SetWorkingMode(uint8)` | — | — |
| `BiosWmi::SetTemperControlPolicy(uint8)` | — | 温控策略 |
| `BiosWmi::GetThermalStatisticInfo(uint8, vector<uint64>&, vector<int>&)` | — | 热统计 |
| `BiosWmi::GetSensorTemperature(int&, idx)` | 0x0202，`in[2] = idx` | 输出：`out[0] == 0` 且 `out[1] == 1` 时 `out[2]` 为温度 |
| `BiosWmi::GetFanSpeed(int&, idx)` | 0x0802，`in[2] = idx` | 输出：`out[1..2]` 为 u16 转速 |
| `BiosWmi::GetSmartChargeMode` / `SetSmartChargeMode` | 0x1603 / — | 见上文 |
| `BiosWmi::SetBatteryChargeThreshlod(uint8, uint8)` | 0x1003，`in[2]`、`in[3]` | 已知的写阈值路径 |

`GetPerformanceMode` 是唯一在非提升进程返回 1 而非 -11 的接口，但输出仍是未改动的
`0x7FFFFFFF`，因此不能认为它成功了；这条差异未进一步追查。

`WmiUtil.dll` 的完整 `BiosWmi` 导出还包括 `GetEnviromentLight`（环境光）、
`GetInPosition`（应为键盘吸附状态）、`GetPdFirmwareVersion`、`GetCellBatteryVoltage`、
`GetKeyboardLightTime` / `SetKeyboardLightTime`、`GetFnReverseStatus` / `SetFnReverseStatus`、
`BrightnessAdjust`、`GetCameraStatus`、`ResetTouchPanel` 等，共 48 项。
若后续要做键盘背光、Fn 反转、环境光这类面板，这里是统一入口。

### 可行性判断（散热）

- **要先解决权限**：风扇转速、传感器温度、电压电流。宿主必须以管理员身份运行，
  且必须先实测确认提权后确实有值。这是所有散热能力的前置条件。
- **不建议经 HardwareHal**：`HotInterface` 只是一层丢弃 `this` 的转发，直接绑定
  `WmiUtil.dll` 的 `BiosWmi::*` 静态函数更直接，还能拿到 HardwareHal 没有暴露的
  性能模式、温控策略、热统计。
- **哨兵值必须处理**：0 转速与 `0x7FFFFFFF` 温度是失败标志，不是读数。

## 系统信息

这一组全部在非提升进程中实测通过，是 HardwareHal 最可用的部分。只需要 COM 初始化。

### `SmBiosHelper`（单例，SMBIOS 直读）

`SmBiosHelper::GetInstance()` 是静态函数，返回 DLL 内的静态对象指针，不要析构。
所有 getter 是普通成员函数，返回 `std::wstring`。

| 导出 | 实测值 |
| --- | --- |
| `GetSysProductName` | `GK-W7X` |
| `GetSysManufactor` | `HUAWEI` |
| `GetSysSerialNumber` | `NCSBB24104800175` |
| `GetSysSKU` | `C233` |
| `GetSysVersion` | `M1010` |
| `GetSysFamily` | `MateBook E` |
| `GetBIOSVersion` / `GetBIOSSysVersion` / `GetBIOSECVersion` | `2.18` |
| `GetBIOSVendor` | `HUAWEI` |
| `GetBIOSReleaseDate` | `06/30/2023` |
| `GetBoardProductName` | `GK-W7X-PCB` |
| `GetBoardSerialNumber` | `BBMC4Z231J000504` |
| `GetBoardVersion` | `M1010` |
| `GetOemProductName` | `GaoKun` |
| `GetOemString1` | `$HUA001CN11000010` |
| `GetOemString2` / `GetOemString3` | `N/A` |
| `GetOemString4` | `GKQ83` |
| `GetSecSKU` | `Gaokun-W6651T` |
| `GetSecSerialNumber` / `GetSecManufactor` / `GetSecVersion` | 同 Sys 组 |
| `GetPropagandaName` | `""`（空） |
| `IsHuaweiDevice` | 1 |
| `IsMagicBookDevice` / `IsSmartDevice` / `IsConsumerTypeToB` | 0 |
| `GetBiosDeviceValue` | 0x0031 |

`IsTypeDetachablePC()`（无修饰名的 C 导出）实测返回 1，可用于判断二合一形态。

### `SystemInfo`（返回 `std::string`，不是 wstring）

| 导出 | 实测值 |
| --- | --- |
| `GetDeviceSerialNumber` | `NCSBB24104800175` |
| `GetDeviceTypeName` | `Gaokun-W7821T 3.231.0.6(C233)` |
| `GetSystemVersion` | `Windows 10 Pro`（在 Windows 11 上仍报 10，见「踩过的坑」第 4 条） |
| `GetBIOSVersion` / `GetDeviceType` / `GetDeviceBaseBoardType` / `GetSystemLanguage` / `GetDeviceHardwareIds` | `""`（空） |

同名能力优先用 `SmBiosHelper`，`SystemInfo` 这一组空值过多。

### `Baseboard`（返回 `std::wstring`）

| 导出 | 实测值 |
| --- | --- |
| `GetBIOSVersion` | `2.18` |
| `GetBIOSManufacturer` | `HUAWEI` |
| `GetBIOSReaseDate`（厂商拼写） | `06/30/2023` |
| `GetBIOSSerialNumber` | `NCSBB24104800175` |
| `GetBaseboardManufacturer` | `HUAWEI` |
| `GetBaseboardProduct` | `GK-W7X-PCB` |
| `GetBaseboardSerialNumber` | `BBMC4Z231J000504` |
| `GetBaseboardVersion` | `M1010` |

与 `SmBiosHelper` 完全重合，任选其一。

### `Cpu`

| 导出 | 实测值 |
| --- | --- |
| `GetCpuName` | `Snapdragon (TM) 8cx Gen 3 @ 2.69 GHz` |
| `GetCpuManufacturer` | `Qualcomm Technologies Inc` |
| `GetCpuCoreNumbers` / `GetCpuCoreThreads` | 8 / 8 |
| `GetCpuCurClock` / `GetCpuMaxClock` | 2688 / 2688（MHz） |
| `GetL1CacheSize` / `GetL2CacheSize` / `GetL3CacheSize` | 768 / 1280 / 2048（KB） |
| `GetCpuLoadPercent` | 9~15（随负载变化） |
| `GetCputype` | 3（`EnmHalCpuType` 枚举，取值含义未验证） |

`GetCpuCurClock` 与 `GetCpuMaxClock` 相等，说明它读的是 `Win32_Processor.CurrentClockSpeed`
的标称值而非实时频率，不能用来做频率曲线。

### `Memory`

需要独立实例（8 字节，清零）。

| 导出 | 实测值 |
| --- | --- |
| `GetMemoryNums(uint&)` | ret=1，n=1（一条内存） |
| `GetTotalMemoryNums(uint64&)` | ret=1，total=16384（MB） |
| `GetMemoryLoad()` | 39（百分比） |
| `GetCapacitybyIndex(uint64&, 0)` | ret=1，16384（MB） |
| `GetSpeedbyIndex(uint&, 0)` | ret=1，2133（MT/s） |
| `GetLocatorbyIndex(wstring&, 0)` | ret=1，`Bank 0` |
| `GetManufacturerbyIndex(wstring&, 0)` | ret=1，`Null` |
| `GetPartNumberbyIndex(wstring&, 0)` | ret=1，`Null` |
| 索引 1 及以上 | ret=0，输出不变 |

厂商与型号为字面量 `Null`，是板载 LPDDR 在 SMBIOS 里没有填写，不是读取失败。
「关于」面板里应当把 `Null` 当作缺失处理。

### `Disk`

需要独立实例（0x28 字节，清零），用完调析构。

| 导出 | 实测值 |
| --- | --- |
| `GetPhysicDiskNumbers()` | 1 |
| `GetCaption(0)` | `PCIe-8 SSD 512GB` |
| `GetSerialNumber(0)` | `YMA1512JA222440C1D  _00000001.` |
| `GetFirmwareRevision(0)` | `YM00D217` |
| `GetMediaType(0)` | `Fixed hard disk media` |
| `GetTotalVendorCapacity(0)` | 512（标称 GB） |
| `GetAllTotalCapacity()` | 488382（MB） |
| `GetAllUsedCapacity()` | 356557（MB） |

序列号带尾随空格与 `_00000001.` 后缀，直接展示前需要裁剪。
**没有健康度 / SMART 相关导出**，硬盘健康要另找来源（`root\Microsoft\Windows\Storage`
的 `MSFT_PhysicalDisk` / `MSFT_StorageReliabilityCounter`，DLL 里也出现了前者的查询字符串）。

### `BiosFun`

```
bool BiosFun::GetBiosSwitch(bool& out, DeviceDriverType type);
```

实测 type 取 0..15 全部返回 false。它底层走 `BiosWmi::GetDeviceBiosSwitchStatus`，
与散热组同因，需要管理员权限。`GetItemCodeByDeviceType` 是 `private`
（修饰名里是 `AEAA` 而非 `QEAA`），虽然被导出但属于内部接口。

### 可行性判断（系统信息）

- **可以直接用**：型号、SN、BIOS 版本与日期、主板信息、CPU 全组、内存全组、硬盘全组。
  一个「关于」面板需要的字段基本齐了，且不需要管理员权限，只需要 COM 初始化。
- **要另找来源**：硬盘健康 / SMART；`SystemInfo::GetSystemVersion` 的 Windows 版本
  （在 Windows 11 上报「Windows 10 Pro」，应改用 `RtlGetVersion` 或注册表
  `CurrentBuildNumber`）。
- **要先解决权限**：`BiosFun::GetBiosSwitch`。

## 其他值得记的导出

以下均为**静态**结论，未实测，按用途分组：

- `VideoCard`：`GetLcdEdidData(wstring&)`、`ParseEdidData(uint8*, ulong, wstring&)`、
  `GetEdidDataPath()`、分辨率 / 刷新率 / 显存 / 驱动版本一组。屏幕管理方向可能用得上。
- `MonitorDeviceInterface::GetMonitorAddressList()` 返回 `vector<ulong>`，
  与 `HalSetCallback`（模块号 3）配套，是唯一走回调的模块。
- `UsbDeviceInterface`：完整的 USB 拓扑枚举，`GetAllUsbPortDeviceTypeMp()`
  返回 `map<wstring, vector<UsbPortInfo>>`。
- `DeviceInformention`（厂商拼写）：按 GUID / 硬件 ID / 描述查设备属性、驱动安装时间、
  未知设备列表。
- `NetworkCard` / `NetworkAdapter`：网卡型号、MAC、驱动版本、Wi-Fi 开关状态。
- `TouchScreen::IsTouchDeviceExist()`。
- **危险导出，只读调研中不要碰**：`DeviceDriver::SetDriverStatus`、`ResetDeviceDriver`、
  `UnInstallDeviceDriver`、`DeviceInformention::UnstallDeviceByHardwareId`、
  `NetworkAdapter::SetWifiSwitchState`、`TouchScreen::OpenTrifingerDriver` /
  `CloseTrifingerDriver`、`ReadReg::WrightLocalMachineRegDW`（厂商拼写）及同组写注册表、
  `FileProcess::DeleteFileByPath`、`StartServiceByName` / `StopServiceByName`、
  `Battery::SetChargeThreshold`。

### `Hal*` C 导出：不必使用

`HalInit` / `HalOpenDevice` / `HalCloseDevice` / `HalRead` / `HalWrite` /
`HalQueryInformation` / `HalSetInformation` / `HalSetCallback` 是一层按模块号分发的门面。
`HalQueryInformation` 的模块跳转表（RVA 0x272F0，17 项）里只有 5 项有效：

| 模块号 | 转发到 |
| --- | --- |
| 0 | `CpuQueryInformation` |
| 3 | 显示器模块（COM 对象 + 回调，`HalSetCallback` 服务的就是它） |
| 4 | `GpQueryInforamtion`（厂商拼写） |
| 5 | 一个基于 `CreateFileW` / `ReadFile` / `WriteFile` 的串行设备通道 |
| 6 | `BtQueryInformation` |
| 11 | `MmQueryInformation` |

其余模块号直接返回 0。`BtQueryInformation` / `CpuQueryInformation` / `MmQueryInformation` /
`GpQueryInforamtion` 都是独立导出，直接调用即可，没有理由多绕一层。

## 六份副本

`C:\Program Files\Huawei` 下有 6 份 `HardwareHal.dll`：

| 路径 | 大小 | 日期 |
| --- | --- | --- |
| `BasicService\` | 553904 | 2026-02-11 |
| `Hiview\` | 553904 | 2026-02-11 |
| `HwSmartAudio\` | 553904 | 2026-02-12 |
| `PCManager\` | 553904 | 2026-02-12 |
| `HwLcdEnhancement\` | 553864 | 2026-02-10 |
| `Huawei OSD\` | 184184 | 2023-09-22 |

前五份大小几乎相同但 MD5 互不相同，属同一代码的不同构建，导出集一致。

`Huawei OSD\HardwareHal.dll` **是完全不同的东西**，只有 28 个导出
（`SetVolume`、`SetMic`、`SetScreen`、`SetRefreshRate`、`OsdFunc::GetSysManufactor` 等），
与本文描述的 448 导出版本没有交集，只有 `HalInit` / `HalQueryInformation` 两个同名导出。
它不是「裁剪版」，是同名不同物。**加载时必须写全路径，不能依赖 PATH 或当前目录搜索。**

推荐用 `BasicService\` 那份：它与 `WmiUtil.dll`、`HwFileUtil.dll` 同目录，依赖能就近解析。

## 加载方式

```
SetDllDirectoryA("C:\\Program Files\\Huawei\\BasicService");
LoadLibraryExA("C:\\Program Files\\Huawei\\BasicService\\HardwareHal.dll",
               nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
```

`LOAD_WITH_ALTERED_SEARCH_PATH` 是必需的：`HardwareHal.dll` 静态导入
`WmiUtil.dll` 与 `HwFileUtil.dll`，两者只存在于安装目录。

架构方面与另外三个厂商 DLL 一致：`HardwareHal.dll` 与 `WmiUtil.dll` 都是 x64，
纯 ARM64 进程加载不了，需要放在 ARM64EC 进程里。但因为它是普通同步导出，
这个 ARM64EC 进程不需要为它专门开线程，也不需要维持消息循环。

## 踩过的坑

**1. 多个类共用一块 `this` 缓冲区会崩。**
最初用一个全局 `char obj[512]` 依次当作 `SystemInfo`、`Cpu`、`Memory` 的实例。
`Cpu` 的惰性初始化在偏移 0 写入了自己的模块句柄，`Memory::Init` 随后看到
`*this != 0` 就跳过初始化，于是 `GetTotalMemoryNums` 和所有 `*byIndex` 全部
`0xC0000005`。换成每个类一块独立清零缓冲区后，同样的调用全部返回正确值
（16384 MB / 2133 MT/s / `Bank 0`）。**这不是 DLL 的问题，是用法错误**，
但排查时很容易误判为「这些导出在本机不可用」。

**2. `std::wstring` 返回值的参数顺序写反会得到全空，而不是崩溃。**
按 Itanium ABI 写成 `f(&ret, this)` 时，`SystemInfo`、`Baseboard`、`SmBiosHelper`、
`Cpu` 全组返回空字符串，看上去像是「WMI 没数据」或「需要提权」。
加了 COM 初始化仍然全空，才回头看反汇编发现 MSVC x64 是 `this` 在前。
改成 `f(this, &ret)` 后全部有值。判据是：调用后 `capacity` 字段仍为 0
说明函数根本没写——正常构造的 `std::wstring` 空串 `capacity` 是 7。

**3. 全组返回 -11 不等于「硬件不支持」。**
`BiosWmi::*` 整组返回 -11 时，很容易推断成「Gaokun 没有实现这些 ACPI 方法」。
实际是 `ROOT\WMI` 的 `OemWMIMethod` 在非提升进程下拒绝访问，用 PowerShell
`Get-CimInstance -Namespace root/wmi -ClassName OemWMIMethod` 一行就能复现
（返回「拒绝访问」）。同时 `Get-CimClass` 能正常列出该类及 `OemWMIfun` 方法签名，
说明类存在、设备存在，只是实例读不到。

**4. `SystemInfo::GetSystemVersion` 在 Windows 11 上报「Windows 10 Pro」。**
这是 `Win32_OperatingSystem.Caption` 的经典问题，不是 DLL 的 bug，但直接展示会误导用户。

**5. `dumpbin` 的开关在 Git Bash 里要写成 `-exports` 而不是 `/exports`。**
MSYS 的路径转换会把 `/exports` 变成 `C:\Program Files\Git\exports`，
报 `LNK1181: 无法打开输入文件`，看起来像是目标文件的问题。

**6. 厂商拼写错误不少，绑定时必须照抄。**
`SetBatteryChargeThreshlod`（Threshold）、`GetBattertLoss`（Battery）、
`GetBIOSReaseDate`（Release）、`GpQueryInforamtion`（Information）、
`DeviceInformention`（Information）、`WrightLocalMachineRegDW`（Write）、
`GetEnviromentLight`（Environment）、`GetAllTotalVendorCapcity`（Capacity）、
`CHeckIPReady`、`JudgeSSd`。

## 尚未验证的事项

按落地优先级排列：

1. **提升权限后 `BiosWmi::*` 是否返回真值。** 整个散热组与充电阈值读回都压在这上面。
   需要以管理员身份重跑一次 x64 测试程序。
2. **`GetSmartChargeMode` 的输出字节含义。** `out[3]` / `out[4]` 与「起充 / 停充」
   的对应关系是从结构体字段顺序推断的，没有实测。确认方式：读一次，与 PC 管家里
   显示的当前阈值对照。
3. **无风扇机型上 `GetFanSpeed` 提权后的行为。** 返回 0 还是失败码，决定 UI 上
   是隐藏风扇卡片还是显示「不适用」。
4. **`EnmHalCpuType` = 3 的含义。** 枚举定义未还原。
5. **`GetPerformanceMode` 为何返回 1 而非 -11。** 它是唯一的例外，但输出未被改写，
   不能认为成功。
6. **`HalOpenDevice` 模块 5 打开的是什么设备。** 设备路径由模板字符串加索引拼成，
   模板本身未定位。与电池 / 散热 / 系统信息三个面板无关，未深入。
