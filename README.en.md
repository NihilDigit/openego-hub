<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/brand/openego-hub-256.png" alt="" width="96">
</p>

<h1 align="center">OpenEGo Hub</h1>

<p align="center">
  <a href="https://github.com/NihilDigit/openego-hub/releases/latest"><img src="https://img.shields.io/github/v/release/NihilDigit/openego-hub?display_name=tag&label=release" alt="Release"></a>
  <a href="https://github.com/NihilDigit/openego-hub/actions/workflows/build.yml"><img src="https://img.shields.io/github/actions/workflow/status/NihilDigit/openego-hub/build.yml?branch=main&label=build" alt="Build"></a>
  <a href="https://github.com/NihilDigit/openego-hub/releases"><img src="https://img.shields.io/github/downloads/NihilDigit/openego-hub/total?label=downloads" alt="Downloads"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/NihilDigit/openego-hub" alt="License"></a>
</p>

<p align="center"><a href="README.md">中文</a> | English</p>

A native ARM64 driver stack for the HUAWEI MateBook E Go, covering touch, pen and the
detachable keyboard. It replaces the vendor touch service, along with the accessory
status and pen settings that PC Manager provides.

This project is a fork of [EGoTouchRev](https://github.com/awarson2233/EGoTouchRev).
The touch stack originates there and has been modified since.

---

## What it does

- **Touch.** Multi-touch, with palm rejection while writing and fingers ignored while
  the pen is in use.
- **Pen.** Pressure and tilt for the M-Pencil. The side-button double click either
  follows the system pen setting or switches between writing and erasing, the latter
  with an optional OneNote compatibility mode.
- **Keyboard.** Wireless-on-detach can be turned on and off.
- **Device information.** Battery, charge and attach state, firmware and hardware
  versions, and serial number, for the pen and the keyboard alike.

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/devices.png" alt="Device page" width="720">
</p>

The tray sits in the notification area with accessory status and raises a prompt when
the pen or keyboard connects. The settings window holds every switch.

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/settings.png" alt="Settings window" width="720">
</p>

---

## Supported hardware

- HUAWEI MateBook E Go, Windows 11 on ARM64.
- Pen: M-Pencil first through third generations (CD52, CD54, CD54R, CD54S). Only the
  CD54R has been tested on hardware.
- Keyboard: the HUAWEI smart magnetic keyboard. Third-party keyboards are not
  recognised.

---

## Non-goals

- **Firmware update.** Requires vendor-signed images, and a failure bricks hardware.
- **Voice assistant integration.**
- **Global annotation.**

---

## Installation

Download `OpenEGoHubSetup_arm64_vX.Y.Z.msi` from the Releases page and run it;
registering the service requires administrator rights. The service then starts with
Windows, and there is a Start menu entry. `OpenEGoHubTestSetup_arm64_*.msi` adds the
diagnostic tools.

Installing disables the vendor touch service — the two cannot drive the same hardware at
once. Switching back does not need an uninstall: quit from the settings window.

---

## Build from source

Requires CMake, Ninja, the ARM64 MSVC toolchain, and WiX v4 for packaging.

The `arm64-*` presets resolve `cl.exe` from `PATH`, so the ARM64 developer environment
has to be in the shell first. `scripts\build.ps1` imports it:

```powershell
.\scripts\build.ps1 -Config Release           # configure and build
.\scripts\build.ps1 -Config Debug -Test       # build, then run ctest
```

After `vcvarsarm64.bat`, the presets also work directly:

```powershell
cmake --preset arm64-Release
cmake --build --preset arm64-Release
```

Packaging:

```powershell
dotnet tool install --global wix
wix extension add -g WixToolset.UI.wixext
wix build -ext WixToolset.UI.wixext -arch arm64 -d BuildVersion=1.2.3 `
    scripts\EGoTouchSetup.wxs -loc scripts\zh-CN.wxl `
    -out build\OpenEGoHubSetup_arm64_v1.2.3.msi
```

`scripts\dev-cycle.ps1` stops the service, rebuilds and restarts it; `scripts\verify.ps1`
builds, runs the tests and replays the recorded corpora. Both need an elevated shell.

---

## Layout

- `EGoTouchService/`: the service. `Device/` is hardware abstraction, `Solvers/` the
  touch and stylus pipelines, `Host/` the OS interfaces.
- `Common/`: cross-process channels and shared configuration.
- `Tools/`: tray, settings window and diagnostics workbench.
- `docs/`: reverse-engineered protocol documentation.
- `scripts/`: build, packaging and development scripts.

---

## Credits

This project is a fork of
**[EGoTouchRev](https://github.com/awarson2233/EGoTouchRev)** (MIT, © Detach2233), whose
touch stack this one started from and has since modified. That notice is preserved in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

The touch pipeline was measured against Chromium's ChromeOS touch stack, which is where
the palm thresholds were recalibrated from.

Three earlier projects on this device were also consulted:

- **[MateBook-E-Pen](https://github.com/eiyooooo/MateBook-E-Pen)** by eiyooooo
- **[goodies](https://github.com/matebook-e-go/goodies)** by dantmnf
- **[EgoTools](https://github.com/SaKongA/EgoTools)** by SaKongA

---

## Licence

MIT. See [LICENSE](LICENSE).

The upstream copyright notice and the vendored Dear ImGui notice are preserved in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and must accompany any redistribution,
in source or binary form.

---

## Disclaimers

This project is not affiliated with, authorised by, or connected to HUAWEI, Himax, or
any other trademark holder. All product and company names belong to their owners. It
was developed through reverse engineering for interoperability, research and
educational purposes.

The tray reads pen artwork and battery icons from an existing PC Manager installation
at runtime and falls back to drawing its own when PC Manager is absent. Those images
are HUAWEI's and are never redistributed with this project.

**Use at your own risk.** This replaces a low-level hardware driver. The authors and
contributors accept no liability for hardware damage, data loss, system instability, or
any violation of third-party terms of service arising from its use.
