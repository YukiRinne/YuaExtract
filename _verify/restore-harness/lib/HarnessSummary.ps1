# YuaExtract 验证脚手架：run-all.ps1 的"汇总 + 假绿护栏"
#
# 为什么单独抽出来：**假绿**是这套脚手架最危险的失效模式，必须能被独立自检
# （见 self-test-runall-guard.ps1）。踩过的坑：
#   某一步在写出 checks-*.json 之前就失败了（例如 lib 编译不过 → 所有 UI 用例全挂），
#   汇总把上一轮的 checks 文件统统当成"残留"跳过，然后打印
#       "通过 0 / 失败 0 / 结论：全部通过"
#   —— 看起来一片绿，实际上这一轮一个断言都没跑。
#
# 三条护栏（任何一条成立都非 0 退出）：
#   ① 一个"本轮新写"的断言组都没有            → ExitCode 3（verdict = no-fresh）
#   ② 本轮实际执行到的断言项数低于下限        → ExitCode 3（verdict = below-min）
#   ③ 有失败项                                → ExitCode 1（verdict = fail）
#
# 依赖：Read-JsonFile / Get-ObjectValue（lib\HarnessUtil.ps1）。
#
# 记账口径（S-4）：`总计：通过 N` 是**逐 check** 数的，里面既有"产品断言"也有"脚手架自检"
# （`checks-selftest*.json` 那些组是用来证明断言有鉴别力的）。两行分开打印，别把它读成"产品过了 N 条"。

function Get-HarnessMinExecutedChecks
{
    param([string]$Scope = 'all')
    # 下限 = "这一步该跑到的断言项数量级"。取值比实际略低（留一点余量），
    # 目的是抓"整段没跑"，不是卡死正常波动。
    #
    # 2026-10-15（E-12..E-19 轮）上调：扩展集组加了 12 项断言（E-12 骨架 + E-12-a/C/E/F/T
    # + E-13..E-19）与 26 个自检场景。实测 -Scope extension ≈ 89 项、-Scope all ≈ 124 项，
    # 所以下限取 70 / 100 —— 仍然留了 ~20% 余量，但"整段没跑"（比如 lib 编译不过、
    # 所有 UI 用例在写 checks 之前就挂了）一定低于下限。
    switch ($Scope)
    {
        'all' { return 100 }
        'extension' { return 70 }
        'restore' { return 25 }
        'collect' { return 8 }
        'supplement' { return 15 }
        'pressure' { return 15 }
        default { return 1 }
    }
}

function Invoke-HarnessCheckSummary
{
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$CheckFiles,
        [Parameter(Mandatory)][datetime]$RunStarted,
        [string]$Scope = 'all',
        [int]$MinExecutedChecks = 0,
        # true = 不打印明细（自检用）
        [switch]$Quiet
    )

    $totalFail = 0
    $totalWarn = 0
    $totalPass = 0
    $freshGroups = 0
    $staleGroups = 0
    $missingGroups = 0
    # S-4（2026-10-16 独立验证记账）：`总计：通过 141` 里有 63 项是**脚手架自检**（自证断言有鉴别力），
    # 产品侧只有 78 项。混在一起容易被读成"产品过了 141 条"，所以分开数、分开打印。
    # 判据是文件名：`checks-selftest*.json` = 自检，其余 = 产品断言。
    $productPass = 0
    $productFail = 0
    $productWarn = 0
    $selfPass = 0
    $selfFail = 0
    $selfWarn = 0

    foreach ($path in $CheckFiles)
    {
        $group = Read-JsonFile -Path $path
        if (-not $group)
        {
            if (-not $Quiet) { Write-Host ("  （没有 {0}）" -f (Split-Path -Leaf $path)) }
            $missingGroups++
            continue
        }
        # 上一次运行留下的结果要标出来：混在一起看会把旧结论当成本次的（含旧指纹的结论）
        $stale = $false
        if (Test-Path -LiteralPath $path)
        {
            $written = (Get-Item -LiteralPath $path).LastWriteTime
            if ($written -lt $RunStarted) { $stale = $true }
        }
        if ($stale)
        {
            if (-not $Quiet) { Write-Host ("  （跳过 {0}：是上一次运行留下的，不是本次结果）" -f (Split-Path -Leaf $path)) }
            $staleGroups++
            continue
        }
        $freshGroups++
        if (-not $Quiet) { Write-Host '' }

        # 字段用 Get-ObjectValue 兜底：不同脚本写出来的 checks json 字段可能不全，
        # 少一个 warned 就把整个汇总打断，不值得（踩过）。
        $groupTitle = [string](Get-ObjectValue -Object $group -Name 'title' -Default (Split-Path -Leaf $path))
        $groupFailed = [int](Get-ObjectValue -Object $group -Name 'failed' -Default 0)
        $groupWarned = [int](Get-ObjectValue -Object $group -Name 'warned' -Default 0)
        $groupTotal = [int](Get-ObjectValue -Object $group -Name 'total' -Default 0)
        if (-not $Quiet) { Write-Host ("{0}   [失败 {1} / 提示 {2} / 共 {3}]" -f $groupTitle, $groupFailed, $groupWarned, $groupTotal) }
        $isSelfTestGroup = ((Split-Path -Leaf $path) -like 'checks-selftest*')
        foreach ($check in $group.checks)
        {
            $tag = if ($check.Ok) { 'PASS' } elseif ($check.Severity -eq 'fail') { 'FAIL' } else { 'WARN' }
            if ($check.Ok) { $totalPass++ } elseif ($check.Severity -eq 'fail') { $totalFail++ } else { $totalWarn++ }
            if ($isSelfTestGroup)
            {
                if ($check.Ok) { $selfPass++ } elseif ($check.Severity -eq 'fail') { $selfFail++ } else { $selfWarn++ }
            }
            else
            {
                if ($check.Ok) { $productPass++ } elseif ($check.Severity -eq 'fail') { $productFail++ } else { $productWarn++ }
            }
            if (-not $Quiet) { Write-Host ("    {0}  {1}  {2}" -f $tag, $check.Id, $check.Title) }
            if (-not $check.Ok -and -not $Quiet)
            {
                foreach ($detail in @($check.Details | Select-Object -First 6)) { Write-Host ("          {0}" -f $detail) }
            }
        }
    }

    $executed = $totalPass + $totalFail + $totalWarn
    $minExecuted = $MinExecutedChecks
    if ($minExecuted -le 0) { $minExecuted = Get-HarnessMinExecutedChecks -Scope $Scope }

    $verdict = 'pass'
    $exitCode = 0
    if ($freshGroups -eq 0)
    {
        $verdict = 'no-fresh'
        $exitCode = 3
    }
    elseif ($executed -lt $minExecuted)
    {
        $verdict = 'below-min'
        $exitCode = 3
    }
    elseif ($totalFail -gt 0)
    {
        $verdict = 'fail'
        $exitCode = 1
    }

    if (-not $Quiet)
    {
        Write-Host ''
        Write-Host ("总计：通过 {0} / 失败 {1} / 提示 {2}" -f $totalPass, $totalFail, $totalWarn)
        Write-Host ("      其中：产品断言 通过 {0} / 失败 {1}；脚手架自检 通过 {2} / 失败 {3}" -f `
                $productPass, $productFail, $selfPass, $selfFail)
        Write-Host ("本轮新写的断言组 {0} 个；被判为'上一次残留'而跳过的 {1} 个；文件不存在 {2} 个" -f `
                $freshGroups, $staleGroups, $missingGroups)

        if ($verdict -eq 'no-fresh')
        {
            Write-Host ''
            Write-Host '结论：不通过 —— 本轮**没有任何**新写的断言结果（checks-*.json 全是上一次的残留）。'
            Write-Host '      拒绝打印"全部通过"：这一次运行什么都没验证。'
            Write-Host '      常见原因：前置条件没满足（没编译 / lib 编译不过）导致所有用例在写结果前就失败了。'
        }
        elseif ($verdict -eq 'below-min')
        {
            Write-Host ''
            Write-Host ('结论：不通过 —— 本轮实际执行到 {0} 项断言，低于下限 {1}（-Scope {2}）。' -f $executed, $minExecuted, $Scope)
            Write-Host '      低于下限说明有步骤没跑到/没写出结果，不能当"通过"。'
            Write-Host '      （下限会随断言总数增长而上调；-MinExecutedChecks 可显式覆盖。）'
        }
        elseif ($verdict -eq 'fail')
        {
            Write-Host '结论：存在失败项，明细见上面各断言小节与 Run\out\*.json。'
        }
        else
        {
            Write-Host '结论：全部通过。'
        }
    }

    return [pscustomobject]@{
        ExitCode      = $exitCode
        Verdict       = $verdict
        Pass          = $totalPass
        Fail          = $totalFail
        Warn          = $totalWarn
        ProductPass   = $productPass
        ProductFail   = $productFail
        SelfTestPass  = $selfPass
        SelfTestFail  = $selfFail
        Executed      = $executed
        MinExecuted   = $minExecuted
        FreshGroups   = $freshGroups
        StaleGroups   = $staleGroups
        MissingGroups = $missingGroups
        Scope         = $Scope
    }
}
