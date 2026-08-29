<#
.SYNOPSIS
    Debug iteration loop for OpenEGoHubService: stop -> build -> start.

.DESCRIPTION
    The debug service (OpenEGoHubServiceDebug) runs straight out of the build tree, so its
    exe is locked while it runs and a rebuild would fail to link. This script enforces the
    stop -> build -> start order and handles the things that bite in practice:

      * Release/Debug mutual exclusion. Both services start a THP host against the same
        Himax device and read the same BT-MCU HID reports. Running both at once produces
        undefined behaviour, so the Release service is suspended for the duration and
        restored on -RestoreRelease.

      * Build environment. cmake --preset resolves CMakePresets.json against the current
        directory, and the arm64-* presets take cl.exe from PATH. Running this script from
        scripts\ used to fail on both counts, so it pins the repo root and imports the
        ARM64 developer environment itself.

      * Touch downtime. Switching services stops and restarts the THP host that owns the
        controller, so touch and pen stop responding for a few seconds. Keep a
        keyboard/mouse attached.

      * Integrity levels. This script has to be elevated to control services, but the tray
        and the settings window must not inherit that: UIPI blocks window messages from a
        medium-integrity process to a high-integrity one, and every setting the user
        changes travels that way. Both are launched through explorer.exe so they end up
        with the ordinary user token they have in a real deployment.

      * Stale incremental state. Ninja has been observed losing a header dependency after
        an interrupted build, then reporting "no work to do" while linking an object
        compiled before the change — the service kept running old code through several
        edit/rebuild rounds and only a scan of the binary's string literals exposed it.
        The script now compares service inputs with the output and automatically takes
        the clean path when the output is older; -Clean remains the explicit escape hatch.

.PARAMETER Clean
    Delete the build directory first, forcing a full reconfigure and rebuild. Reach for
    this when a source change does not seem to take effect: the incremental state, not the
    code, is the usual culprit.

.PARAMETER SkipBuild
    Restart the debug service without rebuilding.

.PARAMETER NoStart
    Build and leave the service stopped (attach a debugger, then start it yourself).

.PARAMETER NoTray
    Skip relaunching EGoTouchTray. It normally comes back with the service, since it is
    part of the running system rather than a diagnostic tool. Skipping it also leaves the
    provider lease released, so Huawei remains the active touch provider.

.PARAMETER Settings
    Launch the WinUI settings window after the service is up. It is not relaunched by
    default: Stop-UiProcesses closes it because its exe is a build output, but unlike the
    tray it is a window the user opens on demand rather than a resident component.

.PARAMETER RestoreRelease
    Stop the debug service and hand control back to the installed Release service.
    Does not build, so it works even when the toolchain is unavailable.

.EXAMPLE
    .\dev-cycle.ps1
    .\dev-cycle.ps1 -Settings
    .\dev-cycle.ps1 -Clean
    .\dev-cycle.ps1 -RestoreRelease
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$SkipBuild,
    [switch]$NoStart,
    [switch]$NoTray,
    [switch]$Settings,
    [switch]$RestoreRelease
)

$ErrorActionPreference = 'Stop'

# Shared with build.ps1 so the toolchain lookup lives in exactly one place.
. (Join-Path $PSScriptRoot 'vsenv.ps1')

$RepoRoot   = Split-Path -Parent $PSScriptRoot
$Preset     = 'arm64-Debug'
$BuildDir   = Join-Path $RepoRoot "build\$Preset"
$ServiceExe = Join-Path $BuildDir 'OpenEGoHubService.exe'
$UiBuildDir = $BuildDir
$TrayExe    = Join-Path $UiBuildDir 'OpenEGoHubTray.exe'
# WinUI 工程走 MSBuild，但 CMake 用 /p:OutDir 把它的产物也导向了这个构建目录。取这里而不是
# vcxproj 自己的 ARM64\Debug\：那份只在单独用 msbuild 编译时才更新，从这个脚本启动会拉起一个
# 比刚构建的版本更旧的 exe。
$SettingsExe = Join-Path $UiBuildDir 'OpenEGoHubSettings.exe'
$DebugSvc   = 'OpenEGoHubServiceDebug'
$ReleaseSvc = 'OpenEGoHubService'

function Write-Step { param($m) Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Warn { param($m) Write-Host "[!] $m" -ForegroundColor Yellow }
function Write-Ok   { param($m) Write-Host "[ok] $m" -ForegroundColor Green }

# 这个脚本必须提权（要控制服务），而提权 shell 里 Start-Process 会把管理员令牌传给子进程。
# 托盘于是变成高完整性，设置窗是中完整性，UIPI 挡掉后者发往前者的窗口消息，托盘的每一次配置
# 提交都变成「托盘没有响应」。真实部署里两者都是中完整性，这个故障只由本脚本造出来。
# 借 explorer.exe 转一手：它以登录用户身份运行，由它拉起的进程拿到的是普通用户令牌。
function Start-DeElevated {
    param($Path)
    Start-Process -FilePath 'explorer.exe' -ArgumentList "`"$Path`""
    # explorer 是异步转交，拿不到子进程句柄也拿不到退出码，失败时什么都不说。所以这里等一下
    # 再确认进程真的起来了，否则「启动了」只是脚本的一厢情愿。
    $name = [IO.Path]::GetFileNameWithoutExtension($Path)
    $deadline = (Get-Date).AddSeconds(5)
    while (-not (Get-Process -Name $name -ErrorAction SilentlyContinue)) {
        if ((Get-Date) -gt $deadline) {
            Write-Warn "$name did not start. Launch it manually from a non-elevated prompt."
            return
        }
        Start-Sleep -Milliseconds 200
    }
    Write-Ok "$name running (unelevated)."
}

function Assert-Admin {
    $isAdmin = ([Security.Principal.WindowsPrincipal]`
        [Security.Principal.WindowsIdentity]::GetCurrent()`
        ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) {
        throw "Administrator privileges required (service control). Re-run from an elevated shell."
    }
}

function Get-SvcState {
    param($Name)
    $svc = Get-Service -Name $Name -ErrorAction SilentlyContinue
    if ($null -eq $svc) { return $null }
    return $svc.Status
}

function Get-SvcBinaryPath {
    $svc = Get-CimInstance Win32_Service -Filter "Name='$DebugSvc'" -ErrorAction SilentlyContinue
    if ($null -eq $svc -or [string]::IsNullOrWhiteSpace($svc.PathName)) { return $null }
    $raw = $svc.PathName.Trim()
    if ($raw.StartsWith('"')) {
        $closing = $raw.IndexOf('"', 1)
        if ($closing -le 1) { return $null }
        return $raw.Substring(1, $closing - 1)
    }
    return ($raw -split '[ \t]+', 2)[0]
}

function Assert-DebugServiceBinary {
    $configured = Get-SvcBinaryPath
    $installHint = 'scripts\install_debug_service.bat'
    if ($null -eq $configured) {
        throw "Unable to read $DebugSvc ImagePath. Reinstall it with $installHint."
    }
    $expected = [IO.Path]::GetFullPath($ServiceExe)
    $actual = [IO.Path]::GetFullPath($configured)
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals($actual, $expected)) {
        throw "$DebugSvc points to '$actual', but this mode requires '$expected'. Run $installHint as administrator."
    }
}

function Test-ServiceBinaryStale {
    # The current CMake/Ninja toolchain can record a mojibake /showIncludes prefix,
    # leaving the dependency database empty.  A timestamp guard is deliberately
    # conservative: if any service input is newer than the executable, rebuild the
    # service from a fresh configure instead of trusting that database.
    if (-not (Test-Path -LiteralPath $ServiceExe)) {
        return $true
    }

    $binaryTime = (Get-Item -LiteralPath $ServiceExe).LastWriteTimeUtc
    $inputs = @(
        Get-Item -LiteralPath (Join-Path $RepoRoot 'CMakeLists.txt'),
                     (Join-Path $RepoRoot 'CMakePresets.json') -ErrorAction SilentlyContinue
    )
    # EGoTouchService links Common and the Device/Host sources as well.  Include those
    # source roots, but not test fixtures or the private .bak snapshots.
    foreach ($root in @('EGoTouchService', 'Common')) {
        $rootPath = Join-Path $RepoRoot $root
        $inputs += @(Get-ChildItem -LiteralPath $rootPath -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object {
                $_.Extension -ne '.bak' -and
                $_.FullName -notmatch '[\\/]tests[\\/]'
            })
    }
    $newest = $inputs |
        Sort-Object -Property LastWriteTimeUtc -Descending |
        Select-Object -First 1
    if ($null -ne $newest -and $newest.LastWriteTimeUtc -gt $binaryTime) {
        Write-Warn "Service output is older than '$($newest.FullName)' ($($newest.LastWriteTime))."
        return $true
    }
    return $false
}

function Stop-Svc {
    param($Name)
    $state = Get-SvcState $Name
    if ($null -eq $state) { return $false }
    if ($state -eq 'Stopped') { return $false }
    Write-Step "Stopping $Name ..."
    Stop-Service -Name $Name -Force -ErrorAction Stop
    # Stop-Service returns before the SCM transition completes; wait it out so the exe
    # is actually unlocked before we try to relink it.
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-SvcState $Name) -ne 'Stopped') {
        if ((Get-Date) -gt $deadline) { throw "$Name did not stop within 30s." }
        Start-Sleep -Milliseconds 300
    }
    Write-Ok "$Name stopped."
    return $true
}

function Stop-UiProcesses {
    # These exes are build outputs, so a running instance blocks the link with LNK1168
    # exactly the way a running service does.
    $procs = @(Get-Process -Name 'OpenEGoHubTray', 'OpenEGoHubSettings' -ErrorAction SilentlyContinue)
    if ($procs.Count -eq 0) { return }
    $names = ($procs | ForEach-Object { $_.ProcessName } | Sort-Object -Unique) -join ', '
    Write-Step "Closing $names (they lock their own exe against the linker) ..."
    foreach ($p in $procs) {
        try { $p.CloseMainWindow() | Out-Null } catch {}
    }
    $deadline = (Get-Date).AddSeconds(5)
    while ((Get-Process -Name 'OpenEGoHubTray', 'OpenEGoHubSettings' -ErrorAction SilentlyContinue) -and
           (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 200
    }
    $stubborn = @(Get-Process -Name 'OpenEGoHubTray', 'OpenEGoHubSettings' -ErrorAction SilentlyContinue)
    if ($stubborn.Count -gt 0) { $stubborn | Stop-Process -Force }
    Start-Sleep -Milliseconds 300
    Write-Ok "$names closed."
}

function Suspend-ReleaseService {
    $state = Get-SvcState $ReleaseSvc
    if ($null -eq $state) { return }
    if ($state -eq 'Running') {
        Write-Warn "$ReleaseSvc is running; it contends with the debug service for the touch controller."
        Stop-Svc $ReleaseSvc | Out-Null
    }
    # Keep it from coming back on the next boot or via recovery actions while iterating.
    Set-Service -Name $ReleaseSvc -StartupType Manual -ErrorAction SilentlyContinue
}

if ($RestoreRelease) {
    Assert-Admin
    Stop-Svc $DebugSvc | Out-Null
    if ($null -eq (Get-SvcState $ReleaseSvc)) {
        Write-Warn "$ReleaseSvc is not installed; nothing to restore."
        return
    }
    Write-Step "Restoring $ReleaseSvc ..."
    Set-Service -Name $ReleaseSvc -StartupType Automatic
    Start-Service -Name $ReleaseSvc
    Write-Ok "$ReleaseSvc running (Automatic). Touch is back."
    return
}

Assert-Admin

if ($null -eq (Get-SvcState $DebugSvc)) {
    throw "$DebugSvc is not installed. Run scripts\install_debug_service.bat as administrator first."
}

Assert-DebugServiceBinary

Suspend-ReleaseService
Stop-Svc $DebugSvc | Out-Null

if (-not $SkipBuild) {
    Stop-UiProcesses
    Import-VsDevEnv
    $forceClean = $Clean
    if (-not $forceClean -and (Test-ServiceBinaryStale)) {
        Write-Warn "Incremental dependency state is not trusted; forcing a clean $Preset rebuild."
        $forceClean = $true
    }
    if ($forceClean -and (Test-Path $BuildDir)) {
        # 直接删除而不是走回收站：构建目录按定义可再生，而且有几个 GB，塞进回收站只是把问题
        # 挪个地方。源码不在这里面。
        Write-Step "Removing $BuildDir for a clean rebuild ..."
        Remove-Item -Recurse -Force -LiteralPath $BuildDir
        Write-Ok "Build directory removed."
    }
    Push-Location $RepoRoot
    try {
        if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
            Write-Step "Configuring $Preset ..."
            cmake --preset $Preset
            if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
        }

        Write-Step "Building $Preset ..."
        cmake --build --preset $Preset --target EGoTouchService EGoTouchTray
        if ($LASTEXITCODE -ne 0) {
            Write-Warn "Build failed. Services left stopped; run -RestoreRelease to get touch back."
            throw "Build failed."
        }
    } finally {
        Pop-Location
    }
    Write-Ok "Build succeeded."
}

if (-not (Test-Path $ServiceExe)) { throw "Service binary not found: $ServiceExe" }

if ($NoStart) {
    Write-Ok "Build complete. $DebugSvc left stopped (attach a debugger, then: sc start $DebugSvc)."
    return
}

Write-Step "Starting $DebugSvc ..."
try {
    Start-Service -Name $DebugSvc -ErrorAction Stop
} catch {
    Write-Warn "Failed to start $DebugSvc. Run -RestoreRelease to bring touch back."
    throw
}
Write-Ok "$DebugSvc running from $ServiceExe"
Write-Host "     log: C:\ProgramData\OpenEGoHub\logs\OpenEGoHubServiceDebug.txt"

if ($NoTray) {
    Write-Warn "-NoTray leaves the provider lease released, so touch intentionally stays on Huawei; omit -NoTray to exercise the OpenEGo path."
}

if (-not $NoTray) {
    # 托盘属于运行中的系统而不是诊断工具，所以它随服务一起回来。上面停掉它是因为
    # 它的 exe 也是构建产物，链接时会被占用。
    if (Test-Path $TrayExe) {
        Write-Step "Launching pen status tray ..."
        Start-DeElevated $TrayExe
    } else {
        Write-Warn "EGoTouchTray not built; pen status panel unavailable."
    }
}

if ($Settings) {
    if (Test-Path $SettingsExe) {
        # 与托盘同为中完整性，两者之间的窗口消息才通得过。
        Write-Step "Launching settings window ..."
        Start-DeElevated $SettingsExe
    } else {
        Write-Warn "EGoTouchSettings not built; build it with msbuild (see Tools\EGoTouchSettings)."
    }
}
