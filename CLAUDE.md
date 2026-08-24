# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

OpenEGo Hub replaces the HUAWEI MateBook E Go touch service and the accessory parts of
PC Manager. ARM64-only, Windows 11. The product was renamed from EGoTouchRev; CMake
target names, directory names and the "EGoTouchRev touch" label in the settings window
are deliberately left alone, because there they name the upstream touch stack rather
than this product.

## Build

Run builds from the **PowerShell** tool, never bash. The ARM64 developer environment has
to be in the shell first; `scripts\build.ps1` imports it and pins the repo root.

```powershell
.\scripts\build.ps1 -Config Debug -Test              # build + ctest
.\scripts\build.ps1 -Config Release                  # LTO, slow, -Jobs 2 by default
.\scripts\build.ps1 -Config Debug -Target SolversUnit_TouchStrokeAggregator
.\scripts\build.ps1 -Config Debug -Clean             # after any interrupted build
```

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
  relinking an object compiled before the edit. `-Clean` is the escape hatch.

The version number lives in `CMakeLists.txt` (`PROJECT_VERSION`) and
`Common/include/AppVersion.h`, because the settings window is a separate MSBuild project
and cannot read CMake variables. Configuration fails when they disagree; change both.

## Tests

```powershell
ctest --preset arm64-Debug                     # all
ctest --preset arm64-Debug -L device-free      # no touch controller needed
ctest --preset arm64-Debug -R TouchPeakHold    # one test
```

CI runs `-L device-free -LE admin`; the `admin` exclusion exists because the IPC control
pipe carries an administrator-only ACL and its loopback test cannot connect under the
runner's token. Solver unit tests are registered as `SolversUnit_<Name>` with labels
`solvers.touch`, `solvers.stylus`, `solvers.config`.

## Running on the device

The Debug service runs straight out of the build tree, so it holds
`build\arm64-Debug\OpenEGoHubService.exe` and a rebuild fails to link (LNK1168). The
tray, the settings window and the workbench each lock their own exe as well.

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

Three processes:

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

`Tools/EGoTouchApp` is the diagnostics workbench and connects over the IPCCore named
pipe, which is administrator-only and linked into Debug builds only.

### Touch pipeline

`EGoTouchService/Solvers/TouchSolver`, in order: conditioning (writes
`frame.touch.conditioned`, never the raw map) → `MacroZoneDetector` → `PeakDetector` →
`ZoneExpander` → `ContactExtractor` → `TouchTracker` → `StrokeAggregator` →
`TouchGestureStateMachine`.

`StrokeAggregator` is a deliberate divergence from the vendor, which has no stroke-level
stage at all. Stroke identity is decoupled from track id (tracks break, strokes should
not); evidence is always the running maximum, because palm false positives are *smaller*
than real contacts and arrive as fragments; the stage emits `Holding` / `Active` /
`Cancelled` rather than a boolean, and the palm verdict can flip back once, in one
direction only.

Frames carry `receiveSystemEpochUs`, stamped in `DeviceRuntime` against QPC. The solvers
have no other clock, and idle frames never enter the pipeline, so anything that counts
its own invocations measures nonsense.

### Stylus

HPP3 only — the HPP2 sub-pipeline was deleted after proving nothing ever wrote its input.
`StylusProtocolHint::Hpp2` remains because it identifies the CD52 pen module over BT.

`StylusTouchArbiter` produces the full-frame verdict in `frame.stylus.interop`;
`StylusTouchSuppressor` folds it in with `max()` on every exit path (it used to clear it
unconditionally, which is why the verdict never reached the tracker) and `TouchTracker`
gates on pen mode.

### Wire formats

`Common/IPCCore/include/Ipc/SharedFrameBuffer.h` (shared frame ABI, currently 9) and
`Common/DVRCore/include/Dvr{Format,FrameSlot}.h` are one change: the ABI guards, the
`static_assert(offsetof(...))` block, `BuildFrameSchema()`, the reader, the CSV export
and the workbench all have to move together or nothing compiles. Recordings made before
a layout change cannot be read.

### Configuration

Every tunable exists three times — member initialiser, `registerBindings` default,
`applyConfig` fallback — and `SolversUnit_PipelineDefaultsConsistency` pins them against
each other. A new knob must also be registered in `Common/include/config/ConfigKeyId.h`
and `ConfigKeyMap.cpp`, or `ConfigCatalog` drops it: the workbench will not show it and
IPC cannot deliver it, leaving `DvrReplay --set` as the only way to reach it. Key ids are
append-only; deleted ones leave a hole rather than being renumbered.

### Release differs from Debug

Release compiles with `EGOTOUCH_SERVICE_ENABLE_IPC=0` and without `_DEBUG`. Members used
at an unconditional call site must be **declared** unconditionally; this has broken the
Release build twice while every local Debug build stayed green. Build Release before
pushing anything that touches `DeviceRuntime` or `ServiceHost`.

## Evidence loop

Claims about touch behaviour are argued from recorded frames, not from reading code.
Record a session, replay it offline, compare against the vendor:

```powershell
.\build\arm64-Debug\DvrReplay.exe <recording>.dvrbin --set touch.stroke.hold_min_peak_signal=800
.\build\arm64-Debug\DvrReplay.exe --diff before.csv after.csv
uv run --with pandas scripts\oracle_compare.py ...
```

`DvrReplay` builds only under `EGOTOUCH_DIAG` (Debug): the `.dvrbin` reader only wires
`rawData` into `HeatmapFrame` in diagnostic builds, and DIAG changes the `HeatmapFrame`
layout, so the tool must be compiled in the same configuration as the solvers.

In DVR analysis, "rawdata" means the heatmap matrix inside the frame, not the
`rawDataLength` / `raw.hex` byte block — a recording can carry a heatmap with no solved
contacts or peaks.

## Where the reasoning is written down

- `docs/touch_stack.md` — the working record, including a section of things that were
  tried, measured and rejected. Read it before re-proposing an idea.
- `docs/tsacore_ground_truth.md` — vendor behaviour established by running TSACore as an
  oracle, with the numbers.
- `docs/touch_stack_refactor_reference.md` — Chromium ChromeOS comparison, line by line.
- `docs/KBDMCU_PROTOCOL.md`, `docs/KEYBOARD_IDENTITY.md`, `docs/ACCESSORY_CENTER.md` —
  reverse-engineered MCU and PC Manager behaviour.

## Documentation

`README.md` is Chinese and `README.en.md` English; keep both in sync. The README is for
users — implementation detail belongs in `docs/`.
