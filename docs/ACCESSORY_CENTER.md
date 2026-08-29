# 华为 PCManager 选件中心逆向

面向接下来实现托盘设备面板的人。回答四个问题：选件中心到底显示哪些状态、每一项从哪里取、
哪些我们自己就能拿到、界面该长什么样。

分析在装有 PCManager 的本机上完成，版本信息见第 9 节。

> 置信度标记
> - **实测**：本机运行日志、注册表、资源文件里直接读到的值
> - **静态**：反编译代码或二进制里读出来的逻辑，未经运行验证
> - **猜测**：显式标注，不作为实现依据

---

## 1. 结论速览

**选件中心不是一个程序，是一个宿主加一组按设备型号分发的插件进程。**

```text
PCManager.exe
  └─ components/accessories_center/AccessoriesCenterUi.dll     原生 DuiLib 主页，画设备卡片
       └─ AccessoriesAdapter.dll / AccessoriesManagerCenter.dll  设备发现与卡片数据
            └─ 以 JSON 命令行参数拉起
                 accessories_app/AccessoryApp/AccessoryApp.exe   WPF 宿主
                   └─ Lib/Plugins/CD54RPenApp.dll                按 modelID 选中的插件
                        └─ Lib/Plugins/Depend/PenService.dll     原生，真正读写 MCU
```

**主页卡片只显示名称和连接状态，不显示电量。** 电量、固件版本、序列号这些都在插件自己的
WPF 窗口里，那是一个独立进程，点卡片才拉起。这一点直接决定了托盘面板该对齐谁：要对齐的是
插件窗口，不是选件中心主页。

**笔和键盘的全部状态都能自取，不需要任何华为组件。** `PenService.dll` 与 `KeyboardService.dll`
只是同一条 USB 通道的薄封装，接口 GUID 与本项目 `PenEventBridge` 用的是同一个
（`{dd0ebedb-f1d6-4cfa-acca-71e66d3178ca}`），按帧头 `byte[4]` 子系统 ID 分流。选件中心插件
拿到的每一个字段，都对应一条我们已经掌握或可以照抄的 MCU 命令——**完整的命令表与事件分发表
在 4.2 节，逐字节从 `PenService.dll` 里读出**，实现时照抄即可。

**唯一依赖华为组件的是蓝牙侧。** 插件同时监听 IConnect（`IConnectClientSdk.dll` →
`HiConnectivityService.exe`）的电量与连接上报，那条路走 BLE GATT，用于蓝牙鼠标一类设备。
对磁吸键盘和 MCU 直连的笔来说这条是冗余源，可以不接。

### 我们自足就能显示的字段

| 字段 | 笔 | 键盘 | 现状 |
|---|---|---|---|
| 电量百分比（1% 粒度） | 有 | 有 | 笔已在 `PenStatusChannel` 里 |
| 充电中 | 有 | 有 | 笔已在 `PenStatusChannel` 里 |
| 连接状态 | 有 | 有 | 笔已在 `PenStatusChannel` 里 |
| 模组 ID / 型号名 | 有 | 见 3.4 | 笔已在 `PenStatusChannel` 里 |
| 吸附 / 分离状态 | 吸附于机身 | detach | 笔已有，键盘未接 |
| 固件版本 | 有 | 有 | 两边都未接 |
| 硬件版本 | 有 | 有 | 两边都未接 |
| 序列号 | 有 | 有 | 两边都未接 |
| 双击功能设置 | 有 | — | 未接 |
| 分离无线连接开关 | — | 有 | 未接，协议已逆向完毕 |

### 拿不到或需要额外逆向的

| 字段 | 情况 |
|---|---|
| 蓝牙 MAC 地址 | 设备信息页有「蓝牙地址」一栏。笔与磁吸键盘走 MCU 不走 BLE，这一栏对它们大概率为空；要填只能走 IConnect。**猜测**，未在本机验证 |
| 固件新版本 / 更新日志 | 走华为云服务（`AccessoriesUpdateCheck.dll` + 账号），不可能自取 |
| 笔尖磨损 | **选件中心根本没有这个字段。** 全量字符串表里没有任何笔尖相关文案，不要再找 |

---

## 2. 组成与启动链路

### 2.1 设备注册表

`components/accessories_center/config/accessories/accessories.xml` 是唯一的设备总表（实测）。
笔的 `productId` 是模组 ID 的十进制值，键盘和鼠标是字母型号码：

| type | productId | 名称（`res/values/zh_CN.res`） |
|---|---|---|
| pen | `49` | M-Pencil |
| pen | `282` | M-Pen 2 |
| pen | `283` | M-Pencil |
| pen | `65819` | M-Pencil |
| pen | `4468738` | HUAWEI M-Pencil 3 |
| keyboard | `RX0G` | HUAWEI 高键程智能键盘（`VID_12D1&PID_10BF`） |
| keyboard | `RX0H` | 华为智能磁吸键盘 |
| keyboard | `RX0I` | 华为智能磁吸键盘 适用于 HUAWEI MateBook E |

十进制与模组 ID 的对应：`283` = `0x00011B`（CD54）、`65819` = `0x01011B`（CD54R）、
`4468738` = `0x443002`（CD54S）、`49` = `0x000031`、`282` = `0x00011A`（CD52）。

`layout_style` 决定用哪个卡片布局：笔和 RX0G 是 `1`，RX0H / RX0I 是 `0`。
`AccessoriesCenterUi.dll` 的字符串表里四个卡片布局文件名按
`Square → Rectangle → Button → FatRectangle` 连续排列，据此推断 `0` = Square、`1` = Rectangle
（静态分析推断，未运行验证）。这与产品图形状吻合：笔是横向长条，用 88×64 的 Rectangle；
磁吸键盘接近正方，用 64×64 的 Square。

### 2.2 宿主如何拉起插件

宿主把一段 JSON 作为**命令行参数**传给 `AccessoryApp.exe`（实测，取自本机日志）：

```text
{"deviceID":"","deviceSN":"RRXEX2*****02497","modelID":"65819","subID":"00",
 "deviceType":"3","connectStatus":"2","deviceName":"M-Pencil","language":"zh-CN"}

{"deviceID":"","deviceSN":"UQNWY2*****00342","modelID":"RX0H","subID":"00",
 "deviceType":"1","connectStatus":"2","deviceName":"华为智能磁吸键盘","language":"zh-CN"}
```

`deviceType`：`1` = 磁吸键盘，`2` = RavenKBD（RX0I），`3` = 笔。`2` 的取值来自
`Window_Battery.Setting_Click` 里硬编码的那条 `ProcessStartInfo`（静态）。

`AccessoryApp.exe` 收到后按 `modelID` 反射出插件类：

```text
MainAccessoryApp.ResolveAccessoryAppInstance  Passed accessory device model is "65819"
MainAccessoryApp.ResolveAccessoryAppInstance  App full class name found: "CD54RPenApp.App"
```

注意 `Configuration/accessory_models.json` 只登记了鼠标和 CD34 键盘，笔与磁吸键盘不在其中。
它们的分发靠**程序集特性**：`AcAppPluginManager.DoResolveAccessoryPlugins` 扫描
`Lib/Plugins/*.dll`，读每个程序集上的 `AccessoryModelInfoAttribute`
（`assembly.GetCustomAttributes(typeof(AccessoryModelInfoAttribute), false)`），
用 `modelId` 建索引（静态）。全部插件的声明如下：

| 插件 DLL | modelName | modelId | subModelId | accessoryType |
|---|---|---|---|---|
| `CD52PenApp` | M Pencil | `49` | `000` | CD52 |
| `AlitaPenApp` | M Pen2 | `282` | `000` | Alita |
| `CD54PenApp` | M Pencil | `283` | `000` | CD54 |
| `CD54RPenApp` | M Pencil | `65819` | `000` | CD54R |
| `CD54SPenApp` | M Pencil | `4468738` | `000` | CD54S |
| `GaokunKeyboardApp` | HUAWEISmartMagneticKeyboard | `RX0H` | `000` | 标准键盘 |
| `DiracRKeyboardApp` | HUAWEISmartMagneticKeyboard | `RX0I` | `000` | 标准键盘 |

所以 **`GaokunKeyboardApp` 对应 RX0H，`DiracRKeyboardApp` 对应 RX0I**，不需要猜。
`accessory_models.json` 不要去改，它管的是另一条（鼠标）路径。

### 2.3 与我们的服务抢端点

`PenService.dll`、`KeyboardService.dll` 和本项目的 `PenEventBridge` 打开的是同一个 device path，
各自 `ReadFile` 0x40 字节。USB 中断管道的一个包只交付给一个读者，谁抢到算谁的。

本机当前 `AcAppDaemon` 未运行、`HuaweiThpService` 已禁用、`OpenEGoHubServiceDebug` 在跑（实测），
所以端点归我们。但如果用户同时开着 PCManager 的选件中心，两边会互相吃包。这一点在
`docs/KBDMCU_PROTOCOL.md` 第 6.3 节已有结论，不重复。

---

## 3. 展示了哪些状态

### 3.1 主页卡片（原生 DuiLib）

布局文件 `components/accessories_center/res/layout/AccessoriesCenter*Card.xml`。四种卡片
骨架相同，只有产品图宽度和右下角控件不同：

| 布局 | 产品图 | 右下角 |
|---|---|---|
| `SquareCard` | 64×64 | 状态图标，可带「共享」角标 |
| `RectangleCard` | 88×64 | 状态图标 |
| `FatRectangleCard` | 120×64 | 开关按钮（`switch_off.png`） |
| `ButtonCard` | 64×64 | 断开连接按钮 |

卡片上只有四个可变元素：主标题（设备名）、副标题、产品图、状态图标。副标题能取到的文案，
全量列在 `res/values/zh_CN.res` 里（实测，用 ICU ResB 解析器逐条读出）：

```text
IDS_ACCESSORIES_CONNECTED       已连接
IDS_ACCESSORIES_UNCONNECT       未连接
IDS_ACCESSORIES_CONNECTING      连接中
IDS_ACCESSORIES_OFFLINE         离线
IDS_ACCESSORIES_CONNECT_LOCAL   本地连接
IDS_ACCESSORIES_CONNECT_ONLINE  在线
IDS_ACCESSORIES_CONNECT_TO      已连接 {0}
```

**整张字符串表里没有任何电量文案。** 主页卡片不显示电量，这是确定的。

### 3.2 插件主窗口（WPF）

`CD54RPenApp` 的 `views/window_main.baml`。绑定到这些属性（静态，来自 BAML 字符串表与
`PenViewModel.cs`）：

| 绑定 | 含义 |
|---|---|
| `ShowPenIdPic` | 按模组 ID 选的产品图 |
| `ShowBatteryPic` | 电量图标，`GetBatteryImage(电量, 是否充电)` |
| `PenBatteryValue` | 电量数值 |
| `BatteryPercent` | 电量百分比文案 |
| `PenFuncSelectIndex` | 双击功能下拉框 |

样式名 `BatteryPercentStyle` / `BatteryValueStyle`。

设置项（来自 `resources/lang/zh-cn.baml` 与 `en-us.baml`，实测解出键与值）：

| 资源键 | 中文 | 英文 |
|---|---|---|
| `IDS_PENAPP_BASIC_SETTINGS` | 基础设置 | Basic settings |
| `IDS_PENAPP_HARDWARE_NUMBER` | 硬件版本 | Hardware version |
| `IDS_PENAPP_FIRMWARE_UPDATE` | 固件更新 | Firmware update |
| `IDS_PENAPP_WRITING_HANDS` | 书写常用手 | Write with |
| `IDS_PENAPP_LEFT_HAND` / `RIGHT_HAND` | 左手 / 右手 | Left hand / Right hand |
| `IDS_PENAPP_IGNORE_TOUCH` | 使用手写笔时忽略触摸 | Ignore touch |
| `IDS_PENAPP_CURSOR` | 显示光标 | Show cursor |
| `IDS_PENAPP_VISUAL_EFFECTS` | 显示视觉效果 | Visual effects |
| `IDS_PENAPP_DOUBLE_CLICK` | 双击功能 | Double-tap |
| `IDS_PENAPP_TOGGLE_ERASER` | 切换橡皮擦 | Switch to eraser |
| `IDS_PENAPP_GLOBAL_ANNOTATION` | 打开全局批注 | Annotate |
| `IDS_PENAPP_SCREENSHOT` | 截屏 | Take screenshot |
| `IDS_PENAPP_WAKE_VOICE` | 打开智慧语音功能 | Wake voice assistant |
| `IDS_PENAPP_GLOBAL_SHORTCUT_MENU` | 全局快捷菜单 | Shortcut menu |
| `IDS_PENAPP_ONE_CLICK` | 一键摘录 | Take a snippet |

双击功能的取值来自 `CustomApplicationContext.PenKeyFunc`（静态）：

```text
0 PRINT_SCREEN          截屏
1 OPEN_SMART_VOICE_PTZ  智慧语音
2 OPEN_WHITE_BOARD      白板
3 FUNC_CLOSE            关闭
4 FUNC_ERASER           切换橡皮擦
5 GLOBAL_ANNOTATION     全局批注（本机实测当前值）
```

### 3.3 设备信息页

`views/pages/deviceinfopage.baml` 里的 `BatteryInfoCard`，字段名来自宿主的资源程序集
`Lib/Lang/zh-CN/AccessoryApp.resources.dll`（实测，键表与值表按文件顺序一一对齐）：

| 资源键 | 中文 |
|---|---|
| `gbl_device_info_f_name` | 设备名称 |
| `gbl_device_info_f_model_id` | 型号 |
| `gbl_device_info_f_sn` | 序列号 |
| `gbl_device_info_f_bt_mac` | 蓝牙地址 |
| `gbl_device_info_f_fw_revision` | 固件版本 |
| `gbl_device_info_current_version` | 当前版本 |
| `gbl_device_info_new_version` | 新版本 |
| `gbl_device_info_f_battery_level` | 电池电量 |
| `gbl_device_info_f_battery_status` | 电池状态 |
| `gbl_device_info_f_battery_status_charging` | 正在充电 |
| `gbl_device_info_f_battery_status_consuming` | 正在耗电 |
| `gbl_battery_lvl_left` | 剩余电量 |
| `gbl_conn_status_connected` / `_disconnected` | 已连接 / 未连接 |
| `gbl_no_vendor_or_version` | 当前未查询到设备厂商和版本信息 |

这一页就是托盘设备面板最直接的参照：**设备名、型号、序列号、固件版本、电量、电池状态**。

页面分三张卡（同样从 BAML 字符串表扫出，属性归属按相邻顺序推断）：

| 卡片 | 内含字段 |
|---|---|
| `DeviceNameCard` | `gbl_device_info_f_name`（值控件 `LblDeviceName`） |
| `BatteryInfoCard` | `gbl_device_info_f_battery_status`（`LblBatteryStatus`）、`gbl_device_info_f_battery_level`（`LblBatteryPower`，可见性由 `LblBatteryPowerVisibility` 控制） |
| `FirmInfoCard` | `gbl_device_info_f_model_id`、`gbl_device_info_f_fw_revision`、`IDS_PENAPP_HARDWARE_NUMBER`、`gbl_device_info_f_sn` |

每张卡的排版一致，这是最值得照抄的一段：

```text
卡片      圆角 16   描边 #6A6A6A   Margin 0,16,0,0
每行      高 48     Padding 0,4
左侧标签  Margin 15,0,8,0   左对齐  垂直居中  字号 14  CharacterEllipsis
右侧取值  Margin 8,0,15,0   右对齐  垂直居中  字号 14  CharacterEllipsis
行分隔线  Margin 15,0,15,0  顶部    #000000  Opacity 0.15
```

标签文字被截断时会弹 `CustomToolTip` 显示全文，判定用
`TrimmedTextTipVisibilityConverter`；数字按区域设置格式化用 `NumberLocaleConverter`；
RTL 语言用 `FlowDirectionValueConverter`。控件是自定义的 `CommonAcTextBlock`。
全局样式在 `AccessoryApp;Component/AppFWK/Themes/G_Window_Styles.xaml`。

### 3.4 键盘侧

键盘的字段来自 `KbdResidentProc` 的回调，本机实测全部出现过：

| 回调 | 实测样本 |
|---|---|
| `CallbackUpdateKbdConnectStatus` | `1` / `0` |
| `CallbackKbdBatteryVolumeEvent` | 1% 粒度，观测到 74–99 |
| `CallbackKbdChargingStatusEvent` | `1` / `0`，另打印 `Get kbd modelId charging status:RX0H` |
| `CallbackUpdateDetachStatus` | `1` / `0` |
| `CallbackUpdateKbdSerialNo` | `UQNWY2*****00342` |
| `CallbackUpdateKbdHardwareVersion` | `C5210_1`，长度 7 |
| `CallbackUpdateKbdFirmwareVersion` | `GAOKUN_KBD_BD 1.0.0.39`，长度 22 |
| `CallbackUpdateKbdModule` | **`0`** |

**模组 ID 这一路对键盘是空的。** `CallbackUpdateKbdModule` 收到的是一个整数，代码里
`CustomApplicationContext.ModuleId = moduleID.ToString()`，本机实测这个整数是 `0`。
而宿主传进来的 `modelID` 是字符串 `RX0H`。两者不同源：

- `RX0H` 来自宿主的 JSON 启动参数，宿主又是从原生的
  `AccessoriesAdapterSdk::AccessoryKeyboard` 拿到的。**这一步的具体取值路径本轮没有定论**
  （原生代码里没有留下可判读的字符串线索）。
- MCU 的 `0x00` 模组查询对这块键盘返回 `0`，用不上。

`KeyboardViewModel` 里有一段 `if (mModuleId != CustomApplicationContext.ModuleId)` 的兜底，
用来在 MCU 报回不同模组时换图；本机因为 MCU 报 `0`，这条永远不成立，界面一直用启动参数里
的 `RX0H`（静态推断，与实测日志一致）。

厂商与型号另有一条从版本串推断的路：`CustomApplicationContext.GetKeyboardVendorByFwVersion`
对固件版本串做 `Contains`（本机 `GAOKUN_KBD_BD` → Gaokun）；笔那边对称地有
`GetTypeByVersion`，匹配列表是 `["CD52", "Alita", "CD54L", "CD54"]`（静态）。

`GaokunKeyboardApp` 内部还区分两种键盘形态（静态，常量直接读出）：

| 常量 | 名称 | 模组 | 图片 | 类型 |
|---|---|---|---|---|
| `MAGIC_*` | `HUAWEI Smart Magnetic Keyboard` | `RX0H` | `GlideKbd.png` | 标准键盘 |
| `GLIDE_*` | `HUAWEIGlideKeyboard` | `RX04` | `SmartKbd.png` | 创新键盘 |

分离状态用两张图切换：`DETACH_PIC = Detach.png`（已分离）、
`UNDETACH_PIC = Snapping.png`（已吸附）。

键盘专有的两条设置（`resources/lang/*.baml`，实测）：

```text
kbd_detachment / IDS_KEYBOARDAPP_...   键盘分离 / Free keyboard
    开启后可在键盘与设备分离时继续使用；关闭可避免误触
gbl_..._battery_life_extension          键盘内置大电池，可为主机供电
```

「键盘分离」就是 `docs/KBDMCU_PROTOCOL.md` 里逆向完毕的 `0x34`（Set）/ `0x35`（Get）开关。

### 3.5 浮窗

三个独立的提示窗（静态，来自各自的 code-behind）：

| 窗口 | 用途 |
|---|---|
| `Window_Battery` | **键盘**电量提示，不是笔的 |
| `Window_KbdLowBattery` | 键盘低电量 |
| `Window_PenLowBattery` | 笔低电量 |

三者的定位与生命周期完全一致：

```text
Left = SystemParameters.WorkArea.Width  - Width  - 15
Top  = SystemParameters.WorkArea.Height - Height - 15
DispatcherTimer 5 秒后自动 Close
```

`Window_Battery` 的文案是 `string.Format(IDS_KEYBOARDAPP_CURRENTPOWER, 电量, 预计小时数)`，
预计小时数的算法是（静态，逐条抄自反编译代码）：

```text
电量 <= 6  ->  0
电量 == 7  ->  1.0
电量 == 8  ->  1.4
其余       ->  电量 * 0.2
```

低电量提示用 `IDS_KEYBOARDAPP_REMAININGPOWER`（`剩余电量 {0}%` / `{0}% battery remaining`）。

是否弹窗受注册表开关控制：`HKCU\Software\Huawei\PCManager\AccessoriesCenter\kbdSetting`
的 `remindEnable`（本机实测值 `True`）。

---

## 4. 数据从哪里来

### 4.1 两条并行的源

```text
                        ┌─ MCU 通道（USB interrupt，我们已经拥有）
插件 ViewModel  ────────┤
                        └─ IConnect（BLE GATT，华为组件）
```

**MCU 通道**：`CD54PenApp.McuInteractTool` 全量 P/Invoke 到 `Depend\PenService.dll`
（cdecl），键盘侧对称地走 `KeyboardService.dll`。模式是「注册回调 + 发查询命令」：

```csharp
[DllImport("Depend\\PenService.dll", CallingConvention = CallingConvention.Cdecl)]
internal static extern void RegisterCallBackUpdateBatteryVolume(CallBackFunc func);

[DllImport("Depend\\PenService.dll", CallingConvention = CallingConvention.Cdecl)]
internal static extern void CommandSendGetPenBattery();
```

`PenService.dll` 的完整 P/Invoke 面（静态，来自 `McuInteractTool.cs`）：

```text
线程控制    GetInterruptPipeMsg  ProcPipeMsg  StopProcPipeMsg  StopLoop
标识        CommandSendGetPenModule / SerialNo / HardwareVersion / FirmwareVersion
            对应 GetPenModule / GetPenSerialNo / GetPenHardwareVersion / GetPenFirmwareVersion
电源        CommandSendGetPenBattery  CommandSendGetPenChargingStatus
连接        CommandSendGetPenConnectStatus  CommandSendNewConnectRespond
按键        CommandSendGetPenKeySupport  CommandSendGetPenKeyFunc  CommandSendSetPenKeyFunc
模式        CommandSendTransferPenMode  CommandSendOskPrevensionMode  CommandSendPenCurrentFunc
杂项        CommandSendSysLang  CommandSendTouchChr  CommandSendGlobalPreventionChr
            CommandSendDoubleFuncChr  RetriveMcuSwVersion  GetMcuVersion
固件        FwUpdateStart(devType, binPath)  FwUpdateStop(devType)
日志        RegisterLogFunc / UnRegisterLogFunc
```

每个 `CommandSend*` 都有一个成对的 `RegisterCallBack*`。这与 `KeyboardService.dll` 的结构
逐条对称——`docs/KBDMCU_PROTOCOL.md` 已经证明两份 DLL 是同一套代码模板的两份实例，打开的是
同一个 USB 接口，靠帧头 `byte[4]` 子系统 ID 分流（笔 `0x01`，键盘 `0x02`，分离开关 `0x00`）。

**结论：选件中心笔侧和键盘侧显示的每一个字段，都能用我们已有的 USB 通道自取。**

### 4.2 笔子系统的完整命令与事件表

下面两张表是从 `PenService.dll` 的机器码里逐条读出来的（静态，但精确到字节）。方法与
`docs/KBDMCU_PROTOCOL.md` 第 8 节相同：发送侧的帧头是两条
`mov dword ptr [rsp+disp], imm32` 拼在栈上，按 `disp` / `disp+4` 配对还原；接收侧的分发表是
16 字节一条的 `(事件码, 函数指针)` 数组，处理函数尾调用监听对象的某个槽位，而每个
`RegisterCallBack*` 导出恰好写一个槽，两边按槽位偏移 join。

**Host → MCU**（全部是 `07 00 02 00 01 <CMD> 11 <LEN>`，即目标 `0x07`、子系统 `0x01`）：

| 导出 | RVA | `byte[5]` | payload |
|---|---|---:|---:|
| `CommandSendGetPenModule` | `0x094b0` | `0x00` | 0 |
| `CommandSendGetPenSerialNo` | `0x09710` | `0x01` | 0 |
| `CommandSendGetPenHardwareVersion` | `0x09940` | `0x02` | 0 |
| `CommandSendGetPenFirmwareVersion` | `0x09b50` | `0x03` | 0 |
| `CommandSendGetPenBattery` | `0x09d60` | `0x08` | 0 |
| `CommandSendGetPenChargingStatus` | `0x09e00` | `0x09` | 0 |
| `CommandSendGetPenConnectStatus` | `0x09db0` | `0x12` | 0 |
| `CommandSendNewConnectRespond` | `0x09ea0` | `0x15` | 1 |
| `CommandSendTransferPenMode` | `0x09500` | `0x24` | 1 |
| `CommandSendGetPenKeySupport` | `0x09f50` | `0x25` | 0 |
| `CommandSendSetPenKeyFunc` | `0x09fc0` | `0x26` | 1 |
| `CommandSendGetPenKeyFunc` | `0x0a020` | `0x27` | 0 |
| `CommandSendPenCloseConnectWindow` | `0x0a090` | `0x29` | 0 |
| `CommandSendOskPrevensionMode` | `0x0a160` | `0x2D` | 1 |
| `CommandSendTpRotateAngle` | `0x0a1e0` | `0x67` | 1 |
| `CommandSendSysLang` | `0x0aaa0` | `0x68` | 1 |
| `CommandSendPenCurrentFunc` | `0x0a9b0` | `0x2F` | 1 |
| `CommandSendTouchChr` | `0x0a9f0` | `0x80` | 4 |
| `CommandSendGlobalPreventionChr` | `0x0aa20` | `0x81` | 4 |
| `CommandSendDoubleFuncChr` | `0x0aa50` | `0x82` | 0 |

前七条与 `KeyboardService.dll` 的码值一一相同（`0x00` 模组、`0x01` SN、`0x02` 硬件版本、
`0x03` 固件版本、`0x08` 电量、`0x09` 充电、`0x12` 连接），只是子系统 ID 不同。这再次印证
两份 DLL 同源。

每条命令的帧头、payload 长度和参数处理见 `docs/penservice_events.md` 第 1 节，那里按导出
逐条列出，并标出哪几条的参数会被布尔化。

**自校验：请求的 `byte[5]` 与应答事件码同值。** 上表二十条命令里，`0x00`、`0x01`、`0x02`、
`0x03`、`0x08`、`0x09`、`0x12`、`0x15`、`0x24`、`0x25`、`0x26`、`0x27`、`0x29`、`0x2D`、
`0x2F`、`0x67`、`0x68` 都能在下面的分发表里找到同码事件。剩下的 `0x80` / `0x81` / `0x82`
三条 Chr 命令不在分发表里，是只发不收的下行命令。往后再往这张表里加行，先用这条规律核一遍。

### 4.2.1 最后四行曾经整体错位一格

这四行原先记为 `CommandSendPenCurrentFunc = 0x80 / payload 4`、`CommandSendTouchChr = 0x81`、
`CommandSendGlobalPreventionChr = 0x82 或 0x81`、`CommandSendDoubleFuncChr = 0x82`，即整体向
后挪了一格，同时 payload 长度也跟着错。重新逐字节反汇编后订正如上。

错位是靠上面那条自校验规律发现的：`CommandSendPenCurrentFunc` 记成 `0x80` 时，分发表里没有
`0x80`，而 `0x2F` 这个事件却找不到对应的发送命令，两头都对不上。订正后 `0x2F` 发出、`0x2F`
收回，缺口消失。`CommandSendGlobalPreventionChr` 那一格「`0x82` / `0x81`，0 / 4」的两可写法
也是错位留下的痕迹——真值只有一个，`0x81` 配 4 字节 payload。

**MCU → Host 分发表**（`.data` RVA `0x14130`，21 条，每条 16 字节）：

| `byte[5]` | handler RVA | 回调 |
|---:|---|---|
| `0x00` | `0x0a220` | `UpdatePenModule` |
| `0x01` | `0x0a280` | `UpdatePenSerialNo` |
| `0x02` | `0x0a460` | `UpdatePenHardwareVersion` |
| `0x03` | `0x0a640` | `UpdatePenFirmwareVersion` |
| `0x08` | `0x0a820` | `UpdateBatteryVolume` |
| `0x09` | `0x0a900` | `UpdatePenChargingStatus` |
| `0x12` | `0x0a890` | `UpdatePenConnectStatus` |
| `0x15` | `0x0aae0` | `NewPenConnectRequest` |
| `0x16` | `0x0ab60` | `NewPenConnectResult` |
| `0x24` | `0x0aba0` | `TransferPenMode` |
| `0x25` | `0x0ac20` | `UpdatePenKeySupport` |
| `0x26` | `0x0ac60` | `UpdatePenKeyFuncSet` |
| `0x27` | `0x0aca0` | `UpdatePenKeyFuncGet` |
| `0x28` | `0x0ace0` | `PenTopBatteryWindow` |
| `0x29` | `0x0ad10` | `PenCloseConnectWindow` |
| `0x2A` | `0x0ad40` | `PenDeviationReminder` |
| `0x2C` | `0x0ad70` | `PenFirstBatAfterConn` |
| `0x2D` | `0x0abe0` | `PenOskPrevensionMode` |
| `0x2F` | `0x0ae20` | `PenCurrentFunc` |
| `0x67` | `0x0ada0` | `TpRotateAngle` |
| `0x68` | `0x0ade0` | `SysLang` |

读线程的校验只有一条（静态，`GetInterruptPipeMsg @ RVA 0x07da0` 里唯一的 `cmp byte` 立即数
比较在 `0x00818d`）：

```text
cmp byte ptr [rbp+0x04], 0x01      即 packet[4] == 0x01
```

**它不校验 `byte[0]`、`byte[2]`、`byte[6]`**，过了这一条就拿 `byte[5]` 去查上面的分发表。
`KeyboardService.dll` 的同名函数里对应位置是 `cmp byte ptr [rbp+0x05], 0x35`，即
`docs/KBDMCU_PROTOCOL.md` 描述的那条 `byte[4] == 0x00 && byte[5] == 0x35` 内联特判。

我们的 `TryParsePenUsbEventFrame()` 额外校验了 `packet[2] == 0x07`，比原厂更严。这在只收笔帧
时无害，但按 `packet[4]` 分流接入键盘时必须放开——键盘帧的 `packet[2]` 不是 `0x07`。

监听对象的槽位布局（`RegisterCallBack*` 各写一个）：

```text
+0x100 RegisterCallBack（通用）          +0x158 UpdatePenKeySupport
+0x108 UpdateBatteryVolume               +0x160 UpdatePenKeyFuncSet
+0x110 UpdatePenConnectStatus            +0x168 UpdatePenKeyFuncGet
+0x118 UpdatePenChargingStatus           +0x170 PenTopBatteryWindow
+0x120 UpdatePenModule                   +0x178 PenCloseConnectWindow
+0x128 UpdatePenSerialNo                 +0x180 PenDeviationReminder
+0x130 UpdatePenHardwareVersion          +0x188 PenFirstBatAfterConn
+0x138 UpdatePenFirmwareVersion          +0x190 PenOskPrevensionMode
+0x140 NewPenConnectRequest              +0x198 TpRotateAngle
+0x148 NewPenConnectResult               +0x1a0 SysLang
+0x150 TransferPenMode                   +0x1a8 PenUpdateRessult
                                         +0x1b0 PenCurrentFunc
```

### 4.3 对 `BTMCU_PROTOCOL.md` 的三点修正

那份文档的事件表把两个来源混在了一起：`0x70`–`0x7F` 一段来自 `THP_Service.dll`（触控服务），
`0x0X`/`0x2X` 一段其实就是上面这张 `PenService.dll` 分发表。据此可以修正：

- **`0x28` 不再是 Open。** 它是 `PenTopBatteryWindow`——请求把电量浮窗置顶。
- **`0x12` 是连接状态，不是配对状态。** `PenService.dll` 把它绑到
  `RegisterCallBackUpdatePenConnectStatus`，而 `BTMCU_PROTOCOL.md` 记为 `DEV_PAIR_STATUS`。
  发送侧的 `CommandSendGetPenConnectStatus` 用的也是 `0x12`，两处互证。原文档的
  「`0x12 DEV_PAIR_STATUS` gap」这一条应当改成连接状态。
- **`0x2A` 此前未记录**，`PenService.dll` 把它命名为 `PenDeviationReminder`（笔吸附偏移提醒）。
  但这只证明协议槽位存在：当前安装版 PenApp 没有注册该回调，不能据此把它标成实机已触发。

另有 `0x10`、`0x21`、`0x23`、`0x2E` 出现在 `BTMCU_PROTOCOL.md` 里但**不在** `PenService.dll`
的分发表中——它们只被 `THP_Service.dll` 处理。两个宿主订阅同一子系统的不同码段。

### 4.4 IConnect 通道（依赖华为组件）

宿主 `AccessoriesAdapter.dll` 调
`IConnectClientProxy::CreateIConnectClientProxy`（`IConnectClientSdk.dll`），注册三个回调：

```text
StartIcconnect(
    function<void(const DeviceBaseInfo&, EventType)>,   设备发现/状态
    function<void(const DeviceInfo&)>,                  设备信息
    function<void(const DevBatteryInfo&)>)              电量
```

插件侧对应 `BaseDeviceViewModel.UpdateBatteryInfoByIConnect`，本机实测打出过：

```text
IConnect report battery value of device 28************56 is 100, battery charge status is False
```

底层是 `AccessoryMgmtService.dll`，导入 `HiConnectivitySDK.dll` 的 BLE GATT 客户端
（`BleGattClientConnect` / `ReadCharacteristic` / `RegisterNotification`）加 `HID.DLL`、
`SETUPAPI.dll`、`bthprops.cpl`。这条路服务的是蓝牙鼠标一类设备，对 MCU 直连的笔和磁吸键盘
是冗余的第二源。

另有 `McuDevConnectService.dll`（`AccessoryMgmtService::McuIConnectDevMgr`），它走串口
API（`SetCommState` / `BuildCommDCBW` / `EscapeCommFunction`）而非 HID，配套配置文件
`McuDevConnectService-SYSTEM.config.xml` 里存着 `PenMacAddr`。这是 MCU 侧的 BLE 配对管理，
与状态展示无关。

### 4.5 不是这些

排查过并排除（实测）：

- **不走注册表存状态。** `HKCU\...\AccessoriesCenter\DeviceMonitor` 是空的；同级只有
  `CardNumber`（本机 `AccessoryApp = 2`，即两张卡片）、`AccessoryApp\IConnect_Model_ID`、
  `kbdSetting\remindEnable`。
- **不走 SQLite 存状态。** `accessory_infos.db` 的 `t_AccessoryDevice` 等六张表在本机全为空，
  schema 是鼠标导向的（`acy_dev_battery`、`acy_bt_addr`、`acy_fw_rev_id`）。
- **不走 WMI 取笔键盘状态。** `AccessoriesAdapter.dll` 里确有 `ROOT\WMI` / `ROOT\CIMV2`
  字符串，但 `WmiUtil.dll` 的用途是 USB 设备枚举，不是状态。

---

## 5. 推送机制

**全部是回调，不是轮询。**

`PenService.dll` / `KeyboardService.dll` 各起一个读线程（`GetInterruptPipeMsg`）和一个分发
线程（`ProcPipeMsg`），读线程阻塞在 `ReadFile` 上，收到包后按 `byte[5]` 查分发表，调用托管侧
注册的委托。没有定时查询。

日志里唯一的周期性定时器是 `PenMistouchPreventionProc.MonitorTimerTick`，**实测周期 12 分钟**
（`18:43:43 → 18:55:43 → 19:07:43`），它做的是防误触状态巡检，不是状态轮询。

启动时会主动发一轮 `CommandSendGet*` 把当前值取回来，之后靠 MCU 主动推送。

`CallbackUpdatePenModule` 在本机日志里出现了 25274 次，远多于其他回调——说明模组 ID 这条被
高频重复上报（或被插件反复查询）。**猜测**是连接保活的副产品，未确认。

---

## 6. 模组 ID 与资源命名

### 6.1 卡片图

`components/accessories_center/res/drawable/cards/<modelID>_<subID>.png`（实测）：

| 文件 | 画布 | 内容包围盒 |
|---|---|---|
| `283_00.png` / `65819_00.png` / `4468738_00.png` / `49_00.png` | 240×128 | 约 216×12，横向笔身 |
| `282_00.png` | 240×128 | 216×14 |
| `RX0H_00.png` / `RX0I_00.png` | 128×128 | 104×86 |
| `RX0G.png` / `RX0G_04.png` … | 132×96 | 约 120×45 |

笔的卡片图四周是大片透明，直接缩放会把笔缩成一根头发——托盘代码里已有的 `CropToContent`
正是为此。

### 6.2 插件 DLL 里的高分辨率图

`Lib/Plugins/<型号>PenApp.dll` 的 WPF 资源流（实测，尺寸用 Pillow 量出）：

| 资源名 | 尺寸 | 内容 | 用途 |
|---|---|---|---|
| `resources/<型号>_mainpage.png` | 704×704 | 236×517 | 竖立产品照，插件主窗口 |
| `resources/pic_%08x_connect@1.5x.png` | 528×96 | 436×26 | 横向笔身 |
| `resources/pic_%08x_setting@1.5x.png` | 600×96 | 540×32 | 横向笔身，带按键标注 |
| `resources/pic_vector_connect@1.5x.png` | 528×96 | 436×29 | 通用矢量图，各插件都带 |
| `resources/sstylus-vector.png` | 1120×176 | 755×49 | 大尺寸矢量笔 |

`pic_` 后面的 8 位十六进制是模组 ID 左移 8 位：`pic_00011b00` 对应 `0x00011B`。

**`CD54RPenApp.dll` 里没有 `pic_01011b00_*`。** 它只带 `00003100` / `00011a00` / `00011b00`
三套，CD54R 自己的产品照是 `cd54r_mainpage.png`。托盘现有代码退到 CD54 那张银色图是对的。

按模组 ID 选主窗口产品图的映射表在 `CustomApplicationContext.picIndex`（静态）：

```csharp
{ "282", ".../Resources/Alita_MainPage.png" },
{ "283", ".../Resources/CD54_MainPage.png" },
{ "49",  ".../Resources/CD52_MainPage.png" },
```

对应的名字表 `nameIndex`：`282` → `M-Pen 2`，`283` / `49` → `M-Pencil`。

### 6.3 三个 CD54 插件的分工

反编译后按文件逐个比对（静态）：

| 插件 | 源文件数 | 角色 |
|---|---:|---|
| `CD54PenApp` | 39 | **常驻插件**，`AcAppDaemon.exe` 加载的就是它 |
| `CD54RPenApp` | 22 | 纯设置界面 |
| `CD54SPenApp` | 22 | 纯设置界面 |

`CD54PenApp` 比另外两个多出来的 17 个文件，全部是「常驻」职责：键盘相关的
`KbdResidentProc` / `KbdFuncInterface` / `KbdApartViewModel` / `KbdBatteryViewModel` /
`Window_Battery` / `Window_KbdLowBattery` / `Window_RavenB_Kbd`，防误触的
`PenMistouchPreventionProc`，守护的 `PenMonitorDaemon`，以及低电量浮窗、焦点检测、
屏幕方向、大数据上报。

**所以键盘的常驻监听是搭在 `CD54PenApp` 里的，跟本机装的是哪支笔无关。** 本机日志正好印证：
`AcAppDaemon` 进程里跑的是 `PenMistouchPreventionProc` 和 `KbdResidentProc`，而 UI 进程
`AccessoryApp` 解析出的类是 `CD54RPenApp.App`（实测）。

`CD54RPenApp` 与 `CD54SPenApp` 逐行比对只差两处：

- `CustomApplicationContext`：CD54S 多了 `CD54S_MODULE_ID = "4468738"`、
  `PEN_NAME_CD54S = "HUAWEI M-Pencil 3"`，名字表里多一条 `4468738 → HUAWEI M-Pencil 3`，
  并在固件版本匹配里多一个 `fwVersion.Contains("CD54S")` 分支。
- `CustomerNotification`：CD54R 留有 `PenDeviationReminderNotification()`，CD54S 没有。
  对全部已安装 PenApp 的 IL 复核表明，没有代码注册
  `RegisterCallbackPenDeviationReminder`；CD54/CD54R 的该方法也只写一条 Start 日志后返回，
  不创建窗口或通知。因此这是当前版本中的遗留/未接通路径，不能当作可工作的原厂实现。

两者的资源文件集合完全相同。

### 6.4 电量图标的分档规则

**这里和托盘现有实现不一致，是一个要修的 bug。**

档位集合两边一样，12 档：`{5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 95, 100}`。

华为的映射（静态，`CustomApplicationContext.GetBatteryImage`，逐条抄出）：

```text
0..5    -> 5
6..10   -> 10
11..89  -> ceil(电量 / 10) * 10       向上取整
90..94  -> 90
95..99  -> 95
100     -> 100
```

托盘 `EGoTouchTray.cpp` 的 `BatteryStepFor()` 取的是「不大于当前电量的最大一档」，即**向下
取整**。电量 15 时华为显示 20 的图标，托盘显示 10 的图标。要对齐观感就得改成向上取整。

两套图标资源并存，用途不同：

| 路径 | 尺寸 | 命名 |
|---|---|---|
| `res/drawable/iconnect/commonResources/discover/battery/` | 54×26 | `battery_white<档位>[_charge].png` |
| 插件 DLL 内 `Resources/normal|charge/48px/@1.5x/` | 未量 | `ic_handwriting_battery <档位>%[_charge_superquick_figure].png` |

托盘现在用的是第一套（散装文件，一定读得到）；插件自己用的是第二套（嵌在 DLL 里）。

---

## 7. 界面规范

### 7.1 主页卡片（原生，可直接照抄数值）

全部来自 `res/layout/AccessoriesCenterRectangleCard.xml`（实测）：

```text
卡片外框    248 × 172         阴影层，不响应鼠标
内容区      padding 8,8,8,8   圆角 32，描边 1px #33000000，填充 #FFFCFCFC
点击区      232 × 156         偏移 (8, 8)
主标题      184 × 28  位置 (32, 32)   字号档 17    #E6000000  单行省略
副标题      184 × 20  位置 (32, 60)   字号档 117   #99000000  支持 HTML 富文本
产品图      88 × 64   位置 (32, 84)
状态图标    40 × 40   位置 (176, 100)
新品角标    6 × 6     主标题右侧
```

`SquareCard` 只把产品图改成 64×64、主标题色改成 `#FF191919`；`FatRectangleCard` 产品图
120×64、右下角换成开关按钮。

主页容器（`AccessoriesCenterMainPage.xml`）：

```text
标题        "我的设备"  544 × 48   字号档 20   #E6000000
副标题      544 × 24    字号档 4    #FF999999
卡片区      760 × 388   padding 32,44,32,24   三列平铺，行高 172
分组标题    728 × 20    字号档 102  #FF191919
底部链接    字号档 11    #FF0A59F7   圆角 40 的 hover 底
提示气泡    圆角 36     底 #FF4D4D4D  字 #FFFFFFFF  字号档 14
```

主色 `#FF0A59F7`（华为蓝）。深色态下托盘代码里用的是 `#317AF7`，与这里的浅色态成对。

### 7.2 插件浮窗

见 3.5：右下角锚定，边距 15px，5 秒自动关闭。悬停只改一张关闭图标的不透明度
（窗口悬停 0.6，图标悬停 0.9 且背景 0.05，按下背景 0.1，离开归 0）。窗口高度随文案
`TextBlock` 的实际高度动态增加。

### 7.3 插件主窗口

下面的数值从 `views/window_main.baml` 的字符串表里扫出来（BAML 把 XAML 的属性值按流顺序存成
长度前缀 UTF-8）。**属性名与数值的对应关系是按相邻顺序推断的（静态分析推断）**，量级和结构
可信，个别归属可能有偏差；要逐字确认得做完整 BAML 反编译。

窗口基类是 `Com.Huawei.Accessory.AccessoryFWK.Views.BasicAppWindow`，模板在
`pack://application:,,,/AccessoryApp;Component/AccessoryFWK/Views/BasicAppWinTpl.xaml`，
`BackGroundType = Custom`，宽高绑定到设置项 `WinWidth` / `WinHeight`。

```text
内容区      两列 Grid，352 + 352 = 704
产品图      ImgDevicePic，居中，Margin 24,0，绑定 ShowPenIdPic
信息卡      DeviceInfoCard / DeviceInfoCard2 / DeviceInfoCard3
            352 × 72（DeviceInfoCard2 另有 104 高的形态）
            圆角 16，描边 #6A6A6A
            DeviceInfoCard2 的 Opacity 绑定 DeviceInfoCard2Opacity
```

信息卡内的一行（连接状态 + 电量）：

```text
LblDeviceConnectStatus   左对齐  Margin 16,0,0,0   垂直居中  CharacterEllipsis
batteryImage             右对齐  Margin 51,0       RenderTransformOrigin 0.5,0.5
                         ScaleX/ScaleY 0.5         绑定 ShowBatteryPic
batteryValText           右对齐  Margin 26,0       绑定 PenBatteryValue
batteryPercentText       右对齐  Margin 16,19      绑定 BatteryPercent
DoubleClickCombo         246 × 40                  ComboBoxStyle
```

文本样式三档（`CardTextNameStyle` / `BatteryPercentStyle` / `BatteryValueStyle` 对应的
`Property`/`Value` 组）：

```text
字号 18  Bold     行高 36  居中   前景 #000000  Opacity 0.9   Margin 0,8
字号 14  Regular  行高 36  居中   前景 #000000  Opacity 0.9   Margin 0,8
字号 10  Regular  底对齐          前景 #000000  Opacity 0.9   Margin 0,8
```

其他出现的颜色：按钮 hover/pressed 用 `#FFFFFF`，描边 `#6A6A6A`，一处强调色 `#43a9c7`，
下拉框 `#000000` 文字配 `#ffffff` 底。下拉弹层用 `DropShadowEffect`。

对比参考：选件中心主页（原生）的主色是 `#FF0A59F7`，卡片圆角 32；插件窗口（WPF）的信息卡
圆角 16，没有用蓝色主色。两者不是同一套视觉规范，托盘面板要对齐的是插件窗口这一套。

---

## 8. 与我们现有实现的差距，以及实现顺序

现状：`Common/include/PenStatusChannel.h`（提交 `ed979d1`）已经发布
`batteryLevel`、`charging`、`deviceAttached`、`stylusLinked`、`modelId`、`modelName`。
笔的核心状态已经齐了。

建议顺序：

1. **先修电量图标分档。** 一行改动，把 `BatteryStepFor()` 从向下取整改成 6.4 节的规则。
   收益立刻可见，且不依赖任何新协议。

2. **补笔的固件版本、硬件版本、序列号。** 三条都是现成的 MCU 命令，码值见 4.2 节：
   `0x01` 序列号、`0x02` 硬件版本、`0x03` 固件版本，无 payload，发出去等同码事件回来即可。
   返回的是字符串，长度由 `packet[7]` 给出（本机实测固件版本 14 字节 `CD54 1.0.0.143`、
   硬件版本 17 字节 `01-0400.0143-0000`）。做完就能照 3.3 节的设备信息页排出完整一页。

3. **放开 RX 解析的子系统校验，接入键盘。** 当前 `TryParsePenUsbEventFrame()` 硬校验
   `packet[2] == 0x07 && packet[4] == 0x01`，键盘帧过不了。按 `docs/KBDMCU_PROTOCOL.md`
   第 6.2 节把 `(destination, subsystem)` 提成常量对，在 `PenEventBridge` 已有的读循环里
   按 `packet[4]` 分流——**不要另开句柄**，会互相吃包。

4. **键盘状态上通道。** 电量、充电、连接、分离四条，命令与事件码在
   `docs/KBDMCU_PROTOCOL.md` 5.1/5.2 节已经列全，照抄字节即可。需要给
   `PenStatusChannel` 加一段键盘 payload，`kAbiVersion` 要跟着升。

5. **「键盘分离」开关。** 唯一一条写命令，协议已完全逆向（`0x35` 查、`0x34` 设，
   Set 之后重发 Get 确认，不要等 `0x34` 应答）。做成托盘菜单项即可。

6. 双击功能设置、固件更新——都要写 MCU 或依赖云服务，优先级最低。

---

## 9. 环境与复现

本机（实测）：

```text
PCManager        C:\Program Files\Huawei\PCManager
选件中心组件      components\accessories_center，config 版本 202601211548121
AccessoryApp     v2.0.2.92 tBeta b2024112718000001
本机设备          笔 CD54R（模组 65819，固件 CD54 1.0.0.143，硬件 01-0400.0143-0000）
                 键盘 RX0H（固件 GAOKUN_KBD_BD 1.0.0.39，硬件 C5210_1）
```

证据来源：

```text
运行日志   C:\ProgramData\Comms\PCManager\log\AccessoryCenter\AccessoryApp\
             InfoAcAppDaemon.log        常驻进程，笔与键盘回调
             InfoAccessoryApp.log       UI 进程，启动参数与设备信息
             McuDevConnectService-SYSTEM.config.xml   PenMacAddr
             PenApp_<SID>.config.xml    KeyFunc 等用户设置
注册表     HKCU\SOFTWARE\Huawei\PCManager\AccessoriesCenter\
           HKLM\SOFTWARE\Huawei\IConnect\   插件版本清单，含 283 / 4468738
```

工具与方法：

```text
托管程序集   ilspycmd 8.2（装在 scratchpad 的 --tool-path 下，非全局）
             ilspycmd -p -o <out> <asm>，产出 .cs 与未转换的 .baml
原生 DLL     pefile 导入表/导出表；自写字符串扫描
帧头还原     正则扫 .text 里的 C7 44 24 <disp8> <imm32>，按 disp/disp+4 配对
分发表       扫非 .text 节里 16 字节一条的 (事件码, .text 指针) 连续数组；
             处理函数里的 FF 90/A0 <imm32> 给出回调槽位，与 Register* 里的
             48 89 98/99/90/91 <imm32> join
ICU .res     自写 formatVersion 2 解析器（注意 STRING_V2/TABLE16 的偏移
             以 16 位单元计，基址是 indexes[1]*4，不是 body 起点）
BAML         长度前缀 UTF-8 扫描可捞出字符串表；完整反编译见第 10 节
图片         Pillow 量 size 与 getbbox
```

托管/原生的分界（实测）：`AccessoryApp.exe`、`AcAppDaemon.exe`、`AccessoryAppCommon.dll`
和全部 `*PenApp.dll` / `*KeyboardApp.dll` 是 .NET；`PenService.dll`、`KeyboardService.dll`、
`AccessoriesCenterUi.dll`、`AccessoriesManagerCenter.dll`、`AccessoriesAdapter.dll`、
`AccessoryMgmtService.dll`、`McuDevConnectService.dll` 是原生。

---

## 10. 待补

| 项 | 状态 |
|---|---|
| 宿主的 `AccessoryKeyboard` 从哪里取到 `RX0H` 这个字符串 | 未解。MCU 的模组查询对这块键盘返回 `0`，所以不是那条路；见 3.4 |
| `0x15` / `0x16`（新连接请求与结果）的 payload 结构 | 未解；与状态展示无关 |
| `0x80` / `0x81` / `0x82`（Chr 系列）的 4 字节 payload 语义 | 未解 |
| `THP_Service.dll` 处理的 `0x10` / `0x21` / `0x23` / `0x2E` | 不在 `PenService.dll` 分发表内，本轮未追 |
| 插件主窗口的视觉参数 | 已从 BAML 字符串表扫出（7.3 节），属性归属靠相邻顺序推断；逐字确认需完整 BAML 反编译 |
| `basesettingpage.baml`、`window_ravenb_kbd.baml` 的视觉参数 | 未扫；`deviceinfopage.baml` 已扫，见 3.3 |
| 设备信息页「蓝牙地址」对 MCU 设备是否为空 | 未在本机验证 |
