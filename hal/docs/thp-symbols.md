# THP_Service 符号地图

工程版的 `THP_Service.dll` 随包带了 `.pdb` 与 `.map`（13494 行，含符号名与 `.obj` 归属），
本文把其中对本项目有用的部分固定下来。零售版没有符号，靠 `.pdata` 函数大小序列比对加锚点
插值对应过去。

地址一律是 RVA，ImageBase `0x180000000`。两版的同一符号相差 `0x3FE0`（工程 757760 字节，
2022-08-11；零售 778120 字节，2022-11-11），但这个差值只在 `.data` 段成立，代码段要查表。

## 模块

不带库前缀的自有 obj 共十个，其余来自 CRT、STL 与 ConcRT。

| obj | 职责 |
| --- | --- |
| `usb_device` | MCU HID 通道：设备枚举、收发线程、命令分发、ACK、电源事件 |
| `THP_Service` | DLL 门面：四个 Register 回调、`ThpFuncStart`/`ThpFuncStop`、日志开关 |
| `framework` | PenKit 应用交互、令牌与会话、`_API_FUNC_S` 函数表持有者 |
| `logFile` | 日志文件、驱动版本、注册表版本号读取 |
| `cmd_server` | 调试用 socket 命令服务 |
| `vhf_device` | HID injector 写入路径 |
| `hid_device` | **已无活函数**，只剩四个全局与静态初始化桩，遗留模块 |
| `commonResource` | 存放宿主注册进来的四个回调指针 |
| `system_event` | `_THP_SYSTEM_STATE_S` 全局与取指针函数 |
| `dllmain` | `DllMain` |

## 七个导出

两版的名称与序号完全一致，`hal/src/thp/ThpModule.cpp` 解析的正是这七个，无遗漏也无多余。

| 序号 | 名称 | 工程版 | 零售版 |
| --- | --- | --- | --- |
| 1 | `GetMESSAGE` | `0xAC80` | `0xDA90` |
| 2 | `RegisterEventLogStatus` | `0xAD90` | `0xDBA0` |
| 3 | `RegisterGetPenEleValue` | `0xADD0` | `0xDBE0` |
| 4 | `RegisterPrintEventLog` | `0xAE10` | `0xDC20` |
| 5 | `RegisterSetPenEleValue` | `0xAE50` | `0xDC60` |
| 6 | `ThpFuncStart` | `0xAE90` | `0xDCA0` |
| 7 | `ThpFuncStop` | `0xB480` | `0xE5B0` |

`ThpFuncStart` **无条件返回 1**：全函数体只有一个 `ret`、一处 `mov eax,1`，SMBIOS 读取、
`SpbModuleInit`、`SpiReadAcpi` 的成败都不改变返回值。两版皆然。检查它的返回值没有意义，
失败要从原厂日志里看。

导入 `ApDaemon.dll` 的只有四个符号：`FunInitList`、`ThpStart`、`ThpStop`、`ThpNotify`。

## `_API_FUNC_S` 函数表

`Vhf_RegisterFunction` 等几个 Register 函数往表里填槽位，`ThpFuncStart` 最后把表交给
`ApDaemon!FunInitList`。表实例是 `?g_stThpApi@@3U_API_FUNC_S@@A`（工程版 `0xB27E0`）。

| 偏移 | 函数 |
| --- | --- |
| `+0x00` | `Vhf_ReportCoordinates`（手指，32 字节） |
| `+0x08` | `Vhf_ReportPenCoordinates`（笔，13 字节） |
| `+0x10` | `Vhf_Start` |
| `+0x18` | `Vhf_Stop` |
| `+0x68` | `Usb_Start` |
| `+0x70` | `Usb_Stop` |
| `+0x78` | `GetUsbSystemStatus` |
| `+0x98` | `Log_Print` |
| `+0xA0` | `RunThreadProcess` |
| `+0xA8` | `GetReportBluetoothPenInfo` |
| `+0xB0` | `ThpReset` |

ApDaemon 侧把表指针存进 `ApDaemon+0x16ADF8`（工程版）。验证整条注册链只需读这个值是否等于
`g_stThpApi`，再看两个槽位指向对不对，做法见
[`thp-power-gate.md`](thp-power-gate.md) 第三节。

## MCU 命令分发

`AsynchProcThreadProc` 从环形缓冲取 8 字节头，校验 `packet[2] == 0x07` 与
`packet[4] == 0x01`，再用 `packet[5]` 作操作码查两级表（索引表 `0xCC24`，跳转表 `0xCBC0`）。
包头模板 `0x12010207` 由 `Usb_Start` 写进 `0xB2A58`。

下表是解出的全部分支，RVA 属工程版。带 ACK 序号的分支在处理完后调一次
`usbAck(序号)`；`usbAck` 每次现开设备再发，构造头 `[+0]=0x07`、`[+1..2]=0x0201`、
`[+4..5]=0x8001`、`[+7]=0x20`，载荷就是那一个序号字节。

| 操作码 | 名称 | RVA | ACK |
| --- | --- | --- | --- |
| `0x03` | `USBD_SW_VERSION` | `0xBA2E` | |
| `0x08` | `BATTERY_STATUS` | `0xBB3D` | |
| `0x09` | `CHARGING_STATUS` | `0xBB4F` | |
| `0x10` | `DEV_CONNECT` | `0xBA67` | |
| `0x12` | `DEV_PAIR_STATUS` | `0xBA76` | |
| `0x21` | `PEN_DOCK_STATUS` | `0xBB5B` | |
| `0x23` | `PEN_UPDATE_STATUS` | `0xBB8B` | |
| `0x27` | `PEN_KEY_FUNC_GET` | `0xBB9D` | |
| `0x2C` | `PEN_BATTERY_AFTER_CONN` | `0xBB67` | |
| `0x2E` | `PEN_PAIR_DETECT_ACK` | `0xBB79` | |
| `0x2F` | `PEN_CURRENT_FUNC` | `0xC7BB` | `0x0B` |
| `0x70` | `PEN_AC_STATUS` | `0xBBAF` | `0x00` |
| `0x71` | `PEN_CONN_STATUS` | `0xBCF7` | `0x01` |
| `0x72` | `PEN_CUR_STATUS` | `0xBFD8` | `0x02` |
| `0x73` | `PEN_TYPE_INFO` | `0xC993` | `0x0D` |
| `0x74` | `PEN_ROATE_ANGLE` | `0xC159` | `0x03` |
| `0x75` | `PEN_TOUCH_MODE` | `0xC338` | `0x04` |
| `0x76` | `PEN_GLOBAL_PREVENT_MODE` | `0xC47A` | `0x05` |
| `0x77` | `PEN_SCREEN_STATUS` | `0xC554` | `0x06` |
| `0x78` | `PEN_HOLSTER` | `0xC5C6` | `0x07` |
| `0x79` | `PEN_FREQ_JUMP` | `0xC66E` | `0x08` |
| `0x7B` | `PEN_REP_PARAM` | `0xC77B` | `0x0A` |
| `0x7C` | `PEN_GLOBAL_ANNOTATION` | `0xC8A9` | `0x0C` |
| `0x7F` | `ERASER_TOGGLE` | `0xC6E0` | `0x09` |
| 其余 | `UNKNOWN` | `0xCA39` | |

`0x7F` 把载荷写进 `?penStatus@@3HA` 后起线程调 `ApDaemon!ThpNotify`，由厂商核心重新出报告；
`0x2F` 写的是另一个全局 `?penkitErase@@3HA`，不通向 HID。两者的区别见
[`thp-eraser.md`](thp-eraser.md)。

## 握手

`0x7101` 由 `?FirstPenConnected@@YAXXZ`（`0xD2E0`）发出，`Usb_Start` 在 USB 读线程就绪后
另起线程调用它，载荷为空。

`0x7D01` 带 32 字节载荷，由 `?PenParamEvent@@...`（`0xE180`）发出。它不在 `_API_FUNC_S` 的
槽位里，而是被 `?GetReportBluetoothPenInfo`（槽位 `+0xA8`）调用——也就是说 InitParam 的下发
是厂商核心经回调触发的，不是 `AsynchProcThreadProc` 收到 `0x7B` 时直接做的：`0x7B` 分支本身
只打日志并回 ACK `0x0A`。

**`0x7701` 在两版的 `.text` 里都不存在**，也没有向包头偏移 4/5 写入 `0x77` 的指令。发出者
不在本模块，要到 `ApDaemon.dll` 或 `himax_thp_drv.dll` 里找。

## 配置下标

`VHFFunction` 与 `LogFunction` 两个字符串不在 `THP_Service.dll` 里，解析在 .NET 宿主
`HuaweiThpService.exe` 的 `XmlOperator` 中，经 `RegisterGetPenEleValue` 把委托交进来，DLL 按
整数下标调用。下标对应关系由该宿主的 IL 确认（`ldarg.1` / `brtrue` 分支）：

- 下标 `0` → `VHFFunction`。`PEN_CONN_STATUS` 分支取它，为 0 才继续走到 `SetXmlOperatorFlag`。
- 下标 `1` → `LogFunction`。`ThpFuncStart` 开头取它存进 `?logFunction@@3HA`，而该全局没有任何
  读取点，所以这个配置项实际上什么都不控制。

读不到配置文件时厂商返回 0（`penFunc` 初值，所有失败路径都返回它）；`CreateXmlFile` 不创建
目录，只 `XmlDocument.Save`，目录不存在就抛异常。我们的 `ThpConfig` 复刻了这些行为，只有兜底
模板的 `VHFFunction` 取 1 而厂商取 0——随包配置本来就是 1，厂商那个默认值反而会自我抑制。
