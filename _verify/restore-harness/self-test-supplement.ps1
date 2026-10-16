# 自检：证明 assert-supplement.ps1 的每条断言都有鉴别力。
#
# 做法和 self-test-assertions.ps1 一致：**不跑产品**，直接喂合成的用例结果：
#   1) "全对"的一份结果 → 所有断言必须通过
#   2) 每个场景故意做错一处 → 对应断言必须炸，且**只有**它（和该场景显式列出的）炸
#
# 这样以后谁把断言改松了（比如永远返回 true），这里会立刻报出来。
#
#   pwsh -File self-test-supplement.ps1

[CmdletBinding()]
param(
    [string]$ManifestPath = '',
    [string]$ChecksJson = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')
. (Join-Path $root 'lib\HarnessNative.ps1')

$runRoot = Join-Path $root 'Run'
$outRoot = Join-Path $runRoot 'out'
$workRoot = Join-Path $runRoot 'selftest-supplement'
if (-not $ManifestPath) { $ManifestPath = Join-Path $runRoot 'manifest.json' }
if (-not $ChecksJson) { $ChecksJson = Join-Path $outRoot 'checks-selftest-supplement.json' }

Reset-HarnessChecks

$manifest = Read-JsonFile -Path $ManifestPath
if (-not $manifest) { throw ("缺少清单：{0}；请先运行 make-workspace.ps1 -Supplement" -f $ManifestPath) }
$supplement = Get-ObjectValue -Object $manifest -Name 'supplement' -Default $null
if (-not $supplement) { throw '清单里没有 supplement 段：请用 make-workspace.ps1 -Supplement 生成夹具' }

$sourceFiles = @(Get-ObjectValue -Object $manifest -Name 'files' -Default @())
$headerSamples = @(Get-ObjectValue -Object $supplement -Name 'headerSamples' -Default @())
$minedNames = @(Get-ObjectValue -Object $supplement -Name 'minedNames' -Default @())
$requiredMined = @($minedNames | Where-Object { $_.required })
$optionalMined = @($minedNames | Where-Object { -not $_.required })
$negativeName = [string](Get-ObjectValue -Object $supplement -Name 'negativeName' -Default '')
$negativeHash = [string](Get-ObjectValue -Object $supplement -Name 'negativeHash' -Default '')
$workspace = [string]$manifest.gameDir
$hashDir = [string]$manifest.hashDir
$hashNamedCount = @($sourceFiles | Where-Object { $_.category -notin @('plain', 'skipped-log') }).Count
$restoredSource = @($sourceFiles | Where-Object { $_.category -eq 'restored' })
$minedSource = @($sourceFiles | Where-Object { $_.category -eq 'mined' })
$plainSource = @($sourceFiles | Where-Object { $_.category -eq 'plain' })

Write-Host ''
Write-Host '=== 补充还原断言自检（鉴别力） ==='
Write-Host ("[自检] 夹具：哈希名源文件 {0} 个 / 普通还原 {1} / 挖掘目标 {2}（必中 {3}）/ 噪声负例 {4}" -f `
        $hashNamedCount, $restoredSource.Count, $minedSource.Count, $requiredMined.Count, $negativeName)

function New-OutItem([string]$Rel, $Source)
{
    return [ordered]@{ rel = $Rel; size = [int]$Source.size; sha256 = [string]$Source.sha256 }
}

function New-BaseOutputs
{
    param([switch]$Mining, [switch]$Keep, [switch]$NoSuffix)

    $items = New-Object System.Collections.ArrayList
    foreach ($item in $restoredSource) { [void]$items.Add((New-OutItem ([string]$item.expectedRel) $item)) }

    if ($Keep)
    {
        foreach ($sample in $headerSamples)
        {
            $rel = if ($NoSuffix) { [string]$sample.expectedKeepRel -replace '\.[a-z0-9]+$', '' } else { [string]$sample.expectedKeepRel }
            $source = @($sourceFiles | Where-Object { $_.sourceRel -eq [string]$sample.sourceRel })[0]
            [void]$items.Add((New-OutItem $rel $source))
        }
    }

    foreach ($mined in $minedNames)
    {
        $source = @($sourceFiles | Where-Object { $_.sourceRel -eq [string]$mined.sourceRel })[0]
        $isRequired = [bool]$mined.required
        if ($Mining -and $isRequired)
        {
            [void]$items.Add((New-OutItem ([string]$mined.expectedRel) $source))
        }
        elseif ($Keep)
        {
            [void]$items.Add((New-OutItem ([string]$mined.expectedKeepRel) $source))
        }
    }

    if ($Keep)
    {
        foreach ($item in $plainSource) { [void]$items.Add((New-OutItem ([string]$item.expectedRel) $item)) }
    }
    else
    {
        # 默认语义下明文名文件照样输出
        foreach ($item in $plainSource) { [void]$items.Add((New-OutItem ([string]$item.expectedRel) $item)) }
    }
    return $items.ToArray()
}

# 详细日志：含两行还原率 + 报告章节标题 + 复选框文案，
# 全部用「推理补充还原」，所以 N-1 的"没有孤立旧文案"也成立。
function New-UiLog
{
    param([int]$FirstX = 2, [int]$X = 5, [int]$Y = 12, [int]$New = 3, [switch]$NoRateLines, [switch]$BadPercent, [switch]$OldWording,
        [switch]$NoMining, [switch]$NoSecondPass)

    $word = if ($OldWording) { '补充还原' } else { '推理补充还原' }
    $firstPct = if ($BadPercent) { 99.99 } else { if ($Y -gt 0) { [Math]::Round($FirstX * 100.0 / $Y, 2) } else { 0 } }
    $finalPct = if ($BadPercent) { 12.34 } else { if ($Y -gt 0) { [Math]::Round($X * 100.0 / $Y, 2) } else { 0 } }

    $lines = New-Object System.Collections.ArrayList
    [void]$lines.Add('20:00:00  封包 bgimage | 排队中 | 等待开始（2 个文件）')
    [void]$lines.Add('20:00:01  开始还原……')
    [void]$lines.Add('20:00:01  已启动 4 个工作线程。')
    if (-not $NoMining)
    {
        [void]$lines.Add(('20:00:02  普通还原完成，开始{0}（挖掘候选名）……' -f $word))
        [void]$lines.Add(('20:00:02  {0}：开始从游戏资源里挖掘候选名……' -f $word))
        [void]$lines.Add(('20:00:03  {0}：扫描 {1} 个文件 / 候选 {2} / 试算 {3} / 命中 {4}（新增 {4}）' -f $word, 2, 20004, 440088, $New))
        [void]$lines.Add(('20:00:03  {0}后映射：目录 3 条 / 文件 {1} 条（本次新增 {2} 条）' -f $word, (2 + $New), $New))
        if (-not $NoSecondPass)
        {
            [void]$lines.Add('20:00:03  推理补充还原第二遍开始：4 个封包，只补第一遍未交付的文件。')
        }
    }
    if (-not $NoRateLines)
    {
        [void]$lines.Add(('20:00:01  第一遍普通还原：已还原 {0} / {1}（{2}%）' -f $FirstX, $Y, $firstPct))
        [void]$lines.Add(('20:00:04  还原率 {0}%（已还原 {1} / 哈希名文件 {2}，{3}新增 {4}）' -f $finalPct, $X, $Y, $word, $New))
    }
    [void]$lines.Add('20:00:04  所有任务已到终态，开始收尾。')
    return ($lines -join "`r`n")
}

function New-ReportText
{
    param([int]$X = 5, [int]$Y = 12, [int]$New = 3, [switch]$OldWording)
    $word = if ($OldWording) { '补充还原' } else { '推理补充还原' }
    return (@(
            'YuaExtract 资源名还原报告（脚手架自检用）'
            ''
            ('—— {0}（候选挖掘）——' -f $word)
            ('候选名命中    {0}' -f $New)
            ('还原率 {0}%（已还原 {1} / 哈希名文件 {2}）' -f [Math]::Round($X * 100.0 / $Y, 2), $X, $Y)
            ''
        ) -join "`r`n")
}

function New-MinedList
{
    param([switch]$WithNegative, [switch]$Empty, [switch]$WrongFormat, [switch]$MissingRequired)
    $path = Join-Path $hashDir 'MinedNames.lst'
    $lines = New-Object System.Collections.ArrayList
    [void]$lines.Add('# YuaExtract MinedNames（脚手架自检用）')
    if (-not $Empty)
    {
        foreach ($mined in $minedNames)
        {
            # 只有"命中"的名字才会写进表里（非 ASCII 加分项抓不到 → 不写），与报告命中数同口径
            if (-not $mined.required) { continue }
            if ($MissingRequired -and $mined.required) { continue }
            [void]$lines.Add(('{0}:{1}' -f ([string]$mined.hash).ToUpperInvariant(), [string]$mined.name))
        }
        if ($WithNegative) { [void]$lines.Add(('{0}:{1}' -f $negativeHash, $negativeName)) }
        if ($WrongFormat) { [void]$lines.Add(('{0} {1}' -f ([string]$minedNames[0].hash).ToLowerInvariant(), [string]$minedNames[0].name)) }
    }
    Write-Utf8File -Path $path -Text (($lines -join "`r`n") + "`r`n") -NoNewline
    return $path
}

function New-RunRecord
{
    param(
        [string]$Label,
        [switch]$Keep,
        [switch]$Mining,
        [switch]$NoSuffix,
        [switch]$FabricateNegative,
        [switch]$FabricateStranger,
        [switch]$NoRateLines,
        [switch]$BadPercent,
        [switch]$OldWording,
        [switch]$FrozenUi,
        [switch]$SlowUi,
        [switch]$SaltRemoved,
        [switch]$KeyedSalt,
        [switch]$NoMinedOutput,
        [switch]$NoSecondPass,
        [switch]$DupBloat,
        [switch]$TouchSource,
        [switch]$ForceMiningLog,
        [switch]$ForceMiningActivity,
        [switch]$ForceMinedOutputs,
        [switch]$CloseStuck,
        [int]$ReportHitOverride = -1,
        [switch]$ListMissing,
        [switch]$ListEmpty,
        [switch]$ListWrongFormat
    )

    # NoMinedOutput：勾了补充还原但一个名字都没命中（盐不可用 / S-6b 场景），
    # 此时命中数必须是 0，输出里也不该有真名。
    $effectiveMining = ($Mining -and -not $NoMinedOutput)
    $outputs = @(New-BaseOutputs -Mining:$effectiveMining -Keep:$Keep -NoSuffix:$NoSuffix)
    $list = New-Object System.Collections.ArrayList
    foreach ($item in $outputs) { [void]$list.Add([pscustomobject]$item) }

    if ($FabricateNegative)
    {
        $source = @($sourceFiles | Where-Object { $_.category -eq 'mined' })[0]
        [void]$list.Add([pscustomobject]@{ rel = ('scn\scripts\data\{0}.ogg' -f $negativeName); size = [int]$source.size; sha256 = [string]$source.sha256 })
    }
    if ($ForceMinedOutputs)
    {
        # 场景：没开补充还原，输出里却出现了真名（对照组违规）——但日志/标记都不带挖掘痕迹
        foreach ($mined in $requiredMined)
        {
            $source = @($sourceFiles | Where-Object { $_.sourceRel -eq [string]$mined.sourceRel })[0]
            [void]$list.Add([pscustomobject]@{ rel = [string]$mined.expectedRel; size = [int]$source.size; sha256 = [string]$source.sha256 })
        }
    }
    if ($DupBloat)
    {
        $source = @($sourceFiles | Where-Object { $_.category -eq 'restored' })[0]
        [void]$list.Add([pscustomobject]@{ rel = ('bgimage\characters\base__dup1.png'); size = [int]$source.size; sha256 = [string]$source.sha256 })
    }
    if ($FabricateStranger)
    {
        $source = @($sourceFiles | Where-Object { $_.category -eq 'mined' })[0]
        [void]$list.Add([pscustomobject]@{ rel = 'scn\scripts\data\zzz_999_9999.ogg'; size = [int]$source.size; sha256 = [string]$source.sha256 })
    }

    # 探测样本：正常情况 40 次、每次 3ms；FrozenUi = 有一次 3000ms；
    # SlowUi = 每次 700ms（改造前"挖掘卡界面"的样子）
    $samples = New-Object System.Collections.ArrayList
    for ($index = 1; $index -le 40; $index++)
    {
        $ms = if ($FrozenUi -and $index -eq 25) { -1 } elseif ($SlowUi) { 700 } else { 3 }
        [void]$samples.Add(('{0}|{1}' -f ($index * 120), $ms))
    }

    # NoMinedOutput：勾了补充还原但一个名字都没命中（盐不可用/S-6b 场景），
    # 此时命中数必须是 0，输出里也不该有真名。
    $effectiveMining = ($Mining -and -not $NoMinedOutput)
    $hitCount = if ($ReportHitOverride -ge 0) { $ReportHitOverride } else { if ($effectiveMining) { $requiredMined.Count } else { 0 } }
    $record = [ordered]@{
        case              = 'selftest-supplement'
        mode              = $Label
        keepUnrestored    = [bool]$Keep
        supplementRestore = [bool]$Mining
        completed         = $true
        failure           = ''
        uiMode            = 'restore-window'
        workspace         = $workspace
        outputDir         = [string]$manifest.defaultOutputDir
        appExe            = '<selftest>'
        output            = @($list)
        outputTsv         = ''
        uiLogFull         = (New-UiLog -X (2 + $hitCount) -Y $hashNamedCount -New $hitCount -NoRateLines:$NoRateLines -BadPercent:$BadPercent -OldWording:$OldWording -NoMining:((-not $effectiveMining) -and (-not $ForceMiningLog)) -NoSecondPass:$NoSecondPass)
        logText           = (New-UiLog -X (2 + $hitCount) -Y $hashNamedCount -New $hitCount -NoRateLines:$NoRateLines -BadPercent:$BadPercent -OldWording:$OldWording -NoMining:((-not $effectiveMining) -and (-not $ForceMiningLog)) -NoSecondPass:$NoSecondPass)
        uiLogLines        = 10
        reportText        = (New-ReportText -X (2 + $hitCount) -Y $hashNamedCount -New $hitCount -OldWording:$OldWording)
        controls          = @(
            [ordered]@{ Class = 'Button'; Text = ('{0}（从游戏资源中挖掘候选名，哈希验证后写入）' -f $(if ($OldWording) { '补充还原' } else { '推理补充还原' })); CtrlId = 5010 }
            [ordered]@{ Class = 'Button'; Text = '保留未还原文件名的文件（文件后缀已确定）'; CtrlId = 5009 }
            [ordered]@{ Class = 'Button'; Text = '开始'; CtrlId = 5001 }
        )
        probeSamples      = @($samples)
        miningFirstSeenMs = $(if ($Mining) { 600 } else { -1 })
        probeAfterMiningCount = $(if ($Mining) { 34 } else { 0 })
        probeAfterMiningMaxMs = $(if ($Mining) { if ($FrozenUi) { -1 } elseif ($SlowUi) { 700 } else { 3 } } else { 0 })
        probeAfterMiningHung = $(if ($FrozenUi) { 1 } else { 0 })
        restoreElapsedMs  = 4800
        checkboxStates    = [ordered]@{ keepUnrestored = $(if ($Keep) { 1 } else { 'missing' }); supplementRestore = $(if ($Mining) { 1 } else { 'missing' }) }
        saltMode          = $(if ($KeyedSalt) { 'keyed' } elseif ($SaltRemoved) { 'remove' } else { 'ensure' })
        saltFiles         = @()
        saltRemoved       = $(if ($SaltRemoved) { @([ordered]@{ original = ($workspace + '\Log\Universal.log'); hidden = ($workspace + '\Log\Universal.log.harness-hidden') }) } else { @() })
        miningStarted     = [bool]$effectiveMining
        miningFinished    = [bool]$effectiveMining
        closeDuringMining = $null
        sourceDigestBefore = [pscustomobject]@{ Count = $sourceFiles.Count; Digest = 'SOURCEDIGEST-BEFORE'; HashDigest = 'H1' }
        sourceDigestAfter  = [pscustomobject]@{ Count = $sourceFiles.Count; Digest = $(if ($TouchSource) { 'SOURCEDIGEST-AFTER-CHANGED' } else { 'SOURCEDIGEST-BEFORE' }); HashDigest = 'H1' }
    }

    if ($ForceMiningActivity)
    {
        # 场景：没勾选却真的挖了（记录 + 日志痕迹都要有）
        $record.miningStarted = $true
        $record.miningFinished = $true
        $record.uiLogFull = $record.uiLogFull + "`r`n21:00:05  挖掘中：扫描 3 个文件 / 候选 12 / 命中 0"
        $record.uiLogFull = $record.uiLogFull + "`r`n21:00:05  推理补充还原：扫描 3 个文件 / 候选 12 / 试算 264 / 命中 0（新增 0）"
    }
    if ($SaltRemoved)
    {
        $record.uiLogFull = $record.uiLogFull + "`r`n20:00:02  找不到 Hash Seed（盐）：跳过推理补充还原（Universal.log 里没有可用种子）"
    }
    if ($KeyedSalt)
    {
        $record.uiLogFull = $record.uiLogFull + "`r`n20:00:02  内核说明：该游戏的 FileNameHasherSalt 非空（带密钥哈希），当前版本的推理补充还原不支持。"
    }
    if ($CloseStuck)
    {
        # 场景：关窗后卡住 —— 还原窗口 12 秒才消失、线程没回收、最后被强杀
        $record.closeDuringMining = [ordered]@{ requested = $true; miningDetected = $true; detectedAtMs = 800; closedAtMs = 900; windowGoneMs = 12000; threadsBefore = 9; threadsAfter = 9; exited = $false; exitMs = 25000; killed = $true; graceSec = 25; confirmDialogSeen = $true; confirmClicks = 1 }
    }
    else
    {
        $record.closeDuringMining = [ordered]@{ requested = $true; miningDetected = $true; detectedAtMs = 800; closedAtMs = 900; windowGoneMs = 250; threadsBefore = 9; threadsAfter = 4; exited = $true; exitMs = 360; killed = $false; graceSec = 25; confirmDialogSeen = $true; confirmClicks = 1 }
    }
    return $record
}

# ---------- 场景 ----------

$scenarios = @(
    @{ Name = 'correct'; ExpectFail = @() }
    @{ Name = 'no-suffix'; ExpectFail = @('S-1'); Params = @{ NoSuffix = $true } }
    @{ Name = 'passthrough-dropped'; ExpectFail = @('S-3'); Params = @{ ForceMinedOutputs = $true } }
    @{ Name = 'not-mined'; ExpectFail = @('S-2', 'S-2b', 'S-5', 'X-1'); Params = @{ SkipMinedOutput = $true; NoList = $true } }
    @{ Name = 'fabricated-negative'; ExpectFail = @('S-4', 'S-5'); Params = @{ FabricateNegative = $true } }
    @{ Name = 'fabricated-stranger'; ExpectFail = @('S-4'); Params = @{ FabricateStranger = $true } }
    @{ Name = 'report-lies'; ExpectFail = @('S-4', 'S-5'); Params = @{ ReportHitOverride = 9 } }
    @{ Name = 'no-rate-lines'; ExpectFail = @('R-1', 'R-2'); Params = @{ NoRateLines = $true } }
    @{ Name = 'bad-percent'; ExpectFail = @('R-1'); Params = @{ BadPercent = $true } }
    @{ Name = 'old-wording'; ExpectFail = @('N-1', 'R-1', 'R-2'); Params = @{ OldWording = $true } }
    @{ Name = 'ui-frozen'; ExpectFail = @('T-1'); Params = @{ FrozenUi = $true } }
    @{ Name = 'ui-slow'; ExpectFail = @('T-1'); Params = @{ SlowUi = $true } }
    @{ Name = 'close-stuck'; ExpectFail = @('T-2'); Params = @{ CloseStuck = $true } }
    @{ Name = 'no-second-pass'; ExpectFail = @('X-1'); Params = @{ NoSecondPass = $true } }
    @{ Name = 'mining-when-unchecked'; ExpectFail = @('X-2'); Params = @{ MiningWhenUnchecked = $true } }
    @{ Name = 'dup-bloat'; ExpectFail = @('X-3', 'S-2b'); Params = @{ DupBloat = $true } }
    @{ Name = 'source-touched'; ExpectFail = @('X-4'); Params = @{ TouchSource = $true } }
)

$selftestResults = New-Object System.Collections.ArrayList

foreach ($scenario in $scenarios)
{
    $dir = Join-Path $workRoot $scenario.Name
    if (Test-Path -LiteralPath $dir) { [void](Remove-TreeUnder -Path $dir -MustBeUnder $runRoot) }
    New-Item -ItemType Directory -Path $dir -Force | Out-Null

    $params = if ($scenario.ContainsKey('Params')) { $scenario.Params } else { @{} }

    # 每条用例的"故意做错"参数：先造正常的，再按场景改写
    $keepParams = @{ Label = 'keep'; Keep = $true }
    $mineParams = @{ Label = 'mine'; Keep = $true; Mining = $true }
    $defaultParams = @{ Label = 'default-mine'; Mining = $true }
    $saltlessParams = @{ Label = 'saltless'; Mining = $true; SaltRemoved = $true; NoMinedOutput = $true }
    $keyedParams = @{ Label = 'keyed'; Mining = $true; KeyedSalt = $true; NoMinedOutput = $true }
    $closeParams = @{ Label = 'close'; Mining = $true }

    foreach ($key in $params.Keys)
    {
        switch ($key)
        {
            'NoSuffix' { $keepParams['NoSuffix'] = $true }
            'ForceMinedOutputs' { $keepParams['ForceMinedOutputs'] = $true }
            'MinedAsReal' {
                # 对照组故意做错：不开补充还原却把真名写出来了
                $keepParams['Mining'] = $true
            }
            'SkipMinedOutput' {
                $mineParams['Mining'] = $false
                $defaultParams['Mining'] = $false
            }
            'NoList' { }
            'FabricateNegative' { $mineParams['FabricateNegative'] = $true }
            'FabricateStranger' { $mineParams['FabricateStranger'] = $true }
            'ReportHitOverride' { $mineParams['ReportHitOverride'] = $params['ReportHitOverride'] }
            'NoRateLines' { $defaultParams['NoRateLines'] = $true }
            'BadPercent' { $defaultParams['BadPercent'] = $true }
            'OldWording' {
                $mineParams['OldWording'] = $true
                $defaultParams['OldWording'] = $true
            }
            'FrozenUi' { $defaultParams['FrozenUi'] = $true }
            'SlowUi' { $defaultParams['SlowUi'] = $true }
            'CloseStuck' { $closeParams['CloseStuck'] = $true }
            'NoSecondPass' { $defaultParams['NoSecondPass'] = $true }
            'MiningWhenUnchecked' { $keepParams['ForceMiningLog'] = $true; $keepParams['ForceMiningActivity'] = $true }
            'DupBloat' { $defaultParams['DupBloat'] = $true }
            'TouchSource' { $defaultParams['TouchSource'] = $true }
        }
    }

    # 名字表：默认是"正常"的；report-lies 场景保持正常（错的只是报告数字）
    $listPath = New-MinedList
    if ($params.ContainsKey('FabricateNegative')) { [void](New-MinedList -WithNegative) }
    if ($params.ContainsKey('NoList'))
    {
        # 场景：根本没挖到东西 → 不该有名字表
        Remove-Item -LiteralPath $listPath -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath (Join-Path (Join-Path $runRoot 'App') 'MinedNames.lst') -Force -ErrorAction SilentlyContinue
    }

    $paths = [ordered]@{
        keep = Join-Path $dir 'keep.json'
        mine = Join-Path $dir 'mine.json'
        defaultMine = Join-Path $dir 'default-mine.json'
        saltless = Join-Path $dir 'saltless.json'
        keyed = Join-Path $dir 'keyed.json'
        close = Join-Path $dir 'close.json'
    }
    Write-JsonFile -Path $paths.keep -Object (New-RunRecord @keepParams)
    Write-JsonFile -Path $paths.mine -Object (New-RunRecord @mineParams)
    Write-JsonFile -Path $paths.defaultMine -Object (New-RunRecord @defaultParams)
    Write-JsonFile -Path $paths.saltless -Object (New-RunRecord @saltlessParams)
    Write-JsonFile -Path $paths.keyed -Object (New-RunRecord @keyedParams)
    Write-JsonFile -Path $paths.close -Object (New-RunRecord @closeParams)

    $checksFile = Join-Path $dir 'checks.json'
    & pwsh -NoProfile -File (Join-Path $root 'assert-supplement.ps1') `
        -ManifestPath $ManifestPath `
        -KeepRunJson $paths.keep -MineRunJson $paths.mine -DefaultMineRunJson $paths.defaultMine `
        -SaltlessRunJson $paths.saltless -KeyedSaltRunJson $paths.keyed -CloseRunJson $paths.close `
        -ChecksJson $checksFile | Out-Null

    $result = Read-JsonFile -Path $checksFile
    $actualFail = @()
    if ($result)
    {
        foreach ($check in $result.checks) { if (-not $check.Ok -and $check.Severity -ne 'info') { $actualFail += $check.Id } }
    }
    $expected = @($scenario.ExpectFail)

    $missing = @($expected | Where-Object { $_ -notin $actualFail })
    $unexpected = @($actualFail | Where-Object { $_ -notin $expected })
    $ok = ($missing.Count -eq 0 -and $unexpected.Count -eq 0)

    $details = @()
    if ($result) { $details += ("{0} 项断言；失败 {1} 项：{2}" -f $result.total, $actualFail.Count, $(if ($actualFail.Count -gt 0) { $actualFail -join ', ' } else { '（无）' })) }
    else { $details += '断言脚本没有写出 checks json' }
    if ($missing.Count -gt 0) { $details += ('本该失败却没失败：' + ($missing -join ', ')) }
    if ($unexpected.Count -gt 0) { $details += ('不该失败却失败了：' + ($unexpected -join ', ')) }

    Add-Check -Id ('SELFTEST-S/' + $scenario.Name) -Title '断言响应符合预期' -Ok $ok -Details $details
}

$selftestFail = @($script:harnessChecks | Where-Object { -not $_.Ok -and $_.Severity -eq 'fail' }).Count
$selftestWarn = @($script:harnessChecks | Where-Object { -not $_.Ok -and $_.Severity -ne 'fail' }).Count
Write-Host ''
Write-Host ("结论：{0}（{1} 项；另有 {2} 条提示）" -f `
    $(if ($selftestFail -eq 0) { '全部通过' } else { '不通过 —— ' + $selftestFail + ' 项失败' }),
    $script:harnessChecks.Count,
    $selftestWarn)

# 字段要和 Show-HarnessChecks 写出来的一致（run-all 的汇总会读 warned）
if ($ChecksJson) { Write-JsonFile -Path $ChecksJson -Object ([ordered]@{ title = '补充还原断言自检（鉴别力）'; failed = $selftestFail; warned = $selftestWarn; total = $script:harnessChecks.Count; checks = $script:harnessChecks }) }
exit $(if ($selftestFail -eq 0) { 0 } else { 1 })
