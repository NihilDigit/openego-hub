<#
.SYNOPSIS
    停挡路进程 → 构建 → 测试 → 重放对比,任一步失败即中止并说明原因。

.DESCRIPTION
    无人值守时最危险的不是失败，是**假成功**。已经中过两次：ninja 撞到第一个失败目标
    就停，它后面的目标不重编，紧接着的 ctest 于是在上一轮的二进制上跑出全绿。所以这里
    不用退出码单独判构建成败，而是同时检查 "ninja: build stopped" 这行——ninja 打印它
    时退出码可能已经被别的东西吞掉，而它是「后面的目标没建」的唯一可靠信号。

    同理，ctest 报告 0 个用例也当失败：目标没建出来时 ctest 会「成功」地什么都不跑。

.EXAMPLE
    pwsh -File scripts/verify.ps1
    pwsh -File scripts/verify.ps1 -Clean
    pwsh -File scripts/verify.ps1 -Replay
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build\arm64-Debug',
    # 依赖信息可疑时用（构建被中断过、换过分支、改过布局常量）。
    [switch]$Clean,
    # 额外跑一遍语料重放，把接触点统计打出来。
    [switch]$Replay,
    [int]$MinTests = 40
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

$steps = [ordered]@{}
$failed = $null

function Complete-Step {
    param([string]$Name, [bool]$Ok, [string]$Detail = '')
    $script:steps[$Name] = @{ Ok = $Ok; Detail = $Detail }
    if (-not $Ok -and -not $script:failed) { $script:failed = $Name }
    $mark = if ($Ok) { 'ok  ' } else { 'FAIL' }
    Write-Host "[$mark] $Name$(if ($Detail) { " — $Detail" })"
}

# ── 1. 让开被锁的 exe ───────────────────────────────────────────────────
try {
    $out = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'svc.ps1') -Action stop -KillApps 2>&1 | Out-String
    Complete-Step 'stop services' ($LASTEXITCODE -eq 0) ($out.Trim() -replace '\s*\r?\n\s*', '; ')
}
catch {
    Complete-Step 'stop services' $false $_.Exception.Message
}

# ── 2. 构建 ─────────────────────────────────────────────────────────────
if (-not $failed) {
    . (Join-Path $PSScriptRoot 'vsenv.ps1')
    Import-VsDevEnv

    $buildArgs = @('--build', $BuildDir)
    if ($Clean) { $buildArgs += '--clean-first' }

    $log = & cmake @buildArgs 2>&1 | Out-String
    $stopped = $log -match 'ninja:\s*build stopped'
    $compileErrors = [regex]::Matches($log, '(?m)^.*error (C|LNK)\d+.*$') |
                     ForEach-Object { $_.Value.Trim() } | Select-Object -First 5

    if ($stopped -or $compileErrors) {
        $detail = if ($compileErrors) { $compileErrors -join ' | ' } else { 'ninja: build stopped (no compiler error text — a link or a locked file)' }
        Complete-Step 'build' $false $detail
        # 构建没走完就不要往下跑：测试会在旧二进制上给出无意义的绿。
    }
    else {
        Complete-Step 'build' $true
    }
}

# ── 2.5 头文件依赖能不能被抽出来 ────────────────────────────────────────
# ninja 靠 /showIncludes 的行首前缀抽依赖，CMake 在配置期探测该前缀写进 rules.ninja
# 的 msvc_deps_prefix。这条前缀在中文环境下是「注意: 包含文件: 」，而它的**编码**
# 取决于探测时 cl 的输出编码。曾经出现过 CMake 记的是 GBK、cl 实际输出 UTF-8，两者
# 永不相等，于是每个 obj 的依赖记录都是空的：改头文件不触发重编，而求解层几乎全是
# .hpp。失败方式是静默的——构建成功、测试通过、行为不变。
#
# 不数「有多少 obj 记了依赖」：那个数字只反映有多少 obj 在本轮被重编过，刚重新生成
# 构建目录时它天然很小，既会漏报也会误报。直接比对根因——把 rules.ninja 里记的前缀
# 字节，和 cl 此刻实际打出来的字节放在一起比。
if (-not $failed) {
    $rulesPath = Join-Path $BuildDir 'CMakeFiles\rules.ninja'
    if (-not (Test-Path $rulesPath)) {
        Complete-Step 'header deps' $true 'rules.ninja not found, skipped'
    }
    else {
        $raw = [IO.File]::ReadAllBytes($rulesPath)
        $latin = [Text.Encoding]::Latin1.GetString($raw)
        $key = 'msvc_deps_prefix = '
        $at = $latin.IndexOf($key)
        if ($at -lt 0) {
            Complete-Step 'header deps' $false ('rules.ninja carries no msvc_deps_prefix — ' +
                'regenerate the build dir: remove CMakeCache.txt and re-run cmake --preset')
        }
        else {
            $start = $at + $key.Length
            $end = $start
            while ($end -lt $raw.Length -and $raw[$end] -ne 0x0d -and $raw[$end] -ne 0x0a) { $end++ }
            $recorded = $raw[$start..($end - 1)]

            $probeDir = Join-Path ([IO.Path]::GetTempPath()) ("depprobe_" + [guid]::NewGuid().ToString('N'))
            New-Item -ItemType Directory -Force $probeDir | Out-Null
            Set-Content (Join-Path $probeDir 'p.cpp') "#include <cstdint>`nint p(){return 0;}"
            $psi = New-Object Diagnostics.ProcessStartInfo
            $psi.FileName = 'cl.exe'
            $psi.Arguments = '/nologo /showIncludes /c p.cpp'
            $psi.WorkingDirectory = $probeDir
            $psi.RedirectStandardOutput = $true
            $psi.UseShellExecute = $false
            $proc = [Diagnostics.Process]::Start($psi)
            $buf = New-Object IO.MemoryStream
            $proc.StandardOutput.BaseStream.CopyTo($buf)
            $proc.WaitForExit()
            $actual = $buf.ToArray()
            trash $probeDir 2>&1 | Out-Null

            # 逐字节找前缀。用 Latin1 把两边都映成一一对应的字符串，避免任何解码干扰。
            $needle = [Text.Encoding]::Latin1.GetString($recorded)
            $hay = [Text.Encoding]::Latin1.GetString($actual)
            if ($hay.Contains($needle)) {
                Complete-Step 'header deps' $true "prefix matches what cl prints ($($recorded.Length) bytes)"
            }
            else {
                $rHex = ($recorded[0..([Math]::Min(7, $recorded.Length - 1))] | ForEach-Object { '{0:x2}' -f $_ }) -join ' '
                $line = $hay -split "`r?`n" | Where-Object { $_ -match ':\s' } | Select-Object -First 1
                $aBytes = [Text.Encoding]::Latin1.GetBytes($line)
                $aHex = ($aBytes[0..([Math]::Min(7, $aBytes.Length - 1))] | ForEach-Object { '{0:x2}' -f $_ }) -join ' '
                # 自愈:前缀的编码取决于**配置时**控制台的代码页,cl 的输出取决于
                # **构建时**的代码页。两步在不同的壳里跑就会不一致(Bash 工具与
                # PowerShell 工具各有各的代码页),一天里因此手工重生成过四次。
                # 在同一次调用内重新生成再重建,配置与构建于是必然同一个代码页。
                Write-Host "[..  ] header deps — prefix mismatch (recorded $rHex, cl $aHex); regenerating once"
                if (Test-Path $BuildDir) {
                    # 构建目录是可再生的产物,但仍优先走回收站——本仓约定删除一律可找回。
                    if (Get-Command trash -ErrorAction SilentlyContinue) { & trash $BuildDir | Out-Null }
                    else { Remove-Item -Recurse -Force $BuildDir }
                }
                & cmake --preset (Split-Path -Leaf $BuildDir) 2>&1 | Out-String | Out-Null
                $rebuildLog = & cmake --build $BuildDir 2>&1 | Out-String

                $raw2 = [IO.File]::ReadAllBytes($rulesPath)
                $latin2 = [Text.Encoding]::Latin1.GetString($raw2)
                $at2 = $latin2.IndexOf($key)
                $healed = $false
                if ($at2 -ge 0) {
                    $s2 = $at2 + $key.Length
                    $e2 = $s2
                    while ($e2 -lt $raw2.Length -and $raw2[$e2] -ne 0x0d -and $raw2[$e2] -ne 0x0a) { $e2++ }
                    $healed = $hay.Contains([Text.Encoding]::Latin1.GetString($raw2[$s2..($e2 - 1)]))
                }
                if ($healed -and $rebuildLog -notmatch 'ninja:\s*build stopped') {
                    Complete-Step 'header deps' $true 'prefix mismatched; regenerated the build dir and it now matches'
                }
                else {
                    Complete-Step 'header deps' $false ("msvc_deps_prefix does not match cl's output — " +
                        "recorded starts $rHex, cl prints $aHex, and regenerating once did not fix it. " +
                        "Header edits will not trigger rebuilds.")
                }
            }
        }
    }
}

# ── 3. 测试 ─────────────────────────────────────────────────────────────
if (-not $failed) {
    $ctest = & ctest --test-dir $BuildDir -E "slow|perf" --output-on-failure 2>&1 | Out-String

    $passLine = [regex]::Match($ctest, '(\d+)% tests passed,\s*(\d+) tests failed out of\s*(\d+)')
    if (-not $passLine.Success) {
        Complete-Step 'tests' $false 'ctest produced no summary line'
    }
    else {
        $failCount  = [int]$passLine.Groups[2].Value
        $totalCount = [int]$passLine.Groups[3].Value
        $names = [regex]::Matches($ctest, '(?m)^\s*\d+\s*-\s*(\S+)\s*\(Failed\)') |
                 ForEach-Object { $_.Groups[1].Value }

        if ($totalCount -lt $MinTests) {
            # 目标没建出来时 ctest 会「成功」地跑 0 个用例。
            Complete-Step 'tests' $false "only $totalCount tests ran, expected at least $MinTests"
        }
        elseif ($failCount -gt 0) {
            Complete-Step 'tests' $false "$failCount/$totalCount failed: $($names -join ', ')"
        }
        else {
            Complete-Step 'tests' $true "$totalCount passed"
        }
    }
}

# ── 4. 重放（可选）──────────────────────────────────────────────────────
if ($Replay -and -not $failed) {
    $replayExe = Join-Path $BuildDir 'DvrReplay.exe'
    $corpusDir = 'C:\ProgramData\OpenEGoHub\exports\dvr'
    if (-not (Test-Path $replayExe)) {
        Complete-Step 'replay' $false "$replayExe not built"
    }
    else {
        $tmp = Join-Path ([IO.Path]::GetTempPath()) "replay_$([guid]::NewGuid().ToString('N')).csv"
        $lines = @()
        foreach ($name in @('dvr20260823_164645_282742', 'dvr20260823_230439_133806', 'dvr20260823_142120_033695')) {
            $set = Join-Path $corpusDir "$name-session.dvrbin"
            if (-not (Test-Path $set)) { $lines += "$name : MISSING"; continue }
            $r = & $replayExe --dataset $set --out $tmp 2>&1 | Out-String
            $m = [regex]::Match($r, 'contact appearances: .*')
            $lines += "$($name.Substring(12)) : $(if ($m.Success) { $m.Value.Trim() } else { 'no summary' })"
        }
        Remove-Item $tmp -Force -ErrorAction SilentlyContinue
        $lines | ForEach-Object { Write-Host "         $_" }
        Complete-Step 'replay' $true
    }
}

# ── 汇总 ────────────────────────────────────────────────────────────────
Write-Host ''
if ($failed) {
    Write-Host "VERIFY FAILED at: $failed"
    Write-Host $steps[$failed].Detail
    exit 1
}
Write-Host 'VERIFY OK'
exit 0
