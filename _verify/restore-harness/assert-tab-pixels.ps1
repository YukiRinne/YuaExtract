# 断言 E-11：页签像素幽灵回归（独立入口）
#
#   pwsh -File assert-tab-pixels.ps1
#   pwsh -File assert-tab-pixels.ps1 -SnapshotJson Run\out\tab-pixels.json -ChecksJson Run\out\checks-tab-pixels.json
#
# 判据与标定依据见 README「E-11」一节；抓图由 run-tab-pixels.ps1 产生。
# 本脚本**只读快照**（BMP/PNG + JSON），不启动产品、不读现场 ——
# 现场会被下一条用例改掉（README 踩坑 #22）。
#
# 为什么要单独一条（而不是塞进可见性断言）：
#   用户看到的"叠字"是**像素残留**：切到制作页签后首页控件的 `IsWindowVisible` 全是 False
#   （可见性正确），旧像素却留在窗口表面上。只读窗口状态的断言在缺陷存在时**照样全绿**，
#   所以幽灵判据必须是逐像素比对。

[CmdletBinding()]
param(
    [string]$SnapshotJson = '',
    [string]$ChecksJson = '',
    [string]$DiffDir = '',
    # 快照缺失：默认硬失败（正式跑）；单独调试断言脚本时可关掉
    [switch]$AllowMissingSnapshot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')
. (Join-Path $root 'lib\HarnessPixels.ps1')

$runRoot = Join-Path $root 'Run'
$outRoot = Join-Path $runRoot 'out'
if (-not $SnapshotJson) { $SnapshotJson = Join-Path $outRoot 'tab-pixels.json' }
if (-not $DiffDir) { $DiffDir = Join-Path $outRoot 'tab-pixels' }
if (-not $ChecksJson) { $ChecksJson = Join-Path $outRoot 'checks-tab-pixels.json' }

# ---------- 性能护栏（沿用既有断言的风格：每段打印 [assert <ms>]、有预算与迭代上限）----------

$script:traceStarted = Get-Date
$script:sectionBudgetMs = 120000
$script:traceLog = New-Object System.Collections.ArrayList
function Trace-Step([string]$Text)
{
    $ms = [int]((Get-Date) - $script:traceStarted).TotalMilliseconds
    Write-Host ("[assert {0,8:N0}ms] {1}" -f $ms, $Text)
    [void]$script:traceLog.Add([pscustomobject]@{ AtMs = $ms; Text = $Text })
}
function Get-SlowSections
{
    $slow = New-Object System.Collections.ArrayList
    for ($index = 1; $index -lt $script:traceLog.Count; $index++)
    {
        $delta = $script:traceLog[$index].AtMs - $script:traceLog[$index - 1].AtMs
        if ($delta -gt $script:sectionBudgetMs) { [void]$slow.Add([pscustomobject]@{ Ms = $delta; Text = $script:traceLog[$index - 1].Text }) }
    }
    return $slow
}

Reset-HarnessChecks

Trace-Step ("读抓图快照：{0}" -f $SnapshotJson)
$summary = Invoke-TabPixelChecks -SnapshotJson $SnapshotJson -DiffDir $DiffDir -RequireSnapshot (-not $AllowMissingSnapshot)
Trace-Step '像素比对完成'

if ($summary)
{
    Trace-Step ("状态矩阵：最差 {0} = {1:P3}（阈值 {2:P2}）；首页历史 {3}；跨进程 {4}" -f `
            $summary.WorstPair, $summary.WorstMakeRatio, $summary.StateThreshold, $summary.HomeOk, $summary.CrossOk)
    # 单段预算：像素比对本身是秒级；这里只把"超预算"的事实记下来，不假装通过。
    $slow = @(Get-SlowSections)
    if ($slow.Count -gt 0)
    {
        Add-Check -Id 'E-11-P' -Title '性能护栏：单段耗时未超预算' -Ok $false -Severity 'warn' `
            -Details @($slow | ForEach-Object { ('{0} 耗时 {1:N0} ms（预算 {2:N0} ms）' -f $_.Text, $_.Ms, $script:sectionBudgetMs) })
    }
    else
    {
        Add-Check -Id 'E-11-P' -Title '性能护栏：单段耗时未超预算' -Ok $true `
            -Details @(('全部段落都在 {0:N0} ms 预算内（抓图阶段由 run-tab-pixels.ps1 的硬超时兜底）' -f $script:sectionBudgetMs))
    }
}
else
{
    Trace-Step '快照缺失/不可读：E-11 未执行（已在 checks 里如实记录）'
}

$code = Show-HarnessChecks -Title '断言 E-11：页签像素幽灵（抓图逐像素比对）' -ChecksJson $ChecksJson
exit $code
