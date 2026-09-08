<#
.SYNOPSIS
    把构建产物部署到 C:\Program Files\OpenEGoHub，让生产服务跑上新代码。

.DESCRIPTION
    开发时直接从 build 目录跑有两个麻烦：产物被运行中的服务锁住，一重新构建就 LNK1168；
    而且 Debug 服务是手动启动的，重启之后不会自己回来。部署到生产位置可以两者都避开。

    gaokun-hal 的宿主必须与服务同目录：ServiceHost 解析宿主路径时先看自己旁边，找不到才
    退到 hal 的构建目录，而生产位置没有那个相对路径。

    首次部署会把原有文件整份备份到同级的 OpenEGoHub.backup，-Restore 从那里还原。

.PARAMETER Config
    取哪个配置的产物，默认 Debug——托盘与设置窗口只在 EGO_BUILD_TOOLS 打开时构建，
    而那是 Debug 预设才有的。

.PARAMETER Restore
    还原到首次部署前的状态。
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',
    [switch]$Restore
)

$ErrorActionPreference = 'Stop'
# 提权后的窗口是独立的，脚本一抛错窗口就关，服务停在半路却没人看见原因。把错误留在窗口里。
trap {
    Write-Host "[x] $_" -ForegroundColor Red
    Read-Host "press Enter to close"
    break
}

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-Config', $Config)
    if ($Restore) { $argList += '-Restore' }
    Start-Process -FilePath 'pwsh.exe' -Verb RunAs -ArgumentList $argList
    return
}

$RepoRoot  = Split-Path -Parent $PSScriptRoot
$BuildDir  = Join-Path $RepoRoot "build\arm64-$Config"
$HalDir    = Join-Path $RepoRoot "hal\build\$Config"
$Target    = 'C:\Program Files\OpenEGoHub'
$Backup    = 'C:\Program Files\OpenEGoHub.backup'

# 托盘不能由这个提权 shell 直接拉起：Start-Process 会把管理员令牌传下去，托盘变成高完整性,
# 设置窗的每一次配置提交都被 UIPI 挡掉。借 explorer.exe 转一手，它以登录用户身份运行,
# 由它拉起的进程拿到的是普通用户令牌。explorer 是异步转交，拿不到句柄也拿不到退出码,
# 所以等一下再确认进程真的起来了。
function Start-Tray {
    $tray = Join-Path $Target 'OpenEGoHubTray.exe'
    Start-Process -FilePath 'explorer.exe' -ArgumentList "`"$tray`""
    $deadline = (Get-Date).AddSeconds(5)
    while (-not (Get-Process -Name 'OpenEGoHubTray' -ErrorAction SilentlyContinue)) {
        if ((Get-Date) -gt $deadline) {
            Write-Host "[!] OpenEGoHubTray did not start; launch it from a non-elevated prompt" -ForegroundColor Yellow
            return
        }
        Start-Sleep -Milliseconds 200
    }
    Write-Host "[ok] tray started" -ForegroundColor Green
}

function Stop-Stack {
    foreach ($name in 'OpenEGoHubServiceDebug', 'OpenEGoHubService') {
        $svc = Get-Service -Name $name -ErrorAction SilentlyContinue
        if ($svc -and $svc.Status -eq 'Running') {
            Write-Host "==> stopping $name" -ForegroundColor Cyan
            Stop-Service -Name $name -Force
            (Get-Service $name).WaitForStatus('Stopped', '00:00:20')
        }
    }
    # Get-Process 按进程名精确匹配，写成 GaokunKeyboard 杀不到 GaokunKeyboardHost；服务停下后
    # 宿主要等父进程句柄才退出，这里不杀干净，下面的 Copy-Item 会撞上被占用的 exe 而中止。
    $apps = 'OpenEGoHubTray', 'OpenEGoHubSettings', 'GaokunThpHost', 'GaokunPenHost', 'GaokunKeyboardHost'
    Get-Process $apps -ErrorAction SilentlyContinue | Stop-Process -Force
    # 宿主在父进程消失后自己收尾再退出，GaokunThpHost 还要交还设备并等华为服务起来，最长要
    # 好几秒。exe 在这之前仍被映射，Copy-Item 会失败，所以等到它们真的不在了再往下走。
    $deadline = (Get-Date).AddSeconds(20)
    while (Get-Process $apps -ErrorAction SilentlyContinue) {
        if ((Get-Date) -gt $deadline) { throw "hosts still running after 20 s: $((Get-Process $apps -ErrorAction SilentlyContinue).Name -join ', ')" }
        Start-Sleep -Milliseconds 250
    }
}

if ($Restore) {
    if (-not (Test-Path $Backup)) { throw "no backup at $Backup" }
    Stop-Stack
    Copy-Item "$Backup\*" $Target -Recurse -Force
    Write-Host "[ok] restored from $Backup" -ForegroundColor Green
    Start-Service OpenEGoHubService
    (Get-Service OpenEGoHubService).WaitForStatus('Running', '00:00:20')
    Start-Tray
    Read-Host "press Enter to close"
    return
}

if (-not (Test-Path $BuildDir)) { throw "not built: $BuildDir" }
if (-not (Test-Path $HalDir))   { throw "gaokun-hal not built: $HalDir" }

Stop-Stack

# 只在第一次备份，否则第二次部署会把备份覆盖成上一次的产物。
if (-not (Test-Path $Backup)) {
    Write-Host "==> backing up to $Backup" -ForegroundColor Cyan
    Copy-Item $Target $Backup -Recurse -Force
}

# 只复制生产目录原本就有的那些，不要把整个 build 目录（含几十个测试 exe）搬过去。
$appFiles = @(
    'OpenEGoHubService.exe', 'OpenEGoHubTray.exe', 'OpenEGoHubSettings.exe',
    'OpenEGoHubSettings.pri', 'EGoTouchSettings.winmd',
    'App.xbf', 'BatteryIndicator.xbf', 'MainWindow.xbf', 'NotificationWindow.xbf',
    'Microsoft.WindowsAppRuntime.Bootstrap.dll'
)
foreach ($f in $appFiles) {
    $src = Join-Path $BuildDir $f
    if (Test-Path $src) { Copy-Item $src $Target -Force }
    else { Write-Host "[!] missing $f" -ForegroundColor Yellow }
}

# hal 的宿主与一次性组件。必须和服务同目录，见文件头的说明。
$halFiles = @(
    'GaokunThpHost.exe', 'GaokunPenHost.exe', 'GaokunKeyboardHost.exe',
    'GaokunDisplay.exe', 'GaokunPower.exe', 'GaokunCtl.exe',
    # 色域组件必须带上它。System32 里的同名 DLL 在本机加载得上却拒绝初始化，
    # 详见 gaokun-hal 的 src/display/Qdcm.cpp。
    'qdcmlib.dll'
)
foreach ($f in $halFiles) {
    $src = Join-Path $HalDir $f
    if (Test-Path $src) { Copy-Item $src $Target -Force }
    else { Write-Host "[!] missing hal component $f" -ForegroundColor Yellow }
}

Write-Host "[ok] deployed $Config to $Target" -ForegroundColor Green
Start-Service OpenEGoHubService
(Get-Service OpenEGoHubService).WaitForStatus('Running', '00:00:20')
Get-Service OpenEGoHubService | Format-Table Name, Status -AutoSize
Start-Tray
Read-Host "press Enter to close"
