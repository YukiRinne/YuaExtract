# 自检：证明 run-all.ps1 的「假绿护栏」真的会炸（lead 要求：护栏必须能自证）
#
#   pwsh -File self-test-runall-guard.ps1
#   pwsh -File self-test-runall-guard.ps1 -ChecksJson Run\out\checks-selftest-runall-guard.json
#
# 背景（踩过的坑）：某一步在写出 checks-*.json 之前就失败了（例如共享 lib 编译不过，
# 所有 UI 用例全挂），旧版汇总会把上一轮的 checks 文件统统当成"残留"跳过，然后打印
#     "总计：通过 0 / 失败 0 / 提示 0"
#     "结论：全部通过。"
# —— 一次运行什么都没验证，却是一片绿。这个自检就是钉死它：
#   造出"全是残留 / 项数不够 / 有失败"三种局面，要求护栏**该炸就炸**，
#   并且要求"全是残留"那一次**不许**出现"全部通过"字样。
#
# 判据全部走 lib\HarnessSummary.ps1 的同一个函数（run-all.ps1 调的就是它），
# 所以这里验的是真代码路径，不是复刻一遍逻辑。

[CmdletBinding()]
param(
    [string]$ChecksJson = '',
    [switch]$KeepWork
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')
. (Join-Path $root 'lib\HarnessSummary.ps1')

$runRoot = Join-Path $root 'Run'
$outRoot = Join-Path $runRoot 'out'
$workRoot = Join-Path $runRoot 'selftest-runall-guard'
if (-not $ChecksJson) { $ChecksJson = Join-Path $outRoot 'checks-selftest-runall-guard.json' }
if (-not (Test-Path -LiteralPath $workRoot)) { New-Item -ItemType Directory -Path $workRoot -Force | Out-Null }

Reset-HarnessChecks
Write-Host ''
Write-Host '=== 假绿护栏自检（run-all.ps1 的汇总逻辑）==='

function New-CheckGroup
{
    param([string]$Path, [int]$Pass, [int]$Fail = 0, [string]$Title = '合成断言组')

    $checks = New-Object System.Collections.ArrayList
    for ($index = 1; $index -le $Pass; $index++)
    {
        [void]$checks.Add([ordered]@{ Id = ('P-{0}' -f $index); Title = '合成通过项'; Ok = $true; Severity = 'fail'; Details = @() })
    }
    for ($index = 1; $index -le $Fail; $index++)
    {
        [void]$checks.Add([ordered]@{ Id = ('F-{0}' -f $index); Title = '合成失败项'; Ok = $false; Severity = 'fail'; Details = @('故意做错，用来验证护栏') })
    }
    Write-JsonFile -Path $Path -Object ([ordered]@{
            title = $Title; failed = $Fail; warned = 0; total = $checks.Count; checks = $checks
        })
    return $Path
}

# 跑一次汇总，把"打印出来的文字"和"返回的结论对象"一起拿回来。
# Write-Host 在 PS 7 上走信息流(6)，这里 6>&1 抓成 InformationRecord 再拼回文本。
function Invoke-SummaryCaptured
{
    param([string[]]$CheckFiles, [datetime]$RunStarted, [string]$Scope = 'all', [int]$MinExecutedChecks = 0)

    $records = @(Invoke-HarnessCheckSummary -CheckFiles $CheckFiles -RunStarted $RunStarted -Scope $Scope `
            -MinExecutedChecks $MinExecutedChecks 6>&1)
    $summary = @($records | Where-Object { $_ -isnot [System.Management.Automation.InformationRecord] })
    $text = (@($records | Where-Object { $_ -is [System.Management.Automation.InformationRecord] }) |
        ForEach-Object { $_.MessageData.ToString() }) -join "`n"
    return [pscustomobject]@{ Summary = $summary[-1]; Text = $text }
}

$cases = New-Object System.Collections.ArrayList

# ---------- 场景 1：本轮新写、项数够、无失败 → 必须 0 ----------
$fresh1 = New-CheckGroup -Path (Join-Path $workRoot 'fresh-a.json') -Pass 60 -Title '合成：A 组'
$fresh2 = New-CheckGroup -Path (Join-Path $workRoot 'fresh-b.json') -Pass 55 -Title '合成：B 组'
$runStarted = (Get-Date).AddMinutes(-1)
$result = Invoke-SummaryCaptured -CheckFiles @($fresh1, $fresh2) -RunStarted $runStarted -Scope 'all'
[void]$cases.Add([pscustomobject]@{
        Name = 'fresh-enough-pass'; ExpectCode = 0; ExpectVerdict = 'pass'
        ActualCode = $result.Summary.ExitCode; ActualVerdict = $result.Summary.Verdict
        Text = $result.Text; Summary = $result.Summary
        ExpectText = @('结论：全部通过。', '其中：产品断言'); ForbidText = @('不通过')
    })

# ---------- 场景 2：**全是上一轮的残留** → 必须 3，且不许出现"全部通过" ----------
$staleA = Join-Path $workRoot 'stale-a.json'
$staleB = Join-Path $workRoot 'stale-b.json'
[void](New-CheckGroup -Path $staleA -Pass 60 -Title '合成：陈旧的 A 组')
[void](New-CheckGroup -Path $staleB -Pass 55 -Title '合成：陈旧的 B 组')
foreach ($file in @($staleA, $staleB)) { (Get-Item -LiteralPath $file).LastWriteTime = (Get-Date).AddHours(-3) }
$runStarted = Get-Date
$result = Invoke-SummaryCaptured -CheckFiles @($staleA, $staleB) -RunStarted $runStarted -Scope 'all'
[void]$cases.Add([pscustomobject]@{
        Name = 'stale-only-no-fresh'; ExpectCode = 3; ExpectVerdict = 'no-fresh'
        ActualCode = $result.Summary.ExitCode; ActualVerdict = $result.Summary.Verdict
        Text = $result.Text; Summary = $result.Summary
        ExpectText = @('不通过', '没有任何'); ForbidText = @('结论：全部通过')
    })

# ---------- 场景 3：本轮新写但项数不够（下限 80）→ 必须 3 ----------
$small = New-CheckGroup -Path (Join-Path $workRoot 'small.json') -Pass 10 -Title '合成：小结果'
$runStarted = (Get-Date).AddMinutes(-1)
$result = Invoke-SummaryCaptured -CheckFiles @($small) -RunStarted $runStarted -Scope 'all'
[void]$cases.Add([pscustomobject]@{
        Name = 'fresh-below-min'; ExpectCode = 3; ExpectVerdict = 'below-min'
        ActualCode = $result.Summary.ExitCode; ActualVerdict = $result.Summary.Verdict
        Text = $result.Text; Summary = $result.Summary
        ExpectText = @('不通过', '低于下限'); ForbidText = @('结论：全部通过')
    })

# ---------- 场景 4：项数够但有失败 → 必须 1（原有行为不能被护栏改坏）----------
$withFail = New-CheckGroup -Path (Join-Path $workRoot 'with-fail.json') -Pass 105 -Fail 2 -Title '合成：有失败'
$runStarted = (Get-Date).AddMinutes(-1)
$result = Invoke-SummaryCaptured -CheckFiles @($withFail) -RunStarted $runStarted -Scope 'all'
[void]$cases.Add([pscustomobject]@{
        Name = 'has-failure'; ExpectCode = 1; ExpectVerdict = 'fail'
        ActualCode = $result.Summary.ExitCode; ActualVerdict = $result.Summary.Verdict
        Text = $result.Text; Summary = $result.Summary
        ExpectText = @('存在失败项'); ForbidText = @('结论：全部通过')
    })

# ---------- 场景 5：下限可显式覆盖（证明护栏不是"永远失败"）----------
$runStarted = (Get-Date).AddMinutes(-1)
$result = Invoke-SummaryCaptured -CheckFiles @($small) -RunStarted $runStarted -Scope 'all' -MinExecutedChecks 5
[void]$cases.Add([pscustomobject]@{
        Name = 'min-override'; ExpectCode = 0; ExpectVerdict = 'pass'
        ActualCode = $result.Summary.ExitCode; ActualVerdict = $result.Summary.Verdict
        Text = $result.Text; Summary = $result.Summary
        ExpectText = @('结论：全部通过。'); ForbidText = @('不通过')
    })

# ---------- 场景 6：本轮**新写的组数**口径 —— 多组新结果照样要被数出来 ----------
# （E-12..E-19 轮之后 checks 组从 9 个变成 9 个仍在跑的；这里验证"组数"这一栏没退化：
#  三组新结果必须报 freshGroups=3，而不是把其中的合成组当成"没有新结果"。）
$fresh3 = New-CheckGroup -Path (Join-Path $workRoot 'fresh-c.json') -Pass 45 -Title '合成：C 组'
$runStarted = (Get-Date).AddMinutes(-1)
$result = Invoke-SummaryCaptured -CheckFiles @($fresh1, $fresh2, $fresh3) -RunStarted $runStarted -Scope 'all' -MinExecutedChecks 5
[void]$cases.Add([pscustomobject]@{
        Name = 'fresh-group-count'; ExpectCode = 0; ExpectVerdict = 'pass'
        ActualCode = $result.Summary.ExitCode; ActualVerdict = $result.Summary.Verdict
        Text = $result.Text; Summary = $result.Summary
        ExpectText = @('本轮新写的断言组 3 个'); ForbidText = @('不通过')
    })

# ---------- 场景 7：扩展集组的下限（E-12..E-19 轮里上调到 70）----------
$medium = New-CheckGroup -Path (Join-Path $workRoot 'medium.json') -Pass 60 -Title '合成：扩展集组量级'
$runStarted = (Get-Date).AddMinutes(-1)
$result = Invoke-SummaryCaptured -CheckFiles @($medium) -RunStarted $runStarted -Scope 'extension'
[void]$cases.Add([pscustomobject]@{
        Name = 'extension-below-min'; ExpectCode = 3; ExpectVerdict = 'below-min'
        ActualCode = $result.Summary.ExitCode; ActualVerdict = $result.Summary.Verdict
        Text = $result.Text; Summary = $result.Summary
        ExpectText = @('不通过', '低于下限', '70'); ForbidText = @('结论：全部通过')
    })

# ---------- 判定 ----------
foreach ($case in $cases)
{
    $problems = New-Object System.Collections.ArrayList
    if ($case.ActualCode -ne $case.ExpectCode) { [void]$problems.Add(('退出码 {0}，期望 {1}' -f $case.ActualCode, $case.ExpectCode)) }
    if ($case.ActualVerdict -ne $case.ExpectVerdict) { [void]$problems.Add(('结论 {0}，期望 {1}' -f $case.ActualVerdict, $case.ExpectVerdict)) }
    foreach ($needle in $case.ExpectText)
    {
        if ($case.Text -notlike ('*' + $needle + '*')) { [void]$problems.Add(('输出里缺少"{0}"' -f $needle)) }
    }
    foreach ($needle in $case.ForbidText)
    {
        if ($case.Text -like ('*' + $needle + '*')) { [void]$problems.Add(('输出里**不该**出现"{0}"' -f $needle)) }
    }
    $ok = ($problems.Count -eq 0)
    $details = @(('退出码 {0}（期望 {1}）/ 结论 {2}（期望 {3}）/ 通过 {4} 失败 {5} 提示 {6} / 新写组 {7} 跳过组 {8}' -f `
                $case.ActualCode, $case.ExpectCode, $case.ActualVerdict, $case.ExpectVerdict,
                $case.Summary.Pass, $case.Summary.Fail, $case.Summary.Warn, $case.Summary.FreshGroups, $case.Summary.StaleGroups))
    $details += $problems
    Add-Check -Id ('SELFTEST-GUARD/' + $case.Name) -Title '假绿护栏：该炸的炸、该过的过' -Ok $ok -Details $details
    Write-Host ("  {0,-24} {1}  退出码={2} 结论={3}" -f $case.Name, $(if ($ok) { 'OK  ' } else { 'BAD ' }), $case.ActualCode, $case.ActualVerdict)
    if (-not $ok) { foreach ($problem in $problems) { Write-Host ('      · ' + $problem) } }
}

if (-not $KeepWork)
{
    foreach ($file in @($fresh1, $fresh2, $fresh3, $staleA, $staleB, $small, $medium, $withFail))
    {
        if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue }
    }
}

exit (Show-HarnessChecks -Title '自检：run-all 假绿护栏' -ChecksJson $ChecksJson)
