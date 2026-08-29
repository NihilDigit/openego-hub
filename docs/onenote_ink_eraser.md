# 桌面版 OneNote 的橡皮识别通路

目标：查清桌面版 OneNote 靠什么信号判断「现在是橡皮」，据此确定我们要让哪一位置起来。

> 置信度标记
> - **实测**：本机运行结果、注册表、设备树、导入表或二进制内容直接读出
> - **文档**：微软官方文档原文，附链接
> - **推断**：由前两类事实推导，未直接观测

---

## 0. 结论先行

| 问题 | 答案 | 置信度 |
|---|---|---|
| 桌面版 OneNote 读不读 `POINTER_PEN_INFO.penFlags` | 不读。整个 Office 只有 Word 和 Visio 导入 `GetPointerPenInfo` | 实测（导入表） |
| 那它靠什么识别橡皮 | 旧 Tablet PC 的 RealTimeStylus，判据是 `StylusInfo.bIsInvertedCursor` | 实测（CLSID + 字符串） |
| 两条通路的上游是不是同一个信号 | 是。都源自 HID digitizer 的 `Invert(0x3C)` | 文档 + 推断 |
| 让厂商 VHF 置位对桌面版有没有用 | 有用。置 `Invert` 会同时点亮两条通路 | 推断 |
| 厂商 VHF 的描述符缺不缺橡皮位 | 不缺，`Invert` 和 `Eraser` 都声明了 | 实测（HID button caps） |
| 运行时置位了吗 | 没有。`penFlags` 恒为 `NONE`，连 `PEN_FLAG_BARREL` 都没出现过 | 实测（`WM_POINTER` 探针） |
| `THP_Service` 知不知道笔已进入橡皮态 | 知道。同一时刻日志里有 `ERASER_TOGGLE1`，笔的行为也确实变了 | 实测（厂商日志 + 探针） |

断点定位到一层：**`THP_Service` 收到了橡皮状态，也据此改变了笔的行为，但没有把那一位写进 report `0x08`
的 button 字段。** 不是 OneNote 不认，不是描述符缺声明，也不是信号没传到厂商进程。这一格正在单独逆向。

---

## 1. 两条通路，同一个 HID 位

HID digitizer（usage page `0x0D`）里与橡皮相关的四个 usage：

| Usage | 名称 | 语义 |
|---|---|---|
| `0x32` | In Range | 笔在感应范围内 |
| `0x42` | Tip Switch | 笔尖接触 |
| `0x3C` | **Invert** | 笔的姿态表示「打算擦」 |
| `0x45` | **Eraser** | 正在擦 |

从这里往上分成两条：

```
                            ┌─ Win32 pointer:  penFlags 的 PEN_FLAG_INVERTED / PEN_FLAG_ERASER
HID Invert(0x3C)/Eraser(0x45) ─┤     消费者：UWP OneNote、Word、Visio
                            └─ Tablet PC:     倒置游标 → StylusInfo.bIsInvertedCursor
                                  消费者：桌面版 OneNote
```

`penFlags` 的取值见 [Pen Flags](https://learn.microsoft.com/en-us/windows/win32/inputmsg/pen-flags-constants)：
`PEN_FLAG_BARREL` `0x01`、`PEN_FLAG_INVERTED` `0x02`、`PEN_FLAG_ERASER` `0x04`，与 HID 位一一对应。

`StylusInfo`（[rtscom.h](https://learn.microsoft.com/en-us/windows/win32/api/rtscom/ns-rtscom-stylusinfo)）只有三个成员，
第三个是 `BOOL bIsInvertedCursor`——「TRUE if the stylus is upside down」。Tablet PC 平台看到 `Invert`
置位就切换到倒置游标，这个布尔值才为 TRUE。微软文档把 `StylusInRange` 描述为
「a good place to check if the stylus is inverted and if so, switch to eraser mode」。

**分歧只在中间层，上游是同一位。** 这是全篇的枢纽：不需要为两个 OneNote 版本做两套东西。

---

## 2. 桌面版 OneNote 走 RealTimeStylus

本机装的是 `C:\Program Files\Microsoft Office\root\Office16\ONENOTE.EXE`（16.0.20326.20100），
UWP 版没装（`Get-AppxPackage *OneNote*` 只返回 `Microsoft.Office.OneNoteVirtualPrinter`）。

**导入表**。对 `Office16\*.dll` 与 `Microsoft Shared\OFFICE16\*.dll` 全目录跑 `dumpbin /imports`：

- 导入 `GetPointerPenInfo` 的只有 `VISLIB.DLL`（Visio）和 `WWLIB.DLL`（Word）。
- `ONENOTE.EXE` 与 `onmain.dll` 只导入 `GetPointerInfo`、`GetPointerType`、`GetPointerDeviceRects`。
  两者的字符串表里也没有 `GetPointerPenInfo` 这个串，排除 `GetProcAddress` 动态解析。

**CLSID**。`onmain.dll` 里嵌着 RealTimeStylus 的 CLSID `{E26B366D-F998-43ce-836F-CB6D904432B0}`，
字节序列精确匹配；本机 `rtscom.dll` 注册的正是这个类。

**字符串**。`onmain.dll` 的 UTF-16 字符串表里是成套的 RTS 插件架构：

```
CStrokeCollector::CreateSynchronousPlugins / CreateAsynchronousPlugins
CStrokeBuilderPlugin::OnStylusDown / OnStylusUp
CTouchAdapterPlugin::OnStylusDown / OnStylusUp / OnPackets
Disable DynamicRendererManagerPlugin
CJotDynamicRenderer: Inverted stylus (eraser) ignored
CInkInputUser::OnPointerMoved, InRange: |0, Inverted: |1, Barrel: |2 at (|3, |4)
CEraserTool::StartDrag |0 - erase |1
```

`CJotDynamicRenderer: Inverted stylus (eraser) ignored` 是这条通路的实现痕迹：笔倒置时动态渲染器
不落湿墨，擦除交给 `CEraserTool`。

---

## 3. 两个 OneNote 版本

| | 进程 | 输入栈 | 橡皮判据 |
|---|---|---|---|
| OneNote for Windows 10（UWP） | `onenoteim.exe` | WinRT `InkPresenter` / `PointerPoint` | `IsEraser` / `IsInverted`，底层即 `penFlags` |
| OneNote 桌面版（Office 16.x） | `ONENOTE.EXE` + `onmain.dll` | RealTimeStylus（`rtscom.dll`） | `StylusInfo.bIsInvertedCursor` |

`onenoteim` 是 UWP 版的进程名，出处是微软
[OneNote for Windows 10 迁移指南](https://learn.microsoft.com/en-us/microsoft-365-apps/deploy/onenote-for-windows-10-migration-guide)
里的官方迁移脚本，同一份脚本对两个版本各用各的标识：

```powershell
## Terminate the OneNote for Windows 10 app ...
    if (Get-Process -Name "OneNoteIm" -ErrorAction SilentlyContinue)

## Check if OneNote for Windows is installed ...
    $oneNotePath = Join-Path $env:ProgramFiles "Microsoft Office\root\Office16\ONENOTE.EXE"
```

华为插件白名单里出现的是 `onenoteim`，也就是**原厂的橡皮切换只服务 UWP 版，桌面版从来不在白名单里**。
UWP 版已于 2025 年 10 月终止支持。

---

## 4. 按键式橡皮的报告序列要求

M-Pencil 2 没有尾端橡皮，属于「按键式」实现。将来真去置位时，这一节是硬规范。

[Required HID Top-Level Collections](https://learn.microsoft.com/en-us/windows-hardware/design/component-guidelines/required-hid-top-level-collections)
规定电平：

> **Invert** — Eraser button implementations: "When delivering an input report, the bit should be set whenever
> the eraser button is depressed, and the pen is in-range of the digitizer and cleared otherwise."
>
> **Eraser** — Eraser button implementations: "the bit should be set whenever the eraser button is depressed
> and the pen tip is in-contact with the screen, and cleared otherwise. To avoid accidentally activating or
> cancelling the erase functionality in this implementation, it is highly recommended that once the pen tip is
> in contact with the screen, depressing or releasing the erase button should have no impact on the reporting
> of the eraser bit."

`Invert` 和 `Eraser` 在那张表里标为 `Optional`，但「Required for HLK」一列是 `Yes`。

只按上面这段实现会出错。[Windows Pen States](https://learn.microsoft.com/en-us/windows-hardware/design/component-guidelines/windows-pen-states)
的 Special Notes for Eraser Button Implementations 追加了状态迁移约束，全文：

> Unlike tail-end eraser implementations, button-based implementations can physically allow the user to
> activate/deactivate the erase affordance without the pen transitioning through the "out of range" state.
> However, this is not supported by the underlying protocol.
>
> It is highly recommended that, while a pen with an eraser button is in contact with the screen, the eraser
> switch state be persisted until the pen is lifted, regardless of whether the button is pressed or released.
> Accidental eraser button presses during the "Pen is in contact" state, and accidental eraser button releases
> during the "Pen is erasing" state, are common occurrences for users, and the resulting transitions via the
> "Pen is out of range" state can result in a very jarring user experience.
>
> While the pen is within detection range of the digitizer, but not in contact with the screen,
> activation/deactivation of the erase affordance should be honored. However, direct transitions between the
> "Pen is in range" and the "Pen is in range with intent to erase" states are not supported, and in this
> scenario, the pen states must always transition via "Pen is out of range."
>
> For example, if the erase button is pressed while the pen is within detection range of the digitizer, but not
> in contact with the screen, a single input report should be delivered with all switches cleared and with the
> last location where the pen was in-range, followed by continuous reports where the invert switch is SET and
> the in-range switch is SET.
>
> In a reverse example, if the erase button is released while the pen is within detection range of the
> digitizer, but not in contact with the screen, a single input report should be delivered with all switches
> cleared and with the last location where the pen was in-range, with the invert switch SET. This should then
> be followed by continuous reports where the invert switch is clear and the in-range switch is SET.

推导出的报告序列，悬停中按下橡皮键：

```
InRange=1, Invert=0                              悬停
全部开关清零，坐标取最后一次 in-range 的坐标        ← 不能省，省了就是非法迁移
InRange=1, Invert=1                              悬停待擦，持续
```

松开时对称：先发一份全清（`Invert` 仍为 1），再转成 `Invert=0`。

两份文档要合起来看。硬件规范只讲电平，照着它写会做出「悬停中把 `Invert` 从 0 直接翻到 1」，正是
Pen States 说的那条不被支持的迁移。

**违规之后 Windows 做什么，两份文档都没有写**，只说 "not supported by the underlying protocol"。没有查到
任何微软文档描述宿主的实际行为——是丢整份报告、忽略该位，还是状态机停在旧值，目前不知道。要坐实只能
拿我们自己的 VHF 做对照实验：分别造合规序列和违规序列，看 `penFlags` 里 `PEN_FLAG_INVERTED` 是否出现。

---

## 5. 本机实测

**描述符里有橡皮位。** 枚举全部 HID 设备接口，用 `HidP_GetCaps` / `HidP_GetButtonCaps` 读 button caps。
测时 `HuaweiThpService` 处于 STOPPED、`GaokunThpHost` 在跑，所以这个 VHF 就是厂商链经我们的 host 造出来的：

```
=== VID_0000&PID_0000  HID VHF Driver
    \\?\hid#hid_device_system_vhf&col02#2&ae9b9e9&1&0001#{4d1e55b2-f16f-11cf-88cb-001111000030}
    top-level: page=0x0D(Digitizer) usage=0x02(Pen)
    input buttons:
      report=0x08 page=0x0D usage=0x45 Eraser
      report=0x08 page=0x0D usage=0x3C Invert
      report=0x08 page=0x0D usage=0x44 BarrelSwitch
      report=0x08 page=0x0D usage=0x42 TipSwitch
      report=0x08 page=0x0D usage=0x32 InRange
    input values:
      report=0x08 usage=0x51 ContactId (bits=8, 0..1)
      report=0x08 page=0x01 usage=0x30 (bits=16, 0..16000)
      report=0x08 page=0x01 usage=0x31 (bits=16, 0..25600)
      report=0x08 page=0x0D usage=0x30 TipPressure (bits=16, 0..4095)
      report=0x08 page=0x0D usage=0x3D XTilt (bits=16, -9000..9000)
      report=0x08 page=0x0D usage=0x3E YTilt (bits=16, -9000..9000)
```

坐标量程 X `0..16000`、Y `0..25600`，压力 `0..4095`，倾角各 `-9000..9000`（单位 0.01 度）。橡皮位要往
里写时，落点就是这份 report `0x08`。

**运行时恒为 0。** `WM_POINTER` 探针读 `POINTER_PEN_INFO`：正常书写、以及调用厂商 `PenService.dll` 的
`CommandSendPenCurrentFunc(1)` 之后，`penFlags` 都恒为 `NONE`，`penMask` 为 `0x0d`
（`PRESSURE|TILT_X|TILT_Y`）。切到「橡皮」后唯一的区别是 `DOWN` 时 `pressure=0`——笔离开了笔模式，
但没有任何位表示它是橡皮。

**`PEN_FLAG_BARREL` 一次都没出现过。** 侧键是必然被按到的，而 Barrel 没有任何状态机限制，规范里就是
「按下置位、松开清零」。它也恒为 0，说明这份笔报告的 button 字段整体没有被写入方碰过，不是某个位漏了。

**MCU 的 toggle 到得了 `THP_Service`。** 物理双击时 MCU 直发 `ERASER_TOGGLE` 给 `THP_Service`
（`C:\ProgramData\Huawei\HuaweiTHP\Service_LogFile-SYSTEM-<日期>.txt` 里的 `[USB]ERASER_TOGGLE1` /
`ERASER_TOGGLE0`），托管层不参与。

**笔不会自己进入橡皮态。** 侧键事件源修好之后（重构删掉的 `PenEventBridge` 接了回来，它负责对 MCU 握手
并回 ACK），物理双击稳定产生 `0x2F`，payload 恒为 `0`，含义是「当前不是橡皮」。要进橡皮态必须由上层下
`CommandSendPenCurrentFunc(1)`。

**下了命令之后，笔变了而报告没变。** 握手已建立的状态下重发该命令，`THP_Service` 日志在严格对应的时刻
出现 `ERASER_TOGGLE1`，笔也确实停止书写（`pressure` 恒 0），但 `penFlags` 仍恒为 `NONE`，`INVERTED`
一次都没出现。

四条合起来把断点收窄到一层：橡皮状态传到了 `THP_Service`，`THP_Service` 据此改了笔的行为，
**但那一位没有进入 report `0x08` 的 button 字段**。这一格正在单独逆向。

---

## 6. 对代码里那条注释的纠正

`Tools/EGoTouchTray/EGoTouchTray.cpp` 的 `SyncForegroundOneNoteTool` 附近写着「Office 桌面版 OneNote
不消费虚拟笔的 eraser flags」。

按字面成立：它确实不读 `penFlags`。但由此推出「只能靠 UIA 点 Ribbon」是**错的**。桌面版通过
`bIsInvertedCursor` 识别橡皮，而那同样源自 HID 的 `Invert` 位。**让 VHF 置位对桌面版有效**，
UIA 不是唯一出路。

将来若把 `Invert` 置位跑通，那套 UIA hack 就可以删掉。

---

## 7. 判据的适用范围

基于 `penFlags` 的探针对 OneNote 这条路径**只能作否定判据**。

作否定判据有效：RealTimeStylus 没有第二个数据源。它是纯用户态 COM 组件，从 Windows 指针输入栈取数，
而那个栈唯一的输入是 HIDCLASS 解析出的报告；它不认识 `THP_Service`，也拿不到 `ERASER_TOGGLE`。
所以 `penFlags == NONE` 意味着报告里所有 button 位都是 0，`bIsInvertedCursor` 不可能为 TRUE。
旁证：本机没有 `wisptis.exe`，Win8 之后旧 Tablet Input 的独立解析进程已并入系统指针栈，不存在一条
可能与 `penFlags` 分叉的平行解析路径。

这个否定判据在第 5 节用了两次，都有效：它排除掉了「命令发出后橡皮位其实悄悄置上了，只是 OneNote 不认」
这条岔路，才把断点定到 `THP_Service`。

作肯定判据不够：只置 `Eraser(0x45)` 不置 `Invert(0x3C)`，`penFlags` 会出现 `PEN_FLAG_ERASER`，
但 Tablet 平台不一定切换游标，`bIsInvertedCursor` 可能仍为 FALSE。Linux 侧有同构案例——内核补丁
[HID: input: Support devices sending Eraser without Invert](https://lkml.iu.edu/hypermail/linux/kernel/2305.3/08753.html)
描述 XP-Pen Artist 24 不报 `Invert`，导致设备 "permanently stuck with the BTN_TOOL_RUBBER tool after
sending Eraser"，因为 `Invert` 是 "the only usage that can release the tool"。

所以一旦位真的置起来了，验证要换成 RTS 侧观察：`CoCreateInstance` RealTimeStylus，写一个
`IStylusSyncPlugin`，在 `StylusInRange` / `StylusDown` 里打印 `bIsInvertedCursor`。或者直接在
OneNote 里试。

---

## 8. 没查证到的

- **Surface Pen 的橡皮端在桌面版 OneNote 里能不能用，没有找到微软的一手声明。**
  只有[微软 Q&A 的一条已采纳答案](https://learn.microsoft.com/en-us/answers/questions/6f7d5497-b632-4901-b778-e5b595942b0c/customizing-the-stylus-side-buttons-to-erase-in?forum=surface-all&referrer=answers)，
  针对 OneNote 2016 给的解法是勾选控制面板 Pen and Touch 的
  "Use the top of the pen to erase ink (where available)"，勾上橡皮即恢复；以及旧 Tablet PC 文档
  [Providing Erasers](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ms698143(v=vs.85))
  把 Office OneNote 与 Windows Journal 并列，说两者都提供四种橡皮，并写「on a tablet pen that provides
  an eraser end, the user can just flip the pen over and erase」。二者都是旁证，成色低于第 2 节的
  结构性证据。
- **违反第 4 节迁移约束后 Windows 的行为**，见该节末尾。

---

## 附：容易找错的两处本机事实

- 旧 Tablet PC 平台在 `C:\Program Files\Common Files\Microsoft Shared\Ink\`，`InkObj.dll`、`rtscom.dll`
  都在这里。**`System32` 下没有这两个文件**，按旧印象去 `System32` 找会得出「平台没装」的错误结论。
- `HKCU\Software\Microsoft\Wisp\Pen\SysEventParameters\EraseEnable = 1`。这道用户开关（控制面板里的
  "Use the top of the pen to erase ink"）本机已开启，可以从排查清单里划掉。
