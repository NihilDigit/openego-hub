# 原厂电源门控与 WaitForResume

本文回答一个问题：同一套原厂 DLL，为什么由原厂服务加载时触摸正常，由 `GaokunThpHost`
加载时一帧都不出。

**结论是 `THP_Service.dll` 内部另有一个 ServiceMain，它要以服务身份向 SCM 注册控制处理器，
注册失败就不订阅电源通知，ApDaemon 随即把系统当成没通电并挂起整条算法链。** 我们的宿主不是
服务，注册必然失败。这个缺陷在零售版上被掩盖了六个月——零售版 ApDaemon 在拿不到电源通知时
默认认为有电，工程版默认认为没电。

三类证据，文中逐条标注：

- **反汇编**：工程版 `THP_Service.dll`、`ApDaemon.dll` 经 `dumpbin /disasm`，符号取自随包的
  `THP_Service.pdb` 与 `.map`。
- **原厂日志**：`C:\ProgramData\Huawei\HuaweiTHP\Service_LogFile-<账户>-<日期>.txt`。该日志
  始终开启，与 `HuaweiTHP.config.xml` 里的 `LogFunction` 无关，见第五节。
- **本机实测**：对运行中的宿主用 `ReadProcessMemory` 观测，个别探针用 `WriteProcessMemory`
  写回原值。

本文的 RVA 属于工程版 `THP_Service.dll`（757760 字节，2022-08-11），ImageBase `0x180000000`。
零售版（778120 字节，2022-11-11）地址不同，两者差 `0x3FE0`。

## 一、门控链路

```
THP_Service ServiceMain
  └─> RegisterServiceCtrlHandlerExW(L"HuaweiThpService", ServiceCtrlHandlerEx, nullptr)
        失败 ─> 打印 "ServiceMain: RegisterServiceCtrlHandlerEx returned error" 并 Exit
        成功 ─> RegisterPowerSettingNotification(GUID_MONITOR_POWER_ON)
                RegisterPowerSettingNotification(GUID_CONSOLE_DISPLAY_STATE)
                RegisterPowerSettingNotification(GUID_LIDSWITCH_STATE_CHANGE)
                  └─> ApDaemon DriverThp::RegListeningPower 收到 Power = 1
                        └─> ThpBase::WaitForResume 放行，算法链开始处理帧
```

注册调用在工程版 `0x18000DA51`（反汇编）：

```
000000018000DA43: lea  rdx,[?ServiceCtrlHandlerEx@@YAKKKPEAX0@Z]
000000018000DA4A: lea  rcx,[L"HuaweiThpService"]
000000018000DA51: call qword ptr [__imp_RegisterServiceCtrlHandlerExW]
```

服务名硬编码为 `HuaweiThpService`。`RegisterServiceCtrlHandlerExW` 要求调用进程正在以该名字
运行服务，因此这条路只对原厂那个 .NET 服务成立。

失败时的表现是算法链整体停摆，而不是报错：

```
ServiceMain: RegisterServiceCtrlHandlerEx returned error
ServiceMain: Exit
DriverThp::RegListeningPower [352] Power = 0
DriverThp::UpdateStatus [1193] mStatus = 32768
ThpBase::WaitForResume [228] -	-	-	-	--->holding
```

`mStatus` 正常时是 `32769`（`0x8001`），holding 时是 `32768`，差的是 bit0。此后原厂日志只剩
`[USB]` 的 MCU 消息，`TsaFrame::ObInput` 一条不出——触摸帧根本没有进入算法链。

## 二、零售版与工程版的分歧

两版 ApDaemon 在注册失败时的默认值相反。这是全部问题的来源。

| | 零售版 `Daemon_WIN_1.0.1.41` | 工程版 `Daemon_WIN_1.0.1.27` |
| --- | --- | --- |
| `RegListeningPower` lambda 符号 | `lambda_02ceccd179c8e8da1e1ff65a690f93cb` | `lambda_01a77ce37ad8a5b00b67ab0c61c0a2cb` |
| 注册失败时的 Power | 1，照常工作 | 0，`WaitForResume` 挂起 |

同一天的日志里三种组合并列，对照清楚（原厂日志）：

| 宿主 | DLL | ServiceMain | Power | 触摸 |
| --- | --- | --- | --- | --- |
| `GaokunThpHost --hosted` | 零售 | error → Exit | 1 | 正常 |
| 原厂服务 | 工程 | success | 1 | 正常 |
| `GaokunThpHost --hosted` | 工程 | error → Exit | 0 | 无 |

第一行就是本机的日常状态：我们从来没能让这个注册成功过，只是零售版不在乎。

## 三、可复用的探针

这几个探针不需要断点，也不需要注入，排查同类问题时可以直接用。

**用 `XmlOperatorFlag` 当触摸总闸。** 该标志非零时 `Vhf_ReportCoordinates` 与
`Vhf_ReportPenCoordinates` 双双跳过 `WriteFile`，手指和笔一起停。把运行中的进程的这一字节写成
1，触摸立即停止，写回 0 恢复——可以用它确认某条触摸到底走不走 VHF，而不必猜。

**给 `g_hFile` 写一个非零的无效句柄，可以问出 `Vhf_ReportCoordinates` 有没有被调用。** 被调用
则 `WriteFile` 失败并打印 `[VHF]WriteFingerFile failed! er...`，随后失败分支自己调用
`ReOpenHidInjectorDevice` 把句柄重开，探针自愈。日志毫无动静就说明这个函数根本没被调用。必须
用非零值：零会先走到 `ReOpenHidInjectorDevice` 分支，到不了 `WriteFile`。

**函数表可以端到端验证。** `ApDaemon!FunInitList` 把表指针存进 `ApDaemon+0x16ADF8`：

```
0000000180002840: sub  rsp,28h
0000000180002844: mov  qword ptr [018016ADF8h],rcx
```

该值应等于 `THP_Service+0xB27E0`（`g_stThpApi`），表的 `+0x00` 应指向 `Vhf_ReportCoordinates`，
`+0x08` 指向 `Vhf_ReportPenCoordinates`。三项都对，就能排除注册与传表环节。

**`BytesWritten`（`+0xC53A8`）是死全局，不要用它判断写入。** `Vhf_ReportCoordinates` 传给
`WriteFile` 的第四个参数是栈上变量：

```
000000018001115F: lea  r9,[rsp+80h]
000000018001116B: call qword ptr [__imp_WriteFile]
```

这个全局在原厂服务进程里同样恒为 0，拿它当判据会得出"从未写入"的错误结论。

## 四、已排除的假设

工程版下触摸不工作，与下列各项无关。逐条实测排除，免得重查。

- **进程身份与会话**。宿主以 LocalSystem 在 session 0 走正常 lease 路径接管，同样无触摸。
- **算法链路**。`TsaFrame::ObInput touch:1, down:1(+0)` 与原厂服务逐字一致——在电源门控放行的
  前提下，帧能一路走到算法层。
- **函数表注册**。两种宿主下 `pFuncReportCoordinates IS REGISTERED!`，表指针与两个槽位全部正确。
- **注入设备**。`GUID_DEVINTERFACE_HIDINJECTOR` 是 `{59819b74-f102-469a-9009-3caf35fd4686}`，
  系统里只有一个实例（`HidInjectorSample`），两种宿主打开的是同一个，`g_hFile` 都非零。
- **写入抑制**。两种宿主下 `XmlOperatorFlag` 都是 0。
- **模块加载方式**。两个进程里 `THP_Service.dll` 与 `ApDaemon.dll` 的基址完全相同，
  `SetDllDirectoryW` 那条路没有问题。
- **面板适配**。工程版认得本机面板：`Project ID = W273AS1310`、`Found BOE new panel!`。

## 五、`LogFunction` 控制不了原厂日志

`ThpFuncStart` 开头把配置值读进 `?logFunction@@3HA`（`+0xB29B0`），而该全局**没有任何读取点**
（全模块交叉引用）。真正的开关是 `?logFlag@@3_NA`（`+0xB29B4`），由 `ThpFuncStart` 无条件置 1、
`ThpFuncStop` 置 0，`GetLogFlag` 读它。`Log_Print` 判过这个标志之后交给 `LogFile::WriteLog`
落盘。

所以原厂日志在接管期间始终是开的，排查不需要改 `HuaweiTHP.config.xml`，直接取
`C:\ProgramData\Huawei\HuaweiTHP\` 下的文件即可。同一目录同时存放 `Service_LogFile-*`（THP 全
链路）与 `hx_hal_log_*`（面板层）——`THP_Service.dll` 写 `HuaweiTHP`，`himax_thp_drv.dll` 写
`HuaweiThp`，Windows 不区分大小写，是同一个目录。

`RegisterPrintEventLog` 注册的回调不是日志出口：模块里只有 `SetPrintEventLog`，没有对应的
getter，也没有任何调用点。实测接管 20 秒内一次都不触发，不要试图从那里取原厂的诊断信息。

## 六、地址表

工程版 `THP_Service.dll` RVA。

| 符号 | RVA | 说明 |
| --- | --- | --- |
| `?g_hFile@@3PEAXEA` | `0xC53A0` | HID injector 句柄 |
| `?BytesWritten@@3KA` | `0xC53A8` | 死全局，见第三节 |
| `?XmlOperatorFlag@@3_NA` | `0xC53AC` | 非零时跳过所有 VHF 写入 |
| `?g_State@@3U_THP_SYSTEM_STATE_S@@A` | `0xB29A0` | 系统状态 |
| `?g_pStateCopy@@3PEAU_THP_SYSTEM_STATE_S@@EA` | `0xB2B10` | 状态副本指针 |
| `g_Monitor*Event` | `0xAF0C8`–`0xAF0F8` | 六个电源事件句柄 |
| `?logFunction@@3HA` / `?logFlag@@3_NA` | `0xB29B0` / `0xB29B4` | 见第五节 |
| `?g_stThpApi@@3U_API_FUNC_S@@A` | `0xB27E0` | 函数表实例 |
| `?Vhf_ReportCoordinates@@...` | `0x110F0` | 手指报告，32 字节 |
| `?Vhf_ReportPenCoordinates@@...` | `0x11230` | 笔报告，13 字节 |
| `?FindMatchingDevice@@...` | `0x10D50` | 枚举并打开注入设备 |
| `?SetXmlOperatorFlag@@YAX_N@Z` | `0x110A0` | 唯一写入点 |
| ServiceMain 的注册调用 | `0xDA51` | 见第一节 |

工程版 `ApDaemon.dll`：`?FunInitList@@YAXPEAU_API_FUNC_S@@@Z` 在 `0x2840`，函数表全局在
`0x16ADF8`。

## 七、以服务身份运行即可解除

把宿主注册成一个 `SERVICE_WIN32_OWN_PROCESS` 服务、由 SCM 启动，原厂那次注册就会成功：

```
ServiceMain: GUID_MONITOR_POWER_ON PowerSettingNotification register success!!!!
DriverThp::UpdateStatus [1193] mStatus = 32769
DriverThp::RegListeningPower [352] Power = 1
```

触摸随即恢复（实测，服务名 `OpenEGoHubThpHostTest`，工程版 DLL）。两点因此确定：

- **服务名不必是 `HuaweiThpService`。** `RegisterServiceCtrlHandlerExW` 对
  `SERVICE_WIN32_OWN_PROCESS` 忽略 `lpServiceName`，原厂硬编码的那个名字会绑定到当前进程所属
  的服务，不管它叫什么。我们不必去占用原厂的服务名，也就不必改动原厂服务的注册。
- **同进程内注册两次不冲突。** 宿主的 `ServiceMain` 已经注册过一次控制处理器
  （`ThpHostMain.cpp`），`THP_Service` 内部再注册一次同样成功，两者并存。

`--console` 因此只适合读配置一类的自检，不能用来判断触控是否正常：它永远拿不到电源通知。

## 八、注意日志按账户分文件

`Service_LogFile-<账户>-<日期>.txt` 里的账户是写入进程的身份。宿主由服务拉起时是 `SYSTEM`，
用 `--console` 手工跑时是当前登录账户，两者落在**不同的文件**里。排查时只盯着 `-SYSTEM-` 那
一份，会把手工跑的那次当成"什么都没发生"，再把前后邻近的原厂服务记录误读成宿主的。

`--console` 与 `--hosted` 的行为没有差别，两者都拿不到控制处理器：

```
10:01:13:151 ServiceMain: RegisterServiceCtrlHandlerEx returned error
10:01:13:152 ServiceMain: Exit
10:01:13:211 DriverThp::RegListeningPower [352] Power = 0
10:01:13:213 ThpBase::WaitForResume [228] --->holding
```

进程是不是服务，是这里唯一的变量。
