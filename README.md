<p align="center">
  <img src="Assets/brand/openego-hub-256.png" alt="OpenEGo Hub" width="88">
</p>

<h1 align="center">OpenEGo Hub</h1>

<p align="center">HUAWEI MateBook E Go 硬件控制中心</p>

<p align="center">
  <a href="https://github.com/NihilDigit/openego-hub/releases/latest"><img src="https://img.shields.io/github/v/release/NihilDigit/openego-hub?style=flat-square&display_name=tag&label=Release&labelColor=334155&color=317AF7" alt="Release"></a>
  <a href="#硬件支持"><img src="https://img.shields.io/badge/Platform-Windows%2011%20ARM64-64748B?style=flat-square&labelColor=334155" alt="Platform"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/NihilDigit/openego-hub?style=flat-square&label=License&labelColor=334155&color=64748B" alt="License"></a>
</p>

<p align="center"><sub><strong>中文</strong> | <a href="README.en.md">English</a></sub></p>

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Assets/screenshots/overview-dark.avif">
    <source media="(prefers-color-scheme: light)" srcset="Assets/screenshots/overview-light.avif">
    <img src="Assets/screenshots/overview-light.avif" alt="概览界面" width="820">
  </picture>
</p>

OpenEGo Hub 以原生 ARM64 实现 MateBook E Go 的日常硬件管理，涵盖触控与手写笔、智能磁吸键盘、电池充电限制及屏幕显示设置。停用华为电脑管家的常驻 x64 服务后，上述硬件功能仍可正常使用。

> [!IMPORTANT]
> 系统中需保留原装华为驱动与华为电脑管家安装目录。触控、手写笔、键盘、充电限制与屏幕校色均需调用电脑管家附带的 DLL，卸载电脑管家会导致相应功能失效。电脑管家的后台服务与登录自启项可在「服务」页停用，无需常驻运行。

---

## 主要功能

- **概览与快捷面板**：汇总主机硬件信息、电源状态及手写笔、磁吸键盘电量；左键点击托盘图标弹出快捷控制面板，用于切换常用功能、电源模式与屏幕色温。
- **触控与手写笔**：支持多点触控、书写防误触及 HUAWEI M-Pencil 压感与倾角检测；支持配置笔侧键双击动作（系统笔快捷方式或切换书写与橡皮擦），内置桌面版 OneNote 兼容模式。
- **智能磁吸键盘**：显示吸附状态、充放电状态、电量与固件版本，控制键盘分离后的无线连接，并在手写笔或键盘接入时显示顶部状态提示。
- **电池与电源**：集成 Windows 11 节能模式与电源模式设置，支持智能充电与六档充电上限（50%–100%），实时显示充放电功率、电压、容量、循环次数与电池健康度。
- **屏幕与色彩**：切换内屏刷新率（120 Hz / 60 Hz）、出厂校准色域（原生 / sRGB / Display P3）、色温与护眼模式，内置色彩与灰阶对照条。
- **服务管理**：支持停用或恢复华为电脑管家后台服务与登录自启项；运行期间接管触控，退出、卸载或异常终止时自动交还华为触控服务。
- **更新与诊断**：支持定期检查更新并在应用内完成下载、SHA-256 校验与静默安装，支持打包导出运行日志。

---

## 界面展示

### 快捷面板与连接提示

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Assets/screenshots/flyout-dark.avif">
    <source media="(prefers-color-scheme: light)" srcset="Assets/screenshots/flyout-light.avif">
    <img src="Assets/screenshots/flyout-light.avif" alt="快捷控制面板" width="340">
  </picture>
</p>

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Assets/screenshots/popup-pen-dark.avif">
    <source media="(prefers-color-scheme: light)" srcset="Assets/screenshots/popup-pen-light.avif">
    <img src="Assets/screenshots/popup-pen-light.avif" alt="手写笔连接提示" width="340">
  </picture>
  &nbsp;&nbsp;
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Assets/screenshots/popup-keyboard-dark.avif">
    <source media="(prefers-color-scheme: light)" srcset="Assets/screenshots/popup-keyboard-light.avif">
    <img src="Assets/screenshots/popup-keyboard-light.avif" alt="磁吸键盘连接提示" width="340">
  </picture>
</p>

### 配件管理

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Assets/screenshots/accessories-dark.avif">
    <source media="(prefers-color-scheme: light)" srcset="Assets/screenshots/accessories-light.avif">
    <img src="Assets/screenshots/accessories-light.avif" alt="配件管理界面" width="820">
  </picture>
</p>

### 电池与屏幕

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Assets/screenshots/battery-dark.avif">
    <source media="(prefers-color-scheme: light)" srcset="Assets/screenshots/battery-light.avif">
    <img src="Assets/screenshots/battery-light.avif" alt="电池设置界面" width="404">
  </picture>
  &nbsp;
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Assets/screenshots/screen-dark.avif">
    <source media="(prefers-color-scheme: light)" srcset="Assets/screenshots/screen-light.avif">
    <img src="Assets/screenshots/screen-light.avif" alt="屏幕设置界面" width="404">
  </picture>
</p>

### 服务与设备

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Assets/screenshots/services-dark.avif">
    <source media="(prefers-color-scheme: light)" srcset="Assets/screenshots/services-light.avif">
    <img src="Assets/screenshots/services-light.avif" alt="服务管理界面" width="404">
  </picture>
  &nbsp;
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Assets/screenshots/device-dark.avif">
    <source media="(prefers-color-scheme: light)" srcset="Assets/screenshots/device-light.avif">
    <img src="Assets/screenshots/device-light.avif" alt="设备信息界面" width="404">
  </picture>
</p>

---

## 硬件支持

- **主机**：HUAWEI MateBook E Go（Windows 11 ARM64）
- **手写笔**：HUAWEI M-Pencil（支持 CD52、CD54、CD54R、CD54S，已在 CD54R 真机验证）
- **键盘**：HUAWEI 智能磁吸键盘

---

## 安装与使用

1. 前往 [Releases](https://github.com/NihilDigit/openego-hub/releases/latest) 下载最新版 `OpenEGoHubSetup_arm64_vX.Y.Z.msi`。
2. 运行安装程序（注册系统服务需管理员权限）。
3. 安装完成后后台服务随系统启动，可通过任务栏托盘图标或开始菜单打开控制中心。

### 升级与卸载

- 安装程序会自动替换旧版服务与程序文件，无需预先卸载；用户配置、日志及华为组件的停用状态均会保留。
- 控制中心内确认更新后，后台服务自动完成下载、校验与静默安装，完成后重新拉起托盘与界面。
- 卸载时触控自动交还华为触控服务，此前在控制中心停用的华为后台服务与登录自启项同步恢复。

---

## 源码构建

### 环境要求

- Windows 11 ARM64
- Visual Studio 2022（ARM64 与 ARM64EC MSVC 工具链、MSBuild）
- CMake 3.20+ 与 Ninja
- WiX Toolset v7（仅打包 MSI 安装包时需要）

### 编译与打包

`hal/` 需先于主工程编译：

```powershell
# 1. 编译 HAL（Debug 或 Release）
.\hal\scripts\build.ps1 -Config Release

# 2. 编译主工程（Debug 配置可附加 -Test 运行测试）
.\scripts\build.ps1 -Config Release

# 3. 打包 Release MSI（依次编译 HAL、主工程并生成 build\OpenEGoHubSetup.msi）
scripts\pack_release_version.bat
```

本机部署与调试：

```powershell
# 将构建产物部署到 C:\Program Files\OpenEGoHub
pwsh -File scripts\deploy.ps1 -Config Release

# 注册 Debug 服务后迭代调试（需管理员权限）
scripts\install_debug_service.bat
pwsh -File scripts\dev-cycle.ps1
```

---

## 项目结构

- `EGoTouchService/`：原生 ARM64 系统服务，负责触控切换、配件状态同步与电源管理。
- `hal/`：硬件抽象层，以及用于加载华为 x64 DLL 的 ARM64EC 辅助进程。
- `Common/`：跨进程共享内存协议、配置定义与公共工具库。
- `Tools/`：用户态组件，包含托盘程序（`EGoTouchTray`）、WinUI 3 控制中心（`EGoTouchSettings`）与安装清理工具（`EGoTouchCleanup`）。
- `docs/`、`hal/docs/`：协议逆向记录、生命周期设计与底层模块分析文档。
- `scripts/`：构建、测试、部署与 MSI 打包脚本。

---

## 致谢

本项目派生自 **[EGoTouchRev](https://github.com/awarson2233/EGoTouchRev)**（MIT License，© Detach2233），第三方组件与版权声明见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

开发过程中参考了以下开源项目与社区成果：

- [MateBook-E-Pen](https://github.com/eiyooooo/MateBook-E-Pen)（by eiyooooo）
- [goodies](https://github.com/matebook-e-go/goodies)（by dantmnf）
- [EgoTools](https://github.com/SaKongA/EgoTools)（by SaKongA）
- [HuaweiPenEraserService](https://github.com/qwqVictor/HuaweiPenEraserService)（by qwqVictor）

---

## 开源协议

本项目基于 [MIT License](LICENSE) 开源。任何形式的源码或二进制分发均须保留 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) 中的版权声明。

---

## 免责声明

- **商标与产权**：本项目与 HUAWEI、Himax 及其他相关权利方无任何官方关联或授权。提及的产品名称、商标与公司名称均归各自权利人所有。
- **第三方资产**：托盘程序与控制中心在运行时按需从本机已安装的华为电脑管家目录读取设备渲染图与状态图标，未检测到时自动回退至内置矢量或系统图标；受版权保护的原厂素材不包含在本仓库或安装包中。
- **风险提示**：本项目涉及底层硬件驱动与系统服务交互，仅用于互操作性研究与技术交流。使用本软件产生的硬件故障、数据丢失或系统不稳定风险由用户自行承担，作者及贡献者不承担相关法律责任。
