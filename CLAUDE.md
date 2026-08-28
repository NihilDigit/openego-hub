# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

OpenEGo Hub replaces the HUAWEI MateBook E Go touch service and PC Manager. Windows 11
on ARM64. Touch and stylus recognition is not implemented here and not called from here
either: `hal/GaokunThpHost.exe` hosts the vendor's own `THP_Service.dll` and the whole chain
behind it. Everything else that needs a Huawei x64 DLL is likewise its own ARM64EC
process under `hal/`.

The product was renamed from EGoTouchRev; CMake target names, directory names and the
"EGoTouchRev touch" label in the settings window are deliberately left alone, because
there they name the upstream project rather than this product.

## Build

Run builds from the **PowerShell** tool, never bash. The ARM64 developer environment has
to be in the shell first; `scripts\build.ps1` imports it and pins the repo root.

```powershell
.\scripts\build.ps1 -Config Debug -Test              # build + ctest
.\scripts\build.ps1 -Config Release                  # LTO, slow, -Jobs 2 by default
.\scripts\build.ps1 -Config Debug -Target Device     # one target
```

`build.ps1` takes `-Config`, `-Target`, `-Jobs` and `-Test`. It reconfigures only when
`CMakeCache.txt` is absent, so a configure that failed halfway leaves a broken
`build.ninja` behind and every later build dies on `loading 'CMakeFiles\rules.ninja'`.
Delete the cache to force a reconfigure.

**`hal/` builds separately and first.** CMake links `hal/build/<Config>/GaokunHal.lib`
and fails configuration when it is missing. The HAL cannot join the main CMake build as
it stands: its vendor-DLL hosts are ARM64EC and its controller library is native ARM64,
which is two compiler invocations, not one.

```powershell
cd hal; .\scripts\build.ps1 -Config Debug            # then build the main tree
```

Debug and Release each need their own HAL build — the static library carries the CRT
choice, and a mismatch surfaces as LNK4098 at the service link step.

`cl.exe`, not clang. The settings window is built by MSBuild from a `.vcxproj` behind an
`if(WIN32 AND MSVC)` gate, so a clang configuration silently produces no
`OpenEGoHubSettings.exe` and packaging then fails on a missing file. Never add
`CMAKE_*_COMPILER_TARGET` to the presets: it is clang-only and makes CMake emit
GNU-style link flags `cl.exe` cannot parse.

Two traps that make a build look successful when it is not:

- ninja stops at the first failed target and leaves the rest unbuilt, so a `ctest` run
  straight after a failed build passes against stale binaries. `scripts\verify.ps1`
  greps for `ninja: build stopped` instead of trusting the exit code alone.
- After an interrupted build ninja has been observed losing a header dependency and
  relinking an object compiled before the edit. Deleting the build directory is the
  escape hatch.

The version number lives in `CMakeLists.txt` (`PROJECT_VERSION`) and
`Common/include/AppVersion.h`, because the settings window is a separate MSBuild project
and cannot read CMake variables. Configuration fails when they disagree; change both.

## Tests

```powershell
ctest --preset arm64-Debug                     # all
ctest --preset arm64-Debug -L device-free      # no touch controller needed
ctest --preset arm64-Debug -R PenUsb            # one group
```

Labels in use: `Common`, `service`, `host`, `config`, `unit`, `integration`,
`device-free`, `windows`. CI runs `-L device-free`.

## Running on the device

The Debug service runs straight out of the build tree, so it holds
`build\arm64-Debug\OpenEGoHubService.exe` and a rebuild fails to link (LNK1168). The
tray and the settings window each lock their own exe as well.

Debug and Release services are **mutually exclusive**: both drive the same Himax device,
create the same VHF virtual HID and read the same BT-MCU HID reports. Running both is
undefined behaviour — check with `sc.exe query` before starting either.

```powershell
.\scripts\dev-cycle.ps1                        # stop -> build -> start, needs an already-elevated shell
pwsh -File scripts\svc.ps1 -Action stop -Service OpenEGoHubServiceDebug -KillApps   # self-elevates
```

`dev-cycle.ps1` does not self-elevate; `svc.ps1` does. `svc.ps1 -Service` selects which
service, and its default is the Debug one.

The tray and the settings window must be launched **unelevated**, which is why
`dev-cycle.ps1` starts them through `explorer.exe`: UIPI blocks window messages from a
medium-integrity process to a high-integrity one, and every setting the user changes
travels that way.

## Architecture

Three processes, plus the HAL hosts:

- **Service** (`EGoTouchService`, LocalSystem, session 0) owns the hardware. It cannot
  draw anything, which is the reason the other two exist.
- **Tray** (`Tools/EGoTouchTray`) is the only process that talks to the service from the
  user session, over two shared mappings with cross-process seqlocks:
  `PenStatusChannel` (service writes, read-only broadcast) and `PenControlChannel`
  (single writer, enum-validated commands). It also injects the pen side-button gesture,
  because `SendInput` from session 0 returns `ERROR_ACCESS_DENIED`.
- **Settings window** (`Tools/EGoTouchSettings`, WinUI 3) never talks to the service. It
  posts commands to the tray's HWND and receives activation and notification messages on
  a message-only window. Keeping the command surface on the tray preserves the control
  channel's single-writer invariant. The command enum in `Common/include/EGoTouchTrayIpc.h`
  is append-only — the two executables are built separately and an inserted value shifts
  every command for a mismatched pair.

The process boundary is also the architecture boundary. Huawei's DLLs are x64 and an
ARM64 process cannot load them, so each capability that needs one is a separate ARM64EC
executable under `hal/`, driven over a pipe or a shared mapping. Nothing above `hal/`
is compiled as ARM64EC, and nothing under it includes anything from above.

### Touch

The service does not compute touch and no longer has the machinery to. It supervises a
vendor process and stays out of the data path:

```
Himax SPI -> THP_Service.dll -> ApDaemon -> TSACore/TSAPrmt -> vendor VHF -> Windows HID
             (all inside GaokunThpHost.exe, ARM64EC)
```

`StartEGoTouchProvider()` launches `hal/`'s `GaokunThpHost.exe`, which resolves the vendor
install directory from the `HuaweiThpService` SCM `ImagePath`, loads `THP_Service.dll`,
registers four callbacks and calls `ThpFuncStart()`. Palm rejection, pressure, tilt and
pen/touch arbitration all come from that chain, and so does the HID injection.

Handover is lease-driven, not automatic. The service boots with `HuaweiThpService` still
owning touch, so the login screen and a crashed tray both keep working. The tray requests
the lease and renews it; the server times out at 5 seconds. Taking over stops the Huawei
service, starts the host, re-confirms Huawei stayed stopped, then publishes `EGoTouch`.
Dropping the lease reverses it, and `TouchProviderCoordinator` restarts the OpenEGo
provider if restoring Huawei fails — never leave zero providers.

Nothing else in this tree touches frames. There used to be a second path — the in-tree
solvers, then `DeviceRuntime` reading Himax frames into `OemTsaBackend` and emitting our
own HID reports through `VhfReporter` — and it is gone: the solvers, `Device/himax`,
`Device/vhf` and `EGoTouchService/Tsa` were all removed once the vendor host became the
only provider. Do not rebuild any of it without deciding first that the vendor chain is
not good enough, which is the opposite of the decision already made.

`DeviceRuntime` kept its name but not its job. It has no worker thread, no chip and no
HID output; what remains is the pen and keyboard MCU state machine — side-button
dispatch, eraser state, bluetooth pressure, power events. Renaming it is a separate
change. `Device::BtPenInputLatch` holds the bluetooth pen sample and its 24-byte layout
is still an ABI contract, because the pen MCU protocol did not change.
`StylusProtocolHint` survives for the same reason: it identifies the pen module over BT.

### The other HAL hosts

`GaokunThpHost` carries writing data. Accessory state is a different chain entirely:
`GaokunPenHost.exe` loads PC Manager's `PenService.dll` for battery, module id, firmware,
serial and side-button events; `GaokunKeyboardHost.exe` loads `KeyboardService.dll` for
keyboard state and wireless-on-detach. Both publish a snapshot through a shared-memory
seqlock and send discrete events over a named pipe; the service polls them every 250 ms
and folds everything into one `PenStatusChannel`.

Side-button events must reach the tray because session 0 cannot `SendInput` to the user
desktop. The tray injects the Windows Ink shortcut and, when OneNote is foreground,
switches the drawing tool through UI Automation.

### Configuration

`RegisterServiceConfigBindings` is the only source of config keys now — five of them,
all under `service.`. A new knob must also be registered in
`Common/include/config/ConfigKeyId.h` and `ConfigKeyMap.cpp`, or `ConfigCatalog` drops
it and IPC cannot deliver it. Key ids are append-only; deleted ones leave a hole rather
than being renumbered.

`ConfigScope::TouchPipeline` and `StylusPipeline` remain in the enum. They are part of
the serialized config ABI; removing them renumbers the rest.

### Release differs from Debug

Release compiles without `_DEBUG`. Members used at an unconditional call site must be
**declared** unconditionally; this has broken the Release build twice while every local
Debug build stayed green. Build Release before pushing anything that touches
`DeviceRuntime` or `ServiceHost`.

## Known gaps

These are real, verified against the code, and none of them are visible from a passing
build. Fix the packaging one before shipping anything.

- **The MSI does not contain the HAL.** `scripts\EGoTouchSetup.wxs` lists
  `OpenEGoHubService.exe`, `OpenEGoHubSettings.exe` and `OpenEGoHubTray.exe` and nothing
  else. No `GaokunThpHost.exe`, no `GaokunPenHost.exe`, no `GaokunKeyboardHost.exe`, no
  `GaokunDisplay.exe`, no `qdcmlib`. `scripts\deploy.ps1` copies them, which is why
  development machines work; an installed build has no touch provider to start.
- **Host death is not noticed.** `HostController::Start` waits 1500 ms to catch a host
  that exits immediately, and that is the only liveness check. `IsRunning()` exists but
  nothing calls it on lease renewal, so a host that crashes later leaves touch dead with
  no fallback to Huawei.
- **OneNote input suppression does nothing.** `SetInputSuppressed` now only publishes a
  state bit, which the tray blocks on; there is no gate behind it any more, and live pen
  input comes out of the vendor VHF inside `GaokunThpHost`. Suppressing it means reaching
  into the vendor chain, and there may be no clean way to do that.
- **`ApplyServicePolicy` still takes `stylusVhfEnabled`** and has nowhere to apply it.
  The log line says `(no effect)`. The setting is still in the config schema.

## Where the reasoning is written down

- `docs/tsacore_ground_truth.md` — vendor behaviour established by running TSACore as an
  oracle, with the numbers. The measurements still hold; the loader they were taken
  through is gone.
- `docs/KBDMCU_PROTOCOL.md`, `docs/KEYBOARD_IDENTITY.md`, `docs/ACCESSORY_CENTER.md` —
  reverse-engineered MCU and PC Manager behaviour.
- `hal/docs/` — the vendor DLL reverse engineering: display colour management, the OSD,
  battery and the vendor services. `display-manage.md` also records two negative results
  worth not repeating.

A number of documents under `docs/` describe the removed solver stack, the DVR recorder
and the IPC control channel. They are history, not current behaviour.

## Documentation

`README.md` is Chinese and `README.en.md` English; keep both in sync. The README is for
users — implementation detail belongs in `docs/`.

Prose outside this repository's code follows the `writing-style` skill. Chinese
technical writing in particular: Chinese sentence shape, accurate verbs, no
English-shaped long pre-modifiers.
