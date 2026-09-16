<#
.SYNOPSIS
    在真实设备上验证宿主服务：拉起、停止、以及控制方消失后的自我收尾。

.DESCRIPTION
    模拟 OpenEGoHub 的角色。触摸设备同一时刻只能由一个实现持有，所以测试前必须让出原厂
    服务；OpenEGoHubService 也要停掉，否则它的 provider coordinator 会在发现
    HuaweiThpService 停止后立刻把它拉回来，两个实现同时抢设备，现象看起来像本项目的缺陷。

    宿主必须作为服务运行，不能用 CreateProcess 拉起：THP_Service.dll 内部的 ServiceMain
    要先向 SCM 注册控制处理器才会订阅电源通知，否则原厂的 ApDaemon 会挂起整条算法链。
    详见 hal/docs/thp-power-gate.md。本脚本因此会注册宿主服务（若尚未注册），并在结束时
    只删除自己创建的那一条。文件不替换，停掉的服务原样拉回。

.PARAMETER KillParent
    不经 SCM 停止，改为直接杀掉扮演控制方的那个进程，用来验证宿主是否仍能走 ThpFuncStop
    干净收尾并交还原厂，而不是留下一个占着设备的孤儿。

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

# 宿主自己写这份日志；它是服务，没有控制台可以重定向。
$log = Join-Path $env:ProgramData 'OpenEGoHub\logs\GaokunThpHost.log'
$HostService = 'OpenEGoHubThpHost'

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

# 扮演 OpenEGoHub。宿主看护的是这个进程的句柄，所以 -KillParent 杀的也是它。
$parent = Start-Process pwsh -ArgumentList @('-NoProfile', '-Command', "Start-Sleep -Seconds 600") `
                             -PassThru -WindowStyle Hidden
Write-Host "==> stand-in parent pid $($parent.Id)" -ForegroundColor Cyan

# HostController 平时会自己注册这条服务。这里手工做同样的事，以便单独驱动宿主。
$createdService = $false
if (-not (Get-Service $HostService -ErrorAction SilentlyContinue)) {
    sc.exe create $HostService binPath= "`"$Host_`"" type= own start= demand `
            DisplayName= "OpenEGo Hub 触控宿主 (smoke)" | Out-Null
    $createdService = $true
    Write-Host "==> registered $HostService" -ForegroundColor Cyan
} else {
    sc.exe config $HostService binPath= "`"$Host_`"" | Out-Null
}

sc.exe start $HostService --parent $parent.Id | Out-Null
(Get-Service $HostService).WaitForStatus('Running', '00:00:20')

Start-Sleep -Seconds 5
$svc = Get-Service $HostService
if ($svc.Status -ne 'Running') {
    Write-Host "[!] host service is $($svc.Status) instead of Running" -ForegroundColor Red
    Get-Content $log -Tail 20 -ErrorAction SilentlyContinue
} else {
    Write-Host "[ok] $HostService running; touch and pen should work now" -ForegroundColor Green
    Write-Host "    holding for $Seconds s ..." -ForegroundColor Yellow
    Start-Sleep -Seconds $Seconds
}

if ($KillParent) {
    Write-Host "==> killing the stand-in parent (no stop request)" -ForegroundColor Cyan
    $parent.Kill()
} else {
    Write-Host "==> stopping $HostService through the SCM" -ForegroundColor Cyan
    sc.exe stop $HostService | Out-Null
}

# 两条路都要走到 ThpFuncStop。留 15 秒观察服务是否真的停下，超时就说明收尾路径没走通，
# 需要看日志而不是直接杀掉了事。-KillParent 时停止由宿主自己的看护线程发起。
try {
    (Get-Service $HostService).WaitForStatus('Stopped', '00:00:15')
    Write-Host "[ok] host stopped cleanly" -ForegroundColor Green
} catch {
    Write-Host "[!] host did not stop within 15 s" -ForegroundColor Red
    Get-Process GaokunThpHost -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill() }
}

if (-not $parent.HasExited) { $parent.Kill() }

# 只删自己注册的那条。HostController 平时注册的同名服务要留着，否则下次接管还得重建。
if ($createdService) {
    sc.exe delete $HostService | Out-Null
    Write-Host "==> removed $HostService" -ForegroundColor Cyan
}

foreach ($name in ($stopped | Sort-Object -Descending)) {
    Write-Host "==> starting $name" -ForegroundColor Cyan
    Start-Service $name
}
Get-Service HuaweiThpService, OpenEGoHubService -ErrorAction SilentlyContinue |
    Format-Table Name, Status -AutoSize

Write-Host "--- tail of $log ---"
Get-Content $log -Tail 12 -ErrorAction SilentlyContinue
Stop-Transcript | Out-Null
