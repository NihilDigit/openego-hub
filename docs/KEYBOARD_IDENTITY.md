# 磁吸键盘身份识别

目标：在设备页上判断「当前接着的是不是华为自家的智能磁吸键盘」，据此显示产品图；能确定型号
时显示对应型号名。

分析对象为本机安装的 PCManager 25 系列（`C:\Program Files\Huawei\PCManager`）与其运行日志
（`C:\ProgramData\Comms\PCManager\log\AccessoryCenter\`）。本机机型 `GK-W7X`（MateBook E，
GaoKun 平台），所接键盘型号 RX0H。

> 置信度标记
> - **实测**：本机运行结果、注册表、设备树、日志或文件内容直接读出
> - **静态分析**：从二进制反汇编 / IL 逐条读出，未运行验证
> - **推断**：由前两类事实推导，未直接观测

---

## 0. 结论先行

| 问题 | 答案 | 置信度 |
|---|---|---|
| 华为磁吸键盘能否与第三方键盘分开 | 能，而且是干净地分开。第三方键盘物理上走不到 MCU 那条通路 | 实测 + 推断 |
| 能否区分 RX0H / RX0I | 能。判据是键盘固件版本串的平台前缀，不是模组 ID | 静态分析（IL 逐条）+ 实测 |
| 「在位 / 已分离 / 无线连接中 / 不存在」能否区分 | 前三种能，「已分离且无线未连上」与「完全不存在」在 MCU 侧同为一种状态，分不开 | 静态分析 + 实测 |

附带一条决定性事实：**华为自己对 RX0H 和 RX0I 用的就是同一张产品图**，两个文件字节相同
（见 4.3）。所以「放一张通用磁吸键盘图」不是妥协，是与原厂一致的做法。

---

## 1. 第一问：能不能与第三方键盘分开

### 1.1 MCU 通路的物理归属（实测）

`docs/KBDMCU_PROTOCOL.md` 里的 interface GUID `{dd0ebedb-f1d6-4cfa-acca-71e66d3178ca}` 在本机
解析到唯一一个设备实例：

```text
HKLM\SYSTEM\CurrentControlSet\Control\DeviceClasses\{dd0ebedb-f1d6-4cfa-acca-71e66d3178ca}
  ##?#USB#VID_12D1&PID_10B8&MI_01#5&178b215&0&0001#{dd0ebedb-...}
```

该设备的完整形态（`Win32_PnPEntity`，实测）：

```text
USB\VID_12D1&PID_10B8            USB Composite Device   Port_#0003.Hub_#0001
  ├─ MI_00  USB 输入设备(HidUsb)   厂商自定义 HID ×2、HID mouse、HID device
  ├─ MI_01  USB device            Service = USBDriver   ← MCU 私有端点，笔和键盘都走这里
  └─ MI_02  USB 输入设备(HidUsb)   HID Keyboard、触摸板、用户控制、Input Configuration
```

`HKLM\SYSTEM\CurrentControlSet\Enum\USB` 下 `VID_12D1` 只有 `PID_10B8` 这一个条目（实测），
挂在内部 Hub 的固定端口上。手写笔与键盘共用这一个接口，而笔在键盘分离时照常工作，
所以这个复合设备是平板内部的 MCU 桥，不是键盘本身（推断）。MCU 固件版本串
`gaokun_bridge_bd 1.0.0.40`（实测，见 `InfoAcAppDaemon.log` 的 `PenMonitorDaemon.NeedExecute`）
里的 bridge 也指向同一结论。

### 1.2 第三方键盘的位置（实测）

本机同时接着一只罗技接收器，设备树里是完全独立的一支：

```text
USB\VID_046D&PID_C548&MI_00 … MI_03
  HID\VID_046D&PID_C548&MI_00\…   HID Keyboard Device
  HID\VID_046D&PID_C548&MI_03\…   符合 HID 标准的触摸板
```

它与 `VID_12D1&PID_10B8` 之间没有任何父子或兄弟关系，也不注册
`{dd0ebedb-...}` 接口。第三方键盘（USB 或蓝牙）要出现在 MCU 通路上，需要华为 MCU 主动
为它代理协议帧，而 MCU 的键盘子系统是为磁吸触点/私有无线链路写的，不存在这条路（推断）。

**所以最初的判断成立**：只有华为磁吸键盘会在 `(DST=0x05, GRP=0x02)` 子系统上产生应答，
`0x12` 连接状态、`0x08` 电量、`0x01` 序列号、`0x31` 分离状态这几条对第三方键盘永远不会出现。

### 1.3 两条需要写进代码的注意事项

**华为自己也有不走这条路的键盘。** `components/accessories_center/config/accessories/accessories.xml`
（实测）里，RX0G「HUAWEI 高键程智能键盘」带 `<deviceId>VID_12D1&PID_10BF</deviceId>`，
而 RX0H / RX0I 没有 `deviceId` 节点：

```xml
<Accessory type="keyboard" layout_style="1" ...>
  <productId>RX0G</productId>
  <deviceId>VID_12D1&PID_10BF</deviceId>
</Accessory>
<Accessory type="keyboard" layout_style="0" ...>
  <productId>RX0H</productId>
</Accessory>
```

RX0G 是普通 USB 键盘，按 VID/PID 识别；磁吸键盘没有自己的 VID/PID，只能按 MCU 通路识别。
两条路互不覆盖。

**MCU 通路上的磁吸键盘不止 RX0H 一种。** `McuDevConnectService.dll` 的
`RegisterDevSubmodByModuleId`（静态分析，RVA `0xd640` 起）对键盘分支逐条比较型号串，
日志前缀直接读出对应关系：

| 型号串 | 日志 | RVA |
|---|---|---|
| `RX03` / `RX04` | `Register Dirac Kbd` | `0xd825` |
| `RX05` | `Register Cezanne Kbd` | `0xd8b4` |
| `RX0H` | `Register Raven Kbd` | `0xd943` |
| `RX0I` | `Register RavenB Kbd` | `0xd9d2` |

其中 RX04 是「HUAWEI Glide Keyboard 创新键盘」，外形与磁吸键盘不同（见 4.2）。所以
「MCU 键盘子系统有应答」只能推出「是华为的一体化键盘」，还推不出「是磁吸键盘那张图」。

---

## 2. 第二问：能不能区分具体型号

上一轮留下的缺口是「宿主拿到 `RX0H` 的路径没查出来」。这一轮查到了，而且这条路我们自己
也能走。

### 2.1 模组 ID 那条路确实是死的（实测，复核上一轮结论）

`InfoAcAppDaemon.log`：`CallbackUpdateKbdModule` 收到的整数是 `0`；MCU 的
`05 00 02 00 02 00 11 00`（`CommandSendGetKeyboardModule`）对这块键盘返回 0。这条不用再试。

### 2.2 真正的判据是固件版本串（静态分析，IL 逐条读出）

`GaokunKeyboardApp.dll` 与 `DiracRKeyboardApp.dll` 里各有一个「按固件版本推型号」的方法。
用 `#US` 堆偏移定位 `ldstr` 后逐字节解 IL：

`DiracRKeyboardApp.dll`，文件偏移 `0x2645` 起（tiny header，代码 46 字节）：

```text
03                      ldarg.3                  ; fwVersion
2d 06                   brtrue.s  +6
72 7b 05 00 70          ldstr     ""             ; 空串直接返回空
2a                      ret
03                      ldarg.3
72 d6 17 00 70          ldstr     "GAOKUN"
6f d0 00 00 0a          callvirt  String::Contains
2c 06                   brfalse.s +6
72 f2 17 00 70          ldstr     "RX0H"
2a                      ret
03                      ldarg.3
72 fc 17 00 70          ldstr     "DIRACR"
6f d0 00 00 0a          callvirt  String::Contains
26                      pop                       ; 比较结果被丢弃
72 b1 00 00 70          ldstr     "RX0I"
2a                      ret
```

即：固件串含 `GAOKUN` → `RX0H`，否则 → `RX0I`。`DIRACR` 那次比较的结果被 `pop` 掉，
是原厂写漏了分支，不影响结论——`DIRACR` 这个常量的存在本身说明 RX0I 键盘的固件串前缀是
`DIRACR`。

`GaokunKeyboardApp.dll` 里对称的一份（文件偏移 `0x9f0` 起）只认 `GAOKUN`：含 `GAOKUN` →
`RX0H`，否则返回空串。

同一处还有一个「推厂商」的方法（`DiracRKeyboardApp.dll` 偏移 `0x2622` 起）：含 `GAOKUN` →
`"Huawei"`，否则空串。与上一轮记的 `GetKeyboardVendorByFwVersion` 对得上。

### 2.3 本机实测值

`InfoAcAppDaemon.log`（实测）：

```text
CallbackUpdateKbdFirmwareVersion   GAOKUN_KBD_BD 1.0.0.39   （长度 22）
CallbackUpdateKbdHardwareVersion   C5210_1                  （长度 7）
CallbackUpdateKbdSerialNo          UQNWY2*****00342
```

`GAOKUN_KBD_BD` 含 `GAOKUN` → `RX0H`，与日志里 `Get kbd modelId charging status:RX0H` 一致。

固件串取自 MCU 的 `05 00 02 00 02 03 11 00`（`CommandSendGetKeyboardFirmwareVersion`，
应答 `byte[5] == 0x03`，取值是字符串）。**这条命令我们自己就能发**，不需要经过任何华为组件。

### 2.4 原生侧的旁证（静态分析）

`KbdConnectNotify.dll`（PCManager 根目录）是另一个走同一个 MCU 端点的组件，它的字符串表里
有一条完整的链路（实测读出）：

```text
Unable to find any MCU devices!
Get Firmware Version Failed
versionNumber = %s
firmwareLength = %d
GetInstance()->systemMoudle = %s
GAOKUN
GaoKun KeyboardConnected CallBack type1!
```

它读固件版本 → 得出 `systemMoudle` → 与 `GAOKUN` 比较 → 走 GaoKun 分支。与 2.2 是同一条判据的
原生实现。

### 2.5 宿主那条路（作为交叉验证，不建议我们走）

上一轮问的「宿主的 `RX0H` 从哪来」，答案是：宿主并不问键盘，它按**主机机型**查表。

`config/IConnectConfig.json`（实测）里键盘条目的匹配键是 `pcProductName`：

```json
{ "devParam": [ { "pcProductName": "GaoKun" } ],
  "info": { "nameEn": "HUAWEI Smart Magnetic Keyboard", "modelId": "RX0H", ... } },
{ "devParam": [ { "pcProductName": "DiracR" } ],
  "info": { "nameEn": "HUAWEI Smart Magnetic Keyboard Compatible with HUAWEI MateBook E",
            "modelId": "RX0I", ... } }
```

主机产品名取自注册表（实测）：

```text
HKLM\SOFTWARE\Huawei\PCManager  ProductName = REG_SZ "GaoKun"
```

与 SMBIOS 对得上（实测）：`Win32_ComputerSystem.Model = GK-W7X`、`SystemFamily = MateBook E`、
`Win32_BaseBoard.Product = GK-W7X-PCB`、机箱类型 32（可分离）。

`config/devicePSIInfo.xml` 里也缓存了一份 `<type>RX0H</type>`（实测），键名是 SN 的哈希。

这条路的问题是判的是主机不是键盘：把 RX0I 键盘接到 GaoKun 主机上，它照样报 RX0H。
**我们应当按 2.2 的固件串判，把机型只当兜底。**

### 2.6 仍然查不到的部分

`McuDevConnectService.dll` 内部通过 `SmartCameraUITrans.dll` 的 `GetProductName` 导出取主机
产品名（静态分析，调用点 RVA `0xf464` `LoadLibraryW` / `0xf4d3` `GetProcAddress` /
`0xf4f0` 调用，缓冲区 `0x64` 字符），随后拿去查一张三元组表。该表的来源没有追到——本机
`McuDevConnectService.dll` 里没有 `GaoKun` 字符串，表很可能来自 `cloudconfig` 下的 XML。
这一段不影响我们的实现，因为我们不走机型这条路。

---

## 3. 第三问：状态区分

### 3.1 可用信号

全部来自 `docs/KBDMCU_PROTOCOL.md` 已确认的分发表，`byte[4] == 0x02`：

| 事件码 | 含义 | 取值 |
|---|---|---|
| `0x12` | 键盘连接状态 | `packet[8] ∈ {1,2,3}` 归一为 1，其余 0 |
| `0x31` | 分离状态 | `packet[8]`：`0` = 已分离，非零 = 已吸附（**已按实测更正**，见下） |

> **更正**：本文原先记为「`1` = 已分离，`0` = 已吸附」。那个结论来自静态分析——插件按 `0x31`
> 的取值在 `Detach.png` 与 `Snapping.png` 之间切换，而哪张图对应哪个值只能推断，推反了。
> 实机验证：键盘吸附在位时 `packet[8]` 为非零。下文凡引用该极性之处一并按此理解。
| `0x08` | 电量 | 百分比 |
| `0x09` | 充电状态 | 1 / 0 |
| `0x35` | 分离后无线连接开关（`byte[4] == 0x00`） | 1 = 允许，0 = 禁止 |

`0x12` 与 `0x31` 都是 MCU 主动推送的（实测：`InfoAcAppDaemon.log` 里
`CallbackUpdateKbdConnectStatus` 出现 148 次、`CallbackUpdateDetachStatus` 出现 23 次，
其中 status 0 有 14 次、1 有 9 次），也可以用 `0x12` / `0x31` 对应的查询命令主动拉一次。

### 3.2 状态映射

| 显示状态 | 判据 | 置信度 |
|---|---|---|
| 已吸附在位 | `0x12` = 1 且 `0x31` = 0 | 静态分析 + 实测 |
| 已分离、无线连接中 | `0x12` = 1 且 `0x31` = 1 | 静态分析（插件用 `Detach.png` / `Snapping.png` 两张图按 `0x31` 切换）+ 实测 |
| 未连接 | `0x12` = 0 | 实测 |
| 完全不存在 | **分不出来** | —— |

原厂在 `0x12` = 0 时的行为是「不显示电量」，日志里能直接读到（实测）：

```text
KbdResidentProc.CallbackUpdateKbdConnectStatus  Kbd connect Status is 0
KbdResidentProc.CallbackKbdBatteryVolumeEvent   Kbd not connected! Do not show BatteryVolume
```

也就是说原厂同样没有区分「分离且未连上」与「从来没有键盘」，它只是在未连接时把数值藏掉。

### 3.3 为什么分不出来

MCU 只知道自己当前有没有和键盘建立链路。键盘被拿走、键盘没电、键盘在无线范围外、
用户从来没买过键盘，对 MCU 都是同一件事：没有链路。`0x31` 分离状态在没有链路时的取值也
无从校验。

可行的补偿手段（推断，未实测）：

- 缓存「本机曾经见过键盘」的证据——只要成功读到过一次序列号（`0x01`）或固件版本（`0x03`），
  就把设备页上的键盘条目保留下来显示为「未连接」；从未读到过则整个条目不显示。
- `0x35` 分离无线连接开关的值与键盘在不在无关（它由 `(DST=0x09, GRP=0x00)` 那个子系统持有，
  推测在平板主控上），所以它不能用来判在位，但可以用来解释「为什么分离后连不上」。

### 3.4 本机日志里的一个坑

`0x12` = 0 在日志里出现 6 次，全部是 1~2 秒后即恢复为 1 的瞬时抖动，集中在息屏/唤醒前后
（实测，例如 `2026-07-27 07:26:19` 报 0、`07:26:21` 报 1）。**不要在收到单次 `0x12` = 0 后
立刻改 UI**，应当加一个去抖窗口（1~2 秒量级）。

---

## 4. 产品图

### 4.1 原厂图在哪（实测）

选件中心主页卡片，散装文件，128×128：

```text
components\accessories_center\res\drawable\cards\RX0H_00.png
components\accessories_center\res\drawable\cards\RX0I_00.png
components\accessories_center\res\drawable\cards\RX0G.png       （高键程键盘，另一张）
```

插件主窗口的高分辨率图嵌在 DLL 的 WPF 资源流里，需要提取：

```text
Lib\Plugins\GaokunKeyboardApp.dll   704×704 (a69b4aa4…)  2160×2160 (dcbf01d1…, 91f4fa8d…)
Lib\Plugins\DiracRKeyboardApp.dll   同上三张，字节相同
```

分离状态另有两张小图：`Resources/Detach.png`、`Resources/Snapping.png`。

### 4.2 型号与图的对应（静态分析，IL 读出）

`GaokunKeyboardApp` 内部维护三张字典，键是型号串：

```text
{ "RX0H": "HUAWEI Smart Magnetic Keyboard", "RX04": "HUAWEIGlideKeyboard" }   名称
{ "RX0H": <标准键盘>,                        "RX04": <创新键盘> }              类型
                                             图片：SmartKbd.png / GlideKbd.png
```

所以 RX04（Glide 创新键盘）与 RX0H 用的是不同的图。**判到 RX04 时不能套磁吸键盘图。**

### 4.3 RX0H 与 RX0I 用的是同一张图（实测）

```text
ffa0166a92fbb2df56635236cfc541da  RX0H_00.png   13490 B  128×128
ffa0166a92fbb2df56635236cfc541da  RX0I_00.png   13490 B  128×128
```

字节完全相同。插件里的 704×704 主图（`a69b4aa4c44cf02b045f4d41bd9a9a3b`）和两张 2160×2160
也在两个插件之间字节相同。只有两张 648×648 的图在两个插件里不同
（Gaokun `0a30c72d` / `013da205`，DiracR `b0bbc5d2` / `b5ffc3ef`）。

结论：**做「一张通用的华为智能磁吸键盘图」，与原厂在主要展示位上的做法完全一致。**

### 4.4 版权

上述 PNG 是华为的资源，不能复制进我们的仓库。可行做法是运行时从已安装的 PCManager 路径读取，
读不到时退回自绘图标。这一条是工程约束，不是逆向结论。

---

## 5. 实现建议

### 5.1 判据（按顺序求值）

```text
第 0 步  MCU 通道能否打开
        CM_Get_Device_Interface_ListW({dd0ebedb-f1d6-4cfa-acca-71e66d3178ca}) 非空
        失败 -> 不是华为 MCU 平台，键盘条目整个不显示

第 1 步  键盘在不在
        0x12 连接状态 == 1（去抖 1~2 秒）
        为 0 -> 若历史上读到过序列号，显示「未连接」；否则不显示条目

第 2 步  型号
        发 05 00 02 00 02 03 11 00，取 0x03 应答里的固件版本串 fw
        fw 含 "GAOKUN"  -> RX0H  华为智能磁吸键盘
        fw 含 "DIRACR"  -> RX0I  华为智能磁吸键盘 适用于 HUAWEI MateBook E
        fw 含 "GLIDE"   -> RX04  华为创新键盘，换另一张图
        其余 / 读不到    -> 走 5.2 的退路

第 3 步  形态
        0x31 分离状态：0 = 已吸附，1 = 已分离（无线连接中）
```

第 2 步的 `GLIDE` 前缀是**推断**：`GaokunKeyboardApp` 只证明了 RX04 对应
`HUAWEIGlideKeyboard`，没有证明 Glide 键盘的固件串以什么开头。本机没有这块键盘，无法实测。
稳妥的写法是把「非 GAOKUN 且非 DIRACR」一律当作未知型号处理，不要猜 GLIDE。

### 5.2 查不出型号时的退路

固件串读不到或前缀不在表里时：

1. 仍然显示磁吸键盘通用图。依据是 4.3：官方对这一系列本来就是一张图；而能走到这一步说明
   MCU 键盘子系统有应答，第三方键盘不可能到这里。
2. 名称降级为不带型号后缀的「华为智能磁吸键盘」。RX0I 的官方名只是在同一个名字后面加了
   「适用于 HUAWEI MateBook E」，去掉后缀不会说错。
3. 可选的二次兜底：读 `HKLM\SOFTWARE\Huawei\PCManager\ProductName`，`GaoKun` → RX0H、
   `DiracR` → RX0I。这是原厂宿主用的判据（2.5），但它判的是主机不是键盘，只适合在固件串
   完全读不到时用来补一个型号名，不应覆盖固件串给出的结论。装了 PCManager 才有这个键值。

### 5.3 不要做的事

- 不要用 `0x00` 模组查询判键盘型号。对磁吸键盘恒返回 0（实测）。
- 不要按 VID/PID 找磁吸键盘。它没有自己的 VID/PID，`accessories.xml` 里 RX0H / RX0I 的
  `deviceId` 节点是空的（实测）。
- 不要在 `PenEventBridge` 之外另开一个句柄跑键盘读线程。同一个 device path 上多读者会互相
  抢包，见 `docs/KBDMCU_PROTOCOL.md` 第 6.3 节。

---

## 6. 复现路径

```text
接口 GUID 归属
  reg query HKLM\SYSTEM\CurrentControlSet\Control\DeviceClasses\{dd0ebedb-...}

主机机型
  Get-CimInstance Win32_ComputerSystem / Win32_BaseBoard / Win32_ComputerSystemProduct
  reg query HKLM\SOFTWARE\Huawei\PCManager /v ProductName

原生侧型号表
  McuDevConnectService.dll  RVA 0xd640 起 RegisterDevSubmodByModuleId
  KbdConnectNotify.dll      文件偏移 0x59000-0x5b400 字符串表

托管侧型号判定
  解析 PE 的 #US 堆得到 ldstr 令牌，再在 .text 里搜 72 <token> 定位 IL
  GaokunKeyboardApp.dll  文件偏移 0x9f0
  DiracRKeyboardApp.dll  文件偏移 0x2645

实测取值
  C:\ProgramData\Comms\PCManager\log\AccessoryCenter\AccessoryApp\InfoAcAppDaemon.log
```

本机没有 capstone 的 ARM64 轮子，上述反汇编是按字节模式匹配（`lea` 的 rip 相对寻址、
`ldstr` 的令牌）加人工解码得出的，没有用反汇编器。
