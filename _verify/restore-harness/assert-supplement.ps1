# 断言 S-1..S-6：『未还原文件按文件头补后缀』（①）与『补充还原』（②）
#
# 用法（一般由 run-all.ps1 -Scope supplement 驱动；也能单独跑）：
#   pwsh -File assert-supplement.ps1 `
#       -KeepRunJson        Run\out\step3-keep.json `
#       -MineRunJson        Run\out\step3-keep-supplement.json `
#       -DefaultMineRunJson Run\out\step3-supplement.json `
#       -SaltlessRunJson    Run\out\step3-supplement-nosalt.json
#
# 口径说明（为什么这么判）：
#   * 六个"未还原样本"的哈希**不在映射表里**，任何名字都算不出来 —— 唯一能做的就是照文件头补后缀。
#     所以 S-1 只看"哈希名 + 后缀"是否落在输出里，对不上就说明①没实现。
#   * "补充还原目标"的哈希 = BLAKE2s(UTF16LE(名字 + xp3hnp))（夹具侧用 Python 算的），
#     名字只出现在假脚本里、映射表里没有 —— 名字能出现就只可能来自"挖掘 + 算哈希 + 命中"。
#   * 负例名 nosuch_999_9999 在脚本里、但磁盘上**没有**对应哈希的文件：
#     它绝不能出现在输出或 MinedNames.lst 里；这条是"伪造名字"的唯一入口，判得最狠。

[CmdletBinding()]
param(
    [string]$ManifestPath = '',
    [string]$KeepRunJson = '',
    [string]$MineRunJson = '',
    [string]$DefaultMineRunJson = '',
    [string]$SaltlessRunJson = '',
    [string]$KeyedSaltRunJson = '',
    [string]$CloseRunJson = '',
    [string]$ChecksJson = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')
. (Join-Path $root 'lib\HarnessNative.ps1')

$runRoot = Join-Path $root 'Run'
$outRoot = Join-Path $runRoot 'out'
if (-not $ManifestPath) { $ManifestPath = Join-Path $runRoot 'manifest.json' }
if (-not $KeepRunJson) { $KeepRunJson = Join-Path $outRoot 'step3-keep.json' }
if (-not $MineRunJson) { $MineRunJson = Join-Path $outRoot 'step3-keep-supplement.json' }
if (-not $DefaultMineRunJson) { $DefaultMineRunJson = Join-Path $outRoot 'step3-supplement.json' }
if (-not $SaltlessRunJson) { $SaltlessRunJson = Join-Path $outRoot 'step3-supplement-nosalt.json' }
if (-not $KeyedSaltRunJson) { $KeyedSaltRunJson = Join-Path $outRoot 'step3-supplement-keysalt.json' }
if (-not $CloseRunJson) { $CloseRunJson = Join-Path $outRoot 'step3-close-during-mining.json' }
if (-not $ChecksJson) { $ChecksJson = Join-Path $outRoot 'checks-supplement.json' }

Reset-HarnessChecks

$manifest = Read-JsonFile -Path $ManifestPath
if (-not $manifest) { throw ("缺少清单：{0}；请先运行 make-workspace.ps1 -Supplement" -f $ManifestPath) }
$supplement = Get-ObjectValue -Object $manifest -Name 'supplement' -Default $null
if (-not $supplement) { throw 'manifest.json 里没有 supplement 段：这份工作区不是补充还原夹具（用 -Supplement 重新生成）' }

$workspace = [string]$manifest.gameDir
$sourceDir = [string]$manifest.sourceDir
$outputDir = [string]$manifest.defaultOutputDir
$headerSamples = @(Get-ObjectValue -Object $supplement -Name 'headerSamples' -Default @())
$minedNames = @(Get-ObjectValue -Object $supplement -Name 'minedNames' -Default @())
$requiredMined = @($minedNames | Where-Object { $_.required })
$optionalMined = @($minedNames | Where-Object { -not $_.required })
$negativeName = [string](Get-ObjectValue -Object $supplement -Name 'negativeName' -Default '')
$negativeHash = [string](Get-ObjectValue -Object $supplement -Name 'negativeHash' -Default '')
$saltFiles = @(Get-ObjectValue -Object $supplement -Name 'saltFiles' -Default @())

# 源文件清单：大夹具（>5000 条）的 JSON 里没有 files 数组，只有 manifest.tsv ——
# 走 Get-ManifestFiles 会自动回退到 TSV（否则命中规模档下独立枚举会数出 0 个，冤枉实现）
$sourceFiles = @(Get-ManifestFiles -JsonPath $ManifestPath)
Write-Host ("[assert-supplement] 源文件清单：{0} 条" -f $sourceFiles.Count)

# 源文件的内容哈希（用来证明"输出里那个文件就是源里那个文件"，而不是凭空造的）
$sourceByRel = @{}
foreach ($item in $sourceFiles)
{
    $sourceByRel[[string]$item.sourceRel] = $item
}

function Get-RunRecord([string]$Path, [string]$Label)
{
    if (-not $Path -or -not (Test-Path -LiteralPath $Path))
    {
        return [pscustomobject]@{ Label = $Label; Path = $Path; Exists = $false; Record = $null; Output = @() }
    }
    $record = Read-JsonFile -Path $Path
    $outputList = New-Object System.Collections.ArrayList
    foreach ($item in @(Get-ObjectValue -Object $record -Name 'output' -Default @())) { [void]$outputList.Add($item) }
    $tsv = [string](Get-ObjectValue -Object $record -Name 'outputTsv' -Default '')
    if ($outputList.Count -eq 0 -and $tsv -and (Test-Path -LiteralPath $tsv))
    {
        # 必须用 ArrayList：在循环里 `$output += ...` 是 O(n²)，
        # 2 万条输出时会把断言拖到几十分钟（踩过）
        foreach ($row in @(Read-TsvFile -Path $tsv))
        {
            if ($row.Count -lt 3) { continue }
            [void]$outputList.Add([pscustomobject]@{ rel = $row[0]; size = [long]$row[1]; sha256 = $row[2] })
        }
    }
    return [pscustomobject]@{ Label = $Label; Path = $Path; Exists = $true; Record = $record; Output = $outputList.ToArray() }
}

$runKeep = Get-RunRecord -Path $KeepRunJson -Label '保留（不开补充还原）'
$runMine = Get-RunRecord -Path $MineRunJson -Label '保留 + 补充还原'
$runDefaultMine = Get-RunRecord -Path $DefaultMineRunJson -Label '默认 + 补充还原'
$runSaltless = Get-RunRecord -Path $SaltlessRunJson -Label '盐挪走 + 补充还原'
$runKeyed = Get-RunRecord -Path $KeyedSaltRunJson -Label '盐带密钥 + 补充还原'
$runClose = Get-RunRecord -Path $CloseRunJson -Label '挖掘进行中关窗'

Write-Host ''
Write-Host '=== 断言 S（① 文件头补后缀 / ② 补充还原） ==='
foreach ($run in @($runKeep, $runMine, $runDefaultMine, $runSaltless))
{
    if ($run.Exists)
    {
        $flag = Get-ObjectValue -Object $run.Record -Name 'supplementRestore' -Default $false
        Write-Host ("[assert-supplement] {0}：{1}（{2} 个输出；补充还原='{3}'）" -f $run.Label, (Split-Path $run.Path -Leaf), $run.Output.Count, $flag)
    }
    else
    {
        Write-Host ("[assert-supplement] {0}：{1} —— 用例结果不存在" -f $run.Label, $run.Path)
    }
}

function Get-OutputRelSet($Run)
{
    $set = @{}
    foreach ($item in $Run.Output) { $set[[string]$item.rel] = $item }
    return $set
}

function Find-OutputByName($Run, [string]$Leaf)
{
    foreach ($item in $Run.Output)
    {
        if ((Split-Path ([string]$item.rel) -Leaf) -eq $Leaf) { return $item }
    }
    return $null
}

# 输出的"叶子名 -> 输出项"索引。命中数上规模时（几万个名字）必须靠它，
# 否则每个名字都要把整个输出列表扫一遍，是 O(名字×输出)。
function Get-OutputLeafIndex($Run)
{
    $index = @{}
    foreach ($item in $Run.Output) { $index[(Split-Path ([string]$item.rel) -Leaf)] = $item }
    return $index
}

# 名字 -> 是否出现在输出里（按叶子名/整段路径都比一遍）
function Test-NamePresent($Run, [string]$Name)
{
    foreach ($item in $Run.Output)
    {
        $rel = [string]$item.rel
        if ((Split-Path $rel -Leaf) -eq $Name) { return $rel }
        if ($rel -like ('*' + $Name)) { return $rel }
    }
    return ''
}

# "挖掘命中"的独立口径：夹具里**所有**埋了哈希的名字（必中 + 加分），
# 只要它以真名出现在这一轮的输出里就算命中。
# 不能只数"必中"那几个 —— 产品的词法比参考实现宽，非 ASCII 的加分项也可能被挖到，
# 那样报告里的命中数会比"必中数"大，硬按必中数对账就会冤枉实现。
# ---------- 进度与预算（防止"静默空转"）----------
#
# 教训：规模档（2 万命中）下脚本曾在某个循环里 99% CPU 空转 3.6 分钟、零输出 ——
# 交给人看就是"卡住了"。现在：
#   * 每一步都打印"在做什么 + 已耗时"
#   * 每个大循环都有迭代上限与进度打印
#   * 单条断言超过预算就记失败并说明卡在哪
$script:traceStarted = Get-Date
$script:sectionBudgetMs = 120000      # 单条断言的时间上限：超过就判"未完成/卡住"
$script:traceLog = New-Object System.Collections.ArrayList
function Trace-Step([string]$Text)
{
    $ms = [int]((Get-Date) - $script:traceStarted).TotalMilliseconds
    Write-Host ("[assert {0,8:N0}ms] {1}" -f $ms, $Text)
    [void]$script:traceLog.Add([pscustomobject]@{ AtMs = $ms; Text = $Text })
}
# 段落耗时 = 本条 Trace-Step 与上一条之间的间隔；超预算的段落会被 X-6 点名
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
function Assert-LoopBudget([string]$Label, [int]$Count, [int]$Limit = 200000)
{
    if ($Count -gt $Limit)
    {
        throw ("{0}：需要迭代 {1} 次，超过上限 {2} —— 拒绝空转，按失败处理" -f $Label, $Count, $Limit)
    }
    Trace-Step ("{0}：{1} 项（上限 {2}）" -f $Label, $Count, $Limit)
}
function Get-MinedHits($Run)
{
    # 先把这一轮输出的"叶子名"建成索引：命中数上规模时（几万个名字）
    # 逐名字 × 逐输出地扫是 O(名字×输出)，会慢到不可用。
    $leafSet = @{}
    foreach ($out in $Run.Output) { $leafSet[(Split-Path ([string]$out.rel) -Leaf)] = $true }

    $hits = New-Object System.Collections.ArrayList
    foreach ($mined in $minedNames)
    {
        $leaf = Split-Path ([string]$mined.expectedRel) -Leaf
        if ($leafSet.ContainsKey($leaf)) { [void]$hits.Add([string]$mined.name) }
    }
    return $hits
}
# ---------- S-1：勾选保留（不开补充还原）时，未还原样本带上正确后缀 ----------
Trace-Step 'S-1：勾选保留（不开补充还原）时，未还原样本带上正确后缀'

$s1Details = @()
$s1Ok = $true
if (-not $runKeep.Exists)
{
    $s1Ok = $false
    $s1Details += ("用例结果不存在：{0}（先跑 run-step3-restore.ps1 -KeepUnrestored）" -f $KeepRunJson)
}
else
{
    $s1Details += ("保留模式输出 {0} 个文件" -f $runKeep.Output.Count)
    foreach ($sample in $headerSamples)
    {
        $hash = [string]$sample.hashName
        $accepted = @($sample.acceptedSuffixes)
        $bare = Find-OutputByName $runKeep $hash
        $hit = $null
        $hitSuffix = ''
        foreach ($suffix in $accepted)
        {
            $candidate = Find-OutputByName $runKeep ($hash + $suffix)
            if ($candidate) { $hit = $candidate; $hitSuffix = $suffix; break }
        }

        if ($hit)
        {
            $s1Details += ("    [OK] {0,-5} {1} → {2}" -f $sample.key, $sample.desc, $hit.rel)
            continue
        }

        $s1Ok = $false
        if ($bare)
        {
            $s1Details += ("    [FAIL] {0,-5} {1}：输出里是裸哈希名 {2}，没有后缀 —— 文件头识别未实现" -f $sample.key, $sample.desc, $bare.rel)
        }
        else
        {
            # 也许落了个别的后缀：把前缀相同的都列出来
            $others = @($runKeep.Output | Where-Object { ([string]$_.rel) -like ('*' + $hash + '*') })
            if ($others.Count -gt 0)
            {
                $s1Details += ("    [FAIL] {0,-5} {1}：后缀不是期望的 {2}，实际是 {3}" -f $sample.key, $sample.desc, ($accepted -join '/'), (($others | ForEach-Object { Split-Path $_.rel -Leaf }) -join ', '))
            }
            else
            {
                $s1Details += ("    [FAIL] {0,-5} {1}：输出里找不到这个哈希名（勾选保留后它本该被保留）" -f $sample.key, $sample.desc)
            }
        }
    }
    # 内容也得对得上：改名可以，改内容不行
    $contentBad = @()
    foreach ($sample in $headerSamples)
    {
        $expected = $sourceByRel[[string]$sample.sourceRel]
        if (-not $expected) { continue }
        foreach ($suffix in @($sample.acceptedSuffixes))
        {
            $candidate = Find-OutputByName $runKeep ([string]$sample.hashName + $suffix)
            if ($candidate -and ([string]$candidate.sha256) -ne ([string]$expected.sha256))
            {
                $contentBad += ('{0}（补后缀后内容变了）' -f $candidate.rel)
            }
        }
    }
    if ($contentBad.Count -gt 0)
    {
        $s1Ok = $false
        $s1Details += '内容不一致：'
        $s1Details += ($contentBad | ForEach-Object { '    ' + $_ })
    }
}
Add-Check -Id 'S-1' -Title '① 勾选保留时：6 个未还原样本按文件头补后缀（未知落 .bin）' -Ok $s1Ok -Details $s1Details

# ---------- S-2：勾选补充还原后，脚本里有名字的文件被还原成真名 ----------
Trace-Step 'S-2：勾选补充还原后，脚本里有名字的文件被还原成真名'

$s2Details = @()
$s2Ok = $true
$minedRestoredCount = 0
if (-not $runMine.Exists)
{
    $s2Ok = $false
    $s2Details += ("用例结果不存在：{0}" -f $MineRunJson)
}
else
{
    $mineLeafIndex = Get-OutputLeafIndex $runMine
    Assert-LoopBudget -Label 'S-2：命中名字核对' -Count $minedNames.Count -Limit 300000
    foreach ($mined in $minedNames)
    {
        $expectedRel = [string]$mined.expectedRel   # 真名（含后缀）落点
        $hash = [string]$mined.hash
        $source = $sourceByRel[[string]$mined.sourceRel]
        $hit = $null
        $expectedLeaf = Split-Path $expectedRel -Leaf
        if ($mineLeafIndex.ContainsKey($expectedLeaf)) { $hit = $mineLeafIndex[$expectedLeaf] }

        $tag = if ($mined.required) { '必中' } else { '加分' }
        if ($hit)
        {
            $contentOk = (-not $source) -or (([string]$hit.sha256) -eq ([string]$source.sha256))
            if ($contentOk)
            {
                $minedRestoredCount++
                # 命中上万时不要把两万行都塞进 details（JSON 涨到几 MB 且没人看）：前 10 条逐条列，其余只计数
                if ($minedRestoredCount -le 10) { $s2Details += ("    [OK]   [{0}] {1} → {2}" -f $tag, $mined.name, $hit.rel) }
                elseif ($minedRestoredCount -eq 11) { $s2Details += '    …（其余命中的名字逐条同形，省略）' }
            }
            else
            {
                $s2Ok = $false
                $s2Details += ("    [FAIL] [{0}] {1}：名字对了但内容不是源文件（{2} ≠ {3}）" -f $tag, $mined.name, $hit.sha256, $source.sha256)
            }
        }
        elseif ($mined.required)
        {
            $s2Ok = $false
            $keptAsHash = if ($mineLeafIndex.ContainsKey($hash)) { [string]$mineLeafIndex[$hash].rel } else { '' }
            $hashWithSuffix = $hash + @($mined.acceptedSuffixes)[0]
            $asHashWithSuffix = if ($mineLeafIndex.ContainsKey($hashWithSuffix)) { [string]$mineLeafIndex[$hashWithSuffix].rel } else { '' }
            if ($keptAsHash -or $asHashWithSuffix)
            {
                $s2Details += ("    [FAIL] [{0}] {1}：还是哈希名 {2} —— 名字没挖出来（或挖到了没用来改名）" -f $tag, $mined.name, $(if ($asHashWithSuffix) { $asHashWithSuffix } else { $keptAsHash }))
            }
            else
            {
                $s2Details += ("    [FAIL] [{0}] {1}：输出里既没有真名 {2}，也没有哈希名" -f $tag, $mined.name, $expectedRel)
            }
        }
        else
        {
            $s2Details += ("    [提示] [{0}] {1}：没被还原（非 ASCII 名字，参考实现的词法抓不到，不算缺陷）" -f $tag, $mined.name)
        }
    }
    $s2Details += ("补充还原目标（必中 {0} 个）实际还原 {1} 个；加分项 {2} 个" -f $requiredMined.Count, $minedRestoredCount, $optionalMined.Count)
    if (-not (Get-ObjectValue -Object $runMine.Record -Name 'supplementRestore' -Default $false))
    {
        $s2Ok = $false
        $s2Details += '这一轮**没有勾上**「推理补充还原」复选框（run JSON 里 supplementRestore=false），结果不可信'
    }
}
Add-Check -Id 'S-2' -Title '② 勾选补充还原后：脚本里有名字的文件被还原成真名（名字与后缀都对）' -Ok $s2Ok -Details $s2Details

# ---------- S-2b：默认语义下，补充还原出来的文件也算"已还原" ----------
Trace-Step 'S-2b：默认语义下，补充还原出来的文件也算"已还原"'

$s2bDetails = @()
$s2bOk = $true
if (-not $runDefaultMine.Exists)
{
    $s2bOk = $false
    $s2bDetails += ("用例结果不存在：{0}" -f $DefaultMineRunJson)
}
else
{
    $restoredInDefault = 0
    $defaultLeafIndex = Get-OutputLeafIndex $runDefaultMine
    foreach ($mined in $requiredMined)
    {
        $hit = if ($defaultLeafIndex.ContainsKey([string]$mined.name)) { [string]$defaultLeafIndex[[string]$mined.name].rel } else { '' }
        if ($hit)
        {
            $restoredInDefault++
            # 命中上万时只列前 10 条，其余只计数
            if ($restoredInDefault -le 10) { $s2bDetails += ("    [OK] {0} → {1}" -f $mined.name, $hit) }
        }
        else { $s2bOk = $false; $s2bDetails += ("    [FAIL] {0}：默认模式（只留已还原）里没有它 —— 补充还原出来的名字没有被当成『已还原』处理" -f $mined.name) }
    }
    # 期望条数 = 普通输出（清单里的 expectedDefaultNoMining）+ 这一轮**实际**命中的个数。
    # 写死 "普通 + 必中" 会冤枉实现：产品也可能挖到加分项（非 ASCII 名字）。
    $baseExpected = [int](Get-ObjectValue -Object $supplement -Name 'expectedDefaultNoMining' -Default 0)
    $hitsHere = @(Get-MinedHits $runDefaultMine).Count
    $expected = $baseExpected + $hitsHere
    $s2bDetails += ("默认 + 补充还原：输出 {0} 个（期望 {1} = 普通输出 {2} + 本轮命中 {3}）；必中项命中 {4}/{5}" -f `
            $runDefaultMine.Output.Count, $expected, $baseExpected, $hitsHere, $restoredInDefault, $requiredMined.Count)
    # 产品自己的产物（输出根目录的 RestoreReport*.txt）不算"还原出来的文件"
    $artifactCount = @($runDefaultMine.Output | Where-Object { (Split-Path ([string]$_.rel) -Leaf) -match '^RestoreReport' }).Count
    if ($artifactCount -gt 0) { $s2bDetails += ("（另有 {0} 个产品产物 RestoreReport，不计入）" -f $artifactCount) }
    $actualCount = $runDefaultMine.Output.Count - $artifactCount
    if ($expected -gt 0 -and $actualCount -ne $expected)
    {
        $s2bOk = $false
        $s2bDetails += ("输出条数与期望对不上：实际计入 {0}，期望 {1}" -f $actualCount, $expected)
    }
}
Add-Check -Id 'S-2b' -Title '② 默认语义下补充还原的文件属于"已还原"（输出条数也对得上）' -Ok $s2bOk -Details $s2bDetails

# ---------- S-3：不勾补充还原时，同一批文件不被还原（对照组） ----------
Trace-Step 'S-3：不勾补充还原时，同一批文件不被还原（对照组）'

$s3Details = @()
$s3Ok = $true
$s3KeptCount = 0
if (-not $runKeep.Exists)
{
    $s3Ok = $false
    $s3Details += ("用例结果不存在：{0}" -f $KeepRunJson)
}
else
{
    $keepLeafIndex = Get-OutputLeafIndex $runKeep
    foreach ($mined in $minedNames)
    {
        $present = if ($keepLeafIndex.ContainsKey([string]$mined.name)) { [string]$keepLeafIndex[[string]$mined.name].rel } else { '' }
        if ($present)
        {
            $s3Ok = $false
            $s3Details += ("    [FAIL] {0}：没勾补充还原，输出里却出现了真名（{1}）" -f $mined.name, $present)
        }
        else
        {
            $asHash = if ($keepLeafIndex.ContainsKey([string]$mined.hash)) { [string]$keepLeafIndex[[string]$mined.hash].rel } else { '' }
            $asHashWithSuffix = if ($keepLeafIndex.ContainsKey([string]$mined.hash + @($mined.acceptedSuffixes)[0])) { [string]$keepLeafIndex[[string]$mined.hash + @($mined.acceptedSuffixes)[0]].rel } else { '' }
            if ($asHash -or $asHashWithSuffix)
            {
        # 命中上万时只列前 10 条（否则 details 会涨到几 MB 且没人看）
        $s3KeptCount++
        if ($s3KeptCount -le 10) { $s3Details += ("    [OK]   {0}：保持哈希名 {1}（没被改名）" -f $mined.name, $(if ($asHashWithSuffix) { $asHashWithSuffix } else { $asHash })) }
            }
            elseif ($mined.required)
            {
                $s3Ok = $false
                $s3Details += ("    [FAIL] {0}：勾了保留，输出里却连哈希名都找不到（保留模式本身有问题）" -f $mined.name)
            }
            else
            {
                $s3Details += ("    [提示] {0}：源文件在输出里没找到哈希名（拿不到名字时按未还原处理，这里给的是提示）" -f $mined.name)
            }
        }
    }
    $s3Details += '对照口径：同一批文件在"保留但不开补充还原"这一轮里必须保持哈希名。'
}
Add-Check -Id 'S-3' -Title '③ 不勾补充还原：同一批文件不被还原（对照组）' -Ok $s3Ok -Details $s3Details

# ---------- S-4：负例 / 安全底线（绝不出现伪造的名字） ----------
Trace-Step 'S-4：负例 / 安全底线（绝不出现伪造的名字）'

$s4Details = @()
$s4Ok = $true
$reportHitCount = $null
$minedListPath = ''
$minedListLines = @()

function Find-MinedNamesFile
{
    $candidates = New-Object System.Collections.ArrayList
    foreach ($base in @($workspace, (Join-Path $runRoot 'App')))
    {
        if (-not (Test-Path -LiteralPath $base)) { continue }
        foreach ($file in (Get-ChildItem -LiteralPath $base -Recurse -File -Force -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -like '*MinedNames*' }))
        {
            [void]$candidates.Add($file)
        }
    }
    if ($candidates.Count -eq 0) { return $null }
    return ($candidates | Sort-Object LastWriteTime -Descending)[0]
}

if (-not $runMine.Exists)
{
    $s4Ok = $false
    $s4Details += ("用例结果不存在：{0}" -f $MineRunJson)
}
else
{
    # (a) 负例名绝不能出现在输出里
    $leaked = @($runMine.Output | Where-Object { ([string]$_.rel) -like ('*' + $negativeName + '*') })
    if ($leaked.Count -gt 0)
    {
        $s4Ok = $false
        $s4Details += ('[伪造] 输出里出现了负例名：')
        $s4Details += ($leaked | ForEach-Object { '    ' + $_.rel })
    }
    else
    {
        $s4Details += ("[OK] 输出 {0} 个文件里没有以「{1}」命名的（脚本里有它，但磁盘上不存在对应哈希）" -f $runMine.Output.Count, $negativeName)
    }

    # (b) 负例名也不能进 MinedNames.lst
    # 优先用 run JSON 里记的名字表快照：产品把名字表写在游戏目录/工具目录，
    # 后续用例会覆盖它，读现场会读串（踩过）。
    $snapshotPath = [string](Get-ObjectValue -Object $runMine.Record -Name 'minedNamesSnapshot' -Default '')
    $minedFile = if ($snapshotPath -and (Test-Path -LiteralPath $snapshotPath)) { Get-Item -LiteralPath $snapshotPath } else { Find-MinedNamesFile }
    if ($minedFile)
    {
        $minedListPath = $minedFile.FullName
        $minedListLines = @([System.IO.File]::ReadAllLines($minedFile.FullName, [System.Text.Encoding]::UTF8) | Where-Object { $_.Trim() })
        $badLines = @($minedListLines | Where-Object { $_ -like ('*' + $negativeName + '*') })
        if ($badLines.Count -gt 0)
        {
            $s4Ok = $false
            $s4Details += ('[伪造] {0} 里出现了负例名' -f $minedListPath)
            $s4Details += ($badLines | ForEach-Object { '    ' + $_ })
        }
        elseif ($negativeHash)
        {
            $hashLeak = @($minedListLines | Where-Object { $_ -like ($negativeHash + ':*') })
            if ($hashLeak.Count -gt 0) { $s4Ok = $false; $s4Details += ('[伪造] {0} 里有负例名的哈希（磁盘上并不存在这个文件）' -f $negativeHash) }
            else { $s4Details += ("[OK] {0} 里没有负例名，也没有它的哈希" -f $minedListPath) }
        }
    }
    else
    {
        $s4Details += '[！] 没找到 MinedNames.lst —— 无法检查"负例名有没有被写进名字表"（S-5 会失败）'
    }

    # (c) 报告里的命中数 vs 我独立数出来的"新增还原"
    $reportText = [string](Get-ObjectValue -Object $runMine.Record -Name 'reportText' -Default '')
    if ($reportText)
    {
        $map = Get-ReportNumberMap -Text $reportText
        $found = Find-ReportNumber -Map $map -Keywords @('命中', '补充还原', '挖掘') -ExcludeKeywords @('率')
        if ($found) { $reportHitCount = [int]$found.Value }
    }
    # 独立口径：夹具里所有埋了哈希的名字，实际以真名出现在这一轮输出里的个数
    $independent = @(Get-MinedHits $runMine).Count
    if ($null -eq $reportHitCount)
    {
        $s4Ok = $false
        $s4Details += '报告里找不到"命中数"口径（关键词：命中/补充还原/挖掘）—— 数字没法对账'
    }
    else
    {
        $s4Details += ("[对账] 报告命中 {0} / 脚手架独立枚举 {1}（夹具里埋的名字实际被还原的个数）" -f $reportHitCount, $independent)
        if ($reportHitCount -ne $independent)
        {
            $s4Ok = $false
            $s4Details += '报告数字与磁盘实际不一致（不信报告，以独立枚举为准）'
        }
    }

    # (d) 输出里"像资源名"的文件名必须全部来自我的夹具清单 —— 一个都不能是编出来的
    $grammar = '^[a-z][a-z0-9]{1,15}_[0-9]{3}_[0-9]{4}\.[A-Za-z0-9]+$'
    $allowed = @{}
    foreach ($mined in $minedNames) { $allowed[[string]$mined.name] = $true }
    $strangers = @()
    foreach ($item in $runMine.Output)
    {
        $leaf = Split-Path ([string]$item.rel) -Leaf
        if ($leaf -match $grammar -and -not $allowed.ContainsKey($leaf)) { $strangers += $leaf }
    }
    if ($strangers.Count -gt 0)
    {
        $s4Ok = $false
        $s4Details += '[伪造] 输出里有"像资源名"但我没埋过的文件：'
        $s4Details += ($strangers | Select-Object -Unique | ForEach-Object { '    ' + $_ })
    }
    else
    {
        $s4Details += '[OK] 输出里所有"资源名形态"的文件名都在夹具清单内（没有凭空造名字）'
    }

    # (e) 没有任何名字的样本不允许被"还原"成名字（默认语义那一路单独查）
    if ($runDefaultMine.Exists)
    {
        $fabricated = @()
        foreach ($sample in $headerSamples)
        {
            $hash = [string]$sample.hashName
            foreach ($item in $runDefaultMine.Output)
            {
                $leaf = Split-Path ([string]$item.rel) -Leaf
                if ($leaf -ne $hash -and $leaf -like ($hash + '*')) { $fabricated += $leaf }
            }
        }
        if ($fabricated.Count -gt 0)
        {
            $s4Ok = $false
            $s4Details += ('[伪造] 没有名字可挖的样本被改成了：' + (($fabricated | Select-Object -Unique) -join ', '))
        }
        else
        {
            $s4Details += '[OK] 没有名字可挖的 6 个样本在默认语义下没有被"还原"（不会凭空造名字）'
        }
    }
}
Add-Check -Id 'S-4' -Title '④ 安全底线：绝不出现伪造的名字（负例 + 报告对账 + 名字形态审计）' -Ok $s4Ok -Details $s4Details

# ---------- S-5：MinedNames.lst 落盘、格式与条数 ----------
Trace-Step 'S-5：MinedNames.lst 落盘、格式与条数'

$s5Details = @()
$s5Ok = $true
if (-not $minedListPath)
{
    # 兜底：run JSON 里记了快照路径就直接读它
    $snapshotPath = [string](Get-ObjectValue -Object $runMine.Record -Name 'minedNamesSnapshot' -Default '')
    if ($snapshotPath -and (Test-Path -LiteralPath $snapshotPath))
    {
        $minedListPath = $snapshotPath
        $minedListLines = @([System.IO.File]::ReadAllLines($snapshotPath, [System.Text.Encoding]::UTF8) | Where-Object { $_.Trim() })
    }
}
if (-not $minedListPath)
{
    $s5Ok = $false
    $s5Details += '没找到 MinedNames.lst（run JSON 没记快照，游戏目录与工具目录下也没搜到 *MinedNames*）'
}
else
{
    $s5Details += ("文件：{0}（{1} 行）" -f $minedListPath, $minedListLines.Count)
    # 注释行（# / ; 开头）不算数据行：产品在表头写一行说明是允许的
    $dataLines = @($minedListLines | Where-Object { -not ($_.TrimStart().StartsWith('#') -or $_.TrimStart().StartsWith(';')) })
    if ($dataLines.Count -ne $minedListLines.Count)
    {
        $s5Details += ("（跳过了 {0} 行注释）" -f ($minedListLines.Count - $dataLines.Count))
    }
    $minedListLines = $dataLines
    $badFormat = @($minedListLines | Where-Object { $_ -notmatch '^[0-9A-Fa-f]{64}:.+' })
    if ($badFormat.Count -gt 0)
    {
        $s5Ok = $false
        $s5Details += '格式不是「64 位十六进制:名字」的行：'
        $s5Details += ($badFormat | Select-Object -First 5 | ForEach-Object { '    ' + $_ })
    }
    else
    {
        $s5Details += '[OK] 每行都是「64 位十六进制:名字」'
        # 必须用 -cmatch：PowerShell 的 -match 默认不区分大小写，会把大写也判成小写（自检逮到过）
        $lower = @($minedListLines | Where-Object { $_ -cmatch '^[0-9a-f]{64}:' })
        if ($lower.Count -gt 0)
        {
            $s5Details += ("[！] 有 {0} 行哈希是小写；既有约定（HashRestore_RecoveredNames.lst）用大写，建议统一" -f $lower.Count)
        }
    }

    if ($null -ne $reportHitCount -and $minedListLines.Count -ne $reportHitCount)
    {
        $s5Ok = $false
        $s5Details += ("条数 {0} ≠ 报告命中数 {1}" -f $minedListLines.Count, $reportHitCount)
    }
    elseif ($null -ne $reportHitCount)
    {
        $s5Details += ("[OK] 条数 {0} == 报告命中数 {1}" -f $minedListLines.Count, $reportHitCount)
    }

    # 我自己埋的名字必须都在里面（挖到了才写得出来）
    # 先把名字表的哈希建成集合：命中上万个时，逐个名字去扫全表是 O(名字×行数)，
    # 2 万 × 2 万能把断言拖到几十分钟（踩过）。
    $listedHashes = @{}
    foreach ($line in $minedListLines)
    {
        $colon = $line.IndexOf(':')
        if ($colon -gt 0) { $listedHashes[$line.Substring(0, $colon).ToUpperInvariant()] = $true }
    }
    Assert-LoopBudget -Label 'S-5：必中名字核对' -Count $requiredMined.Count -Limit 300000
    $missing = @()
    foreach ($mined in $requiredMined)
    {
        $hash = ([string]$mined.hash).ToUpperInvariant()
        if (-not $listedHashes.ContainsKey($hash)) { $missing += ('{0}（{1}…）' -f $mined.name, $hash.Substring(0, 12)) }
    }
    if ($missing.Count -gt 0)
    {
        $s5Ok = $false
        $s5Details += ('名字表里缺了夹具里埋的名字：' + ($missing -join ', '))
    }
    else
    {
        $s5Details += ("[OK] 夹具里 {0} 个必中名字都在表里" -f $requiredMined.Count)
    }
}
Add-Check -Id 'S-5' -Title '⑤ MinedNames.lst：格式为「哈希:名字」，条数 == 报告命中数' -Ok $s5Ok -Details $s5Details

# ---------- S-6：盐不可用时的降级行为 ----------
Trace-Step 'S-6：盐不可用时的降级行为'

$s6Details = @()
$s6Ok = $true
if (-not $runSaltless.Exists)
{
    $s6Ok = $false
    $s6Details += ("用例结果不存在：{0}" -f $SaltlessRunJson)
}
else
{
    $failure = [string](Get-ObjectValue -Object $runSaltless.Record -Name 'failure' -Default '')
    $completed = [bool](Get-ObjectValue -Object $runSaltless.Record -Name 'completed' -Default $false)
    $removed = @(Get-ObjectValue -Object $runSaltless.Record -Name 'saltRemoved' -Default @())
    $s6Details += ("用例启动前挪走的盐：{0} 份" -f $removed.Count)
    if ($removed.Count -eq 0)
    {
        $s6Details += '[！] 这一轮好像没有真的把盐挪走，S-6 的场景没成立'
    }
    if (-not $completed -or $failure)
    {
        $s6Ok = $false
        $s6Details += ('流程没跑完：completed={0} failure={1}' -f $completed, $failure)
    }
    else
    {
        $s6Details += '[OK] 流程没崩：跑完了、没有失败原因'
    }

    # 普通还原必须照常完成
    $baseOk = 0
    $baseTotal = 0
    foreach ($item in $sourceFiles)
    {
        if ([string]$item.category -ne 'restored') { continue }
        $baseTotal++
        $hit = $null
        foreach ($out in $runSaltless.Output)
        {
            if ([string]$out.rel -eq [string]$item.expectedRel) { $hit = $out; break }
        }
        if ($hit -and ([string]$hit.sha256) -eq ([string]$item.sha256)) { $baseOk++ }
    }
    $s6Details += ("普通还原对照：{0}/{1} 个仍按期望路径与内容输出" -f $baseOk, $baseTotal)
    if ($baseTotal -gt 0 -and $baseOk -ne $baseTotal) { $s6Ok = $false; $s6Details += '盐不可用时普通还原也受了影响' }

    # 日志/报告里有没有说清楚原因
    $texts = @()
    $reportText = [string](Get-ObjectValue -Object $runSaltless.Record -Name 'reportText' -Default '')
    if ($reportText) { $texts += $reportText }
    $logText = [string](Get-ObjectValue -Object $runSaltless.Record -Name 'logText' -Default '')
    if ($logText) { $texts += $logText }
    foreach ($event in @(Get-ObjectValue -Object $runSaltless.Record -Name 'events' -Default @())) { $texts += [string]$event }
    $appExePath = [string](Get-ObjectValue -Object $runSaltless.Record -Name 'appExe' -Default '')
    $toolDir = if ($appExePath) { Split-Path -Parent $appExePath } else { '' }
    if ($toolDir -and (Test-Path -LiteralPath (Join-Path $toolDir 'Log')))
    {
        foreach ($file in (Get-ChildItem -LiteralPath (Join-Path $toolDir 'Log') -File -ErrorAction SilentlyContinue))
        {
            $content = Read-TextAutoShared -Path $file.FullName -MaxBytes 2000000
            if ($content) { $texts += $content }
        }
    }
    $joined = $texts -join "`n"
    $keywords = @('盐', 'Salt', 'salt', 'Universal', 'Hash Seed', '种子', '无法', '缺少', '跳过')
    $hitKeywords = @($keywords | Where-Object { $joined -like ('*' + $_ + '*') })
    if ($hitKeywords.Count -gt 0)
    {
        $s6Details += ('[OK] 日志/报告里提到了：' + ($hitKeywords -join ', '))
    }
    else
    {
        $s6Details += '[！] 日志与报告里都没找到"盐/salt/Universal/无法/缺少"之类的说明 —— 用户看不出为什么没做补充还原'
    }

    # 盐没了还照样命中的话，说明要么有内置默认盐、要么读的不是这份盐
    $minedWithoutSalt = @()
    $saltlessLeafIndex = Get-OutputLeafIndex $runSaltless
    foreach ($mined in $requiredMined)
    {
        if ($saltlessLeafIndex.ContainsKey([string]$mined.name)) { $minedWithoutSalt += [string]$mined.name }
    }
    if ($minedWithoutSalt.Count -gt 0)
    {
        $s6Details += ("[！] 盐被挪走后仍然命中了 {0} 个名字（{1}）—— 说明有内置默认盐、或读的不是这份 Universal.log。对别的游戏会算错哈希，请 lead 判一下是不是有意为之。" -f $minedWithoutSalt.Count, ($minedWithoutSalt -join ', '))
    }
    else
    {
        $s6Details += '[OK] 盐不可用时没有硬凑出名字（说明确实依赖读到的盐）'
    }

    # 夹具自洽：跑完要把盐放回去
    $missingSalt = @($saltFiles | Where-Object { -not (Test-Path -LiteralPath $_) })
    if ($missingSalt.Count -gt 0) { $s6Details += ('[！] 用例结束后盐没被放回：' + ($missingSalt -join ', ')) }
}
Add-Check -Id 'S-6' -Title '⑥ 盐不可用：不崩、有明确原因、普通还原照常完成' -Ok $s6Ok -Details $s6Details

# ---------- 独立枚举：给 R-1/R-2 用的"不信日志"的底数 ----------
Trace-Step '独立枚举：给 R-1/R-2 用的"不信日志"的底数'
#
# 口径（与产品日志里那两行的含义对齐）：
#   哈希名文件数 Y = 源目录里所有"哈希名"文件（分类不是明文名/工具日志的）
#   已还原数     X = 其中真的按真名落到输出、且内容一致的
# 全部由脚手架自己从清单 + 输出清单算出来，不读日志、不读报告。

$hashNamedSource = @($sourceFiles |
    Where-Object { $_.category -notin @('plain', 'skipped-log') })
$indepY = $hashNamedSource.Count
$restorableCategories = @('restored', 'mined')

# 先把两张表建好，循环里只做 O(1) 查找。
# 教训：规模档（2 万命中）下，这里原本对每个源文件去 $minedNames 里线性找一遍，
# 20004 × 20004 ≈ 4 亿次比较 —— 纯内存 CPU 空转、磁盘毫不增长，看起来就像"卡死"。
$minedExpectedBySourceRel = @{}
foreach ($mined in $minedNames) { $minedExpectedBySourceRel[[string]$mined.sourceRel] = [string]$mined.expectedRel }
$defaultOutputByLeaf = Get-OutputLeafIndex $runDefaultMine
Assert-LoopBudget -Label '独立枚举：源文件' -Count $hashNamedSource.Count -Limit 300000

$indepX = 0
foreach ($item in $hashNamedSource)
{
    if ([string]$item.category -notin $restorableCategories) { continue }
    # 注意：挖掘目标的 expectedRel 在清单里是空的（名字要靠挖掘才知道），
    # 期望落点记在 supplement.minedNames 里（上面已建好反查表）。
    $expectedRel = [string]$item.expectedRel
    if (-not $expectedRel -and [string]$item.category -eq 'mined') { $expectedRel = [string]$minedExpectedBySourceRel[[string]$item.sourceRel] }
    if (-not $expectedRel) { continue }
    $leaf = Split-Path $expectedRel -Leaf
    if ($defaultOutputByLeaf.ContainsKey($leaf) -and ([string]$defaultOutputByLeaf[$leaf].sha256) -eq ([string]$item.sha256)) { $indepX++ }
}
Trace-Step ("独立枚举结果：已还原 {0} / 哈希名文件 {1}（输出目录：{2}）" -f $indepX, $indepY, $outputDir)

$indepMined = @(Get-MinedHits $runDefaultMine).Count

$rateLogText = [string](Get-ObjectValue -Object $runDefaultMine.Record -Name 'uiLogFull' -Default '')
if (-not $rateLogText) { $rateLogText = [string](Get-ObjectValue -Object $runDefaultMine.Record -Name 'logText' -Default '') }

# ---------- R-1：详细日志里两行还原率，且百分比与 X/Y 自洽 ----------
Trace-Step 'R-1：详细日志里两行还原率，且百分比与 X/Y 自洽'

$r1Details = @()
$r1Ok = $true
$firstPass = $null
$finalRate = $null
if (-not $runDefaultMine.Exists -or -not $rateLogText)
{
    $r1Ok = $false
    $r1Details += ("拿不到详细日志：run={0} 存在={1} 日志长度={2}" -f $DefaultMineRunJson, $runDefaultMine.Exists, $rateLogText.Length)
}
else
{
    # 第一遍：`第一遍普通还原：已还原 X / Y（Z%）`
    # 数字要容忍千分位：产品把 20012 写成 "20,012"（与报告同口径）
    $m1 = [regex]::Match($rateLogText, '第一遍[^\r\n:：]*[:：]\s*已还原\s*([0-9][0-9,]*)\s*/\s*([0-9][0-9,]*)\s*[（(]\s*([0-9]+(?:\.[0-9]+)?)\s*%')
    # 最终：`还原率 Z%（已还原 X / 哈希名文件 Y[，推理补充还原新增 N]）`
    $m2 = [regex]::Match($rateLogText, '还原率\s*([0-9]+(?:\.[0-9]+)?)\s*%[（(]\s*已还原\s*([0-9][0-9,]*)\s*/\s*哈希名文件\s*([0-9][0-9,]*)\s*(?:[，,]\s*推理补充还原新增\s*([0-9][0-9,]*)\s*)?[)）]')

    if (-not $m1.Success)
    {
        $r1Ok = $false
        $r1Details += '日志里没有「第一遍普通还原：已还原 X / Y（Z%）」这一行'
        $candidates = @([regex]::Matches($rateLogText, '.{0,30}已还原.{0,40}') | ForEach-Object { $_.Value })
        if ($candidates.Count -gt 0) { $r1Details += '日志里与「已还原」有关的片段：'; $r1Details += ($candidates | Select-Object -First 5 | ForEach-Object { '    ' + $_ }) }
    }
    else
    {
        $firstPass = [pscustomobject]@{ X = [int]($m1.Groups[1].Value -replace ',', ''); Y = [int]($m1.Groups[2].Value -replace ',', ''); Pct = [double]$m1.Groups[3].Value }
        $expectPct = if ($firstPass.Y -gt 0) { [Math]::Round($firstPass.X * 100.0 / $firstPass.Y, 2) } else { 0 }
        $r1Details += ("第一遍：已还原 {0} / {1}（{2}%）；独立算 {3}%" -f $firstPass.X, $firstPass.Y, $firstPass.Pct, $expectPct)
        if ([Math]::Abs($firstPass.Pct - $expectPct) -gt 0.011)
        {
            $r1Ok = $false
            $r1Details += '第一遍的百分比与 X/Y 对不上'
        }
    }

    if (-not $m2.Success)
    {
        $r1Ok = $false
        $r1Details += '日志里没有「还原率 Z%（已还原 X / 哈希名文件 Y…）」这一行'
        $candidates = @([regex]::Matches($rateLogText, '.{0,20}还原率.{0,60}') | ForEach-Object { $_.Value })
        if ($candidates.Count -gt 0) { $r1Details += '日志里与「还原率」有关的片段：'; $r1Details += ($candidates | Select-Object -First 5 | ForEach-Object { '    ' + $_ }) }
    }
    else
    {
        $newCount = if ($m2.Groups[4].Success) { [int]($m2.Groups[4].Value -replace ',', '') } else { -1 }
        $finalRate = [pscustomobject]@{ Pct = [double]$m2.Groups[1].Value; X = [int]($m2.Groups[2].Value -replace ',', ''); Y = [int]($m2.Groups[3].Value -replace ',', ''); New = $newCount }
        $expectPct = if ($finalRate.Y -gt 0) { [Math]::Round($finalRate.X * 100.0 / $finalRate.Y, 2) } else { 0 }
        $r1Details += ("最终：还原率 {0}%（已还原 {1} / 哈希名文件 {2}{3}）；独立算 {4}%" -f `
                $finalRate.Pct, $finalRate.X, $finalRate.Y,
            $(if ($newCount -ge 0) { '，推理补充还原新增 ' + $newCount } else { '' }), $expectPct)
        if ([Math]::Abs($finalRate.Pct - $expectPct) -gt 0.011)
        {
            $r1Ok = $false
            $r1Details += '最终的百分比与 X/Y 对不上'
        }
        if ($newCount -ge 0 -and $newCount -ne $indepMined)
        {
            $r1Details += ("[！] 日志里「推理补充还原新增 {0}」与独立枚举的 {1} 不一致（R-2 会再对一次账）" -f $newCount, $indepMined)
        }
        if ($firstPass -and $firstPass.X -ne $indepX - $indepMined)
        {
            $r1Details += ("[提示] 第一遍已还原 {0}，独立枚举（普通还原部分）为 {1}" -f $firstPass.X, ($indepX - $indepMined))
        }
    }
}
Add-Check -Id 'R-1' -Title '还原率日志：第一遍与最终各一行，且百分比与 X/Y 自洽' -Ok $r1Ok -Details $r1Details

# ---------- R-2：还原率与磁盘对账（不采信日志） ----------
Trace-Step 'R-2：还原率与磁盘对账（不采信日志）'

$r2Details = @()
$r2Ok = $true
if ($null -eq $finalRate)
{
    $r2Ok = $false
    $r2Details += '日志里没有可对账的「还原率 Z%（已还原 X / 哈希名文件 Y）」行'
    $r2Details += ("脚手架独立枚举：已还原 {0} / 哈希名文件 {1}（其中推理补充还原新增 {2}）" -f $indepX, $indepY, $indepMined)
}
else
{
    $r2Details += ("日志：已还原 {0} / 哈希名文件 {1}" -f $finalRate.X, $finalRate.Y)
    $r2Details += ("脚手架独立枚举（清单 + 输出清单逐条比对）：已还原 {0} / 哈希名文件 {1}" -f $indepX, $indepY)
    $r2Details += ("枚举对象（自证）：源清单 {0}（{1} 条）；输出目录 {2}（{3} 个文件）" -f $ManifestPath, $sourceFiles.Count, $outputDir, $runDefaultMine.Output.Count)
    $r2Details += ("其中推理补充还原新增：日志 {0} / 独立枚举 {1}" -f $finalRate.New, $indepMined)
    if ($finalRate.X -ne $indepX -or $finalRate.Y -ne $indepY)
    {
        $r2Ok = $false
        $r2Details += '数字对不上（以独立枚举为准）'
    }
    if ($finalRate.New -ge 0 -and $finalRate.New -ne $indepMined)
    {
        $r2Ok = $false
        $r2Details += '「推理补充还原新增」与独立枚举不一致'
    }
    if ($finalRate.Y -gt 0)
    {
        $r2Details += ("独立算出的还原率：{0}%（日志写 {1}%）" -f [Math]::Round($indepX * 100.0 / $indepY, 2), $finalRate.Pct)
    }
}
Add-Check -Id 'R-2' -Title '还原率与磁盘对账：独立枚举输出目录，数字必须一致' -Ok $r2Ok -Details $r2Details

# ---------- N-1：文案（「补充还原」→「推理补充还原」） ----------
Trace-Step 'N-1：文案（「补充还原」→「推理补充还原」）'

$n1Details = @()
$n1Ok = $true
$expectedCheckboxText = '推理补充还原（从游戏资源中挖掘候选名，哈希验证后写入）'
$expectedSection = '—— 推理补充还原（候选挖掘）——'

function Test-IsolatedOldWording([string]$Text)
{
    # 找出所有「补充还原」，看它前面一个字是不是「理」；
    # 「推理补充还原」里也含「补充还原」，所以必须逐个看前缀。
    $bad = @()
    if (-not $Text) { return , $bad }
    $index = $Text.IndexOf('补充还原')
    while ($index -ge 0)
    {
        $prefixOk = ($index -ge 1 -and $Text.Substring($index - 1, 1) -eq '理')
        if (-not $prefixOk)
        {
            $start = [Math]::Max(0, $index - 12)
            $length = [Math]::Min($Text.Length - $start, 34)
            $bad += $Text.Substring($start, $length)
        }
        $index = $Text.IndexOf('补充还原', $index + 1)
    }
    # 注意：这里不能写 return ,$bad —— 调用方用 @() 包住时会套成"1 个元素的数组"，
    # 于是"没有孤立旧文案"也会被报成 1 处（自检里逮到过这个）。
    return $bad
}

# (a) 复选框文案
$checkboxText = ''
foreach ($control in @(Get-ObjectValue -Object $runMine.Record -Name 'controls' -Default @()))
{
    if ([string]$control.Class -ne 'Button') { continue }
    $text = [string]$control.Text
    if ($text -like '*从游戏资源中挖掘候选名*' -or $text -like '*推理补充还原*') { $checkboxText = $text; break }
}
if (-not $checkboxText)
{
    $n1Ok = $false
    $n1Details += '还原窗口里没找到「推理补充还原」复选框（控件清单里没有匹配的 Button）'
}
elseif ($checkboxText -ne $expectedCheckboxText)
{
    $n1Ok = $false
    $n1Details += ("复选框文案是「{0}」，期望「{1}」" -f $checkboxText, $expectedCheckboxText)
}
else
{
    $n1Details += ("[OK] 复选框文案：{0}" -f $checkboxText)
}

# (b) 窗口日志 + 报告 + 名字表头部都不能出现孤立的「补充还原」
$textsToScan = @(
    [pscustomobject]@{ Name = '详细日志'; Text = [string](Get-ObjectValue -Object $runMine.Record -Name 'uiLogFull' -Default '') }
    [pscustomobject]@{ Name = '还原报告'; Text = [string](Get-ObjectValue -Object $runMine.Record -Name 'reportText' -Default '') }
)
if ($minedListPath -and (Test-Path -LiteralPath $minedListPath))
{
    $head = Read-TextAutoShared -Path $minedListPath -MaxBytes 4000
    $textsToScan += [pscustomobject]@{ Name = 'MinedNames.lst 头部'; Text = [string]$head }
}
foreach ($entry in $textsToScan)
{
    if (-not $entry.Text) { $n1Details += ("[！] {0}：读不到内容，没法查文案" -f $entry.Name); continue }
    $bad = @(Test-IsolatedOldWording $entry.Text)
    if ($bad.Count -gt 0)
    {
        $n1Ok = $false
        $n1Details += ("[FAIL] {0} 里还有孤立的「补充还原」共 {1} 处：" -f $entry.Name, $bad.Count)
        $n1Details += ($bad | Select-Object -First 6 | ForEach-Object { '    …' + $_ + '…' })
    }
    else
    {
        $n1Details += ("[OK] {0}：没有孤立的「补充还原」" -f $entry.Name)
    }
}

# (c) 报告里的章节标题
$reportText = [string](Get-ObjectValue -Object $runMine.Record -Name 'reportText' -Default '')
if ($reportText)
{
    if ($reportText -like ('*' + $expectedSection + '*'))
    {
        $n1Details += ("[OK] 报告里有章节「{0}」" -f $expectedSection)
    }
    else
    {
        $n1Ok = $false
        $n1Details += ("报告里没有章节「{0}」" -f $expectedSection)
        $found = @([regex]::Matches($reportText, '[^\r\n]*候选挖掘[^\r\n]*') | ForEach-Object { $_.Value })
        if ($found.Count -gt 0) { $n1Details += ('报告里与"候选挖掘"有关的行：'); $n1Details += ($found | Select-Object -First 3 | ForEach-Object { '    ' + $_ }) }
    }
}
Add-Check -Id 'N-1' -Title '文案：用户可见处一律「推理补充还原」，不残留孤立「补充还原」' -Ok $n1Ok -Details $n1Details

# ---------- T-1：挖掘进行中，界面往返延迟 < 500ms（采样 ≥5 次） ----------
Trace-Step 'T-1：挖掘进行中，界面往返延迟 < 500ms（采样 ≥5 次）'

$t1Details = @()
$t1Ok = $true
if (-not $runDefaultMine.Exists)
{
    $t1Ok = $false
    $t1Details += ("用例结果不存在：{0}" -f $DefaultMineRunJson)
}
else
{
    $samples = @(Get-ObjectValue -Object $runDefaultMine.Record -Name 'probeSamples' -Default @())
    $latencies = @()
    foreach ($sample in $samples)
    {
        $parts = ([string]$sample) -split '\|'
        if ($parts.Count -ge 2) { $latencies += [int]$parts[1] }
    }
    $hungCount = @($latencies | Where-Object { $_ -lt 0 }).Count
    $ok = @($latencies | Where-Object { $_ -ge 0 })
    $maxMs = if ($ok.Count -gt 0) { ($ok | Measure-Object -Maximum).Maximum } else { -1 }
    $avgMs = if ($ok.Count -gt 0) { [int]($ok | Measure-Object -Average).Average } else { -1 }
    $t1Details += ("探测样本 {0} 次；最大 {1}ms；平均 {2}ms；超时 {3} 次" -f $latencies.Count, $maxMs, $avgMs, $hungCount)
    $t1Details += ("用例整轮 {0} 秒（挖掘包含在内）" -f [Math]::Round((Get-ObjectValue -Object $runDefaultMine.Record -Name 'restoreElapsedMs' -Default 0) / 1000.0, 1))

    if ($latencies.Count -lt 5)
    {
        $t1Ok = $false
        $t1Details += '采样次数 < 5，不足以说明界面一直可响应'
    }
    if ($hungCount -gt 0)
    {
        $t1Ok = $false
        $t1Details += ("有 {0} 次探测超时（界面在那些时刻没响应）" -f $hungCount)
    }
    if ($maxMs -ge 500)
    {
        $t1Ok = $false
        $t1Details += ("最大往返 {0}ms ≥ 500ms —— 界面被挖掘卡住了" -f $maxMs)
    }

    # 挖掘窗口内的切片（单独一条提示性结论，见 T-1b）
    $miningSeen = [int](Get-ObjectValue -Object $runDefaultMine.Record -Name 'miningFirstSeenMs' -Default -1)
    $afterCount = [int](Get-ObjectValue -Object $runDefaultMine.Record -Name 'probeAfterMiningCount' -Default 0)
    $afterMax = [int](Get-ObjectValue -Object $runDefaultMine.Record -Name 'probeAfterMiningMaxMs' -Default 0)
    $lastMs = 0
    foreach ($sample in $samples)
    {
        $parts = ([string]$sample) -split '\|'
        if ($parts.Count -ge 1) { $lastMs = [Math]::Max($lastMs, [int]$parts[0]) }
    }
    if ($miningSeen -ge 0)
    {
        $t1Details += ("挖掘首次出现在 {0}ms；此后采样 {1} 次（最大 {2}ms），到本轮结束还有 {3}ms" -f $miningSeen, $afterCount, $afterMax, ($lastMs - $miningSeen))
    }
    else
    {
        $t1Details += '[！] 整轮都没在详细日志里看到"挖掘"字样 —— 要么这一轮没勾上补充还原，要么挖掘根本没跑'
    }
}
Add-Check -Id 'T-1' -Title 'T-1 挖掘进行中：界面往返延迟 < 500ms（采样 ≥5 次，无超时）' -Ok $t1Ok -Details $t1Details

# ---------- T-1b：挖掘窗口内确实采到了样（挖掘太快采不到只算提示） ----------
Trace-Step 'T-1b：挖掘窗口内确实采到了样（挖掘太快采不到只算提示）'

$t1bDetails = @()
$t1bOk = $true
$t1bSeverity = 'warn'
if ($runDefaultMine.Exists)
{
    $miningSeen = [int](Get-ObjectValue -Object $runDefaultMine.Record -Name 'miningFirstSeenMs' -Default -1)
    $afterCount = [int](Get-ObjectValue -Object $runDefaultMine.Record -Name 'probeAfterMiningCount' -Default 0)
    $afterMax = [int](Get-ObjectValue -Object $runDefaultMine.Record -Name 'probeAfterMiningMaxMs' -Default 0)
    if ($miningSeen -lt 0)
    {
        $t1bDetails += '没检测到挖掘阶段（详细日志里没有"挖掘"字样）—— 无法证明"挖掘期间界面可响应"'
    }
    elseif ($afterCount -ge 5)
    {
        $t1bDetails += ("[OK] 挖掘开始后有 {0} 次采样，最大往返 {1}ms" -f $afterCount, $afterMax)
    }
    else
    {
        $t1bDetails += ("挖掘开始后只采到 {0} 次（< 5）—— 多半是挖掘太快结束（夹具的噪声候选数可调大：make-workspace.ps1 -Supplement -NoiseCandidates N）" -f $afterCount)
    }
}
else
{
    $t1bDetails += '用例结果不存在，无法判断'
}
Add-Check -Id 'T-1b' -Title 'T-1b 挖掘窗口内采样次数 ≥5（挖掘太快采不到只提示）' -Ok $t1bOk -Severity $t1bSeverity -Details $t1bDetails

# ---------- T-2：挖掘进行中关窗 → 进程按时退出、无残留 ----------
Trace-Step 'T-2：挖掘进行中关窗 → 进程按时退出、无残留'

$t2Details = @()
$t2Ok = $true
if (-not $runClose.Exists)
{
    $t2Ok = $false
    $t2Details += ("用例结果不存在：{0}" -f $CloseRunJson)
}
else
{
    $closeInfo = Get-ObjectValue -Object $runClose.Record -Name 'closeDuringMining' -Default $null
    if (-not $closeInfo)
    {
        $t2Ok = $false
        $t2Details += '用例没有留下 closeDuringMining 记录（没走 T-2 流程？）'
    }
    else
    {
        # T-2 的口径（产品行为是对的，别按"进程必须立刻退出"判）：
        #   关掉还原窗口 = 中止还原并**回到主窗口**，进程本来就该继续活着。
        #   要验的是：确认框有没有出来（安全性）、窗口多快消失（界面没被挖掘卡住）、
        #   工作线程有没有回收、关主窗口后进程能否干净退出（无残留）。
        $t2Details += ("挖掘是否已开始：{0}（{1}ms 时检测到）" -f $closeInfo.miningDetected, $closeInfo.detectedAtMs)
        $t2Details += ("还原窗口消失：{0}ms；线程 {1} → {2}；确认框={3}（{4} 次）；进程退出={5}，整轮 {6}ms；强杀={7}" -f `
                $closeInfo.windowGoneMs, $closeInfo.threadsBefore, $closeInfo.threadsAfter,
            $closeInfo.confirmDialogSeen, $closeInfo.confirmClicks, $closeInfo.exited, $closeInfo.exitMs, $closeInfo.killed)

        if (-not $closeInfo.miningDetected)
        {
            $t2Ok = $false
            $t2Details += '[！] 关窗时挖掘还没开始（检测窗口内没看到挖掘字样）—— 这一轮没测到"挖掘中关窗"'
        }
        if (-not $closeInfo.confirmDialogSeen)
        {
            $t2Ok = $false
            $t2Details += '关窗时没有弹出「中止还原」确认框：用户会在不知情的情况下丢掉正在进行的工作（安全性）'
        }
        if ([int]$closeInfo.windowGoneMs -lt 0)
        {
            $t2Ok = $false
            $t2Details += '还原窗口一直没消失'
        }
        elseif ([int]$closeInfo.windowGoneMs -gt 5000)
        {
            $t2Ok = $false
            $t2Details += ("还原窗口用了 {0}ms 才消失（> 5 秒），关窗被挖掘卡住了" -f $closeInfo.windowGoneMs)
        }
        if (-not $closeInfo.exited)
        {
            $t2Ok = $false
            $t2Details += ("关主窗口后 {0} 秒内进程没退出，只能强杀 —— 有线程/句柄没释放" -f $closeInfo.graceSec)
        }
        elseif ([int]$closeInfo.exitMs -gt 15000)
        {
            $t2Ok = $false
            $t2Details += ("进程退出用了 {0}ms（> 15 秒），太慢" -f $closeInfo.exitMs)
        }
        if ($closeInfo.killed)
        {
            $t2Ok = $false
            $t2Details += '进程是被强杀的（不是自己退出的）'
        }
        # 线程回收：关掉还原窗口后，挖掘线程 + 4 个工作线程都该退掉
        # （实现者实测 9 → 4；时钟/采样再往后还能降到更少）
        if ([int]$closeInfo.threadsAfter -ge 0 -and [int]$closeInfo.threadsBefore -gt 0)
        {
            if ([int]$closeInfo.threadsAfter -ge [int]$closeInfo.threadsBefore)
            {
                $t2Ok = $false
                $t2Details += ("工作线程没有回收：{0} → {1}（挖掘线程 + 工作线程应当退出）" -f $closeInfo.threadsBefore, $closeInfo.threadsAfter)
            }
            else
            {
                $t2Details += ("[OK] 关窗后线程回落：{0} → {1}" -f $closeInfo.threadsBefore, $closeInfo.threadsAfter)
            }
        }
    }
}
# 只统计"我自己那份快照跑起来的"进程：别人（实现者）可能也在开这个 exe 做手工验证，
# 不该算到我头上；但如果残留的确是我的快照实例，就必须报出来。
$aliveMine = @()
$aliveOthers = @()
foreach ($proc in @(Get-Process YuaExtract -ErrorAction SilentlyContinue))
{
    $procPath = ''
    try { $procPath = [string]$proc.Path } catch { $procPath = '' }
    if ($procPath -and $procPath -like (Join-Path $runRoot 'App\*')) { $aliveMine += $proc } else { $aliveOthers += $proc }
}
if ($aliveMine.Count -gt 0)
{
    $t2Ok = $false
    $t2Details += ('用例结束后仍有残留进程（脚手架快照实例）：' + (($aliveMine | ForEach-Object { 'pid=' + $_.Id }) -join ', '))
}
else
{
    $t2Details += '[OK] 断言时没有本脚手架快照残留的 YuaExtract 进程'
}
if ($aliveOthers.Count -gt 0)
{
    $t2Details += ('（另有 {0} 个不属于本脚手架的 YuaExtract 进程，未计入判定）' -f $aliveOthers.Count)
}
Add-Check -Id 'T-2' -Title 'T-2 挖掘进行中关窗：进程按时退出、无残留' -Ok $t2Ok -Details $t2Details

# ---------- S-6b：盐存在但带密钥 → 明确拒绝挖掘，普通还原照常 ----------
Trace-Step 'S-6b：盐存在但带密钥 → 明确拒绝挖掘，普通还原照常'

$s6bDetails = @()
$s6bOk = $true
if (-not $runKeyed.Exists)
{
    $s6bDetails += ("用例结果不存在：{0}（这一条按提示处理）" -f $KeyedSaltRunJson)
}
else
{
    $failure = [string](Get-ObjectValue -Object $runKeyed.Record -Name 'failure' -Default '')
    $completed = [bool](Get-ObjectValue -Object $runKeyed.Record -Name 'completed' -Default $false)
    if (-not $completed -or $failure)
    {
        $s6bOk = $false
        $s6bDetails += ('带密钥盐时流程没跑完：completed={0} failure={1}' -f $completed, $failure)
    }
    else
    {
        $s6bDetails += '[OK] 流程没崩'
    }

    $baseOk = 0
    $baseTotal = 0
    foreach ($item in $sourceFiles)
    {
        if ([string]$item.category -ne 'restored') { continue }
        $baseTotal++
        foreach ($out in $runKeyed.Output)
        {
            if ([string]$out.rel -eq [string]$item.expectedRel -and ([string]$out.sha256) -eq ([string]$item.sha256)) { $baseOk++; break }
        }
    }
    $s6bDetails += ("普通还原对照：{0}/{1}" -f $baseOk, $baseTotal)
    if ($baseTotal -gt 0 -and $baseOk -ne $baseTotal) { $s6bOk = $false; $s6bDetails += '带密钥盐时普通还原也受影响了' }

    $keyedHit = @()
    $keyedLeafIndex = Get-OutputLeafIndex $runKeyed
    foreach ($mined in $requiredMined)
    {
        if ($keyedLeafIndex.ContainsKey([string]$mined.name)) { $keyedHit += [string]$mined.name }
    }
    if ($keyedHit.Count -gt 0)
    {
        $s6bOk = $false
        $s6bDetails += ('带密钥哈希时居然还命中了名字（{0}）—— 这样的哈希算不对，等于伪造名字' -f ($keyedHit -join ', '))
    }
    else
    {
        $s6bDetails += '[OK] 带密钥哈希时没有硬凑出名字'
    }

    $logText = [string](Get-ObjectValue -Object $runKeyed.Record -Name 'uiLogFull' -Default '')
    if ($logText -match '密钥|HasherSalt|不支持')
    {
        $hit = @([regex]::Matches($logText, '[^\r\n]*(?:密钥|HasherSalt|不支持)[^\r\n]*') | ForEach-Object { $_.Value.Trim() })
        $s6bDetails += '[OK] 日志里说明了原因：'
        $s6bDetails += ($hit | Select-Object -First 3 | ForEach-Object { '    ' + $_ })
    }
    else
    {
        $s6bDetails += '[！] 日志里没找到"密钥/HasherSalt/不支持"之类的说明 —— 用户看不出为什么没做补充还原'
    }
}
Add-Check -Id 'S-6b' -Title 'S-6b 盐带密钥：明确拒绝挖掘并说明原因，普通还原照常' -Ok $s6bOk -Details $s6bDetails

# ---------- X 组：本轮改造的回归项（两遍式流程 / 未勾选不挖掘 / 无 __dupN / 源目录只读）----------

$x1Details = @()
$x1Ok = $true
if (-not $runDefaultMine.Exists)
{
    $x1Ok = $false
    $x1Details += ("用例结果不存在：{0}" -f $DefaultMineRunJson)
}
else
{
    $log = [string](Get-ObjectValue -Object $runDefaultMine.Record -Name 'uiLogFull' -Default '')
    # 两遍式：第一遍普通还原 → 挖掘 → 第二遍只补未交付
    $phases = [ordered]@{
        '第一遍普通还原完成' = @('普通还原完成', '第一遍普通还原')
        '挖掘阶段'           = @('挖掘候选名', '候选挖掘', '开始从游戏资源里挖掘')
        '第二遍只补未交付'   = @('第二遍', '只补第一遍未交付')
    }
    foreach ($phase in $phases.Keys)
    {
        $hit = $false
        foreach ($keyword in $phases[$phase]) { if ($log -like ('*' + $keyword + '*')) { $hit = $true; break } }
        if ($hit) { $x1Details += ('[OK] 日志里有「{0}」阶段' -f $phase) }
        else { $x1Ok = $false; $x1Details += ('[FAIL] 日志里找不到「{0}」阶段的痕迹（关键词：{1}）' -f $phase, ($phases[$phase] -join '/')) }
    }
    # 第二遍只补未交付：第二遍交付数应当 ≤ 挖掘命中数（不该把第一遍的东西再复制一遍）
    $secondPass = @([regex]::Matches($log, '第二遍[^\r\n]*') | ForEach-Object { $_.Value })
    if ($secondPass.Count -gt 0) { $x1Details += ('第二遍相关日志：' + ($secondPass | Select-Object -First 3 | ForEach-Object { $_.Trim() }) -join ' | ') }
    $hits = @(Get-MinedHits $runDefaultMine).Count
    $secondDelivered = 0
    foreach ($match in @([regex]::Matches($log, '封包\s*(\S+)\s*\|\s*已完成\s*\|\s*还原\s*(\d+)/(\d+)'))) { $secondDelivered += [int]$match.Groups[2].Value }
    $x1Details += ('挖掘命中 {0} 个；日志里各封包"已完成 还原 X/Y"合计 {1}（含第一遍，只作参考）' -f $hits, $secondDelivered)
}
Add-Check -Id 'X-1' -Title 'X-1 回归：两遍式流程（普通还原 → 挖掘 → 第二遍只补未交付）' -Ok $x1Ok -Details $x1Details

$x2Details = @()
$x2Ok = $true
if (-not $runKeep.Exists)
{
    $x2Ok = $false
    $x2Details += ("用例结果不存在：{0}" -f $KeepRunJson)
}
else
{
    $log = [string](Get-ObjectValue -Object $runKeep.Record -Name 'uiLogFull' -Default '')
    # 注意：产品在窗口初始化时会写一段**功能说明**（"勾「推理补充还原」会……挖候选名……"），
    # 没勾选时也照样出现 —— 不能拿"日志里有没有挖掘字样"当判据（我第一版就这么误判了）。
    # 只认"真的在挖"的痕迹：挖掘进度行 / 开始挖掘 / 最终统计 / 第二遍 / 后映射，
    # 再叠加 run JSON 里跑者按同口径记录的 miningStarted。
    $activityPatterns = @(
        '挖掘中：扫描'
        '开始从游戏资源里挖掘候选名'
        '试算\s*[0-9,]+\s*/\s*命中'
        '补充还原后映射'
        '第二遍开始'
    )
    $activity = @()
    foreach ($pattern in $activityPatterns)
    {
        if ($log -match $pattern) { $activity += $pattern }
    }
    $miningStarted = [bool](Get-ObjectValue -Object $runKeep.Record -Name 'miningStarted' -Default $false)
    if ($activity.Count -gt 0 -or $miningStarted)
    {
        $x2Ok = $false
        $x2Details += ('[FAIL] 没勾「推理补充还原」，却出现了真的挖掘痕迹（miningStarted={0}；命中模式：{1}）' -f $miningStarted, ($activity -join ' / '))
        $lines = @([regex]::Matches($log, '[^\r\n]*(?:挖掘中：扫描|开始从游戏资源里挖掘|试算|第二遍开始)[^\r\n]*') | ForEach-Object { $_.Value.Trim() })
        $x2Details += ($lines | Select-Object -First 5 | ForEach-Object { '    ' + $_ })
    }
    else
    {
        $x2Details += '[OK] 未勾选时详细日志里没有任何"真的在挖"的痕迹（只有初始化时那句功能说明，那是正常的）'
    }
    # 未勾选也不该产出名字表
    $snapshot = [string](Get-ObjectValue -Object $runKeep.Record -Name 'minedNamesSnapshot' -Default '')
    if ($snapshot) { $x2Ok = $false; $x2Details += ('[FAIL] 未勾选却写出了名字表：' + $snapshot) }
    else { $x2Details += '[OK] 未勾选时没有生成 MinedNames.lst' }
    $x2Details += ("这一轮输出 {0} 个文件（保留模式）" -f $runKeep.Output.Count)
}
Add-Check -Id 'X-2' -Title 'X-2 回归：未勾选「推理补充还原」时不跑挖掘、不写名字表' -Ok $x2Ok -Details $x2Details

$x3Details = @()
$x3Ok = $true
$dupTotal = 0
foreach ($run in @($runKeep, $runMine, $runDefaultMine, $runSaltless, $runKeyed))
{
    if (-not $run.Exists) { continue }
    $dups = @($run.Output | Where-Object { (Split-Path ([string]$_.rel) -Leaf) -match '__dup\d+' })
    $dupTotal += $dups.Count
    if ($dups.Count -gt 0)
    {
        $x3Ok = $false
        $x3Details += ("[FAIL] {0}：出现 {1} 个 __dupN 文件" -f $run.Label, $dups.Count)
        $x3Details += ($dups | Select-Object -First 5 | ForEach-Object { '    ' + $_.rel })
    }
}
if ($dupTotal -eq 0) { $x3Details += '[OK] 补充夹具里没有重名冲突，五轮用例的输出里也没有任何 __dupN 膨胀' }
Add-Check -Id 'X-3' -Title 'X-3 回归：输出目录无 __dupN 膨胀' -Ok $x3Ok -Details $x3Details

$x4Details = @()
$x4Ok = $true
foreach ($run in @($runKeep, $runMine, $runDefaultMine, $runSaltless, $runKeyed))
{
    if (-not $run.Exists) { continue }
    $before = Get-ObjectValue -Object $run.Record -Name 'sourceDigestBefore' -Default $null
    $after = Get-ObjectValue -Object $run.Record -Name 'sourceDigestAfter' -Default $null
    if (-not $before -or -not $after)
    {
        $x4Ok = $false
        $x4Details += ("[FAIL] {0}：没有留下源目录摘要，无法判定只读" -f $run.Label)
        continue
    }
    $same = ([string]$before.Digest -eq [string]$after.Digest) -and ([int]$before.Count -eq [int]$after.Count)
    if ($same) { $x4Details += ('[OK] {0}：{1} 个文件，前后摘要一致（{2}）' -f $run.Label, $before.Count, ([string]$before.Digest).Substring(0, 12)) }
    else { $x4Ok = $false; $x4Details += ('[FAIL] {0}：源目录被改动了（{1} → {2}）' -f $run.Label, ([string]$before.Digest).Substring(0, 12), ([string]$after.Digest).Substring(0, 12)) }
}
Add-Check -Id 'X-4' -Title 'X-4 回归：源解包目录全程只读（前后摘要一致）' -Ok $x4Ok -Details $x4Details

$x5Details = @()
if ($runDefaultMine.Exists)
{
    $log = [string](Get-ObjectValue -Object $runDefaultMine.Record -Name 'uiLogFull' -Default '')
    $candidates = 0
    $tried = 0
    $m = [regex]::Match($log, '候选\s*(\d+)\s*/\s*试算\s*(\d+)')
    if ($m.Success) { $candidates = [int]$m.Groups[1].Value; $tried = [int]$m.Groups[2].Value }
    $miningMs = [int](Get-ObjectValue -Object $runDefaultMine.Record -Name 'miningFirstSeenMs' -Default -1)
    $elapsed = [int](Get-ObjectValue -Object $runDefaultMine.Record -Name 'restoreElapsedMs' -Default 0)
    $x5Details += ("候选 {0} 个 / 试算 {1} 次；整轮 {2:N1} 秒（挖掘在 {3}ms 时首次出现）" -f $candidates, $tried, ($elapsed / 1000.0), $miningMs)
    $x5Details += ("探测样本 {0} 次，最大往返 {1}ms，超时 {2} 次" -f `
        @(Get-ObjectValue -Object $runDefaultMine.Record -Name 'probeSamples' -Default @()).Count,
        (Get-ObjectValue -Object $runDefaultMine.Record -Name 'probeLatencyMaxMs' -Default 0),
        (Get-ObjectValue -Object $runDefaultMine.Record -Name 'pollHungCount' -Default 0))
    if ($tried -gt 0 -and $elapsed -gt 0)
    {
        $x5Details += ("吞吐参考：{0:N0} 次试算/秒（试算量与整轮时长的粗估）" -f ($tried * 1000.0 / $elapsed))
    }
}
else { $x5Details += '用例结果不存在' }
Add-Check -Id 'X-5' -Title 'X-5（规模）挖掘规模与耗时/响应性的实测值' -Ok $true -Severity 'info' -Details $x5Details

# ---------- X-6：脚本自身不许"静默空转" ----------
Trace-Step 'X-6：脚本耗时预算'
$x6Details = @()
$x6Ok = $true
$totalMs = [int]((Get-Date) - $script:traceStarted).TotalMilliseconds
$slow = @(Get-SlowSections)
$x6Details += ("整脚本 {0:N1} 秒；单条断言预算 {1} 秒" -f ($totalMs / 1000.0), ($script:sectionBudgetMs / 1000.0))
$slowest = @($script:traceLog | Sort-Object AtMs | Select-Object -Last 1)
if ($slow.Count -gt 0)
{
    $x6Ok = $false
    $x6Details += ('[FAIL] 有 {0} 段超过预算（会表现为"卡住"）：' -f $slow.Count)
    $x6Details += ($slow | ForEach-Object { '    {0:N1}s  {1}' -f ($_.Ms / 1000.0), $_.Text })
}
else
{
    $x6Details += '[OK] 没有任何一段超过预算'
}
# 分段耗时（取最长的 5 段），让人一眼看出时间花在哪
$segments = New-Object System.Collections.ArrayList
for ($index = 1; $index -lt $script:traceLog.Count; $index++)
{
    [void]$segments.Add([pscustomobject]@{ Ms = ($script:traceLog[$index].AtMs - $script:traceLog[$index - 1].AtMs); Text = $script:traceLog[$index - 1].Text })
}
$x6Details += '最长的几段：'
$x6Details += ($segments | Sort-Object Ms -Descending | Select-Object -First 5 | ForEach-Object { '    {0,7:N0}ms  {1}' -f $_.Ms, $_.Text })
Add-Check -Id 'X-6' -Title 'X-6（性能）脚本自身：无静默空转，单条断言在预算内' -Ok $x6Ok -Details $x6Details

exit (Show-HarnessChecks -Title '断言 S（后缀识别 + 补充还原）+ R/N/T/X（本轮改造）' -ChecksJson $ChecksJson)
