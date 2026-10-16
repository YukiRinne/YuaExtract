# S-1 自证：证明 E-4 的「盐不匹配」用例不再假通过。
#
#   pwsh -File self-proof-s1-salt.ps1
#
# 背景（task-4 的 S-1）：旧写法里 `run-extension-ui.ps1` 不看 `-Game`、无条件把**主夹具的盐**
# 写进共享的 `<工具目录>\Log\Universal.log`。于是"别的游戏"（OtherGame）的采集日志被用
# `xp3hnp` 重算 → 名字层全进例外 → 那份档案 `DIRS 0 / FILES 0`、`salt=xp3hnp`（与当前游戏同盐）
# → 模式1 命中的是**别的防线**（空名字 / 归属摘要不符），而断言的关键词又命中了页面套话 → **假通过**。
#
# 本脚本两半都要过：
#   ① 故意改回旧写法（`-ToolSaltOverride <当前游戏盐>`）→ **E-4 必须炸**（前置复核报"同盐"）；
#   ② 改回新写法（按 `-Game` 取盐）→ E-4 必须通过。
#
# 依赖：先跑过一次 `run-all.ps1 -Scope extension`（需要有 `Run\out\extension-*.json` 与
# `Run\out\extension-bad-archives-summary.json` 作为其余用例的输入）。

[CmdletBinding()]
param(
    [string]$ManifestPath = '',
    [switch]$KeepForeignInstances
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')

$runRoot = Join-Path $root 'Run'
$outRoot = Join-Path $runRoot 'out'
if (-not $ManifestPath) { $ManifestPath = Join-Path $runRoot 'extension-manifest.json' }

$fixture = Read-JsonFile -Path $ManifestPath
if (-not $fixture) { throw ("缺少扩展集夹具清单：{0}；请先跑 make-extension-fixture.ps1" -f $ManifestPath) }
$summaryPath = Join-Path $outRoot 'extension-bad-archives-summary.json'
$summary = Read-JsonFile -Path $summaryPath
if (-not $summary) { throw ("缺少坏档案用例清单：{0}；请先跑 run-all.ps1 -Scope extension" -f $summaryPath) }

$gameSalt = [string]$fixture.salt
$otherSalt = [string]$fixture.otherGame.salt
$libraryArchive = [string]$summary.libraryArchive
$otherDraft = Join-Path $fixture.otherGame.workspace 'ExtensionDraft\rules.int'
$uiScript = Join-Path $root 'run-extension-ui.ps1'
$assertScript = Join-Path $root 'assert-extension.ps1'

$commonArgs = @(
    '-DraftRunJson', (Join-Path $outRoot 'extension-old.json'),
    '-RawRunJson', (Join-Path $outRoot 'extension-raw.json'),
    '-RepeatDraftRunJson', (Join-Path $outRoot 'extension-old-repeat.json'),
    '-RepeatInstallRunJson', (Join-Path $outRoot 'extension-old-install2.json'),
    '-Mode1RunJson', (Join-Path $outRoot 'extension-mode1.json'),
    '-ClassicRunJson', (Join-Path $outRoot 'extension-classic.json'),
    '-BadArchiveSummaryJson', $summaryPath
)

function Invoke-Assertion([string]$BadCsv, [string]$ChecksJson)
{
    $assertArgs = @('-NoProfile', '-File', $assertScript, '-ManifestPath', $ManifestPath, '-BadArchiveRunJsonCsv', $BadCsv, '-ChecksJson', $ChecksJson) + $commonArgs
    & pwsh @assertArgs | Out-Null
    return $LASTEXITCODE
}

function Get-CheckResult([string]$ChecksJson, [string]$Id)
{
    $result = Read-JsonFile -Path $ChecksJson
    if (-not $result) { return $null }
    return ($result.checks | Where-Object { $_.Id -eq $Id })
}

$failures = New-Object System.Collections.ArrayList
Write-Host ''
Write-Host '=== S-1 自证：旧写法必须炸 / 新写法必须过 ==='

# ---------- ① 旧写法 ----------
Write-Host ''
Write-Host ("[①] 故意改回旧写法：-Game other 也写主夹具的盐（{0}）" -f $gameSalt)
& pwsh -NoProfile -File $uiScript -Step meta,draft -Game other -ToolSaltOverride $gameSalt `
    -OutJson (Join-Path $outRoot 's1-selfproof-oldstyle-draft.json') -StartupTimeoutSec 30 | Out-Null
if ($LASTEXITCODE -ne 0) { throw '旧写法：生成 OtherGame 草案失败（先确认产品与夹具可用）' }
$oldDraft = Read-JsonFile -Path (Join-Path $outRoot 's1-selfproof-oldstyle-draft.json')
$oldSalt = [string]$oldDraft.archiveDump.parsed.salt
$oldDirs = @($oldDraft.archiveDump.parsed.dirs).Count
$oldFiles = @($oldDraft.archiveDump.parsed.files).Count
Write-Host ("      旧写法档案：salt='{0}' DIRS={1} FILES={2}（这正是 S-1 的现场：与当前游戏同盐、且没有名字）" -f $oldSalt, $oldDirs, $oldFiles)

Copy-Item -LiteralPath $oldDraft.draftArchive -Destination $libraryArchive -Force
& pwsh -NoProfile -File $uiScript -Step step1 -Game old -OutJson (Join-Path $outRoot 's1-selfproof-oldstyle-step1.json') `
    -TimeoutSec 180 -StartupTimeoutSec 30 | Out-Null
Write-Host ("      步骤1 退出码 {0}（2 = 被拒）" -f $LASTEXITCODE)

$oldCsv = (([string]$summary.badArchiveRunJsonCsv -split ';' | ForEach-Object {
            if ($_ -like 'salt-mismatch=*') { 'salt-mismatch=' + (Join-Path $outRoot 's1-selfproof-oldstyle-step1.json') } else { $_ }
        }) -join ';')
$oldChecks = Join-Path $outRoot 's1-selfproof-oldstyle-checks.json'
$oldExit = Invoke-Assertion -BadCsv $oldCsv -ChecksJson $oldChecks
$oldE4 = Get-CheckResult -ChecksJson $oldChecks -Id 'E-4'
if ($oldExit -ne 0 -and $oldE4 -and -not $oldE4.Ok)
{
    $reason = @($oldE4.Details | Where-Object { $_ -match '前置=False' }) -join ' '
    Write-Host ("      [OK] 断言炸了：E-4 = False；前置复核：{0}" -f $reason)
}
else
{
    [void]$failures.Add('① 旧写法下 E-4 竟然还是绿的 —— S-1 没修好（假通过又回来了）')
    Write-Host '      [FAIL] 旧写法下 E-4 仍然是绿的'
}

# ---------- ② 新写法 ----------
Write-Host ''
Write-Host ("[②] 改回新写法：-Game other 写它自己的盐（{0}）" -f $otherSalt)
& pwsh -NoProfile -File $uiScript -Step meta,draft -Game other -OutJson (Join-Path $outRoot 's1-selfproof-newstyle-draft.json') -StartupTimeoutSec 30 | Out-Null
if ($LASTEXITCODE -ne 0) { throw '新写法：生成 OtherGame 草案失败' }
$newDraft = Read-JsonFile -Path (Join-Path $outRoot 's1-selfproof-newstyle-draft.json')
$newSalt = [string]$newDraft.archiveDump.parsed.salt
$newDirs = @($newDraft.archiveDump.parsed.dirs).Count
$newFiles = @($newDraft.archiveDump.parsed.files).Count
Write-Host ("      新写法档案：salt='{0}' DIRS={1} FILES={2}" -f $newSalt, $newDirs, $newFiles)
if ($newSalt -eq $gameSalt) { [void]$failures.Add('② 新写法下 OtherGame 的档案仍然是当前游戏的盐 —— 按 -Game 取盐没生效') }
if (($newDirs + $newFiles) -eq 0) { [void]$failures.Add('② 新写法下 OtherGame 的档案仍然没有名字 —— 盐不对') }

# 恢复现场：库里那份现在是"旧写法"的坏档案，必须先用「安装到扩展集库」把**好档案**（OldGame 草案）
# 装回去，再重跑坏档案编排器（它会把好档案备份成 good.rules.int 再逐份变异）。
& pwsh -NoProfile -File $uiScript -Step install -Game old -OutJson (Join-Path $outRoot 's1-selfproof-reinstall.json') -StartupTimeoutSec 30 | Out-Null
if ($LASTEXITCODE -ne 0) { throw '恢复现场：重新安装好档案失败' }
& pwsh -NoProfile -File (Join-Path $root 'run-extension-bad-archives.ps1') -InstallRunJson (Join-Path $outRoot 'extension-old.json') | Out-Null
$newSummary = Read-JsonFile -Path $summaryPath
$newCsv = if ($newSummary) { [string]$newSummary.badArchiveRunJsonCsv } else { [string]$summary.badArchiveRunJsonCsv }
$newChecks = Join-Path $outRoot 's1-selfproof-newstyle-checks.json'
$newExit = Invoke-Assertion -BadCsv $newCsv -ChecksJson $newChecks
$newE4 = Get-CheckResult -ChecksJson $newChecks -Id 'E-4'
if ($newExit -eq 0 -and $newE4 -and $newE4.Ok)
{
    $line = @($newE4.Details | Where-Object { $_ -match '盐/归属不匹配' }) -join ' '
    Write-Host ("      [OK] 断言通过：E-4 = True；{0}" -f $line)
}
else
{
    [void]$failures.Add('② 新写法下 E-4 没通过 —— 断言或夹具还有问题')
    Write-Host '      [FAIL] 新写法下 E-4 没通过'
}

Write-Host ''
if ($failures.Count -eq 0)
{
    Write-Host '结论：S-1 自证通过（旧写法必炸 / 新写法必过）'
    Write-Host ("证据：{0} / {1} / {2} / {3}" -f $oldChecks, $newChecks, (Join-Path $outRoot 's1-selfproof-oldstyle-step1.json'), (Join-Path $outRoot 's1-selfproof-newstyle-draft.json'))
    exit 0
}
Write-Host '结论：S-1 自证不通过'
$failures | ForEach-Object { Write-Host ('  - ' + $_) }
exit 1
