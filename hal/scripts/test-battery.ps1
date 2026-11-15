[CmdletBinding()]
param([switch]$Live)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. "$root\..\scripts\vsenv.ps1"
Import-VsDevEnv
$out = Join-Path $root 'build\tests\battery'
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($config in @('Debug', 'Release')) {
    $opt = if ($config -eq 'Release') { @('/O2', '/DNDEBUG') } else { @('/Od', '/Zi') }
    foreach ($architecture in @('ARM64', 'ARM64EC')) {
        $arch = @(if ($architecture -eq 'ARM64EC') { '/arm64EC' })
        $exe = Join-Path $out "BatterySampleTest-$config-$architecture.exe"
        & cl /nologo /utf-8 /std:c++20 /EHsc /W4 /MT @opt @arch `
            "/I$root\include" "/I$root\src" "$root\tests\BatterySampleTest.cpp" `
            "/Fo:$out\" "/Fd:$out\$config-$architecture.pdb" "/Fe:$exe"
        if ($LASTEXITCODE -ne 0) { throw "$config $architecture test build failed" }
        & $exe
        if ($LASTEXITCODE -ne 0) { throw "$config $architecture test failed" }
    }
    # Compile the actual callback consumers without replacing a running host.
    & cl /nologo /utf-8 /std:c++20 /EHsc /W4 /MT @opt /arm64EC /c `
        "/I$root\include" "/I$root\src" "$root\src\pen\PenService.cpp" `
        "$root\src\keyboard\KeyboardService.cpp" "/Fo:$out\" "/Fd:$out\callbacks-$config.pdb"
    if ($LASTEXITCODE -ne 0) { throw "$config callback compilation failed" }
    $liveTest = Join-Path $out "BatteryLiveStateTest-$config.exe"
    & cl /nologo /utf-8 /std:c++20 /EHsc /W4 /MT @opt `
        "/I$root\include" "/I$root\src" "$root\tests\BatteryLiveStateTest.cpp" `
        "/Fo:$out\" "/Fd:$out\live-$config.pdb" "/Fe:$liveTest"
    if ($LASTEXITCODE -ne 0) { throw "$config live state test build failed" }
    & $liveTest
    if ($LASTEXITCODE -ne 0) { throw "$config live state test failed" }
    $diagnostic = Join-Path $out "BatteryLiveDiagnostic-$config.exe"
    & cl /nologo /utf-8 /std:c++20 /EHsc /W4 /MT @opt `
        "/I$root\include" "/I$root\src" "$root\tests\BatteryLiveDiagnostic.cpp" `
        "$root\src\power\BatteryInfo.cpp" "/Fo:$out\" `
        "/Fd:$out\diagnostic-$config.pdb" "/Fe:$diagnostic"
    if ($LASTEXITCODE -ne 0) { throw "$config live diagnostic build failed" }
    if ($Live) {
        & $diagnostic
        if ($LASTEXITCODE -ne 0) { throw "$config live diagnostic found a mismatch" }
    }
}
