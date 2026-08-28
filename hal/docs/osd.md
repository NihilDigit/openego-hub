# 华为 OSD 逆向记录

对象为 HUAWEI MateBook E Go（GK-W7X，平台代号 GaoKun）上随机预装的屏幕提示浮层。目的有两个：取到足以用
WinUI 3 重绘的样式数值，以及查清 Fn 功能键从按下到浮层出现的完整链路。

本文所有数值来自二进制与配置文件，取值方式在各节注明。未经实机验证的部分单列一节。

## 一、结论摘要

- OSD 主体 `OSD_Daemon.exe` 是 .NET Framework 4 的 WPF 程序，已完整反编译为 C# 源码，界面 BAML 已还原为
  可读的属性树。样式数值全部可得，自绘无信息缺口。
- Fn 键事件的来源是 WMI，不是 SPI 中断。事件类为 `root\WMI` 命名空间下的 `OemWMIEvent`，键码放在
  `Force`（UInt32）属性里。订阅这个事件不需要加载任何华为 DLL，原生 ARM64 进程可以直接做。
- `StartMonitorSpiInterrupt` 与按键无关，它监听的是屏幕刷新率相关的面板中断，且调用模型与已逆向的三个厂商
  DLL 不同：它不阻塞，内部自建线程，回调是 `void(*)(void)`。
- 执行动作与显示浮层在厂商实现里是**合在一起**的：同一个 `SetFunction.SetMachineFunc` 先调 HardwareHal 改硬件
  状态，再拿返回值填浮层。两者没有分离，接管时必须整体替换。

## 二、组件构成

| 路径 | 说明 |
| --- | --- |
| `C:\Windows\System32\RPC\OSD\osdservice.exe` | 服务 `HW_OSDServer` 的宿主，283512 字节，原生 x64 |
| `C:\Program Files\Huawei\Huawei OSD\OSD_Daemon.exe` | 界面进程，222072 字节，.NET Framework 4 WPF，AnyCPU 标记为 AMD64 |
| `C:\Program Files\Huawei\Huawei OSD\HardwareHal.dll` | 184184 字节，28 个 C 风格导出，OSD 专用 |
| `C:\Program Files\Huawei\Huawei OSD\BlackMagic.dll` | 33656 字节，日志、注册表、麦克风指示灯、大数据上报 |
| `C:\Program Files\Huawei\Huawei OSD\image\` | 全部位图资源，PNG，未加密 |
| `C:\Program Files\Huawei\Huawei OSD\config\LocalInterfaceConfig.ini` | 高能模式浮层的分语言尺寸表 |

`OSD_Daemon.exe` 的 PDB 路径残留为
`D:\workbuild\workspace\Apk_Build_Service\apk_build\apk_code\OSD\Osd\output_temp\pdb\osdservice.pdb`，
与 `HardwareHal.dll` 同一构建目录，说明这两者是一套工程。

### 与 BasicService 下同名 DLL 的区别

`C:\Program Files\Huawei\BasicService\HardwareHal.dll` 有 448 个导出，全部是 C++ 修饰名
（`?GetRefreshRate@VideoCard@@QEAAKXZ` 一类），**不导出** `SetVolume`、`SetScreenWnd`、`SetMic`、
`StartMonitorSpiInterrupt`。OSD 用到的这一组只存在于 OSD 目录下的 184 KB 版本里。两个文件同名而内容不同，
不是「完整版与裁剪版」的关系。

OSD 目录版本的 28 个导出：

```
GetMachineInfo        GetRefreshRate      GetRegInfoKey       GetSecTypeEx
GetVersion            HalCalAllMicMuteStatus                  HalInit
HalQueryInformation   IsPcManagerUpgrading                    IsSupportDataReport
IsWhiteLogoStatus     IsWin11System       MicInit             QueryWifiStatus
SetAudioFunc          SetChangeRefreshanRate                  SetMic
SetMicLightStatus     SetRefreshRate      SetScreen           SetScreenWnd
SetVolume             SetWifi             SpiMonitoringStop   StartMonitorSpiInterrupt
SyncRefreshRate       UpdateMicLightStatus
?GetSysManufactor@OsdFunc@@YA?AV?$basic_string@_W...@std@@XZ
```

## 三、事件链路

### 3.1 从按键到浮层

```
Fn 功能键
  │  固件产生 ACPI 通知
  ▼
ACPI\PNP0C14\HWMI_0   （WMI ACPI 映射设备，字符串来自 HardwareHal.dll）
  │  WMI 事件 root\WMI : OemWMIEvent，Force = 键码
  ├────────────────────────────────┐
  ▼                                ▼
osdservice.exe                 OSD_Daemon.exe
（HW_OSDServer，LocalSystem）  （用户会话内，WmiWatcher 自行订阅）
  │  WTSQueryUserToken                │
  │  DuplicateTokenEx                 │
  │  CreateProcessAsUser              │
  │  "OSD_Daemon.exe" <键码>          │
  └──────────► OSD_Daemon ────────────┘
                    │
                    ▼
       SetFunction.SetMachineFunc(键码, Machine)
                    │
        ┌───────────┴───────────┐
        ▼                       ▼
  HardwareHal.dll          WPF 浮层窗口
  改硬件状态并回读当前值    用回读值渲染并 Show()
```

两条路径同时存在。`OSD_Daemon` 用互斥体 `osd_daemon` 保证单实例，重复启动的实例以退出码 -99 自杀；服务侧的
`OSDKeeper` 负责在用户会话中保活一份，之后的按键由常驻实例自己的 `WmiWatcher` 处理。

`osdservice.exe` 中的相关日志串：`GetWmiEventIDCallback1 uHotkeyID is`、`OSDKeeper::StartProcess
CreateProcessAsUser begin, cmd:`、`OSDKeeper::StartProcess same session, no need to restart.`。

### 3.2 键码表

常量取自 `SetFunction` 的私有常量与 `SetMachineFunc` 的 switch 分支。

| 键码（十进制） | 含义 | 处理 | 浮层 |
| --- | --- | --- | --- |
| 641 | 亮度减 | `SetScreenWnd` | Sound 窗口（本机不触发，见下） |
| 642 | 亮度加 | `SetScreenWnd` | 同上 |
| 644 | 静音切换 | `SetVolume` | Sound 窗口 |
| 645 | 音量减 | `SetVolume` | Sound 窗口 |
| 646 | 音量加 | `SetVolume` | Sound 窗口 |
| 647 | 麦克风静音切换 | `SetMic` + `BmSetMicLightStatus` | WWM 窗口 |
| 649 | Wi-Fi 开关 | `QueryWifiStatus` / `SetWifi` | WWM 窗口 |
| 650 | 启动电脑管家 | 拉起 PCManager 或弹窗 | F10PopWnd（GaoKun 上禁用） |
| 652 / 653 | Win 键解锁 / 锁定 | 仅显示 | WWM 窗口 |
| 656 / 657 | 雷电口提示 | 仅显示 | TBT 窗口 |
| 659 / 660 | PD 充电口 / 视频输出口插错 | 仅显示 | PluginWindow |
| 662 | 非标充电器 | 仅显示 | UnStandardCharger |
| 672 / 673 / 678 | 平衡 / 高能 / 切换失败 | 读写注册表 | PerfSetting（GaoKun 不支持） |
| 700 / 701 | 摄像头开 / 关 | 仅显示 | WWM 窗口 |
| 704 | 刷新率切换 | `SetChangeRefreshanRate` | ReFreshRateSetting |
| 706 | 雷电口不支持该 USB 设备 | 仅显示 | TBTUnsupportUsb |

`HardwareHal.dll` 内部的 `SetAudioFunc` 对键码再分派一次：0x284(644) 走静音，0x285/0x286(645/646) 走
`SetVolume`，0x287(647) 走 `SetMic`。

### 3.3 本机（GaoKun）上被跳过的分支

`App.Machine` 由 `GetMachineInfo` 取回的 BIOS 串在 `config\MachineTypeList.xml` 里查表得到。本机
`Win32_ComputerSystem.Model` 为 `GK-W7X`，该表只有 HZ、Bell、Pascal、Watt、Marconi、Mach、Kepler、
Darwin、Volta 九项，**没有匹配项**，`Select_Machine` 返回 null。

于是 `SetMachineFunc` 中 641/642 的分支条件 `Machine == "Pascal"` 不成立，**亮度键在本机不显示浮层，也不
调用 `SetScreenWnd`**。亮度由 Windows 自身处理还是完全不动，未验证。

`GetVersion` 返回的产品名在本机为 `gaokun`（依据：`IsSupportPCManagerWeb` 对 `gaokun` 返回 false，
`configRefreshRate.xml` 中 `GaoKun` 的刷新率档位为 120/60）。由此：

- 高能模式浮层只对 `HookeG;CurieG` 生效，本机不涉及。
- `mipiProducts.xml` 为 `DiracR;MorganF;MorganG`，本机不在其中。
- 刷新率切换（704）的高低档为 120 / 60。

### 3.4 StartMonitorSpiInterrupt 的调用模型

与已逆向的三个厂商 DLL **不是**同一种模型。反汇编 `HardwareHal.dll+0x162C0` 得到：

1. 把入参（回调指针）存入全局 `0x180029A68`，置监听标志 `0x180029A60 = 1`；
2. 通过 SetupDi 枚举并 `CreateFileW` 打开 SPI/MCU 设备（内部函数 `0x180015540`，失败时日志
   `rr failed to OpenSpiDevice, error=`）；
3. `CreateEventW(NULL, TRUE, FALSE, NULL)` 建事件；
4. `CreateThread(..., 0x180015E30, ...)`，**随即返回**。

工作线程 `0x180015E30` 的循环体是
`DeviceIoControl(hDev, 0x223C40, NULL, 0, out(4 字节), 4, &ret, NULL)`，处理 `ERROR_IO_PENDING`(997)，
在中断到来时执行 `call qword ptr [0x180029A68]`——调用点前没有任何寄存器传参，与 C# 侧声明为 `Action`
（`void(*)(void)`）一致。

结论：这是一次性注册，不需要调用方提供线程，回调无参无返回值，回调在 DLL 自建线程上执行。IOCTL 0x223C40
解开是 DeviceType=0x22（FILE_DEVICE_UNKNOWN）、Function=0xF10、Method=BUFFERED、Access=ANY。

在 OSD 里这个回调只做一件事：`MessageOnlyWindow.SyncRefreshRateToBios()`，即把当前刷新率折算成
高/低两档写回 BIOS。**它不承载按键事件。**

### 3.5 ARM64EC 边界

需要跑在 ARM64EC 宿主里的只有「调用 `HardwareHal.dll` / `BlackMagic.dll`」这一段。具体是：

- 音量与静音：`SetVolume`
- 麦克风静音：`SetMic`、`MicInit`、`BmSetMicLightStatus`（指示灯）
- Wi-Fi：`QueryWifiStatus`、`SetWifi`
- 刷新率：`SetChangeRefreshanRate`、`GetRefreshRate`、`SetRefreshRate`、`SyncRefreshRate`、
  `BMSyncRefreshRateToBIOS`
- 面板中断：`StartMonitorSpiInterrupt`、`SpiMonitoringStop`

不需要 EC 的：

- **事件订阅**。`OemWMIEvent` 是标准 WMI，原生 ARM64 进程直接订阅即可。
- **界面**。全部数值和位图都已取出，WinUI 3 原生绘制。
- **多语言文案**。已从程序集资源中导出为 XML。

## 四、样式规格

数值来源：BAML 用 `Baml2006Reader` 还原为节点流，`TypeConverterMarkupExtension` 与
`DeferredBinaryDeserializerExtension` 通过 `ProvideValue` 解开，得到真实的 double 与 `SolidColorBrush`；
窗口位置与动态几何来自反编译的 C# 代码。所有长度单位是 WPF 的设备无关像素（DIP，96 DPI 基准），移植到
WinUI 3 时同为 effective pixel，可直接沿用。

### 4.1 音量 / 亮度浮层（`Sound`）

窗口：

| 项 | 值 |
| --- | --- |
| 尺寸 | 66 × 140 |
| 位置 | `Left = 48`、`Top = 60`，主屏绝对坐标，固定不随分辨率变化 |
| 背景 | `#CC000001`（不透明度 204/255 ≈ 80%） |
| 圆角 | **无**。XAML 里是 `Window.Background` 直接铺满，没有 Border，没有 CornerRadius |
| 材质 | 纯色，`AllowsTransparency=True`、`WindowStyle=None`、`Topmost=True`、`ShowInTaskbar=False` |
| 停留 | 2000 ms，到期后 `Visibility = Hidden` |
| 动效 | **无**。BAML 里没有 Storyboard，代码里没有动画，显示与消失都是硬切 |

进度条（Grid 内的 StackPanel，`Width=10`、`Height=78`，Grid 中默认居中，即左上角约 (28, 31)）：

三条竖直 `Line`，`X1=X2=5`，`StrokeThickness=10`，端点为默认平头，因此实际是 10 宽 78 高的矩形条：

| 名称 | 颜色 | 几何 | 作用 |
| --- | --- | --- | --- |
| `sound_background` | `#FF6A6A6A` | Y 从 78 到 0 | 整条轨道 |
| `panel` | `#FFE8F2F7` | Y1=78，Y2=70−num；`Margin=0,-78,0,0` | 填充上方 8 px 的白色游标 |
| `sound` | `#FF29A5ED` | Y1=78，Y2=78−num；`Margin=0,-78,0,0` | 蓝色填充，压在最上层 |

`num` 的取值：

- 音量：`num = value × 0.7`，`value` 为 0–100，填充高度 0–70 px；
- 亮度：`num = value × 7.0`，`value` 为 0–10 的档位，填充高度同为 0–70 px。

叠加后的观感是：灰轨道 78 px，蓝色从底部长到 `num`，其上紧接 8 px 白色游标，满值时白色游标正好顶到轨道顶端。

图标：`Image` 24 × 24，`Margin = 21,109,21,7`，即位于窗口内左上角 (21, 109)。选图规则见 5.1。

### 4.2 麦克风 / Wi-Fi / Win 键 / 摄像头浮层（`WWM`）

| 项 | 值 |
| --- | --- |
| 尺寸 | 190 × 66 |
| 位置 | `Top = 60`；LTR 时 `Left = 125`，RTL 时 `Left = 屏宽 − 125 − 190` |
| 背景 | `#CC000001` |
| 圆角 | **无** |
| 停留 | 2000 ms，到期后 `Visibility = Collapsed` 并 `Close()`，同时新建一个实例备用 |
| 动效 | **无** |

内容：

- 图标 `Image` 40 × 40，`Margin = 10,13,140,12.6`，即左上角 (10, 13)；
- 文本容器 `Grid` `Margin = 50,0,0,0`，其内 `TextBlock` 再 `Margin = 10,0,10,0`，
  即文字区左边界为 60，右边界为 180；
- 文本：`FontSize = 14`，`Foreground = #FFF5EDED`，`TextWrapping = Wrap`，垂直居中；
- 摄像头不可用（状态 −1）时字号临时改为 12；
- RTL 语言把整个 `Dispalypanel` 的 `FlowDirection` 翻成 RightToLeft。

XAML 未指定字体，走 WPF 默认（简中环境下即 Microsoft YaHei）。

**实机核对（已验证）**：手动以键码 652 启动 `OSD_Daemon.exe`，在 2560 × 1600、200% 缩放的主屏上截图，
浮层实际占据物理像素 x 250–629、y 120–251，即 380 × 132 物理 = **190 × 66 逻辑**，左上角逻辑坐标
**(125, 60)**，与 BAML 和源码给出的数值逐项吻合。同时确认：

- 四角是**直角**，Win11 没有对它做圆角化；
- 背景为半透明黑，下层窗口内容清晰可见；
- 出现与消失都是硬切，没有淡入淡出。

截图见 `<scratchpad>/shots2/f06..f12.png`，放大裁切见 `<scratchpad>/overlay_wwm.png`。这组数值吻合也反过来
说明从 BAML 取值的方法可靠，可以直接采信 Sound 窗口等未截图窗口的数值。

### 4.3 刷新率浮层（`ReFreshRateSetting`）

| 项 | 值 |
| --- | --- |
| 尺寸 | 160 × 160 |
| 位置 | `Left = (屏宽 − 160) / 2`；`Top = 屏高 − (160 + 屏高 / 8)` |
| 背景 | `bg_refresh.png` 作 `ImageBrush`，`Stretch = Fill` |
| 停留 | 3000 ms |
| 动效 | **无** |

`bg_refresh.png` 实测：216 × 216，纯色 RGB(77,77,77)，Alpha 242（≈95%），四角圆角半径约 20 px。按
`Stretch=Fill` 缩到 160 × 160 后，等效圆角约 **15 px**，等效背景色约 `#F24D4D4D`。这是全套浮层里唯一有
圆角的窗口，圆角画在位图里而非 XAML 里。

内容分两态，同一时刻只显示一个 StackPanel：

- 固定档位：`refresh_stack` 居中，一行 `TextBlock`，内含两个 `Run`——数值 `FontSize = 47`，单位
  `FontSize = 16`，均为 `#FFFFFFFF`。文案取 `IDS_OSD_REFRESH_VALUES`，简中为 `{0}Hz`、英文为 `{0} Hz`；
  代码里用 `string.Format(text, "")` 把占位符替成空串，所以单位 Run 实际只剩 `Hz`。
- 智能档（状态值 1000）：`dynamic_stack`，图标 `Image` 64 × 64、`Margin = 48,32,48,16`、
  `Stretch = UniformToFill`，图源为 `ic_public_rotate@{1,2,3}x.png`；下方 `TextBlock` 高 24、
  `FontSize = 16`、`Margin = 0,0,0,24`，文案 `IDS_OSD_REFRESH_AUTO_VALUES`（简中「智能」）。

本机档位为 120 / 60。

### 4.4 高能模式浮层（`PerfSetting`）

本机产品名 `gaokun` 不在 `configRefreshRate.xml` 的 `Performance/productNameList`（`HookeG;CurieG`）里，
**GaoKun 不显示这个浮层**。为完整性记录：

- 窗口尺寸与内部各元素的尺寸、字号、透明度全部由 `config\LocalInterfaceConfig.ini` 按语言给出，18 个分号
  分隔的字段，索引含义为：0 窗口宽、1 窗口高、2 图上留白高、3 图宽、4 图高、5 名称上留白高、6 名称字号、
  7 名称宽、8 名称高、9 名称字体名、10 名称不透明度、11 描述上留白高、12 描述字号、13 描述宽、14 描述高、
  15 描述字体名、16 描述不透明度。索引 9 与 15 的字体名代码里未使用。
- `[root]` 段 `OPEN=232;230;16;98;98;16;13;200;34;MicrosoftYaHei;0.9;8;9;200;60;MicrosoftYaHei;0.6`。
- 背景 `background.png` 作 `ImageBrush`、`Stretch=Fill`；实测 312 × 276，纯色 RGB(47,47,47)，Alpha 153
  （60%），圆角半径约 18 px。因为窗口宽高比与位图不同，`Fill` 会把圆角拉成椭圆。
- 位置：`Left` 水平居中于工作区，`Top = 工作区高 − (窗口高 + 10)`，均按 `GetDeviceCaps` 取到的 DPI 换算。
- 停留 3000 ms；帧动画由 45 ms 的 Timer 驱动，逐帧换图，共 45 帧（约 2.0 s）后停。

### 4.5 其余窗口

`TBT`、`TBTHighPower`、`TBTUnsupportUsb`、`PluginWindow`、`UnStandardCharger`、`F10PopWnd` 都是插拔提示
类弹窗，与 Fn 键浮层不是一类。BAML 均已还原在
`<scratchpad>/osd_src/`，需要时按同样方式取值。

## 五、资源清单

原始位图未加密，直接是磁盘上的 PNG，可任意复制。已全部导出到：

```
C:\Users\rosetta\AppData\Local\Temp\claude\C--Codes-hwthpec\e478992e-704c-4595-bb3f-3aff11734c71\scratchpad\osd_assets\
```

其中 `background.png`、`bg_refresh.png`、`ic_public_rotate@{1,2,3}x.png` 与 `f10_resoures\` 下三张是
`OSD_Daemon.exe` 的 WPF 资源（`pack://application:,,,/`），由 ilspycmd 从程序集中提取；其余直接来自
`C:\Program Files\Huawei\Huawei OSD\image\`。

### 5.1 Fn 键浮层实际用到的图标

| 文件 | 尺寸 | 格式 | 用途 | 显示尺寸 |
| --- | --- | --- | --- | --- |
| `Img_Sound_Off.png` | 96×96 | PNG RGBA | 静音 | 24×24 |
| `Img_Sound_Small.png` | 96×96 | PNG RGBA | 音量 ≤ 50 | 24×24 |
| `Img_Sound_Big@100.png` | 64×64 | PNG RGBA | 音量 > 50 | 24×24 |
| `Img_Screen_Small.png` | 96×96 | PNG RGBA | 亮度（任意档位都用这一张） | 24×24 |
| `Img_Microphone_Open.png` | 96×96 | PNG RGBA | 麦克风开 | 40×40 |
| `Img_Microphone_Close.png` | 96×96 | PNG RGBA | 麦克风关 | 40×40 |
| `Img_Wifi_Open.png` | 96×96 | PNG RGBA | Wi-Fi 开 | 40×40 |
| `Img_Wifi_Close.png` | 96×96 | PNG RGBA | Wi-Fi 关 | 40×40 |
| `Img_Win_Open.png` | 96×96 | PNG RGBA | Win 键解锁 | 40×40 |
| `Img_Win_Close.png` | 96×96 | PNG RGBA | Win 键锁定 | 40×40 |
| `img_camera_Open.png` | 80×80 | PNG RGBA | 摄像头开 | 40×40 |
| `img_camera_Close.png` | 80×80 | PNG RGBA | 摄像头关 | 40×40 |
| `ic_public_rotate@1x.png` | 64×64 | PNG RGBA | 智能刷新率 | 64×64 |
| `ic_public_rotate@2x.png` | 128×128 | PNG RGBA | 同上，2× | — |
| `ic_public_rotate@3x.png` | 192×192 | PNG RGBA | 同上，3× | — |

代码里的路径写作 `image\Img_WiFi_Open.png`，磁盘上是 `Img_Wifi_Open.png`，大小写不一致，靠 Windows
文件系统不区分大小写才能跑通。移植到区分大小写的构建流程时会踩到。

只有 `ic_public_rotate` 一族提供了 1×/2×/3× 三档，其余图标各只有一份，且实际显示尺寸（24 或 40）远小于
位图尺寸（96 或 80），厂商是靠降采样应付高 DPI 的。自绘时建议改画矢量。

### 5.2 未被引用的图标

`Img_Keyboard.png`(48×48)、`ic_fn.png`(111×110)、`Img_Screen_Big.png`、`Img_Screen_Big@100.png`、
`Img_Screen_Small@100.png`、`Img_Sound_Big.png`、`Img_Sound_Off@100.png`、`Img_Sound_Small@100.png`、
`Pic_02.png` 在反编译出的代码里没有引用，属历史残留。`@100` 后缀一族看起来是另一套 100% 缩放的图标，
只有 `Img_Sound_Big@100.png` 被留用，同一个窗口里因此混着 96×96 与 64×64 两种源图。

### 5.3 帧动画序列

`image\performance\` 下三组逐帧 PNG，仅高能模式浮层使用，GaoKun 用不到：

- `balance_0..44.png`，45 帧，120×120
- `high_0..44.png`，45 帧，200×200
- `error_0..27.png`，28 帧，200×200

## 六、文案与多语言

文案不在磁盘上，而是嵌在 `OSD_Daemon.exe` 的托管资源里，资源名格式 `Hw_OSD.Language.<culture>.xml`，
命中失败时回落到 `Hw_OSD.Language.root.xml`。已全部提取到 `<scratchpad>/osd_src/`。

覆盖 24 个 culture：ar-SA、cs-CZ、da-DK、de-DE、el-GR、en-GB、en-US、es-ES、es-MX、fi-FI、fr-CA、fr-FR、
it-IT、ja-JP、ko-KR、nb-NO、nl-NL、pl-PL、pt-PT、ru-RU、sv-SE、th-TH、tr-TR、zh-CN，外加 root 与伪本地化
用的 zz-ZX，共 26 个资源。

Fn 键浮层直接用到的条目：

| 资源名 | 简体中文 | 英文 |
| --- | --- | --- |
| `IDS_OSD_MICROPHONE_ALL_ON` | 所有麦克风已开启 | All microphones enabled |
| `IDS_OSD_MICROPHONE_ALL_OFF` | 所有麦克风已关闭 | All microphones disabled |
| `IDS_OSD_WIFI_ON` | WLAN 已开启 | Wi-Fi enabled |
| `IDS_OSD_WIFI_OFF` | WLAN 已关闭 | Wi-Fi disabled |
| `IDS_OSD_WIN_KEY_ON` | WIN 已开启 | Windows key enabled |
| `IDS_OSD_WIN_KEY_OFF` | WIN 已关闭 | Windows key disabled |
| `IDS_OSD_CAMERA_ON` | 摄像头已开启 | Camera on |
| `IDS_OSD_CAMERA_OFF` | 摄像头已关闭 | Camera off |
| `IDS_OSD_CAMERA_DISABLE` | 按键失败，请检查华为电脑管家是否运行 | Launch Huawei PC Manager first. |
| `IDS_OSD_REFRESH_VALUES` | `{0}Hz` | `{0} Hz` |
| `IDS_OSD_REFRESH_AUTO_VALUES` | 智能 | Dynamic |

音量与亮度浮层没有文字。

RTL 判定不读资源，而是用 `CultureInfo.TextInfo.IsRightToLeft` 现算。

## 七、可行性判断

### 能自绘

**依据充分**，样式来自 BAML 与源码，位图可直接复制。

- 音量 / 静音浮层。尺寸、坐标、颜色、进度条几何、填充公式、停留时长全部拿到（4.1）。
- 麦克风 / Wi-Fi / Win 键 / 摄像头浮层。同上（4.2），文案已提取（第六节）。
- 刷新率浮层。数值齐全（4.3），唯一需要注意的是圆角画在 `bg_refresh.png` 里，自绘时改成
  `CornerRadius = 15` + 纯色 `#F24D4D4D` 即可，不必带这张图。
- 亮度浮层。数值与音量共用同一个窗口，公式不同（`×7.0`）。但本机不触发（3.3），要做就是我们自己造。

需要注意：原厂**没有任何动画**——没有淡入淡出，没有缓动，到点直接改 `Visibility`。要不要加淡入淡出是产品
决定，不是还原问题。停留时长音量/WWM 为 2000 ms、刷新率与高能为 3000 ms。

### 能接管事件

**依据充分但有一处未验证**。

- 事件源是 `root\WMI` 的 `OemWMIEvent`，已确认该类在本机存在，属性为
  `SECURITY_DESCRIPTOR / TIME_CREATED / Active / Force / InstanceName`，键码在 `Force`。
- 订阅它是纯 WMI 操作，原生 ARM64 进程可做，不经过任何华为 DLL。用 `IWbemServices::ExecNotificationQuery`
  或 .NET 的 `ManagementEventWatcher` 均可，`osdservice.exe` 与 `OSD_Daemon.exe` 各用了其中一种。
- 未验证的是：实际按 Fn 键时事件是否真的到达我们的订阅者（见第八节）。

### 只能保留原厂

- **执行动作**。音量、麦克风、Wi-Fi、刷新率的实际生效依赖 `HardwareHal.dll` 的 `SetVolume`、`SetMic`、
  `SetWifi`、`SetChangeRefreshanRate`，以及 `BlackMagic.dll` 的 `BmSetMicLightStatus`（麦克风指示灯）。
  这些必须在 ARM64EC 宿主里加载。
  - 其中音量一项**可能**可以绕开：`HardwareHal.dll` 导入了 `ole32!CoCreateInstance` 与
    `CoInitializeSecurity`，`SetVolume` 内部还查了 `Win32_SoundDevice`，看起来是走 Core Audio 而非 EC
    寄存器。若确证如此，音量可以在原生 ARM64 侧用 `IAudioEndpointVolume` 自己实现。**这一条只是导入表与
    字符串给出的迹象，没有跟完调用链，不要当结论用。**
  - 麦克风指示灯确定绕不开，是 EC 侧的灯。
- **面板刷新率与 BIOS 的同步**。`StartMonitorSpiInterrupt` + `SyncRefreshRateToBIOS` 这条线只在
  mipiProducts 列表内的机型上跑，GaoKun 不在列表里，暂时可以不管。

## 八、未验证项

明确列出，不要按已确认对待。

1. **按 Fn 键时 `OemWMIEvent` 是否真的送达第三方订阅者**。我用
   `Register-CimIndicationEvent -Namespace root/wmi -Query 'SELECT * FROM OemWMIEvent'` 订阅成功并等待了
   12 分钟，期间零事件——但这段时间内没有人按 Fn 键，所以既不能证实也不能证伪。复现方法：跑
   `<scratchpad>/wmilisten.ps1`，然后按音量键，看 `<scratchpad>/oemwmievent.log` 是否出现
   `Force=645/646`。
2. **HW_OSDServer 停用后 Fn 键是否还有事件**。该服务当前状态是 `Disabled` + `Stopped`，`OSD_Daemon.exe`
   也没有在跑，因此本机现在**没有**厂商 OSD。ACPI 事件按理由固件产生、与服务无关，但没验证。
3. **亮度键在本机的实际行为**。代码路径被 `Machine != "Pascal"` 挡掉（3.3），亮度是完全不动、由 Windows
   处理、还是由别的组件处理，未测。
4. **`SetVolume` 是否只是 Core Audio 的包装**。见第七节的说明。
5. ~~浮层的实机外观~~ **已验证**，见 4.2 的实机核对。`WWM` 浮层的位置、尺寸、直角、无动效均已由截图确认。
   `Sound` 浮层没有截到（原因见第 6 条），其数值凭 `WWM` 的吻合度间接采信。
6. **手动启动 `OSD_Daemon.exe` 无法复现音量浮层**。以键码 646 启动后进程正常驻留，`Sound` 与 `WWM` 窗口对象
   都已创建，但 `IsWindowVisible` 全为 false、矩形为 0,0,0,0——说明 `Hardware.Set_Volume` 返回 0，代码在
   `Show()` 之前就 `return false` 了。原因未查（可能需要 `HalInit`、需要 LocalSystem 派生的令牌，或依赖
   已停用的 `HW_OSDServer`）。音量在事后读数为 0%，若 646 生效过应为非零，据此判断音量未被改动——但没有
   「之前」的读数，这是推断不是直测。改用纯显示型键码 652 才截到浮层。

## 九、坑

- **两个 `HardwareHal.dll` 同名而不同物**。`Huawei OSD\` 下的 184 KB 版本导出 28 个 C 名字，OSD 用的
  `SetVolume` / `StartMonitorSpiInterrupt` 全在这里；`BasicService\` 下的 448 导出版本全是 C++ 修饰名，
  不含这些。按文件名找函数会找错文件。
- **`StartMonitorSpiInterrupt` 与按键无关**。名字容易让人以为它是键盘中断入口，实际是面板刷新率同步用的，
  回调无参，且在 GaoKun 上因为不在 `mipiProducts.xml` 里而基本不起作用。真正的键事件走 WMI。
- **`OSD_Daemon.exe` 有两条互不知情的取事件路径**：命令行参数（服务拉起时传键码）和自身的
  `ManagementEventWatcher`。改造时若只堵一条，另一条还会触发。
- **`legacy_osd_path` 自杀开关**。`OSD_Daemon` 启动时和每次收到 WMI 事件时都检查
  `C:\Program Files\Huawei\HwOsd\OSDListener.exe` 是否存在，存在就以 -99 退出。这是它给旧版 OSD 让路的
  机制，本机该目录不存在。若我们要用文件存在性禁掉厂商 OSD，这是一个比停服务更轻的开关——但会改动华为的
  安装目录，本轮没有采用也没有测试。
- **`SetFunction.SetWIFIfunc` 是一个常驻轮询线程**，构造 `SetFunction` 时就 `Start()`，内部 `while(true)`
  每秒查一次 Wi-Fi 状态并回写。也就是说仅仅实例化这个类就会起一个不受控的线程，反编译时容易漏看。
- **BAML 不能用 `XamlXmlWriter` 直接转回 XAML**。`Baml2006Reader` 的节点流里有空字符串命名空间前缀，
  `XamlXmlWriter` 会抛「无法写入空字符串值」。改成手工遍历节点流、对
  `TypeConverterMarkupExtension` / `DeferredBinaryDeserializerExtension` 调 `ProvideValue` 才拿得到真实值。
- **在 ARM64 机器上反编译 x64 的 .NET Framework 程序集**：`Baml2006Reader` 需要能加载
  `OSD_Daemon.exe` 本身来解析自定义类型，而 AMD64 标记的程序集进不了 ARM64 进程。用
  `C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe` 编一个 `-platform:x64` 的 .NET Framework
  小工具即可，宿主在 x64 模拟下跑，加载正常。net8 + `UseWPF` 走不通。
- **`dumpbin` 要挑对宿主**。BuildTools 里 `HostX64\x64\dumpbin.exe` 在 ARM64 上模拟运行，`/disasm` 一个
  184 KB 的 DLL 超过 5 分钟不出结果；`HostARM64\x64\dumpbin.exe` 是原生的，几秒完成。此外从 Git Bash 调用
  时 MSYS 会把 `/disasm` 当路径改写，要么用 PowerShell 调，要么设 `MSYS2_ARG_CONV_EXCL`。
- **`configRefreshRate.xml` 里的拼写是 `machineConfigRefreash` 和 `ConnnectExternalRecover`**，
  XPath 照抄，不要顺手改对。
- **`LocalInterfaceConfig.ini` 的 en-GB 段名写成了 `[er-GB]`**，键前缀也是 `ERGB_`。`ReadIniFile` 用
  `CultureInfo.Name` 拼段名，`en-GB` 匹配不上，实际回落到 `[root]`。这是原厂的笔误，照抄配置会一并抄进来。

## 十、复现用的中间产物

均在
`C:\Users\rosetta\AppData\Local\Temp\claude\C--Codes-hwthpec\e478992e-704c-4595-bb3f-3aff11734c71\scratchpad\`
下：

| 路径 | 内容 |
| --- | --- |
| `osd_src\` | `OSD_Daemon.exe` 的完整反编译结果（C# + BAML + 语言 XML + 内嵌 PNG） |
| `osd_assets\` | 全部 PNG 资源，含 `performance\` 下的帧序列 |
| `hal_osd.asm` | `Huawei OSD\HardwareHal.dll` 的完整反汇编（28516 行） |
| `b2x\` | BAML 节点流转储工具（.NET Framework，x64），`e.exe <baml>` 输出解析后的属性树 |
| `tools\ilspycmd.exe` | 反编译器，本地 tool-path 安装 |
| `wmilisten.ps1` / `oemwmievent.log` | `OemWMIEvent` 订阅探针与其输出 |
