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

A driver stack for the HUAWEI MateBook E Go, covering touch, pen and the detachable
keyboard. It replaces the vendor touch service and PC Manager.

This project is a fork of [EGoTouchRev](https://github.com/awarson2233/EGoTouchRev).
The acquisition and injection layers originate there and have been modified since.

---

## What it does

- **Touch.** Multi-touch, with palm rejection while writing and fingers ignored while
  the pen is in use.
- **Pen.** Pressure and tilt for the M-Pencil. The side-button double click either
  follows the system pen setting or switches between writing and erasing, the latter
  with an optional OneNote compatibility mode.
- **Keyboard.** Wireless-on-detach can be turned on and off.
- **Battery.** A charge threshold, either the vendor's smart charging or a manual
  window. Battery health, cycle count and time remaining.
- **Display.** Gamut, colour temperature and eye comfort. The values come from the
  panel's factory calibration and are applied by the display driver, not by a software
  filter.
- **Services.** Huawei's background services can be disabled and restored at any time.
- **Device information.** Battery, charge and attach state, firmware version and serial
  number for the pen and the keyboard; model, processor, memory and OS build for the
  machine itself.

The accessories page carries the battery level, firmware and serial number for the pen
and the keyboard, and it is where the pen's side button is configured.

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/accessories.png" alt="Accessories page" width="820">
</p>

The tray sits in the notification area with accessory status and raises a prompt when
the pen or keyboard connects.

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/popup-pen.png" alt="Pen connection prompt" width="340">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/popup-keyboard.png" alt="Keyboard connection prompt" width="340">
</p>

The battery page holds the charge threshold and the health readings; the screen page
adjusts gamut, colour temperature and the eye comfort filter.

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/battery.png" alt="Battery page" width="410">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/screen.png" alt="Screen page" width="410">
</p>

The services page disables and restores Huawei's background services; the device page
lists what this machine is made of.

<p align="center">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/services.png" alt="Services page" width="410">
  <img src="https://raw.githubusercontent.com/NihilDigit/openego-hub/main/Assets/screenshots/device.png" alt="Device page" width="410">
</p>

---

## Touch runs on the vendor's own algorithm

Touch and pen recognition is not reimplemented. The project starts a host process of its
own and loads Huawei's `THP_Service.dll` into it, so frames travel from the Himax
controller through the vendor chain straight to Windows:

```
Himax -> THP_Service -> TSACore -> vendor VHF -> Windows HID
```

Palm rejection, pressure, tilt and pen/finger arbitration are therefore identical to
stock. What this project replaces is the thin .NET service shell Huawei ships around
that chain. The cost is that the Huawei driver has to stay installed; remove it and touch
stops working.

Handover is lease-based rather than automatic. Touch still belongs to Huawei when the
service starts, so the login screen works and so does a machine whose tray has not come
up. The tray takes a lease to switch over, and dropping it — on exit or on a crash —
switches back. No failure path leaves touch without a provider.

Display colour, the charge threshold and the keyboard's wireless link also go through
Huawei's components, which are compiled for x64. An ARM64 process cannot load an x64
DLL, so each of these runs in its own process; they live under `hal/`.

See [`hal/README.md`](hal/README.md) for the details.

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
Windows, and there is a Start menu entry.

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

`hal/` builds separately and has to go first: the main tree links the `GaokunHal.lib`
it produces, and configuration fails without it. Debug and Release each need their own
build.

```powershell
cd hal; .\scripts\build.ps1 -Config Debug
```

The service itself is native ARM64. Only the hosts under `hal/` that load Huawei's x64
DLLs are ARM64EC, and they come from hal's own build script rather than the presets
here.

On a development machine, install the Debug service as Administrator, then use
DevCycle:

```powershell
scripts\install_debug_service.bat
pwsh -File scripts\dev-cycle.ps1
```

`pwsh -File scripts\dev-cycle.ps1 -RestoreRelease` stops debugging and restores the
installed Release service. `-NoTray` starts the service without taking the lease, so
touch intentionally stays with Huawei.

Packaging:

```powershell
dotnet tool install --global wix
wix extension add -g WixToolset.UI.wixext
wix build -ext WixToolset.UI.wixext -arch arm64 -d BuildVersion=1.2.3 `
    -d BuildOutputDir=build\arm64-Release -d HalOutputDir=hal\build\Release `
    scripts\OpenEGoHubSetup.wxs -loc scripts\zh-CN.wxl `
    -out build\OpenEGoHubSetup_arm64_v1.2.3.msi
```

The HAL hosts in the installer come from `HalOutputDir`, which
`hal\scripts\build.ps1 -Config Release` has to produce first.
`scripts\pack_release_version.bat` does the build and the packaging in one go, so the
arguments do not have to be assembled by hand.

`scripts\dev-cycle.ps1` stops the service, rebuilds and restarts it; `scripts\verify.ps1`
builds, runs the tests and catches the "build stopped early, tests still green" kind of
false success. Both need an elevated shell.

---

## Layout

- `EGoTouchService/`: the service. `Device/` is acquisition and injection, `Tsa/` the
  vendor backend adapter, `Host/` the OS interfaces.
- `hal/`: the vendor hardware layer. Everything that loads a Huawei x64 DLL lives here,
  each capability in its own ARM64EC process.
- `Common/`: cross-process channels and shared configuration.
- `Tools/`: tray and settings window.
- `docs/`: reverse-engineered protocol documentation.
- `scripts/`: build, packaging and development scripts.

---

## Credits

This project is a fork of
**[EGoTouchRev](https://github.com/awarson2233/EGoTouchRev)** (MIT, © Detach2233). The
Himax frame acquisition, the pen MCU transport, the VHF injection layer and the service
runtime all come from there. So did the touch algorithm, until it was replaced by the
vendor backend and removed. That notice is preserved in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Earlier projects on this device were also consulted:

- **[MateBook-E-Pen](https://github.com/eiyooooo/MateBook-E-Pen)** by eiyooooo
- **[goodies](https://github.com/matebook-e-go/goodies)** by dantmnf
- **[EgoTools](https://github.com/SaKongA/EgoTools)** by SaKongA
- **[HuaweiPenEraserService](https://github.com/qwqVictor/HuaweiPenEraserService)** by qwqVictor

---

## Licence

MIT. See [LICENSE](LICENSE).

The upstream copyright notice is preserved in
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
