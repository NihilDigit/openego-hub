<#
.SYNOPSIS
    在真实设备上验证托管模式：拉起、停止、以及父进程消失后的自我收尾。

.DESCRIPTION
    模拟 OpenEGoHub 的角色。触摸设备同一时刻只能由一个实现持有，所以测试前必须让出原厂
    服务；OpenEGoHubService 也要停掉，否则它的 provider coordinator 会在发现
    HuaweiThpService 停止后立刻把它拉回来，两个实现同时抢设备，现象看起来像本项目的缺陷。

    本脚本不改动系统的任何持久状态：不替换文件、不注册服务。结束时把停掉的服务原样拉回。

.PARAMETER KillParent
    不发停止事件，改为直接杀掉扮演父进程的那个进程，用来验证宿主是否仍能走 ThpFuncStop
    干净收尾，而不是留下一个占着设备的孤儿。

.PARAMETER Seconds
    保持接管的时长，默认 25 秒，其间可以试触摸和笔。

.EXAMPLE
    .\smoke.ps1
    .\smoke.ps1 -KillParent
#>
[CmdletBinding()]
param(
    [switch]$KillParent,
    [int]$Seconds = 25
)

$ErrorActionPreference = 'Stop'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
                 '-Seconds', $Seconds)
    if ($KillParent) { $argList += '-KillParent' }
    Start-Process -FilePath 'pwsh.exe' -Verb RunAs -ArgumentList $argList
    return
}

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Host_ = Join-Path $RepoRoot 'build\Release\GaokunThpHost.exe'
if (-not (Test-Path $Host_)) { throw "not built: $Host_  (run scripts\build.ps1)" }

# 脚本自提权后跑在另一个窗口里，判定结果不会回到调用方的终端。留一份成绩单。
Start-Transcript -Path (Join-Path $env:TEMP 'hwthpec-smoke-result.txt') -Force | Out-Null

$log = Join-Path $env:TEMP 'hwthpec-hosted.log'
$stopEventName = 'Global\GaokunThpHostStop'

$stopped = @()
function Stop-IfRunning($name) {
    $svc = Get-Service -Name $name -ErrorAction SilentlyContinue
    if ($svc -and $svc.Status -eq 'Running') {
        Write-Host "==> stopping $name" -ForegroundColor Cyan
        Stop-Service -Name $name -Force
        (Get-Service $name).WaitForStatus('Stopped', '00:00:20')
        $script:stopped += $name
    }
}

# 顺序要紧：Hub 先停，否则它会在 Huawei 停止后立刻把它恢复。
Stop-IfRunning 'OpenEGoHubServiceDebug'
Stop-IfRunning 'OpenEGoHubService'
Stop-IfRunning 'HuaweiThpService'

$stopEvent = New-Object System.Threading.EventWaitHandle(
    $false, [System.Threading.EventResetMode]::ManualReset, $stopEventName)

# 扮演 OpenEGoHub。宿主等的是这个进程的句柄，所以 -KillParent 杀的也是它。
$parent = Start-Process pwsh -ArgumentList @('-NoProfile', '-Command', "Start-Sleep -Seconds 600") `
                             -PassThru -WindowStyle Hidden
Write-Host "==> stand-in parent pid $($parent.Id)" -ForegroundColor Cyan

$child = Start-Process -FilePath $Host_ -PassThru -WindowStyle Hidden `
    -ArgumentList @('--hosted', '--parent', $parent.Id, '--stop-event', $stopEventName) `
    -RedirectStandardOutput $log -RedirectStandardError "$log.err"

Start-Sleep -Seconds 5
if ($child.HasExited) {
    Write-Host "[!] host exited on startup, code $($child.ExitCode)" -ForegroundColor Red
    Get-Content $log, "$log.err" -ErrorAction SilentlyContinue | Select-Object -Last 20
} else {
    Write-Host "[ok] host pid $($child.Id) running; touch and pen should work now" -ForegroundColor Green
    Write-Host "    holding for $Seconds s ..." -ForegroundColor Yellow
    Start-Sleep -Seconds $Seconds
}

if ($KillParent) {
    Write-Host "==> killing the stand-in parent (no stop event)" -ForegroundColor Cyan
    $parent.Kill()
} else {
    Write-Host "==> signalling the stop event" -ForegroundColor Cyan
    [void]$stopEvent.Set()
}

# 宿主要在收到任一信号后自行走完 ThpFuncStop 再退出。留 15 秒观察它是否真的会退,
# 超时就说明收尾路径没走通，需要看日志而不是直接杀掉了事。
if (-not $child.WaitForExit(15000)) {
    Write-Host "[!] host did not exit within 15 s; killing it" -ForegroundColor Red
    $child.Kill()
} else {
    Write-Host "[ok] host exited cleanly, code $($child.ExitCode)" -ForegroundColor Green
}

if (-not $parent.HasExited) { $parent.Kill() }
$stopEvent.Dispose()

foreach ($name in ($stopped | Sort-Object -Descending)) {
    Write-Host "==> starting $name" -ForegroundColor Cyan
    Start-Service $name
}
Get-Service HuaweiThpService, OpenEGoHubService -ErrorAction SilentlyContinue |
    Format-Table Name, Status -AutoSize

Write-Host "--- tail of $log ---"
Get-Content $log -Tail 12 -ErrorAction SilentlyContinue
Stop-Transcript | Out-Null
