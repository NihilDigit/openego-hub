# 充电控制：链路、语义与已验证结论

充电模式与阈值经 OEM WMI 写入 EC，规则由 EC 固件执行。手动固定上限与智能充电都已实测
生效，两者都不需要后台进程维护：写一次就一直生效，禁用 PC Manager 不影响。本文记录整
条链路、字段语义、证据与待解决项，供后续改动直接引用。

## 链路

写（设置上限 / 切模式）：

```
EGoTouchSettings（非提权）
  ChargeLimitSlider / SmartChargeToggle  → SendTrayCommand(SetChargeLimit, percent)
    smart 用哨兵 0，非 smart 用滑块值           MainWindow.xaml.cpp:1431 / :1484
EGoTouchTray
  SubmitChargeLimitCommand                    EGoTouchTray.cpp:1654
    ↓ PenControl 共享内存（校验 percent==0 || 50..100，PenControlChannel.cpp:415）
OpenEGoHubService（LocalSystem）
  HandlePenControlCommand 的 ChargeLimit 分支  ServiceHost.cpp:1487-1521
    ↓ RunHalTool 拉起 GaokunPower.exe（--smart / --limit <n>），等退出码，15 s 上限
GaokunPower.exe（继承 LocalSystem，提权由此而来）
  Detail::SetChargeLimit / SetSmartCharge     ChargeLimit.cpp:61 / :80
    ↓ Oem::Invoke → ROOT\WMI 的 OemWMIMethod::OemWMIfun → ACPI PNP0C14\HWMI_0 → EC
```

读走另一条路：服务进程内直调 GaokunHal.lib 发 `03 16`（ServiceHost.cpp:110-119 的
RefreshChargeLimit），结果经 PenStatus 共享内存到托盘与设置窗。读需要提权，设置窗自己
读不到（实测 0x80041003）。读的时机只有三处：AccessoryLoop 首轮、touch_only 启动时一次、
每次写之后；没有周期重读。

## ACPI 层：命令表与 EC 命令字

`OemWMIfun` 的语义不必靠猜。本机 ACPI 的 SSDT（`HUAWEI SSDT1Tbl`）里就是这条通道的实现：
设备 `WMI1`，`_HID` 为 `PNP0C14`，`_UID` 为 `"HWMI"`，方法 `WMAA` 按请求缓冲的头两字节
（MFID/SFID）分发。导出方式见「验证命令」一节。

电源电池是 MFID = 3 这一组：

| SFID | ACPI 方法 | EC 命令 | 作用 |
| --- | --- | --- | --- |
| 0x10 | SBTT | 0x68 逐字节写 | 旧式起充/停充阈值，不带模式 |
| 0x11 | GBTT | 0x69 逐字节读 | 同上，读回 |
| 0x12 | SBAC | 0xE5 | 写一个布尔标志 |
| 0x13 | GBAC | 0xE6 | 读该标志 |
| 0x14 | GAIT | 0xE2 | 交流电已连接时长，16 位 |
| 0x15 | SBCM | 0xE3 | 写模式 + 延时 + 两个阈值 |
| 0x16 | GBCM | 0xE4 | 读回上述四项 |
| 0x17 | GRPL | 0xAA | 功率上限 |

传输统一走 `ECCD(0x02, 命令字, 入参长度, 入参, 出参长度)`。

`03 10` 与 `03 15` 不是同一条命令，也不作用于同一份 EC 记录：前者打 0x68/0x69，一次一个
字节，没有模式字段；后者打 0xE3/0xE4，四个字节一次写完。本机 `03 11` 读回 `0 / 0`，旧记录
是空的。

## SBCM 字段

写请求（`03 15`）六字节，字段名照搬 goodies 的 Set-ChargeLimit.ps1（ChargeLimit.cpp:3），
与 ACPI 里 `SBCM` 的 `CreateByteField` 逐个对应：

| 偏移 | 字段 | 语义 |
|---|---|---|
| [2] | CHMD | 充电模式，取值 1..6 |
| [3] | DELY | 连续接电小时数门限，见下节 |
| [4] | STCP | 起充阈值，厂商恒取停充 - 5 |
| [5] | SOCP | 停充阈值 |

`SBCM` 只校验两个阈值落在 0..100，`CHMD` 与 `DELY` 原样透传给 EC，语义在固件里。

`GBCM`（`03 16`）无载荷，EC 返回六字节 `[status][len][CHMD][DELY][STCP][SOCP]`，ACPI 丢掉
第二字节后拼成输出，因此 `out[0]` 是状态、`out[1..4]` 依次是四个字段，与 ChargeLimit.cpp
的 `kOffset*` 一致。

UI/IPC 层的哨兵 0 与 CHMD 是两套值：0 只存在于 EGoTouchTrayIpc / PenControlChannel，
被服务译成 `--smart`，hal 写的是 CHMD=4。链路上没有一处把 0 当 CHMD 下发。

## 智能充电的语义

CHMD=4 不是「交给某个后台进程去管」，而是 EC 固件里的一条规则：

```
EC 自己按小时累计交流电连接时长（即 GAIT 读到的值，拔电清零）
  时长 < DELY  →  不限充，照常充到 100%
  时长 ≥ DELY  →  按 STCP/SOCP 限充，且在接着电源时主动放电，把电量压到目标区间
```

厂商写下的一组值是 `CHMD=4, DELY=72, STCP=65, SOCP=70`，即连续接电三天后把电量维持在
70% 左右。这与华为对智能充电的描述逐字对应。

`DELY` 的这个含义在两个厂商 DLL 里都没有字符串或符号命名，是由三条证据合起来定下的：
软件侧 `CheckAcDuration` 的判据是 `acDuration < 0x48 则不动作`，与写进 `DELY` 的 0x48 是
同一个常量；`GAIT` 是一条现成的「交流电已连接时长」命令；以及下节的实测。

## 已验证结论

**手动模式在工作。**

- 写 80 后读回 STCP/SOCP = 75/80，CHMD 由 4 变 1（ChargeLimit.cpp:11-13）。
- 一次 84%→95% 充电曲线实测：硬件停在 34861 mWh，95% 满充为 35042 mWh
  （GaokunPower.h:90-96）。截止由 EC 执行，写入后不需要任何后台服务维护。

**智能模式同样由 EC 执行，禁用 PC Manager 不影响。** 2026-08-31 本机实测：

- 起点：`CHMD=4, DELY=72, STCP=65, SOCP=70`，`GAIT=2`，电池满电、接电、不充不放。系统日志
  显示 9:01:36 接上电源，读数时刻 11:32，连续接电 2 小时 31 分——`GAIT` 的单位是小时，
  且在 8:56 那次拔电时清零了。
- 把 `DELY` 改成 1（`GAIT` 已经是 2，条件立即满足）。四分钟内 EC 在接着电源的状态下开始
  放电：`PowerOnline=True`、`ChargeRate=0`、`DischargeRate` 约 5000 mW。
- 拔电一分钟再插回，`GAIT` 归零；此时 `DELY` 写 0，条件仍然成立，放电继续，剩余容量
  36771 → 36068 mWh 持续下降。`DELY=0` 是立即生效，不是关闭。
- 写回 `DELY=72` 后（`GAIT` 已归零，`0 < 72`）放电停止。

**先前「智能模式等于不限充」的判断是错的。** 那台机器读到的 `65 / 70 / mode 4` 是 PC
Manager 按智能档写下的原值，电池充到 100% 的原因只是连续接电没有达到 72 小时。同样作废
的还有「DELY 可能被 EC 忽略」：实测写 3、1、0 都留住了，之前读回 72 是因为 `--smart` 那条
路当时没有跑过。

**EC 接受任意 DELY 与任意阈值，厂商界面的六档是软件侧的限制。** `EnsureCapacityValid`
把上限吸附到 50/60/70/80/90/100 之一，下限取上限减 5。这个吸附发生在 PC 管家的 UI 层，
ACPI 的 `SBCM` 只校验 0..100，固件对 83 和 85 一视同仁——它是产品决策，不是硬件约束。
本仓库不跟随，滑块按 5 步进放行 50..100，实测写 80 生效，写 3、1、0 到 DELY 也都留住了。
后续若要对齐六档，需要另有理由，不能以「厂商这么切」当作硬件限制。

## 厂商侧做了什么

`SmartChargePlugin.dll`（由 `MBAMessageCenter.exe` 加载，`plugincfg.xml` 里 clsid
`msgcenter.smart.charge.plugin`）是唯一写这条命令的模块，转调 `WmiUtil.dll` 的
`HwSDK::BiosWmi::SetSmartChargeMode`（RVA 0x5430）。后者把参数打成
`03 15 <mode> <dely> <lower> <upper>`；它的 bool 参数不占字节，只是取值开关，为真时 `dely`
恒取 0x48、阈值取内置默认表（RVA 0x13798：mode1 = 65/70，mode2 = 85/90，mode3 = 95/100，
mode4..6 = 65/70）。插件六处调用全部传 `false` 加显式 `0x48`，与默认表同值。

**没有学习逻辑，也没有周期性重写。** 写 EC 的入口在插件里只有一个函数，调用点全量枚举是
七处，落在四个离散时刻：插件启动、从睡眠唤醒、用户改档（IPC 消息 0x90003）、用户点提示窗
的「立即开启」。这七处的上下限五处是编译期常量、两处是用户输入，无一来自计算。插件里唯一
的周期任务是一小时一次的循环，只上报大数据并决定是否弹窗，不碰 EC。

`CheckAcDuration` 读 `GAIT`，超过 72 小时后弹窗提醒或建议开启智能充电，同样不改阈值。

档位持久化在 `HKLM\Software\PCManager\MBAPowerManager`：`PowerSafeManagerStatus` 总开关、
`CustomChargeCapacity` 自定义上限、`SmartChargeMode` 与 `PowerSafeManagerMode` 两个旧档
序号。值不存在时按缺省分支走。

`Battery::GetChargeThreshold` 是坏的（hardware-hal.md:157-188），这是读路径自行逆向的原因。

## 验证命令

`GaokunPower.exe` 是阈值唯一的命令行入口（GaokunCtl 没有充电相关命令）：

```powershell
GaokunPower.exe --info               # 电池实况，不需要提权
GaokunPower.exe --query              # 读阈值与模式，需管理员
GaokunPower.exe --limit 80 --dry-run # 走完全部 COM 步骤不下发，打印每步 HRESULT
GaokunPower.exe --limit 80           # 写手动阈值
GaokunPower.exe --smart              # 交还智能模式（CHMD=4）
```

`GAIT`、`GBAC`、`GBTT` 目前没有命令行入口，用 `Invoke-CimMethod` 直接发即可（需管理员，
非提升进程枚举不到 `OemWMIMethod` 的实例）：

```powershell
$i = Get-CimInstance -Namespace root\wmi -ClassName OemWMIMethod | Select-Object -First 1
$b = New-Object byte[] 64; $b[0] = 0x03; $b[1] = 0x14   # GAIT
$r = Invoke-CimMethod -InputObject $i -MethodName OemWMIfun -Arguments @{ u8Input = $b }
$r.u8Output[0..4]
```

导出并反编译 ACPI 表（不需要提权）：`GetSystemFirmwareTable('ACPI', 'DSDT'|'SSDT')` 取到
AML，再用 ACPICA 的 `iasl -e DSDT.aml -d SSDT.aml`。SSDT 需要 DSDT 作为外部引用才能解析
符号。

## 待解决

1. **界面无法显示智能充电是否正在限充。** 厂商没有提供这个状态位——`WmiUtil.dll` 的
   十二个相关导出里只有模式加阈值，没有「当前是否受限」。要显示只能自己拿 `GAIT` 与
   `DELY` 比较推导。
2. **`SetSmartCharge` 写的 DELY 与厂商不一致。** ChargeLimit.cpp:80 恒写 0x18 = 24，厂商写
   72。EC 接受这个值，因此走过这条路的机器会在接电一天后就开始限充，而不是三天。阈值同样
   与厂商不同：厂商固定 65/70，本仓库沿用读回的当前值。
3. **写入无回读校验**：RunHalTool 只看退出码；写后虽会重读（ServiceHost.cpp:1517-1519）
   但只更新缓存，不与写入值比较。ExecMethod 成功而 EC 拒绝的情形没有任何一层能发现。
4. **缓存陈旧**：无周期重读，外部改了模式后界面一直显示服务启动那一刻的值，直到下次写入
   或重启服务。`GAIT` 若要进界面，则必须周期重读。
5. `SBAC`/`GBAC`（`03 12` / `03 13`）的确切语义未定。本机 `GBAC` 返回 2（EC 原值 0），
   `SBAC` 的 RID 1 写 1、RID 2 写 0。`WmiUtil` 里的导出名指向「恢复充电标志」，插件侧的
   `BatterySuperCharge` 指向超级快充开关，两者未对上。
6. `CHMD` 取值 2、3、5、6 的固件行为未测。BIOS 侧 4、5、6 的默认阈值相同，差别只在 PC
   Manager 的策略层；1、2、3 是旧档位的三组阈值。
