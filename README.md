<img src="Assets/brand/openego-hub-256.png" alt="" width="72" align="left" hspace="4" vspace="6">

# OpenEGo Hub

<br clear="left">

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows_11_ARM64-lightgrey.svg)]()
[![Build](https://github.com/NihilDigit/openego-hub/actions/workflows/build.yml/badge.svg)](https://github.com/NihilDigit/openego-hub/actions/workflows/build.yml)

A native ARM64 control centre and driver stack for the HUAWEI MateBook E Go, covering
touch, pen and the detachable keyboard. It replaces the vendor touch service, along with
the accessory status and pen settings that PC Manager provides. Every component targets
ARM64; nothing in the deployment runs under WOW64 emulation.

This project is a fork of [EGoTouchRev](https://github.com/awarson2233/EGoTouchRev),
which contributed the touch stack it is built on. See [Credits](#credits).

---

## What it does

**Touch.** A `LocalSystem` service acquires capacitive heatmap frames from the Himax
controller, runs them through a processing pipeline (anti-jitter, anti-bounce, 1 Euro
filtering, palm and stylus arbitration) and injects HID reports through a virtual
device.

**Pen.** BT-MCU protocol integration for the M-Pencil: pressure, battery, attach and
charge state, and a configurable side-button double-click that either follows the
system pen setting or toggles between writing and erasing.

**Keyboard.** The detachable keyboard's wireless-on-detach setting, read from and
written to the MCU rather than remembered locally, so the displayed state is the real
one.

**Interface.** A tray panel showing accessory status, and a WinUI 3 settings window for
everything configurable. Both run unelevated; the service exposes them a read-only
status channel and a narrow command channel rather than an administrative pipe.

---

## Compatibility

Windows 11 on ARM64, on the HUAWEI MateBook E Go. Nothing here is portable to another
device: the touch pipeline is written against this panel's Himax controller, and the pen
and keyboard protocols against the MCU this tablet exposes.

The pen module identifies itself over the MCU, and CD52, CD54, CD54R and CD54S are
recognised — the M-Pencil first through third generations. Development and measurement
were done on a CD54R, so the other modules are handled but untested.

The detachable keyboard is identified by whether it answers on the MCU's keyboard
subsystem at all, which third-party keyboards do not register for. An unrecognised
keyboard is reported as unknown rather than guessed at.

---

## Non-goals

Reverse engineering the vendor stack reveals more than is worth reimplementing. The
following are deliberately out of scope:

- **Firmware update.** Requires vendor-signed images, and a failure bricks hardware.
- **Voice assistant integration.** Vendor-specific, tied to services this project does
  not replace.
- **Global annotation.** A vendor feature that was unreliable in its original form.

The rule this project applies: implement the functions that are genuinely general and
that the hardware supports directly. Reproducing a vendor gimmick that never worked
well is not an improvement.

---

## Installation

Pure ARM64 MSI installers, packaged with WiX Toolset v4. Commit and PR builds only
compile and run smoke tests; release installers are built from `vMAJOR.MINOR.PATCH`
tags and attached to GitHub Releases.

1. Download the latest `OpenEGoHubSetup_arm64_vX.Y.Z.msi` from the Releases page.
   - `OpenEGoHubSetup_arm64_vX.Y.Z.msi` — service, tray and settings.
   - `OpenEGoHubTestSetup_arm64_vX.Y.Z.msi` — the above plus diagnostic tools.
2. Run the setup wizard. Administrator rights are required to register the service.
3. The installer registers `OpenEGoHubService` to start with Windows and adds a Start
   menu entry.

The vendor touch service and this one drive the same hardware and must not run at the
same time. The installer handles the handover; the tray can give control back to
HUAWEI's driver without uninstalling.

### Build from source

Requires **CMake**, **Ninja**, the ARM64 MSVC toolchain, and **WiX v4** for packaging.

The `arm64-*` presets resolve `cl.exe` from `PATH`, so the ARM64 developer environment
has to be in the shell first. `scripts\build.ps1` imports it and pins the repository
root, which makes it the shorter path:

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

Two scripts cover the development loop, both from an elevated shell.
`scripts\dev-cycle.ps1` stops the debug service, rebuilds, restarts it, and relaunches
the tray and settings window unelevated. `scripts\verify.ps1` builds, runs the tests,
and replays the recorded corpora against the previous results.

---

## Layout

- `EGoTouchService/` — the service.
  - `Device/` — hardware abstraction: Himax controller, BT-MCU pen and keyboard protocols.
  - `Solvers/` — the touch and stylus pipelines.
  - `Host/` — OS interfaces: HID injection, power and lid monitoring.
- `Common/` — cross-process channels and shared configuration.
- `Tools/EGoTouchTray/` — tray panel and accessory status.
- `Tools/EGoTouchSettings/` — WinUI 3 settings window.
- `Tools/EGoTouchApp/` — diagnostics workbench.
- `docs/` — reverse-engineered protocol documentation.
- `scripts/` — build, packaging and development scripts.

---

## Credits

This project is a fork of
**[EGoTouchRev](https://github.com/awarson2233/EGoTouchRev)** (MIT, © Detach2233), whose
touch stack it is built on. That notice is preserved in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

It was also influenced by, and developed with reference to, three projects that worked
on this device before it:

- **[MateBook-E-Pen](https://github.com/eiyooooo/MateBook-E-Pen)** by eiyooooo
- **[goodies](https://github.com/matebook-e-go/goodies)** by dantmnf
- **[EgoTools](https://github.com/SaKongA/EgoTools)** by SaKongA

---

## Licence

MIT, the same terms as the project this one is forked from, so that improvements to the
touch stack can go back upstream as easily as they came down. See [LICENSE](LICENSE).

The upstream copyright notice and the vendored Dear ImGui notice are preserved in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and must accompany any redistribution,
source or binary.

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
