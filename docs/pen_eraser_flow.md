# 侧键双击切换橡皮：原厂托管层的完整行为

这份文档记录 PC Manager 选件中心在「侧键双击切换橡皮擦」这条路径上到底做了什么，
用来回答一个具体问题：我们调用 `CommandSendPenCurrentFunc(1)` 之后笔停止书写、
但橡皮不生效，托管层还做了哪些我们没做的事。

结论先说：托管层做的事比预期少得多。它不碰 THP，不注入 HID，不调用 Windows Ink，
不写任何供触控侧读取的状态。它只做两件有效动作——把侧键功能设成橡皮，以及在收到
MCU 的双击事件后按前台进程白名单回一个确认或撤销。

材料来自 ILSpy 对 `CD54PenApp.dll`、`CD54RPenApp.dll`、`CD52PenApp.dll`、
`AlitaPenApp.dll`、`AccessoryAppCommon.dll`、`AcAppDaemon.exe` 的反编译，
以及 `THP_Service.dll` 的 `.rdata` 字符串和本机运行日志。

## 干活的是 CD54PenApp.dll，不是 CD54RPenApp.dll

按笔的 moduleId 去找插件会找错。全树只有 `CD54PenApp.dll` 实现了 `IAcPluginDaemonBiz`，
它的启用判据是 **MCU 版本串**：

```csharp
// CD54PenApp.PenMonitorDaemon.NeedExecute
McuInteractTool.GetMcuVersion(stringBuilder, ref sbLength);
mcuVersion = stringBuilder.ToString().ToLower();
bool flag = mcuVersion.Contains("dirac") || mcuVersion.Contains("gaokun") || mcuVersion.Contains("diracr");
```

Gaokun 命中，所以本机的常驻橡皮逻辑全部来自 `CD54PenApp.dll`。moduleId 只决定设置页用
哪个插件，M-Pencil 二代（65819 / 0x1011B）对应 `CD54RPenApp.dll`，它提供的是
`pic_00011b00_*.png` 一类资源。

`CD54RPenApp.dll` 里同样声明了 `CommandSendPenCurrentFunc` 的 P/Invoke，但**全程序集没有
任何调用者**，是从 `CD54PenApp` 抄过来的死声明。它也没有 `PenMistouchPreventionProc`、
`Window_Eraser`、`FocusApplicationDetection` 这三个类。照着它读会得出「原厂没做别的」的
错误结论。`CD52PenApp.dll` 和 `AlitaPenApp.dll` 同样没有橡皮路径。

## 启用条件：侧键功能必须是 4

橡皮是侧键的六个可选功能之一，取值表见 `CustomApplicationContext.PenKeyFunc`：

```
0 PRINT_SCREEN   1 OPEN_SMART_VOICE_PTZ   2 OPEN_WHITE_BOARD
3 FUNC_CLOSE     4 FUNC_ERASER            5 GLOBAL_ANNOTATION
```

这个值持久化在 MCU 上，托管层负责把本地配置推下去：

```csharp
// CD54PenApp.PenViewModel.GetPenFuncProc
public static int GetPenFuncProc()
{
    int num = XmlOperator.Instance.GetPenEleValue("KeyFunc");
    if (num < 0 || num >= 255 || num == 2) { num = 0; }
    McuInteractTool.CommandSendSetPenKeyFunc(num);
    CustomApplicationContext.GetInstance().penKeyXmlFunc = (CustomApplicationContext.PenKeyFunc)num;
    return num;
}
```

调用时机有两处：守护进程启动时的 `SendPenCurrentDoubleFuncToMcu` 线程，以及每次笔连接事件
（`CallbackProc.CallbackUpdatePenConnectStatus` 起的 `SetPenFunc` 线程）。

配置落在 `C:\ProgramData\Comms\PCManager\log\AccessoryCenter\AccessoryApp\PenApp_<SID>.config.xml`，
`XmlOperator` 里的默认值是 `5`（全局批注），不是橡皮。设置页改选项时同步写 XML 并立即下发
（`CD54RPenApp.Window_Main`，`CommandSendSetPenKeyFunc` → `SetPenEleValue("KeyFunc", …)` →
选中橡皮时弹一次引导窗）。

这个值不影响 MCU 是否上报双击：本机 `KeyFunc` 当前是 `0`，物理双击照样产生 `0x2F`。
它决定的是这次双击在原厂语义下该做什么。原厂托管层不看它，只看 `0x2F` 的 payload 和前台
进程，所以我们复现时是否必须下发 `4` 还没有验证。

## 双击的处理路径

事件码 `0x2F`（`docs/penservice_events.md`），经 `RegisterCallbackPenCurrentFunc` 注册的托管
回调进入。以下是 `CD54PenApp.PenMistouchPreventionProc.CallbackPenCurrentFunc` 的反编译原文：

```csharp
private static int CallbackPenCurrentFunc(int arg)
{
    APP_LOGGER.Info("CallbackPenCurrentFunc, current pen func is " + arg, Array.Empty<object>());
    switch (arg)
    {
    case 0:
        APP_LOGGER.Info("Enter Close EraserWindowShow.", Array.Empty<object>());
        if (CustomApplicationContext.isEraserWindowShow && windowEraser != null)
        {
            APP_LOGGER.Info("Close EraserWindowShow.", Array.Empty<object>());
            windowEraser.CloseWindow();
        }
        McuInteractTool.CommandSendPenCurrentFunc(0);
        break;
    case 1:
    {
        string processName = FocusApplicationDetection.GetProcessName(FocusApplicationDetection.GetForegroundWindow());
        string text = "";
        string text2 = "";
        if (processName != null)
        {
            string[] array = processName.Split('\\');
            string[] array2 = array[array.Length - 1].Split('.');
            text2 = array2[0];
            for (int i = 0; i < array2.Length - 1; i++)
            {
                text += array2[i];
                if (i < array2.Length - 2) { text += " "; }
            }
        }
        if (eraserAppList.Contains(text2.ToLower()) || eraserAppList.Contains(text.ToLower()))
        {
            McuInteractTool.CommandSendPenCurrentFunc(1);
            APP_LOGGER.Info("Enter Show EraserWindowShow.", Array.Empty<object>());
            if (CustomApplicationContext.isEraserWindowShow)
            {
                APP_LOGGER.Info("EraserWindowShow is showing now.", Array.Empty<object>());
                return -1;
            }
            CustomApplicationContext.isEraserWindowShow = true;
            Thread thread = new Thread((ThreadStart)delegate
            {
                windowEraser = new Window_Eraser();
                windowEraser.ShowDialog();
            });
            thread.TrySetApartmentState(ApartmentState.STA);
            thread.IsBackground = true;
            thread.Start();
            APP_LOGGER.Info("Show EraserWindowShow.", Array.Empty<object>());
            break;
        }
        McuInteractTool.CommandSendPenCurrentFunc(0);
        return -1;
    }
    }
    return 0;
}
```

三条路径的返回值不同：命中并开窗返回 `0`，命中但窗口已在显示返回 `-1`，不命中返回 `-1`。
`CallBackFunc` 的签名是 `int CallBackFunc(int)`，原生侧拿得到这个值，但托管层没有任何地方
解释它的含义。`-1` 只出现在「本次不该进入橡皮态」的两条路径上。

**这支笔的 `0x2F` payload 实测恒为 `0`**，即「当前不是橡皮」。也就是说 MCU 每次都走
`case 0` 这条清理路径，`case 1` 只有在上层已经下过 `CommandSendPenCurrentFunc(1)`、笔确实
处于橡皮态时才会进入。**笔不会自己进入橡皮态**，进入必须由上层下命令。

我们这侧曾按 `if (func == 1)` 才分派双击，而这支笔发的是 `0`，事件到了也被静默丢弃，
已经修正。

### 前台进程名的取法

`CD54PenApp.FocusApplicationDetection.GetProcessName`：`GetForegroundWindow` →
`GetWindowThreadProcessId` → `OpenProcess(0x410)` → `QueryFullProcessImageName`。
若拿到的是 `ApplicationFrameHost.exe`，再 `EnumChildWindows` 找一个 pid 与宿主不同的子窗口，
对它的进程重取一次——这是 UWP 解包，OneNote for Windows 10 由此得到 `onenoteim.exe`。

从全路径派生两个候选串：

- `text2`：文件名里第一个 `.` 之前那一段。`ONENOTE.EXE` → `ONENOTE`。
- `text`：去掉扩展名后各段用空格连接。`Drawboard PDF.UWP.exe` → `Drawboard PDF UWP`。

比对的是进程映像名，不是窗口类名，也不是 AutomationId。

## 白名单

`eraserTrustlist` 是 `CD54PenApp.dll` 的字符串资源，20 行原文：

```
PaintStudio
TopHatch
Sketchable
Maps
NotebookPro
pdfink
Pix2d
FluidMath
StudioApp
InkCalendar
onenoteim
OneNote
iXplain
Xodo
Inkodo
WhiteboardWRT
ScreenSketch
FiiNote
AcAppDaemon
TE
```

入表和比对两侧都 `ToLower()`：

```csharp
// ReadMistouchPreventionList
array = Resources.eraserTrustlist.Split(Environment.NewLine.ToCharArray());
foreach (string text3 in array) { if (!text3.Equals("")) { eraserAppList.Add(text3.ToLower()); } }
```

`onenoteim`（UWP）与 `OneNote`（桌面版）是并列的两条。桌面版 `ONENOTE.EXE` 派生出 `ONENOTE`，
小写后命中 `OneNote.ToLower()`，**桌面版在白名单内**。

`AcAppDaemon` 在表里是因为橡皮提示窗属于宿主进程且会抢前台（见下一节），不放进去的话
500 ms 轮询会立刻把刚进入的橡皮态撤销掉。

另外两张同族的表与橡皮无关，它们是嵌入的文本资源而不是 resx 字符串：
`applicationTrustlist`（`applicationtrustlist.txt`，68 条，防误触）和
`globalPrevention`（`globalprevention.txt`，8 条，软键盘与系统应用的全局防误触）。

## 退出橡皮的三条路径

| 触发 | 动作 | 出处 |
|---|---|---|
| MCU 报 `0x2F` payload=0 | `windowEraser.CloseWindow()` → `CommandSendPenCurrentFunc(0)` | `CallbackPenCurrentFunc` case 0 |
| 500 ms 轮询发现前台离开白名单（仅当窗口在显示） | `CloseWindow()` → `CommandSendPenCurrentFunc(0)` | `PenExitAppListProc` |
| 双击时前台不在白名单 | `CommandSendPenCurrentFunc(0)`，不开窗 | `CallbackPenCurrentFunc` case 1 尾部 |

第二条挂在 `TransferPenMode` 的主循环上，与防误触共用同一次前台探测：

```csharp
private static void PenExitAppListProc(string appSimpleName, string appComleteName)
{
    if (!eraserAppList.Contains(appSimpleName.ToLower()) && !eraserAppList.Contains(appComleteName.ToLower()))
    {
        if (CustomApplicationContext.isEraserWindowShow && windowEraser != null)
        {
            APP_LOGGER.Info("Close EraserWindowShow.", Array.Empty<object>());
            windowEraser.CloseWindow();
        }
        McuInteractTool.CommandSendPenCurrentFunc(0);
    }
}
```

## Window_Eraser 只是提示窗

一个自然的猜测是这个窗口承担了功能——置顶捕获输入、调用 Ink API、或者向触控侧写状态。
都没有。整个类只有构造、拖动、关闭：

```csharp
public Window_Eraser()
{
    InitializeComponent();
    base.Left = SystemParameters.WorkArea.Width / 2.0;
    base.Top = SystemParameters.WorkArea.Height * 0.2;
}

protected override void OnClosed(EventArgs e)
{
    CustomApplicationContext.isEraserWindowShow = false;
    base.OnClosed(e);
}

public void CloseWindow()
{
    base.Dispatcher.Invoke(delegate { Close(); });
}
```

`ShowDialog()` 之后没有任何逻辑，线程委托体就是 `new Window_Eraser()` 加 `ShowDialog()` 两句。

XAML 属性（从 `views/window_eraser.baml` 读出）：144×94、`WindowStyle=None`、
`ShowInTaskbar=False`、`ResizeMode=NoResize`、`AllowsTransparency=True`、`Topmost=True`、
圆角 24、DropShadow，内容是 `Resources/rubber.png` 一张图。**没有 `ShowActivated="False"`**，
所以 `ShowDialog()` 会抢前台，这就是白名单里要放 `AcAppDaemon` 的原因。

## 托管层不做的事

以下都经过全树 grep 确认，`CD54PenApp` 与其余三个笔插件均无：

- 不调用 `THP_Service` 的任何导出。托管代码里 `Thp`/`THP` 零命中；`[DllImport]` 只有
  `Depend\PenService.dll`（56 个）和 `user32`/`kernel32`。
- 不用共享内存、命名管道、Mutex、EventWaitHandle。
- 不注入 HID。`SendInput`、`keybd_event` 零命中。
- 不发窗口消息。全插件只有 `KbdResidentProc.cs` 一处 `PostMessage(hwnd, WM_CLOSE, 0, 0)`，
  属键盘断连提示。
- 不调 Windows Ink。`UIAutomation`、`InkPresenter`、`RadialController` 零命中。
- 不写注册表。`PenUseSettingProc.SetDoubleFuncRegistryValue`（写
  `HKCU\Software\Microsoft\Windows\CurrentVersion\ClickNote\UserCustomization\DoubleClickBelowLock`
  的 `Override` 与 `PenWorkSpaceVerb`）在四个笔插件里**都没有调用者**，配套的
  `PenDoubleFuncType1/2` 枚举同样无人引用。原厂没有走 Windows 自带的笔快捷方式通道。

`CommandSendDoubleFuncChr`（命令码 `0x82`，无参）名字看着像双击功能，实际是埋点。唯一调用点
在 12 分钟一次的统计定时器末尾，紧跟 `CommandSendTouchChr` 和 `CommandSendGlobalPreventionChr`
两批「各应用笔使用时长」之后。`Chr` 是 characteristic，不是 character。

## THP_Service.dll 是 MCU 的第二个客户端

这一点此前不在我们的模型里。`THP_Service.dll` 并不只处理 SPI 触控数据，它自己去找
`HUAWEI MCU GUID_DEVINTERFACE_USBDRIVER` 并打开设备，起自己的读线程
（`AsynchReadThreadProc`）和分发线程（`AsynchProcThreadProc`）。从 `.rdata` 读出的
`[USB]` 标签表，按二进制里的排列顺序：

```
[HID]Find HUAWEI MCU GUID_DEVINTERFACE_USBDRIVER DEVICE, but open failed!
[USB]ReOpenDevice, newHandle        [USB]Unable to find any MCU devices!
[USB]FirstPenConnected!             [USB]FirstUsbStatus()!
[USB]PacketConstructAndSend Error.  [USB]WriteFile failed, error.
[USB]Usb_Start!                     [USB]Usb_Stop!
[USB]AsynchReadThreadProc exited!   [USB]AsynchProcThreadProc
[USB]GetReportBluetoothPenInfo!     [USB]USBD_SW_VERSION recv!
[USB]INIT_PARAM_EVENT               [USB]TP_PEN_MATCH_INFO!
[USB]PenMatchevent11!               [USB]PEN_ROATE_ANGLE
[USB]DEV_CONNECT recv!              [USB]DEV_PAIR_STATUS recv!
[USB]BATTERY_STATUS recv!           [USB]CHARGING_STATUS recv!
[USB]PEN_DOCK_STATUS recv!          [USB]PEN_BATTERY_AFTER_CONN recv!
[USB]PEN_PAIR_DETECT_ACK recv!      [USB]PEN_UPDATE_STATUS recv!
[USB]PEN_KEY_FUNC_GET recv!         [USB]PEN_AC_STATUS
[USB]PEN_CONN_STATUS                [USB]PEN_CUR_STATUS
[USB]PEN_TOUCH_MODE                 [USB]PEN_GLOBAL_PREVENT_MODE
[USB]PEN_SCREEN_STATUS              [USB]PEN_HOLSTER
[USB]PEN_FREQ_JUMP                  [USB]ERASER_TOGGLE
[USB]PEN_REP_PARAM                  [USB]PEN_CURRENT_FUNC
[USB]PEN_GLOBAL_ANNOTATION          [USB]PEN_TYPE_INFO
[USB]ThpReset!
```

`ERASER_TOGGLE` 和 `PEN_CURRENT_FUNC` 是两条不同的消息，各有各的标签。

### 日志：一个之前不知道的观察窗口

`THP_Service.dll` 把上面这些标签逐条写进
`C:\ProgramData\Huawei\HuaweiTHP\Service_LogFile-SYSTEM-<YYYYMMDD>.txt`，按天分文件，
带毫秒时间戳。**只要 `GaokunThpHost` 在跑，这份日志就在写**，因为它加载的正是这个 DLL。

这份日志是判断「MCU 到底发没发某条消息」的权威依据。`THP_Service` 是 MCU 的独立客户端，
它自己开设备、自己收包，不经过我们这侧的任何读者，所以我们的握手、分派、过滤都影响不到
它记下来的东西。我们收不到某条消息时，先看这里能不能分清是 MCU 没发还是我们没收。

日志里还混有 `[Daemon]` 前缀的 ApDaemon 行（`DriverThp::GetStatus`、`UpdateStatus`、
`WaitNotify`，带 `spiScreenStatus`、`PenHibernate`、`PenAttach`）和 `ThpStart` / `ThpStop` /
`Usb_Start` / `Usb_Stop` 的生命周期行，可以直接对齐我们自己的接管时序。

同目录下还有 Himax HAL 的日志 `hx_hal_log_<时间戳>.txt`，逐笔记录
`thp_afe_set_stylus_press PenPressure <值>`，可以用来判断某一时刻笔是否真的在书写。

常用查法：

```powershell
Select-String -Path "C:\ProgramData\Huawei\HuaweiTHP\Service_LogFile-SYSTEM-*.txt" `
    -Pattern "ERASER_TOGGLE|PEN_CURRENT_FUNC" -Context 10,10
```

## ERASER_TOGGLE 是命令回显，不是双击事件

日志里 `ERASER_TOGGLE` 出现得很少，容易被误读成「物理双击的产物」。**这个读法是错的。**

我先前依据时间线判断物理双击会产生 `ERASER_TOGGLE`，后续实测推翻了它：停掉
`GaokunPenHost` 和 `GaokunKeyboardHost`、让 `GaokunThpHost` 独占 MCU 端点之后按双击，
`ERASER_TOGGLE` 一条都不产生。另一处佐证是时间戳本身——那批记录里
`12:11:57:738` 与 `12:11:57:741` 相隔 3 毫秒，不可能是人手的两次按键。

现在证据更强了。发 `CommandSendPenCurrentFunc(1)` 之后，MCU 随即向 `THP_Service` 发
`ERASER_TOGGLE1`，时刻严格对应（实测 `13:14:07:430`）。而在下面那个握手问题修好、物理双击
已经能稳定产生 `0x2F` 之后，**双击仍然不产生 `ERASER_TOGGLE`**，只产生 `0x2F`。两条消息
各自的来源就此确定：`0x2F` 来自用户操作，`ERASER_TOGGLE` 来自我们的命令。

`THP_Service` 收到 `ERASER_TOGGLE1` 之后，笔的行为确实变了——`WM_POINTER` 探针里 `pressure`
恒为 0，笔写不出字。**但 HID 报告里 Invert/Eraser 位始终不置。** 橡皮的断点现在收窄到
`THP_Service` 内部这一格，正在单独逆向。

留下这一节是因为它排除了一整个方向：不要再从日志里找「双击产生的 `ERASER_TOGGLE`」，
那里没有。

### 曾经的另一个误判：以为物理双击不产生任何 MCU 消息

上一版这里还写着「物理双击在当前状态下不产生任何 MCU 消息，侧键功能没设成橡皮时 MCU
不会上报」。**这也是错的，而且错的原因在我们自己这侧。**

真正的原因是没有人对 MCU 做初始化握手。上一次重构删掉了 `PenEventBridge` 的实例化，成员
声明还留着，看不出问题。这个类负责对 MCU 做握手（`0x7101`、两次 `0x7701`、`0x7B` InitParam）
并对每一帧回 ACK。没有它，MCU 不上报任何事件——不是不上报双击，是什么都不上报。把它接回去
之后，物理双击立刻稳定产生 `0x2F PEN_CURRENT_FUNC`。

厂商的 `PenService.dll` 顶不上这个位置：`re-native` 已确认它的 `GetInterruptPipeMsg` 不发
任何包，也就是说它自己不做握手。只加载厂商 DLL 是不够的，握手必须由我们做。

这一条值得留下，因为「MCU 不发」和「我们没收」在日志上长得一样，很容易再判错一次。
分辨方法见上一节：`THP_Service` 的日志不受我们这侧影响，拿它对照即可。

## 尚未查证

以下几项没有事实，不要按推测行事。

- `THP_Service.dll` 收到 `ERASER_TOGGLE` 之后做什么。已知它让笔的压力恒为 0，未知它为什么
  不置 Invert/Eraser 位。这是目前橡皮的唯一断点，要反汇编 handler 才有答案，字符串给不出。
  正在单独逆向。
- `PenService.dll` 里 `0x2F` 的 handler（RVA `0x0ae20`）是否使用托管回调的返回值。
  `-1` 在两条「不该进入橡皮」的路径上出现，分布不像巧合，但没有证据。
- `case 0` 分支为什么在 MCU 已经报 0 之后仍要回一个 `CommandSendPenCurrentFunc(0)`。
  幂等重述和协议回执两种读法都自洽，托管层没有证据能区分。
  （参数语义本身已经清楚：`0x2F` payload 恒为 0、笔不会自己进入橡皮态，所以 `(1)` 是进入
  橡皮的下行命令，`(0)` 是退出与清理，与同族的
  `CommandSendTransferPenMode` / `CommandSendOskPrevensionMode` 一致。）
- 完整的原厂链路走通时是什么样。本机 PC Manager 未运行，所有观察都是在缺少原厂托管层的
  条件下取得的。要建立基准，需要恢复 `HuaweiThpService` 与 PC Manager、把 `KeyFunc` 设成 4、
  在 OneNote 里真按一次双击，再对比同一份 THP 日志与 `WM_POINTER` 探针。

## 相关文档

- `docs/ACCESSORY_CENTER.md` — 选件中心结构、`PenService.dll` 的命令与事件码表
- `docs/penservice_events.md` — 21 个事件码与分发表
- `hal/src/pen/PenService.cpp` — 我们当前的调用方式
