# E-4 编排器：把坏档案（或别的游戏的档案 / 空名字档案）装进扩展集库，逐个驱动「模式1 步骤1」，
# 期望产品**明确报错并拒绝**（不产出 StaticHash.lst）。
#
#   pwsh -File run-extension-bad-archives.ps1
#   pwsh -File run-extension-bad-archives.ps1 -InstallRunJson Run\out\extension-old.json
#
# 前置（由 run-all.ps1 -Scope extension 串起来）：
#   1) run-extension-ui.ps1 -Step meta,draft,install -Game old      -> 库里有一份好档案
#   2) run-extension-ui.ps1 -Step draft -Game other                 -> 另一个盐的草案档案
#   3) run-extension-ui.ps1 -Step draft -Game empty                 -> 空名字的草案档案
#
# 产出：
#   Run\ExtensionFixture\BadArchives\*.rules.int   坏档案本体（由 lib\rules-int.py mutate 造）
#   Run\out\extension-bad-<case>.json              每个用例的运行结果
#   Run\out\extension-bad-archives-summary.json    用例清单（含传给断言的 CSV）
#
# 收尾保证：无论成功失败，**库里那份档案一定会被还原成好档案**（try/finally）。

[CmdletBinding()]
param(
    [string]$InstallRunJson = '',
    [string]$ManifestPath = '',
    [string]$AppExe = '',
    [int]$Step1TimeoutSec = 240,
    [switch]$SkipSnapshotSync,
    [switch]$KeepForeignInstances
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')
. (Join-Path $root 'lib\HarnessNative.ps1')

$runRoot = Join-Path $root 'Run'
$outRoot = Join-Path $runRoot 'out'
if (-not $InstallRunJson) { $InstallRunJson = Join-Path $outRoot 'extension-old.json' }
if (-not $ManifestPath) { $ManifestPath = Join-Path $runRoot 'extension-manifest.json' }

$fixture = Read-JsonFile -Path $ManifestPath
if (-not $fixture) { throw ("缺少扩展集夹具清单：{0}；请先运行 make-extension-fixture.ps1" -f $ManifestPath) }

$installRun = Read-JsonFile -Path $InstallRunJson
if (-not $installRun) { throw ("读不到安装用例结果：{0}；请先跑 run-extension-ui.ps1 -Step meta,draft,install -Game old" -f $InstallRunJson) }
$libraryArchive = [string](Get-ObjectValue -Object $installRun -Name 'libraryArchive' -Default '')
if (-not $libraryArchive -or -not (Test-Path -LiteralPath $libraryArchive))
{
    throw ("安装用例结果里没有可用的库档案路径（libraryArchive='{0}'）；先确认「安装到扩展集库」跑通了" -f $libraryArchive)
}
Write-Host ("[bad-archives] 库档案：{0}" -f $libraryArchive)

$python = $null
foreach ($candidate in @('C:\Users\Administrator\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'))
{
    if (Test-Path -LiteralPath $candidate) { $python = $candidate; break }
}
if (-not $python)
{
    $found = Get-Command python -ErrorAction SilentlyContinue
    if ($found) { $python = $found.Source }
}
if (-not $python) { throw '找不到 Python 运行时（造坏档案需要它）' }

$badRoot = Join-Path $fixture.fixtureRoot 'BadArchives'
if (-not (Test-Path -LiteralPath $badRoot)) { New-Item -ItemType Directory -Path $badRoot -Force | Out-Null }

# ---------- 备份好档案 ----------
$goodBackup = Join-Path $badRoot 'good.rules.int'
Copy-Item -LiteralPath $libraryArchive -Destination $goodBackup -Force
$goodSha = Get-Sha256Hex -Path $goodBackup
Write-Host ("[bad-archives] 好档案已备份：{0}（sha256 {1}）" -f $goodBackup, $goodSha.Substring(0, 16))

# ---------- 造四份结构性坏档案（CRC / 截断 / 版本 / 缺 IDENTITY）----------
$mutateReport = Join-Path $badRoot 'mutate-report.json'
& $python (Join-Path $root 'lib\rules-int.py') mutate --in $goodBackup --outdir $badRoot --report $mutateReport
if ($LASTEXITCODE -ne 0) { throw ("rules-int.py mutate 失败（退出码 {0}）" -f $LASTEXITCODE) }

# ---------- 用例清单 ----------
# salt-mismatch / empty-names 用的是**真实生成出来的**草案档案（别的游戏 / 空名字），
# 因为它们坏在"语义"上，不是坏在字节上。
$cases = New-Object System.Collections.ArrayList
[void]$cases.Add([pscustomobject]@{
        Name = 'salt-mismatch'; Desc = '盐/归属不匹配（别的游戏的档案；步骤1 默认拿不到当前盐）'
        Archive = Join-Path $fixture.otherGame.workspace 'ExtensionDraft\rules.int'
        Keywords = @('归属', '无法校验')
        Expect = '明确拒绝：归属无法校验（摘要不符 + 没有可用的当前游戏盐）'
    })
# 让产品能读到"当前游戏的盐"，从而走 §2.1 真正的"盐不同"判定路径（而不是"无法校验归属"）：
# 目标游戏仍是 OldGame（盐 xp3hnp），库里换成 OtherGame 的档案（盐 othersalt123），
# 并给步骤1 提供工具目录的盐（-ProvideToolSalt）。
[void]$cases.Add([pscustomobject]@{
        Name = 'salt-mismatch-known'; Desc = '盐不匹配（当前游戏的盐可用 → 必须报"盐不同"）'
        Archive = Join-Path $fixture.otherGame.workspace 'ExtensionDraft\rules.int'
        Keywords = @('盐不同', '盐不一致', '盐不匹配')
        Expect = '明确拒绝：该扩展集属于 <会社>/<作品>，与当前游戏盐不同'
        ExtraArgs = @('-ProvideToolSalt')
    })
[void]$cases.Add([pscustomobject]@{
        Name = 'crc-error'; Desc = 'CRC 错'
        Archive = Join-Path $badRoot 'crc-error.rules.int'
        Keywords = @('CRC', '校验', '损坏')
        Expect = '明确拒绝：CRC 校验失败'
    })
[void]$cases.Add([pscustomobject]@{
        Name = 'truncated'; Desc = '截断'
        Archive = Join-Path $badRoot 'truncated.rules.int'
        Keywords = @('截断', '不完整', '尺寸', '长度', '结构', '损坏')
        Expect = '明确拒绝：档案不完整'
    })
[void]$cases.Add([pscustomobject]@{
        Name = 'version-high'; Desc = '版本过高'
        Archive = Join-Path $badRoot 'version-high.rules.int'
        Keywords = @('版本', 'version')
        Expect = '明确拒绝：版本不受支持'
    })
[void]$cases.Add([pscustomobject]@{
        Name = 'missing-identity'; Desc = '缺 IDENTITY'
        Archive = Join-Path $badRoot 'missing-identity.rules.int'
        Keywords = @('IDENTITY', '归属', '身份', '缺')
        Expect = '明确拒绝：没有归属信息'
    })
[void]$cases.Add([pscustomobject]@{
        Name = 'empty-names'; Desc = '空档案（结构合法但一个名字都没有）'
        Archive = Join-Path $badRoot 'empty-names.rules.int'
        Keywords = @('任何名字', '名字层为空')
        Expect = '明确拒绝并报错：不写空表（lead 同步的 task-1 事实 #5）'
    })

$summary = [ordered]@{
    startedUtc = (Get-Date).ToUniversalTime().ToString('o')
    libraryArchive = $libraryArchive
    goodBackup = $goodBackup
    goodSha256 = $goodSha
    cases = @()
    badArchiveRunJsonCsv = ''
}
$csvEntries = New-Object System.Collections.ArrayList

try
{
    foreach ($case in $cases)
    {
        Write-Host ''
        Write-Host ("------------------------------------------------------------------")
        Write-Host (" >> E-4 用例：{0}" -f $case.Desc)
        Write-Host ("------------------------------------------------------------------")
        $entry = [ordered]@{ name = $case.Name; desc = $case.Desc; archive = $case.Archive; expect = $case.Expect; ok = $false; reason = '' }
        if (-not (Test-Path -LiteralPath $case.Archive))
        {
            $entry.reason = ("坏档案不存在：{0}（先跑对应的 -Game draft 生成草案）" -f $case.Archive)
            Write-Host ("  [跳过] {0}" -f $entry.reason)
            $summary.cases += $entry
            continue
        }

        # 把库里那份换成坏档案（产品读的就是它）
        Copy-Item -LiteralPath $case.Archive -Destination $libraryArchive -Force
        Write-Host ("  已把库档案替换为：{0}" -f $case.Archive)

        $caseJson = Join-Path $outRoot ('extension-bad-{0}.json' -f $case.Name)
        $arguments = @(
            '-Step', 'step1', '-Game', 'old',
            '-OutJson', $caseJson,
            '-TimeoutSec', "$Step1TimeoutSec"
        )
        if ($case.PSObject.Properties['ExtraArgs'] -and $case.ExtraArgs) { $arguments += @($case.ExtraArgs) }
        if ($AppExe) { $arguments += @('-AppExe', $AppExe) }
        if ($SkipSnapshotSync) { $arguments += '-SkipSnapshotSync' }
        if ($KeepForeignInstances) { $arguments += '-KeepForeignInstances' }

        & pwsh -NoProfile -File (Join-Path $root 'run-extension-ui.ps1') @arguments
        $code = $LASTEXITCODE
        $entry.exitCode = $code
        $entry.runJson = $caseJson
        # 退出码 2 = 驱动按预期失败（步骤1 被拒 / 没产出 lst），这正是 E-4 想要的；
        # 退出码 0 = 产品居然接受了坏档案（断言会炸）。
        $entry.ok = ($code -eq 2)
        if ($code -eq 0) { $entry.reason = '产品接受了这份坏档案（模式1 步骤1 成功产出了静态 Hash 表）' }
        elseif ($code -ne 2) { $entry.reason = ("驱动退出码 {0}（非预期的失败，先看 {1}）" -f $code, $caseJson) }
        Write-Host ("  → 退出码 {0}；结果 {1}" -f $code, $caseJson)
        $summary.cases += $entry
        [void]$csvEntries.Add(('{0}={1}' -f $case.Name, $caseJson))
    }
}
finally
{
    # 收尾：库里那份必须还原成好档案，否则后面的用例（模式1 对照）会被坏档案污染
    if (Test-Path -LiteralPath $goodBackup)
    {
        Copy-Item -LiteralPath $goodBackup -Destination $libraryArchive -Force
        $restored = Get-Sha256Hex -Path $libraryArchive
        Write-Host ("[bad-archives] 已把库档案还原成好档案（sha256 {0}，与备份一致={1}）" -f $restored.Substring(0, 16), ($restored -eq $goodSha))
        $summary['libraryRestored'] = ($restored -eq $goodSha)
    }
    # 残留实例收尾（lead 硬约束）
    $others = @(Get-Process YuaExtract -ErrorAction SilentlyContinue)
    if ($others.Count -gt 0 -and -not $KeepForeignInstances)
    {
        foreach ($process in $others) { try { $process.Kill() } catch { } }
        Write-Host ("[bad-archives] 收尾：结束了 {0} 个残留 YuaExtract 实例" -f $others.Count)
    }
}

$summary.badArchiveRunJsonCsv = ($csvEntries -join ';')
$summary.finishedUtc = (Get-Date).ToUniversalTime().ToString('o')
$summaryPath = Join-Path $outRoot 'extension-bad-archives-summary.json'
Write-JsonFile -Path $summaryPath -Object $summary

Write-Host ''
Write-Host ("[bad-archives] 用例 {0} 个：{1}" -f $summary.cases.Count, (($summary.cases | ForEach-Object { '{0}={1}' -f $_.name, $(if ($_.ok) { '被拒' } else { '未按预期' }) }) -join ', '))
Write-Host ("[bad-archives] 汇总：{0}" -f $summaryPath)
Write-Host ("[bad-archives] 传给断言的 CSV：{0}" -f $summary.badArchiveRunJsonCsv)

# 退出码：2 = 有用例没按预期（断言会给出明细）；0 = 六个用例都被拒了
$failed = @($summary.cases | Where-Object { -not $_.ok }).Count
if ($failed -gt 0) { exit 2 }
exit 0
