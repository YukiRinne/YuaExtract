# 脚手架自检：不用被测程序，直接伪造 run-step3-restore.ps1 的结果 JSON，
# 验证「断言 A/B/D 本身」能不能按预期报出通过/不通过。
#
#   pwsh -File self-test-assertions.ps1
#
# 为什么需要它：断言脚本"全绿"不代表它有鉴别力 —— 一个永远返回通过的断言也是全绿。
# 这里给每个断言都喂一份**故意做错**的合成结果，要求它必须报 FAIL；
# 再喂一份做对的，要求它必须报 PASS。
#
# 覆盖的坏味道（前两条是 lead 点名要求锁住的）：
#   passthrough-dropped  明文名文件被一起丢掉（丢得太多）→ A-2 必须炸
#   hashdir-leak         只认出文件哈希就落盘，明文名躺在 Hash 目录下（丢得太少）→ A-3 必须炸
#   report-lies          报告里的"未还原被丢弃"计数与事实不符 → A-4 必须炸
#   source-touched       还原过程改了源目录 → D-1 必须炸
#   keep-drops-unrestored 勾了保留却没保留未还原文件 → B-2 必须炸
#   legacy-inline        步骤3 还是旧的内联还原（没弹新窗口）→ UI-1 必须炸

[CmdletBinding()]
param([string]$ChecksJson = '')

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')

$manifest = Read-JsonFile -Path (Join-Path $root 'Run\manifest.json')
if (-not $manifest)
{
    Write-Host '缺少 Run\manifest.json，请先运行 make-workspace.ps1'
    exit 2
}

Reset-HarnessChecks
$selfTestDir = Join-Path $root 'Run\selftest'
if (-not (Test-Path -LiteralPath $selfTestDir)) { New-Item -ItemType Directory -Path $selfTestDir -Force | Out-Null }

$sourceFiles = @($manifest.files)
$reportPath = Join-Path $manifest.defaultOutputDir 'RestoreReport.txt'

function Get-SourceItem([string]$Category)
{
    return @($sourceFiles | Where-Object { $_.category -eq $Category })
}

function New-ManifestCounts
{
    $restored = (Get-SourceItem 'restored').Count
    $dup = (Get-SourceItem 'restored-dup').Count
    $plain = (Get-SourceItem 'plain').Count
    $missingDir = @($sourceFiles | Where-Object { $_.category -in @('missing-dir', 'missing-both') }).Count
    $missingFile = @($sourceFiles | Where-Object { $_.category -in @('missing-file', 'missing-both') }).Count
    $dropped = @($sourceFiles | Where-Object { $_.category -like 'missing-*' }).Count
    $total = @($sourceFiles | Where-Object { $_.category -ne 'skipped-log' }).Count
    $skipped = @($sourceFiles | Where-Object { $_.category -eq 'skipped-log' }).Count
    return [pscustomobject]@{
        Restored = $restored; Dup = $dup; Plain = $plain; MissingDir = $missingDir
        MissingFile = $missingFile; Dropped = $dropped; Total = $total; Skipped = $skipped
    }
}
$counts = New-ManifestCounts

function New-ReportText([int]$DroppedOverride = -1)
{
    $dropped = if ($DroppedOverride -ge 0) { $DroppedOverride } else { $counts.Dropped }
    return @(
        'YuaExtract 资源名还原报告（脚手架自检用，不是真实产物）'
        '生成时间：2026-01-01 00:00:00'
        '解包目录：<selftest>'
        ''
        '—— 统计 ——'
        ("总文件数            {0}" -f $counts.Total)
        ("哈希名文件          {0}" -f ($counts.Restored + $counts.Dup + $counts.Dropped))
        ("  已还原            {0}" -f ($counts.Restored + $counts.Dup))
        ("  缺目录 Hash       {0}" -f $counts.MissingDir)
        ("  缺文件 Hash       {0}" -f $counts.MissingFile)
        ("非哈希名文件（原样） {0}" -f $counts.Plain)
        ("未还原被丢弃        {0}" -f $dropped)
        ("跳过日志文件        {0}" -f $counts.Skipped)
        ''
    ) -join "`r`n"
}

function New-BaseRecord([string]$Mode, [switch]$LegacyInline)
{
    $before = [ordered]@{}
    $after = [ordered]@{}
    foreach ($item in $sourceFiles)
    {
        if ($item.category -eq 'skipped-log') { continue }
        $stamp = '2026-01-01T00:00:00.0000000Z'
        $before[$item.sourceRel] = [ordered]@{ size = [long]$item.size; mtime = $stamp; sha256 = $item.sha256 }
        $after[$item.sourceRel] = [ordered]@{ size = [long]$item.size; mtime = $stamp; sha256 = $item.sha256 }
    }

    # 任务列表：每个封包一行 + 空封包一行，全部终态；标题统计与它对齐。
    $rows = @()
    foreach ($packageName in (@($manifest.packages) + @($manifest.emptyPackage)))
    {
        $rows += [pscustomobject]@{ List = '0x00000002'; Row = $rows.Count; Cells = ('{0} | 已完成 | 100% | （已结束）' -f $packageName) }
    }
    $finalTitle = ('YuaExtract 资源名还原 - 运行中 0 / 排队 0 / 完成 {0}' -f $rows.Count)

    return [ordered]@{
        case                 = 'selftest'
        mode                 = $Mode
        keepUnrestored       = ($Mode -eq 'keep')
        startedUtc           = '2026-01-01T00:00:00.0000000Z'
        finishedUtc          = '2026-01-01T00:01:00.0000000Z'
        appExe               = '<selftest>'
        workspace            = $manifest.gameDir
        failure              = $null
        uiMode               = $(if ($LegacyInline) { 'legacy-inline' } else { 'restore-window' })
        completed            = $true
        completionReason     = '（自检）标题显示 排队0/运行中0/完成3，且输出目录已稳定'
        outputDir            = $manifest.defaultOutputDir
        outputCandidates     = @()
        sourceSnapshotBefore = $before
        sourceSnapshotAfter  = $after
        output               = @()
        reportPath           = $reportPath
        reportText           = (New-ReportText)
        restoreWindow        = [ordered]@{ HwndHex = '0x00000001'; Class = 'CxdecRestoreWindow'; Title = $finalTitle }
        controls             = @()
        listRows             = $rows
        logText              = ''
        uiLogLines           = ($rows.Count * 3 + 6)
        uiLogChars           = ($rows.Count * 120)
        pollCount            = 40
        pollHungCount        = 0
        probeLatencyMaxMs    = 12
        probeLatencyAvgMs    = 3
        restoreElapsedMs     = 1500
        listSampleCount      = 3
        toolLogFiles         = @(
            [pscustomobject]@{ Name = 'YuaExtractRestore.log'; Size = 200; Lines = $rows.Count }
            [pscustomobject]@{ Name = 'RestoreDetail_bgimage.txt'; Size = 900; Lines = 9 }
        )
        outputTsv            = ''
        sourceDigestBefore   = [pscustomobject]@{ Count = $before.Count; Digest = 'DIGEST-A'; HashDigest = 'HASH-A' }
        sourceDigestAfter    = [pscustomobject]@{ Count = $after.Count; Digest = 'DIGEST-A'; HashDigest = 'HASH-A' }
        titleTimeline        = @('资源名还原 - 运行中 1 / 排队 2 / 完成 0', $finalTitle)
        modalDialogs         = @()
        events               = @()
    }
}

function Add-Output([object]$Record, [string]$Rel, [long]$Size, [string]$Sha256)
{
    $Record.output = @($Record.output) + @([ordered]@{ rel = $Rel; size = $Size; sha256 = $Sha256 })
}

function Add-OutputForCategory([object]$Record, [string]$Category, [string]$TargetStyle = 'expected')
{
    foreach ($item in @(Get-SourceItem $Category))
    {
        switch ($TargetStyle)
        {
            'expected'  { Add-Output $Record $item.expectedRel $item.size $item.sha256 }
            'source'    { Add-Output $Record $item.sourceRel $item.size $item.sha256 }
            'hashDirPlainName' {
                # 目录哈希没认出来、文件哈希认出来了：明文文件名躺在 Hash 目录下
                $parts = $item.sourceRel -split '\\'
                Add-Output $Record (($parts[0], $parts[1], $item.knownFile) -join '\') $item.size $item.sha256
            }
            default     { throw "未知的 TargetStyle：$TargetStyle" }
        }
    }
}

# 重名冲突的两个源文件在输出里的形态：一个真名 + 一个 __dup1
function Add-DupOutputs([object]$Record)
{
    $items = @(Get-SourceItem 'restored-dup')
    if ($items.Count -eq 0) { return }
    $first = $items[0]
    Add-Output $Record $first.expectedRel $first.size $first.sha256
    $index = 1
    foreach ($item in @($items | Select-Object -Skip 1))
    {
        $directory = Split-Path $item.expectedRel -Parent
        $stem = [System.IO.Path]::GetFileNameWithoutExtension($item.expectedRel)
        $extension = [System.IO.Path]::GetExtension($item.expectedRel)
        $rel = if ($directory) { Join-Path $directory ("{0}__dup{1}{2}" -f $stem, $index, $extension) } else { "{0}__dup{1}{2}" -f $stem, $index, $extension }
        Add-Output $Record $rel $item.size $item.sha256
        $index++
    }
}
function Add-ReportArtifact([object]$Record, [string]$Text)
{
    Add-Output $Record 'RestoreReport.txt' ([long]$Text.Length) 'F' * 0 + '0' * 64
    $Record.reportText = $Text
}

# ---------- 场景表 ----------

$scenarios = New-Object System.Collections.ArrayList

# 1) 做对了：默认模式只留"已还原 + 本来就是明文名"
$correct = New-BaseRecord 'default'
Add-OutputForCategory $correct 'restored' 'expected'
Add-DupOutputs $correct
Add-OutputForCategory $correct 'plain' 'expected'
Add-ReportArtifact $correct (New-ReportText)
[void]$scenarios.Add([pscustomobject]@{
        Name = 'correct-default'; Mode = 'default'; Record = $correct; Args = @()
        Expect = @{ 'RUN-1' = $true; 'UI-1' = $true; 'A-1' = $true; 'A-2' = $true; 'A-3' = $true; 'A-4' = $true; 'A-6' = $true; 'A-7' = $true; 'A-8' = $true; 'A-10' = $true; 'A-11' = $true; 'D-1' = $true }
    })

# 2) lead 点名的缺陷：passthrough（明文名文件）被一起丢掉 —— 丢得太多
$dropPlain = New-BaseRecord 'default'
Add-OutputForCategory $dropPlain 'restored' 'expected'
Add-DupOutputs $dropPlain
Add-ReportArtifact $dropPlain (New-ReportText)   # 报告口径仍然只算"未还原"，所以 A-4 不该炸
[void]$scenarios.Add([pscustomobject]@{
        Name = 'passthrough-dropped'; Mode = 'default'; Record = $dropPlain; Args = @()
        Expect = @{ 'A-1' = $true; 'A-2' = $false; 'A-3' = $true; 'A-4' = $true; 'A-6' = $true; 'A-7' = $false; 'A-8' = $true; 'P-3' = $false }
    })

# 3) 丢得太少：未还原的照落盘（明文文件名躺在 Hash 目录下 / 干脆还是 hash 名）
$leak = New-BaseRecord 'default'
Add-OutputForCategory $leak 'restored' 'expected'
Add-DupOutputs $leak
Add-OutputForCategory $leak 'plain' 'expected'
Add-OutputForCategory $leak 'missing-dir' 'hashDirPlainName'
Add-OutputForCategory $leak 'missing-file' 'source'
Add-OutputForCategory $leak 'missing-both' 'source'
Add-ReportArtifact $leak (New-ReportText)
[void]$scenarios.Add([pscustomobject]@{
        Name = 'hashdir-leak'; Mode = 'default'; Record = $leak; Args = @()
        Expect = @{ 'A-1' = $true; 'A-2' = $true; 'A-3' = $false; 'A-4' = $true; 'A-6' = $false; 'A-7' = $false; 'A-8' = $false; 'P-3' = $false }
    })

# 4) 报告里的"未还原被丢弃"与事实不符
$liar = New-BaseRecord 'default'
Add-OutputForCategory $liar 'restored' 'expected'
Add-DupOutputs $liar
Add-OutputForCategory $liar 'plain' 'expected'
Add-ReportArtifact $liar (New-ReportText -DroppedOverride 2)
[void]$scenarios.Add([pscustomobject]@{
        Name = 'report-lies'; Mode = 'default'; Record = $liar; Args = @()
        Expect = @{ 'A-1' = $true; 'A-2' = $true; 'A-4' = $false; 'A-7' = $true; 'D-1' = $true }
    })

# 5) 还原过程动了源目录
$touched = New-BaseRecord 'default'
Add-OutputForCategory $touched 'restored' 'expected'
Add-DupOutputs $touched
Add-OutputForCategory $touched 'plain' 'expected'
Add-ReportArtifact $touched (New-ReportText)
$firstName = @($touched.sourceSnapshotAfter.Keys)[0]
$touched.sourceSnapshotAfter[$firstName].mtime = '2026-01-02T00:00:00.0000000Z'
[void]$scenarios.Add([pscustomobject]@{
        Name = 'source-touched'; Mode = 'default'; Record = $touched; Args = @()
        Expect = @{ 'A-1' = $true; 'D-1' = $false }
    })

# 6) 步骤3 仍是旧的内联还原（没弹新窗口）
$legacy = New-BaseRecord 'default' -LegacyInline
Add-OutputForCategory $legacy 'restored' 'expected'
Add-DupOutputs $legacy
Add-OutputForCategory $legacy 'plain' 'expected'
Add-ReportArtifact $legacy (New-ReportText)
[void]$scenarios.Add([pscustomobject]@{
        Name = 'legacy-inline'; Mode = 'default'; Record = $legacy; Args = @()
        Expect = @{ 'UI-1' = $false; 'A-1' = $true }
    })

# 7) 勾选保留后正常：一个不丢
$correctKeep = New-BaseRecord 'keep'
Add-OutputForCategory $correctKeep 'restored' 'expected'
Add-DupOutputs $correctKeep
Add-OutputForCategory $correctKeep 'plain' 'expected'
Add-OutputForCategory $correctKeep 'missing-dir' 'hashDirPlainName'
Add-OutputForCategory $correctKeep 'missing-file' 'source'
Add-OutputForCategory $correctKeep 'missing-both' 'source'
Add-ReportArtifact $correctKeep (New-ReportText)
[void]$scenarios.Add([pscustomobject]@{
        Name = 'correct-keep'; Mode = 'keep'; Record = $correctKeep; Args = @()
        Expect = @{ 'RUN-1' = $true; 'B-1' = $true; 'B-2' = $true; 'B-3' = $true; 'B-4' = $true; 'B-6' = $true; 'D-1' = $true }
    })

# 8) 勾了保留却还是把未还原的丢了
$keepDrops = New-BaseRecord 'keep'
Add-OutputForCategory $keepDrops 'restored' 'expected'
Add-DupOutputs $keepDrops
Add-OutputForCategory $keepDrops 'plain' 'expected'
Add-ReportArtifact $keepDrops (New-ReportText)
[void]$scenarios.Add([pscustomobject]@{
        Name = 'keep-drops-unrestored'; Mode = 'keep'; Record = $keepDrops; Args = @()
        Expect = @{ 'B-1' = $false; 'B-2' = $false; 'B-3' = $true; 'B-4' = $true }
    })

# ---------- 逐个场景跑断言并核对 ----------

Write-Host ("[selftest] 合成工作区：{0}" -f $manifest.gameDir)
Write-Host ("[selftest] 期望计数：已还原 {0} / 明文名 {1} / 缺目录 {2} / 缺文件 {3} / 应丢弃 {4} / 总 {5}" -f `
        $counts.Restored, $counts.Plain, $counts.MissingDir, $counts.MissingFile, $counts.Dropped, $counts.Total)

$scenarioFailures = 0
foreach ($scenario in $scenarios)
{
    $runJson = Join-Path $selfTestDir ("run-{0}.json" -f $scenario.Name)
    $checkJson = Join-Path $selfTestDir ("checks-{0}.json" -f $scenario.Name)
    Write-JsonFile -Path $runJson -Object $scenario.Record
    Remove-Item -LiteralPath $checkJson -Force -ErrorAction SilentlyContinue

    $arguments = @('-Mode', $scenario.Mode, '-RunJson', $runJson, '-ChecksJson', $checkJson) + @($scenario.Args)
    & pwsh -NoProfile -File (Join-Path $root 'assert-restore.ps1') @arguments | Out-Null

    $checks = Read-JsonFile -Path $checkJson
    if (-not $checks)
    {
        Add-Check -Id ("SELFTEST/" + $scenario.Name) -Title '断言脚本跑出了结果' -Ok $false -Details @('assert-restore.ps1 没有写出 checks json')
        $scenarioFailures++
        continue
    }

    $actual = @{}
    foreach ($check in $checks.checks) { $actual[[string]$check.Id] = [bool]$check.Ok }

    $problems = @()
    foreach ($id in $scenario.Expect.Keys)
    {
        $want = [bool]$scenario.Expect[$id]
        if (-not $actual.ContainsKey($id))
        {
            $problems += ("期望 {0} 存在且为 {1}，但断言脚本根本没有产出这个检查项" -f $id, $(if ($want) { 'PASS' } else { 'FAIL' }))
            continue
        }
        if ($actual[$id] -ne $want)
        {
            $problems += ("{0}：期望 {1}，实际 {2}" -f $id, $(if ($want) { 'PASS' } else { 'FAIL' }), $(if ($actual[$id]) { 'PASS' } else { 'FAIL' }))
        }
    }

    # 没在预期里的失败项也算问题：说明场景构造或断言口径跑偏了
    foreach ($check in $checks.checks)
    {
        if ($check.Ok -or $check.Severity -ne 'fail') { continue }
        if (-not $scenario.Expect.ContainsKey([string]$check.Id)) { $problems += ("出现预期外的失败项：{0} {1}" -f $check.Id, $check.Title) }
    }

    if ($problems.Count -eq 0)
    {
        Add-Check -Id ("SELFTEST/" + $scenario.Name) -Title ("断言响应符合预期（{0} 项对照）" -f $scenario.Expect.Count) -Ok $true `
            -Details @(("预期失败的断言确实失败了，预期通过的确实通过了"))
    }
    else
    {
        $scenarioFailures++
        Add-Check -Id ("SELFTEST/" + $scenario.Name) -Title '断言响应符合预期' -Ok $false -Details $problems
    }
}

exit (Show-HarnessChecks -Title '脚手架自检（断言脚本的鉴别力）' -ChecksJson $ChecksJson)
