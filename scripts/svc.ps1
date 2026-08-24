<#
.SYNOPSIS
    停启 OpenEGoHub 服务与相关进程，需要时自行提权。

.DESCRIPTION
    构建会被运行中的服务、工作台、托盘锁住各自的 exe（LNK1168）。这个脚本把
    「停掉挡路的东西」做成一条命令，并在权限不足时自己申请提权。

    提权走 ShellExecute，而标准流重定向要求 UseShellExecute=false，两者互斥，
    所以子进程不能直接把输出回传给父进程。这里让子进程把自己的全部流（含错误
    与 native 命令的 stderr）重定向进一个日志文件，父进程等它结束后整份读回来
    再按原样打印，并沿用子进程的退出码——否则提权分支里的失败会静默消失，
    调用方只看到一个没有上下文的非零退出码。

.EXAMPLE
    pwsh -File scripts/svc.ps1 -Action status
    pwsh -File scripts/svc.ps1 -Action stop
    pwsh -File scripts/svc.ps1 -Action stop -KillApps
    pwsh -File scripts/svc.ps1 -Action restart
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('status', 'stop', 'start', 'restart')]
    [string]$Action,

    [string]$Service = 'OpenEGoHubServiceDebug',

    # 一并结束工作台、托盘与设置窗口。它们各锁自己的 exe，而服务只锁
    # OpenEGoHubService.exe，全量构建这几个都要让开。
    [switch]$KillApps,

    [int]$TimeoutSeconds = 30,

    # 以下两个由提权分支内部使用，不要手工传。
    [switch]$Elevated,
    [string]$LogPath
)

$ErrorActionPreference = 'Stop'

# 锁住构建产物的进程，名字取自输出文件名。列表只此一份：status 与 stop 两处都读它，
# 早先各写各的，设置窗口只加进了其中一处都没有，于是 -KillApps 之后链接仍然 LNK1104。
# EGoTouchApp 是改名前的工作台名，留着以便在旧构建树上也管用。
$script:LockingProcesses = @(
    'OpenEGoHubApp',
    'OpenEGoHubTray',
    'OpenEGoHubSettings',
    'EGoTouchApp'
)

function Test-Elevated {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-ServiceStatus {
    param([string]$Name)
    $svc = Get-Service -Name $Name -ErrorAction SilentlyContinue
    if (-not $svc) { return 'NotInstalled' }
    return [string]$svc.Status
}

function Wait-ServiceState {
    param([string]$Name, [string]$Desired, [int]$Seconds)
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        if ((Get-ServiceStatus -Name $Name) -eq $Desired) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return ((Get-ServiceStatus -Name $Name) -eq $Desired)
}

function Stop-LockingApps {
    # 托盘可能以提权身份运行，所以这一步也放在提权分支里做。
    foreach ($proc in $script:LockingProcesses) {
        $running = Get-Process -Name $proc -ErrorAction SilentlyContinue
        if (-not $running) { continue }
        Write-Host "killing $proc (pid $($running.Id -join ','))"
        $running | Stop-Process -Force -ErrorAction SilentlyContinue
    }
}

function Invoke-Action {
    switch ($Action) {
        'status' {
            Write-Host "$Service : $(Get-ServiceStatus -Name $Service)"
            foreach ($proc in $script:LockingProcesses) {
                $running = Get-Process -Name $proc -ErrorAction SilentlyContinue
                Write-Host ("{0,-18} : {1}" -f $proc, $(if ($running) { "running (pid $($running.Id -join ','))" } else { 'not running' }))
            }
        }
        'stop' {
            if ($KillApps) { Stop-LockingApps }
            $state = Get-ServiceStatus -Name $Service
            if ($state -eq 'NotInstalled') { Write-Host "$Service not installed, nothing to stop"; return }
            if ($state -eq 'Stopped') { Write-Host "$Service already stopped"; return }
            Stop-Service -Name $Service -Force
            if (-not (Wait-ServiceState -Name $Service -Desired 'Stopped' -Seconds $TimeoutSeconds)) {
                throw "$Service did not reach Stopped within ${TimeoutSeconds}s (now $(Get-ServiceStatus -Name $Service))"
            }
            Write-Host "$Service stopped"
        }
        'start' {
            $state = Get-ServiceStatus -Name $Service
            if ($state -eq 'NotInstalled') { throw "$Service is not installed" }
            if ($state -eq 'Running') { Write-Host "$Service already running"; return }
            Start-Service -Name $Service
            if (-not (Wait-ServiceState -Name $Service -Desired 'Running' -Seconds $TimeoutSeconds)) {
                throw "$Service did not reach Running within ${TimeoutSeconds}s (now $(Get-ServiceStatus -Name $Service))"
            }
            Write-Host "$Service started"
        }
        'restart' {
            if ($KillApps) { Stop-LockingApps }
            $state = Get-ServiceStatus -Name $Service
            if ($state -eq 'NotInstalled') { throw "$Service is not installed" }
            if ($state -ne 'Stopped') {
                Stop-Service -Name $Service -Force
                if (-not (Wait-ServiceState -Name $Service -Desired 'Stopped' -Seconds $TimeoutSeconds)) {
                    throw "$Service did not stop within ${TimeoutSeconds}s"
                }
            }
            Start-Service -Name $Service
            if (-not (Wait-ServiceState -Name $Service -Desired 'Running' -Seconds $TimeoutSeconds)) {
                throw "$Service did not restart within ${TimeoutSeconds}s"
            }
            Write-Host "$Service restarted"
        }
    }
}

# ── 提权分支 ────────────────────────────────────────────────────────────
# status 只读，不必提权；其余动作需要 SERVICE_STOP/START 权限。
if ($Action -ne 'status' -and -not $Elevated -and -not (Test-Elevated)) {
    $log = [IO.Path]::Combine([IO.Path]::GetTempPath(), "egosvc_$([guid]::NewGuid().ToString('N')).log")

    # 这里不用 -f：命令体本身带大量花括号，格式化字符串要求逐个转义成 {{ }}，
    # 一处漏掉就是运行期解析错误，而且错误信息完全指不到出处。
    $killFlag = if ($KillApps) { ' -KillApps' } else { '' }
    $body = "& '$PSCommandPath' -Action $Action -Service '$Service'" +
            " -TimeoutSeconds $TimeoutSeconds$killFlag -Elevated"
    # *> 把 6 条流全部并进日志；native 命令的 stderr 也在内。
    # 结尾显式 exit，让父进程能拿到真实退出码。
    $command = "& { $body } *> '$log'; exit `$LASTEXITCODE"

    $inner = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', $command)

    Write-Host "elevating for '$Action' ..."
    try {
        $proc = Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList $inner `
                              -Verb RunAs -Wait -PassThru
    }
    catch {
        Write-Error "elevation refused or failed: $($_.Exception.Message)"
        exit 2
    }

    if (Test-Path $log) {
        Get-Content -Path $log -Raw -ErrorAction SilentlyContinue | Write-Host
        Remove-Item $log -Force -ErrorAction SilentlyContinue
    }
    else {
        Write-Warning "elevated run produced no log at $log"
    }

    exit $proc.ExitCode
}

# ── 实际动作 ────────────────────────────────────────────────────────────
try {
    Invoke-Action
    exit 0
}
catch {
    # 这里刻意不用 Write-Error：$ErrorActionPreference = 'Stop' 会让它自己再抛一次，
    # 提权分支里脚本就在把消息写进日志之前终止，父进程只拿到一个空日志和退出码 1。
    Write-Host "ERROR: $($_.Exception.Message)"
    if ($_.ScriptStackTrace) { Write-Host $_.ScriptStackTrace }
    exit 1
}
