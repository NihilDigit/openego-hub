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

<p align="center"><a href="README.md">中文</a> | English</p>

OpenEGo Hub is a hardware control centre for the HUAWEI MateBook E Go on Windows 11
ARM64, covering touch, pen and keyboard configuration, battery and power management, and
display colour.

It is built as a native ARM64 service alongside ARM64EC host processes. Those hosts call
the vendor's own modules for the low-level algorithms and take over the hardware, so
every feature here keeps working with Huawei PC Manager and its background services
disabled.

> [!IMPORTANT]
> A complete installation of Huawei PC Manager must be retained on the system.
> Touch, accessory status, battery thresholds, and colour management directly rely
> on low-level DLLs shipped with PC Manager; removing related components will cause
> the corresponding features to fail.
>
> This dependency introduces no background overhead: OpenEGo Hub disables the
> official x64 services of PC Manager and removes its logon autostart entries, so
> they no longer remain resident in the background, eliminating the original
> emulation overhead. Required DLLs are loaded on demand by this project's host
> processes.

---

## Features

- **Touch.** Multi-touch with palm rejection while writing; finger input is suppressed
  while the pen is down.
- **Pen.** Pressure and tilt for the HUAWEI M-Pencil. The side button's double-tap is
  configurable — either the system pen shortcut, or switching between writing and the
  eraser — and a OneNote compatibility mode is built in.
- **Keyboard.** Control over whether the detachable keyboard keeps its wireless link once
  detached.
- **Battery and power.** Smart charging or a charge threshold of your own, plus battery
  health, cycle count and remaining runtime.
- **Colour and display.** sRGB and Display P3 gamuts, colour temperature and an eye
  comfort mode. The values reach the panel through the display driver, from its factory
  calibration.
- **Services.** Status and start/stop control for Huawei's background services.
- **Device information.** Live battery level, charge state, attach state, firmware
  version and serial number for the pen and the keyboard; model, processor, memory and OS
  build for the machine itself.

---

## Screenshots

### Accessories

Battery level, connection state, firmware version and serial number for the pen and the
keyboard, along with the side-button configuration.

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/accessories.png" alt="Accessories page" width="820">
</p>

### Tray and connection prompts

The tray sits in the notification area with accessory status, and raises a prompt when
the pen or the keyboard connects.

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/popup-pen.png" alt="Pen connection prompt" width="340">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/popup-keyboard.png" alt="Keyboard connection prompt" width="340">
</p>

### Battery and power

Smart charging or a charge threshold of your own, with battery health, cycle count and
remaining runtime.

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/battery.png" alt="Battery page" width="820">
</p>

### Colour and display

Gamut, colour temperature and the eye comfort mode.

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/screen.png" alt="Display page" width="820">
</p>

### Services

Status and start/stop control for Huawei's background services.

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/services.png" alt="Services page" width="820">
</p>

### Device information

Model, processor, memory and OS build for the machine itself.

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/device.png" alt="Device page" width="820">
</p>

---

## Supported hardware

- **Machine.** HUAWEI MateBook E Go, Windows 11 ARM64.
- **Pen.** HUAWEI M-Pencil — CD52, CD54, CD54R and CD54S; only CD54R has been tested on
  real hardware.
- **Keyboard.** The HUAWEI smart magnetic keyboard.

---

## Installation

### Prerequisites

The factory Huawei drivers and PC Manager have to stay installed. Touch, panel colour
calibration, the charge threshold and the keyboard's wireless link all rely on DLLs that
come with them.

### Steps

1. Download the latest `OpenEGoHubSetup_arm64_vX.Y.Z.msi` from the
   [Releases](https://github.com/NihilDigit/openego-hub/releases/latest) page.
2. Run the installer. Registering the system service requires administrator rights.
3. The service starts with Windows from then on, and the settings window opens from the
   Start menu.

### Upgrading and uninstalling

- The installer cleans up before it installs: services, program files and shortcuts left
  by any previous version — including pre-rename and test builds — are removed
  automatically, with no manual uninstall needed first.
- User configuration survives an upgrade: settings, logs, and the records needed to
  restore any Huawei services and autostart entries you disabled.
- Uninstalling hands touch back to the Huawei touch service and re-enables any Huawei
  background services and logon autostart entries that were disabled from the
  settings window.

### How it behaves

- **Touch handover.** The program holds the touch input path while it runs. On exit or on
  a crash, touch returns to the system's own touch service.
- **Going back.** To hand touch back to Huawei's service, quit OpenEGo Hub from the
  settings window.

---

## Build from source

### Requirements

- CMake 3.20 or newer
- Ninja
- The Visual Studio ARM64 MSVC toolchain (`cl.exe`)
- WiX Toolset v4, for the MSI

### Building

The ARM64 build environment has to be loaded into the PowerShell session first;
`scripts\build.ps1` does that itself:

```powershell
# Release build
.\scripts\build.ps1 -Config Release

# Debug build, followed by the test suite
.\scripts\build.ps1 -Config Debug -Test
```

In a shell that has already run `vcvarsarm64.bat`, the presets work directly:

```powershell
cmake --preset arm64-Release
cmake --build --preset arm64-Release
```

### The HAL builds separately

The hardware abstraction layer and its host processes under `hal/` build on their own,
and have to go first: the main project links the `GaokunHal.lib` they produce.

```powershell
cd hal
.\scripts\build.ps1 -Config Debug
cd ..
```

> The service itself is native ARM64. Only the hosts under `hal/` that load Huawei's x64
> DLLs are compiled as ARM64EC, and they come from the HAL's own build script.

### Development loop

Register the Debug service once from an elevated shell, then iterate with the development
scripts:

```powershell
# Register the Debug service
scripts\install_debug_service.bat

# Rebuild and restart it (needs administrator rights)
pwsh -File scripts\dev-cycle.ps1

# Restore the installed Release service
pwsh -File scripts\dev-cycle.ps1 -RestoreRelease
```

### Packaging

WiX produces the ARM64 MSI:

```powershell
dotnet tool install --global wix
wix extension add -g WixToolset.UI.wixext
wix build -ext WixToolset.UI.wixext -arch arm64 -d BuildVersion=1.2.3 `
    -d BuildOutputDir=build\arm64-Release -d HalOutputDir=hal\build\Release `
    scripts\OpenEGoHubSetup.wxs -loc scripts\zh-CN.wxl `
    -out build\OpenEGoHubSetup_arm64_v1.2.3.msi
```

Or let the packaging script do the whole thing:

```powershell
scripts\pack_release_version.bat
```

---

## Layout

- `EGoTouchService/`: the system service. `Device/` for acquisition and virtual HID
  injection, `Tsa/` for the vendor algorithm adapter, `Host/` for the system interfaces.
- `hal/`: the hardware abstraction layer and its ARM64EC hosts, which isolate and drive
  Huawei's x64 DLLs.
- `Common/`: cross-process channel definitions, shared-memory layouts and common
  configuration.
- `Tools/`: the user-session applications — the tray (`EGoTouchTray`) and the WinUI 3
  settings window (`EGoTouchSettings`).
- `docs/`: reverse-engineered protocol documentation and design notes.
- `scripts/`: build, test, packaging and development scripts.

---

## Credits

This project is derived from **[EGoTouchRev](https://github.com/awarson2233/EGoTouchRev)**
(MIT License, © Detach2233) and inherits its Himax frame acquisition, the pen's MCU
protocol, the VHF virtual HID injection layer and the service runtime. The full licence
notice is in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

The following projects and community work were consulted along the way, with thanks:

- [MateBook-E-Pen](https://github.com/eiyooooo/MateBook-E-Pen) (by eiyooooo)
- [goodies](https://github.com/matebook-e-go/goodies) (by dantmnf)
- [EgoTools](https://github.com/SaKongA/EgoTools) (by SaKongA)
- [HuaweiPenEraserService](https://github.com/qwqVictor/HuaweiPenEraserService) (by qwqVictor)

---

## Licence

Released under the [MIT License](LICENSE).

Per its terms, any redistribution in source or binary form has to keep the copyright
notices in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

---

## Disclaimers

- **Trademarks.** This project has no official affiliation with, or endorsement from,
  HUAWEI, Himax or any other rights holder. Product names, trademarks and company names
  belong to their respective owners.
- **Third-party assets.** At runtime the tray tries to load the pen and status icons from
  a locally installed Huawei PC Manager, and falls back to built-in vector drawing when it
  is not present. Those copyrighted vendor assets are in neither the source nor the
  releases.
- **Use at your own risk.** This project works against low-level hardware drivers and
  system services, and exists for interoperability research and technical exchange. You
  assume the risk of using it; the authors and contributors accept no liability for
  hardware faults, data loss or system instability arising from it.
