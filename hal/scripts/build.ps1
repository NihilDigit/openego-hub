<#
.SYNOPSIS
    构建 gaokun-hal 的两个目标。

.DESCRIPTION
    两个目标的架构是不同的，这是本项目的结构要点，不是构建上的偶然：

      GaokunThpHost.exe        ARM64EC。它在自己的进程里加载 x64 的 THP_Service.dll 及其
                           整条依赖链，所以必须是 EC——纯 ARM64 进程加载不了 x64 DLL。
      GaokunHal.lib        原生 ARM64。只用 Win32 的进程与同步原语，不碰任何华为 DLL，
                           因此调用方（OpenEGoHub 的 UI 层）不必改成 ARM64EC。

    进程边界同时也是架构边界。goodies 的作者卡在「没有现代 GUI 框架支持 arm64ec」，
    这个分法正好绕开它：EC 只存在于后端进程里。

    ARM64EC 产物的 PE machine 字段是 0x8664（x64），这是正常形态——它对加载器伪装成 x64
    以便与 x64 DLL 共存。真正的判据是 load config 里的 CHPEMetadataPointer 非零，-Verify
    检查的是这一点，不要用 machine 字段判断构建是否成功。

.PARAMETER Config
    Debug 或 Release，默认 Release。

.PARAMETER Verify
    构建后校验宿主确为 ARM64EC、控制器库确为原生 ARM64。
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [switch]$Verify
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$OutDir = Join-Path $RepoRoot "build\$Config"
$HostExe = Join-Path $OutDir 'GaokunThpHost.exe'
$HostLib = Join-Path $OutDir 'GaokunHal.lib'

function Import-VsDevEnv {
    if ($env:VSCMD_ARG_TGT_ARCH -eq 'arm64') { return }

    $rel = Join-Path 'VC\Auxiliary\Build' 'vcvarsarm64.bat'
    $roots = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\BuildTools'),
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\Community'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\2022\BuildTools')
    )
    $vcvars = $roots | ForEach-Object { Join-Path $_ $rel } |
              Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $vcvars) { throw "vcvarsarm64.bat not found; install the MSVC ARM64 build tools." }

    Write-Host "==> Importing ARM64 developer environment ..." -ForegroundColor Cyan
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2] -ErrorAction SilentlyContinue
        }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "cl.exe still not on PATH after importing $vcvars."
    }
}

Import-VsDevEnv
New-Item -ItemType Directory -Force $OutDir | Out-Null
New-Item -ItemType Directory -Force "$OutDir\ec" | Out-Null
New-Item -ItemType Directory -Force "$OutDir\native" | Out-Null

# 源码是 UTF-8 且含中文注释。不给 /utf-8 时 cl 按 cp936 解析，会把 #include 之后的内容
# 连同换行一起吃掉，报出一连串「未声明的标识符」，与源码本身无关。
# src 也在包含路径里：通道的线路格式由写者（ARM64EC）与读者（原生 ARM64）共用一个头，
# 两侧分处不同目录，用 "pen/PenChannelLayout.h" 这样的路径互相引用。
$common = @('/nologo', '/utf-8', '/std:c++20', '/EHsc', '/W4',
            "/I$RepoRoot\include", "/I$RepoRoot\src")
# 优化与调试信息按配置走，运行库单独决定——两者不能捆在一起。
#
# 这里不给 /D_DEBUG：_DEBUG 决定的是「引用哪一套 CRT 的诊断符号」，而不是「是否带调试信息」。
# 手写它会把宏和运行库绑死，配上 /MT 就会在链接期要 _CrtDbgReport 而 Release CRT 里没有。
# /MTd 本身就隐含定义 _DEBUG，交给编译器保持一致即可。
$opt = if ($Config -eq 'Release') { @('/O2', '/DNDEBUG') } else { @('/Od', '/Zi') }

# 加载厂商 DLL 的组件一律用 Release CRT，即使在 Debug 配置下。
#
# 厂商的 DLL 是 Release 二进制，用 Debug CRT 的进程去加载它们，工厂函数会直接失败：
# qdcmlib 的 Create_QDCMLibrary 与 Create_QDCMLibrary2 双双返回 null，DLL 却加载成功、
# 符号也解析得到，于是错误看起来像「这台机器没有 qdcmlib」。实测踩过，排查代价很高。
# /Od 与 /Zi 照常给，Debug 配置仍然可以断点调试。
$ecCrt = @('/MT')

# 原生组件跟随配置：GaokunHal.lib 要被上层链接，两侧的 RuntimeLibrary 与
# _ITERATOR_DEBUG_LEVEL 必须一致，否则报 LNK2038。它不碰厂商 DLL，所以没有上面的顾虑。
#

# @() 要包在最外层。if 作为表达式返回单元素数组时 PowerShell 会把它拆成标量字符串，
# @splat 随后按字符展开，于是 "/MTd" 变成 /、M、T、d 四个参数，链接器去找 M.obj。
# 与前面 Get-ChildItem 那处是同一个坑。
$nativeCrt = @(if ($Config -eq 'Release') { '/MT' } else { '/MTd' })

# 加新组件只需在这张表里加一行。凡是要加载华为 x64 DLL 的，一律 ARM64EC。
$ecTargets = @(
    @{ Name = 'GaokunThpHost.exe';      Dir = 'thp';      Libs = @('advapi32.lib') },
    # ole32 用于环境光传感器的 COM，advapi32 用于色彩状态的注册表——色温、护眼、自然色彩
    # 三项各自是一次性命令，状态不持久化就会互相清掉。传感器的 GUID 由 initguid.h 就地定义、
    # 接口 IID 走 __uuidof，所以不需要 sensorsapi.lib。
    @{ Name = 'GaokunDisplay.exe';  Dir = 'display';  Libs = @('ole32.lib', 'oleaut32.lib', 'advapi32.lib') },
    @{ Name = 'GaokunKeyboardHost.exe'; Dir = 'keyboard'; Libs = @('advapi32.lib') },
    @{ Name = 'GaokunPenHost.exe';  Dir = 'pen';      Libs = @('advapi32.lib') }
)

# 不需要厂商 DLL 的组件保持原生 ARM64。充电阈值只用 WMI，没有理由把它拖进 EC。
# ExtraDirs 里的源文件一并编进来，用于让 hostctl 复用控制器实现。
$nativeTargets = @(
    @{ Name = 'GaokunPower.exe'; Dir = 'power'; Libs = @('ole32.lib', 'oleaut32.lib', 'wbemuuid.lib') },
    @{ Name = 'GaokunCtl.exe'; Dir = 'ctl'; ExtraDirs = @('host'); Libs = @() }
)

foreach ($t in $ecTargets) {
    Write-Host "==> Building $($t.Name) (ARM64EC) ..." -ForegroundColor Cyan
    $objDir = Join-Path $OutDir "ec\$($t.Dir)"
    New-Item -ItemType Directory -Force $objDir | Out-Null
    # @(...) 不能省。只匹配到一个文件时管道返回的是字符串而不是数组，@splat 会把它按字符
    # 拆成一堆单字母参数，cl 于是报「无法识别的源文件类型 C」之类并且什么都不编译。
    $sources = @(Get-ChildItem (Join-Path $RepoRoot "src\$($t.Dir)\*.cpp") |
                 ForEach-Object { $_.FullName })
    if ($sources.Count -eq 0) { throw "no sources under src\$($t.Dir)" }
    $exe = Join-Path $OutDir $t.Name
    & cl @common @opt @ecCrt /arm64EC "/Fo:$objDir\" "/Fe:$exe" @sources `
         /link /MACHINE:ARM64EC @($t.Libs)
    if ($LASTEXITCODE -ne 0) { throw "$($t.Name) build failed." }
    Write-Host "[ok] $exe" -ForegroundColor Green
}

foreach ($t in $nativeTargets) {
    Write-Host "==> Building $($t.Name) (native ARM64) ..." -ForegroundColor Cyan
    $objDir = Join-Path $OutDir "native\$($t.Dir)"
    New-Item -ItemType Directory -Force $objDir | Out-Null
    $sources = @(Get-ChildItem (Join-Path $RepoRoot "src\$($t.Dir)\*.cpp") |
                 ForEach-Object { $_.FullName })
    foreach ($extra in @($t.ExtraDirs)) {
        if ($extra) {
            $sources += @(Get-ChildItem (Join-Path $RepoRoot "src\$extra\*.cpp") |
                          ForEach-Object { $_.FullName })
        }
    }
    if ($sources.Count -eq 0) { throw "no sources under src\$($t.Dir)" }
    $exe = Join-Path $OutDir $t.Name
    & cl @common @opt @nativeCrt "/Fo:$objDir\" "/Fe:$exe" @sources /link @($t.Libs)
    if ($LASTEXITCODE -ne 0) { throw "$($t.Name) build failed." }
    Write-Host "[ok] $exe" -ForegroundColor Green
}

Write-Host "==> Building GaokunHal.lib (native ARM64) ..." -ForegroundColor Cyan
New-Item -ItemType Directory -Force "$OutDir\native\host" | Out-Null
# GaokunHal.lib 的来源：宿主控制器，加上不含入口点的 power 读取实现。上层要直接读电池，
# 不能只给它一个 GaokunPower.exe 然后去解析标准输出。
#
# PowerMain.cpp 是 GaokunPower.exe 的 wmain，必须排除——静态库里带一个 wmain，调用方
# 链接时会撞上重复入口点。
$libSources = @(Get-ChildItem (Join-Path $RepoRoot 'src\host\*.cpp')) +
              @(Get-ChildItem (Join-Path $RepoRoot 'src\power\*.cpp') |
                Where-Object { $_.Name -ne 'PowerMain.cpp' })
$hostSources = @($libSources | ForEach-Object { $_.FullName })
& cl @common @opt @nativeCrt /c "/Fo:$OutDir\native\host\" @hostSources
if ($LASTEXITCODE -ne 0) { throw "controller build failed." }
$objs = @(Get-ChildItem "$OutDir\native\host\*.obj" | ForEach-Object { $_.FullName })
if ($objs.Count -eq 0) { throw "controller build produced no objects." }
& lib /nologo /MACHINE:ARM64 "/OUT:$HostLib" @objs
if ($LASTEXITCODE -ne 0) { throw "controller archive failed." }
Write-Host "[ok] $HostLib" -ForegroundColor Green

if ($Verify) {
    function Get-PeInfo($path) {
        $bytes = [IO.File]::ReadAllBytes($path)
        $peOff = [BitConverter]::ToInt32($bytes, 0x3c)
        $machine = [BitConverter]::ToUInt16($bytes, $peOff + 4)
        $optOff = $peOff + 24
        $lcRva = [BitConverter]::ToInt32($bytes, $optOff + 112 + 8 * 10)
        $nsec = [BitConverter]::ToUInt16($bytes, $peOff + 6)
        $optSz = [BitConverter]::ToUInt16($bytes, $peOff + 20)
        $secOff = $optOff + $optSz
        $lcOff = 0
        for ($i = 0; $i -lt $nsec; $i++) {
            $s = $secOff + 40 * $i
            $va = [BitConverter]::ToInt32($bytes, $s + 12)
            $vs = [BitConverter]::ToInt32($bytes, $s + 8)
            $ro = [BitConverter]::ToInt32($bytes, $s + 20)
            if ($lcRva -ge $va -and $lcRva -lt ($va + $vs)) { $lcOff = $ro + ($lcRva - $va); break }
        }
        $chpe = if ($lcOff) { [BitConverter]::ToUInt64($bytes, $lcOff + 0xC8) } else { 0 }
        [pscustomobject]@{ Machine = $machine; Chpe = $chpe }
    }

    $h = Get-PeInfo $HostExe
    Write-Host ("host: machine=0x{0:x4} CHPEMetadataPointer=0x{1:x}" -f $h.Machine, $h.Chpe)
    if ($h.Chpe -eq 0) { throw "GaokunThpHost.exe is not ARM64EC." }
    Write-Host "[ok] host is ARM64EC" -ForegroundColor Green

    # 静态库里没有 PE 头，用 dumpbin 读成员对象的目标架构。原生 ARM64 应报 ARM64，
    # 若报 ARM64EC 说明 /arm64EC 漏加到了这一侧，调用方链接时才会发现，代价更高。
    $line = (& dumpbin /nologo /headers $HostLib 2>&1 |
             Select-String 'machine' | Select-Object -First 1).ToString()
    Write-Host ("lib : " + ($line -replace '\s+', ' ').Trim())
    if ($line -match 'ARM64EC') { throw "GaokunHal.lib must be native ARM64, not ARM64EC." }
    Write-Host "[ok] controller is native ARM64" -ForegroundColor Green
}
