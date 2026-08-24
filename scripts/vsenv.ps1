<#
.SYNOPSIS
    Imports the ARM64 MSVC developer environment into the current PowerShell session.

.DESCRIPTION
    Dot-source this from any script that shells out to cmake or cl. The arm64-* presets
    resolve the compiler from PATH, so a plain PowerShell prompt fails with
    "fatal error C1083: cannot open include file 'cstdint'" — the standard library is
    only reachable once vcvarsarm64.bat has populated INCLUDE/LIB/PATH.

    vcvarsarm64.bat cannot set variables in the calling PowerShell session directly, so
    this runs it inside cmd and copies the resulting environment back out.

.EXAMPLE
    . "$PSScriptRoot\vsenv.ps1"
    Import-VsDevEnv
#>

function Import-VsDevEnv {
    # 不要在这里改 VSLANG。ninja 靠 /showIncludes 的行首前缀抽头文件依赖，CMake 在
    # 配置期探测该前缀并写进 CMakeFiles/rules.ninja。配置期与构建期必须看到同一种
    # 语言，强行改语言只会让两者错开，而依赖失效是静默的。
    # 依赖是否健康由 scripts/verify.ps1 检查，坏了就重新生成构建目录。
    if ($env:VSCMD_ARG_TGT_ARCH -eq 'arm64') { return }   # already inside a dev prompt

    $rel = Join-Path 'VC\Auxiliary\Build' 'vcvarsarm64.bat'
    $roots = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\BuildTools'),
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\Community'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\2022\BuildTools')
    )
    $vcvars = $roots | ForEach-Object { Join-Path $_ $rel } |
              Where-Object { Test-Path $_ } | Select-Object -First 1

    if (-not $vcvars) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path $vswhere) {
            $found = & $vswhere -latest -products * -property installationPath 2>$null |
                     Select-Object -First 1
            if ($found) {
                $candidate = Join-Path $found $rel
                if (Test-Path $candidate) { $vcvars = $candidate }
            }
        }
    }
    if (-not $vcvars) {
        throw "vcvarsarm64.bat not found. Install the MSVC ARM64 build tools, or run from an ARM64 developer prompt."
    }

    Write-Host "==> Importing ARM64 developer environment ..." -ForegroundColor Cyan
    # vcvarsarm64.bat 自己会调 vswhere，装的是 BuildTools 时它不在，于是每次导入都往
    # stderr 打一行「'vswhere.exe' is not recognized」。这不影响结果，但会混进每一份
    # 构建日志，无人值守时按关键字筛日志很碍事。只静音 vcvars 自身的输出，后面的
    # set 照常收集。
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2] -ErrorAction SilentlyContinue
        }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "cl.exe still not on PATH after importing $vcvars."
    }
}
