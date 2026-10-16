# 断言 A（默认语义）/ B（勾选保留）/ D（源目录只读）：读 run-step3-restore.ps1 的
# 结果 JSON + make-workspace.ps1 的期望清单，给出通过/不通过与差异明细。
#
#   pwsh -File assert-restore.ps1 -Mode default    # 断言A + 断言D
#   pwsh -File assert-restore.ps1 -Mode keep       # 断言B + 断言D
#   pwsh -File assert-restore.ps1 -Mode default -RunJson Run\out\step3-default.json
#
# 判定口径：
#   断言A（默认"只输出已还原的文件"）——四个方向都要锁住：
#     A-1 已还原的文件在期望路径上且逐字节一致          （丢得对）
#     A-2 本来就是明文名的文件（.alst 清单/readme 等）必须在输出里且逐字节一致
#                                                       （防"丢得太多"：passthrough 被一起丢掉）
#     A-3 路径里目录段仍是 Hash 的条目不得出现           （防"丢得太少"：只认了文件哈希就当成还原）
#     A-4 报告里的"未还原被丢弃"计数 == 脚手架独立枚举的应丢弃数
#                                                       （不信报告，两边各算一遍）
#     A-5 不存在任何 64 位十六进制文件名的文件
#     A-6 输出文件数与期望一致、没有多余文件
#     A-7 未还原文件的内容没有以任何名字混进输出
#     A-8 Extractor_Log 里的文件被跳过（软）
#   断言B（勾选「保留未还原文件名的文件」）：
#     B-1 源目录每个文件的内容都能在输出里找到（按内容比对，不绑死未还原文件的落点）
#     B-2 未还原的那几类确实被保留（不只是已还原的）
#     B-3/B-4 已还原、明文名文件仍在期望路径且逐字节一致
#     B-5 允许出现 hash 名/hash 目录（数量仅作提示）
#   断言D：还原前后源目录的文件数、大小、修改时间、内容 sha256 完全未变。
#
# 实现还没写完时：运行阶段就会留下 failure（没有还原窗口/没有产出），
# 这里会先把那条原因原样报出来，再逐项给出"缺什么"，不会崩。

[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('default', 'keep')][string]$Mode,
    [string]$RunJson = '',
    [string]$ManifestPath = '',
    [switch]$AllowLegacyInline,
    [string]$ChecksJson = '',
    [string]$ReportArtifactPattern = '^RestoreReport'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')

if (-not $RunJson) { $RunJson = Join-Path $root ("Run\out\step3-{0}.json" -f $Mode) }
if (-not $ManifestPath) { $ManifestPath = Join-Path $root 'Run\manifest.json' }

Reset-HarnessChecks
$prefix = if ($Mode -eq 'keep') { 'B' } else { 'A' }
$title = "断言 $prefix（步骤3 还原，模式 $Mode）"

$run = Read-JsonFile -Path $RunJson
$manifest = Read-JsonFile -Path $ManifestPath

if (-not $manifest)
{
    Add-Check -Id 'PRE-1' -Title '合成工作区清单存在' -Ok $false `
        -Details @("找不到 $ManifestPath；请先运行 make-workspace.ps1")
    exit (Show-HarnessChecks -Title $title -ChecksJson $ChecksJson)
}
if (-not $run)
{
    Add-Check -Id 'PRE-1' -Title '用例结果存在' -Ok $false `
        -Details @("找不到 $RunJson；请先运行 run-step3-restore.ps1" + $(if ($Mode -eq 'keep') { ' -KeepUnrestored' } else { '' }))
    exit (Show-HarnessChecks -Title $title -ChecksJson $ChecksJson)
}

Write-Host ("[assert-$Mode] 用例结果：{0}" -f $RunJson)
Write-Host ("[assert-$Mode] 界面模式：{0}；完成判据：{1}" -f $run.uiMode, $run.completionReason)
Write-Host ("[assert-$Mode] 输出目录：{0}" -f $run.outputDir)

# ---------- 0) 运行阶段本身有没有失败 ----------

$runFailed = -not [string]::IsNullOrWhiteSpace($run.failure)
$runOk = (-not $runFailed) -and $run.completed
$runDetails = @()
if ($run.failure) { $runDetails += ("失败原因：" + $run.failure) }
if (-not $run.completed -and -not $run.failure) { $runDetails += '运行阶段没有判定为完成' }
foreach ($dialog in @($run.modalDialogs)) { $runDetails += ("模态框：{0} / {1}" -f $dialog.Title, $dialog.Text) }
if (@($run.titleTimeline).Count -gt 0)
{
    $runDetails += ("还原窗口标题时间线：" + ((@($run.titleTimeline) | Select-Object -Last 6) -join ' → '))
}
if ($runDetails.Count -eq 0) { $runDetails += '还原跑完且没有弹出对话框' }
Add-Check -Id 'RUN-1' -Title '还原流程跑完（无失败原因/无意外弹框）' -Ok $runOk -Details $runDetails

# ---------- UI-1) 步骤3 是不是新的"还原窗口" ----------
# 本轮改造要求步骤3 是"按封包拆任务的多线程还原窗口"；旧实现是主窗口内联还原。

if ($run.uiMode -eq 'restore-window')
{
    Add-Check -Id 'UI-1' -Title '步骤3 打开了独立的还原窗口' -Ok $true `
        -Details @(("窗口：class='{0}' title='{1}'" -f $run.restoreWindow.Class, $run.restoreWindow.Title),
                   ("控件 " + @($run.controls).Count + " 个"))
}
else
{
    Add-Check -Id 'UI-1' -Title '步骤3 打开了独立的还原窗口' -Ok ([bool]$AllowLegacyInline) `
        -Severity $(if ($AllowLegacyInline) { 'warn' } else { 'fail' }) `
        -Details @("uiMode=$($run.uiMode)：没等到独立还原窗口。若本轮改造尚未落地，这就是预期结果。")
}

# ---------- 输出清单整理 ----------

# 输出清单：大用例在 TSV 里（几万条），小用例直接内联在结果 JSON 里。
$outputAll = @()
$outputTsv = Get-ObjectValue -Object $run -Name 'outputTsv' -Default ''
if ($outputTsv -and (Test-Path -LiteralPath $outputTsv))
{
    foreach ($row in (Read-TsvFile -Path $outputTsv))
    {
        if ($row.Count -lt 3) { continue }
        $outputAll += [pscustomobject]@{ rel = $row[0]; size = [long]$row[1]; sha256 = $row[2] }
    }
    Write-Host ("[assert-$Mode] 输出清单来自 TSV：{0}（{1} 条）" -f $outputTsv, $outputAll.Count)
}
else
{
    $outputAll = @($run.output)
}
$artifacts = @($outputAll | Where-Object { $_.rel -notmatch '\\' -and $_.rel -match $ReportArtifactPattern })
$payload = @($outputAll | Where-Object { -not ($_.rel -notmatch '\\' -and $_.rel -match $ReportArtifactPattern) })

Write-Host ("[assert-$Mode] 输出：{0} 个文件（其中报表产物 {1} 个，计入比对 {2} 个）" -f $outputAll.Count, $artifacts.Count, $payload.Count)

if ($artifacts.Count -gt 0)
{
    Add-Check -Id 'REPORT-1' -Title '输出了还原报告（RestoreReport）' -Ok $true -Severity 'info' `
        -Details @(($artifacts | ForEach-Object { "{0}（{1} 字节）" -f $_.rel, $_.size }))
}
else
{
    Add-Check -Id 'REPORT-1' -Title '输出了还原报告（RestoreReport）' -Ok $false -Severity 'warn' `
        -Details @('输出目录里没有还原报告；断言A-4 因此无法做"报告 vs 独立枚举"的对照。')
}

$sourceFiles = @(Get-ManifestFiles -JsonPath $ManifestPath)
$restoredSource = @($sourceFiles | Where-Object { $_.category -eq 'restored' })
$dupSource = @($sourceFiles | Where-Object { $_.category -eq 'restored-dup' })
$plainSource = @($sourceFiles | Where-Object { $_.category -eq 'plain' })
$notRestored = @($sourceFiles | Where-Object { $_.category -in @('missing-dir', 'missing-file', 'missing-both') })
$skippedLog = @($sourceFiles | Where-Object { $_.category -eq 'skipped-log' })

# 期望值由脚手架**独立枚举**得出（不读实现写的报告，也不读生成器算好的计数）。
$expectRestored = $restoredSource.Count
$expectDup = $dupSource.Count
$expectPlain = $plainSource.Count
$expectMissingDir = @($sourceFiles | Where-Object { $_.category -in @('missing-dir', 'missing-both') }).Count
$expectMissingFile = @($sourceFiles | Where-Object { $_.category -in @('missing-file', 'missing-both') }).Count
$expectDropped = $notRestored.Count
$expectTotal = $sourceFiles.Count - $skippedLog.Count
# 重名冲突：两个源文件落成"真名 + __dupN"，输出条数与源条数一致（一个都不能被覆盖掉）。
$expectDefaultOutput = $expectRestored + $expectPlain + $expectDup
$expectKeepOutput = $expectTotal

# 一个输出路径算不算"期望内的"：完全相等，或者是重名冲突落下来的 __dupN 变体。
$expectedRelSet = @{}
foreach ($item in @($restoredSource + $plainSource + $dupSource))
{
    $expectedRelSet[$item.expectedRel.ToLowerInvariant()] = $true
}
function Test-ExpectedOutputName
{
    param([Parameter(Mandatory)][string]$Rel)

    $lower = $Rel.ToLowerInvariant()
    if ($expectedRelSet.ContainsKey($lower)) { return $true }
    $directory = Split-Path $Rel -Parent
    $leaf = Split-Path $Rel -Leaf
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($leaf)
    $extension = [System.IO.Path]::GetExtension($leaf)
    if ($stem -notmatch '^(?<base>.+)__dup\d+$') { return $false }
    $candidate = if ($directory) { Join-Path $directory ($matches['base'] + $extension) } else { $matches['base'] + $extension }
    return $expectedRelSet.ContainsKey($candidate.ToLowerInvariant())
}

# 以相对路径为键建立输出索引（大小写不敏感，Windows 语义）
$outputByRel = @{}
foreach ($item in $payload) { $outputByRel[$item.rel.ToLowerInvariant()] = $item }

# 按内容找（用于"未还原文件有没有混进来"和断言B）
$outputByHash = @{}
foreach ($item in $payload)
{
    if (-not $outputByHash.ContainsKey($item.sha256)) { $outputByHash[$item.sha256] = @() }
    $outputByHash[$item.sha256] += $item
}

# 输出路径里"目录段仍是 Hash"的条目：默认语义下这类东西不该出现
# （文件哈希认出来了、目录哈希没认出来，名字虽明文但躺在 Hash 目录下）。
function Get-HashResidue
{
    param([Parameter(Mandatory)][string]$Relative)

    $parts = $Relative -split '\\'
    $found = @()
    for ($index = 0; $index -lt ($parts.Count - 1); $index++)
    {
        if ($parts[$index] -match '^[0-9A-Fa-f]{16}$') { $found += ('16位目录 ' + $parts[$index]) }
        elseif ($parts[$index] -match '^[0-9A-Fa-f]{64}$') { $found += ('64位目录 ' + $parts[$index]) }
    }
    # 逗号不能省：空数组会被 PowerShell 拆成"没有输出"，调用方拿到 $null，
    # 后面 .Count 就会在 StrictMode 下直接报错。
    return ,$found
}

# 期望集合里逐路径比对的公共逻辑
function Compare-Expected
{
    param([Parameter(Mandatory)]$Items)

    $missing = @()
    $mismatch = @()
    foreach ($item in $Items)
    {
        $key = $item.expectedRel.ToLowerInvariant()
        if (-not $outputByRel.ContainsKey($key))
        {
            $missing += ("{0}（类别 {1}，源 {2}）" -f $item.expectedRel, $item.category, $item.sourceRel)
            continue
        }
        $actual = $outputByRel[$key]
        if ($actual.sha256 -ne $item.sha256)
        {
            $mismatch += ("{0}：期望 sha256 {1}，实际 {2}（{3} 字节 vs 期望 {4} 字节）" -f `
                $item.expectedRel, $item.sha256, $actual.sha256, $actual.size, $item.size)
        }
    }
    return [pscustomobject]@{ Missing = $missing; Mismatch = $mismatch }
}

# ---------- 断言 A：默认语义 ----------

if ($Mode -eq 'default')
{
    # A-1 已还原的文件
    $compare = Compare-Expected -Items $restoredSource
    $details = @()
    if ($compare.Missing.Count -eq 0 -and $compare.Mismatch.Count -eq 0)
    {
        $details += ("{0} 个已还原文件都在期望路径上，且逐字节一致" -f $expectRestored)
    }
    else
    {
        if ($compare.Missing.Count -gt 0) { $details += ("缺失 {0} 个：" -f $compare.Missing.Count); $details += ($compare.Missing | ForEach-Object { '    ' + $_ }) }
        if ($compare.Mismatch.Count -gt 0) { $details += ("内容不一致 {0} 个：" -f $compare.Mismatch.Count); $details += ($compare.Mismatch | ForEach-Object { '    ' + $_ }) }
        if ($payload.Count -eq 0) { $details += '（输出目录里一个文件都没有 —— 多半是实现还没产出，或输出到了别处）' }
    }
    Add-Check -Id 'A-1' -Title '已还原文件按期望路径逐字节一致' -Ok ($compare.Missing.Count -eq 0 -and $compare.Mismatch.Count -eq 0) -Details $details

    # A-2 本来就是明文名的文件必须还在（防"丢得太多"）
    $comparePlain = Compare-Expected -Items $plainSource
    $plainDetails = @()
    if ($comparePlain.Missing.Count -eq 0 -and $comparePlain.Mismatch.Count -eq 0)
    {
        $plainDetails += ("{0} 个明文名文件（.alst 清单 / readme 等）都在输出里，且逐字节一致" -f $expectPlain)
    }
    else
    {
        $plainDetails += '默认模式只该丢掉"没还原出来的哈希名文件"，本来就是明文名的文件必须原样保留；'
        if ($comparePlain.Missing.Count -gt 0)
        {
            $plainDetails += ("丢了 {0} 个：" -f $comparePlain.Missing.Count)
            $plainDetails += ($comparePlain.Missing | ForEach-Object { '    ' + $_ })
        }
        if ($comparePlain.Mismatch.Count -gt 0)
        {
            $plainDetails += ("内容不一致 {0} 个：" -f $comparePlain.Mismatch.Count)
            $plainDetails += ($comparePlain.Mismatch | ForEach-Object { '    ' + $_ })
        }
    }
    Add-Check -Id 'A-2' -Title '明文名文件（passthrough）在默认模式下必须保留' -Ok ($comparePlain.Missing.Count -eq 0 -and $comparePlain.Mismatch.Count -eq 0) -Details $plainDetails

    # A-3 目录段仍是 Hash 的条目不得出现
    $residue = @()
    foreach ($item in $payload)
    {
        $segments = Get-HashResidue -Relative $item.rel
        if ($segments.Count -gt 0) { $residue += ("{0}  ← {1}" -f $item.rel, ($segments -join ', ')) }
    }
    $residueDetails = @()
    if ($residue.Count -eq 0)
    {
        $residueDetails += '输出里没有"明文文件名躺在 Hash 目录下"的条目'
    }
    else
    {
        $residueDetails += ("出现 {0} 个目录段仍是 Hash 的条目（默认语义下不该有 —— 说明只认了文件哈希就落盘了）：" -f $residue.Count)
        $residueDetails += ($residue | Select-Object -First 25 | ForEach-Object { '    ' + $_ })
        if ($residue.Count -gt 25) { $residueDetails += ("    ……还有 {0} 个" -f ($residue.Count - 25)) }
    }
    Add-Check -Id 'A-3' -Title '输出里不存在"目录段仍是 Hash"的条目' -Ok ($residue.Count -eq 0) -Details $residueDetails

    # A-4 报告里的"未还原被丢弃"计数 vs 脚手架独立枚举
    $reportNumbers = $null
    if ($run.reportText) { $reportNumbers = Get-ReportNumberMap -Text ([string]$run.reportText) }
    $dropped = $null
    if ($reportNumbers) { $dropped = Find-ReportNumber -Map $reportNumbers -Keywords @('丢弃', '未输出', '未还原') -ExcludeKeywords @('已还原', '还原率', '成功率') }
    $a4Details = @(("脚手架独立枚举：源文件 {0} 个（不含工具日志 {1} 个）＝ 已还原 {2} + 明文名 {3} + 未还原 {4}" -f `
                $expectTotal, $skippedLog.Count, $expectRestored, $expectPlain, $expectDropped))
    $a4Ok = $false
    if (-not $reportNumbers)
    {
        $a4Details += ("报告没读到（reportPath='{0}'）—— 无法做对照" -f $run.reportPath)
    }
    elseif ($null -eq $dropped)
    {
        $a4Details += '报告里找不到"未还原被丢弃 / 未输出 / 未还原"这类计数口径'
        $a4Details += ('报告里识别到的标签：' + (($reportNumbers.Keys) -join ' / '))
    }
    else
    {
        $a4Ok = ($dropped.Value -eq $expectDropped)
        $a4Details += ("报告口径「{0}」= {1}；期望 {2}" -f $dropped.Label, $dropped.Value, $expectDropped)
        if (-not $a4Ok)
        {
            $a4Details += '数字对不上：要么报告没算对，要么实际丢弃的文件数与"映射查不到"的预期不一致。'
            $a4Details += ("实际输出 {0} 个文件；期望默认模式输出 {1} 个" -f $payload.Count, $expectDefaultOutput)
        }
    }
    Add-Check -Id 'A-4' -Title '报告里的"未还原被丢弃"计数与独立枚举一致' -Ok $a4Ok -Details $a4Details

    # A-5 报告其它口径的对照（软）
    # 注意：实现说的"已还原"包含重名冲突那两个（它们也还原了，只是名字带了 __dupN），
    # 所以这里用 restored + dup 作为期望值。
    $expectRestoredForReport = $expectRestored + $expectDup
    $a5Details = @()
    $a5Ok = $true
    if ($reportNumbers)
    {
        $dimensions = @(
            @{ Name = '已还原'; Keywords = @('已还原'); Exclude = @('未还原', '率'); Expect = $expectRestoredForReport }
            @{ Name = '缺目录 Hash'; Keywords = @('缺目录'); Exclude = @(); Expect = $expectMissingDir }
            @{ Name = '缺文件 Hash'; Keywords = @('缺文件'); Exclude = @(); Expect = $expectMissingFile }
            @{ Name = '非哈希名/原样'; Keywords = @('非哈希', '原样', '明文'); Exclude = @(); Expect = $expectPlain }
            @{ Name = '总文件数'; Keywords = @('总文件数', '文件总数', '源文件数'); Exclude = @(); Expect = $expectTotal }
            @{ Name = '目标重名改名'; Keywords = @('重名'); Exclude = @(); Expect = $(if ($expectDup -gt 0) { $expectDup - 1 } else { 0 }) }
            @{ Name = '失败'; Keywords = @('失败'); Exclude = @('率'); Expect = 0 }
            @{ Name = '跳过日志文件'; Keywords = @('跳过'); Exclude = @(); Expect = $skippedLog.Count }
        )
        foreach ($dimension in $dimensions)
        {
            $found = Find-ReportNumber -Map $reportNumbers -Keywords $dimension.Keywords -ExcludeKeywords $dimension.Exclude
            if ($null -eq $found)
            {
                $a5Details += ("    {0}：报告里没有这个口径（跳过）" -f $dimension.Name)
                continue
            }
            $ok = ($found.Value -eq $dimension.Expect)
            if (-not $ok) { $a5Ok = $false }
            $a5Details += ("    {0}：报告 {1} / 期望 {2}  {3}" -f $dimension.Name, $found.Value, $dimension.Expect, $(if ($ok) { 'OK' } else { '不一致' }))
        }
    }
    else
    {
        $a5Details += '报告没读到，这一项跳过'
    }
    Add-Check -Id 'A-5' -Title '报告其它计数与独立枚举的对照' -Ok $a5Ok -Severity 'warn' -Details $a5Details

    # A-6 不存在 64 位十六进制文件名
    $hashNamedOutput = @($payload | Where-Object { (Split-Path $_.rel -Leaf) -match '^[0-9A-Fa-f]{64}$' })
    $a6Details = @()
    if ($hashNamedOutput.Count -eq 0)
    {
        $a6Details += '输出目录里没有任何 64 位十六进制文件名'
    }
    else
    {
        $a6Details += ("出现 {0} 个仍是 64 位十六进制文件名的文件（默认语义下不该有）：" -f $hashNamedOutput.Count)
        $a6Details += ($hashNamedOutput | Select-Object -First 25 | ForEach-Object { '    ' + $_.rel })
        if ($hashNamedOutput.Count -gt 25) { $a6Details += ("    ……还有 {0} 个" -f ($hashNamedOutput.Count - 25)) }
    }
    Add-Check -Id 'A-6' -Title '输出目录不存在 64 位十六进制文件名的文件' -Ok ($hashNamedOutput.Count -eq 0) -Details $a6Details

    # A-7 数量一致（不多不少）
    $extra = @($payload | Where-Object { -not (Test-ExpectedOutputName -Rel $_.rel) })
    $a7Details = @(("期望 {0} 个（已还原 {1} + 同名冲突 {2} + 明文名 {3}），实际 {4} 个" -f $expectDefaultOutput, $expectRestored, $expectDup, $expectPlain, $payload.Count))
    if ($extra.Count -gt 0)
    {
        $a7Details += ("多出 {0} 个不该出现的文件：" -f $extra.Count)
        $a7Details += ($extra | Select-Object -First 25 | ForEach-Object { '    ' + $_.rel })
    }
    Add-Check -Id 'A-7' -Title '输出文件数与期望一致（不多不少）' -Ok ($payload.Count -eq $expectDefaultOutput -and $extra.Count -eq 0) -Details $a7Details

    # A-10 重名冲突：同名条目必须落 __dupN，两份数据都不能被覆盖掉
    $a10Details = @()
    $a10Ok = $true
    if ($expectDup -eq 0)
    {
        $a10Details += '合成工作区里没有同名冲突条目，跳过'
    }
    else
    {
        $canonical = @{}
        foreach ($item in $dupSource)
        {
            $key = $item.expectedRel.ToLowerInvariant()
            if (-not $canonical.ContainsKey($key)) { $canonical[$key] = @() }
            $canonical[$key] += $item
        }
        foreach ($key in $canonical.Keys)
        {
            $group = @($canonical[$key])
            $rel = $group[0].expectedRel
            $produced = @($payload | Where-Object { $_.rel.ToLowerInvariant() -eq $key })
            $duplicates = @($payload | Where-Object {
                    $_.rel.ToLowerInvariant() -ne $key -and
                    (Split-Path $_.rel -Leaf) -match ('^' + [regex]::Escape([System.IO.Path]::GetFileNameWithoutExtension($rel)) + '__dup\d+' + [regex]::Escape([System.IO.Path]::GetExtension($rel)) + '$') -and
                    (Split-Path $_.rel -Parent) -eq (Split-Path $rel -Parent)
                })
            $a10Details += ("{0}：源里 {1} 个同名文件；输出里正好叫这个名字的 {2} 个，落成 __dupN 的 {3} 个" -f $rel, $group.Count, $produced.Count, $duplicates.Count)
            foreach ($item in $group)
            {
                if (-not $outputByHash.ContainsKey($item.sha256))
                {
                    $a10Ok = $false
                    $a10Details += ("    内容丢失：源 {0}（{1} 字节）的内容在输出里找不到 —— 被同名文件覆盖了" -f $item.sourceRel, $item.size)
                }
            }
            if (($produced.Count + $duplicates.Count) -ne $group.Count)
            {
                $a10Ok = $false
                $a10Details += ("    输出条目数 {0} 与源条目数 {1} 不等（应为 1 个真名 + {2} 个 __dupN）" -f ($produced.Count + $duplicates.Count), $group.Count, ($group.Count - 1))
            }
            if ($produced.Count -ne 1)
            {
                $a10Ok = $false
                $a10Details += ("    真名条目应为 1 个，实际 {0} 个" -f $produced.Count)
            }
            foreach ($item in $duplicates) { $a10Details += ("    落成：{0}" -f $item.rel) }
        }
    }
    Add-Check -Id 'A-10' -Title '重名冲突落 __dupN，两份数据都留下来了' -Ok $a10Ok -Details $a10Details

    # A-11 多线程任务全部收尾（不出现永远"处理中"）
    $rows = @($run.listRows)
    $a11Details = @()
    $a11Ok = $true
    if ($rows.Count -eq 0)
    {
        $a11Details += '没读到任务列表（ListView 不存在或读不出）—— 无法逐行判定；'
        $a11Details += ("界面终态证据（窗口标题时间线结尾）：{0}" -f (@($run.titleTimeline) | Select-Object -Last 1))
        $a11Ok = [bool]$run.completed
    }
    else
    {
        $stuckPattern = '等待|排队|处理中|运行中|进行中|待处理'
        foreach ($row in $rows)
        {
            $cells = [string]$row.Cells -split ' \| '
            $state = if ($cells.Count -ge 2) { $cells[1] } else { '' }
            $terminal = ($state -notmatch $stuckPattern)
            if (-not $terminal) { $a11Ok = $false }
            $a11Details += ("    [{0}] {1}  <- {2}" -f $(if ($terminal) { '终态' } else { '未收尾' }), $row.Cells, $state)
        }
        $a11Details += ("共 {0} 行任务；标题时间线结尾：{1}" -f $rows.Count, (@($run.titleTimeline) | Select-Object -Last 1))
    }
    Add-Check -Id 'A-11' -Title '所有封包任务都到了终态（无"永远处理中"）' -Ok $a11Ok -Details $a11Details

    # A-8 未还原文件的内容没有以任何名字混进来
    $leaked = @()
    foreach ($item in $notRestored)
    {
        if ($outputByHash.ContainsKey($item.sha256))
        {
            $where = ($outputByHash[$item.sha256] | ForEach-Object { $_.rel }) -join ', '
            $leaked += ("[{0}] {1}（源 {2}）出现在：{3}" -f $item.category, $item.note, $item.sourceRel, $where)
        }
    }
    $a8Details = @()
    if ($leaked.Count -eq 0)
    {
        $a8Details += ("{0} 个未还原文件（缺目录名 {1} / 缺文件名 {2}）的内容都没有出现在输出里" -f $notRestored.Count, $expectMissingDir, $expectMissingFile)
    }
    else
    {
        $a8Details += ("{0} 个未还原文件仍被输出（默认语义是「只输出已还原的文件」）：" -f $leaked.Count)
        $a8Details += ($leaked | ForEach-Object { '    ' + $_ })
    }
    Add-Check -Id 'A-8' -Title '未还原文件的内容没有出现在默认输出里' -Ok ($leaked.Count -eq 0) -Details $a8Details

    # A-9（软）工具自己的日志目录应被跳过
    $logLeak = @($payload | Where-Object { $_.rel -match '(?i)(^|\\)(Extractor_Log|ExtractLog)\\' })
    Add-Check -Id 'A-9' -Title '工具日志目录（Extractor_Log）被跳过' -Ok ($logLeak.Count -eq 0) -Severity 'warn' `
        -Details $(if ($logLeak.Count -eq 0) { @('没有把 Extractor_Log 里的文件复制出来') } else { @(($logLeak | ForEach-Object { '    ' + $_.rel })) })

    # A-12 报告里的「跳过日志文件」必须等于脚手架独立枚举的条数。
    # 合成工作区里故意放了两处：根目录下 1 个 + 封包内部 1 个；
    # 如果实现"根任务递归数一遍 + 封包任务自己再数一遍"，这里就会变成 3 → 立刻暴露双计。
    $reportNumbersForSkip = $null
    if ($run.reportText) { $reportNumbersForSkip = Get-ReportNumberMap -Text ([string]$run.reportText) }
    $skipFound = $null
    if ($reportNumbersForSkip) { $skipFound = Find-ReportNumber -Map $reportNumbersForSkip -Keywords @('跳过') -ExcludeKeywords @() }
    $skipExpected = $skippedLog.Count
    $skipRoot = @($skippedLog | Where-Object { $_.sourceRel -notmatch '\\' -or ($_.sourceRel -split '\\').Count -eq 2 }).Count
    $skipInside = $skipExpected - $skipRoot
    $a12Details = @(("脚手架独立枚举：共 {0} 个应跳过（根目录下 {1} 个 + 封包内部 {2} 个）" -f $skipExpected, $skipRoot, $skipInside))
    $a12Details += ($skippedLog | ForEach-Object { '    ' + $_.sourceRel })
    $a12Ok = $false
    if ($null -eq $skipFound)
    {
        $a12Details += '报告里找不到「跳过日志文件」这个口径'
        if ($reportNumbersForSkip) { $a12Details += ('报告里识别到的标签：' + (($reportNumbersForSkip.Keys) -join ' / ')) }
    }
    else
    {
        $a12Ok = ($skipFound.Value -eq $skipExpected)
        $a12Details += ("报告口径「{0}」= {1}；期望 {2}" -f $skipFound.Label, $skipFound.Value, $skipExpected)
        if (-not $a12Ok)
        {
            $a12Details += '数字对不上：既可能是没数，也可能是把封包内部的日志重复数了一遍（根任务递归 + 封包任务自查）。'
        }
    }
    Add-Check -Id 'A-12' -Title '报告的「跳过日志文件」计数 == 独立枚举的跳过条数（防重复计数）' -Ok $a12Ok -Details $a12Details
}

# ---------- 断言 P：压力/多线程收尾（默认模式）----------

if ($Mode -eq 'default')
{
    $rows = @(Get-ObjectValue -Object $run -Name 'listRows' -Default @())
    $titles = @(Get-ObjectValue -Object $run -Name 'titleTimeline' -Default @())
    $lastTitle = if ($titles.Count -gt 0) { [string]$titles[-1] } else { '' }
    $expectedTasks = @($manifest.packages).Count + 1   # 每个封包一个任务 + 空封包
    $stuckPattern = '等待|排队|处理中|运行中|进行中|待处理'

    # P-1 所有任务终态
    $terminal = 0
    $stuck = @()
    foreach ($row in $rows)
    {
        $cells = [string]$row.Cells -split ' \| '
        $state = if ($cells.Count -ge 2) { $cells[1] } else { '' }
        if ($state -match $stuckPattern) { $stuck += ("{0}（状态 {1}）" -f $row.Cells, $state) } else { $terminal++ }
    }
    $p1Details = @(("任务列表 {0} 行；终态 {1} 行；期望任务数 {2}（{3} 个封包 + 空封包）" -f $rows.Count, $terminal, $expectedTasks, (@($manifest.packages).Count)))
    if ($stuck.Count -gt 0) { $p1Details += ("未收尾 {0} 行：" -f $stuck.Count); $p1Details += ($stuck | ForEach-Object { '    ' + $_ }) }
    $p1Ok = ($rows.Count -gt 0) -and ($stuck.Count -eq 0) -and ($rows.Count -eq $expectedTasks)
    if ($rows.Count -eq 0) { $p1Details += '没读到任务列表（ListView 读不出）—— 无法逐行判定' }
    Add-Check -Id 'P-1' -Title '多线程：所有封包任务都到终态，且任务数与封包数一致' -Ok $p1Ok -Details $p1Details

    # P-2 标题统计与任务列表一致
    $titleDone = $null
    $titleRunning = $null
    $titleQueued = $null
    if ($lastTitle -match '完成\s*(\d+)') { $titleDone = [int]$matches[1] }
    if ($lastTitle -match '运行中\s*(\d+)') { $titleRunning = [int]$matches[1] }
    if ($lastTitle -match '排队\s*(\d+)') { $titleQueued = [int]$matches[1] }
    $p2Details = @(("标题：'{0}'" -f $lastTitle),
                   ("解析出：完成 {0} / 运行中 {1} / 排队 {2}；任务列表终态 {3} 行" -f $titleDone, $titleRunning, $titleQueued, $terminal))
    $p2Ok = ($null -ne $titleDone) -and ($titleDone -eq $terminal) -and ($titleRunning -eq 0) -and ($titleQueued -eq 0)
    if ($null -eq $titleDone) { $p2Details += '标题里没有「完成 N」字样' }
    Add-Check -Id 'P-2' -Title '标题统计（完成/运行中/排队）与任务列表一致' -Ok $p2Ok -Details $p2Details

    # P-3 输出语义（重述一遍，让压力报告能独立成篇）
    $hashNamed = @($payload | Where-Object { (Split-Path $_.rel -Leaf) -match '^[0-9A-Fa-f]{64}$' })
    $p3Ok = ($hashNamed.Count -eq 0) -and ($payload.Count -eq $manifest.expectedDefaultCount)
    Add-Check -Id 'P-3' -Title '压力用例下：输出无 64 位 Hash 名，且输出数 == 期望数' -Ok $p3Ok `
        -Details @(("输出 {0} 个（期望 {1}）；其中 64 位 Hash 名 {2} 个" -f $payload.Count, $manifest.expectedDefaultCount, $hashNamed.Count))

    # P-4 节流：界面上报次数必须远小于文件数
    $uiLogLines = [int](Get-ObjectValue -Object $run -Name 'uiLogLines' -Default 0)
    $restoreLogLines = 0
    foreach ($file in @(Get-ObjectValue -Object $run -Name 'toolLogFiles' -Default @()))
    {
        if ($file.Name -eq 'YuaExtractRestore.log') { $restoreLogLines = [int]$file.Lines }
    }
    $fileCount = $sourceFiles.Count
    $p4Details = @(("源文件 {0} 个；界面详细日志 {1} 行；YuaExtractRestore.log {2} 行；任务 {3} 个" -f $fileCount, $uiLogLines, $restoreLogLines, $rows.Count),
                   ("界面日志/文件数 = 1 : {0:N0}" -f $(if ($uiLogLines -gt 0) { $fileCount / $uiLogLines } else { 0 })),
                   ("界面采样到的列表变化次数：{0}" -f (Get-ObjectValue -Object $run -Name 'listSampleCount' -Default 0)))
    $bound = [Math]::Max(50, $rows.Count * 10 + 20)
    $p4Ok = ($uiLogLines -gt 0) -and ($uiLogLines -le $bound) -and ($fileCount -lt 1000 -or $uiLogLines -lt ($fileCount / 10))
    $p4Details += ("判定：界面日志行数 ≤ max(50, 任务数×10+20) = {0}，且大用例下 < 文件数/10" -f $bound)
    Add-Check -Id 'P-4' -Title '进度上报被节流：界面上报行数远小于文件数' -Ok $p4Ok -Details $p4Details

    # P-5（提示）耗时、吞吐、界面响应
    $elapsedMs = [int](Get-ObjectValue -Object $run -Name 'restoreElapsedMs' -Default 0)
    $pollCount = [int](Get-ObjectValue -Object $run -Name 'pollCount' -Default 0)
    $hungCount = [int](Get-ObjectValue -Object $run -Name 'pollHungCount' -Default 0)
    $latencyMax = [int](Get-ObjectValue -Object $run -Name 'probeLatencyMaxMs' -Default 0)
    $latencyAvg = [int](Get-ObjectValue -Object $run -Name 'probeLatencyAvgMs' -Default 0)
    $p5Details = @(("整轮耗时 {0:N1} 秒（从点「开始」到判定完成）；吞吐 {1:N0} 文件/秒（按输出 {2} 个文件算）" -f `
                ($elapsedMs / 1000.0), $(if ($elapsedMs -gt 0) { $payload.Count / ($elapsedMs / 1000.0) } else { 0 }), $payload.Count),
                   ("界面探测（WM_NULL + SendMessageTimeout）：{0} 次，超时 {1} 次，最大 {2} ms，平均 {3} ms" -f $pollCount, $hungCount, $latencyMax, $latencyAvg),
                   ("每封包明细文件：{0}" -f ((@(Get-ObjectValue -Object $run -Name 'toolLogFiles' -Default @()) | ForEach-Object { '{0}({1}行)' -f $_.Name, $_.Lines }) -join ', ')))
    Add-Check -Id 'P-5' -Title '（提示）耗时 / 吞吐 / 界面响应' -Ok $true -Severity 'info' -Details $p5Details
}

# ---------- 断言 B：勾选保留 ----------

if ($Mode -eq 'keep')
{
    $sources = @($sourceFiles | Where-Object { $_.category -ne 'skipped-log' })
    $lost = @()
    foreach ($item in $sources)
    {
        if (-not $outputByHash.ContainsKey($item.sha256)) { $lost += ("[{0}] {1}（源 {2}）" -f $item.category, $item.note, $item.sourceRel) }
    }
    $b1Details = @(("源目录 {0} 个应保留的文件，输出里按内容匹配到 {1} 个" -f $sources.Count, ($sources.Count - $lost.Count)))
    if ($lost.Count -gt 0) { $b1Details += ("丢了 {0} 个：" -f $lost.Count); $b1Details += ($lost | ForEach-Object { '    ' + $_ }) }
    Add-Check -Id 'B-1' -Title '勾选保留后，源目录每个文件的内容都能在输出里找到' -Ok ($lost.Count -eq 0) -Details $b1Details

    $keptUnrestored = @($notRestored | Where-Object { $outputByHash.ContainsKey($_.sha256) })
    $b2Details = @(("{0} 个未还原文件里，有 {1} 个被保留到了输出" -f $notRestored.Count, $keptUnrestored.Count))
    $b2Details += ($notRestored | ForEach-Object {
            $state = if ($outputByHash.ContainsKey($_.sha256)) { '已保留' } else { '丢失' }
            ('    [{0}] {1}  {2}' -f $state, $_.category, $_.sourceRel)
        })
    Add-Check -Id 'B-2' -Title '未还原文件被保留（不只是已还原的那些）' -Ok ($keptUnrestored.Count -eq $notRestored.Count) -Details $b2Details

    $compare = Compare-Expected -Items $restoredSource
    Add-Check -Id 'B-3' -Title '已还原文件仍在期望路径且内容一致' -Ok ($compare.Missing.Count -eq 0 -and $compare.Mismatch.Count -eq 0) `
        -Details $(if ($compare.Missing.Count -eq 0 -and $compare.Mismatch.Count -eq 0) { @(("{0} 个已还原文件路径正确、逐字节一致" -f $expectRestored)) } else { @(($compare.Missing + $compare.Mismatch) | ForEach-Object { '    ' + $_ }) })

    $comparePlain = Compare-Expected -Items $plainSource
    Add-Check -Id 'B-4' -Title '明文名文件仍在期望路径且内容一致' -Ok ($comparePlain.Missing.Count -eq 0 -and $comparePlain.Mismatch.Count -eq 0) `
        -Details $(if ($comparePlain.Missing.Count -eq 0 -and $comparePlain.Mismatch.Count -eq 0) { @(("{0} 个明文名文件路径正确、逐字节一致" -f $expectPlain)) } else { @(($comparePlain.Missing + $comparePlain.Mismatch) | ForEach-Object { '    ' + $_ }) })

    # B-5（提示）保留模式下允许出现 hash 名与 hash 目录
    $hashNamedOutput = @($payload | Where-Object { (Split-Path $_.rel -Leaf) -match '^[0-9A-Fa-f]{64}$' })
    $residue = @($payload | Where-Object { (Get-HashResidue -Relative $_.rel).Count -gt 0 })
    $b5Details = @(("仍是 64 位十六进制文件名的文件：{0} 个" -f $hashNamedOutput.Count),
                   ("目录段仍是 16/64 位 Hash 的条目：{0} 个" -f $residue.Count),
                   ("保留模式下这些都允许出现（未还原文件按原样落盘）；" + ("示例：{0}" -f (($residue | Select-Object -First 3 | ForEach-Object { $_.rel }) -join ' ; '))))
    Add-Check -Id 'B-5' -Title '保留模式下允许出现 hash 名 / hash 目录（仅提示）' -Ok $true -Severity 'info' -Details $b5Details

    # B-6 数量对照（软）：保留模式下应当"一个不丢"
    $expectedKeys = @($sources | ForEach-Object { $_.expectedRel } | Where-Object { $_ })
    $b6Details = @(("源文件 {0} 个（不含工具日志），期望输出 {1} 个；实际 {2} 个" -f $sources.Count, $expectKeepOutput, $payload.Count))
    $b6Ok = ($sources.Count -eq $payload.Count)
    if (-not $b6Ok)
    {
        $b6Details += '数量不等：按内容匹配能对上就说明没丢数据，多出来的可能是报告或额外产物，点开看明细。'
    }
    Add-Check -Id 'B-6' -Title '保留模式下输出文件数与源文件数一致' -Ok $b6Ok -Severity 'warn' -Details $b6Details
}

# ---------- 断言 D：源目录只读 ----------

$dDetails = @()
if ($null -eq $run.sourceSnapshotBefore -or $null -eq $run.sourceSnapshotAfter)
{
    $beforeDigest = Get-ObjectValue -Object $run -Name 'sourceDigestBefore' -Default $null
    $afterDigest = Get-ObjectValue -Object $run -Name 'sourceDigestAfter' -Default $null
    if ($beforeDigest -and $afterDigest)
    {
        # 大用例不存逐条明细，比"文件数 + 排序行 sha256"的摘要：
        # 任何一个文件被改名/增删/改时间都会让摘要变化。
        $same = ($beforeDigest.Digest -eq $afterDigest.Digest) -and ($beforeDigest.Count -eq $afterDigest.Count) -and
                ($beforeDigest.HashDigest -eq $afterDigest.HashDigest)
        $dDetails += ("源目录摘要（前）{0} 个文件 digest {1}" -f $beforeDigest.Count, ([string]$beforeDigest.Digest).Substring(0, 16))
        $dDetails += ("源目录摘要（后）{0} 个文件 digest {1}" -f $afterDigest.Count, ([string]$afterDigest.Digest).Substring(0, 16))
        $dDetails += '摘要 = 排序后的「相对路径|大小|修改时间」的 sha256，另比对了内容哈希摘要'
        Add-Check -Id 'D-1' -Title '源解包目录（Extractor_Output）只读：还原前后摘要一致' -Ok $same -Details $dDetails
    }
    else
    {
        $dDetails += '用例没有留下完整的源目录快照（多半是运行阶段提前失败了，见上面第 1 项）'
        Add-Check -Id 'D-1' -Title '源解包目录（Extractor_Output）只读：还原前后完全未变' -Ok $false -Details $dDetails
    }
}
else
{
    $diff = Compare-DirectorySnapshot -Before $run.sourceSnapshotBefore -After $run.sourceSnapshotAfter
    if ($diff.Same)
    {
        $dDetails += ("源目录 {0} 个文件：文件数、大小、修改时间、内容 sha256 全都没变" -f $diff.Before)
    }
    else
    {
        if ($diff.Added.Count -gt 0) { $dDetails += ("新增 {0} 个：" -f $diff.Added.Count); $dDetails += ($diff.Added | ForEach-Object { '    + ' + $_ }) }
        if ($diff.Removed.Count -gt 0) { $dDetails += ("消失 {0} 个：" -f $diff.Removed.Count); $dDetails += ($diff.Removed | ForEach-Object { '    - ' + $_ }) }
        if ($diff.Changed.Count -gt 0) { $dDetails += ("被改动 {0} 个：" -f $diff.Changed.Count); $dDetails += ($diff.Changed | ForEach-Object { '    * ' + $_ }) }
    }
    Add-Check -Id 'D-1' -Title '源解包目录（Extractor_Output）只读：还原前后完全未变' -Ok $diff.Same -Details $dDetails
}

exit (Show-HarnessChecks -Title $title -ChecksJson $ChecksJson)
