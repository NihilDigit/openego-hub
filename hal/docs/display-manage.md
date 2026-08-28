# 原厂「显示管理」逆向记录

本文记录 `C:\Program Files\Huawei\HwLcdEnhancement\` 这一整套组件的行为，重点是我们尚未
实现的部分：色温、护眼模式、自然色彩显示、白点、ICC 与原厂文案。色域（Native / sRGB /
Display P3）已由 `src/display/` 接管，此处只在与其他特性存在耦合时才提及。

结论来源分三类，文中逐条标注：

- **托管 IL**：`Util_WPF.dll`、`MonitorManage.exe`、`LCDOpenAPI.dll`、`LCD_Service.exe`
  经 `ildasm` 反汇编。
- **原生反汇编**：`LCDPlatform.dll`、`OneControlApi.dll` 经 `dumpbin /disasm`。
- **本机实测读取**：注册表 `HKLM\SOFTWARE\Huawei\LCDEnhancement`、`config\` 目录、
  系统色彩目录。读取全程只读，未启用 `LCD_Service`，未向面板下发任何 LUT 或矩阵。

凡属推断而未验证的，均在该段落内显式写明。

## 一、组件结构

### 1.1 模块分层

安装目录里同时存在两代实现，靠 `framework` 属性切换，本机走新框架：

| 层 | 模块 | 类型 | 角色 |
| --- | --- | --- | --- |
| 界面 | `MonitorManage.exe` | .NET x64 / WPF | 「显示管理」窗口本体 |
| 界面启动器 | `MonitorManageStart.exe` | .NET x64 | 右键菜单入口 |
| 服务 | `LCD_Service.exe` | .NET x64 | 开机下发配置、监听换屏与切换用户 |
| 助手 | `LCDAssistant.exe` | .NET x64 | 自然色彩的会话内执行体（本机不用，见 4.3） |
| 对外接口 | `LCDOpenAPI.dll` | .NET x64 | 供 PC 管家调用的窄接口 |
| 公共库 | `Util_WPF.dll` | .NET x64 | 命名空间 `LCDUtil`，全部 P/Invoke 声明在此 |
| 新框架引擎 | `OneControlApi.dll` | 原生 x64 | 色域、色温、护眼、自然色彩、ICC、注册表 |
| 旧框架引擎 | `SetColorDll.dll` | 原生 x64 | 老机型路径，本机不参与 |
| 平台抽象 | `LCDPlatform.dll` | 原生 x64 | Intel / AMD / Qualcomm / Nvidia 各自的下发实现 |
| 色彩管理 | `lcms2.dll` | 原生 x64 | 生成与改写 ICC |

`igfxext.exe` 是 Intel 显卡扩展宿主，本机（高通平台）不会被拉起，属打包冗余。

新旧框架的分界是 `LCDUtil.FrameWorkType`：`FRAMEWORK_TYPE_OLD` 走 `SetColorDll.dll`，
`FRAMEWORK_TYPE_NEW` 走 `OneControlApi.dll`。本机注册表
`HKLM\SOFTWARE\Huawei\LCDEnhancement\FeatureInfo\FrameWorkType = 1`，即新框架。

### 1.2 调用链

以设置色域为例（`LCDOpenAPI.dll` 的 IL）：

```
LCDOpenAPI.ColorFieldManager.SetColorField(mode)
  → LCDUtil.CSharpAdapter.FunctionApi.Init()            // OneControlApi!Init
  → LCDUtil.ColorField.ColorFieldSwitch(newMode, oldMode)
      → OneControlApi!SetColorField(mode)
          → ColorFieldApi::EnableColorFieldMode
              → EnableColorFieldByDriver / ByPcc / ByGamma / ByDdcci
                  → LCDPlatform!PlatformSet3DLut / PlatformSetPccMatrixInfo
                      → IPlatformApi 虚表 → PlatformApiQcom::Set3DLut / SetPccMatrixInfo
                          → qdcmlib_x64.dll
  → LCDUtil.CSharpAdapter.FunctionApi.Fini()
```

`LCDPlatform.dll` 不静态导入 qdcm，而是 `LoadLibraryHelper::LoadLibraryFromSysPath` 动态
加载。失败日志原文为 `Load Qcom library:qdcmlib_x64.dll from sys path failed!`，与我们
已走通的那条路是同一个库。导出符号 `Create_QDCMLibrary` / `Create_QDCMLibrary2` /
`Destroy_QDCMLibrary` / `Destroy_QDCMLibrary2` 说明厂商同样采用虚表绑定，且区分两代接口
（类名 `PlatformApiQcom` 与 `PlatformApiQcomV1`）。

### 1.3 IPlatformApi 虚表

从 `LCDPlatform.dll` 各导出函数末尾的间接调用偏移反推（`dumpbin /disasm`，已核对全部
十四个导出）：

| 偏移 | 方法 |
| --- | --- |
| +0x18 | `SetWhitePoint` |
| +0x20 | `ResetWhitePoint` |
| +0x28 | `Set3DColorTemperature` |
| +0x30 | `Set2DColorTemperature` |
| +0x38 | `ResetColorTemperature` |
| +0x40 | `Set3DLut` |
| +0x48 | `Reset3DLut` |
| +0x50 | `SetPccMatrixInfo` |
| +0x58 | `ResetPccMatrixInfo` |
| +0x60 | `SetGammaLut` |
| +0x68 | `ResetGamma` |

+0x00 至 +0x10 三槽未被导出函数引用，推测为析构与 `Init` / `InitModule`，未验证。

高通实现真正落地的原语只有两条：`PlatformApiQcom::Set3DLut` 与
`PlatformApiQcom::SetPccMatrixInfo`。白点、色温、Gamma 三组接口在高通上没有独立通道，
最终都折算成 PCC 矩阵（见第三节）。这与我们观察到的 `qdcmlib` 只有
`SetPcc` / `SetIgc` / `Set3dLut` 三个可用通道一致；厂商完全没有用 IGC。

## 二、设备适配表

### 2.1 配置来源

新框架的适配表不是磁盘文件，而是嵌在 `Util_WPF.dll` 里的托管资源
`LCDUtil.FeatureManager.config.devices.xml`（`ildasm` 会把它导出到输出目录旁）。
`MonitorManage.exe` 内另有一份同名资源 `MonitorManage.Config.devices.xml`，只有 28 个
条目、结构完全不同、不含任何 GaoKun，属旧框架遗留，本机不生效。

机型串由 `BasicLib!GetDeviceType` 从 SMBIOS 取得，随后在 XML 中按
`.//device[@type='<串>']` 精确匹配（`FeatureManager.InitDeviceInfoImpl` 的 IL）。
表内共 60 个机型，与高通相关的两条是 `GaoKun` 和 `GaoKun3`。

本机是 `GaoKun3`：`config\3DLutGoldenBin\Gaokun3\` 与 `config\icc\Gaokun3\` 是安装包里
唯一存在的机型目录，且注册表中的护眼 `RhineValue = 0.720000` 与 `GaoKun3` 条目一致
（`GaoKun` 是 0.48）。

### 2.2 本机条目全文

```xml
<device type="GaoKun3" displayType="Qualcomm" framework="FRAMEWORK_NEW">
  <protectEye support="true">
    <item type="rhine" priority="true">0.72</item>
  </protectEye>
  <naturalColor support="true" monitorType="Sensor" executionType="platform">
    <algoInfo frameCount="50" frameTime="100" dimmingThreshold="500"
              methodVersion="0" minLuma="2000.0" luminanceThreshold="30">
      <item r="1.0" g="0.9130434782608695" b="0.8188405797101449">2000.0</item>
      <item r="1.0" g="0.9264705882352942" b="0.8566176470588235">3000.0</item>
      <item r="1.0" g="0.9368029739776952" b="0.8884758364312267">4000.0</item>
      <item r="1.0" g="0.9402985074626866" b="0.9067164179104478">5000.0</item>
      <item r="1.0" g="0.9547169811320755" b="0.9358490566037736">6000.0</item>
      <item r="1.0" g="0.9619771863117871" b="0.9581749049429658">7000.0</item>
      <item r="1.0" g="0.9693486590038314" b="0.9923371647509579">8000.0</item>
      <item r="0.9772727272727273" g="0.9583333333333334" b="1.0">9000.0</item>
      <item r="0.9553903345724907" g="0.9405204460966543" b="1.0">10000.0</item>
    </algoInfo>
  </naturalColor>
  <colorField support="true" setType="3dLut" defaultMode="Native"
              checkDisplayerChange="true"
              iccCheckFile="\config\icc\GaoKun3\digest.sha"
              goldenFile="\config\3DLutGoldenBin\GaoKun3\golden.bin">
    <colorFieldMode mode="Native">\config\icc\GaoKun3\native.icc</colorFieldMode>
    <colorFieldMode mode="sRGB">\config\icc\GaoKun3\sRGBICC.icc</colorFieldMode>
    <colorFieldMode mode="P3">\config\icc\GaoKun3\displayP3.icc</colorFieldMode>
  </colorField>
</device>
```

注意条目里**没有** `<whitePoint>`、`<colorTemperature>`、`<dpInOut>`、`<esd>` 四个节点。

### 2.3 特性开关的真实取值

`FeatureManager.InitFeatureDict()` 先给七个特性建默认值，再由 XML 覆盖。IL 显示七项中
只有一项默认为「支持」：

```
featureDict[0 WHITEPOINT]     = new FeatureSupportInfo()              // isSupport = false
featureDict[1 EYE_PROTECT]    = new FeatureSupportInfo()              // false
featureDict[2 TEMPERATURE]    = new FeatureSupportInfo(true, false, false)
featureDict[3 COLOR_FIELD]    = new FeatureSupportInfo()              // false
featureDict[4 NATURAL_COLOR]  = new FeatureSupportInfo()              // false
featureDict[5 DP_IN_OUT]      = new FeatureSupportInfo()              // false
featureDict[6 ESD]            = new FeatureSupportInfo()              // false
```

`ParseColorTemperature` 只会把 `support` 置真，不会置假。因此**色温在缺少
`<colorTemperature>` 节点时反而是开启的**，与 devices.xml 顶部注释所写的
「colorTemperature support 不填该属性时默认为 false」相反。注释是错的，IL 为准。

本机注册表印证了这一点（`LCD_Service` 曾经运行时写入，现服务已禁用，值为最后一次的结果）：

```
HKLM\SOFTWARE\Huawei\LCDEnhancement\FeatureInfo\Support
    WhitePoint = 0    EyeProtect = 1    Temperature = 1    ColorField = 1
    NaturalColor = 1  DpInOut = 0       Esd = 0
HKLM\SOFTWARE\Huawei\LCDEnhancement\FeatureInfo\LcdCheck
    ColorField = 1    其余全 0
HKLM\SOFTWARE\Huawei\LCDEnhancement\FeatureInfo\MultiUser
    全 0
```

结论：本机原厂开放的特性是**色域、色温、护眼模式、自然色彩显示**四项；白点微调、
DP 输入输出、ESD 三项关闭。

### 2.4 其余注册表现状

```
ColorField      SetType = 1 (SET_TYPE_3DLUT)  CurrentMode = 0 (Native)  ResetMode = 0
ColorTemperature  TemperatureType = 255 (COLOR_TYPE_INVALID)
                  TemperatureValue = 175      X = 80.000000   Y = 80.000000
EyeProtect      HandleType = 0 (OLD)  IsEnable = 0  ProtectValue = 185  RhineValue = 0.720000
NaturalColor    IsEnable = 0
Platform        DisplayType = 2 (QUALCOMM)
                currentGammaPath = C:\ProgramData\Comms\HwLcdEnhancement\currentGamma.bin
Printer         SensorCCT = 5000
```

全部在 `HKLM` 下，`HKCU\SOFTWARE\Huawei\LCDEnhancement` 不存在——与 `MultiUser` 全 0 一致。
所有值均为出厂默认（见 3.2 与 4.1 的常量表），说明用户从未改动过这些设置。

## 三、色温

### 3.1 数据通路

托管侧有两条互不相同的下发路径，都在 `LCDUtil.ColorTemperatureBase`（`Util_WPF.dll`）：

**2D 路径** —— `SetTemperatureImpl(slider, type)`：

```csharp
colorBlue   = ConfigApi.GetColorBlueValue(type);
temperature = TransSliderToTemperatureValue(slider);
FunctionApi.Set2DColorTemperature(new ColorTemperature2DInfo(temperature, colorBlue));
```

`TransSliderToTemperatureValue` 的 IL 是纯整数算式：

```
temperature = (93330 - slider * 255) / 366        // 0x16c92, 0xff, 0x16e
```

slider 取值域 0..366，输出 255..0。`SetColor.ini` 里 `CurrentTemperatureSlider = 175` 与
`CurrentTemperature = 133` 正好对上：`(93330 − 175×255) / 366 = 133`。这条路对应的是
`PlatformSet2DColorTemperature`，面向外接华为显示器走 DDC/CI（`LCDPlatform` 导入 `dxva2.dll`），
对内置面板无效。

**3D 路径** —— `SetColorTemperatureImpl(slider, type, x, y)`，内置面板走这条：

```csharp
colorBlue = ConfigApi.GetColorBlueValue(type);
RGB gain  = CoordinateChange.GetRGBGain((float)x, (float)y, (float)circleRadius);
var info  = new ColorTemperature3DInfo(gain.r, gain.g, gain.b, colorBlue, protectValue);
FunctionApi.Set3DColorTemperature(ref info);
// 随后 Fusion(featureType, ref featureInfo, out fusionInfo) 再 Set3DColorTemperature 一次
```

`ColorTemperature3DInfo` 的布局（IL 字段序 + 反汇编中的偏移，两侧一致）：

```c
struct ColorTemperature3DInfo {   // 40 字节
    double   r;              // +0x00
    double   g;              // +0x08
    double   b;              // +0x10
    uint32_t colorBlue;      // +0x18
    double   protectValue;   // +0x20
};
```

### 3.2 色温模型：色轮而非开尔文

`LCDUtil.ColorTemperatureBase` 的静态构造函数给出全部常量：

| 字段 | 值 | 含义 |
| --- | --- | --- |
| `circleRadius` | 80.0 | 色轮半径 |
| `circleXDefault` / `circleYDefault` | 80.0 / 80.0 | 圆心，即中性点 |
| `warmX` / `warmY` | 33.938468 / 53.332989 | 「暖色」预设点 |
| `coldX` / `coldY` | 126.061728 / 106.666672 | 「冷色」预设点 |
| `temperatureSliderDefault` | 175 (0xAF) | 2D 路径默认滑条 |
| `eyeProtectSliderDefault` | 185 (0xB9) | 护眼默认滑条 |
| `eyeProtectDefault` | 0.82 | 护眼保护值兜底 |

所以厂商的「色温」不是一维 K 值，而是**以 (80, 80) 为心、半径 80 的二维色轮上取一点**，
`EColorType` 只有三档语义标签：`COLOR_TYPE_DEFAULT = 0`、`COLOR_TYPE_WARM = 1`、
`COLOR_TYPE_COLD = 2`、`COLOR_TYPE_INVALID = 0xFF`。冷暖两个按钮就是跳到上表的两个预设点，
「还原默认设置」跳回圆心。本机 `TemperatureType = 255`、`X = Y = 80.0`，即从未离开中性点。

`Config.ini` 的 `[WARM_COLD]` 段是这两个预设点的极坐标形式，可直接验算：

```
暖：dx = 33.938468 − 80 = −46.061532   dy = 53.332989 − 80 = −26.667011
    r/80 = 0.66528783…  = WRadiusScale      |atan2(dy,dx)| = 2.6167969 = WAngle
冷：dx = +46.061728      dy = +26.666672
    r/80 = 0.66528783…  = WRadiusScale      2π − atan2 = 5.7583895   = CAngle
```

原生侧由 `IniHelper::GetValue` 读取同一个 ini，`ColorTemperatureTrans::InitAlphaInfo`
读取 `[ALPHA_SETTING]`。

### 3.3 色轮到 RGB 增益

`Util.Tools.CoordinateChange.GetRGBGain(x, y, radius)` 的算法（IL）：

1. `rho = hypot(x − radius, y − radius) / radius`，`rho ≈ 0` 时直接返回 `(1, 1, 1)`。
2. 由 `asin` 求出极角，归一到 0..360 度。
3. `GainCalc(rho, theta)` 把圆周按 60° 切成六段（`THETA_270_TO_330` … `THETA_210_TO_270`），
   每段取相邻两个锚点色做 `ColorInterpo`，`interpo(x, y, a) = a·x + (1−a)·y`，先按段内角度
   插值、再按 `rho` 向白点插值。
4. 六个锚点色由 `ALPHA_SETTING` 的六个分量 `r g b c m y` 参数化。

`ALPHA_SETTING` 的编译期默认全为 0.9；`Config.ini` 覆盖为：

```
AlphaR = 0.94   AlphaG = 0.93   AlphaB = 0.90
AlphaC = 0.93   AlphaM = 0.94   AlphaY = 0.90
```

`GetXYFromRGB` 是它的逆变换，界面用来把已保存的 RGB 反算成色轮上的光标位置。

要做到与原厂逐位一致，需要把 `GainCalc` 的六段分支完整重写；本文只描述结构，**未验证**
每一段的锚点组合顺序。若只需可用而非逐位一致，直接在 UI 上给出 RGB 增益三元组即可，
下发格式完全相同。

### 3.4 高通侧：色温落在 PCC 上

`PlatformApiQcom::Set3DColorTemperature` → `ProcessSetColorTemperature` 的反汇编流程是：
取当前 PCC 矩阵 → 调 `TransferPccMatrixColorInfoToPccMatrix` → 经虚表 `+0x40` 下发。

`TransferPccMatrixColorInfoToPccMatrix(colorInfo, baseMatrix, outPcc)` 的算式，逐条从
`LCDPlatform.dll` 反汇编读出（常量 `0x1800464B8 = 255.0`，`0x180044D80 = 1.0`）：

```c
double t     = (double)colorInfo->colorBlue / 255.0;
       t     = t * (1.0 - colorInfo->protectValue);
double gainR = colorInfo->r;
double gainG = colorInfo->g;
double gainB = colorInfo->b * (1.0 - t);

// baseMatrix 为 9 个 double，outPcc 为 9 个 float
for (int row = 0; row < 3; row++) {
    outPcc[row*3 + 0] = (float)(baseMatrix[row*3 + 0] * gainR);
    outPcc[row*3 + 1] = (float)(baseMatrix[row*3 + 1] * gainG);
    outPcc[row*3 + 2] = (float)(baseMatrix[row*3 + 2] * gainB);
}
```

即 `PCC = M · diag(gainR, gainG, gainB)`。当 `M` 为单位阵时退化为纯逐通道增益，这正是
本机的情形（色域走 3D LUT，PCC 上没有色域矩阵，见 3.5）。

按行列语义的解读依赖 `baseMatrix` 是行主序这一前提——devices.xml 用 `x11 x12 x13 x21…`
命名，属行主序。若实际是列主序，则应读作 `diag · M`。**这一点未实机验证**，但对本机没有
影响，因为基矩阵是单位阵，两种解读结果相同。

`PlatformApiQcom::SetPccColor` 的反汇编是一个五分支跳转表（比较值 0/1/2/3/4），与
`LCDUtil.FeatureType` 的前五项 `WHITEPOINT / EYE_PROTECT / TEMPERATURE / COLOR_FIELD /
NATURAL_COLOR` 一一对应，每个分支调一个独立的处理函数。可以判断平台层为每个特性各自
保存一份 PCC 贡献，再合成为当前矩阵下发。**各分支的合成方式（相乘还是覆盖）未逐条验证。**

### 3.5 色温与色域是叠加，不是互斥

本机 `colorField setType="3dLut"`，色域经 `PlatformSet3DLut` 下发到 3D LUT 通道；色温经
`PlatformSetPccMatrixInfo` 下发到 PCC 通道。两者硬件通道不同，**天然叠加，互不覆盖**。

这与第一代 `GaoKun` 不同——那台机器 `setType="pcc"`，色域本身就是 PCC 矩阵，此时色温的
增益会乘在色域矩阵之上，即 3.4 里 `M` 非单位阵的情形。`GaoKun` 的三组矩阵在 devices.xml
里是明文，可作为交叉验证的参考数据：

```
Native : 单位阵
sRGB   : 0.785549068  0.186090422  0.028360510
         0.033526872  0.878529912  0.000664024
         0.011827291  0.045304280  0.733071368
P3     : 0.950874127  0.014886323  0.034239550
         0.004184100  0.920089318 -0.009270806
        -0.001814872 -0.013107511  0.805782069
```

本机（`GaoKun3`）在 devices.xml 里**没有** PCC 矩阵，色域数据只有 3D LUT 一条路径，
上表不能直接套用到本机面板。

## 四、护眼模式与自然色彩显示

### 4.1 护眼模式（Eye Comfort）

护眼与色温共用 `Set3DColorTemperature` 这一条通路，通过 `ColorTemperature3DInfo` 的
`colorBlue` 与 `protectValue` 两个字段作用于蓝通道。

`colorBlue` 的取值由 `OneControlApi!GetColorBlueValue` 决定，其内部实现（RVA 0x1BD20）
反汇编后只有一句：

```c
return (switchValue == SWITCH_ENABLE) ? 255 : 0;
```

**护眼是纯开关，没有强度可调。** 界面上的护眼滑条不参与 PCC 计算。代入 3.4 的算式：

```
护眼关：colorBlue = 0   → t = 0                → gainB = b
护眼开：colorBlue = 255 → t = 1 − protectValue → gainB = b × protectValue
```

`protectValue` 来自 devices.xml 的莱茵认证值，本机为 **0.72**，与注册表
`EyeProtect\RhineValue = 0.720000` 一致。即护眼开启时蓝通道乘 0.72，红绿不变。

`SetColorTemperatureImpl` 的 IL 中有一处例外：当 `BasicApi.IsDesktopDevice &&
BasicApi.IsHwMonitor` 时 `protectValue` 被硬编码为 0.77。本机是笔电，不会走到。

那条滑条真正作用的是另一条路 —— `ColorTemperatureBase.SetEyeProtectRGBImpl(protectValue)`：
它从 devices.xml 的 `<protectEye>` 里取**面板厂商专属的 CCT→RGB 表**（`<item vendor="BOE">`
之类），按滑条对应的色温值在表内线性插值，得到 RGB 后调 `ConfigApi.SetEyeProtectRGB`。
`GaoKun3` 的 `<protectEye>` 只有一个 `<item type="rhine">0.72</item>`，没有任何
`<param>` 子节点，因此 `eyeProtectParamInfos` 为空、方法在 `Enumerable.Any` 处提前返回
`RET_FALSE`。**本机上这条路是空转的**，只有 `VanGoghH`、`EnzoH` 那批 Intel 机型才有厂商表。

滑条到保护值的换算函数 `TransEyeProtectSliderValue`（RVA 0x1AFC0 → 内部实现 0x1CA60），
反汇编所得，`base` 为 `GetRhineValue()`：

```c
if (!rhineFeatureEnabled)   return base;
if (180 <= s && s <= 195)   return base;                              // 认证平台区
if (s > 195)                return base - (s - 195) * base * 0.10 / 171.0;
else                        return v - s * (1.0 - base) * 0.65 / 180.0,
                                   其中 v = base + (1.0 - base) * 0.65;
```

默认滑条 185 落在平台区内，返回值恰为 `base`。注册表 `ProtectValue = 185` 即此默认值。

### 4.2 界面上的对应关系

护眼开关的资源键是 `EyeProtect_Title`（「护眼模式」），说明文字
`EyeProtect_Description` 为「长期阅读时开启，可有效减少蓝光辐射，预防用眼疲劳。」，
提示 `EyeProtect_Attention` 为「此模式下，屏幕偏黄为正常现象。」——这句提示与
「蓝通道乘 0.72」的实现完全吻合。

### 4.3 自然色彩显示（Natural tone）

资源键 `NaturalColor_Title` = 「自然色彩显示」，
`NaturalColor_Description` = 「根据环境光线，智能调节屏幕色彩以保持一致，提供纸张般的
观阅体验。」

数据来源由 `monitorType` 决定，两条路都在 `OneControlApi` 的 `NaturalColorMsg` 里：

- `monitorType="Sensor"`（本机）：COM 传感器 API。字符串证据包括
  `CoCreateInstance NatureColor SensorManager Faild:`、`GetSensorsByCategory
  SensorCollection Faild:`、`ISensor QueryInterface Faild`、`Light sensor is now ready.`、
  `No permission for the time sensor.`，以及 RTTI `.?AUISensorEvents@@`。即
  `ISensorManager::GetSensorsByCategory(SENSOR_CATEGORY_LIGHT)` 加
  `ISensorEvents::OnDataUpdated` 回调，读环境色温与照度。
- `monitorType="Wmi"`（Intel 机型）：`ReadWmiSensorColor` 走 `ROOT\WMI`，读 `SensorColor`。

算法在 `NaturalColorAdjustAlgorithm`：`InitNaturalColorAdjustAlgorithm` 接收
`NcInfo`（`frameCount=50`、`frameTime=100`、`dimmingThreshold=500`、`methodVersion=0`、
`minLuma=2000.0`、`luminanceThreshold=30`）与 `algoInfo` 里九个 CCT 锚点的 RGB 增益表
（2000K 至 10000K，本机取值见 2.2），`CctDimming` / `GetStepValue` / `StartCctDimming`
负责在 50 帧、每帧 100 ms 内平滑过渡，避免色彩跳变。

`executionType="platform"`（本机）表示由 `OneControlApi` 内部直接下发
（`SetColorTemperByPlatform`）；`executionType` 缺省时为 `EXECUTION_TYPE_LCDASSISTANT`，
由 `LCDAssistant.exe` 在用户会话内执行（`SetColorTemperByLCDAssistant`）。本机不需要
`LCDAssistant.exe`。

**自然色彩不是色温偏移的别名，但共用下发通道。** 两者的 RGB 增益经
`OneControlApi!Fusion(FeatureType, FeatureInfo, out ColorTemperature3DInfo)` 合成
（`FusionAlgorithm::Fusion` / `IsNeedFusion` / `GetColorTemperatureInfo` /
`GetProtectValue`），再一次性下发。`SetColorDll` 侧还有一个同名逻辑的
`MergeCircleCtAndSensorCt(circleR, circleG, circleB, &r, &g, &b)` —— 把色轮色温与
传感器色温合并为一组增益。**Fusion 的具体合成公式未逐条反汇编验证**，从函数签名与
`MergeCircleCtAndSensorCt` 的形参看应是逐通道相乘，需要实机或进一步反汇编确认。

环境光传感器确实参与，且是本机自然色彩的唯一输入源。以上讲的是厂商实现；本机的传感器
当前读不出持续数据，这一项因此无法复刻，实测见第九节。

## 五、白点、锐度、刷新率

### 5.1 白点微调

本机**不支持**。`GaoKun3` 条目没有 `<whitePoint>` 节点，`FeatureType.WHITEPOINT` 默认为
false，注册表 `Support\WhitePoint = 0`。

厂商实现存在但走另一套：`OneControlApi!WhitePointFineTue(WhitePointFineTueValue&)`，
结构体为 `{ char id[100]; ColorFieldMode mode; double r, g, b; }`，即按色彩模式分别保存
一组 RGB 微调，存到 `SOFTWARE\Huawei\LCDEnhancement\ColorField\WhitePointFineTue`。
校准基准由 devices.xml 的 `goldenData` 属性提供，形如
`$HUA01305032506395330730085994155305650339031831314E3A202013BOE#4E`（`CurieM` 条目），
`GaoKun3` 没有这个属性。另有 `SetColorDll!SetWhitePointGoldenData` 与
`SmBiosHelper::GetOemBiosWhitePointInfo`，说明部分机型的白点基准存在 SMBIOS OEM 串里。

界面文案存在完整一套（`ColorField_FineTuningWhitePoint_*`：「微调白点」「白点校准」
「重设」等），但在本机不会显示。

`WhitePointMode` 枚举另有一处用途——自定义色彩模式里选白点：
`WHITEPOINT_DEFAULT = 0`、`D50 = 1`、`D65 = 2`、`DCI = 3`、`DESIGN = 4`。

### 5.2 锐度

**本机不存在这条通路。** 整个安装目录里与锐度相关的符号只有 `HardwareHal.dll` 中的
`ADL_Display_Sharpness_Caps` / `_Get` / `_Info_Get` / `_Set` 四个 AMD ADL 导入名，
是 AMD 独占接口。`SetColor.ini` 里的 `[SHARP] ON-Off = 1 / OnSharpingValue = 5` 属旧框架
遗留，新框架的 `OneControlApi` 没有任何锐度接口，`LCDPlatform` 的高通实现也没有。

### 5.3 刷新率

**不属于这套组件。** `OneControlApi.dll` 与 `LCDPlatform.dll` 的导出表里没有任何刷新率
接口；`HardwareHal.dll` 只有一个只读的 `?GetRefreshRate@VideoCard@@QEAAKXZ`，没有 setter。
任务书里提到的 `SetRefreshRate` 与 `SyncRefreshRateToBIOS` 不在这份 `HardwareHal.dll` 里，
应归 OSD / PC 管家那条线，交由对应方向确认。

## 六、ICC

### 6.1 厂商装了什么

`config\icc\Gaokun3\` 三个文件，均已被安装到系统色彩目录
`C:\Windows\System32\spool\drivers\color\`（本机实测存在，时间戳 2026-02-10）：

| 文件 | 大小 | `desc` 标签 | 白点 XYZ | 红原色 XYZ |
| --- | --- | --- | --- | --- |
| `native.icc` | 572 | `displayP3_D75` | 0.96420, 1.0, 0.82491 | 0.49770, 0.23259, −0.00136 |
| `sRGBICC.icc` | 568 | `sRGB` | 同上 | 0.43604, 0.22249, 0.01392 |
| `displayP3.icc` | 548 | `P3` | 同上 | 0.51497, 0.24112, −0.00105 |

三份都是 `mntr`/`RGB `/`XYZ ` 的矩阵-TRC 显示器配置文件，11 个标签，结构相同。
`native.icc` 的 `desc` 写成 `displayP3_D75` 是厂商的复制粘贴残留——它的原色三角明显宽于
`displayP3.icc`，是面板原生色域，名字与内容不符。三份的 `wtpt` 完全相同（D65 的
0.9642/1.0/0.8249），色域差异全部体现在 `rXYZ`/`gXYZ`/`bXYZ` 与 TRC 上：`sRGBICC.icc` 的
TRC 是 32 字节参数曲线（sRGB 分段），另两份是 16 字节单值伽马曲线。

`digest.sha` 是三份 ICC 的 SHA-256 清单，格式为「64 字符左对齐的文件名 + `:` + 32 字节
原始摘要」，非十六进制文本。校验逻辑在 `IccFileCheck::GetSha256DataFromDigestFile` /
`CompareSha256`，失败时界面弹「色彩文件被篡改」（`ColorField_*IccFalsifyAttention`）。

### 6.2 怎么应用

`ColorFieldApi` 走标准 Windows 色彩管理 API，`OneControlApi.dll` 静态导入 `mscms.dll`
的 `InstallColorProfileW` 与 `WcsAssociateColorProfileWithDevice`：

```
InstallIccFile           → InstallColorProfileW，拷进 iccDestPath
                           （devices.xml 顶部：C:\Windows\System32\spool\drivers\color\）
GetTargetMonitorName     → 经 WMI WmiMonitorConnectionParams
                           WHERE VideoOutputTechnology = 2147483648 定位内置面板
AssociateIccFile         → WcsAssociateColorProfileWithDevice
IsIccAlreadyAssociated   → 幂等检查
ResetOsIccField          → 解除关联
```

`VideoOutputTechnology = 2147483648`（`0x80000000`，即
`DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL`）是厂商用来区分内置屏与外接屏的判据，
与我们「校色只对内置屏生效」的做法思路一致，可以直接借鉴。

`lcms2.dll` 用于生成新 ICC，不用于应用：`ICCProfileOperation::CreateAndSaveIccProfile` /
`CreateUsrDesignIccProfile` / `ModifyICCProfile` / `GamutAssign` / `SetDescriptionTags`，
只在「自定义模式」里按用户填的色域、白点、伽马现场合成一份 ICC。预置的三种模式直接用
`config\icc\` 里的成品，不经过 lcms2。

### 6.3 ICC 与 3D LUT 的关系

两者是**并行的两件事，没有数据依赖**：

- 3D LUT 改的是面板实际输出（经 qdcmlib 下发到显示管线）。
- ICC 改的是操作系统告诉色彩感知应用「这块屏是什么色域」。

`ColorFieldApi::EnableColorFieldMode` 先调 `EnableColorFieldByDriver`（下发 LUT），成功后
再调 `SetOsIccFileWithCheck`（换 ICC 关联）。任何一步失败都会回滚并弹
`ColorField_ColorModeTransFailAttention`（「色彩模式切换失败。」）。

我们现在只做了前一半。要与原厂行为一致，切换色域时需要同步改 ICC 关联，否则
Photoshop、Lightroom 一类做色彩管理的应用仍按旧色域解释画面。三份 ICC 已经在系统目录里，
可以直接复用，不必自己生成。

### 6.4 3D LUT 的两个来源

`ColorField3DLut` 同时支持两条取数路径，这一点值得记录：

- **BIOS 路径**：`ReadBios3DLutData` / `ReadBiosLutDataBlock` / `GetBiosLutData3D` /
  `GetBiosLutDataBlockSize`，字符串常量里有 `DLUT` 与 `EDID`，配合
  `BiosHeaderV1` / `BiosHeaderV2` / `BiosControlHeader` 两代表头解析与
  `IsControlHeaderCheckSumValid` 校验。这就是我们已经在用的 ACPI `DLUT` 表。
- **Golden 文件路径**：`GetGoldenFileLutData3D` / `GetFullGoldenFilePath`，读
  `config\3DLutGoldenBin\Gaokun3\golden.bin`（131072 字节）。

两者之后统一进 `UpdateThreeLutBinFile3D` / `CreateThreeLutBinFile3D`，把每个模式的成品
LUT 缓存成 `C:\ProgramData\Comms\HwLcdEnhancement\{sRGB,P3,Adobe,EBOOK,Gamma}.bin`
与 `Golden{sRGB,P3,Adobe,sGamma}.bin`。`IsLcdChanged` / `IsLcdDataValid` 用 EDID 比对，
换屏后缓存作废重建——这正是 `checkDisplayerChange="true"` 与
`FeatureInfo\LcdCheck\ColorField = 1` 的含义。

本机 `C:\ProgramData\Comms\HwLcdEnhancement\` 只有 `IccFileUpdate.xml`、`SetColor.ini`、
空的 `language.txt`，没有任何 `.bin` 缓存，说明服务禁用后从未生成过。

注意 golden.bin 是 131072 字节（整 128 KiB，无表头），而 ACPI `DLUT` 表是 131124 字节，
两者差 52 字节。**未逐字节比对两份数据是否等价**，若要用 golden.bin 作为 DLUT 缺失时的
兜底，需要先做这项比对。

### 6.5 实测：mscms 的逐显示器关联在本机走不通

6.2 描述的是厂商怎么做的。我们照着做了一遍，**在本机不成立**：mscms 收下写入、校验配置
文件、返回成功，然后把它丢掉。代码已经删除，记录留在这里——「切色域时同步 ICC」是个自然
的想法，没有这一节，下一个人会把这一轮完整重做。

**决定性证据是同一套接口自己的 set 与 get 互相矛盾。** 同一进程内连续调用，参数完全一致
（同作用域、同设备、`CPT_ICC` / `CPST_NONE` / `dwProfileID = 0`）：

```
WcsAssociateColorProfileWithDevice(SYSTEM_WIDE, "displayP3.icc", 接口路径)   ok=1 err=0
WcsSetDefaultColorProfile(SYSTEM_WIDE, 同一设备, CPT_ICC, CPST_NONE, 0)      ok=1 err=0
  紧接着
WcsGetDefaultColorProfile(SYSTEM_WIDE, 同一设备, CPT_ICC, CPST_NONE, 0)      ok=0 err=2
WcsEnumColorProfiles 按该设备过滤                                            count=0
GetICMProfile                                    仍是兜底的 sRGB Color Space Profile.icm
```

写说成功，紧接着读说不存在。

**返回成功不是无差别的。** 把文件名换成一个不存在的，两个 Associate 都返回 `ok=0 err=2`，
即接口确实校验了配置文件，对我们那三份返回 TRUE 是真的接受了。这条对照排除了「关联指向
了不存在的文件、系统于是回退到兜底那份」这个解释。

三份 ICC 本身没有问题：都在 `C:\Windows\System32\spool\drivers\color\`，显式重跑一次
`InstallColorProfileW` 返回 `ok=1 err=0`。`HKLM\...\ICM\RegisteredProfiles` 里只有渲染
意图（`camp`、`ri`、`rip` 之类），不是逐份配置文件的注册表，与这件事无关。

**传统 ICM 那套更直接：`AssociateColorProfileWithDeviceW` 返回 FALSE，连收都不收。** WCS
与传统两套体系都试过，都不行。

**设备名只有一种能用，另外两种会让 mscms 死锁。**

| 设备名 | 行为 |
| --- | --- |
| `\\?\DISPLAY#BOE2873#...#{e6f07b5f-...}` | 正常返回。取自 `DISPLAYCONFIG_TARGET_DEVICE_NAME.monitorDevicePath` |
| `\\.\DISPLAY1` | **挂起不返回**，进程只能杀掉 |
| `\\.\DISPLAY1\Monitor0` | 同上 |

死锁在传统 API 与 WCS API 两套里都会发生，在原生 ARM64 进程里也一样，与 ARM64EC 无关。

**管理员身份下结果完全相同**，权限不是原因。

枚举调用本身没写错：去掉设备过滤时 `WcsEnumColorProfiles` 列出 13 份已安装配置文件，
加上设备过滤才是 0，所以 `count=0` 是真的没有关联。

内置屏判定不必走 6.2 的 WMI。`QueryDisplayConfig` 的同一条 path 上，target 给出
`outputTechnology`（判据仍是 `0x80000000`，与 WMI 的 `VideoOutputTechnology = 2147483648`
是同一个字段），source 给出 GDI 名，一次拿全，不需要再把 WMI 的实例名映射成 mscms 认的
设备名。这部分是有效的，只是下游的关联接口不工作。

**试过并排除的替代路径：**

- `SetICMProfile` 是逐 DC 的，进程退出即失效，不能用作持久关联。
- `SetStandardColorSpaceProfileW` 改的是系统级 sRGB 映射，影响面远超一块显示器。

因此第十节第 6 条「关联后是否需要广播 `WM_SYSCOLORCHANGE`」在本机无从验证——关联本身就
没有生效，广播与否不影响结果。

## 七、色域模式的完整名单与原厂文案

### 7.1 十二种模式

`LCDUtil.ColorFieldMode`（`Util_WPF.dll` IL）：

| 值 | 枚举名 | devices.xml `mode` | 资源键 | 中文 | 英文 |
| --- | --- | --- | --- | --- | --- |
| 0 | `DEFAULT_MODE` | `Native` | `ColorField_NativeMode` | Native | Native |
| 1 | `SRGB_MODE` | `sRGB` | `ColorField_WebModeNew` | 网页和办公 (sRGB) | Internet and Web (sRGB) |
| 2 | `P3_MODE` | `P3` | `ColorField_PhotographModeNew` | 摄影 (P3-D65) | Photography (P3-D65) |
| 3 | `WEB_MODE` | `Web` | `ColorField_WebModeNew` | 网页和办公 (sRGB) | Internet and Web (sRGB) |
| 4 | `DIGITAL_MODE` | `Digital` | `ColorField_DigitalDCIMode` | 数码影院 (P3-DCI) | Digital Cinema (P3-DCI) |
| 5 | `PHOTO_MODE` | `Photo` | `ColorField_PhotographModeNew` | 摄影 (P3-D65) | Photography (P3-D65) |
| 6 | `DESIGN_MODE` | `Design` | `ColorField_DesignModeNew` | 平面设计 (P3-D50) | Design and Print (P3-D50) |
| 7 | `EBOOK_MODE` | `EBook` | `ColorField_EbookMode` | 电子书 | E-books |
| 8 | `HDTV_MODE` | `HDTV` | `ColorField_HDTVMode` | HDTV 视频 (BT.709-BT.1886) | HDTV Video (BT.709-BT.1886) |
| 9 | `DIGITAL_D65_MODE` | `DigitalD65` | `ColorField_DigitalD65Mode` | 数码影院 (P3-D65) | Digital Cinema (P3-D65) |
| 10 | `ADOBE_MODE` | `Adobe` | `ColorField_AdobeMode` | 摄影印刷 (Adobe RGB-D65) | Photography and Printing (Adobe RGB-D65) |
| 11 | `USRDESIGN_MODE` | — | `ColorField_UsrDesignModeInfo_AddMode` | 自定义模式 | Custom mode |
| −1 | `MODE_BUTT` | — | — | 无效值 | — |

枚举到资源键的映射取自 `MonitorManage.FeatureManage.ColorFieldControl.AdjustColorFieldUI`
的 IL（构造一个 `Dictionary<ColorFieldMode, string>`，逐项 `Add`）。注意 1 与 3、2 与 5
指向同一条文案，这是历史包袱：老机型用 `sRGB`/`P3`，新机型用 `Web`/`Photo`，两组枚举值
共存但显示同一个名字。

### 7.2 本机实际可用的档位

`GaoKun3` 的 `<colorField>` 只声明三个 `<colorFieldMode>`：`Native`、`sRGB`、`P3`，
默认 `Native`。`ColorFieldControl` 会按 `colorFieldModeInfoList.Count` 决定 UI 形态——
三项以内用单选按钮，更多则用下拉框（`AdjustColorFieldUI` 那条路）。三项时用的是另一组
文案键：

| 模式 | 名称键 → 值 | 描述键（笔电分支）→ 值 |
| --- | --- | --- |
| Native | `ColorField_NativeMode` → 「Native」 | `ColorFieldNative_Description_NoteBook` → 「屏幕原生色彩显示，更持久的续航。」 |
| sRGB | `ColorField_SrgbMode` → 「sRGB」 | `ColorFieldSrgb_Description_NoteBook` → 「适配 sRGB 色彩空间显示，建议连接适配器使用。」 |
| P3 | `ColorField_P3Mode` → 「P3」 | `ColorFieldP3_Description_NoteBook` → 「适配 P3 色彩空间显示，建议连接适配器使用。」 |

分组标题 `ColorField_Title` = 「色彩模式」（英文 `Color mode`）。

所以本机原厂只有三档，名字就叫 `Native` / `sRGB` / `P3`，与我们已实现的三档完全一致。
另外九种模式属于 `DaqianF`、`MorganG`、`EnzoH` 等 Intel 机型，本机既没有对应的 ICC，
也没有对应的 LUT 数据，无法启用。

### 7.3 文案取自哪里

**不是 `Language.xml`。** 那个 3606 字节的文件只有两条：`display`（「显示管理」）和
`modeswitch`（「显示器功能」），是给资源管理器右键菜单用的，25 种语言。

真正的界面文案在 `MonitorManage.exe` 的 WPF 资源 `MonitorManage.g.resources` 里，每种
语言一个 BAML 流：`zh_cn.baml`、`en_us.baml`、`ja_jp.baml` 等 24 个，另有一个
`zz_zx.baml` 是带 `[MK_xxxxxx]_` 前缀的伪本地化占位。每个 BAML 是一份
`ResourceDictionary`，117 个 `x:Key` → `sys:String` 条目。

提取方法（复现步骤记录在此，便于日后取其他语言）：

1. `ildasm MonitorManage.exe /out=... ` 会把 `MonitorManage.g.resources` 一并落盘。
2. 用 `System.Resources.ResourceReader` 枚举，取出 `zh_cn.baml` 等流。
3. BAML 中键记录的格式是 `20 <recLen> <idx:u16> <strLen> <utf8>`，其中 `recLen` 自
   `recLen` 字节本身算起；值记录的格式是 `03 9C FD 01 11 <recLen> <strLen> <utf8>`，
   两者都用 7 位变长整数。键与值各自连续成块，按出现顺序一一对应。
4. 值块开头会多出一条重复的窗口标题，需要跳过第 2 条值才能与键对齐——这是踩过的坑，
   详见第九节。

语言选择由 `Util_WPF.dll` 的资源 `LCDUtil.LanguageManager.LanguageConfig.xml` 决定，
把系统区域（如 `zh-SG`、`de-AT`）映射到上述 24 种之一，未覆盖的一律回落 `en-US`。

### 7.4 自定义模式

`USRDESIGN_MODE` 是一整套独立功能：用户填模式名、描述、色域（`GamutMode`：
`DEFAULT/SRGB/P3/ADOBE/BT2020/EBU/SMPTEC`）、白点（`WhitePointMode` 或自定义 xy 色坐标）、
伽马（`GammaMode`：`DEFAULT/BT/PURE` 或数值），由 `UsrDesignAlgo::ThreeDLutCal` /
`GammaLutCal` / `CalcColorSpaceConversionRgbToXyz` / `MatInvert` 现场算出 3D LUT 与
Gamma LUT，同时用 lcms2 合成一份 ICC，存到
`C:\ProgramData\Comms\HwLcdEnhancement\User\3dlut` 与注册表
`SOFTWARE\Huawei\LCDEnhancement\ColorField\UserDesign`。

启用条件是 devices.xml 的 `usrDesignMode="true"`，全表只有 `EnzoH` 有。本机不可用。

## 八、可行性判断

### 8.1 能在现有 qdcmlib 通道上实现

**色温调节**。依据：高通侧最终就是一个 `SetPcc`，算式已从 `LCDPlatform.dll` 反汇编逐条
读出（3.4）。我们已有 `SetPcc` 通道，且本机色域走 3D LUT，PCC 上是空的，基矩阵为单位阵，
下发 `diag(gainR, gainG, gainB)` 即可，与色域天然叠加。色轮到 RGB 增益的映射（3.3）
需要重写，或者干脆换成更直观的 UI——只要最终产出 RGB 三元组，下发格式不变。

**护眼模式**。依据：算式确定且极简——蓝通道乘 0.72，红绿不变，无中间档。同一个
`SetPcc` 调用，与色温增益逐通道相乘即可。0.72 这个数取自 devices.xml 的 `GaoKun3` 条目，
是本机面板的莱茵认证值，可以硬编码。

**自然色彩显示**。依据：算法输入是环境光传感器的 CCT，查 2.2 的九点表插值得 RGB 增益，
输出仍是 `SetPcc`。传感器一侧用 Windows 标准
`ISensorManager::GetSensorsByCategory(SENSOR_CATEGORY_LIGHT)`，不依赖任何厂商 DLL。
唯一没吃透的是 `Fusion` 的合成公式（4.3），但从形参看是逐通道相乘，实现时可先按相乘做，
再与原厂效果比对。

~~**切换色域时同步 ICC**~~。**已实现并删除，本机走不通，详见 6.5。** 原本的依据是
`InstallColorProfileW` 与 `WcsAssociateColorProfileWithDevice` 都是公开的 `mscms.dll`
API、三份 ICC 已在系统色彩目录、内置屏判据也明确。这些前提全部成立，唯独关联接口本身
在本机收下写入却不落地，管理员身份下同样如此。

### 8.2 需要新的厂商接口或额外数据

**白点微调**。本机 devices.xml 没有 `goldenData`，`Support\WhitePoint = 0`。要做的话需要
自己定义校准基准，等于重新设计一套功能，不是「实现原厂已有的东西」。技术通道上它同样
落在 PCC 上，可行，但没有出厂数据可依。

**其余九种色域档位**。本机既无对应 ICC 也无对应 LUT 数据（`DLUT` 表只有两个槽位有数据），
要做只能自己算色域变换矩阵与 LUT，属于新开发而非移植。

### 8.3 不可行 / 不适用

**锐度**。依据：整个组件里只有 AMD ADL 的四个符号，高通平台没有对应通道，
`qdcmlib` 也没有暴露任何锐度相关能力。

**刷新率**。依据：不在这套组件的 API 面内（5.3）。归 OSD 方向。

**DP 输入输出 / 显示器功能 / 一体机模式**。依据：`GaoKun3` 未声明 `<dpInOut>`，
`Support\DpInOut = 0`。这套功能面向能反向当显示器用的机型。

**ESD**。同上，未声明，`Support\Esd = 0`。

## 九、踩到的坑

**`ildasm` 的 `/out=` 与 PowerShell 字符串插值冲突。** 写 `"$f.il"` 时 PowerShell 会把
`$f.il` 解析成属性访问，`il` 属性不存在则返回 `$null`，参数变成 `/out=`，`ildasm` 报
「指定了多个输入文件」并打印用法。要写 `"${f}.il"`。这个报错完全不提示真实原因，
容易误以为是 x86 的 `ildasm` 在 ARM64 上跑不起来。

**BAML 键值对齐差一条。** 值块最前面有一条重复的窗口标题（`Display manager` 出现两次），
键表只有 117 条而值有 118 条。按顺序直接 `zip` 会从第 3 条开始整体错位一格，症状是
「NaturalColor_Title = sRGB 模式」这种明显对不上的配对，但头两条和尾一条恰好看起来正常，
很容易漏过。正确做法是丢掉第 2 条值再配对。

**BAML 记录长度的基准点。** 键记录 `20 <recLen> …` 的 `recLen` 是从 `recLen` 字节自身
算起，不是从其后一字节算起。按后者算会每条多跳一个字节，第二条记录起就解析失败。

**7 位变长整数不能用贪心扫描代替。** 长度超过 127 字节的字符串用两字节变长长度，
直接拿单字节当长度去试探会把长度前缀当成正文，得到 `qmAutomatically a` 这类被截断的
串——前两个字符正是被误读的长度字节。必须按记录结构定位，不能扫描。

**devices.xml 的注释与实现不一致。** 顶部写「colorTemperature support 不填该属性时默认
为 false」，但 `InitFeatureDict` 里 `FEATURE_TYPE_TEMPERATURE` 的默认构造是
`FeatureSupportInfo(true, false, false)`，即默认为真。注册表里 `Temperature = 1` 印证了
IL 而非注释。所有从这份 XML 读出的默认值都应回到 IL 里复核。

**`native.icc` 的 `desc` 标签写的是 `displayP3_D75`。** 与文件内容不符（它的原色三角比
`displayP3.icc` 更宽，是面板原生色域）。按 `desc` 判断文件用途会认错。

**注册表里的值是历史快照。** `LCD_Service` 已禁用，`HKLM\SOFTWARE\Huawei\LCDEnhancement`
下的内容是服务最后一次运行时写入的，读到的是当时的判定结果而非当前状态。用它交叉验证
IL 结论是可靠的（本次 `Support\Temperature = 1` 就印证了 IL），但不能当作实时状态。

**`strings` 在本机不存在。** Git Bash 没带 binutils，`strings` 静默返回空，看起来像
「这个 DLL 里没有任何字符串」。要用 python 正则同时扫 ASCII 与 UTF-16LE 两种编码——
`LCDPlatform.dll` 的日志标签是 ASCII，而部分路径是宽字符，只扫一种会漏。

## 十、待实机验证项

以下结论来自静态分析。第 6 条已作废（见 6.5），其余五条在实现阶段仍未触及——实现走的是
单位基矩阵加逐通道增益这条最短路径，没有用到它们。实现阶段真正验掉的事项另见第十一节。

调研阶段全部涉及向面板下发参数，未执行：

1. PCC 基矩阵是行主序还是列主序（3.4）。本机基矩阵为单位阵，两种解读结果相同，
   但若日后把色域也改到 PCC 上，这一点会产生实际差异。
2. `SetPccColor` 五个特性分支的合成方式是相乘还是覆盖（3.4）。
3. `Fusion` 把色温、护眼、自然色彩三组增益合成为一组的具体公式（4.3）。
4. `GainCalc` 六段插值的锚点组合顺序（3.3）。
5. `golden.bin`（131072 字节）与 ACPI `DLUT` 表（131124 字节）的数据是否等价，
   52 字节的差是否全在表头（6.4）。
6. ~~切换色域后调用 `WcsAssociateColorProfileWithDevice` 是否需要额外通知（如广播
   `WM_SYSCOLORCHANGE`）才能让已运行的应用感知到 ICC 变化。~~ 无从验证：关联本身在本机
   就不生效，见 6.5。

## 十一、实现阶段的实机结论

以下是实现色温、护眼、自然色彩三项时在真机上确认的，与第十节那些仍未触及的推断分开记。

**PCC 通道确实作用到面板。** 这一条只能靠人眼：截图走的是合成前的路径，`SetPcc` 之后
读回来的像素不变，软件无从分辨「下发生效」与「对着一个只会返回成功的接口空写」。实测
`--eye-comfort on` 之后屏幕明显偏黄，`--reset` 后恢复。

**护眼就是蓝通道乘 0.72。** 与 4.1 从反汇编读出的算式一致，下发的增益为
`r=1.0000 g=1.0000 b=0.7200`。

**环境光传感器读不出持续数据，这条结论已被推翻。** 先前记的是「传感器可用，色温可读，
实测值 10143K」，后续复测表明那不成立。`tcs3701 front CCT` 在驱动初始化时上报一帧，
之后不再采样：遮光无反应，冷启动无用，WinRT 与 COM 两条栈拿到的是同一份陈旧缓存，
时间戳几十秒不变。`ISensor::GetData` 返回 `S_OK` 而六个字段一律 `E_FAIL`，因为报告里
没有值。

10143K 这个数本身也不可信——它相当于晴天蓝天的漫射光，不像室内读数，多半就是初始化的
那一帧。当时观察到屏幕颜色变化，证明的只是 PCC 下发通道通，与传感器是否工作无关：
「传感器 → 色温 → 增益 → 下发」四段里，真正验证到的只有后三段。

逐条实测排除过的原因：订阅 `ISensorEvents`（有无两种情形输出逐字相同，而首次读到值的
那份代码里根本没有 `SetEventSink`）、`SENSOR_PROPERTY_CURRENT_REPORT_INTERVAL`（读写都
成功，10 ms 与默认 250 ms 无差别）、挑错传感器（`SENSOR_CATEGORY_LIGHT` 下只有一个）、
隐私权限（`location` 为 Allow，`lfsvc` 在跑，状态是 READY 而非 ACCESS_DENIED）、厂商服务
与厂商用户态进程（七个服务与 `HwMdcCenter` 等全部 Running 时照样读不到）、驱动更新与冷
启动（无驱动安装记录，`HiberbootEnabled=0`，真冷启动也一样）。问题在高通传感器驱动一侧，
从 Sensor API 这一层够不着。

同期原厂的显示管理也失效到连色域都切不动，与传感器卡在同一处。诊断入口保留为
`GaokunDisplay --sensor-probe`，它枚举整个类别、摊开每个字段、并对照订阅与不订阅两种
情形。**未验证服务会话（Session 0 / LocalSystem）下是否可用**——传感器的隐私门是逐用户的，
但在传感器本身恢复之前，这一条没有验证的意义。

**三项按逐通道相乘合成，自洽。** 色温 4000K、护眼、自然色彩 10143K 三项同开，实测下发
`r=0.9554 g=0.8811 b=0.6397`，与三组增益逐通道相乘的手算结果逐位一致。这只说明我们这
一侧的实现自洽，**与原厂 `Fusion` 的效果没有比对过**——那需要启用 `LCD_Service`，第十节
第 3 条因此仍然挂着。

**手动色温没有走色轮。** 实现改用 2.2 那张九点 CCT 标定表插值，绕开了第十节第 4 条的
`GainCalc` 锚点顺序。代价是色轮上偏离冷暖轴的点无法表达。副作用是色温与自然色彩共用同
一张表，两者同开时偏暖叠加两次；厂商那边两条曲线不同，但同样相乘，同样会叠加。
