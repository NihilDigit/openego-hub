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

OpenEGo Hub 是面向 HUAWEI MateBook E Go（Windows 11 ARM64）的硬件控制中心，提供触控管理、手写笔与键盘配置、电池电源管理及显示色彩调节等功能。

系统架构由原生 ARM64 服务与 ARM64EC 宿主进程组成，直接调用原厂功能模块执行底层算法，接管硬件交互，支持在停用华为电脑管家及后台服务的情况下正常使用全部硬件特性。

---

## 主要功能

- **触控管理**：支持多点触控与书写防误触，落笔时自动屏蔽手指触摸。
- **手写笔支持**：支持 HUAWEI M-Pencil 压感与倾角检测；支持自定义侧键双击动作（系统笔快捷方式或书写/橡皮擦切换），内置 OneNote 兼容模式。
- **磁吸键盘配置**：支持键盘脱离主机后的无线连接开关控制。
- **电池与电源**：支持智能充电模式与自定义充放电阈值；提供电池健康度、充放电循环次数及剩余续航时间查询。
- **色彩与显示**：支持 sRGB 与 P3 色域切换、色温调节及护眼模式，参数直接通过显示驱动应用面板出厂校准数据。
- **服务管理**：提供华为后台服务的状态监控与启停控制。
- **设备信息**：实时显示手写笔与键盘的电量、充放电状态、磁吸连接状态、固件版本和硬件序列号；显示主机型号、处理器规格、内存容量及系统版本。

---

## 界面展示

### 配件管理
显示手写笔与键盘的电量、连接状态、固件版本及序列号，并提供侧键功能配置。

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/accessories.png" alt="配件管理界面" width="820">
</p>

### 状态托盘与连接提示
常驻任务栏托盘显示配件状态，并在手写笔或键盘连接时显示状态提示。

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/popup-pen.png" alt="手写笔连接提示" width="340">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/popup-keyboard.png" alt="磁吸键盘连接提示" width="340">
</p>

### 电池与电源
提供智能充电模式与自定义充电阈值，并显示电池健康度、循环次数及剩余续航时间。

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/battery.png" alt="电池设置界面" width="820">
</p>

### 色彩与显示
提供色域切换、色温调节与护眼模式配置。

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/screen.png" alt="显示设置界面" width="820">
</p>

### 服务管理
提供华为后台服务的状态监控与启停控制。

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/services.png" alt="服务管理界面" width="820">
</p>

### 设备信息
显示主机型号、处理器规格、内存容量及系统版本等硬件信息。

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/device.png" alt="设备信息界面" width="820">
</p>

---

## 硬件支持

- **主机**：HUAWEI MateBook E Go（Windows 11 ARM64）
- **手写笔**：HUAWEI M-Pencil（支持 CD52、CD54、CD54R、CD54S；其中 CD54R 已完成真机测试）
- **键盘**：华为智能磁吸键盘

---

## 安装与使用

### 前置依赖
运行环境需保留系统原装的华为硬件驱动及华为电脑管家组件，触控、屏幕色彩校准、电池阈值及键盘无线连接均依赖其底层动态链接库。

### 安装步骤
1. 前往 [Releases](https://github.com/NihilDigit/openego-hub/releases/latest) 页面下载最新版安装包 `OpenEGoHubSetup_arm64_vX.Y.Z.msi`。
2. 运行安装程序完成部署（注册系统服务需要管理员权限）。
3. 安装完成后，后台服务将随系统启动，用户可通过开始菜单打开设置中心。

### 升级与卸载
- 安装包自带清理流程：安装前自动移除旧版本（含改名前的早期版本与测试版）遗留的服务、程序文件与快捷方式，无需先手动卸载。
- 用户配置在升级中保留，包括各项设置、日志与已禁用华为服务的还原记录。
- 卸载时触控自动交还华为触控服务；曾通过设置中心禁用的华为后台服务会一并恢复。

### 运行机制
- **触控接管**：程序运行期间接管触控输入通道；程序退出或异常终止时，触控控制权自动归还系统原生触控服务。
- **服务切换**：如需恢复使用华为原厂触控服务，在设置界面中退出 OpenEGo Hub 即可。

---

## 源码构建

### 环境要求
- CMake 3.20 或更高版本
- Ninja 构建系统
- Visual Studio ARM64 MSVC 工具链（`cl.exe`）
- WiX Toolset v4（用于生成 MSI 安装包）

### 构建步骤

构建前需在 PowerShell 中加载 ARM64 编译环境。主构建脚本 `scripts\build.ps1` 会自动配置环境：

```powershell
# 编译 Release 版本
.\scripts\build.ps1 -Config Release

# 编译 Debug 版本并执行自动化测试
.\scripts\build.ps1 -Config Debug -Test
```

在已执行 `vcvarsarm64.bat` 的环境中，可直接使用 CMake Presets：

```powershell
cmake --preset arm64-Release
cmake --build --preset arm64-Release
```

### HAL 模块独立编译
`hal/` 目录下的硬件抽象层与宿主程序需优先独立编译，主工程依赖其生成的 `GaokunHal.lib`：

```powershell
cd hal
.\scripts\build.ps1 -Config Debug
cd ..
```

> 注：核心服务为原生 ARM64 程序；`hal/` 目录下用于加载 x64 DLL 的宿主程序编译为 ARM64EC，由 HAL 专属构建脚本生成。

### 调试与开发流程
在开发机上以管理员权限注册 Debug 服务后，可使用开发脚本进行迭代：

```powershell
# 注册 Debug 服务
scripts\install_debug_service.bat

# 快速更新与重启服务（需管理员权限）
pwsh -File scripts\dev-cycle.ps1

# 恢复运行已安装的 Release 服务
pwsh -File scripts\dev-cycle.ps1 -RestoreRelease
```

### 安装包制作
使用 WiX 生成 ARM64 MSI 安装包：

```powershell
dotnet tool install --global wix
wix extension add -g WixToolset.UI.wixext
wix build -ext WixToolset.UI.wixext -arch arm64 -d BuildVersion=1.2.3 `
    -d BuildOutputDir=build\arm64-Release -d HalOutputDir=hal\build\Release `
    scripts\OpenEGoHubSetup.wxs -loc scripts\zh-CN.wxl `
    -out build\OpenEGoHubSetup_arm64_v1.2.3.msi
```

或直接执行自动化打包脚本：

```powershell
scripts\pack_release_version.bat
```

---

## 项目结构

- `EGoTouchService/`：核心系统服务源码（`Device/` 硬件采集与虚拟 HID 注入、`Tsa/` 厂商算法适配、`Host/` 系统服务接口）。
- `hal/`：硬件抽象层与 ARM64EC 宿主进程，负责隔离并调用华为 x64 动态链接库。
- `Common/`：跨进程通信接口定义、共享内存数据结构及公共配置模块。
- `Tools/`：用户态应用程序，包含托盘监控程序（`EGoTouchTray`）与 WinUI 3 设置界面（`EGoTouchSettings`）。
- `docs/`：协议逆向文档与技术设计规格说明。
- `scripts/`：自动化编译、测试、安装打包与开发辅助脚本。

---

## 致谢与衍生说明

本项目基于 **[EGoTouchRev](https://github.com/awarson2233/EGoTouchRev)**（MIT License，© Detach2233）派生开发，继承了其 Himax 帧采集逻辑、手写笔 MCU 通信机制、VHF 虚拟 HID 注入架构及服务运行框架。完整授权声明参见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

开发过程中参考了以下开源项目及社区成果：

- [MateBook-E-Pen](https://github.com/eiyooooo/MateBook-E-Pen)（by eiyooooo）
- [goodies](https://github.com/matebook-e-go/goodies)（by dantmnf）
- [EgoTools](https://github.com/SaKongA/EgoTools)（by SaKongA）
- [HuaweiPenEraserService](https://github.com/qwqVictor/HuaweiPenEraserService)（by qwqVictor）

---

## 开源协议

本项目基于 [MIT License](LICENSE) 开源。

依据协议条款，任何形式的代码或二进制分发均须保留 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) 中的版权声明。

---

## 免责声明

- **商标与产权**：本项目与 HUAWEI、Himax 及其他相关权利方无官方关联或授权。文档中提及的产品名称、商标及公司名称均属其合法权利人所有。
- **第三方资产**：托盘程序在运行时尝试加载本地已安装华为电脑管家中的手写笔与状态图标资源；未检测到安装时自动回退至内置矢量绘制。受版权保护的原厂素材不包含在本项目的源码或发布包中。
- **风险提示**：本项目涉及底层硬件驱动与系统服务交互，主要用于互操作性研究与技术交流。用户须自行承担使用风险，作者及贡献者对使用过程中可能导致的硬件故障、数据丢失或系统不稳定不承担法律责任。
