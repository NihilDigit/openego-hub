<p align="center">
  <img src="Assets/brand/openego-hub-256.png" alt="OpenEGo Hub" width="88">
</p>

<h1 align="center">OpenEGo Hub</h1>

<p align="center">Hardware control centre for HUAWEI MateBook E Go</p>

<p align="center">
  <a href="https://github.com/NihilDigit/openego-hub/releases/latest"><img src="https://img.shields.io/github/v/release/NihilDigit/openego-hub?style=flat-square&display_name=tag&label=Release&labelColor=334155&color=317AF7" alt="Release"></a>
  <a href="#supported-hardware"><img src="https://img.shields.io/badge/Platform-Windows%2011%20ARM64-64748B?style=flat-square&labelColor=334155" alt="Platform"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/NihilDigit/openego-hub?style=flat-square&label=License&labelColor=334155&color=64748B" alt="License"></a>
</p>

<p align="center"><sub><a href="README.md">中文</a> | <strong>English</strong></sub></p>

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Assets/screenshots/overview-dark.avif">
    <source media="(prefers-color-scheme: light)" srcset="Assets/screenshots/overview-light.avif">
    <img src="Assets/screenshots/overview-light.avif" alt="Overview page" width="820">
  </picture>
</p>

OpenEGo Hub implements day-to-day hardware management for the MateBook E Go natively on ARM64, covering touch and stylus input, the smart magnetic keyboard, battery charge limits, and display settings. All hardware features remain functional with Huawei PC Manager's resident x64 services disabled.

> [!IMPORTANT]
> Keep the factory Huawei drivers and Huawei PC Manager installation directory on the system. Touch, stylus, keyboard, charge limits, and display calibration load DLLs shipped with PC Manager, and uninstalling PC Manager breaks the corresponding features. PC Manager's background services and logon autostart entries can be disabled on the Services page instead of remaining resident.

---

## Features

- **Overview & quick flyout.** Summarizes machine specs, power telemetry, and live pen and keyboard battery levels; left-clicking the tray icon opens a customizable quick flyout with toggles, power mode selection, and an optional colour temperature slider.
- **Touch & HUAWEI M-Pencil.** Multi-touch and palm rejection while writing, with pressure and tilt output, configurable side-button double-tap actions (system pen shortcut or writing/eraser toggle), and built-in desktop OneNote compatibility.
- **Smart magnetic keyboard.** Live attach state, charging state, battery percentage, and firmware version, wireless-on-detach control, and top-centre prompts when the pen or keyboard connects.
- **Battery & power.** Integrates Windows 11 Energy Saver and Power Mode alongside vendor smart charging and six manual charge limits (50%–100%), with live wattage, voltage, capacity, cycle count, and battery health readouts.
- **Display & colour.** Switch internal panel refresh rate (120 Hz / 60 Hz), factory-calibrated colour gamuts (Native, sRGB, Display P3), colour temperature, and eye comfort mode with a built-in colour and grayscale reference strip.
- **Service management.** Disable or restore Huawei PC Manager's background services and startup entries in one click. OpenEGo Hub takes over touch while running and hands it back to Huawei's touch service automatically on exit, uninstall, or crash.
- **Updates & diagnostics.** Periodic background release checks with in-app download, SHA-256 verification, and silent installation, plus one-click diagnostic log export.

---

## Screenshots

### Quick flyout & connection prompts

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Assets/screenshots/flyout-dark.avif">
    <source media="(prefers-color-scheme: light)" srcset="Assets/screenshots/flyout-light.avif">
    <img src="Assets/screenshots/flyout-light.avif" alt="Quick settings flyout" width="340">
  </picture>
</p>

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Assets/screenshots/popup-pen-dark.avif">
    <source media="(prefers-color-scheme: light)" srcset="Assets/screenshots/popup-pen-light.avif">
    <img src="Assets/screenshots/popup-pen-light.avif" alt="Pen connection prompt" width="340">
  </picture>
  &nbsp;&nbsp;
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Assets/screenshots/popup-keyboard-dark.avif">
    <source media="(prefers-color-scheme: light)" srcset="Assets/screenshots/popup-keyboard-light.avif">
    <img src="Assets/screenshots/popup-keyboard-light.avif" alt="Keyboard connection prompt" width="340">
  </picture>
</p>

### Accessories

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Assets/screenshots/accessories-dark.avif">
    <source media="(prefers-color-scheme: light)" srcset="Assets/screenshots/accessories-light.avif">
    <img src="Assets/screenshots/accessories-light.avif" alt="Accessories page" width="820">
  </picture>
</p>

### Battery & Display

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Assets/screenshots/battery-dark.avif">
    <source media="(prefers-color-scheme: light)" srcset="Assets/screenshots/battery-light.avif">
    <img src="Assets/screenshots/battery-light.avif" alt="Battery page" width="404">
  </picture>
  &nbsp;
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Assets/screenshots/screen-dark.avif">
    <source media="(prefers-color-scheme: light)" srcset="Assets/screenshots/screen-light.avif">
    <img src="Assets/screenshots/screen-light.avif" alt="Display page" width="404">
  </picture>
</p>

### Services & Device

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Assets/screenshots/services-dark.avif">
    <source media="(prefers-color-scheme: light)" srcset="Assets/screenshots/services-light.avif">
    <img src="Assets/screenshots/services-light.avif" alt="Services page" width="404">
  </picture>
  &nbsp;
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Assets/screenshots/device-dark.avif">
    <source media="(prefers-color-scheme: light)" srcset="Assets/screenshots/device-light.avif">
    <img src="Assets/screenshots/device-light.avif" alt="Device page" width="404">
  </picture>
</p>

---

## Supported hardware

- **Machine:** HUAWEI MateBook E Go (Windows 11 ARM64)
- **Pen:** HUAWEI M-Pencil (CD52, CD54, CD54R, and CD54S supported; verified on CD54R hardware)
- **Keyboard:** HUAWEI Smart Magnetic Keyboard

---

## Installation

1. Download the latest `OpenEGoHubSetup_arm64_vX.Y.Z.msi` from [Releases](https://github.com/NihilDigit/openego-hub/releases/latest).
2. Run the installer (administrator privileges are required to register the system service).
3. Once installed, the background service starts with Windows; open the control centre from the tray icon or the Start menu.

### Upgrading and uninstalling

- Upgrades replace earlier service registrations and binaries in place without requiring a manual uninstall first; settings, logs, and disabled Huawei component state are preserved.
- Updates confirmed in the control centre are downloaded, verified, and installed silently, with the tray and window relaunching automatically afterwards.
- Uninstalling hands touch back to Huawei's touch service and restores any Huawei background services and logon autostart entries disabled through OpenEGo Hub.

---

## Build from source

### Requirements

- Windows 11 ARM64
- Visual Studio 2022 (ARM64 and ARM64EC MSVC toolchains, MSBuild)
- CMake 3.20+ and Ninja
- WiX Toolset v7 (required only for MSI packaging)

### Build and package

Build `hal/` before configuring the main tree:

```powershell
# 1. Build HAL (Debug or Release)
.\hal\scripts\build.ps1 -Config Release

# 2. Build main tree (pass -Test with Debug to run CTest)
.\scripts\build.ps1 -Config Release

# 3. Package Release MSI (builds HAL Release, main Release, and build\OpenEGoHubSetup.msi)
scripts\pack_release_version.bat
```

Local deployment and debugging:

```powershell
# Deploy binaries to C:\Program Files\OpenEGoHub
pwsh -File scripts\deploy.ps1 -Config Release

# Register the Debug service once, then iterate from an elevated shell
scripts\install_debug_service.bat
pwsh -File scripts\dev-cycle.ps1
```

---

## Layout

- `EGoTouchService/`: native ARM64 system service for touch switching, accessory state, and power management.
- `hal/`: hardware abstraction layer and ARM64EC helper processes that load Huawei's x64 DLLs.
- `Common/`: cross-process shared-memory channels, config definitions, and shared utilities.
- `Tools/`: user-session apps — tray monitor (`EGoTouchTray`), WinUI 3 control centre (`EGoTouchSettings`), and installer cleanup helper (`EGoTouchCleanup`).
- `docs/`, `hal/docs/`: reverse-engineered protocol notes, lifecycle design, and vendor module research.
- `scripts/`: build, test, deployment, and WiX packaging scripts.

---

## Credits

Derived from **[EGoTouchRev](https://github.com/awarson2233/EGoTouchRev)** (MIT License, © Detach2233); see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for full licence notices.

The following open-source projects and community work were consulted during development:

- [MateBook-E-Pen](https://github.com/eiyooooo/MateBook-E-Pen) (by eiyooooo)
- [goodies](https://github.com/matebook-e-go/goodies) (by dantmnf)
- [EgoTools](https://github.com/SaKongA/EgoTools) (by SaKongA)
- [HuaweiPenEraserService](https://github.com/qwqVictor/HuaweiPenEraserService) (by qwqVictor)

---

## Licence

Released under the [MIT License](LICENSE). Any redistribution in source or binary form must retain the copyright notices in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

---

## Disclaimers

- **Trademarks.** This project has no official affiliation with, or endorsement from, HUAWEI, Himax, or any other rights holder. Product names, trademarks, and company names belong to their respective owners.
- **Third-party assets.** At runtime the tray and control centre load device renders and status icons from a locally installed Huawei PC Manager when present, falling back to built-in vector or Fluent icons otherwise. Copyrighted vendor artwork is included in neither this repository nor release installers.
- **Use at your own risk.** This project interacts with low-level hardware drivers and system services and is provided for interoperability research and technical exchange. You assume all risk of hardware faults, data loss, or system instability; the authors and contributors accept no liability.
