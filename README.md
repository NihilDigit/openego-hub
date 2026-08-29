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

HUAWEI MateBook E Go 的触控、手写笔与磁吸键盘驱动，替代华为触控服务与 PC Manager。

本项目 fork 自 [EGoTouchRev](https://github.com/awarson2233/EGoTouchRev)，采集与注入的骨架源自该项目，此后经过修改。

---

## 功能

- **触控**：多指触控。写字时手掌压在屏幕上不会误触，用笔时忽略手指。
- **手写笔**：M-Pencil 的压力与倾斜。侧键双击可设为遵循系统笔设置，或切换书写与橡皮擦（后者可启用 OneNote 兼容）。
- **键盘**：可开关「分离后保持无线连接」。
- **电池**：充电阈值，可用厂商的智能充电，也可手动设定区间。电池健康、循环次数、剩余时间。
- **屏幕**：色域、色温、护眼模式。取值来自面板的出厂标定，由显示驱动下发，不经软件后处理。
- **服务**：停用华为的后台服务，可随时恢复。
- **设备信息**：笔与键盘的电量、充电与吸附状态、固件版本、序列号；主机型号、处理器、内存、系统版本。

配件页显示笔与键盘的电量、固件与序列号，笔的侧键行为也在这里设定。

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/accessories.png" alt="配件页" width="820">
</p>

托盘常驻显示配件状态，笔或键盘接入时弹出提示。

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/popup-pen.png" alt="手写笔接入提示" width="340">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/popup-keyboard.png" alt="磁吸键盘接入提示" width="340">
</p>

电池页管充电阈值与健康状况，屏幕页调色域、色温与护眼。

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/battery.png" alt="电池页" width="410">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/screen.png" alt="屏幕页" width="410">
</p>

服务页停用或恢复华为的后台服务，设备页列出本机规格。

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/services.png" alt="服务页" width="410">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/device.png" alt="设备页" width="410">
</p>

---

## 触控用的是原厂算法

触控与手写笔没有重新实现。本项目启动一个自己的宿主进程，在其中加载华为原本的 `THP_Service.dll`，数据从 Himax 控制器经原厂算法链直达 Windows：

```
Himax → THP_Service → TSACore → 原厂 VHF → Windows HID
```

因此掌拒、压力、倾斜、笔与手指的仲裁都与原厂一致。替换掉的只是华为那个很薄的 .NET 服务外壳。代价是不能卸载华为驱动，否则触控失效。

接管是租约式的，不是开机就抢。服务启动时触控仍归华为，登录界面和托盘未启动时照常可用；托盘取得租约后才切换，租约断开（托盘退出或崩溃）则自动交还。任一环节失败都不会让触控无人负责。

屏幕色彩、电池阈值、键盘无线连接同样依赖华为的组件，而这些组件编译为 x64。ARM64 进程无法加载 x64 DLL，因此每项能力各自运行在独立进程中，统一放在 `hal/` 下。

细节见 [`hal/README.md`](hal/README.md)。

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

从 Releases 下载 `OpenEGoHubSetup_arm64_vX.Y.Z.msi` 并运行，注册服务需要管理员权限。安装后服务随 Windows 启动，开始菜单中有入口。

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

`hal/` 单独构建，且要先于主项目——主项目链接它产出的 `GaokunHal.lib`，缺了会在配置阶段失败。Debug 与 Release 各需构建一次：

```powershell
cd hal; .\scripts\build.ps1 -Config Debug
```

服务本身是原生 ARM64。需要 ARM64EC 的只有 `hal/` 下那几个加载华为 x64 DLL 的宿主，它们由 hal 自己的构建脚本产出，不经过这里的 preset。

开发机上以管理员身份安装 Debug 服务，随后用 DevCycle 循环：

```powershell
scripts\install_debug_service.bat
pwsh -File scripts\dev-cycle.ps1
```

退出调试并恢复发行服务用 `pwsh -File scripts\dev-cycle.ps1 -RestoreRelease`。
`-NoTray` 只启动服务而不取得租约，因此会按设计继续由华为提供触控。

打包：

```powershell
dotnet tool install --global wix
wix extension add -g WixToolset.UI.wixext
wix build -ext WixToolset.UI.wixext -arch arm64 -d BuildVersion=1.2.3 `
    -d BuildOutputDir=build\arm64-Release -d HalOutputDir=hal\build\Release `
    scripts\OpenEGoHubSetup.wxs -loc scripts\zh-CN.wxl `
    -out build\OpenEGoHubSetup_arm64_v1.2.3.msi
```

安装包里的 `hal\` 宿主取自 `HalOutputDir`，那个目录要先由 `hal\scripts\build.ps1 -Config
Release` 产出。`scripts\pack_release_version.bat` 把构建和打包一次做完，参数不必自己拼。

`scripts\dev-cycle.ps1` 停服务、重新构建、再启动；`scripts\verify.ps1` 构建后跑测试，并挡住「构建没走完、测试却全绿」这一类假成功。两者都需要提权 shell。

---

## 目录结构

- `EGoTouchService/`：服务。`Device/` 采集与注入，`Tsa/` 厂商后端适配，`Host/` 系统接口。
- `hal/`：厂商硬件层。凡是需要加载华为 x64 DLL 的部分都在这里，各自独立进程、编译为 ARM64EC。
- `Common/`：跨进程通道与共享配置。
- `Tools/`：托盘与设置窗口。
- `docs/`：逆向所得的协议文档。
- `scripts/`：构建、打包与开发脚本。

---

## 致谢

本项目 fork 自 **[EGoTouchRev](https://github.com/awarson2233/EGoTouchRev)**（MIT，© Detach2233）。Himax 帧采集、笔的 MCU 传输、VHF 注入与服务骨架都源自该项目；触控算法原先也是，后来换成厂商后端并移除。其许可声明保留在 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

另有几个更早在这台设备上做过工作的项目，本项目参考过它们：

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
