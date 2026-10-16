# 扩展集一期断言 E-1..E-10（docs\扩展集设计.md §11）
#
#   pwsh -File assert-extension.ps1
#   pwsh -File assert-extension.ps1 -ManifestPath Run\extension-manifest.json `
#        -DraftRunJson Run\out\extension-old.json -RawRunJson Run\out\extension-raw.json `
#        -RepeatDraftRunJson Run\out\extension-old-repeat.json -RepeatInstallRunJson Run\out\extension-old-install2.json `
#        -Mode1RunJson Run\out\extension-mode1.json -ClassicRunJson Run\out\extension-classic.json `
#        -BadArchiveRunJsonCsv 'salt-mismatch=Run\out\extension-bad-salt-mismatch.json;crc-error=...' `
#        -ChecksJson Run\out\checks-extension.json
#
# 原则（沿用既有断言的风格）：
#   * 期望值来自**夹具清单**（生成夹具时独立枚举出的），实际值来自驱动脚本从**磁盘**枚举出的清单；
#     两边各算一遍，绝不采信产品报告里的数字（报告只用来对"它自己说了什么"）。
#   * 每条断言都必须能真的失败：self-test-extension.ps1 会喂"故意做错"的合成结果验证鉴别力。
#   * 性能护栏：每段打印 [assert <ms>]，单段超预算判失败，大循环有迭代上限，详情限流。

[CmdletBinding()]
param(
    [string]$ManifestPath = '',
    [string]$DraftRunJson = '',
    [string]$RawRunJson = '',
    [string]$RepeatDraftRunJson = '',
    [string]$RepeatInstallRunJson = '',
    [string]$Mode1RunJson = '',
    [string]$ClassicRunJson = '',
    # 'salt-mismatch=<json>;crc-error=<json>;truncated=<json>;version-high=<json>;missing-identity=<json>'
    [string]$BadArchiveRunJsonCsv = '',
    # 坏档案用例清单（run-extension-bad-archives.ps1 写出）：E-4 用它拿到每份坏档案的路径，
    # 自己用 lib\rules-int.py 独立解析来复核"这份档案真的是这一类的坏档案"（S-1 的教训）。
    [string]$BadArchiveSummaryJson = '',
    [string]$ChecksJson = '',
    # 幂等口径：
    #   auto（默认）= 字节一致最好；不一致时按"除 META 易变字段外逐 chunk 一致"判（实测 UI 会填 generatedAt）
    #   byte        = 必须逐字节一致
    #   chunks      = 同 auto（保留旧名，兼容）
    [ValidateSet('auto', 'byte', 'chunks')][string]$IdempotenceMode = 'auto',
    # 强制逐字节一致（与 -IdempotenceMode byte 等价，用来自检"严格口径也能炸"）
    [switch]$StrictBytes,
    # E-8：体积上界（同量级判据；§3.3 估 6 万条 ≈ 1.3 MB 未压缩 / ≈ 0.6 MB 压缩）
    [int]$MaxArchiveBytes = 8 * 1024 * 1024,
    [int]$LoopLimit = 200000,
    # E-11（像素幽灵）：run-tab-pixels.ps1 写出的抓图快照。缺失时按 warn 记录（不静默），
    # 正式流程（run-all.ps1 -Scope extension）会先跑驱动，所以这里一定存在。
    [string]$TabPixelsJson = '',
    [string]$TabPixelsDiffDir = '',
    # 自检/临时排查：显式跳过 E-11（会在结果里留一条 info，不伪装成"验证过"）
    [switch]$SkipTabPixels,
    # ---------- E-12..E-19：「扩展集」页签 + 在线更新（§8.5 / §14）----------
    # 这些用例的输出由 run-extension-online.ps1 统一编排（每个用例起一个本地夹具服务）
    [string]$OnlineSummaryJson = '',
    [string]$SetTabRunJson = '',
    [string]$SetEmptyRunJson = '',
    [string]$SetOneRunJson = '',
    [string]$SetMultiRunJson = '',
    # 'none=<json>;ok=<json>;missing=<json>'
    [string]$SetOpenRunJsonCsv = '',
    [string]$SetNoteRunJson = '',
    # E-14-b：名字会被改写的条目的浏览用例（编排里的 hostile-install + set-hostile）
    [string]$SetHostileRunJson = '',
    # 'clean=<json>;bad=<json>'
    [string]$SetCheckRunJsonCsv = '',
    # E-12 的像素口径：run-tab-pixels.ps1 抓的「扩展集页签」快照（缺失按 warn，不静默）
    [string]$SetPixelsJson = '',
    [string]$SetPixelsDiffDir = '',
    [switch]$SkipSetPixels
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')
. (Join-Path $root 'lib\HarnessNative.ps1')
. (Join-Path $root 'lib\HarnessPixels.ps1')

$runRoot = Join-Path $root 'Run'
$outRoot = Join-Path $runRoot 'out'
if (-not $ManifestPath) { $ManifestPath = Join-Path $runRoot 'extension-manifest.json' }
if (-not $DraftRunJson) { $DraftRunJson = Join-Path $outRoot 'extension-old.json' }
if (-not $RawRunJson) { $RawRunJson = Join-Path $outRoot 'extension-raw.json' }
if (-not $RepeatDraftRunJson) { $RepeatDraftRunJson = Join-Path $outRoot 'extension-old-repeat.json' }
if (-not $RepeatInstallRunJson) { $RepeatInstallRunJson = Join-Path $outRoot 'extension-old-install2.json' }
if (-not $Mode1RunJson) { $Mode1RunJson = Join-Path $outRoot 'extension-mode1.json' }
if (-not $ClassicRunJson) { $ClassicRunJson = Join-Path $outRoot 'extension-classic.json' }
if (-not $BadArchiveSummaryJson) { $BadArchiveSummaryJson = Join-Path $outRoot 'extension-bad-archives-summary.json' }
if (-not $ChecksJson) { $ChecksJson = Join-Path $outRoot 'checks-extension.json' }
if (-not $TabPixelsJson) { $TabPixelsJson = Join-Path $outRoot 'tab-pixels.json' }
if (-not $TabPixelsDiffDir) { $TabPixelsDiffDir = Join-Path $outRoot 'tab-pixels' }
if (-not $OnlineSummaryJson) { $OnlineSummaryJson = Join-Path $outRoot 'extension-online-summary.json' }
if (-not $SetTabRunJson) { $SetTabRunJson = Join-Path $outRoot 'extension-set-one.json' }
if (-not $SetEmptyRunJson) { $SetEmptyRunJson = Join-Path $outRoot 'extension-set-empty.json' }
if (-not $SetOneRunJson) { $SetOneRunJson = Join-Path $outRoot 'extension-set-one.json' }
if (-not $SetMultiRunJson) { $SetMultiRunJson = Join-Path $outRoot 'extension-set-multi.json' }
if (-not $SetOpenRunJsonCsv)
{
    $SetOpenRunJsonCsv = ('none={0};ok={1};missing={2}' -f
        (Join-Path $outRoot 'extension-set-open-none.json'),
        (Join-Path $outRoot 'extension-set-open-ok.json'),
        (Join-Path $outRoot 'extension-set-open-missing.json'))
}
if (-not $SetNoteRunJson) { $SetNoteRunJson = Join-Path $outRoot 'extension-set-note.json' }
if (-not $SetHostileRunJson) { $SetHostileRunJson = Join-Path $outRoot 'extension-set-hostile.json' }
if (-not $SetCheckRunJsonCsv)
{
    $SetCheckRunJsonCsv = ('clean={0};bad={1}' -f
        (Join-Path $outRoot 'extension-set-check-clean.json'),
        (Join-Path $outRoot 'extension-set-check-bad.json'))
}
# E-12-a 的像素快照**就是 E-11 那一份**（run-tab-pixels.ps1 一次抓完两页，
# set-* 那几张由它单独走一条进程抓）——口径相同、证据同源，横向对照才成立。
if (-not $SetPixelsJson) { $SetPixelsJson = $TabPixelsJson }
if (-not $SetPixelsDiffDir) { $SetPixelsDiffDir = $TabPixelsDiffDir }

# 独立解析档案（E-4 前置 / E-8 结构）：调脚手架自己的 lib\rules-int.py，
# 绝不调产品内部函数（黑盒纪律）。
$script:assertPython = ''
function Get-HarnessPythonForAssert
{
    if ($script:assertPython) { return $script:assertPython }
    foreach ($candidate in @('C:\Users\Administrator\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'))
    {
        if (Test-Path -LiteralPath $candidate) { $script:assertPython = $candidate; return $candidate }
    }
    $found = Get-Command python -ErrorAction SilentlyContinue
    if ($found) { $script:assertPython = $found.Source; return $script:assertPython }
    throw '找不到 Python 运行时（独立解析 rules.int 需要它）'
}

function Get-ArchiveDumpForAssertion([string]$Path, [string]$Tag)
{
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return $null }
    $python = Get-HarnessPythonForAssert
    $jsonPath = Join-Path $outRoot ('assert-archive-dump-{0}.json' -f $Tag)
    & $python (Join-Path $root 'lib\rules-int.py') dump --in $Path --json $jsonPath | Out-Null
    $dump = Read-JsonFile -Path $jsonPath
    if ($dump) { $dump | Add-Member -NotePropertyName 'dumpedFrom' -NotePropertyValue $Path -Force }
    return $dump
}

Reset-HarnessChecks

# ---------- 进度与预算（防"静默空转"）----------

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
function Assert-LoopBudget([string]$Label, [int]$Count, [int]$Limit = 0)
{
    if ($Limit -le 0) { $Limit = $LoopLimit }
    if ($Count -gt $Limit) { throw ("{0}：需要迭代 {1} 次，超过上限 {2} —— 拒绝空转，按失败处理" -f $Label, $Count, $Limit) }
    Trace-Step ("{0}：{1} 项（上限 {2}）" -f $Label, $Count, $Limit)
}
# 详情限流：命中上万时只列前 N 个，否则 checks JSON 会涨到几 MB
function Limit-Details([string[]]$Items, [int]$Max = 10)
{
    if ($Items.Count -le $Max) { return $Items }
    return @($Items | Select-Object -First $Max) + @(('… 另有 {0} 条同类，已省略（详情限流）' -f ($Items.Count - $Max)))
}

# ---------- 夹具清单 ----------

$manifest = Read-JsonFile -Path $ManifestPath
if (-not $manifest) { throw ("缺少扩展集夹具清单：{0}；请先运行 make-extension-fixture.ps1" -f $ManifestPath) }
$expectedFiles = @(Get-ManifestFiles -JsonPath $ManifestPath)
if ($expectedFiles.Count -eq 0) { throw ("夹具清单里没有逐条明细：{0}" -f $ManifestPath) }
$expectations = Get-ObjectValue -Object $manifest -Name 'expectations' -Default $null
$extensionSet = Get-ObjectValue -Object $manifest -Name 'extensionSet' -Default $null
$rawGame = Get-ObjectValue -Object $manifest -Name 'rawGame' -Default $null
$negativeName = [string](Get-ObjectValue -Object $manifest -Name 'negativeName' -Default '')
$restorableCategories = @(Get-ObjectValue -Object $expectations -Name 'restorableCategories' -Default @('restored', 'mined', 'manual', 'recovered'))
Write-Host ("[assert-extension] 夹具：{0} 条逐条明细；可还原类别 {1}" -f $expectedFiles.Count, ($restorableCategories -join '/'))

# 期望索引（全部由夹具独立枚举得出）
$expectedBySource = @{}
$expectedByOutput = @{}
$declaredNames = @{}
foreach ($item in $expectedFiles)
{
    $expectedBySource[[string]$item.sourceRel] = $item
    if ($item.expectedRel) { $expectedByOutput[[string]$item.expectedRel] = $item }
    if ($item.knownFile) { $declaredNames[[string]$item.knownFile] = $true }
    $leaf = Split-Path ([string]$item.sourceRel) -Leaf
    if ($leaf) { $declaredNames[$leaf] = $true }
}
$expectedRestorable = @($expectedFiles | Where-Object { $_.category -in $restorableCategories })
$expectedMissing = @($expectedFiles | Where-Object { $_.category -like 'missing-*' })
$expectedPlain = @($expectedFiles | Where-Object { $_.category -eq 'plain' })
$expectedSkipped = @($expectedFiles | Where-Object { $_.category -eq 'skipped-log' })

# ---------- 运行结果读取 ----------

function Get-RunBundle([string]$Path, [string]$Label)
{
    $bundle = [pscustomobject]@{
        Label = $Label; Path = $Path; Exists = $false; Record = $null; Output = @(); Failure = ''
        Overview = ''; SetStatus = ''; TaskText = ''
    }
    if (-not $Path -or -not (Test-Path -LiteralPath $Path))
    {
        return $bundle
    }
    $record = Read-JsonFile -Path $Path
    if (-not $record) { return $bundle }

    $outputList = New-Object System.Collections.ArrayList
    foreach ($item in @(Get-ObjectValue -Object $record -Name 'output' -Default @())) { [void]$outputList.Add($item) }
    $tsv = [string](Get-ObjectValue -Object $record -Name 'outputTsv' -Default '')
    if ($outputList.Count -eq 0 -and $tsv -and (Test-Path -LiteralPath $tsv))
    {
        foreach ($row in @(Read-TsvFile -Path $tsv))
        {
            if ($row.Count -lt 3) { continue }
            [void]$outputList.Add([pscustomobject]@{ rel = $row[0]; size = [long]$row[1]; sha256 = $row[2] })
        }
    }
    # 驱动脚本自己也会把输出目录再枚举一遍（diskOutput）；两份都在时以磁盘那份为准并交叉核对
    $diskOutput = @(Get-ObjectValue -Object $record -Name 'diskOutput' -Default @())
    $overview = [string](Get-ObjectValue -Object $record -Name 'overviewText' -Default '')
    $setStatus = [string](Get-ObjectValue -Object $record -Name 'setStatusText' -Default '')
    $taskText = [string](Get-ObjectValue -Object $record -Name 'taskText' -Default '')
    $stepResults = Get-ObjectValue -Object $record -Name 'stepResults' -Default $null
    if ($stepResults)
    {
        foreach ($name in @('step1', 'step3', 'draft', 'dryrun', 'install'))
        {
            $section = Get-ObjectValue -Object $stepResults -Name $name -Default $null
            if ($section)
            {
                $overview += "`r`n" + [string](Get-ObjectValue -Object $section -Name 'overviewText' -Default '')
                $setStatus += "`r`n" + [string](Get-ObjectValue -Object $section -Name 'setStatusText' -Default '')
                $taskText += "`r`n" + [string](Get-ObjectValue -Object $section -Name 'taskText' -Default '')
            }
        }
    }
    return [pscustomobject]@{
        Label = $Label; Path = $Path; Exists = $true; Record = $record
        Output = $outputList.ToArray(); DiskOutput = $diskOutput
        Failure = [string](Get-ObjectValue -Object $record -Name 'failure' -Default '')
        Overview = $overview; SetStatus = $setStatus; TaskText = $taskText
    }
}

function Get-EvidenceText($Run)
{
    $parts = New-Object System.Collections.ArrayList
    if ($Run.Failure) { [void]$parts.Add($Run.Failure) }
    [void]$parts.Add([string]$Run.Overview)
    [void]$parts.Add([string]$Run.SetStatus)
    [void]$parts.Add([string]$Run.TaskText)
    if ($Run.Exists)
    {
        foreach ($dialog in @(Get-ObjectValue -Object $Run.Record -Name 'modalDialogs' -Default @()))
        {
            [void]$parts.Add(('{0} {1}' -f [string]$dialog.Title, [string]$dialog.Text))
        }
        $stepResults = Get-ObjectValue -Object $Run.Record -Name 'stepResults' -Default $null
        if ($stepResults)
        {
            foreach ($property in $stepResults.PSObject.Properties)
            {
                $section = $property.Value
                if ($section) { [void]$parts.Add([string](Get-ObjectValue -Object $section -Name 'reason' -Default '')) }
            }
        }
        foreach ($log in @(Get-ObjectValue -Object $Run.Record -Name 'toolLogs' -Default @()))
        {
            [void]$parts.Add([string](Get-ObjectValue -Object $log -Name 'Text' -Default ''))
        }
    }
    return ($parts -join "`r`n")
}

# 干跑数字的解析：产品可能写成"一行一项"，也可能写成
#   `可解目录数 5 / 可解文件数 15 / 完整路径可还原数 12 / 成功率 80.00%`
# 既有的 Get-ReportNumberMap 是**按行**只取一对 label/number，一行多项时会漏掉后面几项，
# 于是明明有那一项却报"找不到"。这里整段扫所有 `label number` 对（不限行、可带 %）。
function Get-NumberPairs([string]$Text)
{
    $pairs = [ordered]@{}
    foreach ($match in [regex]::Matches($Text, '(?<label>[^\d\r\n]{1,40}?)\s*[:：=]?\s*(?<num>\d[\d,]*)'))
    {
        $label = ($match.Groups['label'].Value -replace '[\s/|]+$', '').Trim()
        if (-not $label) { continue }
        if ($pairs.Contains($label)) { continue }
        $pairs[$label] = [int]($match.Groups['num'].Value -replace ',', '')
    }
    return $pairs
}

function Find-NumberPair($Pairs, [string[]]$Keywords)
{
    foreach ($label in $Pairs.Keys)
    {
        foreach ($keyword in $Keywords)
        {
            if ($label.Contains($keyword)) { return [pscustomobject]@{ Label = $label; Value = $Pairs[$label] } }
        }
    }
    return $null
}

function Get-OutputIndex($Run)
{
    $index = @{}
    foreach ($item in $Run.Output) { $index[[string]$item.rel] = $item }
    return $index
}

# 路径口径统一：档案里的目录名是 `characters/yuki/`（正斜杠，§3 的规范化形态），
# 但还原到磁盘后是 `characters\yuki\`。夹具的期望值也统一成 Windows 形态再比，
# 否则"文件明明在，只是分隔符不同"会被判成"缺少期望文件"（踩过）。
function Get-CanonicalRel([string]$Rel)
{
    if (-not $Rel) { return $Rel }
    return ($Rel -replace '/', '\')
}

# 独立枚举"干跑应当报出的数字"：StaticHash.lst（= 档案展开物）× 源目录（磁盘上的纯哈希名目录）。
# 定义（与 §5 的四个输出项对应；口径来自 lead 同步的 task-1 事实 #4）：
#   **只有「最后两段 = 16 位目录哈希 + 64 位文件哈希」才算哈希名文件**；
#   包根下裸的 64 位哈希文件不算（按"原样搬运"处理），不进入分母。
#   Dirs      = 源目录里出现、且档案里有的 16 位目录段数量
#   Files     = 源目录里出现、且档案里有的 64 位文件哈希数量
#   FullPaths = 目录段与文件名**同时**可解的哈希名文件数
#   Total     = 源目录里的哈希名文件总数（成功率的分母）
#   Percent   = FullPaths / Total * 100
function Measure-DryRunExpectation([string]$LstPath, [string]$SourceDir)
{
    $dirHashes = @{}
    $fileHashes = @{}
    $text = Read-TextAutoShared -Path $LstPath -MaxBytes 8000000
    $rows = @($text -split "`r?`n" | Where-Object { $_.Trim() })
    Assert-LoopBudget -Label ('StaticHash.lst 条目（{0}）' -f (Split-Path $LstPath -Leaf)) -Count $rows.Count
    foreach ($row in $rows)
    {
        $parts = $row -split ':', 2
        if ($parts.Count -lt 2) { continue }
        $hash = $parts[0].Trim().ToUpperInvariant()
        if ($hash.Length -eq 16) { $dirHashes[$hash] = $parts[1] }
        elseif ($hash.Length -eq 64) { $fileHashes[$hash] = $parts[1] }
    }

    $dirSeen = @{}
    $hitFileHashes = @{}
    $fileHitEntries = 0
    $fullPaths = 0
    $total = 0
    $bareHashFiles = 0
    $entries = if ('Harness.Batch' -as [type]) { [Harness.Batch]::Rows($SourceDir) } else { @() }
    Assert-LoopBudget -Label ('源目录文件（{0}）' -f (Split-Path $SourceDir -Leaf)) -Count $entries.Count
    foreach ($entry in $entries)
    {
        $rel = [string](($entry -split '\|')[0])
        $leaf = Split-Path $rel -Leaf
        if ($leaf -notmatch '^[0-9A-Fa-f]{64}$') { continue }
        $parent = Split-Path $rel -Parent
        $dirHash = ''
        if ($parent)
        {
            $parentLeaf = Split-Path $parent -Leaf
            if ($parentLeaf -match '^[0-9A-Fa-f]{16}$') { $dirHash = $parentLeaf.ToUpperInvariant() }
        }
        # 包根下裸的 64 位哈希文件：不算哈希名文件（原样搬运），既不计分母也不计可解
        if (-not $dirHash) { $bareHashFiles++; continue }
        $total++
        $fileHash = $leaf.ToUpperInvariant()
        $fileOk = $fileHashes.ContainsKey($fileHash)
        if ($fileOk) { $fileHitEntries++; $hitFileHashes[$fileHash] = $true }
        if ($dirHashes.ContainsKey($dirHash)) { $dirSeen[$dirHash] = $true }
        if ($fileOk -and $dirHashes.ContainsKey($dirHash)) { $fullPaths++ }
    }
    return [pscustomobject]@{
        # ① 目录：按 16 位哈希去重
        Dirs = $dirSeen.Count
        # ② 文件名可解：按 **64 位文件哈希去重**（§5 / 内核 ExtensionSet.cpp:2879 的 hitFiles 是 set）
        Files = $hitFileHashes.Count
        # ② 的条目口径（同一个哈希在两个目录下出现两次时会计两次）—— 只作参考，别拿它当 ②
        FileEntriesHit = $fileHitEntries
        # ③ 整路径可还原：按**条目**（内核 ++result.fullyResolvedFiles）
        FullPaths = $fullPaths
        Total = $total
        BareHashFiles = $bareHashFiles
        Percent = $(if ($total -gt 0) { [Math]::Round($fullPaths * 100.0 / $total, 2) } else { 0 })
        DirEntries = $dirHashes.Count; FileEntries = $fileHashes.Count; SourceDir = $SourceDir; Lst = $LstPath
    }
}

$runDraft = Get-RunBundle -Path $DraftRunJson -Label '生成（旧作）'
$runRaw = Get-RunBundle -Path $RawRunJson -Label '生成（未归一化采集日志，E-2）'
$runRepeatDraft = Get-RunBundle -Path $RepeatDraftRunJson -Label '再生成一次（E-7）'
$runRepeatInstall = Get-RunBundle -Path $RepeatInstallRunJson -Label '再安装一次（E-7）'
$runMode1 = Get-RunBundle -Path $Mode1RunJson -Label '模式1'
$runClassic = Get-RunBundle -Path $ClassicRunJson -Label '传统动态模式'

Write-Host ''
Write-Host '=== 断言 E（扩展集一期） ==='
foreach ($run in @($runDraft, $runRaw, $runRepeatDraft, $runRepeatInstall, $runMode1, $runClassic))
{
    if ($run.Exists)
    {
        Write-Host ("[assert-extension] {0}：{1}（输出 {2} 条；失败='{3}'）" -f $run.Label, (Split-Path $run.Path -Leaf), $run.Output.Count, $run.Failure)
    }
    else
    {
        Write-Host ("[assert-extension] {0}：{1} —— 结果不存在" -f $run.Label, $run.Path)
    }
}

# ==================================================================
# E-1 闭环免跑游戏：清空输出 → 只用扩展集跑模式1，结果与"有原始映射直接跑"逐条一致
# ==================================================================

Trace-Step 'E-1：模式1 与"原始映射直接跑"逐条一致（独立枚举对账）'
$e1Details = @()
$e1Ok = $true

function Compare-OutputSets($RunA, $RunB, [string]$LabelA, [string]$LabelB, [ref]$Details)
{
    # 还原报告不是"资源"：它的正文天然随映射来源/时间/统计变化（模式1 与动态模式必然不同），
    # 参与逐条对账会制造假失败。这里把它剔除，并在详情里如实记下"报告存在且不同"。
    function Get-ResourceIndex($Run)
    {
        $index = @{}
        foreach ($item in $Run.Output)
        {
            $rel = Get-CanonicalRel ([string]$item.rel)
            if ($rel -notmatch '\\' -and $rel -match '^RestoreReport') { continue }
            $index[$rel] = $item
        }
        return $index
    }
    function Get-ReportRels($Run)
    {
        return @($Run.Output | Where-Object { ([string]$_.rel) -notmatch '\\' -and ([string]$_.rel) -match '^RestoreReport' } | ForEach-Object { [string]$_.rel })
    }
    $indexA = Get-ResourceIndex $RunA
    $indexB = Get-ResourceIndex $RunB
    # 注意：函数返回单个元素时会被解包成标量，标量在 StrictMode 下没有 .Count —— 调用方一律 @() 包一层
    $reportA = @(Get-ReportRels $RunA)
    $reportB = @(Get-ReportRels $RunB)
    Assert-LoopBudget -Label ("{0} 输出条数" -f $LabelA) -Count ($indexA.Count + $indexB.Count)

    $onlyA = @($indexA.Keys | Where-Object { -not $indexB.ContainsKey($_) } | Sort-Object)
    $onlyB = @($indexB.Keys | Where-Object { -not $indexA.ContainsKey($_) } | Sort-Object)
    $diffContent = @()
    foreach ($key in $indexA.Keys)
    {
        if (-not $indexB.ContainsKey($key)) { continue }
        if ([string]$indexA[$key].sha256 -ne [string]$indexB[$key].sha256) { $diffContent += $key }
    }
    $ok = ($onlyA.Count -eq 0 -and $onlyB.Count -eq 0 -and $diffContent.Count -eq 0)
    $Details.Value += ("{0} {1} 个 / {2} {3} 个（已剔除还原报告）；只在 {0}：{4}；只在 {2}：{5}；内容不同：{6}" -f `
            $LabelA, $indexA.Count, $LabelB, $indexB.Count, $onlyA.Count, $onlyB.Count, $diffContent.Count)
    $Details.Value += ("还原报告：{0} {1} / {2} {3}（正文不参与逐条对账）" -f $LabelA, $(if ($reportA.Count) { ($reportA -join ',') } else { '<无>' }), $LabelB, $(if ($reportB.Count) { ($reportB -join ',') } else { '<无>' }))
    if ($onlyA.Count -gt 0) { $Details.Value += @('  只在 ' + $LabelA + '：') + (Limit-Details $onlyA) }
    if ($onlyB.Count -gt 0) { $Details.Value += @('  只在 ' + $LabelB + '：') + (Limit-Details $onlyB) }
    if ($diffContent.Count -gt 0) { $Details.Value += @('  内容不同：') + (Limit-Details $diffContent) }
    return [pscustomobject]@{ Ok = $ok; OnlyA = $onlyA; OnlyB = $onlyB; DiffContent = $diffContent }
}

if (-not $runMode1.Exists -or -not $runClassic.Exists)
{
    $e1Ok = $false
    $e1Details += ("用例结果缺失：模式1='{0}'（存在={1}） 传统模式='{2}'（存在={3}）" -f $Mode1RunJson, $runMode1.Exists, $ClassicRunJson, $runClassic.Exists)
}
else
{
    # ① 模式1 ≡ 传统动态模式（同一份预置解包目录）
    $compare = Compare-OutputSets $runMode1 $runClassic '模式1' '传统动态模式' ([ref]$e1Details)
    if (-not $compare.Ok) { $e1Ok = $false }

    # ①b 输入等价的**硬证据**：两个工作区的 Extractor_Output 内容摘要（不含 mtime）必须相同。
    #     否则"0 差异"可能只是因为"输入本来就不同"，读者没法排除这种解释。
    $contentMode1 = Get-ObjectValue -Object $runMode1.Record -Name 'extractorContentDigest' -Default $null
    $contentClassic = Get-ObjectValue -Object $runClassic.Record -Name 'extractorContentDigest' -Default $null
    $pathMode1 = [string](Get-ObjectValue -Object $runMode1.Record -Name 'extractorOutputPath' -Default '')
    $pathClassic = [string](Get-ObjectValue -Object $runClassic.Record -Name 'extractorOutputPath' -Default '')
    if ($contentMode1 -and $contentClassic)
    {
        $sameInput = ([int]$contentMode1.Count -eq [int]$contentClassic.Count) -and ([string]$contentMode1.Digest -eq [string]$contentClassic.Digest)
        $e1Details += ("输入等价证据（内容摘要，不含 mtime）：模式1 {0} 个文件 digest {1} @ {2}；传统模式 {3} 个文件 digest {4} @ {5}；相同={6}" -f `
                $contentMode1.Count, ([string]$contentMode1.Digest).Substring(0, 16), $pathMode1,
            $contentClassic.Count, ([string]$contentClassic.Digest).Substring(0, 16), $pathClassic, $sameInput)
        if (-not $sameInput)
        {
            $e1Ok = $false
            $e1Details += '[FAIL] 两个模式的解包目录内容不一致 —— "逐条一致"就没有意义了（先证明输入等价）'
        }
    }
    else
    {
        $e1Ok = $false
        $e1Details += '[FAIL] 用例结果里没有 extractorContentDigest（输入等价无法证明）—— 请用新版 run-extension-ui.ps1 重跑'
    }
    # 枚举对象自证（照 VERIFICATION.md 的做法：把"我到底枚举了哪些东西"写进详情）
    $e1Details += ("枚举对象：模式1 输出清单 {0}；传统模式输出清单 {1}；期望值清单 {2}" -f $Mode1RunJson, $ClassicRunJson, $ManifestPath)

    # ② 模式1 的输出必须与夹具期望逐条一致（可还原的都在期望路径、内容同源）
    #    索引键统一成 Windows 分隔符：档案里的目录名是 `characters/yuki/`，
    #    落盘后是 `characters\yuki\`（这一步不做，会出现"文件明明在却报缺少"的假失败）。
    $mode1Index = @{}
    foreach ($item in $runMode1.Output) { $mode1Index[(Get-CanonicalRel ([string]$item.rel))] = $item }
    Assert-LoopBudget -Label 'E-1 期望可还原条数' -Count $expectedRestorable.Count
    $missingExpected = @()
    $wrongContent = @()
    foreach ($item in $expectedRestorable)
    {
        $rel = Get-CanonicalRel ([string]$item.expectedRel)
        if (-not $mode1Index.ContainsKey($rel)) { $missingExpected += $rel; continue }
        if ([string]$mode1Index[$rel].sha256 -ne [string]$item.sha256) { $wrongContent += $rel }
    }
    # ③ 不可还原的绝不能出现（默认语义）。
    #    哈希从 sourceRel 现算（pkg\<16位目录哈希>\<64位文件哈希>），不依赖清单有没有额外列。
    $leakedMissing = @()
    foreach ($item in $expectedMissing)
    {
        $parts = ([string]$item.sourceRel) -split '\\'
        $fileHash = $parts[$parts.Count - 1]
        $dirHash = if ($parts.Count -ge 3) { $parts[1] } else { '' }
        foreach ($key in $mode1Index.Keys)
        {
            if ($key -like ('*' + $fileHash)) { $leakedMissing += $key; break }
            if ($dirHash -and $key -like ('*' + $dirHash + '*')) { $leakedMissing += $key; break }
        }
    }
    # ④ 明文名必须还在
    $missingPlain = @()
    foreach ($item in $expectedPlain)
    {
        $rel = Get-CanonicalRel ([string]$item.expectedRel)
        if (-not $mode1Index.ContainsKey($rel)) { $missingPlain += $rel }
    }
    # ⑤ 工具日志必须被跳过
    $skippedLeak = @()
    foreach ($item in $expectedSkipped)
    {
        $rel = Get-CanonicalRel ([string]$item.sourceRel)
        if ($mode1Index.ContainsKey($rel)) { $skippedLeak += $rel }
    }

    if ($missingExpected.Count -gt 0) { $e1Ok = $false; $e1Details += @('模式1 输出里缺少期望的可还原文件：') + (Limit-Details $missingExpected) }
    if ($wrongContent.Count -gt 0) { $e1Ok = $false; $e1Details += @('模式1 输出的内容与源文件不一致：') + (Limit-Details $wrongContent) }
    if ($leakedMissing.Count -gt 0) { $e1Ok = $false; $e1Details += @('不可还原的文件居然出现在输出里：') + (Limit-Details $leakedMissing) }
    if ($missingPlain.Count -gt 0) { $e1Ok = $false; $e1Details += @('明文名文件被丢掉了（丢得太多）：') + (Limit-Details $missingPlain) }
    if ($skippedLeak.Count -gt 0) { $e1Ok = $false; $e1Details += @('工具日志目录的文件被复制进输出：') + (Limit-Details $skippedLeak) }
    $e1Details += ("期望可还原 {0} 个 / 不可还原 {1} 个 / 明文 {2} 个 / 跳过日志 {3} 个" -f `
            $expectedRestorable.Count, $expectedMissing.Count, $expectedPlain.Count, $expectedSkipped.Count)

    # ⑥ "免跑游戏"的硬证据：模式1 工作区没有盐、没有动态采集成果，且确实用了 StaticHash_Output
    $mode1Workspace = [string](Get-ObjectValue -Object $runMode1.Record -Name 'workspace' -Default '')
    $saltCandidates = @(
        (Join-Path $mode1Workspace 'Log\Universal.log'),
        (Join-Path $mode1Workspace 'Universal.log')
    )
    $saltPresent = @($saltCandidates | Where-Object { Test-Path -LiteralPath $_ })
    $dynamicDir = Join-Path $mode1Workspace 'StringHashDumper_Output'
    $dynamicCount = if (Test-Path -LiteralPath $dynamicDir) { Get-FastFileCount -Path $dynamicDir } else { 0 }
    $staticLst = [string](Get-ObjectValue -Object $runMode1.Record -Name 'staticHashLst' -Default '')
    $staticLines = [int](Get-ObjectValue -Object $runMode1.Record -Name 'staticHashLineCount' -Default 0)
    if ($saltPresent.Count -gt 0) { $e1Ok = $false; $e1Details += @('模式1 工作区里居然有盐文件（免跑游戏就不该需要它）：') + $saltPresent }
    if ($dynamicCount -gt 0) { $e1Ok = $false; $e1Details += ('模式1 工作区的 StringHashDumper_Output 里有 {0} 个文件（不该依赖动态采集成果）' -f $dynamicCount) }
    if (-not $staticLst -or -not (Test-Path -LiteralPath $staticLst)) { $e1Ok = $false; $e1Details += ('模式1 没有产出 StaticHash_Output\StaticHash.lst（记录里是 {0}）' -f $staticLst) }
    elseif ($staticLines -le 0) { $e1Ok = $false; $e1Details += 'StaticHash.lst 是空的' }
    $e1Details += ("免跑游戏证据：模式1 工作区盐文件 {0} 个 / 动态采集成果 {1} 个文件 / StaticHash.lst {2} 行" -f $saltPresent.Count, $dynamicCount, $staticLines)
    $e1Details += ("步骤2 口径：{0}" -f ((@(Get-ObjectValue -Object $runMode1.Record -Name 'notes' -Default @()) | Where-Object { $_ -like '*步骤2*' }) -join ' | '))
}
Add-Check -Id 'E-1' -Title 'E-1 闭环免跑游戏：仅用扩展集跑模式1，结果与"原始映射直接跑"逐条一致' -Ok $e1Ok -Details $e1Details

# ==================================================================
# E-2 生成期自校验：故意写错归一化 → 例外数必须大面积报出
# ==================================================================

Trace-Step 'E-2：未归一化采集日志必须被生成期自校验大面积报成例外'
$e2Details = @()
$e2Ok = $true
$expectedExceptions = [int](Get-ObjectValue -Object $rawGame -Name 'expectExceptionsAtLeast' -Default 0)
$rawDirEntries = [int](Get-ObjectValue -Object $rawGame -Name 'unnormalizedDirCount' -Default 0)

if (-not $runRaw.Exists)
{
    $e2Ok = $false
    $e2Details += ("E-2 用例结果不存在：{0}（先跑 run-extension-ui.ps1 -Game raw -Step meta,draft）" -f $RawRunJson)
}
else
{
    $dump = Get-ObjectValue -Object $runRaw.Record -Name 'archiveDump' -Default $null
    $exceptions = @()
    if ($dump) { $exceptions = @(Get-ObjectValue -Object (Get-ObjectValue -Object $dump -Name 'parsed' -Default $null) -Name 'exceptions' -Default @()) }
    $evidence = Get-EvidenceText $runRaw
    $reported = ($evidence -match '例外' -or $evidence -match '重算' -or $evidence -match '不一致')
    $e2Details += ("夹具：未归一化的目录条目 {0} 条（期望例外 ≥ {1}）；档案里 EXCEPTIONS {2} 条；日志/概览里提到例外={3}" -f `
            $rawDirEntries, $expectedExceptions, $exceptions.Count, $reported)
    if ($exceptions.Count -lt $expectedExceptions)
    {
        $e2Ok = $false
        $e2Details += ("[FAIL] 例外数 {0} < 期望 {1} —— 生成期自校验没有把「归一化口径不一致」报出来（这正是 §3.2 要防的静默失效）" -f $exceptions.Count, $expectedExceptions)
    }
    if (-not $reported)
    {
        $e2Ok = $false
        $e2Details += '[FAIL] 生成结束后的日志/概览里没有任何「例外/重算不一致」的报数 —— 必须如实报数'
    }
    if ($exceptions.Count -gt 0) { $e2Details += @('例外样例：') + (Limit-Details @($exceptions | ForEach-Object { [string]$_ }) 5) }

    # 对照组：正常夹具只应报出**故意留的那 1 条**例外（same.png 的伪造哈希）。
    # 多出来任何一条都说明"夹具哈希与实现的重算口径不一致"，必须查清楚 —— 这条正是
    # 夹具自己踩过的坑（PowerShell 大小写不敏感把 hash('UI') 写成了 hash('ui/')）。
    $goodDump = Get-ObjectValue -Object $runDraft.Record -Name 'archiveDump' -Default $null
    if ($goodDump)
    {
        $goodExceptions = @(Get-ObjectValue -Object (Get-ObjectValue -Object $goodDump -Name 'parsed' -Default $null) -Name 'exceptions' -Default @())
        $expectedGood = @(Get-ObjectValue -Object $extensionSet -Name 'expectedExceptions' -Default @())
        $goodUnexpected = @($goodExceptions | Where-Object { [string]$_ -notin @($expectedGood | ForEach-Object { [string]$_ }) })
        $e2Details += ("对照组（正常夹具）EXCEPTIONS {0} 条（期望 {1} 条；正式口径 0，-PlantFakeDup 演示 1）" -f $goodExceptions.Count, $expectedGood.Count)
        if ($goodUnexpected.Count -gt 0)
        {
            $e2Ok = $false
            $e2Details += @('[FAIL] 正常夹具报了**没埋过**的例外 —— 要么夹具哈希错了，要么实现的重算口径与产品不一致：') + (Limit-Details @($goodUnexpected | ForEach-Object { [string]$_ }) 5)
        }
    }
    else { $e2Details += '（没有正常夹具的档案解析结果，无法做对照组）' }
}
Add-Check -Id 'E-2' -Title 'E-2 生成期自校验：写错归一化必须大面积报例外' -Ok $e2Ok -Details $e2Details

# ==================================================================
# E-3 干跑 == 实测
# ==================================================================

Trace-Step 'E-3：干跑成功率与真实步骤3 结果一致'
$e3Details = @()
$e3Ok = $true
$dryText = ''
if ($runDraft.Exists)
{
    $stepResults = Get-ObjectValue -Object $runDraft.Record -Name 'stepResults' -Default $null
    $drySection = if ($stepResults) { Get-ObjectValue -Object $stepResults -Name 'dryrun' -Default $null } else { $null }
    if ($drySection)
    {
        $dryText = ([string](Get-ObjectValue -Object $drySection -Name 'setStatusText' -Default '')) + "`r`n" + ([string](Get-ObjectValue -Object $drySection -Name 'overviewText' -Default ''))
    }
}
if (-not $dryText.Trim())
{
    $e3Ok = $false
    $e3Details += ("E-3 用例结果里没有干跑文本（先跑 run-extension-ui.ps1 -Step dryrun）：{0}" -f $DraftRunJson)
}
else
{
    # 干跑报的数字（产品侧）
    $numberMap = Get-NumberPairs -Text $dryText
    $reportedFull = Find-NumberPair $numberMap @('完整路径', '整路径', '完整可还原', '可还原数')
    $reportedDirs = Find-NumberPair $numberMap @('可解目录', '目录可解')
    # 面板四行口径（task-2 已改）：① 可解目录（去重）/ ② 文件名可解（去重）/ ③ 整路径可还原 / 成功率
    $reportedFiles = Find-NumberPair $numberMap @('文件名可解', '可解文件', '文件可解')
    $reportedPct = $null
    $pctMatch = [regex]::Match($dryText, '成功率[^0-9]{0,8}([0-9]+(?:\.[0-9]+)?)\s*%')
    if ($pctMatch.Success) { $reportedPct = [double]$pctMatch.Groups[1].Value }

    # 干跑对象是"测试来源目录"（默认 = 来源目录 = Restored_Extractor_Output），
    # 实现也可能直接测 Extractor_Output —— 所以把候选目录各算一遍，
    # 取与产品报数**一致**的那个（选中的候选写进详情自证，避免"口径不同"被误判成缺陷）。
    # StaticHash.lst：优先用驱动留的**不可变快照**（后续用例会清掉现场目录）
    $lstPath = [string](Get-ObjectValue -Object $runMode1.Record -Name 'staticHashLstSnapshot' -Default '')
    if (-not $lstPath) { $lstPath = [string](Get-ObjectValue -Object $runDraft.Record -Name 'staticHashLstSnapshot' -Default '') }
    if (-not $lstPath) { $lstPath = [string](Get-ObjectValue -Object $runDraft.Record -Name 'staticHashLst' -Default '') }
    if (-not $lstPath) { $lstPath = [string](Get-ObjectValue -Object $runMode1.Record -Name 'staticHashLst' -Default '') }
    $newUnpackedRoot = [string](Get-ObjectValue -Object (Get-ObjectValue -Object $manifest -Name 'newUnpacked' -Default $null) -Name 'root' -Default '')
    $oldGame = Get-ObjectValue -Object $manifest -Name 'oldGame' -Default $null
    $mode1Game = Get-ObjectValue -Object $manifest -Name 'mode1Game' -Default $null
    $candidateDirs = @()
    foreach ($dir in @(
            $newUnpackedRoot,
            (Join-Path ([string](Get-ObjectValue -Object $mode1Game -Name 'workspace' -Default '')) 'Extractor_Output'),
            [string](Get-ObjectValue -Object $oldGame -Name 'restoredDir' -Default ''),
            [string](Get-ObjectValue -Object $oldGame -Name 'sourceDir' -Default '')
        ))
    {
        if (-not $dir -or -not (Test-Path -LiteralPath $dir)) { continue }
        if ($candidateDirs -contains $dir) { continue }
        $candidateDirs += $dir
    }

    $measured = @()
    if ($lstPath -and (Test-Path -LiteralPath $lstPath))
    {
        foreach ($dir in $candidateDirs)
        {
            $result = Measure-DryRunExpectation -LstPath $lstPath -SourceDir $dir
            $measured += $result
            # 口径（§5 / 内核 ExtensionSet.cpp:2879）：② 按 **64 位文件哈希去重**，
            # ③ 按条目；条目命中数只作参考（S-3：之前拿条目数当 ②，才出现 13 vs 12 的分歧）。
            $e3Details += ("独立枚举 [{0}]：① 可解目录 {1}（去重）/ ② 文件名可解 {2}（哈希去重，条目命中 {3}）/ ③ 整路径可还原 {4} / 哈希名总数 {5}（{6:N2}%）" -f `
                    (Split-Path $dir -Leaf), $result.Dirs, $result.Files, $result.FileEntriesHit, $result.FullPaths, $result.Total, $result.Percent)
        }
    }
    else
    {
        $e3Details += ("无法独立枚举：StaticHash.lst='{0}'（不存在或没记录）" -f $lstPath)
    }

    $independent = $null
    if ($reportedFull)
    {
        $independent = @($measured | Where-Object { $_.FullPaths -eq $reportedFull.Value }) | Select-Object -First 1
        $e3Details += ("干跑报的「{0}」= {1}；匹配到的候选目录：{2}" -f $reportedFull.Label, $reportedFull.Value, $(if ($independent) { $independent.SourceDir } else { '<没有一个候选对得上>' }))
    }
    else
    {
        $e3Details += '[FAIL] 干跑文本里找不到「整路径可还原」这一项（§5 要求有）'
        if ($measured.Count -eq 1) { $independent = $measured[0] }
    }
    if ($reportedDirs) { $e3Details += ("干跑报的 ① 可解目录数 = {0}" -f $reportedDirs.Value) }
    if ($reportedFiles) { $e3Details += ("干跑报的 ② 文件名可解数 = {0}" -f $reportedFiles.Value) }
    if ($null -ne $reportedPct) { $e3Details += ("干跑报的成功率 = {0}%" -f $reportedPct) }
    else { $e3Details += '[FAIL] 干跑文本里找不到成功率百分比' }

    # 面板的"测试来源目录 + 扫描数"自证（S-5）：干跑对象必须是那份纯哈希名目录，不能是 0/0/0。
    # 两种写法都认：新面板 `哈希名文件：15 / 扫描文件总数：19`，旧面板 `测试来源目录文件总数 19（哈希名 15 …）`。
    $sourcePathMatch = [regex]::Match($dryText, '测试来源目录[：:]\s*(?<path>[^\r\n]+)')
    if ($sourcePathMatch.Success) { $e3Details += ("面板的测试来源目录：{0}" -f $sourcePathMatch.Groups['path'].Value.Trim()) }
    $scanMatch = [regex]::Match($dryText, '哈希名文件\s*[：:]\s*(?<hash>[0-9][0-9,]*)\s*/\s*扫描文件总数\s*[：:]\s*(?<total>[0-9][0-9,]*)')
    if (-not $scanMatch.Success)
    {
        $scanMatch = [regex]::Match($dryText, '测试来源目录文件总数\s*(?<total>[0-9][0-9,]*)\s*（哈希名\s*(?<hash>[0-9][0-9,]*)')
    }
    if ($scanMatch.Success)
    {
        $panelTotal = [int]($scanMatch.Groups['total'].Value -replace ',', '')
        $panelHash = [int]($scanMatch.Groups['hash'].Value -replace ',', '')
        $e3Details += ("面板扫描数：文件总数 {0} / 哈希名 {1}" -f $panelTotal, $panelHash)
        if ($panelHash -le 0)
        {
            $e3Ok = $false
            $e3Details += '[FAIL] 面板显示干跑扫到的哈希名文件为 0 —— 干跑对象被改过了（S-5：默认来源目录 = 上一轮步骤3 的输出目录）'
        }
        if ($independent -and $panelHash -ne $independent.Total)
        {
            $e3Ok = $false
            $e3Details += ("[FAIL] 面板的哈希名文件数 {0} != 独立枚举 {1}（干跑对象与脚手架枚举的目录不是同一份）" -f $panelHash, $independent.Total)
        }
    }
    else { $e3Details += '（干跑文本里没有「测试来源目录文件总数（哈希名 …）」这一行，无法核对扫描对象）' }

    # 真实步骤3 的结果（模式1）
    $actualRestored = 0
    if ($runMode1.Exists)
    {
        $mode1Canon = @{}
        foreach ($item in $runMode1.Output) { $mode1Canon[(Get-CanonicalRel ([string]$item.rel))] = $item }
        foreach ($item in $expectedRestorable) { if ($mode1Canon.ContainsKey((Get-CanonicalRel ([string]$item.expectedRel)))) { $actualRestored++ } }
    }
    $e3Details += ("真实步骤3（模式1）实际还原 {0} 个（夹具期望可还原 {1} 个）" -f $actualRestored, $expectedRestorable.Count)
    # ② 对账（S-3）：按 §5/内核口径（64 位哈希去重）与面板比对；不等才提示（lead 裁决：② 只记录不判失败）
    if ($independent -and $reportedFiles)
    {
        if ($reportedFiles.Value -ne $independent.Files)
        {
            $e3Details += ("[注意-口径] 面板的 ② 文件名可解 = {0}，按 64 位哈希去重的独立枚举 = {1}（条目命中 {2}，③ {3}）—— 请核对面板是否按内核口径显示" -f `
                    $reportedFiles.Value, $independent.Files, $independent.FileEntriesHit, $independent.FullPaths)
        }
        else
        {
            $e3Details += ("[OK] 产品的「可解文件」= {0} 与②「文件名可解（去重）」独立枚举一致" -f $reportedFiles.Value)
        }
    }
    $e3Details += ("枚举对象：StaticHash.lst {0}；干跑来源目录 {1}；期望值清单 {2}" -f $lstPath, $(if ($independent) { $independent.SourceDir } else { '<未定>' }), $ManifestPath)

    if (-not $reportedFull) { $e3Ok = $false }
    elseif (-not $independent)
    {
        $e3Ok = $false
        $e3Details += '[FAIL] 干跑报的完整路径可还原数与任何一个候选目录的独立枚举都对不上'
    }
    if ($independent -and $actualRestored -ne $independent.FullPaths)
    {
        $e3Ok = $false
        $e3Details += ("[FAIL] 真实步骤3 还原 {0} 个 != 干跑声称 {1} 个（干跑 == 实测 不成立）" -f $actualRestored, $independent.FullPaths)
    }
    if ($null -ne $reportedPct -and $independent -and [Math]::Abs($reportedPct - $independent.Percent) -gt 0.05)
    {
        $e3Ok = $false
        $e3Details += ("[FAIL] 干跑成功率 {0}% 与独立枚举 {1:N2}%（{2}/{3}）不符" -f $reportedPct, $independent.Percent, $independent.FullPaths, $independent.Total)
    }
}
Add-Check -Id 'E-3' -Title 'E-3 干跑成功率 == 真实步骤3 结果（同一目录、同一档案）' -Ok $e3Ok -Details $e3Details

# ==================================================================
# E-4 坏档案必须被拒
# ==================================================================

Trace-Step 'E-4：坏档案（盐不匹配 / CRC 错 / 截断 / 版本过高 / 缺 IDENTITY）必须**按该用例专属的理由**明确拒绝'
$e4Details = @()
$e4Ok = $true
$badRunMap = [ordered]@{}
foreach ($entry in ($BadArchiveRunJsonCsv -split ';'))
{
    if (-not $entry.Trim()) { continue }
    $parts = $entry -split '=', 2
    if ($parts.Count -lt 2) { continue }
    $badRunMap[$parts[0].Trim()] = $parts[1].Trim()
}
# 坏档案用例的**前置条件**靠 summary 里记的档案路径独立复核（S-1 的教训：
# "盐不匹配"用例的档案其实同盐、且没有名字 → 实际命中的是"空名字防线"，断言却因为
# 页面套话里的"会社/作品"而变绿）。前置不成立 = 用例无效 = 直接判失败。
$summary = Read-JsonFile -Path $BadArchiveSummaryJson
$caseArchive = @{}
$caseRunJson = @{}
if ($summary)
{
    foreach ($case in @(Get-ObjectValue -Object $summary -Name 'cases' -Default @()))
    {
        $name = [string](Get-ObjectValue -Object $case -Name 'name' -Default '')
        if (-not $name) { continue }
        $caseArchive[$name] = [string](Get-ObjectValue -Object $case -Name 'archive' -Default '')
        $caseRunJson[$name] = [string](Get-ObjectValue -Object $case -Name 'runJson' -Default '')
    }
    $e4Details += ("用例清单：{0}" -f $BadArchiveSummaryJson)
}
else { $e4Details += ("（没有读到用例清单 {0}；前置条件只能靠 run JSON 里的 archiveDump 复核）" -f $BadArchiveSummaryJson) }

$gameSalt = [string](Get-ObjectValue -Object $manifest -Name 'salt' -Default '')
$otherSalt = [string](Get-ObjectValue -Object (Get-ObjectValue -Object $manifest -Name 'otherGame' -Default $null) -Name 'salt' -Default '')

$badCases = @(
    [pscustomobject]@{
        Name = 'salt-mismatch'; Desc = '盐/归属不匹配（别的游戏的档案，且当前游戏没有可用盐）'
        # 产品走的是"无法校验归属"这条出口（因为步骤1 拿不到当前游戏的盐）；
        # 只认归属/摘要不符这类**本用例专属**措辞，且必须排除结构类与空名字类的具体措辞。
        MustHit = @('无法校验扩展集归属', '归属', '摘要与档案记录不符', '没有可用的当前游戏盐')
        Forbidden = @('CRC32 校验失败', 'payload 解压失败', '版本过高', '缺少 IDENTITY chunk', '没有名字', '名字层为空')
        Precondition = 'salt-differs-and-has-names'
    }
    [pscustomobject]@{
        Name = 'salt-mismatch-known'; Desc = '盐不匹配（当前游戏的盐可用 → 必须走"盐不同"判定）'
        MustHit = @('盐不同', '盐不一致', '盐不匹配', '与当前游戏盐')
        Forbidden = @('CRC32 校验失败', 'payload 解压失败', '版本过高', '缺少 IDENTITY chunk', '没有名字', '名字层为空', '无法校验扩展集归属')
        Precondition = 'salt-differs-and-has-names'
    }
    [pscustomobject]@{
        Name = 'crc-error'; Desc = 'CRC 错'
        # 产品 2026-10-13 修复轮把结构类统一包在一条"档案结构缺失或已损坏（读不出来 / 版本不符 /
        # 截断 / CRC 错 / 缺 META 或 IDENTITY）"的伞形正文里，**具体原因在第二行**。
        # 所以判别用"具体那一行"，Forbidden 也只列**其他用例的具体措辞**（不能用伞形里的词，
        # 否则每条结构类用例都会被自己的伞形文案误杀）。
        MustHit = @('CRC32 校验失败', 'CRC 校验失败')
        Forbidden = @('缺少 IDENTITY chunk', 'payload 解压失败', '版本过高', '没有名字', '名字层为空', '盐不同')
        Precondition = 'crc-bad'
    }
    [pscustomobject]@{
        Name = 'truncated'; Desc = '截断 / 解压失败'
        MustHit = @('payload 解压失败', '数据损坏', '截断')
        Forbidden = @('CRC32 校验失败', '缺少 IDENTITY chunk', '版本过高', '没有名字', '名字层为空', '盐不同')
        Precondition = 'structural-problems'
    }
    [pscustomobject]@{
        Name = 'version-high'; Desc = '版本过高'
        MustHit = @('版本过高', '版本不符', '高于本工具支持')
        Forbidden = @('CRC32 校验失败', 'payload 解压失败', '缺少 IDENTITY chunk', '没有名字', '名字层为空', '盐不同')
        Precondition = 'version-ffff'
    }
    [pscustomobject]@{
        Name = 'missing-identity'; Desc = '缺 IDENTITY'
        MustHit = @('缺少 IDENTITY chunk', '结构缺失（IDENTITY）')
        Forbidden = @('CRC32 校验失败', 'payload 解压失败', '版本过高', '没有名字', '名字层为空', '盐不同')
        Precondition = 'no-identity'
    }
    [pscustomobject]@{
        Name = 'empty-names'; Desc = '空档案（一个名字都没有）'
        # 产品原文是「档案里**没有任**何名字（DIRS/FILES 都是空的）」—— 用"任何名字"才稳
        # （`没有名字` 不是它的子串，踩过：合成用例因此被判"没命中专属措辞"）。
        MustHit = @('任何名字', '名字层为空')
        Forbidden = @('CRC32 校验失败', 'payload 解压失败', '版本过高', '缺少 IDENTITY chunk', '盐不同', '无法校验扩展集归属')
        Precondition = 'no-names'
    }
)
# 前置：好档案必须能跑通（否则"被拒"没有对照意义）
if (-not $runMode1.Exists -or [string](Get-ObjectValue -Object $runMode1.Record -Name 'failure' -Default '') -ne '')
{
    $e4Ok = $false
    $e4Details += '[FAIL] 对照组（好档案）没跑通 —— 先让模式1 步骤1 在好档案上成功，坏档案的"被拒"才有意义'
}

# 独立解析用例档案（优先读 summary 里的路径；读不到就退回 run JSON 里的 archiveDump）
function Get-CaseArchiveDump([string]$Name, $Run)
{
    $path = if ($caseArchive.Contains($Name)) { [string]$caseArchive[$Name] } else { '' }
    if ($path -and (Test-Path -LiteralPath $path))
    {
        $dump = Get-ArchiveDumpForAssertion -Path $path -Tag ('e4-' + $Name)
        if ($dump) { return $dump }
    }
    return (Get-ObjectValue -Object $Run.Record -Name 'archiveDump' -Default $null)
}

function Test-CasePrecondition([string]$Kind, $Dump, [ref]$Reason)
{
    if (-not $Dump)
    {
        $Reason.Value = '拿不到用例档案的独立解析结果（既没有 summary 路径也没有 archiveDump）—— 前置无法复核'
        return $false
    }
    $parsed = Get-ObjectValue -Object $Dump -Name 'parsed' -Default $null
    $dirs = @(Get-ObjectValue -Object $parsed -Name 'dirs' -Default @())
    $files = @(Get-ObjectValue -Object $parsed -Name 'files' -Default @())
    $salt = [string](Get-ObjectValue -Object $parsed -Name 'salt' -Default '')
    $problems = @(Get-ObjectValue -Object $Dump -Name 'problems' -Default @())
    $crcOk = [bool](Get-ObjectValue -Object $Dump -Name 'crcOk' -Default $false)
    $version = [int](Get-ObjectValue -Object $Dump -Name 'version' -Default 0)
    $hasIdentity = @(Get-ObjectValue -Object $Dump -Name 'chunks' -Default @() | Where-Object { $_.name -eq 'IDENTITY' }).Count -gt 0
    switch ($Kind)
    {
        'salt-differs-and-has-names'
        {
            if ($salt -eq $gameSalt)
            {
                $Reason.Value = ("用例档案的盐 = '{0}'，与当前游戏**同盐** —— 这样测到的是别的防线（S-1 的假通过），不是盐不匹配" -f $salt)
                return $false
            }
            if (($dirs.Count + $files.Count) -eq 0)
            {
                $Reason.Value = '用例档案一个名字都没有 —— 命中的会是"空名字防线"，不是盐不匹配（S-1 的假通过）'
                return $false
            }
            $Reason.Value = ("盐 '{0}' ≠ 当前游戏 '{1}'，且档案有 {2} 目录 / {3} 文件" -f $salt, $gameSalt, $dirs.Count, $files.Count)
            return $true
        }
        'no-names'
        {
            $ok = (($dirs.Count + $files.Count) -eq 0)
            $Reason.Value = ("DIRS {0} / FILES {1}（要求都为 0）" -f $dirs.Count, $files.Count)
            return $ok
        }
        'crc-bad'
        {
            $Reason.Value = ("crcOk={0}（要求 False）" -f $crcOk)
            return (-not $crcOk)
        }
        'structural-problems'
        {
            $Reason.Value = ("结构问题 {0} 条（要求 > 0）：{1}" -f $problems.Count, (($problems | Select-Object -First 2) -join '；'))
            return ($problems.Count -gt 0)
        }
        'version-ffff'
        {
            $Reason.Value = ("version=0x{0:X4}（要求 0xFFFF）" -f $version)
            return ($version -eq 0xFFFF)
        }
        'no-identity'
        {
            $Reason.Value = ("含 IDENTITY chunk = {0}（要求 False）" -f $hasIdentity)
            return (-not $hasIdentity)
        }
        default { $Reason.Value = ('未知前置类型 ' + $Kind); return $false }
    }
}

foreach ($case in $badCases)
{
    $path = if ($badRunMap.Contains($case.Name)) { [string]$badRunMap[$case.Name] } else { '' }
    if (-not $path -and $caseRunJson.Contains($case.Name)) { $path = [string]$caseRunJson[$case.Name] }
    if (-not $path) { $e4Ok = $false; $e4Details += ("[FAIL] 没有给「{0}」的用例结果路径" -f $case.Desc); continue }
    $run = Get-RunBundle -Path $path -Label $case.Desc
    if (-not $run.Exists)
    {
        $e4Ok = $false
        $e4Details += ("[FAIL] {0}：用例结果不存在（{1}）" -f $case.Desc, $path)
        continue
    }

    # ① 前置条件：这份档案必须**真的是**这一类的坏档案（独立解析，不看产品报告）
    $preReason = ''
    $preOk = Test-CasePrecondition -Kind $case.Precondition -Dump (Get-CaseArchiveDump -Name $case.Name -Run $run) -Reason ([ref]$preReason)
    if (-not $preOk) { $e4Ok = $false }

    # ② 拒绝理由：只用"产品说出来的话"（失败原因 / 对话框正文 / 步骤原因），
    #    **不含**概览与制作状态面板 —— 那是页面套话（S-1：会社/作品就是从那里来的）。
    #    工具日志只在前三者都为空时兜底（它是会话日志，混进无关文本会制造假命中）。
    $reasonParts = New-Object System.Collections.ArrayList
    if ($run.Failure) { [void]$reasonParts.Add($run.Failure) }
    foreach ($dialog in @(Get-ObjectValue -Object $run.Record -Name 'modalDialogs' -Default @()))
    {
        [void]$reasonParts.Add(('{0} {1}' -f [string]$dialog.Title, [string]$dialog.Text))
    }
    $stepResults = Get-ObjectValue -Object $run.Record -Name 'stepResults' -Default $null
    if ($stepResults)
    {
        foreach ($property in $stepResults.PSObject.Properties)
        {
            if ($property.Value) { [void]$reasonParts.Add([string](Get-ObjectValue -Object $property.Value -Name 'reason' -Default '')) }
        }
    }
    $reasonBlob = ($reasonParts -join "`r`n").Trim()
    if (-not $reasonBlob)
    {
        foreach ($log in @(Get-ObjectValue -Object $run.Record -Name 'toolLogs' -Default @()))
        {
            [void]$reasonParts.Add([string](Get-ObjectValue -Object $log -Name 'Text' -Default ''))
        }
        $reasonBlob = ($reasonParts -join "`r`n")
    }
    $hit = @($case.MustHit | Where-Object { $reasonBlob -match [regex]::Escape($_) })
    $forbiddenHit = @($case.Forbidden | Where-Object { $reasonBlob -match [regex]::Escape($_) })
    $rejected = ($run.Failure -ne '') -or ($reasonBlob -match '拒绝' -or $reasonBlob -match '失败' -or $reasonBlob -match '不支持' -or $reasonBlob -match '错误')

    # ③ 被拒的硬证据：没有产出可用的 StaticHash.lst
    $lst = [string](Get-ObjectValue -Object $run.Record -Name 'staticHashLst' -Default '')
    $lstLines = [int](Get-ObjectValue -Object $run.Record -Name 'staticHashLineCount' -Default 0)
    $lstExists = ($lst -and (Test-Path -LiteralPath $lst))

    $ok = ($preOk -and $rejected -and $hit.Count -gt 0 -and $forbiddenHit.Count -eq 0 -and -not $lstExists -and $lstLines -eq 0)
    if (-not $ok) { $e4Ok = $false }
    $e4Details += ("{0}：{1}；前置={2}（{3}）；专属措辞命中 [{4}]；他类防线措辞 [{5}]；拒绝痕迹={6}；StaticHash.lst 存在={7} 行数={8}" -f `
            $(if ($ok) { '[OK]' } else { '[FAIL]' }), $case.Desc, $preOk, $preReason, ($hit -join ','), ($forbiddenHit -join ','), $rejected, $lstExists, $lstLines)
    if (-not $ok)
    {
        $e4Details += ("  失败原因字段='{0}'" -f $run.Failure)
        $excerpt = if ($reasonBlob.Length -gt 400) { $reasonBlob.Substring(0, 400) + '…' } else { $reasonBlob }
        $e4Details += ('  拒绝理由摘录（不含页面套话）：' + ($excerpt -replace "`r?`n", ' / '))
    }
}
$e4Details += ("枚举对象：用例清单 {0}；每份坏档案由脚手架用 lib\rules-int.py 独立解析" -f $BadArchiveSummaryJson)
Add-Check -Id 'E-4' -Title 'E-4 坏档案必须按**该用例专属的理由**被明确拒绝（盐不匹配 / CRC / 截断 / 版本过高 / 缺 IDENTITY / 空名字）' -Ok $e4Ok -Details $e4Details

# ==================================================================
# E-5 不伪造名字
# ==================================================================

Trace-Step 'E-5：负例名 0 次 + 输出里每个名字都能追溯到声明来源'
$e5Details = @()
$e5Ok = $true
if (-not $runMode1.Exists)
{
    $e5Ok = $false
    $e5Details += ("E-5 用例结果不存在：{0}" -f $Mode1RunJson)
}
else
{
    $mode1Index = Get-OutputIndex $runMode1
    $lstText = ''
    $lst = [string](Get-ObjectValue -Object $runMode1.Record -Name 'staticHashLst' -Default '')
    if ($lst -and (Test-Path -LiteralPath $lst)) { $lstText = Read-TextAutoShared -Path $lst -MaxBytes 4000000 }

    # ① 负例名在输出与映射里 0 次
    $negativeHits = @()
    if ($negativeName)
    {
        foreach ($key in $mode1Index.Keys) { if ($key -like ('*' + $negativeName + '*')) { $negativeHits += $key } }
        if ($lstText -like ('*' + $negativeName + '*')) { $negativeHits += 'StaticHash.lst' }
        $reportText = [string](Get-ObjectValue -Object $runMode1.Record -Name 'reportText' -Default '')
        if ($reportText -like ('*' + $negativeName + '*')) { $negativeHits += 'RestoreReport.txt' }
        $e5Details += ("负例名 '{0}' 出现次数：{1}" -f $negativeName, $negativeHits.Count)
    }
    else { $e5Details += '夹具没有提供负例名（E-5 的负例检查被跳过 —— 视为不合格）'; $e5Ok = $false }
    if ($negativeHits.Count -gt 0) { $e5Ok = $false; $e5Details += @('负例名出现在这些地方：') + (Limit-Details $negativeHits) }

    # ② 输出里每个"哈希名形态"的条目都必须能追溯到夹具声明的名字（防凭空造名字）
    $unexplained = @()
    Assert-LoopBudget -Label 'E-5 输出条目' -Count $mode1Index.Count
    foreach ($key in $mode1Index.Keys)
    {
        $leaf = Split-Path $key -Leaf
        # 明文名（.alst/readme/txt 之类）本来就来自源目录，不参与"伪造名字"判定
        if ($key -match '\.(alst|txt|ks|log)$') { continue }
        if (-not $declaredNames.ContainsKey($leaf)) { $unexplained += $key }
    }
    $e5Details += ("输出 {0} 条；名字不在夹具声明清单里的：{1} 条" -f $mode1Index.Count, $unexplained.Count)
    if ($unexplained.Count -gt 0) { $e5Ok = $false; $e5Details += @('凭空出现的名字（无法追溯到任何采集来源）：') + (Limit-Details $unexplained) }
}
Add-Check -Id 'E-5' -Title 'E-5 不伪造名字：负例名 0 次、输出名字全部可追溯' -Ok $e5Ok -Details $e5Details

# ==================================================================
# E-6 源目录只读
# ==================================================================

Trace-Step 'E-6：生成 + 干跑 + 消费全程，源解包目录只读'
$e6Details = @()
$e6Ok = $true
foreach ($run in @($runDraft, $runRaw, $runMode1, $runClassic))
{
    if (-not $run.Exists) { continue }
    foreach ($pair in @(
            [pscustomobject]@{ Name = '解包目录'; Before = (Get-ObjectValue -Object $run.Record -Name 'extractorDigestBefore' -Default $null); After = (Get-ObjectValue -Object $run.Record -Name 'extractorDigestAfter' -Default $null) }
            [pscustomobject]@{ Name = '生成来源目录'; Before = (Get-ObjectValue -Object $run.Record -Name 'sourceDigestBefore' -Default $null); After = (Get-ObjectValue -Object $run.Record -Name 'sourceDigestAfter' -Default $null) }
        ))
    {
        if (-not $pair.Before -or -not $pair.After) { continue }
        $same = ([string]$pair.Before.Digest -eq [string]$pair.After.Digest) -and ([int]$pair.Before.Count -eq [int]$pair.After.Count)
        if ($same)
        {
            $e6Details += ('[OK] {0} 的{1}：{2} 个文件，前后摘要一致（{3}）' -f $run.Label, $pair.Name, $pair.Before.Count, ([string]$pair.Before.Digest).Substring(0, 12))
            continue
        }
        # mtime 不一致时看**内容**：干跑步骤会按 S-5 把"来源目录"重铺一遍（同一份内容、新的 mtime），
        # 那不是"源目录被产品改动"，按内容判才公平。
        $contentBefore = Get-ObjectValue -Object $run.Record -Name 'sourceContentDigestBefore' -Default $null
        $contentAfter = Get-ObjectValue -Object $run.Record -Name 'sourceContentDigestAfter' -Default $null
        $contentSame = ($contentBefore -and $contentAfter -and [string]$contentBefore.Digest -eq [string]$contentAfter.Digest -and [int]$contentBefore.Count -eq [int]$contentAfter.Count)
        if ($pair.Name -eq '生成来源目录' -and $contentSame)
        {
            $e6Details += ('[OK] {0} 的{1}：内容未变（{2} 个文件，内容摘要 {3}）；mtime 变了是因为**脚手架自己**在干跑前重铺了该目录（S-5 的修法），不算产品改动' -f `
                    $run.Label, $pair.Name, $contentBefore.Count, ([string]$contentBefore.Digest).Substring(0, 12))
            continue
        }
        $e6Ok = $false
        $e6Details += ('[FAIL] {0} 的{1}被改动了：{2} 个 -> {3} 个，摘要 {4} -> {5}' -f $run.Label, $pair.Name, $pair.Before.Count, $pair.After.Count, ([string]$pair.Before.Digest).Substring(0, 12), ([string]$pair.After.Digest).Substring(0, 12))
    }
}
if ($e6Details.Count -eq 0) { $e6Ok = $false; $e6Details += '没有任何运行留下了源目录摘要 —— 无法判定只读（不合格）' }
Add-Check -Id 'E-6' -Title 'E-6 源目录只读：生成 / 干跑 / 消费全程内容不变（mtime 由脚手架重铺导致的变化不算）' -Ok $e6Ok -Details $e6Details

# ==================================================================
# E-7 幂等
# ==================================================================

Trace-Step 'E-7：重跑生成档案可复现；重复安装 upsert 不产生重复条目'
$e7Details = @()
$e7Ok = $true
if (-not $runDraft.Exists -or -not $runRepeatDraft.Exists)
{
    $e7Ok = $false
    $e7Details += ("E-7 用例结果缺失：第一次='{0}'（存在={1}） 第二次='{2}'（存在={3}）" -f $DraftRunJson, $runDraft.Exists, $RepeatDraftRunJson, $runRepeatDraft.Exists)
}
else
{
    $sha1 = [string](Get-ObjectValue -Object $runDraft.Record -Name 'draftArchiveSha256' -Default '')
    $sha2 = [string](Get-ObjectValue -Object $runRepeatDraft.Record -Name 'draftArchiveSha256' -Default '')
    $size1 = [long](Get-ObjectValue -Object $runDraft.Record -Name 'draftArchiveSize' -Default 0)
    $size2 = [long](Get-ObjectValue -Object $runRepeatDraft.Record -Name 'draftArchiveSize' -Default 0)
    $e7Details += ("两次生成：{0} 字节 / sha256 {1} vs {2} 字节 / sha256 {3}" -f $size1, $sha1.Substring(0, [Math]::Min(16, $sha1.Length)), $size2, $sha2.Substring(0, [Math]::Min(16, $sha2.Length)))

    # 先记录两次生成的 META.generatedAt（S-4：UI 路径下它会"继承" —— `-Step draft` 会先跑 meta，
    # 而编辑信息对话框会从已存在的档案预填 Meta（含 generatedAt），内核只在它为空时才盖时间戳）。
    # 所以"字节相同"在 UI 路径下**不是**稳定的性质，判据是"除 META.generatedAt 外逐 chunk 一致"。
    $dumpA = Get-ObjectValue -Object $runDraft.Record -Name 'archiveDump' -Default $null
    $dumpB = Get-ObjectValue -Object $runRepeatDraft.Record -Name 'archiveDump' -Default $null
    $metaA = Get-ObjectValue -Object (Get-ObjectValue -Object $dumpA -Name 'parsed' -Default $null) -Name 'meta' -Default $null
    $metaB = Get-ObjectValue -Object (Get-ObjectValue -Object $dumpB -Name 'parsed' -Default $null) -Name 'meta' -Default $null
    $generatedAtA = [string](Get-ObjectValue -Object $metaA -Name 'generatedAt' -Default '')
    $generatedAtB = [string](Get-ObjectValue -Object $metaB -Name 'generatedAt' -Default '')
    $e7Details += ("META.generatedAt：第一次='{0}' 第二次='{1}' 相同={2}（相同说明第二次是从已有档案**继承**的，不是「生成时刻」）" -f `
            $generatedAtA, $generatedAtB, ($generatedAtA -eq $generatedAtB))
    foreach ($stamp in @($generatedAtA, $generatedAtB))
    {
        if (-not $stamp) { continue }
        if ($stamp -notmatch '^\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2}$')
        {
            $e7Ok = $false
            $e7Details += ("[FAIL] META.generatedAt 格式不合法：'{0}'（要求 yyyy-MM-dd HH:mm:ss）" -f $stamp)
        }
    }
    $e7Details += ("幂等口径：-IdempotenceMode {0}{1}（判据 = 除 META.generatedAt 外逐 chunk 一致；-StrictBytes 仅用于内核级 stampGeneratedAt=false 的场景）" -f `
            $IdempotenceMode, $(if ($StrictBytes) { ' + -StrictBytes' } else { '' }))

    if ($sha1 -and $sha1 -eq $sha2 -and -not $StrictBytes)
    {
        $e7Details += '[OK] 两次生成字节相同（本次两轮的 META.generatedAt 也相同 —— 注意这是"继承"的结果，不作为稳定性判据）'
    }
    else
    {
        # 逐 chunk 比对。META 里的 `generatedAt` 是**已知易变字段**（UI 生成路径会填/继承它），
        # 所以 META 按"去掉易变键"再比，其余 chunk 一律逐字节比。
        $volatileMetaKeys = @('generatedAt', 'generatedUtc', 'generatedTime', 'elapsedMillis', 'elapsedMs', '生成时间', '生成耗时')
        $dump1 = $dumpA
        $dump2 = $dumpB
        $chunkDiffs = @()
        $volatileOnly = @()
        if ($dump1 -and $dump2)
        {
            $chunks1 = @{}
            foreach ($chunk in @(Get-ObjectValue -Object $dump1 -Name 'chunks' -Default @())) { $chunks1[[string]$chunk.name] = $chunk }
            foreach ($chunk in @(Get-ObjectValue -Object $dump2 -Name 'chunks' -Default @()))
            {
                $name = [string]$chunk.name
                if (-not $chunks1.ContainsKey($name)) { $chunkDiffs += ($name + '（只出现在第二次）'); continue }
                if ([string]$chunks1[$name].sha256 -eq [string]$chunk.sha256) { continue }
                if ($name -ne 'META') { $chunkDiffs += $name; continue }

                # META：去掉易变键后逐键比较
                $meta1 = $metaA
                $meta2 = $metaB
                $map1 = @{}
                $map2 = @{}
                if ($meta1) { foreach ($property in $meta1.PSObject.Properties) { $map1[$property.Name] = [string]$property.Value } }
                if ($meta2) { foreach ($property in $meta2.PSObject.Properties) { $map2[$property.Name] = [string]$property.Value } }
                $keys = @($map1.Keys) + @($map2.Keys) | Sort-Object -Unique
                foreach ($key in $keys)
                {
                    $value1 = if ($map1.ContainsKey($key)) { $map1[$key] } else { '<缺>' }
                    $value2 = if ($map2.ContainsKey($key)) { $map2[$key] } else { '<缺>' }
                    if ($value1 -eq $value2) { continue }
                    if ($key -in $volatileMetaKeys) { $volatileOnly += $key; continue }
                    $chunkDiffs += ('META.{0}' -f $key)
                }
            }
            if ($chunkDiffs.Count -gt 0) { $e7Details += @('逐 chunk 差异（非易变）：') + (Limit-Details $chunkDiffs) }
            if ($volatileOnly.Count -gt 0) { $e7Details += @('仅易变字段不同（按裁决不计差异）：') + (Limit-Details $volatileOnly) }
            $meta1 = $metaA
            if ($meta1) { $e7Details += ('第一次的 META：' + (($meta1.PSObject.Properties | ForEach-Object { '{0}={1}' -f $_.Name, $_.Value }) -join ', ')) }
        }
        else { $e7Details += '（没有档案解析结果，无法定位差异）' }

        if ($IdempotenceMode -eq 'byte' -or $StrictBytes)
        {
            $e7Ok = $false
            $e7Details += '[FAIL] 两次生成的档案不是逐字节一致（-IdempotenceMode byte / -StrictBytes）'
        }
        elseif ($chunkDiffs.Count -eq 0)
        {
            $e7Details += ("[OK] 除 META 的易变字段（{0}）外，其余 chunk 逐字节一致 —— 这就是 E-7 的判据（lead 裁决：UI 路径不要求字节一致；-StrictBytes 只用于内核级 stampGeneratedAt=false 场景）" -f ($volatileOnly -join ', '))
        }
        else
        {
            $e7Ok = $false
            $e7Details += '[FAIL] 两次生成的内容不一致（不是只有易变字段）—— 同名同盐同内容必须可复现（E-7）'
        }
    }

    # 重复安装：同一 exe 摘要只应有一条清单项
    if ($runRepeatInstall.Exists)
    {
        $manifestText = [string](Get-ObjectValue -Object $runRepeatInstall.Record -Name 'libraryManifestText' -Default '')
        $manifestPath2 = [string](Get-ObjectValue -Object $runRepeatInstall.Record -Name 'libraryManifest' -Default '')
        if (-not $manifestText -and $manifestPath2 -and (Test-Path -LiteralPath $manifestPath2)) { $manifestText = Read-TextAutoShared -Path $manifestPath2 }
        # 库清单是 `# 表头` + TSV 10 列（lead 同步的 task-1 事实 #6），注释行不参与去重判定
        $rows = @($manifestText -split "`r?`n" | Where-Object { $_.Trim() -and -not $_.TrimStart().StartsWith('#') })
        $exeSha = [string](Get-ObjectValue -Object (Get-ObjectValue -Object $manifest -Name 'oldGame' -Default $null) -Name 'gameExeSha256' -Default '')
        $dupRows = @($rows | Where-Object { $exeSha -and $_ -like ('*' + $exeSha + '*') })
        $e7Details += ("库清单行数 {0}；含本作 exe 摘要（{1}）的行数 {2}" -f $rows.Count, $exeSha.Substring(0, [Math]::Min(12, $exeSha.Length)), $dupRows.Count)
        if ($rows.Count -eq 0) { $e7Ok = $false; $e7Details += '[FAIL] 库清单是空的（读不到 manifest.txt）' }
        elseif ($dupRows.Count -gt 1) { $e7Ok = $false; $e7Details += ('[FAIL] 重复安装产生了 {0} 条同 exe 摘要的清单项（upsert 没生效）' -f $dupRows.Count) }
    }
    else
    {
        $e7Details += ("（没有第二次安装的结果：{0}；upsert 去重这条未覆盖）" -f $RepeatInstallRunJson)
    }
}
Add-Check -Id 'E-7' -Title 'E-7 幂等：档案字节可复现 + 安装 upsert 不重复' -Ok $e7Ok -Details $e7Details

# ==================================================================
# E-8 体积（结构自洽 + 每条平均字节；小夹具上不比"同量级"）
# ==================================================================

Trace-Step 'E-8：档案结构自洽 + 条数 + 每条平均字节'
$e8Details = @()
$e8Ok = $true
if (-not $runDraft.Exists)
{
    $e8Ok = $false
    $e8Details += ("E-8 用例结果不存在：{0}" -f $DraftRunJson)
}
else
{
    $size = [long](Get-ObjectValue -Object $runDraft.Record -Name 'draftArchiveSize' -Default 0)
    $dump = Get-ObjectValue -Object $runDraft.Record -Name 'archiveDump' -Default $null
    $dirCount = 0; $fileCount = 0; $exceptionCount = 0
    $headerSize = 0; $chunkCount = 0; $payloadSize = 0; $metaSize = 0; $identitySize = 0; $nameLayerSize = 0
    if ($dump)
    {
        $parsed = Get-ObjectValue -Object $dump -Name 'parsed' -Default $null
        $dirCount = @(Get-ObjectValue -Object $parsed -Name 'dirs' -Default @()).Count
        $fileCount = @(Get-ObjectValue -Object $parsed -Name 'files' -Default @()).Count
        $exceptionCount = @(Get-ObjectValue -Object $parsed -Name 'exceptions' -Default @()).Count
        $headerSize = [int](Get-ObjectValue -Object $dump -Name 'headerSize' -Default 20)
        $chunkCount = [int](Get-ObjectValue -Object $dump -Name 'chunkCount' -Default 0)
        $payloadSize = [int](Get-ObjectValue -Object $dump -Name 'payloadSize' -Default 0)
        foreach ($chunk in @(Get-ObjectValue -Object $dump -Name 'chunks' -Default @()))
        {
            switch ([string]$chunk.name)
            {
                'META' { $metaSize = [int]$chunk.size }
                'IDENTITY' { $identitySize = [int]$chunk.size }
                { $_ -in @('DIRS', 'FILES', 'EXCEPTIONS') } { $nameLayerSize += [int]$chunk.size }
            }
        }
    }
    $expectDirs = [int](Get-ObjectValue -Object $extensionSet -Name 'expectedDirCount' -Default 0)
    $expectFiles = [int](Get-ObjectValue -Object $extensionSet -Name 'expectedFileCount' -Default 0)
    $expectExceptions = [int](Get-ObjectValue -Object $extensionSet -Name 'expectedExceptionCount' -Default 0)
    $entries = $dirCount + $fileCount
    $perEntryAll = if ($entries -gt 0) { [double]$size / $entries } else { 0 }
    $perEntryNames = if ($entries -gt 0) { [double]$nameLayerSize / $entries } else { 0 }
    # 口径要说清：`size` 是**压缩后**的文件大小，chunk 的 size 是**解压后**的；
    # 所以"固定开销/名字层"都在解压域里算，和 payloadSize 对得上（别拿压缩后的大小去减解压后的分量）。
    $metaOverhead = $metaSize + $identitySize
    $otherSize = [Math]::Max(0, $payloadSize - $metaOverhead - $nameLayerSize)
    $headerTableSize = $headerSize + 12 * $chunkCount

    # ① 结构自洽（脚手架独立解析，真独立检查）
    if ($dump)
    {
        $problems = @(Get-ObjectValue -Object $dump -Name 'problems' -Default @())
        $e8Details += ("结构自洽（magic/headerSize/版本/CRC/偏移，由 lib\rules-int.py 独立解析）：{0}" -f $(if ($problems.Count -eq 0) { 'OK' } else { ($problems -join '；') }))
        if ($problems.Count -gt 0) { $e8Ok = $false }
    }
    else { $e8Ok = $false; $e8Details += '[FAIL] 没有档案解析结果（lib\rules-int.py dump 没跑或失败）' }

    # ② 条数（名字层口径）
    $e8Details += ("条数实测：DIRS {0} / FILES {1} / EXCEPTIONS {2}；期望（夹具独立枚举）：{3} / {4} / {5}" -f `
            $dirCount, $fileCount, $exceptionCount, $expectDirs, $expectFiles, $expectExceptions)
    if ($dirCount -ne $expectDirs) { $e8Ok = $false; $e8Details += ("[FAIL] DIRS 条数 {0} != 期望 {1}" -f $dirCount, $expectDirs) }
    if ($fileCount -ne $expectFiles) { $e8Ok = $false; $e8Details += ("[FAIL] FILES 条数 {0} != 期望 {1}（同名去重口径也要对得上）" -f $fileCount, $expectFiles) }
    if ($exceptionCount -ne $expectExceptions) { $e8Ok = $false; $e8Details += ("[FAIL] EXCEPTIONS 条数 {0} != 期望 {1}" -f $exceptionCount, $expectExceptions) }

    # ③ 体积：**只在小夹具上做量级自检**，并把"固定开销"与"名字层"拆开报
    #    （S-6：20 条档案里固定开销占大头，拿整档 ÷ 条数去比 §3.3 没有鉴别力，别夸大结论）
    $e8Details += ("体积实测：档案 {0} 字节（{1:N1} KiB，压缩后）/ 解压后 payload {2} 字节" -f $size, ($size / 1024.0), $payloadSize)
    $e8Details += ("  解压后构成：头 {0} + chunk 表 {1}×12 = {2}；META {3} + IDENTITY {4}（元数据 {5}）；名字层（DIRS+FILES+EXCEPTIONS）{6}；其他（STATS 等）{7}" -f `
            $headerSize, $chunkCount, $headerTableSize, $metaSize, $identitySize, $metaOverhead, $nameLayerSize, $otherSize)
    $e8Details += ("  每条平均：名字层 {0:N1} 字节/条（§3.3 估 ≈22 字节/条未压缩）—— 这是本断言唯一看的量级指标；整档 {1:N1} 字节/条**不作判据**" -f $perEntryNames, $perEntryAll)
    $e8Details += "  说明：本夹具只有 20 条，量级结论不成立；要量 §3.3 请用 make-extension-fixture.ps1 -ExtraFiles 3000 放大后再看名字层每条平均"
    if ($size -le 0) { $e8Ok = $false; $e8Details += '[FAIL] 档案大小是 0' }
    elseif ($size -gt $MaxArchiveBytes) { $e8Ok = $false; $e8Details += ("[FAIL] 档案 {0} 字节超过上界 {1} 字节（体积失控）" -f $size, $MaxArchiveBytes) }
    # 名字层每条超过 128 字节就说明"只存名字"的口径没落实（32 字节哈希 + 名字也才 ~50）
    if ($entries -gt 0 -and $perEntryNames -gt 128) { $e8Ok = $false; $e8Details += ("[FAIL] 名字层每条 {0:N1} 字节，远超「只存名字 + 盐」的量级（像是把哈希/冗余也存进去了）" -f $perEntryNames) }
    if ($entries -eq 0) { $e8Ok = $false; $e8Details += '[FAIL] 名字层是空的（DIRS+FILES 都是 0）' }
}
Add-Check -Id 'E-8' -Title 'E-8 档案结构自洽 + 条数 + 名字层每条平均字节（小夹具不比 §3.3 量级）' -Ok $e8Ok -Details $e8Details

# ==================================================================
# E-9 大小写与分隔符（归一化单测）
# ==================================================================

Trace-Step 'E-9：目录一律小写 + 结尾斜杠；文件名为裸名；%EmptyString% / / / \ 归一化'
$e9Details = @()
$e9Ok = $true

function Test-NormalizationTable([string]$LstPath, $ArchiveDump, [ref]$Details)
{
    $ok = $true
    $dirEntries = @()
    $fileEntries = @()
    if ($LstPath -and (Test-Path -LiteralPath $LstPath))
    {
        $text = Read-TextAutoShared -Path $LstPath -MaxBytes 4000000
        $rows = @($text -split "`r?`n" | Where-Object { $_.Trim() })
        Assert-LoopBudget -Label 'StaticHash.lst 条目' -Count $rows.Count
        foreach ($row in $rows)
        {
            $parts = $row -split ':', 2
            if ($parts.Count -lt 2) { continue }
            if ($parts[0].Length -eq 16) { $dirEntries += $parts[1] }
            elseif ($parts[0].Length -eq 64) { $fileEntries += $parts[1] }
        }
    }
    $Details.Value += ("StaticHash.lst：16 位目录条目 {0} 条 / 64 位文件条目 {1} 条" -f $dirEntries.Count, $fileEntries.Count)

    $badDirs = @()
    foreach ($name in $dirEntries)
    {
        if ($name -cne $name.ToLowerInvariant()) { $badDirs += ($name + '（不是小写）'); continue }
        if ($name -match '\\') { $badDirs += ($name + '（含反斜杠）'); continue }
        if ($name -ne '/' -and -not $name.EndsWith('/')) { $badDirs += ($name + '（没有结尾斜杠）') }
    }
    $badFiles = @()
    foreach ($name in $fileEntries)
    {
        if ($name -match '[\\/]') { $badFiles += ($name + '（文件名里不该有目录分隔符）') }
        if ($name -ne [System.IO.Path]::GetFileName($name)) { $badFiles += ($name + '（不是裸名）') }
    }
    $Details.Value += ("目录条目不合格 {0} 条 / 文件条目不合格 {1} 条" -f $badDirs.Count, $badFiles.Count)
    if ($badDirs.Count -gt 0) { $ok = $false; $Details.Value += @('目录名不合格：') + (Limit-Details $badDirs) }
    if ($badFiles.Count -gt 0) { $ok = $false; $Details.Value += @('文件名不合格：') + (Limit-Details $badFiles) }

    # 根域口径：目录条目里应当出现 '/'（§13.2 根记 /），而不是 %EmptyString%
    if ($dirEntries -contains '%EmptyString%') { $Details.Value += '提示：目录条目里出现 %EmptyString%（设计口径是根记 "/"；若还原内核能认它，请 lead 裁决口径）' }
    if ($dirEntries.Count -gt 0 -and -not ($dirEntries -contains '/')) { $Details.Value += '提示：目录条目里没有 "/"（夹具含根域文件，若根域条目缺失，根下文件会还原不出来）' }

    # 档案里的 DIRS/FILES 也要满足同一口径
    if ($ArchiveDump)
    {
        $parsed = Get-ObjectValue -Object $ArchiveDump -Name 'parsed' -Default $null
        $dirs = @(Get-ObjectValue -Object $parsed -Name 'dirs' -Default @())
        $files = @(Get-ObjectValue -Object $parsed -Name 'files' -Default @())
        $badArchiveDirs = @($dirs | Where-Object { $_ -cne ([string]$_).ToLowerInvariant() -or ($_ -ne '/' -and -not ([string]$_).EndsWith('/')) -or $_ -match '\\' })
        $badArchiveFiles = @($files | Where-Object { $_ -match '[\\/]' })
        $Details.Value += ("档案 DIRS {0} 条（不合格 {1}）/ FILES {2} 条（不合格 {3}）" -f $dirs.Count, $badArchiveDirs.Count, $files.Count, $badArchiveFiles.Count)
        if ($badArchiveDirs.Count -gt 0) { $ok = $false; $Details.Value += @('档案 DIRS 不合格：') + (Limit-Details @($badArchiveDirs | ForEach-Object { [string]$_ })) }
        if ($badArchiveFiles.Count -gt 0) { $ok = $false; $Details.Value += @('档案 FILES 不合格：') + (Limit-Details @($badArchiveFiles | ForEach-Object { [string]$_ })) }
    }
    else { $Details.Value += '（没有档案解析结果 —— 只验证了 StaticHash.lst 那一半）' }
    return $ok
}

# 归一化单测的"夹具侧口径"：%EmptyString% / / / \ 都必须归到根
$unitCases = @(
    [pscustomobject]@{ Input = '%EmptyString%'; Expect = '/' }
    [pscustomobject]@{ Input = '/'; Expect = '/' }
    [pscustomobject]@{ Input = '\'; Expect = '/' }
    [pscustomobject]@{ Input = ''; Expect = '/' }
    [pscustomobject]@{ Input = 'Characters\Yuki'; Expect = 'characters/yuki/' }
    [pscustomobject]@{ Input = 'UI'; Expect = 'ui/' }
)
function ConvertTo-NormalizedDirectory([string]$Raw)
{
    $text = ($Raw -replace '\\', '/')
    while ($text -match '//') { $text = $text -replace '//', '/' }
    $text = $text.Trim('/')
    if (-not $text -or $Raw -eq '%EmptyString%') { return '/' }
    return ($text.ToLowerInvariant() + '/')
}
$unitBad = @()
foreach ($case in $unitCases)
{
    $actual = ConvertTo-NormalizedDirectory $case.Input
    if ($actual -ne $case.Expect) { $unitBad += ("'{0}' -> '{1}'（期望 '{2}'）" -f $case.Input, $actual, $case.Expect) }
}
$e9Details += ("归一化单测（脚手架口径）：{0} 例，失败 {1}" -f $unitCases.Count, $unitBad.Count)
if ($unitBad.Count -gt 0) { $e9Ok = $false; $e9Details += $unitBad }

# 用真实产物再判一遍：StaticHash.lst（模式1 步骤1 的展开物）+ 档案 DIRS/FILES
$lstForE9 = ''
$dumpForE9 = $null
if ($runMode1.Exists) { $lstForE9 = [string](Get-ObjectValue -Object $runMode1.Record -Name 'staticHashLstSnapshot' -Default '') }
if (-not $lstForE9 -and $runMode1.Exists) { $lstForE9 = [string](Get-ObjectValue -Object $runMode1.Record -Name 'staticHashLst' -Default '') }
if (-not $lstForE9 -and $runDraft.Exists) { $lstForE9 = [string](Get-ObjectValue -Object $runDraft.Record -Name 'staticHashLstSnapshot' -Default '') }
if ($runDraft.Exists) { $dumpForE9 = Get-ObjectValue -Object $runDraft.Record -Name 'archiveDump' -Default $null }
if (-not $lstForE9 -and -not $dumpForE9)
{
    $e9Ok = $false
    $e9Details += '[FAIL] 既没有 StaticHash.lst 也没有档案解析结果 —— 无法验证真实产物的归一化口径'
}
else
{
    $e9Details += ("产物来源：StaticHash.lst='{0}'；档案解析={1}" -f $lstForE9, $(if ($dumpForE9) { '有' } else { '无' }))
    if (-not (Test-NormalizationTable -LstPath $lstForE9 -ArchiveDump $dumpForE9 -Details ([ref]$e9Details))) { $e9Ok = $false }
}
Add-Check -Id 'E-9' -Title 'E-9 大小写与分隔符：目录小写 + 结尾斜杠、文件裸名、根域归一化' -Ok $e9Ok -Details $e9Details

# ==================================================================
# E-10 模式1 ≡ 传统动态模式（差异只允许出现在"动态采集才有的名字"上）
# ==================================================================

Trace-Step 'E-10：模式1 与传统动态模式逐条一致（差异必须可解释）'
$e10Details = @()
$e10Ok = $true
if (-not $runMode1.Exists -or -not $runClassic.Exists)
{
    $e10Ok = $false
    $e10Details += ("E-10 用例结果缺失：模式1（存在={0}） 传统模式（存在={1}）" -f $runMode1.Exists, $runClassic.Exists)
}
else
{
    $compare = Compare-OutputSets $runMode1 $runClassic '模式1' '传统动态模式' ([ref]$e10Details)
    if (-not $compare.Ok)
    {
        # 差异只允许出现在"动态采集才有的名字"上：即传统模式多出来的条目，
        # 其名字不在扩展集（夹具声明的名字层）里。逐条判定并解释。
        $archiveNames = @{}
        foreach ($spec in @(Get-ObjectValue -Object $extensionSet -Name 'files' -Default @())) { $archiveNames[[string]$spec.name] = $true }
        $explained = @()
        $unexplained = @()
        foreach ($rel in $compare.OnlyB)
        {
            $leaf = Split-Path $rel -Leaf
            if ($archiveNames.ContainsKey($leaf)) { $unexplained += $rel } else { $explained += $rel }
        }
        foreach ($rel in $compare.OnlyA)
        {
            # 模式1 多出来的 = 扩展集里有、传统模式没还原的（说明动态采集缺了名字）
            $leaf = Split-Path $rel -Leaf
            if ($archiveNames.ContainsKey($leaf)) { $unexplained += ('模式1 独有：' + $rel) } else { $explained += ('模式1 独有：' + $rel) }
        }
        foreach ($rel in $compare.DiffContent) { $unexplained += ('内容不同：' + $rel) }
        $e10Details += ("差异分解：只有传统模式有 {0} 条（其中名字不在扩展集里 {1} 条）/ 只有模式1 有 {2} 条 / 内容不同 {3} 条" -f `
                $compare.OnlyB.Count, $explained.Count, $compare.OnlyA.Count, $compare.DiffContent.Count)
        if ($explained.Count -gt 0) { $e10Details += @('可解释的差异（名字只在动态采集里）：') + (Limit-Details $explained) }
        if ($unexplained.Count -gt 0)
        {
            $e10Ok = $false
            $e10Details += @('[FAIL] 无法用"动态采集才有的名字"解释的差异：') + (Limit-Details $unexplained)
        }
        else
        {
            $e10Details += '[OK] 所有差异都能用"名字只在动态采集里"解释（报告里已逐条列出）'
        }
    }
    # 报告里必须能解释差异：检查报告/概览文本里是否有"来源/扩展集/动态"这类说明
    $evidence = (Get-EvidenceText $runMode1) + (Get-EvidenceText $runClassic)
    $explainsSource = ($evidence -match '扩展集' -and ($evidence -match '动态' -or $evidence -match '来源'))
    $e10Details += ("报告/概览里能解释映射来源：{0}" -f $explainsSource)
    if (-not $explainsSource) { $e10Ok = $false; $e10Details += '[FAIL] 报告里看不出"映射来源"（模式1 用扩展集 / 传统模式用动态采集），差异就无法被解释' }
    # 输入等价的硬证据（与 E-1 同一份数据，这里再列一次，方便报告单独引用）
    $contentMode1 = Get-ObjectValue -Object $runMode1.Record -Name 'extractorContentDigest' -Default $null
    $contentClassic = Get-ObjectValue -Object $runClassic.Record -Name 'extractorContentDigest' -Default $null
    if ($contentMode1 -and $contentClassic)
    {
        $e10Details += ("输入等价（内容摘要不含 mtime）：模式1 {0} 个 / {1}；传统模式 {2} 个 / {3}；相同={4}" -f `
                $contentMode1.Count, ([string]$contentMode1.Digest).Substring(0, 16),
            $contentClassic.Count, ([string]$contentClassic.Digest).Substring(0, 16),
            (([int]$contentMode1.Count -eq [int]$contentClassic.Count) -and ([string]$contentMode1.Digest -eq [string]$contentClassic.Digest)))
    }
    $e10Details += ("枚举对象：模式1 输出清单 {0}；传统模式输出清单 {1}；期望值清单 {2}" -f $Mode1RunJson, $ClassicRunJson, $ManifestPath)
}
Add-Check -Id 'E-10' -Title 'E-10 模式1 ≡ 传统动态模式（独立枚举对账，差异必须可解释）' -Ok $e10Ok -Details $e10Details

# ==================================================================
# X-6（性能）：脚本自身无静默空转
# ==================================================================

Trace-Step 'X-6：脚本耗时预算'
$x6Details = @()
$x6Ok = $true
$totalMs = [int]((Get-Date) - $script:traceStarted).TotalMilliseconds
$slow = @(Get-SlowSections)
$x6Details += ("整脚本 {0:N1} 秒；单条断言预算 {1} 秒" -f ($totalMs / 1000.0), ($script:sectionBudgetMs / 1000.0))
if ($slow.Count -gt 0)
{
    $x6Ok = $false
    $x6Details += ('[FAIL] 有 {0} 段超过预算（表现为"卡住"）：' -f $slow.Count)
    $x6Details += ($slow | ForEach-Object { '    {0:N1}s  {1}' -f ($_.Ms / 1000.0), $_.Text })
}
else { $x6Details += '[OK] 没有任何一段超过预算' }
$segments = New-Object System.Collections.ArrayList
for ($index = 1; $index -lt $script:traceLog.Count; $index++)
{
    [void]$segments.Add([pscustomobject]@{ Ms = ($script:traceLog[$index].AtMs - $script:traceLog[$index - 1].AtMs); Text = $script:traceLog[$index - 1].Text })
}
$x6Details += '最长的几段：'
$x6Details += ($segments | Sort-Object Ms -Descending | Select-Object -First 5 | ForEach-Object { '    {0,7:N0}ms  {1}' -f $_.Ms, $_.Text })
Add-Check -Id 'X-6' -Title 'X-6（性能）脚本自身：无静默空转，单条断言在预算内' -Ok $x6Ok -Details $x6Details

# ==================================================================
# E-11（像素幽灵）：页签切换后画面是否干净 —— 逐像素比对
# ==================================================================
#
# 为什么要独立一条：切到「制作扩展集」后首页控件的 `IsWindowVisible` 全是 False
# （可见性是对的），但**旧像素留在窗口表面上**（用户看到的叠字）。
# 只读窗口状态的断言在缺陷存在时照样全绿 —— 幽灵判据只能是像素比对。
# 抓图由 run-tab-pixels.ps1 产生（-Scope extension 下由 run-all.ps1 先跑），
# 这里**只读快照**、不启动产品（README 踩坑 #22：现场会被下一条用例改掉）。
#
# 口径说明（别混）：E-11a/b 是像素判据；E-11e 只看可见性，**不能**当幽灵判据。
if (-not $SkipTabPixels)
{
    Trace-Step ("E-11：读抓图快照 {0}" -f $TabPixelsJson)
    [void](Invoke-TabPixelChecks -SnapshotJson $TabPixelsJson -DiffDir $TabPixelsDiffDir -RequireSnapshot $false)
    Trace-Step 'E-11：像素比对完成'
}
else
{
    Add-Check -Id 'E-11' -Title '页签像素幽灵回归（-SkipTabPixels：本项未执行）' -Ok $true -Severity 'info' `
        -Details @('调用方显式要求跳过 E-11 —— 本次结论里**没有**覆盖像素幽灵，别当成"验证过"')
    Trace-Step 'E-11：-SkipTabPixels，跳过'
}

# ==================================================================
# E-12 .. E-19：「扩展集」页签（§8.5）+ 在线更新（§14）
# ==================================================================
#
# 期望值**一律独立算**，不采信产品自述：
#   · 列表 / 面板里的会社、作品、条数 —— 断言自己解析 run 记录里那份**库清单快照**
#     （driver 在驱动当时把 manifest.txt 拷到 Run\out，后续用例改库不影响它）
#   · 档案里的 META / IDENTITY / STATS —— 断言自己调 lib\rules-int.py dump
#   · 远端索引 —— 断言自己解析仓库目录里的 index.txt（不是产品给的那份）
# 判据落在"产品的界面文字 / 弹窗文字 / 日志文字里**有没有出现**这些值"上。

# ---------- 独立解析：清单 / 索引（10 列 TSV）----------

function ConvertFrom-ManifestText
{
    param([AllowEmptyString()][string]$Text)

    $rows = New-Object System.Collections.ArrayList
    $problems = New-Object System.Collections.ArrayList
    $line = 0
    foreach ($raw in ([string]$Text -split "`r?`n"))
    {
        $line++
        if (-not $raw.Trim()) { continue }
        if ($raw.TrimStart().StartsWith('#')) { continue }
        $fields = $raw -split "`t"
        if ($fields.Count -ne 10)
        {
            [void]$problems.Add(('第 {0} 行有 {1} 列（应为 10 列）' -f $line, $fields.Count))
            continue
        }
        [void]$rows.Add([pscustomobject]@{
                Publisher = $fields[0]; Title = $fields[1]; ExeDigest = $fields[2]; SiblingDigest = $fields[3]
                SaltDigest = $fields[4]; ArchiveDigest = $fields[5]; EntryCount = $fields[6]
                GeneratedAt = $fields[7]; InstalledAt = $fields[8]; Source = $fields[9]; Line = $line
            })
    }
    return [pscustomobject]@{ Rows = @($rows); Problems = @($problems) }
}

# ---------- 独立解析：档案事实（META / IDENTITY / STATS / 摘要）----------

function Get-ArchiveFacts
{
    param([string]$Path, [string]$Tag)

    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return $null }
    $dump = Get-ArchiveDumpForAssertion -Path $Path -Tag $Tag
    if (-not $dump) { return $null }
    $parsed = Get-ObjectValue -Object $dump -Name 'parsed' -Default $null
    $meta = Get-ObjectValue -Object $parsed -Name 'meta' -Default $null
    $identity = Get-ObjectValue -Object $parsed -Name 'identity' -Default $null
    $stats = Get-ObjectValue -Object $parsed -Name 'stats' -Default $null
    $dirs = @(Get-ObjectValue -Object $parsed -Name 'dirs' -Default @())
    $files = @(Get-ObjectValue -Object $parsed -Name 'files' -Default @())
    $exceptions = @(Get-ObjectValue -Object $parsed -Name 'exceptions' -Default @())
    $salt = [string](Get-ObjectValue -Object $identity -Name 'salt' -Default '')
    $statDirs = [int](Get-ObjectValue -Object $stats -Name 'directories' -Default 0)
    $statFiles = [int](Get-ObjectValue -Object $stats -Name 'files' -Default 0)
    return [pscustomobject]@{
        Path          = [System.IO.Path]::GetFullPath($Path)
        Sha256        = [string](Get-ObjectValue -Object $dump -Name 'sha256' -Default '')
        Size          = [long](Get-ObjectValue -Object $dump -Name 'fileSize' -Default 0)
        Publisher     = [string](Get-ObjectValue -Object $meta -Name 'publisher' -Default '')
        Title         = [string](Get-ObjectValue -Object $meta -Name 'title' -Default '')
        Comment       = [string](Get-ObjectValue -Object $meta -Name 'comment' -Default '')
        ExeDigest     = [string](Get-ObjectValue -Object $identity -Name 'target.sha256' -Default '')
        Salt          = $salt
        SaltDigest    = (Get-TextSha256Hex -Text $salt)
        DirCount      = $dirs.Count
        FileCount     = $files.Count
        StatDirs      = $statDirs
        StatFiles     = $statFiles
        EntryCount    = $(if (($statDirs + $statFiles) -gt 0) { $statDirs + $statFiles } else { $dirs.Count + $files.Count })
        ExceptionCount = $exceptions.Count
        Problems      = @(Get-ObjectValue -Object $dump -Name 'problems' -Default @())
    }
}

# ---------- 一次 set / online 用例的运行记录 ----------

function Get-SetRun
{
    param([string]$Path, [string]$Label)

    $bundle = [pscustomobject]@{
        Label = $Label; Path = $Path; Exists = $false; Record = $null; Failure = ''
        SetTab = $null; SetBrowse = $null; SetOpen = $null; SetNote = $null; SetCheck = $null; Online = $null
        Modals = @(); Logs = ''; Mode = ''
    }
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return $bundle }
    $record = Read-JsonFile -Path $Path
    if (-not $record) { return $bundle }

    $logText = New-Object System.Collections.ArrayList
    foreach ($log in @(Get-ObjectValue -Object $record -Name 'toolLogs' -Default @()))
    {
        [void]$logText.Add([string](Get-ObjectValue -Object $log -Name 'Text' -Default ''))
    }
    $modals = New-Object System.Collections.ArrayList
    foreach ($dialog in @(Get-ObjectValue -Object $record -Name 'modalDialogs' -Default @()))
    {
        [void]$modals.Add([pscustomobject]@{
                Title = [string](Get-ObjectValue -Object $dialog -Name 'Title' -Default '')
                Text = [string](Get-ObjectValue -Object $dialog -Name 'Text' -Default '')
            })
    }
    return [pscustomobject]@{
        Label = $Label; Path = $Path; Exists = $true; Record = $record; Mode = [string](Get-ObjectValue -Object $record -Name 'mode' -Default '')
        Failure = [string](Get-ObjectValue -Object $record -Name 'failure' -Default '')
        SetTab = Get-ObjectValue -Object $record -Name 'setTab' -Default $null
        SetBrowse = Get-ObjectValue -Object $record -Name 'setBrowse' -Default $null
        SetOpen = Get-ObjectValue -Object $record -Name 'setOpen' -Default $null
        SetNote = Get-ObjectValue -Object $record -Name 'setNote' -Default $null
        SetCheck = Get-ObjectValue -Object $record -Name 'setCheck' -Default $null
        Online = Get-ObjectValue -Object $record -Name 'online' -Default $null
        Modals = @($modals); Logs = ($logText -join "`r`n")
    }
}

# 把一次用例里"人说出来的话"拼起来（面板 / 弹窗 / 日志）——只用来判"有没有说到"
# 安全截断：`$text.Substring(0, 12)` 在字符串比 12 短时会抛异常。
# E-12..E-19 的详情行大量用它，合成世界的摘要又是短的 —— 一旦抛就**整支断言提前结束**，
# 表现成"所有场景都失败"（实测踩过），所以统一走这个函数。
function Get-ShortText
{
    param([AllowEmptyString()][string]$Text, [int]$Max = 12)

    if (-not $Text) { return '<空>' }
    if ($Text.Length -le $Max) { return $Text }
    return $Text.Substring(0, $Max)
}

# 只属于这一次用例的日志尾部（driver 会写 toolLogTails；老记录没有这个字段时退回整份日志）
function Get-SetLogTail
{
    param($Run)

    $parts = New-Object System.Collections.ArrayList
    $tails = @(Get-ObjectValue -Object $Run.Record -Name 'toolLogTails' -Default @())
    foreach ($item in $tails) { [void]$parts.Add([string](Get-ObjectValue -Object $item -Name 'Text' -Default '')) }
    if ($parts.Count -gt 0) { return ($parts -join "`r`n") }
    return [string]$Run.Logs
}

# 在线更新用例"本条自己的话"= **结论文字**（状态 + 弹窗），**不掺**两样东西：
#   ① 累积日志（上一条用例写下的"档案摘要不符"会让这一条假通过 —— S-1 的教训）；
#   ② **远端列表行**（`<索引会社> / <索引作品> / N 条 / 状态`）。
# ② 是 **N-S2**（2026-10-16 独立验证发现）：列表行里**永远**带着索引那侧的会社/作品，
# 所以任何"要求出现索引名"的判据读它都是**恒真**的 —— 名字不一致那条用例因此会在
# "状态文字只点名了档案那一侧"时照样报绿（验证者的 `meta-only-synthetic`）。
# 需要列表内容的判据（E-18 正例）直接读 `listItemsAfterRefresh` 字段，不走这里。
function Get-OnlineCaseText
{
    param($Run)

    $parts = New-Object System.Collections.ArrayList
    $online = Get-ObjectValue -Object $Run -Name 'Online' -Default $null
    foreach ($round in @(Get-ObjectValue -Object $online -Name 'rounds' -Default @()))
    {
        [void]$parts.Add([string](Get-ObjectValue -Object $round -Name 'statusAfterRefresh' -Default ''))
        [void]$parts.Add([string](Get-ObjectValue -Object $round -Name 'statusAfterInstall' -Default ''))
    }
    foreach ($modal in @($Run.Modals)) { [void]$parts.Add(('{0} {1}' -f $modal.Title, $modal.Text)) }
    return ($parts -join "`r`n")
}

function Get-SetEvidenceText
{
    param($Run)

    $parts = New-Object System.Collections.ArrayList
    if ($Run.Failure) { [void]$parts.Add([string]$Run.Failure) }
    foreach ($sectionName in @('SetTab', 'SetBrowse', 'SetOpen', 'SetNote', 'SetCheck'))
    {
        $section = Get-ObjectValue -Object $Run -Name $sectionName -Default $null
        if (-not $section) { continue }
        foreach ($field in @('pageText', 'pageBeforeSelect', 'panelBefore', 'panelAfter', 'pageTextAfterClose'))
        {
            $value = [string](Get-ObjectValue -Object $section -Name $field -Default '')
            if ($value) { [void]$parts.Add($value) }
        }
    }
    $online = Get-ObjectValue -Object $Run -Name 'Online' -Default $null
    if ($online)
    {
        foreach ($round in @(Get-ObjectValue -Object $online -Name 'rounds' -Default @()))
        {
            [void]$parts.Add([string](Get-ObjectValue -Object $round -Name 'statusAfterRefresh' -Default ''))
            [void]$parts.Add([string](Get-ObjectValue -Object $round -Name 'statusAfterInstall' -Default ''))
            foreach ($item in @(Get-ObjectValue -Object $round -Name 'listItemsAfterRefresh' -Default @())) { [void]$parts.Add([string]$item) }
        }
    }
    foreach ($modal in @($Run.Modals)) { [void]$parts.Add(('{0} {1}' -f $modal.Title, $modal.Text)) }
    [void]$parts.Add([string]$Run.Logs)
    return ($parts -join "`r`n")
}

# 在"面板文字"里取一个字段值（形如 `会社：YuaHarness`）。取不到返回空串。
function Get-PanelFieldValue
{
    param([AllowEmptyString()][string]$Text, [Parameter(Mandatory)][string]$Label)

    if (-not $Text) { return '' }
    # 行分隔符不能假定是 \n：RichEdit 经 WM_GETTEXT 读回来的可能是**纯 CR**，
    # 那样 `(?m)^` 只在整段开头匹配，所有字段都会读成空（实测踩过）。
    $match = [regex]::Match($Text, ('(?:^|[\r\n])\s*' + [regex]::Escape($Label) + '[:：]\s*(?<value>[^\r\n]*)'))
    if (-not $match.Success) { return '' }
    return $match.Groups['value'].Value.Trim()
}

# 断言辅助：要求一段文字里**必须出现**某串（返回问题描述，空数组 = 都出现了）
function Test-TextContains
{
    param([AllowEmptyString()][string]$Text, [string[]]$Needles, [string]$Where, [switch]$Forbid)

    $problems = New-Object System.Collections.ArrayList
    foreach ($needle in $Needles)
    {
        $hit = $Text -like ('*' + $needle + '*')
        if ($Forbid)
        {
            if ($hit) { [void]$problems.Add(('{0}里**不该**出现「{1}」' -f $Where, $needle)) }
        }
        elseif (-not $hit)
        {
            [void]$problems.Add(('{0}里没有出现「{1}」' -f $Where, $needle))
        }
    }
    return @($problems)
}

Trace-Step 'E-12..E-19：读取用例结果'

$setTabRun = Get-SetRun $SetTabRunJson '页签骨架'
$setEmptyRun = Get-SetRun $SetEmptyRunJson '空库'
$setOneRun = Get-SetRun $SetOneRunJson '1 条'
$setMultiRun = Get-SetRun $SetMultiRunJson '多会社多作品'
$setNoteRun = Get-SetRun $SetNoteRunJson '扩展集留言'
$openRuns = [ordered]@{}
foreach ($pair in ($SetOpenRunJsonCsv -split ';'))
{
    if (-not $pair.Trim()) { continue }
    $parts = $pair -split '='
    if ($parts.Count -lt 2) { continue }
    $openRuns[$parts[0].Trim()] = Get-SetRun $parts[1].Trim() ('打开选中扩展集/' + $parts[0].Trim())
}
$checkRuns = [ordered]@{}
foreach ($pair in ($SetCheckRunJsonCsv -split ';'))
{
    if (-not $pair.Trim()) { continue }
    $parts = $pair -split '='
    if ($parts.Count -lt 2) { continue }
    $checkRuns[$parts[0].Trim()] = Get-SetRun $parts[1].Trim() ('运行环境自检/' + $parts[0].Trim())
}
$onlineSummary = Read-JsonFile -Path $OnlineSummaryJson
$onlineCases = [ordered]@{}
if ($onlineSummary)
{
    foreach ($case in @(Get-ObjectValue -Object $onlineSummary -Name 'cases' -Default @()))
    {
        $name = [string](Get-ObjectValue -Object $case -Name 'name' -Default '')
        if ($name) { $onlineCases[$name] = $case }
    }
}

# ==================================================================
# E-12 页签骨架与可见性（4003：该可见的可见、该隐藏的隐藏、切回去必须全隐）
# ==================================================================
#
# 判据从**真实界面**取（driver 切页签时逐个控件记 visible），不是从设计稿抄：
#   · set-1 / set-2 两次切进来的可见集合必须一致（页签状态与"怎么到达"无关）
#   · 切回首页后 4130-4136 必须**全隐**（lead 冒烟要求的那一路）
# 像素那一半（幽灵残留）不在这里判，见 E-12-a 与下面调用 Invoke-SetTabPixelChecks。

Trace-Step 'E-12：页签骨架与可见性'
$e12Details = @()
$e12Ok = $true
$setTabVisible = @(4130..4136) + @(4080, 4081, 4082, 4083, 4084, 4071)
# 该隐藏的：制作页签那一整组 + 首页那一整组（含目标游戏行 / 任务进度 / 主流程三步 / 概览 / 日志视图）
$setTabHidden = @(4100..4111) + @(4010, 4020, 4021, 4022) + @(4030, 4031, 4032) + @(4050, 4051, 4052) +
    @(4085, 4086, 4087, 4088) + @(4070, 4073)

if (-not $setTabRun.Exists)
{
    $e12Ok = $false
    $e12Details += ("[FAIL] 页签骨架用例结果不存在：{0}（run-extension-online.ps1 会产出它）" -f $SetTabRunJson)
}
elseif ($setTabRun.Failure)
{
    $e12Ok = $false
    $e12Details += ("[FAIL] 页签骨架用例自己失败了：{0}" -f $setTabRun.Failure)
}
else
{
    $passes = @(Get-ObjectValue -Object $setTabRun.SetTab -Name 'passes' -Default @())
    $byName = @{}
    foreach ($pass in $passes) { $byName[[string](Get-ObjectValue -Object $pass -Name 'name' -Default '?')] = $pass }
    $e12Details += ("三路取样：{0}" -f (($byName.Keys) -join ' / '))
    if (-not $byName.ContainsKey('set-1') -or -not $byName.ContainsKey('home-back') -or -not $byName.ContainsKey('set-2'))
    {
        $e12Ok = $false
        $e12Details += '[FAIL] 三路取样不全（要 set-1 / home-back / set-2）'
    }
    else
    {
        $set1Visible = @(Get-ObjectValue -Object $byName['set-1'] -Name 'Visible' -Default @() | ForEach-Object { [int]$_ })
        $set2Visible = @(Get-ObjectValue -Object $byName['set-2'] -Name 'Visible' -Default @() | ForEach-Object { [int]$_ })
        # set-3 = 从「关于」页签切回来的那一次（顺带的"经过去别页再回来"对照，S-3 的附带产物）
        $set3Visible = @()
        if ($byName.ContainsKey('set-3')) { $set3Visible = @(Get-ObjectValue -Object $byName['set-3'] -Name 'Visible' -Default @() | ForEach-Object { [int]$_ }) }
        $homeVisible = @(Get-ObjectValue -Object $byName['home-back'] -Name 'Visible' -Default @() | ForEach-Object { [int]$_ })
        $e12Details += ("set-1 可见 {0} 个：{1}" -f $set1Visible.Count, ($set1Visible -join ' '))
        $e12Details += ("set-2 可见 {0} 个；home-back 可见 {1} 个" -f $set2Visible.Count, $homeVisible.Count)

        Assert-LoopBudget -Label 'E-12 可见性矩阵' -Count ($setTabVisible.Count + $setTabHidden.Count)
        $missingVisible = @($setTabVisible | Where-Object { $_ -notin $set1Visible })
        $stubbornVisible = @($setTabHidden | Where-Object { $_ -in $set1Visible })
        if ($missingVisible.Count -gt 0)
        {
            $e12Ok = $false
            $e12Details += ('[FAIL] 切到「扩展集」页签后，这些控件该可见却不可见：' + ($missingVisible -join ' '))
        }
        if ($stubbornVisible.Count -gt 0)
        {
            $e12Ok = $false
            $e12Details += ('[FAIL] 切到「扩展集」页签后，这些控件该隐藏却仍可见：' + ($stubbornVisible -join ' '))
        }
        foreach ($pair in @(@('set-2', $set2Visible), @('set-3（去过关于页签再回来）', $set3Visible)))
        {
            if ($pair[1].Count -eq 0) { continue }
            $diff = @(Compare-Object -ReferenceObject $set1Visible -DifferenceObject $pair[1])
            if ($diff.Count -gt 0)
            {
                $e12Ok = $false
                $e12Details += ('[FAIL] {0} 与 set-1 的可见集合不一致（页签状态与到达路径有关？）：{1}' -f `
                        $pair[0], (($diff | ForEach-Object { '{0}{1}' -f $_.SideIndicator, $_.InputObject }) -join ' '))
            }
        }
        # 「扩展集选择」整组（4080-4084）是首页与扩展集页签**共用**的（§8.5），切回首页本来就该可见；
        # 真正要判的是本页签**自己**的 4130-4136 在首页必须全隐（实测首页可见集里确实没有它们）。
        $sharedIds = @(4080, 4081, 4082, 4083, 4084)
        $backNotHidden = @($setTabVisible | Where-Object { $_ -notin $sharedIds -and $_ -in $homeVisible })
        if ($backNotHidden.Count -gt 0)
        {
            $e12Ok = $false
            $e12Details += ('[FAIL] 切回首页后，扩展集页签自己的控件没有全隐：' + ($backNotHidden -join ' '))
        }
        if ($homeVisible.Count -eq 0)
        {
            $e12Ok = $false
            $e12Details += '[FAIL] 切回首页后一个控件都不可见 —— 多半是页签根本没切回去（可见性矩阵不可判读）'
        }
    }
}
Add-Check -Id 'E-12' -Title 'E-12 页签骨架：4130-4136 与共用列表/详情可见，首页与制作页签控件全隐，切回去同样全隐' -Ok $e12Ok -Details $e12Details

# ---------- E-12-b：「关于」页签（4004）的骨架与正文（S-3：最便宜的一条覆盖，不单开一组）----------
#
# 为什么要有它：本轮 LayoutWindow 的分派刚被改过（新增 setTab 分支，About 落到
# `if (!homeTab && !makeTab && !setTab)` 那一路）。若分支写错，PAGE 的可见性/宽度会坏，
# 而此前**没有任何断言会响**（4004 在断言脚本里零覆盖）。
# 期望可见/隐藏由 ShowTabControls 的分组推出（与 E-12 的实测矩阵同一来源）：
#   About 上只有 页签按钮 + 4071（内容视图）+ 4072（日志按钮）该可见；首页/制作/扩展集三组一律隐藏。
$expectAboutVisible = @(4001, 4002, 4003, 4004, 4071, 4072)
$expectAboutHidden = @(4130..4136) + @(4080..4084) + @(4100..4111) + @(4030, 4031, 4032) + @(4050, 4051, 4052) +
    @(4085, 4086, 4087, 4088) + @(4010, 4020, 4021, 4022, 4070)

Trace-Step 'E-12-b：「关于」页签骨架与正文'
$e12bDetails = @()
$e12bOk = $true
if (-not $setTabRun.Exists)
{
    $e12bOk = $false
    $e12bDetails += ("[FAIL] 页签骨架用例结果不存在：{0}" -f $SetTabRunJson)
}
elseif (-not $setTabRun.SetTab -or -not (Get-ObjectValue -Object $setTabRun.SetTab -Name 'about' -Default $null))
{
    $e12bOk = $false
    $e12bDetails += '[FAIL] 记录里没有「关于」页签的取样（driver 的 settab 步骤没跑到那一段）'
}
else
{
    $about = Get-ObjectValue -Object $setTabRun.SetTab -Name 'about' -Default $null
    $aboutVisible = @(Get-ObjectValue -Object $about -Name 'visible' -Default @() | ForEach-Object { [int]$_ })
    $aboutText = [string](Get-ObjectValue -Object $about -Name 'pageText' -Default '')
    $fingerprint = [string](Get-ObjectValue -Object $about -Name 'appFingerprint' -Default '')
    $exeSize = [int64](Get-ObjectValue -Object $about -Name 'appExeSize' -Default 0)

    Assert-LoopBudget -Label 'E-12-b 关于页签可见性' -Count (@($expectAboutVisible).Count + @($expectAboutHidden).Count)
    $missingAbout = @($expectAboutVisible | Where-Object { $_ -notin $aboutVisible })
    $leakedAbout = @($expectAboutHidden | Where-Object { $_ -in $aboutVisible })
    if ($missingAbout.Count -gt 0)
    {
        $e12bOk = $false
        $e12bDetails += ('[FAIL] 「关于」页签上这些控件该可见却不可见：' + ($missingAbout -join ' '))
    }
    if ($leakedAbout.Count -gt 0)
    {
        $e12bOk = $false
        $e12bDetails += ('[FAIL] 「关于」页签上这些控件该隐藏却仍可见：' + ($leakedAbout -join ' '))
    }
    $e12bDetails += ("关于页签可见 {0} 个：{1}" -f $aboutVisible.Count, ($aboutVisible -join ' '))

    # 正文：构建指纹栏必须报**这次跑的这一个 exe**，加上免责声明（定位口径）
    if ($aboutText.Length -lt 200)
    {
        $e12bOk = $false
        $e12bDetails += ('[FAIL] 「关于」页签正文只有 {0} 字 —— 多半是页面没渲染出来' -f $aboutText.Length)
    }
    $contentProblems = @(Test-TextContains -Text $aboutText -Needles @('构建指纹', 'SHA-256', '免责声明', '不要用于传播游戏资源') -Where '「关于」页签正文')
    if ($contentProblems.Count -gt 0) { $e12bOk = $false; $e12bDetails += $contentProblems }
    if (-not $fingerprint)
    {
        $e12bOk = $false
        $e12bDetails += '[FAIL] 用例记录里没有被测 exe 的指纹（driver 没记 appFingerprint）'
    }
    elseif ($aboutText -notlike ('*' + $fingerprint + '*'))
    {
        $e12bOk = $false
        $e12bDetails += ('[FAIL] 「关于」页签的构建指纹栏里没有这次跑的那个 exe 的指纹 {0}（"这轮绿属于哪个构建"必须能在页面上读到）' -f $fingerprint)
    }
    if ($exeSize -gt 0 -and $aboutText -notlike ('*' + "$exeSize" + '*'))
    {
        $e12bOk = $false
        $e12bDetails += ('[FAIL] 「关于」页签里没有这次跑的那个 exe 的大小 {0} 字节' -f $exeSize)
    }
    if ($e12bOk) { $e12bDetails += ('正文 {0} 字，含构建指纹 {1} 与免责声明' -f $aboutText.Length, $fingerprint) }
}
Add-Check -Id 'E-12-b' -Title 'E-12-b 「关于」页签（4004）：骨架正确（只有页签+内容视图+日志按钮可见）且正文有构建指纹与免责声明' -Ok $e12bOk -Details $e12bDetails

# ---------- E-12-a（像素口径）：扩展集页签的画面必须与"怎么到达"无关 ----------
if (-not $SkipSetPixels)
{
    Trace-Step ("E-12-a：读扩展集页签抓图快照 {0}" -f $SetPixelsJson)
    [void](Invoke-SetTabPixelChecks -SnapshotJson $SetPixelsJson -DiffDir $SetPixelsDiffDir -RequireSnapshot $false)
    Trace-Step 'E-12-a：像素比对完成'
}
else
{
    Add-Check -Id 'E-12-a' -Title '扩展集页签像素幽灵回归（-SkipSetPixels：本项未执行）' -Ok $true -Severity 'info' `
        -Details @('调用方显式要求跳过 E-12-a —— 本次结论里**没有**覆盖扩展集页签的像素残留，别当成"验证过"')
}

# ==================================================================
# E-13 刷新列表：空库 / 1 条 / 多会社多作品
# ==================================================================
#
# 期望值 = **断言自己解析**那份库清单快照（driver 在驱动当时拷的），不是产品说的数字。
# 另外单独挡一类假通过：列表**读不到文字**时计数会变成 0，看起来"空库用例通过"。

Trace-Step 'E-13：刷新列表（空库 / 1 条 / 多会社多作品）'
$e13Details = @()
$e13Ok = $true
$e13Cases = @(
    [pscustomobject]@{ Name = '空库'; Run = $setEmptyRun; ExpectPublishers = 0; ExpectEntries = 0 }
    [pscustomobject]@{ Name = '1 条'; Run = $setOneRun; ExpectPublishers = $null; ExpectEntries = $null }
    [pscustomobject]@{ Name = '多会社多作品'; Run = $setMultiRun; ExpectPublishers = $null; ExpectEntries = $null }
)
foreach ($case in $e13Cases)
{
    $run = $case.Run
    if (-not $run.Exists) { $e13Ok = $false; $e13Details += ("[FAIL] {0}：用例结果不存在（{1}）" -f $case.Name, $run.Path); continue }
    if ($run.Failure) { $e13Ok = $false; $e13Details += ("[FAIL] {0}：用例自己失败了：{1}" -f $case.Name, $run.Failure); continue }
    $browse = $run.SetBrowse
    if (-not $browse) { $e13Ok = $false; $e13Details += ("[FAIL] {0}：记录里没有 setBrowse（步骤没跑到？）" -f $case.Name); continue }

    $manifestText = [string](Get-ObjectValue -Object $browse -Name 'libraryManifestText' -Default '')
    $manifest = ConvertFrom-ManifestText $manifestText
    $rows = @($manifest.Rows)
    $panel = [string](Get-ObjectValue -Object $browse -Name 'pageText' -Default '')
    $pubCount = [int](Get-ObjectValue -Object $browse -Name 'publisherCount' -Default -1)
    $titleCount = [int](Get-ObjectValue -Object $browse -Name 'titleCount' -Default -1)
    $publishers = @(Get-ObjectValue -Object $browse -Name 'publishers' -Default @() | ForEach-Object { [string]$_ } | Where-Object { $_ })
    $titles = @(Get-ObjectValue -Object $browse -Name 'titles' -Default @() | ForEach-Object { [string]$_ } | Where-Object { $_ })
    $expectPublishers = @($rows | ForEach-Object { $_.Publisher } | Sort-Object -Unique)
    $selectedPublisher = [string](Get-ObjectValue -Object $browse -Name 'selectedPublisher' -Default '')
    $expectTitles = @($rows | Where-Object { $_.Publisher -eq $selectedPublisher } | ForEach-Object { $_.Title } | Sort-Object -Unique)

    $e13Details += ("{0}：清单独立解析 {1} 行 / {2} 个会社；界面报 会社 {3} 个、作品 {4} 个；列表文字 [{5}] / [{6}]" -f `
            $case.Name, $rows.Count, $expectPublishers.Count, $pubCount, $titleCount, ($publishers -join ','), ($titles -join ','))

    if ($case.ExpectPublishers -ne $null -and $pubCount -ne [int]$case.ExpectPublishers)
    {
        $e13Ok = $false
        $e13Details += ('[FAIL] {0}：会社数 {1} ≠ 期望 {2}' -f $case.Name, $pubCount, $case.ExpectPublishers)
    }
    if ($pubCount -ne $expectPublishers.Count)
    {
        $e13Ok = $false
        $e13Details += ('[FAIL] {0}：界面会社数 {1} ≠ 独立解析清单得出的 {2}' -f $case.Name, $pubCount, $expectPublishers.Count)
    }
    $actualPublisherText = (@($publishers) -join "`n")
    $expectPublisherText = (@($expectPublishers) -join "`n")
    if ($actualPublisherText -ne $expectPublisherText)
    {
        $e13Ok = $false
        $e13Details += ('[FAIL] {0}：会社列表内容与清单不一致（界面 [{1}] vs 清单 [{2}]）' -f $case.Name, ($publishers -join ','), ($expectPublishers -join ','))
    }
    if ($expectPublishers.Count -gt 0)
    {
        if ($titleCount -ne $expectTitles.Count)
        {
            $e13Ok = $false
            $e13Details += ('[FAIL] {0}：选中会社「{1}」的作品数 {2} ≠ 清单里的 {3}' -f $case.Name, $selectedPublisher, $titleCount, $expectTitles.Count)
        }
        $actualTitleText = (@($titles) -join "`n")
        $expectTitleText = (@($expectTitles) -join "`n")
        if ($actualTitleText -ne $expectTitleText)
        {
            $e13Ok = $false
            $e13Details += ('[FAIL] {0}：作品列表内容与清单不一致（界面 [{1}] vs 清单 [{2}]）' -f $case.Name, ($titles -join ','), ($expectTitles -join ','))
        }
        # 面板的"共 N 个会社 / M 份扩展集"必须与独立解析一致
        $match = [regex]::Match($panel, '共\s*(?<pub>\d+)\s*个会社\s*/\s*(?<entry>\d+)\s*份扩展集')
        if (-not $match.Success)
        {
            $e13Ok = $false
            $e13Details += ('[FAIL] {0}：面板里没有「共 N 个会社 / M 份扩展集」这一行' -f $case.Name)
        }
        else
        {
            $panelPub = [int]$match.Groups['pub'].Value
            $panelEntry = [int]$match.Groups['entry'].Value
            if ($panelPub -ne $expectPublishers.Count -or $panelEntry -ne $rows.Count)
            {
                $e13Ok = $false
                $e13Details += ('[FAIL] {0}：面板报 {1} 个会社 / {2} 份，独立解析是 {3} 个会社 / {4} 份' -f `
                        $case.Name, $panelPub, $panelEntry, $expectPublishers.Count, $rows.Count)
            }
        }
    }
    else
    {
        $problems = @(Test-TextContains -Text $panel -Needles @('一份扩展集都没有') -Where '面板')
        if ($problems.Count -gt 0) { $e13Ok = $false; $e13Details += $problems }
    }
}
Add-Check -Id 'E-13' -Title 'E-13 刷新列表：空库 / 1 条 / 多会社多作品，4083/4084 内容与计数与独立解析的库清单一致' -Ok $e13Ok -Details $e13Details

# ==================================================================
# E-14 详情面板：选中项每个字段与「清单 + 独立解析的档案」逐项一致
# ==================================================================
#
# 面板只是个显示层，**判它必须拿独立算出来的值去比**：
#   会社 / 作品 / 条数 / 目标 exe 摘要 / 盐摘要 / 档案摘要 / 时间 / 来源 ← 清单行
#   会社 / 作品 / 目标 exe 摘要 / 条数 ← 档案本体（rules-int.py 独立 dump）
# 两边还要互相印证（清单行里的摘要必须真的等于档案的实际值），否则"清单写了什么就显示什么"
# 这种"显示层正确、数据层错"的情况会被漏掉。

Trace-Step 'E-14：详情面板逐项一致'
$e14Details = @()
$e14Ok = $true
if (-not $setOneRun.Exists -or -not $setOneRun.SetBrowse)
{
    $e14Ok = $false
    $e14Details += ("[FAIL] 1 条库的用例结果不存在/不完整：{0}" -f $SetOneRunJson)
}
else
{
    $browse = $setOneRun.SetBrowse
    $manifestPath = [string](Get-ObjectValue -Object $browse -Name 'manifestSnapshot' -Default '')
    $archivePath = [string](Get-ObjectValue -Object $browse -Name 'archiveSnapshot' -Default '')
    $panel = [string](Get-ObjectValue -Object $browse -Name 'pageText' -Default '')
    $manifestText = [string](Get-ObjectValue -Object $browse -Name 'libraryManifestText' -Default '')
    $manifest = ConvertFrom-ManifestText $manifestText
    $selectedPublisher = [string](Get-ObjectValue -Object $browse -Name 'selectedPublisher' -Default '')
    $selectedTitle = [string](Get-ObjectValue -Object $browse -Name 'selectedTitle' -Default '')

    $row = @($manifest.Rows | Where-Object { $_.Publisher -eq $selectedPublisher -and $_.Title -eq $selectedTitle })
    $facts = Get-ArchiveFacts -Path $archivePath -Tag 'e14-selected'

    if ($row.Count -ne 1)
    {
        $e14Ok = $false
        $e14Details += ('[FAIL] 清单里「{0} / {1}」对应的数据行有 {2} 条（应恰好 1 条）' -f $selectedPublisher, $selectedTitle, $row.Count)
    }
    if (-not $facts)
    {
        $e14Ok = $false
        $e14Details += ('[FAIL] 独立解析选中档案失败（快照 {0}；清单快照 {1}）' -f $archivePath, $manifestPath)
    }
    if ($e14Ok)
    {
        $entry = $row[0]
        # ① 清单行 ↔ 档案本体（数据层自洽：清单说的必须真是档案的事实）
        $crossChecks = @(
            [pscustomobject]@{ What = '会社'; List = $entry.Publisher; Archive = $facts.Publisher }
            [pscustomobject]@{ What = '作品'; List = $entry.Title; Archive = $facts.Title }
            [pscustomobject]@{ What = '目标 exe 摘要'; List = $entry.ExeDigest; Archive = $facts.ExeDigest }
            [pscustomobject]@{ What = '盐摘要'; List = $entry.SaltDigest; Archive = $facts.SaltDigest }
            [pscustomobject]@{ What = '档案摘要'; List = $entry.ArchiveDigest; Archive = $facts.Sha256 }
            [pscustomobject]@{ What = '条数'; List = $entry.EntryCount; Archive = "$($facts.EntryCount)" }
        )
        foreach ($check in $crossChecks)
        {
            if ([string]$check.List -ne [string]$check.Archive)
            {
                $e14Ok = $false
                $e14Details += ('[FAIL] 数据层不一致：{0} 清单写「{1}」，档案独立解析是「{2}」' -f $check.What, $check.List, $check.Archive)
            }
        }
        if ($facts.ExceptionCount -ne 0)
        {
            $e14Ok = $false
            $e14Details += ('[FAIL] 选中档案有 {0} 条生成期例外 —— 库里不该有这种东西（安装前应当拦住）' -f $facts.ExceptionCount)
        }

        # ② 面板 ↔ 上面那两份事实（每一项都要在面板里出现，且是同一个值）
        $short = { param($digest) if (-not $digest) { '（未记录）' } elseif ($digest.Length -gt 16) { $digest.Substring(0, 16) + '…' } else { $digest } }
        $expectPairs = @(
            @{ Label = '会社'; Value = $entry.Publisher }
            @{ Label = '作品'; Value = $entry.Title }
            @{ Label = '条数'; Value = ('{0} 条' -f $entry.EntryCount) }
            @{ Label = '目标 exe 摘要'; Value = (& $short $entry.ExeDigest) }
            @{ Label = '盐摘要'; Value = (& $short $entry.SaltDigest) }
            @{ Label = '档案摘要'; Value = (& $short $entry.ArchiveDigest) }
            @{ Label = '生成时间'; Value = $(if ($entry.GeneratedAt) { $entry.GeneratedAt } else { '（未记录）' }) }
            @{ Label = '安装时间'; Value = $(if ($entry.InstalledAt) { $entry.InstalledAt } else { '（未记录）' }) }
            @{ Label = '来源'; Value = $(if ($entry.Source) { $entry.Source } else { '（未记录）' }) }
        )
        foreach ($pair in $expectPairs)
        {
            $shown = Get-PanelFieldValue -Text $panel -Label $pair.Label
            if ($shown -ne $pair.Value)
            {
                $e14Ok = $false
                $e14Details += ('[FAIL] 面板「{0}」显示「{1}」，独立算出来是「{2}」' -f $pair.Label, $shown, $pair.Value)
            }
        }
        # 库内目录 / 档案路径：面板里必须出现**真的那个路径**。
        # 两个要点：
        #   ① 用「库里的真身」而不是快照副本：面板显示的是库内路径，拿快照路径去比必然对不上；
        #   ② 拼路径必须走 **ConvertTo-SafePathComponent**（与产品的 MakeSafePathComponent 同口径）——
        #      产品的落库目录是 sanitize 之后的形态，拿 manifest 里的**原始名**拼会指向一个
        #      不存在的路径（夹具用普通名字时碰巧相等，一旦名字会被改写就假红/假绿）。见 E-14-b。
        $libraryRootForPanel = [string](Get-ObjectValue -Object $browse -Name 'libraryRoot' -Default '')
        $entryDirectoryForPanel = Join-Path (Join-Path $libraryRootForPanel (ConvertTo-SafePathComponent $entry.Publisher)) `
            (ConvertTo-SafePathComponent $entry.Title)
        $libraryArchiveForPanel = Join-Path $entryDirectoryForPanel 'rules.int'
        foreach ($needle in @($libraryRootForPanel, $libraryArchiveForPanel))
        {
            if ($needle -and $panel -notlike ('*' + $needle + '*'))
            {
                $e14Ok = $false
                $e14Details += ('[FAIL] 面板里没有出现真实路径「{0}」' -f $needle)
            }
        }
        $e14Details += ("逐项比对完成：{0} / {1}；档案 {2} 字节 sha256 {3}…；DIRS {4} / FILES {5}" -f `
                $entry.Publisher, $entry.Title, $facts.Size, $facts.Sha256.Substring(0, 16), $facts.DirCount, $facts.FileCount)
    }
}
Add-Check -Id 'E-14' -Title 'E-14 详情面板：选中项的会社/作品/条数/摘要/时间/来源与「清单 + 独立解析的档案」逐项一致' -Ok $e14Ok -Details $e14Details

# ==================================================================
# E-14-b 名字会被改写时，「库内目录」必须是**磁盘上真实存在的那一级**（P-4 的口径）
# ==================================================================
#
# 为什么单列（2026-10-16 协调者要求）：E-14 的路径判据原来拿 manifest 里的**原始名**拼 ——
# 夹具用普通名字时 raw == sanitize 后，判据碰巧成立；一旦名字里有 `:` `/` 这类字符，
# 产品落库的是被改写的目录名（`A:B` → `AB`），拿原始名拼出来的路径**根本不存在**，
# 判据要么假红、要么两边都错成一样而假绿。这一条**真的跑一遍会被改写的名字**：
#   ① 面板显示的「库内目录」必须等于 sanitize 后的路径，且**磁盘上真的有这个目录**（逐字符 + Test-Path）；
#   ② 面板必须给出「库里的目录名是改写过的」这类说明（P-4）；
#   ③ 面板绝不许出现"拿原始名拼出来"的那条路径（它不存在）。
$e14bDetails = @()
$e14bOk = $true
$hostileRun = Get-SetRun $SetHostileRunJson '改名后的落库目录'
if (-not $hostileRun.Exists -or -not $hostileRun.SetBrowse)
{
    $e14bOk = $false
    $e14bDetails += ('[FAIL] 改名用例结果不存在/不完整：{0}（编排里的 hostile-install + set-hostile）' -f $SetHostileRunJson)
}
else
{
    $browse = $hostileRun.SetBrowse
    $panel = [string](Get-ObjectValue -Object $browse -Name 'pageText' -Default '')
    $libraryRoot = [string](Get-ObjectValue -Object $browse -Name 'libraryRoot' -Default '')
    $rawPublisher = [string](Get-ObjectValue -Object $browse -Name 'selectedPublisher' -Default '')
    $rawTitle = [string](Get-ObjectValue -Object $browse -Name 'selectedTitle' -Default '')
    $safePublisher = ConvertTo-SafePathComponent $rawPublisher
    $safeTitle = ConvertTo-SafePathComponent $rawTitle
    $expectedDirectory = Join-Path (Join-Path $libraryRoot $safePublisher) $safeTitle
    $rawDirectory = Join-Path (Join-Path $libraryRoot $rawPublisher) $rawTitle

    $e14bDetails += ('选中项：原始名「{0} / {1}」→ 落库名「{2} / {3}」' -f $rawPublisher, $rawTitle, $safePublisher, $safeTitle)
    if ($safePublisher -eq $rawPublisher -and $safeTitle -eq $rawTitle)
    {
        $e14bOk = $false
        $e14bDetails += ('[FAIL] 用例无效：这条用例要的是"名字会被改写"（sanitize 后不同），实际原名与落库名相同（{0} / {1}）' -f $rawPublisher, $rawTitle)
    }
    $shown = Get-PanelFieldValue -Text $panel -Label '库内目录'
    if ($shown -ne $expectedDirectory)
    {
        $e14bOk = $false
        $e14bDetails += ('[FAIL] 面板「库内目录」显示「{0}」，按产品口径（sanitize 后拼）应当是「{1}」' -f $shown, $expectedDirectory)
    }
    elseif (-not (Test-Path -LiteralPath $shown))
    {
        $e14bOk = $false
        $e14bDetails += ('[FAIL] 面板显示的库内目录在磁盘上**不存在**：{0}' -f $shown)
    }
    else
    {
        $e14bDetails += ('面板的「库内目录」= 磁盘上真实存在的那一级：{0}' -f $shown)
    }
    $problems = @(Test-TextContains -Text $panel -Needles @('库里的目录名是改写过的') -Where '改名条目的面板')
    if ($problems.Count -gt 0)
    {
        $e14bOk = $false
        $e14bDetails += $problems
        $e14bDetails += '（P-4 要求：名字被改写时面板要说明"库里那两级是改写过的"，否则用户按面板去找目录会找不到）'
    }
    if ($rawDirectory -ne $expectedDirectory -and $panel -like ('*' + $rawDirectory + '*'))
    {
        $e14bOk = $false
        $e14bDetails += ('[FAIL] 面板里出现了**拿原始名拼出来的**路径（它不存在）：{0}' -f $rawDirectory)
    }
}
Add-Check -Id 'E-14-b' -Title 'E-14-b 名字会被改写时，「库内目录」是磁盘上真实存在的那一级（sanitize 口径）且有改写说明' -Ok $e14bOk -Details $e14bDetails

# ==================================================================
# E-15 打开选中扩展集：无选中 / 目录缺失 / 正常（只判路径文本，不判资源管理器窗口）
# ==================================================================
Trace-Step 'E-15：打开选中扩展集（三种路径）'
$e15Details = @()
$e15Ok = $true
$openNone = if ($openRuns.Contains('none')) { $openRuns['none'] } else { $null }
$openOk = if ($openRuns.Contains('ok')) { $openRuns['ok'] } else { $null }
$openMissing = if ($openRuns.Contains('missing')) { $openRuns['missing'] } else { $null }
foreach ($pair in @(@('none', $openNone), @('ok', $openOk), @('missing', $openMissing)))
{
    if (-not $pair[1] -or -not $pair[1].Exists)
    {
        $e15Ok = $false
        $e15Details += ('[FAIL] 「{0}」路径的用例结果不存在' -f $pair[0])
    }
    elseif ($pair[1].Failure)
    {
        $e15Ok = $false
        $e15Details += ('[FAIL] 「{0}」路径的用例自己失败了：{1}' -f $pair[0], $pair[1].Failure)
    }
}
if ($e15Ok)
{
    # ① 无选中：必须给出可操作提示，且不能假装打开成功
    $evidence = Get-SetEvidenceText $openNone
    $problems = @(Test-TextContains -Text $evidence -Needles @('先在左侧', '选一个会社') -Where '「无选中」的弹窗/面板')
    if ($problems.Count -gt 0) { $e15Ok = $false; $e15Details += $problems }
    $forbidden = @(Test-TextContains -Text $evidence -Needles @('已打开 ') -Where '「无选中」的弹窗/面板' -Forbid)
    if ($forbidden.Count -gt 0) { $e15Ok = $false; $e15Details += $forbidden }

    # ② 目录缺失：必须明确报"目录不存在"，且不许说"已打开"
    $evidence = Get-SetEvidenceText $openMissing
    $problems = @(Test-TextContains -Text $evidence -Needles @('没有目录') -Where '「目录缺失」的弹窗/面板')
    if ($problems.Count -gt 0) { $e15Ok = $false; $e15Details += $problems }
    $forbidden = @(Test-TextContains -Text $evidence -Needles @('已打开 ') -Where '「目录缺失」的弹窗/面板' -Forbid)
    if ($forbidden.Count -gt 0) { $e15Ok = $false; $e15Details += $forbidden }
    $expectedDirectory = [string](Get-ObjectValue -Object $openMissing.SetOpen -Name 'expectedDirectory' -Default '')
    if ($expectedDirectory -and $evidence -notlike ('*' + $expectedDirectory + '*'))
    {
        $e15Ok = $false
        $e15Details += ('[FAIL] 「目录缺失」的提示里没有点名那个目录：{0}' -f $expectedDirectory)
    }

    # ③ 正常打开：面板「最近操作」与日志里都必须出现「已打开 <路径>」，路径必须**逐字符相等**
    $section = $openOk.SetOpen
    $expectedDirectory = [string](Get-ObjectValue -Object $section -Name 'expectedDirectory' -Default '')
    $panel = [string](Get-ObjectValue -Object $section -Name 'panelAfter' -Default '')
    $logText = Get-SetLogTail $openOk
    if (-not $expectedDirectory)
    {
        $e15Ok = $false
        $e15Details += '[FAIL] 「正常打开」用例没记下期望目录'
    }
    else
    {
        $panelNeedle = '已打开 ' + $expectedDirectory
        if ($panel -notlike ('*' + $panelNeedle + '*'))
        {
            $e15Ok = $false
            $e15Details += ('[FAIL] 面板「最近操作」里没有「{0}」' -f $panelNeedle)
        }
        $logNeedle = '已打开扩展集目录：' + $expectedDirectory
        if ($logText -notlike ('*' + $logNeedle + '*'))
        {
            $e15Ok = $false
            $e15Details += ('[FAIL] 日志里没有「{0}」' -f $logNeedle)
        }
        $e15Details += ('期望目录（界面选中的会社/作品算出）：{0}' -f $expectedDirectory)
    }
    $e15Details += '判据说明：只判路径文本；**不判资源管理器窗口**（起没起 Explorer 是 ShellExecute 的事，混进产品结论里会冤枉实现）。'
}
Add-Check -Id 'E-15' -Title 'E-15 打开选中扩展集：无选中给提示、目录缺失明确报错、正常打开时面板与日志都写出正确的「已打开 <路径>」' -Ok $e15Ok -Details $e15Details

# ==================================================================
# E-16 运行环境自检：正常全绿；故意造坏 → 每一项都必须报出；自检不改任何文件
# ==================================================================
#
# 两个判据一起看：
#   ① **该报的必须报**（每个变体点名自己那一条）；
#   ② **自检是只读的**：库根与工具目录（除 Log）的树摘要前后必须逐字节相同。
# 变体与其期望（对照产品真实措辞，见 YuaExtractMainWindow.cpp 的 RunEnvironmentSelfCheck）：
#   clean             → 结论「全部正常」
#   bad-archive       → 「库内档案」那一项报「解码失败」
#   byte-flip         → 同上（改一个字节必然 CRC 不符）
#   manifest-malformed→ 清单那一项报「解析失败」或「不合规」（跳过并报告）
#   manifest-bad-digest→ 同上（"写了值但值是坏的" → 整份不可信）
#   library-readonly  → 库根那一项报「不可写」
#   missing-dll       → Hash 收集模块那一项报「找不到」

# 从自检结论里取出**某一项**那一段（`· 项名：…` 到下一个 `· ` 之前）。
# 面板与弹框都是这个形状（AppendSelfCheckRuns / SelfCheckPlainText）。
# 找不到那一项返回 $null —— 调用方要把它当成"那一项根本没报"。
function Get-SelfCheckItemBlock
{
    param([AllowEmptyString()][string]$Text, [Parameter(Mandatory)][string]$Label)

    if (-not $Text) { return $null }
    $lines = @([string]$Text -split "`r?`n")
    $start = -1
    for ($index = 0; $index -lt $lines.Count; $index++)
    {
        if ($lines[$index] -match ('^\s*(·|\*|-)?\s*' + [regex]::Escape($Label) + '[:：]')) { $start = $index; break }
    }
    if ($start -lt 0) { return $null }
    $block = New-Object System.Collections.ArrayList
    for ($index = $start; $index -lt $lines.Count; $index++)
    {
        if ($index -gt $start -and $lines[$index] -match '^\s*(·|\*|-)\s*\S') { break }
        [void]$block.Add($lines[$index])
    }
    return ($block -join "`r`n")
}

Trace-Step 'E-16：运行环境自检（正常 + 故意造坏）'
$e16Details = @()
$e16Ok = $true
# 每条变体给两个东西：
#   Needles —— 该报出来的措辞
#   Owner   —— 该由**哪一项**（自检面板里 `· <项名>：…` 那一段）报出来
# 为什么必须限定在"那一项自己的那一段"（照 S-1 的教训）：全篇找关键词的话，
# 别的项目/别的原因凑巧带同一个词，这条判据就会因为**错误的原因**通过。
$variantOwner = [ordered]@{
    'bad-archive'         = '库内档案'
    'byte-flip'           = '库内档案'
    'manifest-malformed'  = '库清单 manifest.txt'
    'manifest-bad-digest' = '库清单 manifest.txt'
    'library-readonly'    = '扩展集库目录'
    'missing-dll'         = 'Hash 收集模块 CxdecStringDumper.dll'
}
$variantExpect = [ordered]@{
    'clean'              = @('全部正常')
    'bad-archive'        = @('解码失败')
    'byte-flip'          = @('解码失败')
    # 清单坏行：产品有两条路 —— "列数不对/会社为空"是**跳过并报告**（面板「清单提示」写「不合规」），
    # "写了值但值是坏的"是**整份拒**（面板写「格式不对」）。两条都必须被自检报出来。
    'manifest-malformed' = @('不合规', '跳过', '格式不对', '解析失败')
    'manifest-bad-digest' = @('格式不对', '不合规', '解析失败')
    'library-readonly'   = @('不可写')
    'missing-dll'        = @('找不到')
}
$seenVariants = 0
foreach ($key in $checkRuns.Keys)
{
    $run = $checkRuns[$key]
    if (-not $run.Exists)
    {
        $e16Ok = $false
        $e16Details += ('[FAIL] 自检用例（{0}）结果不存在：{1}' -f $key, $run.Path)
        continue
    }
    if ($run.Failure)
    {
        $e16Ok = $false
        $e16Details += ('[FAIL] 自检用例（{0}）自己失败了：{1}' -f $key, $run.Failure)
        continue
    }
    foreach ($variant in @(Get-ObjectValue -Object $run.SetCheck -Name 'variants' -Default @()))
    {
        $seenVariants++
        $name = [string](Get-ObjectValue -Object $variant -Name 'variant' -Default '?')
        $mutationError = [string](Get-ObjectValue -Object $variant -Name 'mutationError' -Default '')
        $panel = [string](Get-ObjectValue -Object $variant -Name 'panelAfter' -Default '')
        $modalText = (@(Get-ObjectValue -Object $variant -Name 'modals' -Default @()) | ForEach-Object { '{0} {1}' -f (Get-ObjectValue -Object $_ -Name 'title' -Default ''), (Get-ObjectValue -Object $_ -Name 'text' -Default '') }) -join "`r`n"
        $evidence = $panel + "`r`n" + $modalText

        if ($mutationError)
        {
            $e16Ok = $false
            $e16Details += ('[FAIL] 变体 {0} 的"造坏"动作本身出错了（这一条没测到东西）：{1}' -f $name, $mutationError)
            continue
        }
        if (-not $variantExpect.Contains($name))
        {
            $e16Ok = $false
            $e16Details += ('[WARN→FAIL] 变体 {0} 不在已知清单里，判不出它该报什么' -f $name)
            continue
        }
        $needles = @($variantExpect[$name])
        $scope = $evidence
        $owner = if ($variantOwner.Contains($name)) { [string]$variantOwner[$name] } else { '' }
        if ($owner)
        {
            $block = Get-SelfCheckItemBlock -Text $evidence -Label $owner
            if (-not $block)
            {
                $e16Ok = $false
                $e16Details += ('[FAIL] 变体 {0}：自检结论里**根本没有「{1}」那一项**（该报的是它）' -f $name, $owner)
                $scope = ''
            }
            else { $scope = $block }
        }
        $hit = $false
        foreach ($needle in $needles) { if ($scope -like ('*' + $needle + '*')) { $hit = $true; break } }
        if ($scope -and -not $hit)
        {
            $e16Ok = $false
            $flat = ($scope -replace "`r?`n", ' / ')
            $e16Details += ('[FAIL] 变体 {0}：{1} 那一项里没有出现它该报的措辞 [{2}]（那一段：{3}）' -f `
                    $name, $owner, ($needles -join '/'), $flat.Substring(0, [Math]::Min(300, $flat.Length)))
        }
        elseif ($hit)
        {
            $e16Details += ('  变体 {0}：{1} 报出了该报的（改法：{2}）' -f $name, $(if ($owner) { $owner } else { '结论行' }), `
                    [string](Get-ObjectValue -Object $variant -Name 'mutation' -Default ''))
        }

        # 自检是只读的：库根与工具目录（除 Log）前后摘要必须相等
        $libBefore = Get-ObjectValue -Object $variant -Name 'libraryDigestBefore' -Default $null
        $libAfter = Get-ObjectValue -Object $variant -Name 'libraryDigestAfter' -Default $null
        $toolBefore = Get-ObjectValue -Object $variant -Name 'toolDigestBefore' -Default $null
        $toolAfter = Get-ObjectValue -Object $variant -Name 'toolDigestAfter' -Default $null
        foreach ($pair in @(@('库根', $libBefore, $libAfter), @('工具目录（除 Log）', $toolBefore, $toolAfter)))
        {
            $beforeDigest = [string](Get-ObjectValue -Object $pair[1] -Name 'Digest' -Default '')
            $afterDigest = [string](Get-ObjectValue -Object $pair[2] -Name 'Digest' -Default '')
            $beforeCount = [int](Get-ObjectValue -Object $pair[1] -Name 'Count' -Default -1)
            $afterCount = [int](Get-ObjectValue -Object $pair[2] -Name 'Count' -Default -1)
            # N-S1：目录维度也要比（只比文件时，"产品留下一个空目录"看不见）
            $beforeDirs = [int](Get-ObjectValue -Object $pair[1] -Name 'DirCount' -Default -1)
            $afterDirs = [int](Get-ObjectValue -Object $pair[2] -Name 'DirCount' -Default -1)
            if (-not $beforeDigest -or $beforeDigest -ne $afterDigest -or $beforeDirs -ne $afterDirs)
            {
                $e16Ok = $false
                $e16Details += ('[FAIL] 变体 {0}：自检**改了**{1}（前后摘要 {2} → {3}；文件数 {4} → {5}；**目录数 {6} → {7}**）' -f `
                        $name, $pair[0], (Get-ShortText $beforeDigest), (Get-ShortText $afterDigest), $beforeCount, $afterCount, $beforeDirs, $afterDirs)
            }
        }
    }
}
if ($seenVariants -eq 0)
{
    $e16Ok = $false
    $e16Details += '[FAIL] 一个自检变体都没跑到 —— 这一条什么都没验'
}
$e16Details += ('共 {0} 个变体；判据：① 该报的报出来 ② 自检前后库根与工具目录（除 Log）的树摘要逐字节相同' -f $seenVariants)
Add-Check -Id 'E-16' -Title 'E-16 运行环境自检：正常全绿、坏档案/坏字节/坏清单/库不可写/缺 DLL 逐项报出，且自检不改任何文件或目录' -Ok $e16Ok -Details $e16Details

# ==================================================================
# E-17 扩展集留言：弹窗内容含选中档案 META 的 comment（comment 独立解析）
# ==================================================================
Trace-Step 'E-17：扩展集留言弹窗'
$e17Details = @()
$e17Ok = $true
if (-not $setNoteRun.Exists)
{
    $e17Ok = $false
    $e17Details += ("[FAIL] 留言用例结果不存在：{0}" -f $SetNoteRunJson)
}
elseif ($setNoteRun.Failure)
{
    $e17Ok = $false
    $e17Details += ("[FAIL] 留言用例自己失败了：{0}" -f $setNoteRun.Failure)
}
else
{
    $archiveSnapshot = [string](Get-ObjectValue -Object $setNoteRun.SetBrowse -Name 'archiveSnapshot' -Default '')
    $facts = Get-ArchiveFacts -Path $archiveSnapshot -Tag 'e17-note'
    $dialogs = @(Get-ObjectValue -Object $setNoteRun.SetNote -Name 'dialogs' -Default @())
    $dialogText = (@($dialogs) | ForEach-Object { '{0} {1}' -f (Get-ObjectValue -Object $_ -Name 'title' -Default ''), (Get-ObjectValue -Object $_ -Name 'text' -Default '') }) -join "`r`n"
    if (-not $facts)
    {
        $e17Ok = $false
        $e17Details += ('[FAIL] 独立解析选中档案失败（快照 {0}）' -f $archiveSnapshot)
    }
    elseif (@($dialogs).Count -eq 0)
    {
        $e17Ok = $false
        $e17Details += '[FAIL] 点「扩展集留言」没有出现任何模态框（它应当是个只读弹窗）'
    }
    else
    {
        $problems = @(Test-TextContains -Text $dialogText -Needles @($facts.Comment) -Where '留言弹窗')
        if ($problems.Count -gt 0) { $e17Ok = $false; $e17Details += $problems }
        # 顺带：弹窗必须能说清"这是哪一份"，并且如实写明"名字对不对验不了"
        foreach ($needle in @($facts.Publisher, $facts.Title))
        {
            if ($dialogText -notlike ('*' + $needle + '*'))
            {
                $e17Ok = $false
                $e17Details += ('[FAIL] 留言弹窗里没有出现「{0}」' -f $needle)
            }
        }
        if ($dialogText -notlike '*无法校验*' -and $dialogText -notlike '*无法验证*')
        {
            $e17Ok = $false
            $e17Details += '[FAIL] 留言弹窗里没有「无法校验 / 无法验证」这类如实说明（§14.4 要求工具**不得声称**能验名字对不对）'
        }
        $e17Details += ('档案 META.comment（独立解析）："{0}"；弹窗 {1} 个' -f $facts.Comment, @($dialogs).Count)
    }
}
Add-Check -Id 'E-17' -Title 'E-17 扩展集留言：只读弹窗里出现选中档案 META.comment（独立解析核对）与归属信息' -Ok $e17Ok -Details $e17Details

# ==================================================================
# E-18 在线更新闭环（本地夹具）：索引 → 分类 → 下载 → 校验 → 入库；坏的一律拒；重复幂等
# ==================================================================
#
# 期望值全部独立算：仓库里的 index.txt 自己解析一遍，档案自己 sha256 + dump 一遍。
# 判据：
#   · 正例：库清单**多一条**；新增那一条的字段与远端索引/档案逐项一致；暂存目录不留东西
#   · 幂等：第二轮不得再产生条目（同一条不重复）
#   · 坏例：必须按**该用例专属**的理由拒；本地库与库清单**逐字节不变**；暂存目录不留东西

Trace-Step 'E-18：在线更新闭环与坏输入'
$e18Details = @()
$e18Ok = $true

# 远端仓库的独立解析（每个用例的仓库根由汇总里的 serverRoot 给出）
function Get-RepoIndexFacts([string]$CaseRoot)
{
    $indexPath = Join-Path $CaseRoot 'index.txt'
    if (-not (Test-Path -LiteralPath $indexPath)) { return $null }
    $text = Read-TextAutoShared -Path $indexPath -MaxBytes 2000000
    $parsed = ConvertFrom-ManifestText $text
    return [pscustomobject]@{ Path = $indexPath; Text = $text; Rows = $parsed.Rows; Problems = $parsed.Problems }
}

# 档案在仓库里的位置：<会社>/<作品>/rules.int
function Get-RepoArchivePath([string]$CaseRoot, $Row)
{
    return (Join-Path (Join-Path (Join-Path $CaseRoot $Row.Publisher) $Row.Title) 'rules.int')
}

$goodCase = if ($onlineCases.Contains('online-good')) { $onlineCases['online-good'] } else { $null }
if (-not $goodCase)
{
    $e18Ok = $false
    $e18Details += ('[FAIL] 汇总里没有 online-good 用例（{0}）' -f $OnlineSummaryJson)
}
else
{
    $run = Get-SetRun ([string](Get-ObjectValue -Object $goodCase -Name 'runJson' -Default '')) '在线更新正例'
    if (-not $run.Exists -or -not $run.Online)
    {
        $e18Ok = $false
        $e18Details += ('[FAIL] 正例的驱动结果不存在/没有 online 段：{0}' -f $run.Path)
    }
    elseif ($run.Failure)
    {
        $e18Ok = $false
        $e18Details += ('[FAIL] 正例的驱动自己失败了：{0}' -f $run.Failure)
    }
    else
    {
        $caseRoot = [string](Get-ObjectValue -Object $goodCase -Name 'serverRoot' -Default '')
        $index = Get-RepoIndexFacts $caseRoot
        $endpoint = [string](Get-ObjectValue -Object $goodCase -Name 'endpoint' -Default '')
        $shown = [string](Get-ObjectValue -Object $run.Online -Name 'endpointShown' -Default '')
        # §14.5：弹窗**必须显示当前基址**，否则"点了没反应却不知道在打哪个地址"
        if ($shown -ne $endpoint)
        {
            $e18Ok = $false
            $e18Details += ('[FAIL] 弹窗「仓库地址」显示「{0}」，实际用的是「{1}」（§14.5 要求显示当前基址）' -f $shown, $endpoint)
        }
        else { $e18Details += ('基址一致：{0}' -f $shown) }

        if (-not $index)
        {
            $e18Ok = $false
            $e18Details += ('[FAIL] 独立解析仓库索引失败：{0}' -f $caseRoot)
        }
        else
        {
            $rows = @($index.Rows)
            $beforeBlock = Get-ObjectValue -Object $run.Online -Name 'before' -Default $null
            $afterBlock = Get-ObjectValue -Object $run.Online -Name 'after' -Default $null
            $before = [int](Get-ObjectValue -Object $beforeBlock -Name 'manifestRows' -Default -1)
            $after = [int](Get-ObjectValue -Object $afterBlock -Name 'manifestRows' -Default -1)
            $expectedNew = 0
            foreach ($row in $rows)
            {
                $archivePath = Get-RepoArchivePath $caseRoot $row
                $facts = Get-ArchiveFacts -Path $archivePath -Tag ('e18-repo-' + $row.Publisher)
                if (-not $facts) { continue }
                if ($facts.Sha256 -ne $row.ArchiveDigest) { $e18Ok = $false; $e18Details += ('[FAIL] 夹具自身不一致：{0} 的索引摘要与档案实际 sha256 不同' -f $row.Title) }
                if ($facts.ExeDigest -ne $row.ExeDigest) { $e18Ok = $false; $e18Details += ('[FAIL] 夹具自身不一致：{0} 的索引目标 exe 摘要与档案不同' -f $row.Title) }
                if ("$($facts.EntryCount)" -ne $row.EntryCount) { $e18Ok = $false; $e18Details += ('[FAIL] 夹具自身不一致：{0} 的条数列与档案不同' -f $row.Title) }
            }

            $rounds = @(Get-ObjectValue -Object $run.Online -Name 'rounds' -Default @())
            if ($rounds.Count -lt 2)
            {
                $e18Ok = $false
                $e18Details += ('[FAIL] 正例至少要跑两轮（第一轮安装、第二轮幂等），实际 {0} 轮' -f $rounds.Count)
            }
            else
            {
                $first = $rounds[0]
                $statusRefresh = [string](Get-ObjectValue -Object $first -Name 'statusAfterRefresh' -Default '')
                $items = @(Get-ObjectValue -Object $first -Name 'listItemsAfterRefresh' -Default @())
                $problems = @(Test-TextContains -Text $statusRefresh -Needles @('索引拉取成功') -Where '第一轮拉索引后的状态')
                if ($problems.Count -gt 0) { $e18Ok = $false; $e18Details += $problems }
                if ($items.Count -ne $rows.Count)
                {
                    $e18Ok = $false
                    $e18Details += ('[FAIL] 远端列表 {0} 项 ≠ 索引里的 {1} 行' -f $items.Count, $rows.Count)
                }
                # 每一行都要能在列表里看到「会社 / 作品 / N 条」（三项都对得上）
                foreach ($row in $rows)
                {
                    $needle = ('{0} / {1} / {2} 条' -f $row.Publisher, $row.Title, $row.EntryCount)
                    if ((@($items) -join "`n") -notlike ('*' + $needle + '*'))
                    {
                        $e18Ok = $false
                        $e18Details += ('[FAIL] 远端列表里没有「{0}」' -f $needle)
                    }
                }
                $statusInstall = [string](Get-ObjectValue -Object $first -Name 'statusAfterInstall' -Default '')
                $problems = @(Test-TextContains -Text $statusInstall -Needles @('校验结论', '字节与索引一致') -Where '第一轮安装后的状态')
                if ($problems.Count -gt 0) { $e18Ok = $false; $e18Details += $problems }
                $rowsAfterFirst = [int](Get-ObjectValue -Object $first -Name 'manifestRowsAfter' -Default -1)
                if ($rowsAfterFirst -ne ($before + 1))
                {
                    $e18Ok = $false
                    $e18Details += ('[FAIL] 第一轮安装后库清单 {0} 行，期望 {1} 行（多一条）' -f $rowsAfterFirst, ($before + 1))
                }
                # 入库与库清单本身再独立验一遍：新增的那一条必须与远端索引/档案逐项一致
                $afterText = [string](Get-ObjectValue -Object $afterBlock -Name 'manifestText' -Default '')
                $afterRows = ConvertFrom-ManifestText $afterText
                foreach ($row in $rows)
                {
                    $installed = @($afterRows.Rows | Where-Object { $_.Publisher -eq $row.Publisher -and $_.Title -eq $row.Title })
                    if ($installed.Count -ne 1)
                    {
                        $e18Ok = $false
                        $e18Details += ('[FAIL] 安装后库清单里「{0} / {1}」有 {2} 行（应恰好 1 行）' -f $row.Publisher, $row.Title, $installed.Count)
                        continue
                    }
                    if ($installed[0].ArchiveDigest -ne $row.ArchiveDigest -or $installed[0].ExeDigest -ne $row.ExeDigest -or $installed[0].EntryCount -ne $row.EntryCount)
                    {
                        $e18Ok = $false
                        $e18Details += ('[FAIL] 入库那一条与远端索引不一致：清单 {0}/{1}/{2}，索引 {3}/{4}/{5}' -f `
                                (Get-ShortText $installed[0].ArchiveDigest), (Get-ShortText $installed[0].ExeDigest), $installed[0].EntryCount,
                                (Get-ShortText $row.ArchiveDigest), (Get-ShortText $row.ExeDigest), $row.EntryCount)
                    }
                }
                # 暂存目录：成功之后也不许留半截
                foreach ($round in $rounds)
                {
                    $staging = @(Get-ObjectValue -Object $round -Name 'stagingFiles' -Default @())
                    if ($staging.Count -gt 0)
                    {
                        $e18Ok = $false
                        $e18Details += ('[FAIL] 暂存目录里留下了 {0} 个文件：{1}' -f $staging.Count, (($staging | ForEach-Object { $_.rel }) -join ', '))
                    }
                }
                # 幂等：第二轮不得再产生条目，且必须说清"已经是同一份"
                $second = $rounds[1]
                $statusInstall2 = [string](Get-ObjectValue -Object $second -Name 'statusAfterInstall' -Default '')
                $rowsAfterSecond = [int](Get-ObjectValue -Object $second -Name 'manifestRowsAfter' -Default -1)
                if ($rowsAfterSecond -ne $rowsAfterFirst)
                {
                    $e18Ok = $false
                    $e18Details += ('[FAIL] 第二轮之后库清单变成 {0} 行（第一轮 {1} 行）—— 重复更新产生了重复条目' -f $rowsAfterSecond, $rowsAfterFirst)
                }
                if ($statusInstall2 -notlike '*同一份*')
                {
                    $e18Ok = $false
                    $e18Details += ('[FAIL] 第二轮的状态里没有「同一份」这类说明（实际：{0}）' -f (($statusInstall2 -replace "`r?`n", ' / ').Substring(0, [Math]::Min(200, $statusInstall2.Length))))
                }
                $e18Details += ('正例：库清单 {0} → {1} 行；索引 {2} 行；第二轮幂等（仍 {3} 行）' -f $before, $after, $rows.Count, $rowsAfterSecond)
            }
        }
    }
}

# ---------- 首次安装（S-2）：库目录还不存在时，在线更新应当把它建出来 ----------
#
# 为什么单列：这一条以前**测不到** —— 库不存在时 Get-LibraryInfo 的 Root 是空串，驱动在
# Get-TreeDigestExcluding 的空串绑定校验上直接崩（未捕获异常、结果文件都没写出来）。
# 而"首次安装"正是"在线更新第一次把库建出来"唯一的现场。
$firstCase = if ($onlineCases.Contains('online-first-install')) { $onlineCases['online-first-install'] } else { $null }
if (-not $firstCase)
{
    $e18Ok = $false
    $e18Details += ('[FAIL] 汇总里缺用例 online-first-install（"库不存在 → 在线更新把库建出来"这条路径没被覆盖）')
}
else
{
    $run = Get-SetRun ([string](Get-ObjectValue -Object $firstCase -Name 'runJson' -Default '')) '首次安装'
    if (-not $run.Exists -or -not $run.Online)
    {
        $e18Ok = $false
        $e18Details += ('[FAIL] 首次安装：驱动结果不存在/没有 online 段（{0}）—— 库不存在时驱动崩掉就是这种形态' -f $run.Path)
    }
    elseif ($run.Failure)
    {
        $e18Ok = $false
        $e18Details += ('[FAIL] 首次安装：驱动自己失败了：{0}' -f $run.Failure)
    }
    else
    {
        $beforeBlock = Get-ObjectValue -Object $run.Online -Name 'before' -Default $null
        $afterBlock = Get-ObjectValue -Object $run.Online -Name 'after' -Default $null
        $beforeRoot = [string](Get-ObjectValue -Object $beforeBlock -Name 'libraryRoot' -Default '')
        $beforeDigestBlock = Get-ObjectValue -Object $beforeBlock -Name 'libraryDigest' -Default $null
        $beforeExists = Get-ObjectValue -Object $beforeDigestBlock -Name 'Exists' -Default $null
        $beforeRows = [int](Get-ObjectValue -Object $beforeBlock -Name 'manifestRows' -Default -1)
        $afterRoot = [string](Get-ObjectValue -Object $afterBlock -Name 'libraryRoot' -Default '')
        $afterRows = [int](Get-ObjectValue -Object $afterBlock -Name 'manifestRows' -Default -1)
        $afterText = [string](Get-ObjectValue -Object $afterBlock -Name 'manifestText' -Default '')
        $rounds = @(Get-ObjectValue -Object $run.Online -Name 'rounds' -Default @())
        $statusInstall = if ($rounds.Count -gt 0) { [string](Get-ObjectValue -Object $rounds[0] -Name 'statusAfterInstall' -Default '') } else { '' }

        # ① 前置：跑之前库**真的**不存在（否则这条用例没测到"首次安装"）
        if ($beforeRoot -ne '' -or $beforeExists -eq $true -or $beforeRows -gt 0)
        {
            $e18Ok = $false
            $e18Details += ('[FAIL] 首次安装：用例无效 —— 跑之前库竟然存在（libraryRoot="{0}"、Exists={1}、清单 {2} 行）；' -f $beforeRoot, $beforeExists, $beforeRows) +
                '这一条要的是"从来没有装过任何东西"的现场。'
        }
        # ② 产品必须把库建出来并写对清单
        if (-not $afterRoot)
        {
            $e18Ok = $false
            $e18Details += '[FAIL] 首次安装：装完之后 libraryRoot 还是空 —— 库没被建出来'
        }
        if ($afterRows -ne 1)
        {
            $e18Ok = $false
            $e18Details += ('[FAIL] 首次安装：装完之后库清单 {0} 行（应恰好 1 行）' -f $afterRows)
        }
        $problems = @(Test-TextContains -Text $statusInstall -Needles @('已新增') -Where '首次安装的结论文字')
        if ($problems.Count -gt 0) { $e18Ok = $false; $e18Details += $problems }

        # ③ 那一行必须与远端索引逐项一致（独立解析两边）
        $caseRoot = [string](Get-ObjectValue -Object $firstCase -Name 'serverRoot' -Default '')
        $index = Get-RepoIndexFacts $caseRoot
        $afterRowsParsed = ConvertFrom-ManifestText $afterText
        if (-not $index)
        {
            $e18Ok = $false
            $e18Details += ('[FAIL] 首次安装：独立解析远端索引失败（{0}）' -f $caseRoot)
        }
        elseif (@($index.Rows).Count -eq 0)
        {
            $e18Ok = $false
            $e18Details += '[FAIL] 首次安装：远端索引一行都没有（夹具不对）'
        }
        else
        {
            # 装进去的是**列表第 0 项**；用（会社/作品）在索引里把它找出来再逐项比
            # （不假定索引只有一行：真实夹具的 good 仓库是 2 行）
            $installedProbe = @($afterRowsParsed.Rows)[0]
            $remoteRow = @($index.Rows | Where-Object { $_.Publisher -eq $installedProbe.Publisher -and $_.Title -eq $installedProbe.Title })
            if ($remoteRow.Count -ne 1)
            {
                $e18Ok = $false
                $e18Details += ('[FAIL] 首次安装：库清单里那一行（{0} / {1}）不在远端索引里' -f $installedProbe.Publisher, $installedProbe.Title)
                $remoteRow = $null
            }
            else { $remoteRow = $remoteRow[0] }
            $installed = @()
            if ($remoteRow) { $installed = @($afterRowsParsed.Rows | Where-Object { $_.Publisher -eq $remoteRow.Publisher -and $_.Title -eq $remoteRow.Title }) }
            if (-not $remoteRow) { }
            elseif ($installed.Count -ne 1)
            {
                $e18Ok = $false
                $e18Details += ('[FAIL] 首次安装：库清单里没有「{0} / {1}」这一行（实际 {2} 行）' -f $remoteRow.Publisher, $remoteRow.Title, $installed.Count)
            }
            elseif ($installed[0].ArchiveDigest -ne $remoteRow.ArchiveDigest -or $installed[0].ExeDigest -ne $remoteRow.ExeDigest -or $installed[0].EntryCount -ne $remoteRow.EntryCount)
            {
                $e18Ok = $false
                $e18Details += ('[FAIL] 首次安装：库清单那一行与远端索引不一致（清单 {0}/{1}/{2}，索引 {3}/{4}/{5}）' -f `
                        (Get-ShortText $installed[0].ArchiveDigest), (Get-ShortText $installed[0].ExeDigest), $installed[0].EntryCount,
                        (Get-ShortText $remoteRow.ArchiveDigest), (Get-ShortText $remoteRow.ExeDigest), $remoteRow.EntryCount)
            }
            else
            {
                $e18Details += ('  首次安装：库从"不存在"被建出来，清单 1 行且与远端索引逐项一致（{0} / {1}）' -f $remoteRow.Publisher, $remoteRow.Title)
            }
        }
    }
}

# ---------- 坏输入：必须按各自理由被拒，且本地一个字节都不动 ----------
# 每条坏用例"必须被哪一句理由拒"——措辞取自**实测**（Run\out\_probe-online-status*.txt 那类现场记录），
# 不照记忆写。注意前三条（截断 / 短读 / 乱字节）实际都落在**同一条防线**上：
# "下载到的字节 ≠ 索引里的档案摘要"。这不是缺陷 —— 信任链的第一条就是干这个的；
# 真正能考到更深两条（结构解码 / 自洽性）的是 truncate-declared（索引摘要按截断后的字节算）。
# 每条用例另外可以带的两类约束（都写在这张表旁边，别散在代码里）：
#   Forbid    —— 这些措辞**不许**出现（防"被别的防线顺带拒了"= 因为错误的原因通过）
#   NameMismatch —— 结构化判据（§14.4 第 9 条）：错误文字必须**同时点名两边**
#                   （索引那行的会社/作品 + 档案内嵌 META 的会社/作品），缺一边即失败（S-1 的教训）
$badCaseExtra = [ordered]@{
    # 盐不匹配那条：现在索引名与档案 META 一致，**不许**被"名字对不上"那条抢答
    'online-salt-mismatch'   = @{ Forbid = @('索引说', 'META 说', '会社 / 作品与档案') }
    'online-index-name-mismatch' = @{ NameMismatch = $true }
}
$badCaseExpect = [ordered]@{
    'online-bad-sha256'          = @('档案摘要不符')
    'online-truncate'            = @('档案摘要不符')
    'online-truncate-declared'   = @('结构校验不通过', '解码失败', '自洽性', '名字层展开失败')
    'online-short-body'          = @('档案摘要不符', '连接被中断')
    'online-garbage'             = @('档案摘要不符', '结构校验不通过')
    'online-salt-mismatch'       = @('目标 exe 摘要与索引不一致')
    'online-index-name-mismatch' = @('索引说', 'META 说', '会社', '作品')   # 兜底用；真正判据见 badCaseExtra.NameMismatch
    'online-index-cols'          = @('索引里有不合规的行')
    'online-index-empty-publisher' = @('索引里有不合规的行')
    'online-index-bad-digest'    = @('索引格式不对')
    'online-index-empty-digest'  = @('没有档案摘要')
    'online-missing-archive'     = @('404')
}
foreach ($name in $badCaseExpect.Keys)
{
    if (-not $onlineCases.Contains($name)) { $e18Ok = $false; $e18Details += ('[FAIL] 汇总里缺用例 {0}' -f $name); continue }
    $case = $onlineCases[$name]
    $run = Get-SetRun ([string](Get-ObjectValue -Object $case -Name 'runJson' -Default '')) $name
    if (-not $run.Exists -or -not $run.Online)
    {
        $e18Ok = $false
        $e18Details += ('[FAIL] {0}：驱动结果不存在/没有 online 段（{1}）' -f $name, $run.Path)
        continue
    }
    $evidence = Get-OnlineCaseText $run
    # **前置复核**（S-1 的教训：用例本身没测到东西 = 用例无效，必须判失败而不是当作"拒了"）：
    # 如果本地已经有索引里那一份，产品会在**下载之前**就短路成「本地已经是同一份，无需下载」，
    # 于是"截断 / 乱字节必须被拒"这些用例根本走不到校验那一步（实测踩过）。
    if ($evidence -like '*无需下载*')
    {
        $e18Ok = $false
        $e18Details += ('[FAIL] {0}：用例无效 —— 本地已经有这一份，产品在下载前就短路了（"无需下载"），' -f $name +
            '这一条根本没测到下载/校验路径。夹具顺序或库的初始状态要修（负例必须在库干净时跑）。')
        continue
    }
    $needles = @($badCaseExpect[$name])
    $extra = if ($badCaseExtra.Contains($name)) { $badCaseExtra[$name] } else { $null }
    $hit = $false
    foreach ($needle in $needles) { if ($evidence -like ('*' + $needle + '*')) { $hit = $true; break } }
    if (-not $hit)
    {
        $e18Ok = $false
        $e18Details += ('[FAIL] {0}：没有被按预期理由拒（要出现 [{1}]）' -f $name, ($needles -join '/'))
    }
    # ---- 结构化判据：名字对不上这条，错误文字必须**同时点名两边**（S-1 的教训：只判"被拒"区分不了原因）----
    if ($extra -and $extra.ContainsKey('NameMismatch'))
    {
        $caseRootForName = [string](Get-ObjectValue -Object $case -Name 'serverRoot' -Default '')
        $indexForName = Get-RepoIndexFacts $caseRootForName
        $bothSidesOk = $false
        if ($indexForName -and @($indexForName.Rows).Count -ge 1)
        {
            $idxRow = @($indexForName.Rows)[0]
            $archPath = Get-RepoArchivePath $caseRootForName $idxRow
            $archFacts = Get-ArchiveFacts -Path $archPath -Tag 'e18-name-mismatch'
            if (-not $archFacts)
            {
                $e18Ok = $false
                $e18Details += ('[FAIL] {0}：独立解析仓库里那份档案失败（{1}）' -f $name, $archPath)
            }
            else
            {
                $needIdxPub = ($evidence -like ('*' + $idxRow.Publisher + '*'))
                $needIdxTitle = ($evidence -like ('*' + $idxRow.Title + '*'))
                $needMetaPub = ($evidence -like ('*' + $archFacts.Publisher + '*'))
                $needMetaTitle = ($evidence -like ('*' + $archFacts.Title + '*'))
                $bothSidesOk = ($needIdxPub -and $needIdxTitle -and $needMetaPub -and $needMetaTitle)
                if (-not $bothSidesOk)
                {
                    $e18Ok = $false
                    $e18Details += ('[FAIL] {0}：被拒的理由**没有同时点名两边**（索引名 {1}/{2} 命中 {3}/{4}；档案 META 名 {5}/{6} 命中 {7}/{8}）' -f `
                            $name, $idxRow.Publisher, $idxRow.Title, $needIdxPub, $needIdxTitle,
                            $archFacts.Publisher, $archFacts.Title, $needMetaPub, $needMetaTitle) +
                        '—— 只判"被拒"区分不了"因为名字不一致被拒"和"因为别的原因被拒"。'
                }
                else
                {
                    $e18Details += ('  {0}：按"索引名 ≠ 档案 META 名"被拒，且两边都点了名（索引 {1}/{2} vs 档案 {3}/{4}）' -f `
                            $name, $idxRow.Publisher, $idxRow.Title, $archFacts.Publisher, $archFacts.Title)
                }
            }
        }
        else
        {
            $e18Ok = $false
            $e18Details += ('[FAIL] {0}：独立解析仓库索引失败（{1}）—— 判据没法核' -f $name, $caseRootForName)
        }
    }
    if ($extra -and $extra.ContainsKey('Forbid'))
    {
        foreach ($needle in @($extra.Forbid))
        {
            if ($evidence -like ('*' + $needle + '*'))
            {
                $e18Ok = $false
                $e18Details += ('[FAIL] {0}：被拒的理由里出现了**别的防线**的措辞「{1}」—— 这说明它不是被这条防线拦下的（因为错误的原因通过）' -f $name, $needle)
            }
        }
    }
    # 本地必须一个字节都不动
    $badBeforeBlock = Get-ObjectValue -Object $run.Online -Name 'before' -Default $null
    $badAfterBlock = Get-ObjectValue -Object $run.Online -Name 'after' -Default $null
    $beforeRows = [int](Get-ObjectValue -Object $badBeforeBlock -Name 'manifestRows' -Default -1)
    $afterRows = [int](Get-ObjectValue -Object $badAfterBlock -Name 'manifestRows' -Default -1)
    $beforeDigest = [string](Get-ObjectValue -Object (Get-ObjectValue -Object $badBeforeBlock -Name 'libraryDigest' -Default $null) -Name 'digest' -Default '')
    $afterDigest = [string](Get-ObjectValue -Object (Get-ObjectValue -Object $badAfterBlock -Name 'libraryDigest' -Default $null) -Name 'digest' -Default '')
    $beforeManifest = [string](Get-ObjectValue -Object $badBeforeBlock -Name 'manifestText' -Default '')
    $afterManifest = [string](Get-ObjectValue -Object $badAfterBlock -Name 'manifestText' -Default '')
    # N-S1：目录维度也要比（"失败后留下一个空目录"只比文件时看不见）
    $badBeforeLibrary = Get-ObjectValue -Object $badBeforeBlock -Name 'libraryDigest' -Default $null
    $badAfterLibrary = Get-ObjectValue -Object $badAfterBlock -Name 'libraryDigest' -Default $null
    $beforeDirs = [int](Get-ObjectValue -Object $badBeforeLibrary -Name 'dirCount' -Default -1)
    $afterDirs = [int](Get-ObjectValue -Object $badAfterLibrary -Name 'dirCount' -Default -1)
    if ($beforeDigest -ne $afterDigest -or $beforeManifest -ne $afterManifest -or $beforeRows -ne $afterRows -or $beforeDirs -ne $afterDirs)
    {
        $e18Ok = $false
        $e18Details += ('[FAIL] {0}：被拒的同时**本地被改了**（库摘要 {1} → {2}；清单 {3} → {4} 行；**目录数 {5} → {6}**）' -f `
                $name, (Get-ShortText $beforeDigest), (Get-ShortText $afterDigest), $beforeRows, $afterRows, $beforeDirs, $afterDirs)
    }
    foreach ($round in @(Get-ObjectValue -Object $run.Online -Name 'rounds' -Default @()))
    {
        $staging = @(Get-ObjectValue -Object $round -Name 'stagingFiles' -Default @())
        if ($staging.Count -gt 0)
        {
            $e18Ok = $false
            $e18Details += ('[FAIL] {0}：失败后暂存目录里留下 {1} 个文件（{2}）' -f $name, $staging.Count, (($staging | ForEach-Object { $_.rel }) -join ', '))
        }
    }
    if ($hit -and -not ($extra -and $extra.ContainsKey('NameMismatch')) -and $beforeDigest -eq $afterDigest)
    {
        $e18Details += ('  {0}：已按预期拒，本地未动' -f $name)
    }
}
Add-Check -Id 'E-18' -Title 'E-18 在线更新闭环（本地夹具）：新增入库且与远端索引/档案逐项一致、重复更新幂等；坏 sha256/截断/盐不一致/索引格式错一律拒且本地字节不变' -Ok $e18Ok -Details $e18Details

# ==================================================================
# E-19 端点不可达 / 404 / 超时：各自明确报错，本地一个文件都不动
# ==================================================================
Trace-Step 'E-19：端点不可达 / 404 / 超时'
$e19Details = @()
$e19Ok = $true
$e19Cases = [ordered]@{
    # 上限 20 秒：连不上是**立刻**能知道的（实测 2.9 秒）。给 20 秒是留余量，同时能抓住
    # "不可达却等了一整轮接收超时（30s+）"这种明显不对的行为。
    'online-unreachable' = @{ Needles = @('连不上服务器'); MaxSeconds = 20; Name = '端点不可达（端口上什么都没有）' }
    'online-no-index'    = @{ Needles = @('404'); MaxSeconds = 120; Name = '仓库根下没有 index.txt' }
    'online-slow'        = @{ Needles = @('超时'); MinSeconds = 25; MaxSeconds = 300; Name = 'index.txt 拖住不回应' }
    # N-P2 第二层：前缀**是** https:// 但缺主机名 → 与"不是 https"是两句不同的话。
    # 判据：必须点到"主机名 / 基址不完整"，并且**不许**出现"不是 https"那一层的措辞。
    'online-https-no-host' = @{
        Name = '基址前缀是 https:// 但缺主机名（N-P2 第二层）'
        Needles = @('缺少主机名', '没有主机名', '主机名为空', '基址不完整')
        # 只挡"另一层"那句最独特的措辞：② 的正文里可能也会提"只允许 https:// 开头的地址"，
        # 拿它当禁词会误伤（判别"两层不同"只需盯住 ③ 独有的"不是 https 的地址"）。
        Forbid = @('不是 https 的地址')
        MaxSeconds = 20
    }
    # §14.4 第 1 条：只走 https。**这一条不靠单一措辞判**（S-1，2026-10-16 独立验证打穿的假绿）：
    #   旧写法只列了三个 needle，其中前两个（'只允许 https' / '不是 https'）在"基址不可用"那条分支里
    #   **永远不会出现**（NormalizeBase 在 ParseHttpsUrl 之前就把非 https 折成空串），实际只有最泛的
    #   '基址不可用' 会命中 —— 于是把 Endpoint 换成 `ftp://…` 照样绿，判据区分不了"拒绝明文 http"
    #   和"拒绝一切非 https"，却在结论里声称验了前者。
    # 现在改成结构化判据（三条都要）：
    #   ① **引用了我们传进去的原址**（原样字符串）—— "它拒绝的是这个地址"的直接证据；
    #   ② 点到"只接受 https"（下面 SpecificShapes 里任一）—— 泛化兜底句不算；
    #   ③ 不许把原因说成别的（Forbid：DNS / 证书 / 连不上 / 超时 / 404 / 索引格式）—— 证明它在发请求之前就拦了。
    # 补：若文字只是泛化兜底形态（含"地址也不能为空"）且**没有**原址 → 直接判失败并点名（S-1 的现场）。
    # 注：SpecificShapes 是**形状**不是死串，task-1 改成三态措辞（非 https 那一态引用用户原文）后不用改；
    #     承载判别力的是 ①（地址原文），② 只负责"它把只接受 https 这件事说出来了"。
    'online-http-base'   = @{
        Name = '基址是明文 http（§14.4 第 1 条：只走 https）'
        RequireAddress = $true
        # 形状取自**实测**（task-1 的三态措辞落地后的现场）：非 https 那一态会回显用户原文并点明只接受 https。
        # 留几个同义变体是防措辞微调，但**承载判别力的是 RequireAddress**（回显原址）。
        SpecificShapes = @('只允许 https:// 开头的地址', '不是 https 的地址', '只在 https 上拉扩展集',
            '不是 https:// 开头的', '只允许 https 地址', '只接受 https', '必须以 https:// 开头')
Forbid = @('DNS 解析不了', 'TLS 握手', '证书', '连不上服务器', '等不到对方的回应', 'HTTP 404', '索引格式不对',
            '缺少主机名', '没有主机名', '基址不完整')
        GenericShape = '地址也不能为空'
        MaxSeconds = 20
    }
}
foreach ($name in $e19Cases.Keys)
{
    $expect = $e19Cases[$name]
    if (-not $onlineCases.Contains($name)) { $e19Ok = $false; $e19Details += ('[FAIL] 汇总里缺用例 {0}' -f $name); continue }
    $case = $onlineCases[$name]
    $run = Get-SetRun ([string](Get-ObjectValue -Object $case -Name 'runJson' -Default '')) $name
    if (-not $run.Exists -or -not $run.Online)
    {
        $e19Ok = $false
        $e19Details += ('[FAIL] {0}（{1}）：驱动结果不存在/没有 online 段' -f $expect.Name, $run.Path)
        continue
    }
    $evidence = Get-OnlineCaseText $run
    $endpointForCase = [string](Get-ObjectValue -Object $case -Name 'endpoint' -Default '')
    $hit = $false
    if ($expect.ContainsKey('SpecificShapes'))
    {
        # 结构化判据（S-1）：原址 + 形状，缺一不可
        $addressHit = ($endpointForCase -and $evidence -like ('*' + $endpointForCase + '*'))
        $shapeHits = @($expect.SpecificShapes | Where-Object { $evidence -like ('*' + $_ + '*') })
        $genericShape = $expect.ContainsKey('GenericShape') -and ($evidence -like ('*' + [string]$expect.GenericShape + '*'))
        $hit = ($addressHit -and $shapeHits.Count -gt 0)
        if (-not $addressHit)
        {
            $e19Ok = $false
            if ($genericShape)
            {
                $e19Details += ('[FAIL] {0}：只命中了**泛化兜底措辞**（含「{1}」），错误里**没有引用它拒绝的那个地址**（我们传的是 {2}）' -f `
                        $expect.Name, $expect.GenericShape, $endpointForCase + ' —— 这条判据因此区分不了"拒绝明文 http"与"拒绝一切非 https"（S-1 的假绿现场）。')
            }
            else
            {
                $e19Details += ('[FAIL] {0}：错误里没有引用它拒绝的那个地址（我们传的是 {1}）—— 无法证明它拒的就是这个基址' -f $expect.Name, $endpointForCase)
            }
        }
        if ($shapeHits.Count -eq 0)
        {
            $e19Ok = $false
            $e19Details += ('[FAIL] {0}：文字里没有点到"只接受 https"（认这几种形状：{1}）' -f $expect.Name, (@($expect.SpecificShapes) -join ' / '))
        }
        foreach ($needle in @($expect.Forbid))
        {
            if ($evidence -like ('*' + $needle + '*'))
            {
                $e19Ok = $false
                $e19Details += ('[FAIL] {0}：文字里出现了别的原因「{1}」—— 这条用例要的是"发请求之前就拦住"' -f $expect.Name, $needle)
            }
        }
        if ($hit) { $e19Details += ('  {0}：引用了原址、点到了 https（形状命中 [{1}]），没有把原因说成网络故障' -f $expect.Name, ($shapeHits -join ',')) }
    }
    else
    {
        foreach ($needle in @($expect.Needles)) { if ($evidence -like ('*' + $needle + '*')) { $hit = $true; break } }
        if (-not $hit)
        {
            $e19Ok = $false
            $e19Details += ('[FAIL] {0}：没有出现该有的措辞 [{1}]（§14.6 要求各种失败各有各的措辞，不许合并成「更新失败」一句）' -f $expect.Name, (@($expect.Needles) -join '/'))
        }
        # N-P2：非 https 与"缺主机名"是**两句不同的措辞** —— 判据必须能区分，不许一句通吃
        if ($expect.ContainsKey('Forbid'))
        {
            foreach ($needle in @($expect.Forbid))
            {
                if ($evidence -like ('*' + $needle + '*'))
                {
                    $e19Ok = $false
                    $e19Details += ('[FAIL] {0}：文字里出现了**另一层**基址措辞「{1}」—— N-P2 把"非 https"与"缺主机名"分成两句，判据不许让一句通吃' -f $expect.Name, $needle)
                }
            }
        }
        if ($evidence -match '更新失败' -and $evidence -notmatch (@($expect.Needles) -join '|'))
        {
            $e19Ok = $false
            $e19Details += ('[FAIL] {0}：只剩一句笼统的「更新失败」' -f $expect.Name)
        }
    }
    # 用时：不可达必须**快速**失败；超时必须真的等到了接收超时（否则可能是别的原因报的"超时"）
    $elapsed = 0
    foreach ($round in @(Get-ObjectValue -Object $run.Online -Name 'rounds' -Default @()))
    {
        $ms = [int](Get-ObjectValue -Object $round -Name 'refreshElapsedMs' -Default 0)
        if ($ms -gt $elapsed) { $elapsed = $ms }
    }
    if ($expect.ContainsKey('MaxSeconds') -and $elapsed -gt ([int]$expect.MaxSeconds * 1000))
    {
        $e19Ok = $false
        $e19Details += ('[FAIL] {0}：驱动等了 {1:N1} 秒（上限 {2} 秒）—— 要么产品没按自己的超时收口，要么失败路径挂住了' -f $expect.Name, ($elapsed / 1000.0), $expect.MaxSeconds)
    }
    if ($expect.ContainsKey('MinSeconds') -and $elapsed -lt ([int]$expect.MinSeconds * 1000))
    {
        $e19Ok = $false
        $e19Details += ('[FAIL] {0}：{1:N1} 秒就报错了，比产品的接收超时（30s）短得多 —— 这条"超时"多半是别的原因报出来的' -f $expect.Name, ($elapsed / 1000.0))
    }
    # 本地一个文件都不动
    $e19BeforeBlock = Get-ObjectValue -Object $run.Online -Name 'before' -Default $null
    $e19AfterBlock = Get-ObjectValue -Object $run.Online -Name 'after' -Default $null
    $e19BeforeLibrary = Get-ObjectValue -Object $e19BeforeBlock -Name 'libraryDigest' -Default $null
    $e19AfterLibrary = Get-ObjectValue -Object $e19AfterBlock -Name 'libraryDigest' -Default $null
    $beforeDigest = [string](Get-ObjectValue -Object $e19BeforeLibrary -Name 'digest' -Default '')
    $afterDigest = [string](Get-ObjectValue -Object $e19AfterLibrary -Name 'digest' -Default '')
    $beforeRows = [int](Get-ObjectValue -Object $e19BeforeBlock -Name 'manifestRows' -Default -1)
    $afterRows = [int](Get-ObjectValue -Object $e19AfterBlock -Name 'manifestRows' -Default -1)
    # N-S1：目录维度也要比
    $beforeDirs = [int](Get-ObjectValue -Object $e19BeforeLibrary -Name 'dirCount' -Default -1)
    $afterDirs = [int](Get-ObjectValue -Object $e19AfterLibrary -Name 'dirCount' -Default -1)
    if ($beforeDigest -ne $afterDigest -or $beforeRows -ne $afterRows -or $beforeDirs -ne $afterDirs)
    {
        $e19Ok = $false
        $e19Details += ('[FAIL] {0}：本地被改了（库摘要 {1} → {2}；清单 {3} → {4} 行；**目录数 {5} → {6}**）' -f `
                $expect.Name, (Get-ShortText $beforeDigest), (Get-ShortText $afterDigest), $beforeRows, $afterRows, $beforeDirs, $afterDirs)
    }
    foreach ($round in @(Get-ObjectValue -Object $run.Online -Name 'rounds' -Default @()))
    {
        $staging = @(Get-ObjectValue -Object $round -Name 'stagingFiles' -Default @())
        if ($staging.Count -gt 0)
        {
            $e19Ok = $false
            $e19Details += ('[FAIL] {0}：暂存目录留下 {1} 个文件' -f $expect.Name, $staging.Count)
        }
    }
    if ($hit -and -not $expect.ContainsKey('SpecificShapes'))
    {
        $e19Details += ('  {0}：措辞与用时都对（{1:N1} 秒），本地未动' -f $expect.Name, ($elapsed / 1000.0))
    }
    elseif ($hit)
    {
        $e19Details += ('  用时 {0:N1} 秒（上限 {1} 秒），本地未动' -f ($elapsed / 1000.0), $expect.MaxSeconds)
    }
}
Add-Check -Id 'E-19' -Title 'E-19 端点不可达 / 404 / 超时：各自明确措辞（不可达要快、超时要真的等到 30s），本地一个文件都不动' -Ok $e19Ok -Details $e19Details

# ==================================================================
# X-7（脚手架自身）：空路径 / 不存在的路径不得把驱动弄崩（S-2 的自证）
# ==================================================================
#
# S-2 的现场：库目录不存在（= 首次安装）时 Get-LibraryInfo 返回 Root=''，驱动拿着空串去调
# Get-TreeDigestExcluding，撞上 `[Parameter(Mandatory)][string]$Path` 的空串绑定校验 →
# ParameterBindingValidationException → **驱动在写任何结果文件之前就死了**（不是断言红）。
# 这一条当面证明：现在的实现放行空串；而且**旧签名确实会抛**（否则这条自证无效）。
Trace-Step 'X-7：脚手架自身 —— 空路径容忍度'
$x7Details = @()
$x7Ok = $true
try
{
    $emptyDigest = Get-TreeDigestExcluding -Path ''
    $missingDigest = Get-TreeDigestExcluding -Path (Join-Path $env:TEMP 'yua-harness-definitely-not-here')
    $x7Details += ('空串：Exists={0} Count={1}；不存在的路径：Exists={2} Count={3}（都不得抛异常）' -f `
            $emptyDigest.Exists, $emptyDigest.Count, $missingDigest.Exists, $missingDigest.Count)
    if ($emptyDigest.Exists -ne $false -or $missingDigest.Exists -ne $false)
    {
        $x7Ok = $false
        $x7Details += '[FAIL] 空串 / 不存在的路径应当返回 Exists=false'
    }
}
catch
{
    $x7Ok = $false
    $x7Details += ('[FAIL] 空串或不存在的路径抛异常了（S-2 会再次发生）：{0}' -f $_.Exception.Message)
}
# 旧签名的自证：Mandatory 的非空串参数遇到空串必抛 —— 这条不成立就说明"自证"本身没意义
function Test-OldStyleMandatoryPath { param([Parameter(Mandatory)][string]$Path) return $Path }
$oldThrew = $false
try { [void](Test-OldStyleMandatoryPath -Path '') }
catch { $oldThrew = $true }
if ($oldThrew) { $x7Details += '旧签名（[Parameter(Mandatory)][string]）对空串确实抛异常 —— 这就是 S-2 的现场' }
else
{
    $x7Ok = $false
    $x7Details += '[FAIL] 旧签名竟然不抛 —— 这条自证失去了意义（说明空串本来就是被放行的）'
}
Add-Check -Id 'X-7' -Title 'X-7（脚手架自身）空路径 / 不存在的路径不得把驱动弄崩（S-2 的回归）' -Ok $x7Ok -Details $x7Details

# ==================================================================
# X-8（脚手架自身）：树摘要必须看得见「新建/删除**空目录**」（N-S1 的自证）
# ==================================================================
#
# N-S1 的现场：`Get-TreeDigestExcluding` 原来只枚举文件，于是产品在失败路径上留下
# `ExtensionLibrary\CON\`、`AB\T\` 这种**空目录**时，"本地一个字节都没改"照旧成立 ——
# 一整族安全判据（E-16 的只读性、E-18/E-19 的前后全等、暂存目录"不留东西"）在空目录上是假绿。
# 这一条当面证明：两个"文件完全相同、只差一个空目录"的树，摘要必须**不同**；
# 并且用 `-FilesOnly` 退回旧口径时摘要**相同**（旧口径的盲区被复现，自证才有意义）。
Trace-Step 'X-8：脚手架自身 —— 树摘要的目录维度'
$x8Details = @()
$x8Ok = $true
$probeRoot = Join-Path $env:TEMP ('yua-x8-' + $PID)
try
{
    $a = Join-Path $probeRoot 'A'
    $b = Join-Path $probeRoot 'B'
    foreach ($directory in @($a, $b)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
    # **时间戳必须钉死**：两棵树的文件内容一样、mtime 也必须一样，否则"文件口径"天然就有差异，
    # 探测本身不可判读（第一次写这条时就是这么翻车的：Write-Utf8File 循环两次差了毫秒）。
    $stamp = [datetime]::SpecifyKind([datetime]'2026-01-01T00:00:00', [System.DateTimeKind]::Utc)
    foreach ($directory in @($a, $b))
    {
        $file = Join-Path $directory 'same.txt'
        Write-Utf8File -Path $file -Text 'same' -NoNewline
        [System.IO.File]::SetLastWriteTimeUtc($file, $stamp)
    }
    $empties = @(
        (Join-Path $b 'AB')
        (Join-Path (Join-Path $b 'AB') 'T')
        (Join-Path $b 'CON')
    )
    foreach ($directory in $empties) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }

    $digestA = Get-TreeDigestExcluding -Path $a
    $digestB = Get-TreeDigestExcluding -Path $b
    $legacyA = Get-TreeDigestExcluding -Path $a -FilesOnly
    $legacyB = Get-TreeDigestExcluding -Path $b -FilesOnly
    $x8Details += ('A：文件 {0} / 目录 {1}；B：文件 {2} / 目录 {3}（B 比 A 多 3 个**空目录**）' -f `
            $digestA.Count, $digestA.DirCount, $digestB.Count, $digestB.DirCount)
    if ($digestA.Digest -eq $digestB.Digest)
    {
        $x8Ok = $false
        $x8Details += '[FAIL] 只差空目录的两棵树算出了**同一个摘要** —— 空目录改动又看不见了（N-S1 复发）'
    }
    if ($digestA.FileDigest -ne $digestB.FileDigest)
    {
        $x8Ok = $false
        $x8Details += '[FAIL] 两份文件完全相同，FileDigest 却不同（文件口径算错了）'
    }
    if ($legacyA.Digest -ne $legacyB.Digest)
    {
        $x8Ok = $false
        $x8Details += '[FAIL] `-FilesOnly` 旧口径应当看不见空目录（两份摘要应相同）—— 自证失去意义'
    }
    else
    {
        $x8Details += '`-FilesOnly`（旧口径）对这两棵树给出同一个摘要 —— 空目录盲区被复现，自证有效'
    }
    if ($digestB.DirCount -ne 3)
    {
        $x8Ok = $false
        $x8Details += ('[FAIL] 新建的空目录没被数到（DirCount={0}，期望 3）' -f $digestB.DirCount)
    }
}
catch
{
    $x8Ok = $false
    $x8Details += ('[FAIL] 探测过程抛异常：{0}' -f $_.Exception.Message)
}
finally
{
    if (Test-Path -LiteralPath $probeRoot) { [void](Remove-TreeUnder -Path $probeRoot -MustBeUnder $env:TEMP) }
}
Add-Check -Id 'X-8' -Title 'X-8（脚手架自身）树摘要覆盖目录：只差一个空目录的两棵树必须算出不同摘要（N-S1 的回归）' -Ok $x8Ok -Details $x8Details

exit (Show-HarnessChecks -Title '扩展集一期断言 E-1..E-19' -ChecksJson $ChecksJson)
