<#
.SYNOPSIS
    Configure + build a preset with the ARM64 developer environment already set up.

.DESCRIPTION
    A plain `cmake --build --preset arm64-Release` from a normal PowerShell prompt fails
    with "cannot open include file 'cstdint'": the arm64-* presets resolve cl.exe from
    PATH, and the standard library headers only become visible once vcvarsarm64.bat has
    run. This wrapper imports that environment and pins the repo root, so the preset
    resolves no matter which directory you call it from.

    Release enables LTO, which is memory- and CPU-hungry. -Jobs limits the parallelism so
    a build does not make the machine unusable while you are testing on it.

.PARAMETER Config
    Debug (default) or Release.

.PARAMETER Target
    Restrict the build to specific targets. Builds everything when omitted.

.PARAMETER Jobs
    Parallel job count. Defaults to 2 for Release (LTO), otherwise CMake's default.

.PARAMETER Test
    Run ctest for the configuration after a successful build.

.EXAMPLE
    .\build.ps1 -Config Release
    .\build.ps1 -Config Release -Test
    .\build.ps1 -Target SolversUnit_TouchPenModeSuppress
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',
    [string[]]$Target,
    [int]$Jobs = 0,
    [switch]$Test
)

$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\vsenv.ps1"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Preset   = "arm64-$Config"
$BuildDir = Join-Path $RepoRoot "build\$Preset"

function Write-Step { param($m) Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok   { param($m) Write-Host "[ok] $m" -ForegroundColor Green }

# A running service or workbench holds its own exe open, and the linker cannot replace it.
$locking = @(Get-Process -Name 'OpenEGoHubApp', 'OpenEGoHubService' -ErrorAction SilentlyContinue)
if ($locking.Count -gt 0) {
    $names = ($locking | ForEach-Object { $_.ProcessName } | Sort-Object -Unique) -join ', '
    Write-Host "[!] $names is running and will block linking its own exe (LNK1168)." -ForegroundColor Yellow
    Write-Host "    Use dev-cycle.ps1 to stop, rebuild and restart, or pass -Target to build" -ForegroundColor Yellow
    Write-Host "    something else (tests link to their own binaries and are unaffected)." -ForegroundColor Yellow
}

Import-VsDevEnv

if ($Jobs -le 0 -and $Config -eq 'Release') { $Jobs = 2 }

Push-Location $RepoRoot
try {
    if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
        Write-Step "Configuring $Preset ..."
        cmake --preset $Preset
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
    }

    $args = @('--build', '--preset', $Preset)
    if ($Jobs -gt 0)   { $args += @('--parallel', $Jobs) }
    if ($Target)       { $args += @('--target') + $Target }

    Write-Step "Building $Preset$(if ($Jobs -gt 0) { " (parallel $Jobs)" }) ..."
    cmake @args
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }
    Write-Ok "Build succeeded."

    if ($Test) {
        Write-Step "Running tests for $Preset ..."
        ctest --test-dir "build/$Preset" --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "Tests failed." }
        Write-Ok "Tests passed."
    }
} finally {
    Pop-Location
}
