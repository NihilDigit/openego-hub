# Offline regression: map the installed DLL as data, never execute its entry point.
[CmdletBinding()]
param(
    [string]$VendorDll = 'C:\Program Files\Huawei\HuaweiThpService\himax_thp_drv.dll'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. "$root\..\scripts\vsenv.ps1"
Import-VsDevEnv
$out = Join-Path $root 'build\tests\himax-log'
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($config in @('Debug', 'Release')) {
    $opt = if ($config -eq 'Release') { @('/O2', '/DNDEBUG') } else { @('/Od', '/Zi') }
    $exe = Join-Path $out "HimaxLogSuppressionTest-$config.exe"
    & cl /nologo /utf-8 /std:c++20 /EHsc /W4 /MT @opt "/I$root\src" `
        "$root\tests\HimaxLogSuppressionTest.cpp" "$root\src\thp\ThpModule.cpp" `
        "/Fo:$out\" "/Fd:$out\$config.pdb" "/Fe:$exe"
    if ($LASTEXITCODE -ne 0) { throw "$config test build failed" }
    & $exe $VendorDll
    if ($LASTEXITCODE -ne 0) { throw "$config test failed" }
}
