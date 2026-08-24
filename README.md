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

HUAWEI MateBook E Go 的触控、手写笔与磁吸键盘驱动，替代华为触控服务，以及 PC Manager 的配件状态与笔设置。全部为 ARM64 原生。

本项目 fork 自 [EGoTouchRev](https://github.com/awarson2233/EGoTouchRev)，触控栈源自该项目，此后经过修改。

---

## 功能

- **触控**：多指触控。写字时手掌压在屏幕上不会误触，用笔时忽略手指。
- **手写笔**：M-Pencil 的压力与倾斜。侧键双击可设为遵循系统笔设置，或切换书写与橡皮擦（后者可启用 OneNote 兼容）。
- **键盘**：可开关「分离后保持无线连接」。
- **设备信息**：笔与键盘的电量、充电与吸附状态、固件版本、硬件版本、序列号。

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/devices.png" alt="设备页" width="720">
</p>

托盘常驻显示配件状态，笔或键盘接入时弹出提示。

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/popup.png" alt="接入提示" width="480">
</p>

设置窗口集中全部开关。

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/settings.png" alt="设置窗口" width="720">
</p>

---

## 支持的设备

- HUAWEI MateBook E Go，Windows 11 ARM64。
- 手写笔：M-Pencil 一代至三代（CD52、CD54、CD54R、CD54S），仅在 CD54R 上实测。
- 键盘：华为智能磁吸键盘，不支持第三方键盘。

---

## 不在范围内

- **固件升级**：需要厂商签名的镜像，刷写失败会导致设备变砖。
- **语音助手集成**
- **全局批注**

---

## 安装

从 Releases 下载 `OpenEGoHubSetup_arm64_vX.Y.Z.msi` 并运行，注册服务需要管理员权限。安装后服务随 Windows 启动，开始菜单中有入口。`OpenEGoHubTestSetup_arm64_*.msi` 在此基础上附带诊断工具。

安装时华为触控服务会被停用，两者不能同时驱动同一块硬件。切换回华为驱动无需卸载，在设置窗口中退出即可。

---

## 从源码构建

需要 CMake、Ninja、ARM64 MSVC 工具链，打包还需要 WiX v4。

`arm64-*` preset 从 `PATH` 解析 `cl.exe`，shell 中需先具备 ARM64 开发者环境。`scripts\build.ps1` 会自行导入：

```powershell
.\scripts\build.ps1 -Config Release           # 配置并构建
.\scripts\build.ps1 -Config Debug -Test       # 构建后跑 ctest
```

执行过 `vcvarsarm64.bat` 之后也可直接用 preset：

```powershell
cmake --preset arm64-Release
cmake --build --preset arm64-Release
```

打包：

```powershell
dotnet tool install --global wix
wix extension add -g WixToolset.UI.wixext
wix build -ext WixToolset.UI.wixext -arch arm64 -d BuildVersion=1.2.3 `
    scripts\EGoTouchSetup.wxs -loc scripts\zh-CN.wxl `
    -out build\OpenEGoHubSetup_arm64_v1.2.3.msi
```

`scripts\dev-cycle.ps1` 停服务、重新构建、再启动，`scripts\verify.ps1` 构建后跑测试并重放录制语料。两者都需要提权 shell。

---

## 目录结构

- `EGoTouchService/`：服务。`Device/` 硬件抽象，`Solvers/` 触控与手写笔管线，`Host/` 系统接口。
- `Common/`：跨进程通道与共享配置。
- `Tools/`：托盘、设置窗口、诊断工作台。
- `docs/`：逆向所得的协议文档。
- `scripts/`：构建、打包与开发脚本。

---

## 致谢

本项目 fork 自 **[EGoTouchRev](https://github.com/awarson2233/EGoTouchRev)**（MIT，© Detach2233），触控栈源自该项目，此后经过修改。其许可声明保留在 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

触控管线以 Chromium 的 ChromeOS 触控栈为对照做过测量，掌抑制阈值即由此重新标定。

另有三个更早在这台设备上做过工作的项目，本项目参考过它们：

- **[MateBook-E-Pen](https://github.com/eiyooooo/MateBook-E-Pen)**，作者 eiyooooo
- **[goodies](https://github.com/matebook-e-go/goodies)**，作者 dantmnf
- **[EgoTools](https://github.com/SaKongA/EgoTools)**，作者 SaKongA

---

## 许可

MIT。见 [LICENSE](LICENSE)。

上游的版权声明与内置 Dear ImGui 的声明保留在 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)，任何形式的再分发（源码或二进制）都须随附该文件。

---

## 声明

本项目与 HUAWEI、Himax 及任何其他商标持有者均无隶属、授权或关联关系。所有产品名与公司名归各自所有者。本项目通过逆向工程开发，用于互操作、研究与教学。

托盘在运行时从已安装的 PC Manager 读取笔的图片与电量图标，PC Manager 不存在时回退到自绘。那些图片属于 HUAWEI，不随本项目分发。

**风险自负。** 本项目替换的是底层硬件驱动。对于由此产生的硬件损坏、数据丢失、系统不稳定，或对第三方服务条款的违反，作者与贡献者不承担任何责任。
