<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/brand/openego-hub-256.png" alt="" width="96">
</p>

<h1 align="center">OpenEGo Hub</h1>

<p align="center">
  <a href="https://github.com/NihilDigit/openego-hub/releases/latest"><img src="https://img.shields.io/github/v/release/NihilDigit/openego-hub?display_name=tag&label=release" alt="Release"></a>
  <a href="https://github.com/NihilDigit/openego-hub/actions/workflows/build.yml"><img src="https://img.shields.io/github/actions/workflow/status/NihilDigit/openego-hub/build.yml?branch=main&label=build" alt="Build"></a>
  <img src="https://img.shields.io/badge/Windows%20on%20ARM-ARM64-0078D4?logo=arm&logoColor=white" alt="Windows on ARM">
  <a href="LICENSE"><img src="https://img.shields.io/github/license/NihilDigit/openego-hub" alt="License"></a>
</p>

<p align="center">中文 | <a href="README.en.md">English</a></p>

HUAWEI MateBook E Go 的控制中心，替代华为电脑管家：手写笔、磁吸键盘、电池、屏幕与触控服务。

---

## 功能

- **触控**：多指触控与掌拒。笔在屏时忽略手指。
- **手写笔**：M-Pencil 的压力与倾斜。侧键双击两种行为：遵循系统笔设置，或书写与橡皮擦互切（后者含 OneNote 兼容）。
- **键盘**：「分离后保持无线连接」开关。
- **电池**：充电阈值，厂商的智能充电与手动区间二选一。电池健康、循环次数、剩余时间。
- **屏幕**：色域、色温、护眼模式。取值来自面板的出厂标定，由显示驱动下发，不经软件后处理。
- **服务**：华为后台服务的停用与恢复。
- **设备信息**：笔与键盘的电量、充电与吸附状态、固件版本、序列号；主机型号、处理器、内存、系统版本。

配件页：笔与键盘的电量、固件与序列号，以及侧键行为。

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/accessories.png" alt="配件页" width="820">
</p>

托盘常驻显示配件状态。笔或键盘接入时弹出浮窗。

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/popup-pen.png" alt="手写笔接入提示" width="340">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/popup-keyboard.png" alt="磁吸键盘接入提示" width="340">
</p>

电池页为充电阈值与电池健康，屏幕页为色域、色温与护眼。

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/battery.png" alt="电池页" width="410">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/screen.png" alt="屏幕页" width="410">
</p>

服务页为华为后台服务的开关，设备页为本机规格。

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/services.png" alt="服务页" width="410">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/device.png" alt="设备页" width="410">
</p>

---

## 支持的设备

- HUAWEI MateBook E Go，Windows 11 ARM64。
- 手写笔：M-Pencil 一代至三代（CD52、CD54、CD54R、CD54S），仅 CD54R 实测。
- 键盘：华为智能磁吸键盘，不支持第三方键盘。

---

## 安装

从 Releases 下载 `OpenEGoHubSetup_arm64_vX.Y.Z.msi` 运行。服务注册需要管理员权限；安装后随 Windows 启动，入口在开始菜单。

**华为驱动与 PC Manager 需保留**，卸载后触控失效：触控、屏幕色彩、电池阈值、键盘无线连接都复用其中的原厂组件。

安装过程停用华为触控服务，两者不能同时驱动同一块硬件。切换回华为驱动无需卸载，在设置窗口中退出。

---

## 触控：原厂算法

触控与手写笔并非重新实现。本项目加载华为原本的 `THP_Service.dll`，数据从 Himax 控制器经原厂算法链直达 Windows：

```
Himax → THP_Service → TSACore → 原厂 VHF → Windows HID
```

掌拒、压力、倾斜、笔与手指的仲裁因此与原厂一致，替换的只是华为的 .NET 服务外壳。

本程序运行期间接管触控，退出或异常终止后自动交还华为。登录界面与未启动本程序时，触控由华为提供，照常可用。

---

## 不在范围内

- **固件升级**：需要厂商签名的镜像，刷写失败会导致设备变砖。
- **语音助手集成**
- **全局批注**

---

## 从源码构建

需要 CMake、Ninja、ARM64 MSVC 工具链，打包另需 WiX v4。

`arm64-*` preset 从 `PATH` 解析 `cl.exe`，shell 中需先具备 ARM64 开发者环境。`scripts\build.ps1` 自行导入：

```powershell
.\scripts\build.ps1 -Config Release           # 配置并构建
.\scripts\build.ps1 -Config Debug -Test       # 构建后跑 ctest
```

执行过 `vcvarsarm64.bat` 之后也可直接用 preset：

```powershell
cmake --preset arm64-Release
cmake --build --preset arm64-Release
```

`hal/` 单独构建，且先于主项目：主项目链接其产出的 `GaokunHal.lib`，缺失时配置阶段失败。Debug 与 Release 各需一次：

```powershell
cd hal; .\scripts\build.ps1 -Config Debug
```

服务为原生 ARM64。ARM64EC 只用于 `hal/` 下加载华为 x64 DLL 的那几个宿主，它们由 hal 自己的构建脚本产出，不经过此处的 preset。

开发机以管理员身份安装 Debug 服务，之后进入 DevCycle 循环：

```powershell
scripts\install_debug_service.bat
pwsh -File scripts\dev-cycle.ps1
```

退出调试并恢复发行服务用 `pwsh -File scripts\dev-cycle.ps1 -RestoreRelease`。
`-NoTray` 只启动服务、不取得租约，触控按设计仍由华为提供。

打包：

```powershell
dotnet tool install --global wix
wix extension add -g WixToolset.UI.wixext
wix build -ext WixToolset.UI.wixext -arch arm64 -d BuildVersion=1.2.3 `
    -d BuildOutputDir=build\arm64-Release -d HalOutputDir=hal\build\Release `
    scripts\OpenEGoHubSetup.wxs -loc scripts\zh-CN.wxl `
    -out build\OpenEGoHubSetup_arm64_v1.2.3.msi
```

安装包中的 `hal\` 宿主取自 `HalOutputDir`，该目录先由 `hal\scripts\build.ps1 -Config
Release` 产出。`scripts\pack_release_version.bat` 一次完成构建与打包，无需手写参数。

`scripts\dev-cycle.ps1` 停服务、重新构建、再启动。`scripts\verify.ps1` 构建后运行测试，并拦截「构建未走完、测试却全绿」的假成功。两者需要提权 shell。

---

## 目录结构

- `EGoTouchService/`：服务。`Device/` 采集与注入，`Tsa/` 厂商后端适配，`Host/` 系统接口。
- `hal/`：厂商硬件层。凡需加载华为 x64 DLL 的部分都在这里，各自独立进程，编译为 ARM64EC。
- `Common/`：跨进程通道与共享配置。
- `Tools/`：托盘与设置窗口。
- `docs/`：逆向所得的协议文档。
- `scripts/`：构建、打包与开发脚本。

---

## 致谢

本项目 fork 自 **[EGoTouchRev](https://github.com/awarson2233/EGoTouchRev)**（MIT，© Detach2233）。Himax 帧采集、笔的 MCU 传输、VHF 注入与服务骨架均来自该项目，其许可声明保留在 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

另有几个更早在这台设备上的项目，本项目参考过：

- **[MateBook-E-Pen](https://github.com/eiyooooo/MateBook-E-Pen)**，作者 eiyooooo
- **[goodies](https://github.com/matebook-e-go/goodies)**，作者 dantmnf
- **[EgoTools](https://github.com/SaKongA/EgoTools)**，作者 SaKongA
- **[HuaweiPenEraserService](https://github.com/qwqVictor/HuaweiPenEraserService)**，作者 qwqVictor

---

## 许可

MIT。见 [LICENSE](LICENSE)。

上游的版权声明保留在 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)，任何形式的再分发（源码或二进制）都须随附该文件。

---

## 声明

本项目与 HUAWEI、Himax 及任何其他商标持有者均无隶属、授权或关联关系。所有产品名与公司名归各自所有者。本项目通过逆向工程开发，用于互操作、研究与教学。

托盘在运行时从已安装的 PC Manager 读取笔的图片与电量图标，PC Manager 不存在时回退到自绘。那些图片属于 HUAWEI，不随本项目分发。

**风险自负。** 本项目替换的是底层硬件驱动。对于由此产生的硬件损坏、数据丢失、系统不稳定，或对第三方服务条款的违反，作者与贡献者不承担任何责任。
